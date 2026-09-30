/*
 * builders.h - construct test files byte by byte.
 *
 * The tests build their MP3, FLAC and WAV inputs in memory rather than
 * shipping binary fixtures, so every input is visible and explained in the
 * test that uses it, and edge cases (a bad size field, a stray BOM) are one
 * line to create.
 */
#ifndef MP_TEST_BUILDERS_H
#define MP_TEST_BUILDERS_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_bytes(Buf *b, const void *p, size_t n);
void buf_str(Buf *b, const char *s); /* without the NUL */
void buf_u8(Buf *b, unsigned v);
void buf_zeros(Buf *b, size_t n);
void buf_be16(Buf *b, unsigned v);
void buf_be24(Buf *b, uint32_t v);
void buf_be32(Buf *b, uint32_t v);
void buf_le16(Buf *b, unsigned v);
void buf_le32(Buf *b, uint32_t v);
void buf_syncsafe(Buf *b, uint32_t v);

/* ID3v2 text frame (header + encoding byte + text bytes). For v2.2 the ID
 * is 3 characters, otherwise 4. flags goes into the 2 flag bytes (v2.3/4).
 * The size is written syncsafe for v2.4, plain for v2.2/2.3. */
void id3_frame(Buf *out, int major, const char *id, unsigned flags, uint8_t encoding,
	const void *text, size_t text_len);
/* A raw frame with arbitrary content (no encoding byte added). */
void id3_raw_frame(Buf *out, int major, const char *id, unsigned flags, const void *content, size_t len);
/* Whole tag: "ID3" header + frames + `padding` zero bytes. */
void id3_tag(Buf *out, int major, uint8_t flags, const Buf *frames, size_t padding);
/* ID3v1: 128 bytes, fields NUL-padded. */
void id3v1_tag(Buf *out, const char *title, const char *artist);

/* A RIFF chunk: id, little-endian size, data, pad byte if odd. */
void riff_chunk(Buf *out, const char *id, const void *data, size_t n);
/* A complete WAV file. `extra` (may be NULL) is appended after the data
 * chunk - it holds LIST/id3 chunks built with riff_chunk. */
void wav_file(Buf *out, uint16_t format_tag, uint16_t channels, uint32_t rate, uint16_t bits,
	const void *pcm, size_t pcm_bytes, const Buf *extra);

/* A Vorbis comment payload (vendor + fields "KEY=value"). */
void vorbis_comment(Buf *out, const char *vendor, const char *const *fields, size_t count);
/* A valid FLAC file with 16-bit samples stored in VERBATIM subframes. If
 * comment is non-NULL it becomes a VORBIS_COMMENT block. */
void flac_file(Buf *out, uint32_t rate, uint32_t channels, const int16_t *samples, size_t frames,
	uint32_t block_size, const Buf *comment);

/* `frames` MPEG-1 Layer III frames of digital silence, 44.1 kHz stereo,
 * 128 kbit/s. Each decodes to 1152 sample frames of zeros. */
void mp3_silence(Buf *out, int frames);

/* Writes b to a new file in %TEMP% with the given extension (e.g. L".mp3").
 * Returns 1 on success. Delete it with DeleteFileW. */
int temp_file(const Buf *b, const wchar_t *ext, wchar_t *path, size_t cap);

#endif
