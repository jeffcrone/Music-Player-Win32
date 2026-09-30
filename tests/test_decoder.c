/*
 * test_decoder.c - format detection and decoding of MP3, FLAC and WAV.
 *
 * WAV and FLAC are lossless, so decoded samples are compared exactly with
 * what went in. The MP3 test file is digital silence (see mp3_silence), the
 * one MP3 whose exact output is known without an encoder.
 */
#include <stdlib.h>
#include <windows.h>

#include "builders.h"
#include "decoder.h"
#include "mptest.h"

/* A deterministic, non-trivial test signal: a sawtooth per channel with
 * different slopes, covering negative and positive values. */
static int16_t test_sample(size_t frame, uint32_t channel)
{
	return (int16_t)((int32_t)((frame * (37 + channel * 11)) % 65536) - 32768);
}

static int16_t *make_signal(size_t frames, uint32_t channels)
{
	int16_t *s = (int16_t *)malloc(frames * channels * sizeof(int16_t));
	size_t f;
	uint32_t c;
	for (f = 0; f < frames; f++)
		for (c = 0; c < channels; c++)
			s[f * channels + c] = test_sample(f, c);
	return s;
}

static MpDecoder *open_buf(const Buf *b, const wchar_t *hint, MpStream **stream_out)
{
	wchar_t err[256];
	MpStream *s = mp_stream_open_memory(b->data, b->len);
	MpDecoder *d = mp_decoder_open_stream(s, hint, err, 256);
	if (d == NULL) {
		mp_stream_close(s);
		fprintf(stderr, "    (open failed: %ls)\n", err);
	}
	if (stream_out)
		*stream_out = s;
	return d;
}

static MpFormat detect(const Buf *b, const wchar_t *hint)
{
	MpStream *s = mp_stream_open_memory(b->data, b->len);
	MpFormat f = mp_detect_format(s, hint);
	mp_stream_close(s);
	return f;
}

/* ---- Detection ---------------------------------------------------------- */

static void detection(void)
{
	Buf b, tag, frames;
	static const int16_t pcm[4] = { 0 };

	buf_init(&b);
	wav_file(&b, 1, 2, 44100, 16, pcm, sizeof(pcm), NULL);
	CHECK_INT(detect(&b, NULL), MP_FORMAT_WAV);
	/* Content beats a misleading extension. */
	CHECK_INT(detect(&b, L"C:\\x.mp3"), MP_FORMAT_WAV);
	buf_free(&b);

	buf_init(&b);
	flac_file(&b, 44100, 1, pcm, 4, 4096, NULL);
	CHECK_INT(detect(&b, NULL), MP_FORMAT_FLAC);
	buf_free(&b);

	/* FLAC and MP3 behind an ID3v2 tag. */
	buf_init(&b);
	buf_init(&tag);
	buf_init(&frames);
	id3_frame(&frames, 3, "TIT2", 0, 0, "x", 1);
	id3_tag(&tag, 3, 0, &frames, 100);
	buf_bytes(&b, tag.data, tag.len);
	flac_file(&b, 44100, 1, pcm, 4, 4096, NULL);
	CHECK_INT(detect(&b, NULL), MP_FORMAT_FLAC);
	b.len = 0;
	buf_bytes(&b, tag.data, tag.len);
	mp3_silence(&b, 2);
	CHECK_INT(detect(&b, NULL), MP_FORMAT_MP3);
	buf_free(&b);
	buf_free(&tag);
	buf_free(&frames);

	buf_init(&b);
	mp3_silence(&b, 2);
	CHECK_INT(detect(&b, NULL), MP_FORMAT_MP3);
	buf_free(&b);

	/* RF64 and Sony Wave64 headers. */
	buf_init(&b);
	buf_str(&b, "RF64");
	buf_le32(&b, 0xFFFFFFFF);
	buf_str(&b, "WAVE");
	CHECK_INT(detect(&b, NULL), MP_FORMAT_WAV);
	buf_free(&b);
	{
		static const uint8_t w64[16] = { 0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
			0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 };
		buf_init(&b);
		buf_bytes(&b, w64, 16);
		buf_zeros(&b, 16);
		CHECK_INT(detect(&b, NULL), MP_FORMAT_WAV);
		buf_free(&b);
	}

	/* Unrecognizable content: the extension decides, case-insensitively. */
	buf_init(&b);
	buf_str(&b, "junk junk junk junk");
	CHECK_INT(detect(&b, NULL), MP_FORMAT_UNKNOWN);
	CHECK_INT(detect(&b, L"C:\\Music\\song.MP3"), MP_FORMAT_MP3);
	CHECK_INT(detect(&b, L"C:\\Music\\song.Flac"), MP_FORMAT_FLAC);
	CHECK_INT(detect(&b, L"C:\\Music\\song.wav"), MP_FORMAT_WAV);
	CHECK_INT(detect(&b, L"C:\\Music\\song.ogg"), MP_FORMAT_UNKNOWN);
	CHECK_INT(detect(&b, L"C:\\Music.mp3\\song"), MP_FORMAT_UNKNOWN);
	buf_free(&b);

	/* 0xFF 0xFF 0xFF: sync bits, but reserved field values - not MPEG. */
	buf_init(&b);
	buf_u8(&b, 0xFF);
	buf_u8(&b, 0xFF);
	buf_u8(&b, 0xFF);
	buf_u8(&b, 0xFF);
	CHECK_INT(detect(&b, NULL), MP_FORMAT_UNKNOWN);
	buf_free(&b);
}

/* ---- WAV ---------------------------------------------------------------- */

static void wav_16bit_stereo(void)
{
	const size_t frames = 5000;
	int16_t *pcm = make_signal(frames, 2);
	int16_t *out = (int16_t *)calloc(frames * 2, sizeof(int16_t));
	Buf b;
	MpDecoder *d;
	buf_init(&b);
	wav_file(&b, 1, 2, 48000, 16, pcm, frames * 4, NULL);
	d = open_buf(&b, NULL, NULL);
	CHECK(d != NULL);
	if (d != NULL) {
		CHECK_INT(mp_decoder_format(d), MP_FORMAT_WAV);
		CHECK_INT(mp_decoder_sample_rate(d), 48000);
		CHECK_INT(mp_decoder_channels(d), 2);
		CHECK_INT(mp_decoder_source_channels(d), 2);
		CHECK_INT(mp_decoder_total_frames(d), frames);
		/* Read in uneven pieces to cross internal buffer boundaries. */
		CHECK_INT(mp_decoder_read(d, out, 1234), 1234);
		CHECK_INT(mp_decoder_read(d, out + 1234 * 2, frames), frames - 1234);
		CHECK(memcmp(out, pcm, frames * 4) == 0);
		/* At the end: nothing more. */
		CHECK_INT(mp_decoder_read(d, out, 10), 0);
		/* Seek back and read again. */
		CHECK(mp_decoder_seek(d, 4000));
		CHECK_INT(mp_decoder_read(d, out, 10), 10);
		CHECK(memcmp(out, pcm + 4000 * 2, 10 * 4) == 0);
		/* Seeking past the end clamps to the end. */
		CHECK(mp_decoder_seek(d, 999999));
		CHECK_INT(mp_decoder_read(d, out, 10), 0);
		mp_decoder_close(d);
	}
	buf_free(&b);
	free(pcm);
	free(out);
}

static void wav_other_sample_formats(void)
{
	Buf b;
	MpDecoder *d;
	int16_t out[16];

	/* 8-bit is unsigned with 128 as silence; it becomes (x - 128) << 8. */
	{
		static const uint8_t u8[4] = { 0x80, 0x00, 0xFF, 0x81 };
		buf_init(&b);
		wav_file(&b, 1, 1, 8000, 8, u8, 4, NULL);
		d = open_buf(&b, NULL, NULL);
		CHECK(d != NULL);
		if (d != NULL) {
			CHECK_INT(mp_decoder_channels(d), 1);
			CHECK_INT(mp_decoder_read(d, out, 4), 4);
			CHECK_INT(out[0], 0);
			CHECK_INT(out[1], -32768);
			CHECK_INT(out[2], 32512);
			CHECK_INT(out[3], 256);
			mp_decoder_close(d);
		}
		buf_free(&b);
	}
	/* 24-bit keeps the top 16 bits. Little-endian 3-byte samples. */
	{
		static const uint8_t s24[9] = { 0xCD, 0x34, 0x12, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F };
		buf_init(&b);
		wav_file(&b, 1, 1, 44100, 24, s24, 9, NULL);
		d = open_buf(&b, NULL, NULL);
		CHECK(d != NULL);
		if (d != NULL) {
			CHECK_INT(mp_decoder_read(d, out, 3), 3);
			CHECK_INT(out[0], 0x1234);
			CHECK_INT(out[1], -32768);
			CHECK_INT(out[2], 32767);
			mp_decoder_close(d);
		}
		buf_free(&b);
	}
	/* 32-bit float (format tag 3). Conversion may differ by one LSB
	 * between rounding modes, so allow that. */
	{
		static const float f32[4] = { 0.0f, 0.5f, -1.0f, 1.0f };
		buf_init(&b);
		wav_file(&b, 3, 1, 44100, 32, f32, sizeof(f32), NULL);
		d = open_buf(&b, NULL, NULL);
		CHECK(d != NULL);
		if (d != NULL) {
			CHECK_INT(mp_decoder_read(d, out, 4), 4);
			CHECK_INT(out[0], 0);
			CHECK(out[1] >= 16383 && out[1] <= 16384);
			CHECK(out[2] <= -32767);
			CHECK(out[3] >= 32766);
			mp_decoder_close(d);
		}
		buf_free(&b);
	}
}

static void wav_surround_is_downmixed(void)
{
	/* 6 channels, 3 frames. Even channels -> left, odd -> right. */
	static const int16_t pcm[18] = {
		300, 600, 900, 1200, 1500, 1800,
		-300, -600, -900, -1200, -1500, -1800,
		32767, -32768, 32767, -32768, 32767, -32768,
	};
	Buf b;
	MpDecoder *d;
	int16_t out[6];
	buf_init(&b);
	wav_file(&b, 1, 6, 44100, 16, pcm, sizeof(pcm), NULL);
	d = open_buf(&b, NULL, NULL);
	CHECK(d != NULL);
	if (d != NULL) {
		CHECK_INT(mp_decoder_source_channels(d), 6);
		CHECK_INT(mp_decoder_channels(d), 2);
		CHECK_INT(mp_decoder_read(d, out, 3), 3);
		CHECK_INT(out[0], 900);    /* (300 + 900 + 1500) / 3 */
		CHECK_INT(out[1], 1200);   /* (600 + 1200 + 1800) / 3 */
		CHECK_INT(out[2], -900);
		CHECK_INT(out[3], -1200);
		CHECK_INT(out[4], 32767);  /* full-scale input cannot overflow */
		CHECK_INT(out[5], -32768);
		mp_decoder_close(d);
	}
	buf_free(&b);
}

static void downmix_function(void)
{
	int16_t in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, out[8];
	/* Mono and stereo pass through untouched. */
	CHECK_INT(mp_downmix_s16(in, 1, out, 8), 1);
	CHECK(memcmp(in, out, sizeof(in)) == 0);
	CHECK_INT(mp_downmix_s16(in, 2, out, 4), 2);
	CHECK(memcmp(in, out, sizeof(in)) == 0);
	/* 3 channels: left = avg(ch0, ch2), right = ch1. */
	CHECK_INT(mp_downmix_s16(in, 3, out, 2), 2);
	CHECK_INT(out[0], 2);
	CHECK_INT(out[1], 2);
	CHECK_INT(out[2], 5);
	CHECK_INT(out[3], 5);
	/* 4 channels (quad): avg(0,2), avg(1,3). */
	CHECK_INT(mp_downmix_s16(in, 4, out, 2), 2);
	CHECK_INT(out[0], 2);
	CHECK_INT(out[1], 3);
	CHECK_INT(out[2], 6);
	CHECK_INT(out[3], 7);
}

/* ---- FLAC --------------------------------------------------------------- */

static void flac_decoding(void)
{
	const size_t frames = 3000;
	int16_t *pcm = make_signal(frames, 2);
	int16_t *out = (int16_t *)calloc(frames * 2, sizeof(int16_t));
	Buf b;
	MpDecoder *d;
	buf_init(&b);
	/* 1024-frame blocks: 2 full FLAC frames and a short last one. */
	flac_file(&b, 44100, 2, pcm, frames, 1024, NULL);
	d = open_buf(&b, NULL, NULL);
	CHECK(d != NULL);
	if (d != NULL) {
		CHECK_INT(mp_decoder_format(d), MP_FORMAT_FLAC);
		CHECK_INT(mp_decoder_sample_rate(d), 44100);
		CHECK_INT(mp_decoder_channels(d), 2);
		CHECK_INT(mp_decoder_total_frames(d), frames);
		CHECK_INT(mp_decoder_read(d, out, frames + 100), frames);
		CHECK(memcmp(out, pcm, frames * 4) == 0);
		CHECK_INT(mp_decoder_read(d, out, 10), 0);
		/* Seek into the last (short) block. */
		CHECK(mp_decoder_seek(d, 2500));
		CHECK_INT(mp_decoder_read(d, out, 100), 100);
		CHECK(memcmp(out, pcm + 2500 * 2, 100 * 4) == 0);
		/* And back to the very start. */
		CHECK(mp_decoder_seek(d, 0));
		CHECK_INT(mp_decoder_read(d, out, 5), 5);
		CHECK(memcmp(out, pcm, 5 * 4) == 0);
		mp_decoder_close(d);
	}
	buf_free(&b);
	free(pcm);
	free(out);

	/* Mono, 22.05 kHz, several FLAC frames, a Vorbis comment block, and
	 * an ID3v2 tag in front. That combination trips an upstream dr_flac bug
	 * (an absolute seek to byte 42 that ignores the tag), which the decoder
	 * works around by hiding the tag from dr_flac - see flac_on_seek. Seeks
	 * go through the same path, so check those too. */
	{
		Buf vc, tag, fr;
		static const char *const fields[] = { "TITLE=x" };
		int16_t mono[1000], got[1000];
		size_t i;
		for (i = 0; i < 1000; i++)
			mono[i] = (int16_t)(i * 61 - 30000);
		buf_init(&vc);
		buf_init(&tag);
		buf_init(&fr);
		buf_init(&b);
		vorbis_comment(&vc, "v", fields, 1);
		id3_frame(&fr, 3, "TIT2", 0, 0, "x", 1);
		id3_tag(&b, 3, 0, &fr, 300);
		flac_file(&b, 22050, 1, mono, 1000, 256, &vc);
		d = open_buf(&b, NULL, NULL);
		CHECK(d != NULL);
		if (d != NULL) {
			CHECK_INT(mp_decoder_sample_rate(d), 22050);
			CHECK_INT(mp_decoder_channels(d), 1);
			CHECK_INT(mp_decoder_total_frames(d), 1000);
			CHECK_INT(mp_decoder_read(d, got, 1000), 1000);
			CHECK(memcmp(got, mono, sizeof(mono)) == 0);
			CHECK(mp_decoder_seek(d, 700));
			CHECK_INT(mp_decoder_read(d, got, 50), 50);
			CHECK(memcmp(got, mono + 700, 50 * sizeof(int16_t)) == 0);
			CHECK(mp_decoder_seek(d, 3));
			CHECK_INT(mp_decoder_read(d, got, 10), 10);
			CHECK(memcmp(got, mono + 3, 10 * sizeof(int16_t)) == 0);
			mp_decoder_close(d);
		}
		buf_free(&vc);
		buf_free(&tag);
		buf_free(&fr);
		buf_free(&b);
	}
}

/* ---- MP3 ---------------------------------------------------------------- */

static void check_silent_mp3(const Buf *b, uint64_t expect_frames)
{
	MpDecoder *d = open_buf(b, NULL, NULL);
	CHECK(d != NULL);
	if (d != NULL) {
		int16_t *out = (int16_t *)malloc((size_t)(expect_frames + 2000) * 2 * sizeof(int16_t));
		uint64_t got, i;
		int silent = 1;
		CHECK_INT(mp_decoder_format(d), MP_FORMAT_MP3);
		CHECK_INT(mp_decoder_sample_rate(d), 44100);
		CHECK_INT(mp_decoder_channels(d), 2);
		CHECK_INT(mp_decoder_total_frames(d), expect_frames);
		got = mp_decoder_read(d, out, expect_frames + 2000);
		CHECK_INT(got, expect_frames);
		for (i = 0; i < got * 2; i++)
			silent &= out[i] == 0;
		CHECK(silent);
		/* Seeking uses the seek table built at open. */
		CHECK(mp_decoder_seek(d, 1152 * 5 + 17));
		CHECK_INT(mp_decoder_read(d, out, expect_frames), expect_frames - (1152 * 5 + 17));
		free(out);
		mp_decoder_close(d);
	}
}

static void mp3_decoding(void)
{
	Buf b, tag, fr;
	buf_init(&b);
	mp3_silence(&b, 20);
	check_silent_mp3(&b, 20 * 1152);
	buf_free(&b);

	/* Tags at both ends must not be decoded as audio. */
	buf_init(&b);
	buf_init(&tag);
	buf_init(&fr);
	id3_frame(&fr, 3, "TIT2", 0, 0, "Tagged", 6);
	id3_tag(&b, 3, 0, &fr, 500);
	mp3_silence(&b, 20);
	id3v1_tag(&b, "Tagged", "Artist");
	check_silent_mp3(&b, 20 * 1152);
	buf_free(&b);
	buf_free(&tag);
	buf_free(&fr);
}

/* ---- Errors and files --------------------------------------------------- */

static void errors(void)
{
	Buf b;
	wchar_t err[256];
	MpStream *s;
	MpDecoder *d;

	/* Unknown content. */
	buf_init(&b);
	buf_str(&b, "definitely not audio");
	s = mp_stream_open_memory(b.data, b.len);
	err[0] = 0;
	d = mp_decoder_open_stream(s, L"x.txt", err, 256);
	CHECK(d == NULL);
	CHECK_WSTR(err, L"This is not an MP3, FLAC or WAV file.");
	mp_stream_close(s); /* on failure the caller still owns the stream */
	buf_free(&b);

	/* A WAV header with no fmt/data chunks. */
	buf_init(&b);
	buf_str(&b, "RIFF");
	buf_le32(&b, 4);
	buf_str(&b, "WAVE");
	s = mp_stream_open_memory(b.data, b.len);
	err[0] = 0;
	d = mp_decoder_open_stream(s, NULL, err, 256);
	CHECK(d == NULL);
	CHECK(err[0] != 0);
	mp_stream_close(s);
	buf_free(&b);

	/* Truncated FLAC (just the marker). */
	buf_init(&b);
	buf_str(&b, "fLaC");
	s = mp_stream_open_memory(b.data, b.len);
	d = mp_decoder_open_stream(s, NULL, err, 256);
	CHECK(d == NULL);
	mp_stream_close(s);
	buf_free(&b);

	/* A WAV claiming 0 channels. */
	{
		static const int16_t pcm[2] = { 0 };
		buf_init(&b);
		wav_file(&b, 1, 0, 44100, 16, pcm, sizeof(pcm), NULL);
		s = mp_stream_open_memory(b.data, b.len);
		d = mp_decoder_open_stream(s, NULL, err, 256);
		CHECK(d == NULL);
		if (d != NULL)
			mp_decoder_close(d);
		else
			mp_stream_close(s);
		buf_free(&b);
	}

	/* NULL stream. */
	CHECK(mp_decoder_open_stream(NULL, NULL, err, 256) == NULL);

	/* Missing file: a friendly message. */
	err[0] = 0;
	CHECK(mp_decoder_open_file(L"C:\\this\\does\\not\\exist.mp3", err, 256) == NULL);
	CHECK(wcsstr(err, L"could not be found") != NULL);
}

static void from_disk(void)
{
	Buf b;
	wchar_t path[MAX_PATH], err[256];
	MpDecoder *d;
	int16_t pcm[200], out[200];
	size_t i;
	for (i = 0; i < 200; i++)
		pcm[i] = (int16_t)(i * 97);
	buf_init(&b);
	wav_file(&b, 1, 2, 44100, 16, pcm, sizeof(pcm), NULL);
	CHECK(temp_file(&b, L".wav", path, MAX_PATH));
	d = mp_decoder_open_file(path, err, 256);
	CHECK(d != NULL);
	if (d != NULL) {
		CHECK_INT(mp_decoder_read(d, out, 100), 100);
		CHECK(memcmp(out, pcm, sizeof(pcm)) == 0);
		mp_decoder_close(d);
	}
	DeleteFileW(path);
	buf_free(&b);
}

int suite_decoder(void)
{
	detection();
	wav_16bit_stereo();
	wav_other_sample_formats();
	wav_surround_is_downmixed();
	downmix_function();
	flac_decoding();
	mp3_decoding();
	errors();
	from_disk();
	return 0;
}
