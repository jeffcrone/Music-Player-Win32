/*
 * decoder.c - MP3/FLAC/WAV decoding behind one interface. See decoder.h.
 */
#include "decoder.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "dr_libs_config.h"
#include "text.h"
#include "track.h"

/* Frames decoded per step when downmixing surround files. */
#define DOWNMIX_CHUNK 1024

/* Seek table entries for MP3. MP3 has no index, so without a table every
 * seek means decoding from the start; 1024 points makes seeking in even a
 * long file near-instant. */
#define MP3_SEEK_POINTS 1024

struct MpDecoder {
	MpFormat format;
	MpStream *stream;
	uint32_t sample_rate;
	uint32_t src_channels;
	uint32_t out_channels;
	uint64_t total_frames;
	int16_t *mix_buf; /* src_channels * DOWNMIX_CHUNK samples, or NULL */
	/* Where the audio format's own data starts in the stream: after any
	 * ID3v2 tags for FLAC, 0 otherwise. See flac_on_seek for why. */
	int64_t base;

	drmp3 *mp3;
	drmp3_seek_point *mp3_seek_points;
	drflac *flac;
	drwav *wav;
};

/* ---- Stream callbacks for dr_libs --------------------------------------- */

/* All three libraries use SET/CUR/END in that order, the same as ours, but
 * map explicitly so an upstream change cannot silently break seeking. */
static int map_origin(int set, int cur, int origin)
{
	if (origin == set)
		return MP_SEEK_SET;
	if (origin == cur)
		return MP_SEEK_CUR;
	return MP_SEEK_END;
}

static size_t on_read(void *user, void *buf, size_t n)
{
	return mp_stream_read((MpStream *)user, buf, n);
}

static drmp3_bool32 mp3_on_seek(void *user, int offset, drmp3_seek_origin origin)
{
	return (drmp3_bool32)mp_stream_seek((MpStream *)user, offset,
		map_origin(DRMP3_SEEK_SET, DRMP3_SEEK_CUR, origin));
}

static drmp3_bool32 mp3_on_tell(void *user, drmp3_int64 *cursor)
{
	*cursor = mp_stream_tell((MpStream *)user);
	return DRMP3_TRUE;
}

/*
 * dr_flac gets a view of the stream that starts at the "fLaC" marker, with
 * any ID3v2 tag in front hidden. dr_flac can skip ID3 tags itself, but its
 * metadata reader then does an absolute seek to byte 42 (where the first
 * block header would be in a tag-less file), which lands inside the tag
 * whenever the file has more than one metadata block - i.e. nearly every
 * real FLAC file. Offsetting absolute seeks and tells by the tag size makes
 * the file look exactly like a tag-less one. (Tested in test_decoder.c.)
 */
static drflac_bool32 flac_on_seek(void *user, int offset, drflac_seek_origin origin)
{
	MpDecoder *d = (MpDecoder *)user;
	int o = map_origin(DRFLAC_SEEK_SET, DRFLAC_SEEK_CUR, origin);
	return (drflac_bool32)mp_stream_seek(d->stream, o == MP_SEEK_SET ? d->base + offset : offset, o);
}

static drflac_bool32 flac_on_tell(void *user, drflac_int64 *cursor)
{
	MpDecoder *d = (MpDecoder *)user;
	*cursor = mp_stream_tell(d->stream) - d->base;
	return DRFLAC_TRUE;
}

static size_t flac_on_read(void *user, void *buf, size_t n)
{
	return mp_stream_read(((MpDecoder *)user)->stream, buf, n);
}

static drwav_bool32 wav_on_seek(void *user, int offset, drwav_seek_origin origin)
{
	return (drwav_bool32)mp_stream_seek((MpStream *)user, offset,
		map_origin(DRWAV_SEEK_SET, DRWAV_SEEK_CUR, origin));
}

static drwav_bool32 wav_on_tell(void *user, drwav_int64 *cursor)
{
	*cursor = mp_stream_tell((MpStream *)user);
	return DRWAV_TRUE;
}

/* ---- Format detection --------------------------------------------------- */

static int is_mpeg_audio_header(const uint8_t *h)
{
	/* 11 sync bits, then reject the reserved values of version, layer,
	 * bitrate and sample rate, which is what makes random data unlikely to
	 * pass. */
	return h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 &&
		((h[1] >> 3) & 3) != 1 && ((h[1] >> 1) & 3) != 0 &&
		(h[2] >> 4) != 0xF && ((h[2] >> 2) & 3) != 3;
}

/* Sony Wave64 files start with this GUID ("riff" + a fixed suffix). */
static const uint8_t w64_riff_guid[16] = {
	0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
	0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00
};

static MpFormat format_from_extension(const wchar_t *name)
{
	const wchar_t *base, *dot = NULL, *p;
	if (name == NULL)
		return MP_FORMAT_UNKNOWN;
	base = mp_path_basename(name);
	for (p = base; *p != 0; p++) {
		if (*p == L'.')
			dot = p;
	}
	if (dot == NULL)
		return MP_FORMAT_UNKNOWN;
	if (mp_wascii_ieq(dot, L".mp3") || mp_wascii_ieq(dot, L".mp2") ||
		mp_wascii_ieq(dot, L".mp1") || mp_wascii_ieq(dot, L".mpga"))
		return MP_FORMAT_MP3;
	if (mp_wascii_ieq(dot, L".flac") || mp_wascii_ieq(dot, L".fla"))
		return MP_FORMAT_FLAC;
	if (mp_wascii_ieq(dot, L".wav") || mp_wascii_ieq(dot, L".wave"))
		return MP_FORMAT_WAV;
	return MP_FORMAT_UNKNOWN;
}

/* Offset just past any ID3v2 tags at the start of the stream. */
static int64_t skip_id3v2(MpStream *s)
{
	uint8_t h[10];
	int64_t start = 0;
	int i;
	/* Some tools stack several tags; four is more than anyone does. */
	for (i = 0; i < 4; i++) {
		if (!mp_stream_read_at(s, start, h, 10) || memcmp(h, "ID3", 3) != 0 ||
			((h[6] | h[7] | h[8] | h[9]) & 0x80))
			break;
		start += 10 + ((int64_t)h[6] << 21 | (int64_t)h[7] << 14 | (int64_t)h[8] << 7 | h[9]) +
			((h[3] == 4 && (h[5] & 0x10)) ? 10 : 0);
	}
	return start;
}

MpFormat mp_detect_format(MpStream *s, const wchar_t *name_hint)
{
	uint8_t h[16];
	/* FLAC files sometimes carry an ID3v2 tag too. */
	int64_t start = skip_id3v2(s);


	if (mp_stream_read_at(s, start, h, 4) && memcmp(h, "fLaC", 4) == 0)
		return MP_FORMAT_FLAC;
	if (mp_stream_read_at(s, 0, h, 12) && memcmp(h + 8, "WAVE", 4) == 0 &&
		(memcmp(h, "RIFF", 4) == 0 || memcmp(h, "RF64", 4) == 0))
		return MP_FORMAT_WAV;
	if (mp_stream_read_at(s, 0, h, 16) && memcmp(h, w64_riff_guid, 16) == 0)
		return MP_FORMAT_WAV;
	if (mp_stream_read_at(s, start, h, 4) && is_mpeg_audio_header(h))
		return MP_FORMAT_MP3;
	return format_from_extension(name_hint);
}

/* ---- Opening and closing ------------------------------------------------ */

static void set_err(wchar_t *err, size_t cap, const wchar_t *msg)
{
	if (err != NULL && cap > 0)
		mp_wcopy(err, cap, msg);
}

static void free_decoder(MpDecoder *d)
{
	if (d == NULL)
		return;
	if (d->mp3 != NULL) {
		drmp3_uninit(d->mp3);
		free(d->mp3);
	}
	free(d->mp3_seek_points);
	if (d->flac != NULL)
		drflac_close(d->flac);
	if (d->wav != NULL) {
		drwav_uninit(d->wav);
		free(d->wav);
	}
	free(d->mix_buf);
	free(d);
}

MpDecoder *mp_decoder_open_stream(MpStream *s, const wchar_t *name_hint, wchar_t *err, size_t err_cap)
{
	MpDecoder *d;
	MpFormat format;

	if (s == NULL) {
		set_err(err, err_cap, L"The file could not be opened.");
		return NULL;
	}
	format = mp_detect_format(s, name_hint);
	if (format == MP_FORMAT_UNKNOWN) {
		set_err(err, err_cap, L"This is not an MP3, FLAC or WAV file.");
		return NULL;
	}
	d = (MpDecoder *)calloc(1, sizeof(*d));
	if (d == NULL) {
		set_err(err, err_cap, L"Out of memory.");
		return NULL;
	}
	d->format = format;
	/* Set now (not at the end) because the FLAC callbacks go through d. On
	 * failure it is cleared again so free_decoder leaves the stream alone. */
	d->stream = s;
	d->base = format == MP_FORMAT_FLAC ? skip_id3v2(s) : 0;
	mp_stream_seek(s, d->base, MP_SEEK_SET);

	switch (format) {
	case MP_FORMAT_MP3:
		d->mp3 = (drmp3 *)calloc(1, sizeof(drmp3));
		if (d->mp3 == NULL || !drmp3_init(d->mp3, on_read, mp3_on_seek, mp3_on_tell, NULL, s, NULL)) {
			free(d->mp3);
			d->mp3 = NULL;
			set_err(err, err_cap, L"The MP3 data in this file could not be read.");
			goto fail;
		}
		d->sample_rate = d->mp3->sampleRate;
		d->src_channels = d->mp3->channels;
		d->mp3_seek_points = (drmp3_seek_point *)malloc(MP3_SEEK_POINTS * sizeof(drmp3_seek_point));
		if (d->mp3_seek_points != NULL) {
			drmp3_uint32 count = MP3_SEEK_POINTS;
			/* A failure here only makes seeking slower, so it is not fatal. */
			if (drmp3_calculate_seek_points(d->mp3, &count, d->mp3_seek_points) && count > 0)
				drmp3_bind_seek_table(d->mp3, count, d->mp3_seek_points);
		}
		/* MP3 has no length field (short of an optional Xing/LAME header),
		 * so this scans the frame headers of the whole file. */
		d->total_frames = drmp3_get_pcm_frame_count(d->mp3);
		drmp3_seek_to_pcm_frame(d->mp3, 0);
		break;
	case MP_FORMAT_FLAC:
		d->flac = drflac_open(flac_on_read, flac_on_seek, flac_on_tell, d, NULL);
		if (d->flac == NULL) {
			set_err(err, err_cap, L"The FLAC data in this file could not be read.");
			goto fail;
		}
		d->sample_rate = d->flac->sampleRate;
		d->src_channels = d->flac->channels;
		/* 0 when the encoder did not know the length (streamed encodes). */
		d->total_frames = d->flac->totalPCMFrameCount;
		break;
	case MP_FORMAT_WAV:
		d->wav = (drwav *)calloc(1, sizeof(drwav));
		if (d->wav == NULL || !drwav_init(d->wav, on_read, wav_on_seek, wav_on_tell, s, NULL)) {
			free(d->wav);
			d->wav = NULL;
			set_err(err, err_cap, L"The WAV data in this file could not be read, or uses an unsupported encoding.");
			goto fail;
		}
		d->sample_rate = d->wav->sampleRate;
		d->src_channels = d->wav->channels;
		d->total_frames = d->wav->totalPCMFrameCount;
		break;
	default:
		goto fail;
	}

	/* waveOut on XP will not open 0 Hz or absurd rates, and 0 channels
	 * would divide by zero later; treat them as corrupt files. */
	if (d->src_channels == 0 || d->src_channels > 32 || d->sample_rate < 1000 || d->sample_rate > 768000) {
		set_err(err, err_cap, L"This file reports an impossible sample rate or channel count.");
		goto fail;
	}
	d->out_channels = d->src_channels <= 2 ? d->src_channels : 2;
	if (d->src_channels > 2) {
		d->mix_buf = (int16_t *)malloc((size_t)d->src_channels * DOWNMIX_CHUNK * sizeof(int16_t));
		if (d->mix_buf == NULL) {
			set_err(err, err_cap, L"Out of memory.");
			goto fail;
		}
	}
	return d;

fail:
	d->stream = NULL; /* the caller keeps ownership on failure */
	free_decoder(d);
	return NULL;
}

MpDecoder *mp_decoder_open_file(const wchar_t *path, wchar_t *err, size_t err_cap)
{
	unsigned long code = 0;
	MpDecoder *d;
	MpStream *s = mp_stream_open_file(path, &code);
	if (s == NULL) {
		if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
			set_err(err, err_cap, L"The file could not be found. It may have been moved, renamed or deleted.");
		} else if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) {
			set_err(err, err_cap, L"The file could not be opened: access was denied or another program is using it.");
		} else if (err != NULL && err_cap > 0) {
			/* Let Windows describe anything unusual, in the user's language. */
			if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
				err, (DWORD)err_cap, NULL))
				set_err(err, err_cap, L"The file could not be opened.");
			mp_trim(err);
		}
		return NULL;
	}
	d = mp_decoder_open_stream(s, path, err, err_cap);
	if (d == NULL)
		mp_stream_close(s);
	return d;
}

void mp_decoder_close(MpDecoder *d)
{
	MpStream *s;
	if (d == NULL)
		return;
	s = d->stream;
	/* The libraries may touch the stream while uninitializing, so close
	 * them first. */
	free_decoder(d);
	mp_stream_close(s);
}

/* ---- Queries ------------------------------------------------------------ */

MpFormat mp_decoder_format(const MpDecoder *d) { return d->format; }
uint32_t mp_decoder_sample_rate(const MpDecoder *d) { return d->sample_rate; }
uint32_t mp_decoder_channels(const MpDecoder *d) { return d->out_channels; }
uint32_t mp_decoder_source_channels(const MpDecoder *d) { return d->src_channels; }
uint64_t mp_decoder_total_frames(const MpDecoder *d) { return d->total_frames; }

/* ---- Decoding ----------------------------------------------------------- */

static uint64_t read_native(MpDecoder *d, int16_t *out, uint64_t frames)
{
	switch (d->format) {
	case MP_FORMAT_MP3: return drmp3_read_pcm_frames_s16(d->mp3, frames, out);
	case MP_FORMAT_FLAC: return drflac_read_pcm_frames_s16(d->flac, frames, out);
	case MP_FORMAT_WAV: return drwav_read_pcm_frames_s16(d->wav, frames, out);
	default: return 0;
	}
}

uint64_t mp_decoder_read(MpDecoder *d, int16_t *out, uint64_t frames)
{
	uint64_t done = 0;
	if (d == NULL || out == NULL)
		return 0;
	if (d->mix_buf == NULL)
		return read_native(d, out, frames);
	while (done < frames) {
		uint64_t want = frames - done, got;
		if (want > DOWNMIX_CHUNK)
			want = DOWNMIX_CHUNK;
		got = read_native(d, d->mix_buf, want);
		if (got == 0)
			break;
		mp_downmix_s16(d->mix_buf, d->src_channels, out + done * 2, (size_t)got);
		done += got;
	}
	return done;
}

int mp_decoder_seek(MpDecoder *d, uint64_t frame)
{
	if (d == NULL)
		return 0;
	if (d->total_frames > 0 && frame > d->total_frames)
		frame = d->total_frames;
	switch (d->format) {
	case MP_FORMAT_MP3: return drmp3_seek_to_pcm_frame(d->mp3, frame) != 0;
	case MP_FORMAT_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame) != 0;
	case MP_FORMAT_WAV: return drwav_seek_to_pcm_frame(d->wav, frame) != 0;
	default: return 0;
	}
}

uint32_t mp_downmix_s16(const int16_t *in, uint32_t in_channels, int16_t *out, size_t frames)
{
	size_t f;
	uint32_t c;
	if (in_channels <= 2) {
		memmove(out, in, frames * in_channels * sizeof(int16_t));
		return in_channels;
	}
	for (f = 0; f < frames; f++) {
		const int16_t *src = in + f * in_channels;
		int32_t left = 0, right = 0;
		uint32_t nl = 0, nr = 0;
		for (c = 0; c < in_channels; c++) {
			if ((c & 1) == 0) {
				left += src[c];
				nl++;
			} else {
				right += src[c];
				nr++;
			}
		}
		/* Integer division truncates toward zero, so the average of values
		 * in [-32768, 32767] stays in range. */
		out[f * 2] = (int16_t)(left / (int32_t)nl);
		out[f * 2 + 1] = (int16_t)(right / (int32_t)nr);
	}
	return 2;
}
