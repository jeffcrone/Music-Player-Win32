/*
 * stretch.c - WSOLA time stretching. See stretch.h for the idea.
 *
 * Terms used below, all in frames (one sample per channel):
 *   N  frame length, 30 ms. Long enough to hold a couple of periods of a
 *      low note (so the pitch survives), short enough that the repeated or
 *      skipped slices are not heard as echoes or stutters.
 *   H  output hop, N / 2. With a periodic Hann window, frames overlapping
 *      by half add up to exactly 1, so a steady signal comes out at the
 *      same level it went in.
 *   Ha input hop, H * speed / 100.
 *   D  search tolerance, 10 ms: how far a frame's start may be moved to
 *      line it up with the previous one. About one period of an 100 Hz
 *      tone, which is enough to find a matching phase for anything above
 *      the lowest bass notes.
 *
 * Synthesis frame m is centered on input position m * Ha. Its first half is
 * added to the second half of frame m - 1 and those H frames are final,
 * so each frame yields exactly H output frames.
 */
#include "stretch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How many input frames to read from the source at most in one call. */
#define READ_CHUNK 4096

/* The similarity search looks at every 4th sample of the overlap. The
 * waveform is still well represented (at 44.1 kHz that is 11 kHz, above
 * where most of a song's energy is), and it makes the search 4 times
 * cheaper - it is by far the most expensive part. */
#define SEARCH_STEP 4

struct MpStretch {
	uint32_t ch;
	size_t n, h, d;       /* N, H, D above */
	float *win;           /* N window values */
	int speed;            /* percent */

	/* Input buffer: in[0] is input frame in_start (counted from the last
	 * reset). Frames before what the next synthesis frame can need are
	 * discarded as it goes. */
	int16_t *in;
	int32_t *mix;         /* the buffered input with its channels added */
	size_t cap, in_len;
	uint64_t in_start;
	uint64_t src_total;   /* frames read from the source since the reset */
	int eof;

	uint64_t m;           /* next synthesis frame */
	uint64_t prev;        /* input position of the last frame laid down */
	int have_prev;

	float *ola;           /* H frames: the last frame's windowed second half */
	int16_t *outq;        /* H frames of finished output */
	size_t outq_pos, outq_len;
	int finished;         /* the source ran out; the tail has been queued */
};

MpStretch *mp_stretch_create(uint32_t rate, uint32_t channels)
{
	MpStretch *s;
	size_t i;
	if (rate < 1000 || rate > 1000000 || channels < 1 || channels > 8)
		return NULL;
	s = (MpStretch *)calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->ch = channels;
	s->n = (size_t)rate * 30 / 1000;
	s->n &= ~(size_t)1; /* even, so the two halves are the same size */
	s->h = s->n / 2;
	s->d = (size_t)rate * 10 / 1000;
	/*
	 * The furthest back the next frame can need is the previous frame's
	 * position plus H, which is at least (target - Ha - D); the furthest
	 * ahead is target + D + N. At the top speed Ha is 4H = 2N, so the span
	 * is under 3N + 2D. Twice that leaves room for the read-ahead too.
	 */
	s->cap = 2 * (3 * s->n + 2 * s->d) + READ_CHUNK;
	s->win = (float *)malloc(s->n * sizeof(float));
	s->in = (int16_t *)malloc(s->cap * channels * sizeof(int16_t));
	s->mix = (int32_t *)malloc(s->cap * sizeof(int32_t));
	s->ola = (float *)malloc(s->h * channels * sizeof(float));
	s->outq = (int16_t *)malloc(s->h * channels * sizeof(int16_t));
	if (s->win == NULL || s->in == NULL || s->mix == NULL || s->ola == NULL || s->outq == NULL) {
		mp_stretch_destroy(s);
		return NULL;
	}
	/* Periodic Hann (N, not N - 1, in the denominator): win[i] +
	 * win[i + H] is exactly 1, which is what makes 50% overlap-add flat. */
	for (i = 0; i < s->n; i++)
		s->win[i] = (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * (double)i / (double)s->n));
	mp_stretch_reset(s, 100);
	return s;
}

void mp_stretch_destroy(MpStretch *s)
{
	if (s == NULL)
		return;
	free(s->win);
	free(s->in);
	free(s->mix);
	free(s->ola);
	free(s->outq);
	free(s);
}

void mp_stretch_reset(MpStretch *s, int speed_percent)
{
	if (speed_percent < MP_STRETCH_MIN_SPEED)
		speed_percent = MP_STRETCH_MIN_SPEED;
	if (speed_percent > MP_STRETCH_MAX_SPEED)
		speed_percent = MP_STRETCH_MAX_SPEED;
	s->speed = speed_percent;
	s->in_len = 0;
	s->in_start = 0;
	s->src_total = 0;
	s->eof = 0;
	s->m = 0;
	s->prev = 0;
	s->have_prev = 0;
	memset(s->ola, 0, s->h * s->ch * sizeof(float));
	s->outq_pos = 0;
	s->outq_len = 0;
	s->finished = 0;
}

/* Sample `c` of input frame `pos`. Past the end of the input is silence,
 * which is how the last frames are completed. */
static int16_t at(const MpStretch *s, uint64_t pos, uint32_t c)
{
	if (pos < s->in_start || pos - s->in_start >= s->in_len)
		return 0;
	return s->in[(size_t)(pos - s->in_start) * s->ch + c];
}

/*
 * Fills s->mix with every buffered frame's channels added together, and
 * zeros up to `end` (past the end of the input). The similarity search
 * matches the mix: that is enough to keep every channel in phase, since
 * they were recorded together. Doing the mix-down once per frame, rather
 * than inside the search, is what keeps the search affordable on the
 * single-core machines of the Windows XP era.
 */
static void make_mix(MpStretch *s, uint64_t end)
{
	size_t k, n = end - s->in_start > s->cap ? s->cap : (size_t)(end - s->in_start);
	uint32_t c;
	for (k = 0; k < n; k++) {
		int32_t v = 0;
		if (k < s->in_len) {
			for (c = 0; c < s->ch; c++)
				v += s->in[k * s->ch + c];
		}
		s->mix[k] = v;
	}
}

/* Drops input before `pos`; nothing will need it again. */
static void discard_before(MpStretch *s, uint64_t pos)
{
	size_t k;
	if (pos <= s->in_start)
		return;
	k = pos - s->in_start > s->in_len ? s->in_len : (size_t)(pos - s->in_start);
	memmove(s->in, s->in + k * s->ch, (s->in_len - k) * s->ch * sizeof(int16_t));
	s->in_start += k;
	s->in_len -= k;
}

/* Reads until input frame `pos` is buffered (exclusive), or the source
 * ends. */
static void fill_to(MpStretch *s, MpStretchSource src, void *ctx, uint64_t pos)
{
	while (!s->eof && s->in_start + s->in_len < pos && s->in_len < s->cap) {
		uint64_t want = pos - (s->in_start + s->in_len);
		uint64_t room = s->cap - s->in_len, got;
		if (want > room)
			want = room;
		if (want > READ_CHUNK)
			want = READ_CHUNK;
		got = src(ctx, s->in + s->in_len * s->ch, want);
		if (got == 0) {
			s->eof = 1;
			break;
		}
		if (got > want) /* a misbehaving source must not overrun us */
			got = want;
		s->in_len += (size_t)got;
		s->src_total += got;
	}
}

/*
 * How well the input from `cand` matches the input from `natural` over the
 * overlap: the cross-correlation divided by the candidate's energy. The
 * division stops the search from simply favoring louder candidates, which
 * plain correlation does.
 */
static double similarity(const MpStretch *s, uint64_t natural, uint64_t cand)
{
	int64_t xy = 0, yy = 0;
	size_t i;
	const int32_t *a = s->mix + (size_t)(natural - s->in_start);
	const int32_t *b = s->mix + (size_t)(cand - s->in_start);
	for (i = 0; i < s->h; i += SEARCH_STEP) {
		int64_t x = a[i], y = b[i];
		xy += x * y;
		yy += y * y;
	}
	return (double)xy / sqrt((double)yy + 1.0);
}

static int16_t clamp16(float v)
{
	/* Round to nearest; overlapping frames can add up past full scale. */
	if (v >= 32767.0f)
		return 32767;
	if (v <= -32768.0f)
		return -32768;
	return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* Lays down the next synthesis frame and queues its H finished output
 * frames. Returns 0, doing nothing, once the input is used up. */
static int next_frame(MpStretch *s, MpStretchSource src, void *ctx)
{
	/* Exact integer arithmetic for the nominal position. */
	uint64_t target = (s->m * s->h * (uint64_t)s->speed + 50) / 100;
	uint64_t lo = target > s->d ? target - s->d : 0, hi = target + s->d;
	uint64_t natural = s->prev + s->h, best = target, keep_from, c;
	size_t j;
	uint32_t ch;

	keep_from = lo;
	if (s->have_prev && natural < keep_from)
		keep_from = natural;
	discard_before(s, keep_from);
	fill_to(s, src, ctx, hi + s->n);
	if (s->eof && target >= s->src_total)
		return 0;

	if (s->have_prev) {
		/* The search reads up to H past the last candidate (hi) and past
		 * the natural continuation. At slow speeds the latter can be the
		 * further of the two: the input barely advances per frame, while
		 * the continuation is a whole H on. Both are below hi + N, which
		 * fill_to has buffered (or found to be past the end). */
		make_mix(s, (natural > hi ? natural : hi) + s->h);
		/* The candidate most like the natural continuation of the last
		 * frame. Ties go to the earliest, so the result is repeatable. */
		double best_score = -1e300;
		for (c = lo; c <= hi; c++) {
			double score = similarity(s, natural, c);
			if (score > best_score) {
				best_score = score;
				best = c;
			}
		}
	}

	for (j = 0; j < s->h; j++) {
		for (ch = 0; ch < s->ch; ch++) {
			size_t k = j * s->ch + ch;
			s->outq[k] = clamp16(s->ola[k] + (float)at(s, best + j, ch) * s->win[j]);
			s->ola[k] = (float)at(s, best + s->h + j, ch) * s->win[s->h + j];
		}
	}
	s->outq_pos = 0;
	s->outq_len = s->h;
	s->prev = best;
	s->have_prev = 1;
	s->m++;
	return 1;
}

size_t mp_stretch_process(MpStretch *s, MpStretchSource src, void *ctx, int16_t *out, size_t frames)
{
	size_t done = 0;
	if (s == NULL || src == NULL || out == NULL)
		return 0;
	while (done < frames) {
		size_t n;
		if (s->outq_pos == s->outq_len) {
			if (s->finished)
				break;
			if (!next_frame(s, src, ctx)) {
				/* The input is used up. The last frame's second half is
				 * still waiting in `ola`: play it out as a short fade
				 * (unless there never was a frame, i.e. no input). */
				s->finished = 1;
				if (!s->have_prev)
					break;
				for (n = 0; n < s->h * s->ch; n++)
					s->outq[n] = clamp16(s->ola[n]);
				s->outq_pos = 0;
				s->outq_len = s->h;
			}
		}
		n = s->outq_len - s->outq_pos;
		if (n > frames - done)
			n = frames - done;
		memcpy(out + done * s->ch, s->outq + s->outq_pos * s->ch, n * s->ch * sizeof(int16_t));
		s->outq_pos += n;
		done += n;
	}
	return done;
}
