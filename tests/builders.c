/*
 * builders.c - see builders.h.
 */
#include "builders.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

void buf_init(Buf *b)
{
	memset(b, 0, sizeof(*b));
}

void buf_free(Buf *b)
{
	free(b->data);
	buf_init(b);
}

void buf_bytes(Buf *b, const void *p, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		while (b->len + n > cap)
			cap *= 2;
		b->data = (uint8_t *)realloc(b->data, cap);
		if (b->data == NULL)
			abort(); /* tests: failing loudly beats limping on */
		b->cap = cap;
	}
	if (n > 0)
		memcpy(b->data + b->len, p, n);
	b->len += n;
}

void buf_str(Buf *b, const char *s) { buf_bytes(b, s, strlen(s)); }

void buf_u8(Buf *b, unsigned v)
{
	uint8_t x = (uint8_t)v;
	buf_bytes(b, &x, 1);
}

void buf_zeros(Buf *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		buf_u8(b, 0);
}

void buf_be16(Buf *b, unsigned v) { buf_u8(b, v >> 8); buf_u8(b, v); }
void buf_be24(Buf *b, uint32_t v) { buf_u8(b, v >> 16); buf_u8(b, v >> 8); buf_u8(b, v); }
void buf_be32(Buf *b, uint32_t v) { buf_be16(b, v >> 16); buf_be16(b, v & 0xFFFF); }
void buf_le16(Buf *b, unsigned v) { buf_u8(b, v); buf_u8(b, v >> 8); }
void buf_le32(Buf *b, uint32_t v) { buf_le16(b, v & 0xFFFF); buf_le16(b, v >> 16); }

void buf_syncsafe(Buf *b, uint32_t v)
{
	buf_u8(b, (v >> 21) & 0x7F);
	buf_u8(b, (v >> 14) & 0x7F);
	buf_u8(b, (v >> 7) & 0x7F);
	buf_u8(b, v & 0x7F);
}

/* ---- ID3 ---------------------------------------------------------------- */

void id3_raw_frame(Buf *out, int major, const char *id, unsigned flags, const void *content, size_t len)
{
	if (major == 2) {
		buf_bytes(out, id, 3);
		buf_be24(out, (uint32_t)len);
	} else {
		buf_bytes(out, id, 4);
		if (major == 4)
			buf_syncsafe(out, (uint32_t)len);
		else
			buf_be32(out, (uint32_t)len);
		buf_be16(out, flags);
	}
	buf_bytes(out, content, len);
}

void id3_frame(Buf *out, int major, const char *id, unsigned flags, uint8_t encoding,
	const void *text, size_t text_len)
{
	Buf content;
	buf_init(&content);
	buf_u8(&content, encoding);
	buf_bytes(&content, text, text_len);
	id3_raw_frame(out, major, id, flags, content.data, content.len);
	buf_free(&content);
}

void id3_tag(Buf *out, int major, uint8_t flags, const Buf *frames, size_t padding)
{
	buf_str(out, "ID3");
	buf_u8(out, (unsigned)major);
	buf_u8(out, 0); /* revision */
	buf_u8(out, flags);
	buf_syncsafe(out, (uint32_t)(frames->len + padding));
	buf_bytes(out, frames->data, frames->len);
	buf_zeros(out, padding);
}

void id3v1_tag(Buf *out, const char *title, const char *artist)
{
	uint8_t t[128];
	memset(t, 0, sizeof(t));
	memcpy(t, "TAG", 3);
	memcpy(t + 3, title, strlen(title) < 30 ? strlen(title) : 30);
	memcpy(t + 33, artist, strlen(artist) < 30 ? strlen(artist) : 30);
	t[127] = 255; /* genre: none */
	buf_bytes(out, t, 128);
}

/* ---- WAV ---------------------------------------------------------------- */

void riff_chunk(Buf *out, const char *id, const void *data, size_t n)
{
	buf_bytes(out, id, 4);
	buf_le32(out, (uint32_t)n);
	buf_bytes(out, data, n);
	if (n & 1)
		buf_u8(out, 0);
}

void wav_file(Buf *out, uint16_t format_tag, uint16_t channels, uint32_t rate, uint16_t bits,
	const void *pcm, size_t pcm_bytes, const Buf *extra)
{
	Buf fmt;
	size_t size_pos;
	uint16_t block = (uint16_t)(channels * bits / 8);
	buf_init(&fmt);
	buf_le16(&fmt, format_tag);
	buf_le16(&fmt, channels);
	buf_le32(&fmt, rate);
	buf_le32(&fmt, rate * block);
	buf_le16(&fmt, block);
	buf_le16(&fmt, bits);
	if (format_tag != 1)
		buf_le16(&fmt, 0); /* cbSize, required for non-PCM formats */

	buf_str(out, "RIFF");
	size_pos = out->len;
	buf_le32(out, 0); /* patched below */
	buf_str(out, "WAVE");
	riff_chunk(out, "fmt ", fmt.data, fmt.len);
	riff_chunk(out, "data", pcm, pcm_bytes);
	if (extra != NULL)
		buf_bytes(out, extra->data, extra->len);
	{
		uint32_t riff_size = (uint32_t)(out->len - size_pos - 4);
		out->data[size_pos] = (uint8_t)riff_size;
		out->data[size_pos + 1] = (uint8_t)(riff_size >> 8);
		out->data[size_pos + 2] = (uint8_t)(riff_size >> 16);
		out->data[size_pos + 3] = (uint8_t)(riff_size >> 24);
	}
	buf_free(&fmt);
}

/* ---- FLAC --------------------------------------------------------------- */

void vorbis_comment(Buf *out, const char *vendor, const char *const *fields, size_t count)
{
	size_t i;
	buf_le32(out, (uint32_t)strlen(vendor));
	buf_str(out, vendor);
	buf_le32(out, (uint32_t)count);
	for (i = 0; i < count; i++) {
		buf_le32(out, (uint32_t)strlen(fields[i]));
		buf_str(out, fields[i]);
	}
}

/* CRC-8, polynomial x^8 + x^2 + x + 1, as FLAC frame headers use. */
static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t crc = 0;
	size_t i;
	int k;
	for (i = 0; i < n; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
	}
	return crc;
}

/* CRC-16, polynomial x^16 + x^15 + x^2 + 1, over a whole FLAC frame. */
static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t crc = 0;
	size_t i;
	int k;
	for (i = 0; i < n; i++) {
		crc ^= (uint16_t)(p[i] << 8);
		for (k = 0; k < 8; k++)
			crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
	}
	return crc;
}

/* FLAC codes frame numbers like UTF-8 code points. */
static void flac_utf8_number(Buf *b, uint32_t v)
{
	if (v < 0x80) {
		buf_u8(b, v);
	} else if (v < 0x800) {
		buf_u8(b, 0xC0 | (v >> 6));
		buf_u8(b, 0x80 | (v & 0x3F));
	} else if (v < 0x10000) {
		buf_u8(b, 0xE0 | (v >> 12));
		buf_u8(b, 0x80 | ((v >> 6) & 0x3F));
		buf_u8(b, 0x80 | (v & 0x3F));
	} else {
		buf_u8(b, 0xF0 | (v >> 18));
		buf_u8(b, 0x80 | ((v >> 12) & 0x3F));
		buf_u8(b, 0x80 | ((v >> 6) & 0x3F));
		buf_u8(b, 0x80 | (v & 0x3F));
	}
}

void flac_file(Buf *out, uint32_t rate, uint32_t channels, const int16_t *samples, size_t frames,
	uint32_t block_size, const Buf *comment)
{
	uint64_t packed;
	size_t done = 0;
	uint32_t frame_no = 0;

	buf_str(out, "fLaC");
	/* STREAMINFO, 34 bytes. Block type 0; "last" flag only if no comment. */
	buf_u8(out, comment ? 0x00 : 0x80);
	buf_be24(out, 34);
	buf_be16(out, block_size); /* min block size */
	buf_be16(out, block_size); /* max block size */
	buf_be24(out, 0);          /* min frame size: unknown */
	buf_be24(out, 0);          /* max frame size: unknown */
	/* 20 bits rate, 3 bits channels-1, 5 bits bits-per-sample-1, 36 bits
	 * total samples. */
	packed = (uint64_t)rate << 44 | (uint64_t)(channels - 1) << 41 | (uint64_t)(16 - 1) << 36 | frames;
	buf_be32(out, (uint32_t)(packed >> 32));
	buf_be32(out, (uint32_t)packed);
	buf_zeros(out, 16); /* MD5 of the audio: all zeros means "not computed" */

	if (comment != NULL) {
		buf_u8(out, 0x80 | 4); /* last block, VORBIS_COMMENT */
		buf_be24(out, (uint32_t)comment->len);
		buf_bytes(out, comment->data, comment->len);
	}

	while (done < frames) {
		size_t n = frames - done < block_size ? frames - done : block_size, i;
		uint32_t c;
		Buf fr;
		buf_init(&fr);
		buf_u8(&fr, 0xFF);
		buf_u8(&fr, 0xF8); /* sync code, fixed block size strategy */
		buf_u8(&fr, 0x70); /* block size: 16-bit value at end of header; rate: from STREAMINFO */
		buf_u8(&fr, ((channels - 1) << 4) | 0x08); /* independent channels, 16 bits */
		flac_utf8_number(&fr, frame_no);
		buf_be16(&fr, (unsigned)(n - 1));
		buf_u8(&fr, crc8(fr.data, fr.len));
		for (c = 0; c < channels; c++) {
			buf_u8(&fr, 0x02); /* VERBATIM subframe, no wasted bits */
			for (i = 0; i < n; i++)
				buf_be16(&fr, (uint16_t)samples[(done + i) * channels + c]);
		}
		buf_be16(&fr, crc16(fr.data, fr.len));
		buf_bytes(out, fr.data, fr.len);
		buf_free(&fr);
		done += n;
		frame_no++;
	}
}

/* ---- MP3 ---------------------------------------------------------------- */

void mp3_silence(Buf *out, int frames)
{
	int f;
	for (f = 0; f < frames; f++) {
		/* FF FB: sync, MPEG-1, Layer III, no CRC. 90: 128 kbit/s, 44.1 kHz,
		 * no padding. 00: stereo. Frame length is
		 * 144 * 128000 / 44100 = 417 bytes. With all-zero side info every
		 * granule has zero bits of Huffman data, i.e. silence. */
		buf_u8(out, 0xFF);
		buf_u8(out, 0xFB);
		buf_u8(out, 0x90);
		buf_u8(out, 0x00);
		buf_zeros(out, 417 - 4);
	}
}

/* ---- Temp files --------------------------------------------------------- */

int temp_file(const Buf *b, const wchar_t *ext, wchar_t *path, size_t cap)
{
	static unsigned counter = 0;
	wchar_t dir[MAX_PATH];
	HANDLE h;
	DWORD written = 0;
	if (!GetTempPathW(MAX_PATH, dir))
		return 0;
	/* Include the process ID so parallel CTest runs cannot collide. */
	if (_snwprintf(path, cap, L"%lsmp_test_%lu_%u%ls", dir, (unsigned long)GetCurrentProcessId(),
		++counter, ext) < 0)
		return 0;
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	WriteFile(h, b->data, (DWORD)b->len, &written, NULL);
	CloseHandle(h);
	return written == b->len;
}
