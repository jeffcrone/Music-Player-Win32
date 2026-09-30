/*
 * tags.h - read the title and artist from MP3, FLAC and WAV files.
 *
 * Supported tag formats:
 *   - ID3v2.2, 2.3 and 2.4 at the start of any file (MP3 normally, but some
 *     tools also put one in front of FLAC). Handles syncsafe sizes, extended
 *     headers, tag- and frame-level unsynchronization, the data length
 *     indicator, grouping bytes, all four text encodings, and skips
 *     compressed or encrypted frames instead of misreading them.
 *   - ID3v1 / v1.1 (the 128-byte "TAG" block at the end of an MP3).
 *   - FLAC VORBIS_COMMENT blocks (TITLE, ARTIST, ALBUMARTIST).
 *   - WAV RIFF "LIST/INFO" chunks (INAM, IART) and "id3 " chunks holding an
 *     ID3v2 tag, which is how many taggers store tags in WAV files.
 *
 * Only the few bytes that matter are read: embedded cover art, which can be
 * megabytes, is skipped over with a seek rather than loaded.
 *
 * Precedence when a file has several tags: ID3v2 at the start of the file,
 * then the format's own tags (Vorbis comment / RIFF id3 chunk / RIFF INFO),
 * then ID3v1. For the artist, an album-artist field is used only when no
 * source at all has a track artist.
 */
#ifndef MP_TAGS_H
#define MP_TAGS_H

#include <wchar.h>

#include "stream.h"

/* Longer titles are truncated; a label could not show them anyway. */
#define MP_TAG_MAX 256

typedef struct {
	wchar_t title[MP_TAG_MAX];  /* empty string if the file has no title */
	wchar_t artist[MP_TAG_MAX]; /* empty string if the file has no artist */
} MpTags;

/* Fills *tags (always initializing it to empty strings first). Returns 1 if
 * a title or an artist was found, 0 otherwise. Never fails on malformed
 * data: whatever can be read safely is used and the rest is ignored. */
int mp_tags_read_stream(MpStream *s, MpTags *tags);

/* Convenience wrapper that opens the file. Returns 0 if it cannot be opened
 * or has no usable tags. */
int mp_tags_read_file(const wchar_t *path, MpTags *tags);

#endif
