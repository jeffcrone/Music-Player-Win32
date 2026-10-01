/*
 * test_stretch.c - the pitch-preserving time stretcher.
 *
 * Fed synthetic signals from memory and measured: the output's length must
 * scale with 1 / speed, a tone must keep its pitch and its loudness, and
 * the plumbing (resets, chunk sizes, short and empty input, full-scale
 * input, a channel of silence) must behave exactly.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mptest.h"

#include "stretch.h"

#define RATE 44100
#define PI 3.14159265358979323846

/* A source reading from an array, at most `max_read` frames per call (to
 * check the stretcher copes with sources that return less than asked). */
typedef struct {
	const int16_t *data;
	size_t frames, pos, max_read;
	uint32_t ch;
	int calls_after_end;
} Src;

static uint64_t src_read(void *ctx, int16_t *buf, uint64_t frames)
{
	Src *s = (Src *)ctx;
	size_t n = s->frames - s->pos;
	if (n == 0) {
		s->calls_after_end++;
		return 0;
	}
	if (n > frames)
		n = (size_t)frames;
	if (s->max_read && n > s->max_read)
		n = s->max_read;
	memcpy(buf, s->data + s->pos * s->ch, n * s->ch * sizeof(int16_t));
	s->pos += n;
	return n;
}

static void src_init(Src *s, const int16_t *data, size_t frames, uint32_t ch)
{
	memset(s, 0, sizeof(*s));
	s->data = data;
	s->frames = frames;
	s->ch = ch;
}

/* A sine of `hz` at `amp` in channel 0 of a `ch`-channel buffer; the other
 * channels silent. */
static int16_t *tone(size_t frames, uint32_t ch, double hz, double amp)
{
	int16_t *p = (int16_t *)calloc(frames * ch, sizeof(int16_t));
	size_t i;
	for (i = 0; i < frames; i++)
		p[i * ch] = (int16_t)(amp * sin(2 * PI * hz * (double)i / RATE));
	return p;
}

/* Runs a whole input through, `chunk` output frames per call. Returns the
 * output (malloc'd) and its length in *out_frames. */
static int16_t *run(MpStretch *st, Src *src, size_t chunk, size_t *out_frames)
{
	size_t cap = 1 << 16, len = 0;
	int16_t *out = (int16_t *)malloc(cap * src->ch * sizeof(int16_t));
	for (;;) {
		size_t got;
		if (len + chunk > cap) {
			cap *= 2;
			out = (int16_t *)realloc(out, cap * src->ch * sizeof(int16_t));
		}
		got = mp_stretch_process(st, src_read, src, out + len * src->ch, chunk);
		len += got;
		if (got < chunk)
			break;
	}
	*out_frames = len;
	return out;
}

/* The frequency of channel 0 over frames [from, to), from its rising zero
 * crossings, interpolated between samples. */
static double frequency(const int16_t *p, uint32_t ch, size_t from, size_t to)
{
	double first = -1, last = -1;
	int crossings = 0;
	size_t i;
	for (i = from + 1; i < to; i++) {
		int a = p[(i - 1) * ch], b = p[i * ch];
		if (a < 0 && b >= 0) {
			double t = (double)(i - 1) + (double)-a / (double)(b - a);
			if (first < 0)
				first = t;
			last = t;
			crossings++;
		}
	}
	if (crossings < 2)
		return 0;
	return (double)(crossings - 1) * RATE / (last - first);
}

static double rms(const int16_t *p, uint32_t ch, size_t from, size_t to)
{
	double sum = 0;
	size_t i;
	for (i = from; i < to; i++)
		sum += (double)p[i * ch] * p[i * ch];
	return to > from ? sqrt(sum / (double)(to - from)) : 0;
}

static void create_and_bad_arguments(void)
{
	MpStretch *st;
	int16_t out[16];
	CHECK(mp_stretch_create(RATE, 0) == NULL);
	CHECK(mp_stretch_create(RATE, 9) == NULL);
	CHECK(mp_stretch_create(0, 2) == NULL);
	CHECK(mp_stretch_create(999, 2) == NULL);
	/* The rates that occur: telephone quality up to studio. */
	st = mp_stretch_create(8000, 1);
	CHECK(st != NULL);
	mp_stretch_destroy(st);
	st = mp_stretch_create(192000, 8);
	CHECK(st != NULL);
	mp_stretch_destroy(st);
	mp_stretch_destroy(NULL);

	st = mp_stretch_create(RATE, 2);
	CHECK_INT(mp_stretch_process(st, NULL, NULL, out, 8), 0);
	CHECK_INT(mp_stretch_process(NULL, src_read, NULL, out, 8), 0);
	mp_stretch_destroy(st);
}

/* The speeds the player offers, and the limits. */
static const int speeds[] = { 25, 50, 75, 100, 125, 150, 175, 200, 300, 400 };
#define NSPEEDS ((int)(sizeof(speeds) / sizeof(speeds[0])))

static void length_scales(void)
{
	size_t in_frames = RATE * 2, out_frames;
	int16_t *in = tone(in_frames, 2, 440, 10000), *out;
	int i;
	for (i = 0; i < NSPEEDS; i++) {
		MpStretch *st = mp_stretch_create(RATE, 2);
		Src src;
		double want = (double)in_frames * 100.0 / speeds[i];
		src_init(&src, in, in_frames, 2);
		mp_stretch_reset(st, speeds[i]);
		out = run(st, &src, 4096, &out_frames);
		/* Within one frame length (30 ms) plus the closing fade. */
		CHECK(fabs((double)out_frames - want) < RATE * 0.06);
		/* All the input was read, and the end was only asked about once
		 * more than needed: it does not keep calling a finished source. */
		CHECK_INT(src.pos, in_frames);
		CHECK(src.calls_after_end <= 1);
		/* Once finished it stays finished. */
		CHECK_INT(mp_stretch_process(st, src_read, &src, out, 100), 0);
		free(out);
		mp_stretch_destroy(st);
	}
	free(in);
}

static void pitch_and_level_kept(void)
{
	/* A low, a middle and a high note: the low one is the hard case (a
	 * 110 Hz period is 9 ms, close to the 10 ms search). */
	static const double notes[] = { 110, 440, 2000 };
	size_t in_frames = RATE * 2, out_frames;
	int n, i;
	for (n = 0; n < 3; n++) {
		int16_t *in = tone(in_frames, 1, notes[n], 12000);
		for (i = 0; i < NSPEEDS; i++) {
			MpStretch *st = mp_stretch_create(RATE, 1);
			Src src;
			int16_t *out;
			double f, level;
			size_t a, b;
			src_init(&src, in, in_frames, 1);
			mp_stretch_reset(st, speeds[i]);
			out = run(st, &src, 4096, &out_frames);
			/* The middle half, away from the fade-in and fade-out. */
			a = out_frames / 4;
			b = out_frames * 3 / 4;
			f = frequency(out, 1, a, b);
			level = rms(out, 1, a, b);
			/* Same pitch within 1%. (Resampling would have moved it by
			 * the speed factor: 2x would read 880.) */
			CHECK(fabs(f - notes[n]) < notes[n] * 0.01);
			/* Same loudness within 10%: the frames were put together in
			 * phase, so they did not cancel. A sine at 12000 has an RMS
			 * of 12000 / sqrt(2). */
			CHECK(fabs(level - 12000 / sqrt(2.0)) < 12000 / sqrt(2.0) * 0.10);
			free(out);
			mp_stretch_destroy(st);
		}
		free(in);
	}
}

static void silence_and_channels(void)
{
	size_t in_frames = RATE, out_frames, i;
	int16_t *in = tone(in_frames, 2, 440, 10000), *out;
	int bad = 0, loud = 0;
	MpStretch *st = mp_stretch_create(RATE, 2);
	Src src;

	/* The right channel is silent going in, so it is exactly silent coming
	 * out: channels are never mixed together, only compared. */
	src_init(&src, in, in_frames, 2);
	mp_stretch_reset(st, 150);
	out = run(st, &src, 1000, &out_frames);
	for (i = 0; i < out_frames; i++) {
		if (out[i * 2 + 1] != 0)
			bad++;
		if (out[i * 2] > 5000)
			loud++;
	}
	CHECK_INT(bad, 0);
	CHECK(loud > 0);
	free(out);

	/* Silence in, silence out. */
	memset(in, 0, in_frames * 2 * sizeof(int16_t));
	src_init(&src, in, in_frames, 2);
	mp_stretch_reset(st, 50);
	out = run(st, &src, 1000, &out_frames);
	bad = 0;
	for (i = 0; i < out_frames * 2; i++)
		bad += out[i] != 0;
	CHECK_INT(bad, 0);
	CHECK(out_frames > in_frames);
	free(out);
	free(in);
	mp_stretch_destroy(st);
}

static void full_scale_does_not_wrap(void)
{
	/*
	 * Overlapping frames are blended with weights that add up to 1, so in
	 * exact arithmetic nothing can exceed full scale; but in floats a
	 * blend of 32767s can come out a hair above 32767, and a careless
	 * conversion would wrap that to -32768. So: constant full-scale input,
	 * both polarities. After the fade-in, the output must sit exactly at
	 * full scale, and nowhere may it have the opposite sign.
	 */
	static const int16_t levels[] = { 32767, -32768 };
	size_t in_frames = RATE, out_frames, i;
	int16_t *in = (int16_t *)malloc(in_frames * sizeof(int16_t)), *out;
	int l, s;
	MpStretch *st = mp_stretch_create(RATE, 1);
	Src src;
	for (l = 0; l < 2; l++) {
		for (s = 0; s < NSPEEDS; s++) {
			int wrong_sign = 0, not_full = 0;
			for (i = 0; i < in_frames; i++)
				in[i] = levels[l];
			src_init(&src, in, in_frames, 1);
			mp_stretch_reset(st, speeds[s]);
			out = run(st, &src, 4096, &out_frames);
			for (i = 0; i < out_frames; i++) {
				if ((levels[l] > 0 && out[i] < 0) || (levels[l] < 0 && out[i] > 0))
					wrong_sign++;
			}
			for (i = out_frames / 4; i < out_frames * 3 / 4; i++)
				not_full += out[i] != levels[l];
			CHECK_INT(wrong_sign, 0);
			CHECK_INT(not_full, 0);
			free(out);
		}
	}
	free(in);
	mp_stretch_destroy(st);
}

static void chunking_does_not_matter(void)
{
	/* The same input read in different chunk sizes, and from a source that
	 * trickles 7 frames at a time, gives exactly the same output. */
	size_t in_frames = RATE / 2, len1, len2, len3;
	int16_t *in = tone(in_frames, 2, 330, 9000), *o1, *o2, *o3;
	MpStretch *st = mp_stretch_create(RATE, 2);
	Src src;
	src_init(&src, in, in_frames, 2);
	mp_stretch_reset(st, 125);
	o1 = run(st, &src, 4096, &len1);
	src_init(&src, in, in_frames, 2);
	mp_stretch_reset(st, 125);
	o2 = run(st, &src, 1, &len2);
	src_init(&src, in, in_frames, 2);
	src.max_read = 7;
	mp_stretch_reset(st, 125);
	o3 = run(st, &src, 333, &len3);
	CHECK_INT(len1, len2);
	CHECK_INT(len1, len3);
	CHECK(len1 == len2 && memcmp(o1, o2, len1 * 2 * sizeof(int16_t)) == 0);
	CHECK(len1 == len3 && memcmp(o1, o3, len1 * 2 * sizeof(int16_t)) == 0);
	free(o1);
	free(o2);
	free(o3);
	free(in);
	mp_stretch_destroy(st);
}

static void reset_starts_over(void)
{
	size_t in_frames = RATE / 2, len1, len2;
	int16_t *in = tone(in_frames, 1, 500, 8000), *o1, *o2, part[1000];
	MpStretch *st = mp_stretch_create(RATE, 1);
	Src src;
	/* Stop half way through one input, reset, and run a fresh one: the
	 * result is as if the first had never happened. */
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, 200);
	o1 = run(st, &src, 4096, &len1);
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, 50);
	CHECK_INT(mp_stretch_process(st, src_read, &src, part, 1000), 1000);
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, 200);
	o2 = run(st, &src, 4096, &len2);
	CHECK(len1 == len2 && memcmp(o1, o2, len1 * sizeof(int16_t)) == 0);
	free(o1);
	free(o2);
	free(in);
	mp_stretch_destroy(st);
}

static void short_and_empty_input(void)
{
	int16_t in[100], out[4096];
	MpStretch *st = mp_stretch_create(RATE, 1);
	Src src;
	size_t got;
	int i;
	for (i = 0; i < 100; i++)
		in[i] = (int16_t)(i * 100);

	/* No input at all: no output, and no endless calls. */
	src_init(&src, in, 0, 1);
	mp_stretch_reset(st, 150);
	CHECK_INT(mp_stretch_process(st, src_read, &src, out, 4096), 0);
	CHECK_INT(mp_stretch_process(st, src_read, &src, out, 4096), 0);
	CHECK(src.calls_after_end <= 1);

	/* Less than one frame (100 frames, about 2 ms): some output, then the
	 * end, at every speed. */
	for (i = 0; i < NSPEEDS; i++) {
		src_init(&src, in, 100, 1);
		mp_stretch_reset(st, speeds[i]);
		got = mp_stretch_process(st, src_read, &src, out, 4096);
		CHECK(got > 0 && got < 4096);
		CHECK_INT(mp_stretch_process(st, src_read, &src, out, 4096), 0);
	}
	mp_stretch_destroy(st);
}

static void speed_is_clamped(void)
{
	size_t in_frames = RATE, a, b;
	int16_t *in = tone(in_frames, 1, 440, 8000), *o1, *o2;
	MpStretch *st = mp_stretch_create(RATE, 1);
	Src src;
	/* Below the minimum behaves as the minimum, above the maximum as the
	 * maximum. */
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, 1);
	o1 = run(st, &src, 4096, &a);
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, MP_STRETCH_MIN_SPEED);
	o2 = run(st, &src, 4096, &b);
	CHECK(a == b && memcmp(o1, o2, a * sizeof(int16_t)) == 0);
	free(o1);
	free(o2);
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, 100000);
	o1 = run(st, &src, 4096, &a);
	src_init(&src, in, in_frames, 1);
	mp_stretch_reset(st, MP_STRETCH_MAX_SPEED);
	o2 = run(st, &src, 4096, &b);
	CHECK(a == b && memcmp(o1, o2, a * sizeof(int16_t)) == 0);
	free(o1);
	free(o2);
	free(in);
	mp_stretch_destroy(st);
}

int suite_stretch(void)
{
	create_and_bad_arguments();
	length_scales();
	pitch_and_level_kept();
	silence_and_channels();
	full_scale_does_not_wrap();
	chunking_does_not_matter();
	reset_starts_over();
	short_and_empty_input();
	speed_is_clamped();
	return 0;
}
