/*
 * tags.c - title/artist/track extraction. See tags.h for what is supported.
 *
 * Every size field read from a file is treated as hostile: it is checked
 * against what is actually left before it is used, so a truncated or
 * corrupt tag can make us miss a title but can never make us read out of
 * bounds or allocate gigabytes.
 */
#include "tags.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h> /* wsprintfW */

#include "text.h"

/* How much of a single text frame / field we look at. 4 KB holds far more
 * than MP_TAG_MAX characters in any encoding. */
#define FIELD_READ_MAX 4096u

/* A v2.3 tag with tag-level unsynchronization has to be loaded whole to be
 * decoded. Refuse anything bigger than this (it would be almost all cover
 * art anyway). */
#define UNSYNC_TAG_MAX (16u * 1024u * 1024u)

/* Vorbis comment blocks can carry base64 cover art; the text fields come
 * first in practice, so this much is plenty. */
#define VORBIS_BLOCK_MAX (1024u * 1024u)

/* Upper bounds on loop iterations, so a crafted file with thousands of
 * tiny chunks cannot keep us busy. */
#define MAX_FLAC_BLOCKS 256
#define MAX_RIFF_CHUNKS 4096
#define MAX_STACKED_ID3 4

typedef struct {
	wchar_t title[MP_TAG_MAX];
	wchar_t artist[MP_TAG_MAX];
	wchar_t album_artist[MP_TAG_MAX];
	/* Kept as text until the end, so that every source's value can be
	 * checked with mp_tags_parse_track and a bad one passed over. */
	wchar_t track[MP_TAG_MAX];

	/* Album art, only looked for when want_picture is set. pic_front
	 * records that the picture held is the front cover, which nothing
	 * later in the same source can beat. */
	int want_picture;
	uint8_t *pic;
	size_t pic_size;
	int pic_front;
} Slots;

/* ID3 and FLAC number picture types the same way; 3 is the front cover. */
#define PICTURE_FRONT_COVER 3

/* Keeps a copy of a picture found in this source: the first one, unless a
 * front cover turns up later. */
static void offer_picture(Slots *slots, const uint8_t *img, size_t n, int type)
{
	uint8_t *copy;
	if (!slots->want_picture || n == 0)
		return;
	if (slots->pic != NULL && (slots->pic_front || type != PICTURE_FRONT_COVER))
		return;
	copy = (uint8_t *)malloc(n);
	if (copy == NULL)
		return;
	memcpy(copy, img, n);
	free(slots->pic);
	slots->pic = copy;
	slots->pic_size = n;
	slots->pic_front = type == PICTURE_FRONT_COVER;
}

/* Whether a source still has use for another picture. */
static int wants_picture(const Slots *slots)
{
	return slots->want_picture && !(slots->pic != NULL && slots->pic_front);
}

/* First value wins: a slot that already holds something is left alone. That
 * matches the ID3 convention that repeated frames are a mistake and the
 * first one is authoritative. Whitespace-only values do not count. */
static void offer(wchar_t *slot, wchar_t *value)
{
	if (slot[0] != 0)
		return;
	if (mp_trim(value) == 0)
		return;
	mp_wcopy(slot, MP_TAG_MAX, value);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t be24(const uint8_t *p)
{
	return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* ID3v2 "syncsafe" integers use 7 bits per byte so that no size field can
 * contain a 0xFF byte that an old MP3 decoder might mistake for audio. */
static uint32_t syncsafe32(const uint8_t *p)
{
	return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 |
		(uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

/* Undo ID3 unsynchronization: the encoder inserted a 0x00 after every 0xFF,
 * so drop each 0x00 that follows a 0xFF. In place; returns the new length. */
static size_t remove_unsync(uint8_t *buf, size_t n)
{
	size_t in = 0, out = 0;
	while (in < n) {
		uint8_t b = buf[in++];
		buf[out++] = b;
		if (b == 0xFF && in < n && buf[in] == 0x00)
			in++;
	}
	return out;
}

/* ---- ID3v2 -------------------------------------------------------------- */

/* The tag body, either still in the file or loaded into memory (only when
 * tag-level unsynchronization forces us to decode it all first). */
typedef struct {
	MpStream *s;
	int64_t base;
	const uint8_t *mem;
	size_t size;
} Body;

static int body_read(const Body *b, size_t off, void *buf, size_t n)
{
	if (off > b->size || n > b->size - off)
		return 0;
	if (b->mem != NULL) {
		memcpy(buf, b->mem + off, n);
		return 1;
	}
	return mp_stream_read_at(b->s, b->base + (int64_t)off, buf, n);
}

/* Decode an ID3v2 text frame's content (encoding byte + text). */
static void id3_text(const uint8_t *d, size_t n, wchar_t *out)
{
	uint8_t enc;
	out[0] = 0;
	if (n < 1)
		return;
	enc = d[0];
	d++;
	n--;
	switch (enc) {
	case 0: /* ISO-8859-1 on paper; Windows-1252 in practice. */
		mp_cp1252_decode(d, n, out, MP_TAG_MAX);
		break;
	case 1: /* UTF-16 with a byte order mark. */
		if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF)
			mp_utf16_decode(d + 2, n - 2, 1, out, MP_TAG_MAX);
		else if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE)
			mp_utf16_decode(d + 2, n - 2, 0, out, MP_TAG_MAX);
		else /* BOM missing: a common tagger bug; they were all little-endian. */
			mp_utf16_decode(d, n, 0, out, MP_TAG_MAX);
		break;
	case 2: /* UTF-16BE, no BOM by spec - but tolerate one. */
		if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF)
			mp_utf16_decode(d + 2, n - 2, 1, out, MP_TAG_MAX);
		else if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE)
			mp_utf16_decode(d + 2, n - 2, 0, out, MP_TAG_MAX);
		else
			mp_utf16_decode(d, n, 1, out, MP_TAG_MAX);
		break;
	case 3: /* UTF-8; skip a stray BOM some tools add. */
		if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
			d += 3;
			n -= 3;
		}
		mp_utf8_decode(d, n, out, MP_TAG_MAX);
		break;
	default:
		break;
	}
	/* Decoding stops at the first NUL, which is exactly right for v2.4's
	 * NUL-separated multiple values: we keep the first one. */
}

static wchar_t *id3_slot_for(Slots *slots, const char *id, int major)
{
	if (major == 2) {
		if (memcmp(id, "TT2", 3) == 0) return slots->title;
		if (memcmp(id, "TP1", 3) == 0) return slots->artist;
		if (memcmp(id, "TP2", 3) == 0) return slots->album_artist;
		if (memcmp(id, "TRK", 3) == 0) return slots->track;
	} else {
		if (memcmp(id, "TIT2", 4) == 0) return slots->title;
		if (memcmp(id, "TPE1", 4) == 0) return slots->artist;
		if (memcmp(id, "TPE2", 4) == 0) return slots->album_artist;
		if (memcmp(id, "TRCK", 4) == 0) return slots->track;
	}
	return NULL;
}

/*
 * Finds the image inside an APIC (v2.3/2.4) or PIC (v2.2) frame's content:
 *   encoding byte
 *   MIME type, Latin-1, NUL-terminated  (v2.2: a 3-letter format, "JPG")
 *   picture type byte
 *   description in the frame's encoding, NUL-terminated (two NUL bytes,
 *     on a 2-byte boundary, for the UTF-16 encodings)
 *   the image file's bytes, to the end of the frame
 * Sets *type and *offset (where the image starts). Returns 0 if the frame
 * is malformed or has no image bytes.
 */
static int apic_parse(const uint8_t *d, size_t n, int v22, int *type, size_t *offset)
{
	size_t p = 1;
	uint8_t enc;
	if (n < 2)
		return 0;
	enc = d[0];
	if (v22) {
		p += 3;
	} else {
		while (p < n && d[p] != 0)
			p++;
		p++; /* the NUL */
	}
	if (p >= n)
		return 0;
	*type = d[p++];
	if (enc == 1 || enc == 2) {
		/* UTF-16: a 16-bit NUL, so step two bytes at a time from the
		 * description's start; a 0x00 high or low byte of a real
		 * character must not end it. */
		while (p + 1 < n && !(d[p] == 0 && d[p + 1] == 0))
			p += 2;
		p += 2;
	} else {
		while (p < n && d[p] != 0)
			p++;
		p++;
	}
	if (p >= n)
		return 0;
	*offset = p;
	return 1;
}

static int is_picture_frame(const char *id, int major)
{
	return major == 2 ? memcmp(id, "PIC", 3) == 0 : memcmp(id, "APIC", 4) == 0;
}

/*
 * How to read a frame's content, from its flags: how many bytes come
 * before the content proper (*skip) and whether it is unsynchronized.
 * Returns 0 for frames that cannot be read at all (compressed or
 * encrypted): skipped rather than misread.
 */
static int frame_layout(int major, unsigned fflags, int tag_flags, size_t *skip, int *unsync)
{
	*skip = 0;
	*unsync = 0;
	if (major == 3) {
		/* 0x0080 compression (zlib), 0x0040 encryption. */
		if (fflags & 0x00C0)
			return 0;
		if (fflags & 0x0020)
			*skip += 1; /* grouping identity byte */
	} else if (major == 4) {
		/* 0x0008 compression, 0x0004 encryption. */
		if (fflags & 0x000C)
			return 0;
		if (fflags & 0x0040)
			*skip += 1; /* grouping identity byte */
		if (fflags & 0x0001)
			*skip += 4; /* data length indicator */
		/* In v2.4 the tag-level unsync flag means "every frame is
		 * unsynchronized", so it applies per frame. */
		*unsync = (fflags & 0x0002) || (tag_flags & 0x80);
	}
	return 1;
}

static int is_frame_id_char(uint8_t c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static void id3_frames(const Body *body, size_t pos, int major, int tag_flags, Slots *slots)
{
	const size_t hdr_len = (major == 2) ? 6 : 10;
	const size_t id_len = (major == 2) ? 3 : 4;
	uint8_t buf[FIELD_READ_MAX];
	wchar_t text[MP_TAG_MAX];

	while (pos <= body->size && body->size - pos >= hdr_len) {
		uint8_t fh[10];
		char id[5] = { 0 };
		uint32_t fsize;
		unsigned fflags = 0;
		size_t k, n, skip = 0;
		int frame_unsync = 0;
		wchar_t *slot;

		if (!body_read(body, pos, fh, hdr_len))
			break;
		/* A zero byte where a frame ID should be is the start of padding. */
		if (fh[0] == 0)
			break;
		for (k = 0; k < id_len; k++) {
			if (!is_frame_id_char(fh[k]))
				return; /* garbage: stop rather than guess */
			id[k] = (char)fh[k];
		}
		if (major == 2) {
			fsize = be24(fh + 3);
		} else if (major == 3) {
			fsize = be32(fh + 4);
			fflags = (unsigned)fh[8] << 8 | fh[9];
		} else {
			/* v2.4 frame sizes are syncsafe, but some old iTunes versions
			 * wrote plain 32-bit sizes into v2.4 tags. A byte with its high
			 * bit set cannot be syncsafe, so fall back to plain in that case. */
			if ((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80)
				fsize = be32(fh + 4);
			else
				fsize = syncsafe32(fh + 4);
			fflags = (unsigned)fh[8] << 8 | fh[9];
		}
		pos += hdr_len;
		if (fsize > body->size - pos)
			break; /* frame claims to run past the tag */

		slot = id3_slot_for(slots, id, major);
		if (slot != NULL && slot[0] == 0 && fsize > 0) {
			int usable = frame_layout(major, fflags, tag_flags, &skip, &frame_unsync);
			n = fsize < FIELD_READ_MAX ? fsize : FIELD_READ_MAX;
			if (usable && n > skip && body_read(body, pos, buf, n)) {
				size_t len = n - skip;
				if (frame_unsync)
					len = remove_unsync(buf + skip, len);
				id3_text(buf + skip, len, text);
				offer(slot, text);
			}
		} else if (is_picture_frame(id, major) && wants_picture(slots) && fsize > 0 &&
			fsize <= MP_PICTURE_MAX &&
			frame_layout(major, fflags, tag_flags, &skip, &frame_unsync) && fsize > skip) {
			/* A picture is read whole: unlike text, it cannot be cut
			 * short. Only reached when a picture was asked for. */
			uint8_t *frame = (uint8_t *)malloc(fsize);
			if (frame != NULL && body_read(body, pos, frame, fsize)) {
				uint8_t *d = frame + skip;
				size_t len = fsize - skip, off = 0;
				int type = 0;
				if (frame_unsync)
					len = remove_unsync(d, len);
				if (apic_parse(d, len, major == 2, &type, &off))
					offer_picture(slots, d + off, len - off, type);
			}
			free(frame);
		}
		pos += fsize;
	}
}

/*
 * Parses an ID3v2 tag at `offset`, if there is one. `avail` is how many
 * bytes of the stream belong to the region the tag lives in (the rest of the
 * file, or the enclosing RIFF chunk). Returns the number of bytes the tag
 * occupies, or 0 if there is no ID3v2 tag at that offset.
 */
static int64_t id3v2_parse(MpStream *s, int64_t offset, int64_t avail, Slots *slots)
{
	uint8_t h[10];
	uint32_t body_size;
	int major, flags;
	int64_t total;
	Body body;
	uint8_t *loaded = NULL;
	size_t pos = 0;

	if (avail < 10 || !mp_stream_read_at(s, offset, h, 10))
		return 0;
	if (h[0] != 'I' || h[1] != 'D' || h[2] != '3')
		return 0;
	if (h[3] == 0xFF || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80))
		return 0;
	major = h[3];
	flags = h[5];
	body_size = syncsafe32(h + 6);
	total = 10 + (int64_t)body_size + ((major == 4 && (flags & 0x10)) ? 10 : 0);

	/* Versions we do not understand are still skipped over correctly. In
	 * v2.2, flag 0x40 means the whole tag is compressed - no known tool ever
	 * wrote that and the spec never defined the scheme. */
	if (major < 2 || major > 4 || (major == 2 && (flags & 0x40)))
		return total;

	memset(&body, 0, sizeof(body));
	body.s = s;
	body.base = offset + 10;
	/* A tag claiming to be bigger than the file (a truncated download, say)
	 * is read as far as the data actually goes. */
	body.size = body_size;
	if ((int64_t)body.size > avail - 10)
		body.size = (size_t)(avail - 10);

	if ((flags & 0x80) && major <= 3) {
		/* v2.2/v2.3 tag-level unsynchronization covers the frame headers
		 * too, so frame offsets in the file are meaningless until the
		 * whole body is decoded. */
		if (body.size > UNSYNC_TAG_MAX)
			return total;
		loaded = (uint8_t *)malloc(body.size ? body.size : 1);
		if (loaded == NULL)
			return total;
		if (!mp_stream_read_at(s, body.base, loaded, body.size)) {
			free(loaded);
			return total;
		}
		body.size = remove_unsync(loaded, body.size);
		body.mem = loaded;
	}

	if (flags & 0x40) {
		/* Extended header; we only need to know how long it is. */
		uint8_t eh[4];
		if (!body_read(&body, 0, eh, 4)) {
			free(loaded);
			return total;
		}
		if (major == 3) {
			/* v2.3: size excludes the 4-byte size field itself. */
			uint32_t ext = be32(eh);
			if (ext > body.size - 4) {
				free(loaded);
				return total;
			}
			pos = 4 + ext;
		} else {
			/* v2.4: syncsafe, and includes itself (minimum 6). */
			uint32_t ext = syncsafe32(eh);
			if (ext < 6 || ext > body.size) {
				free(loaded);
				return total;
			}
			pos = ext;
		}
	}

	id3_frames(&body, pos, major, flags, slots);
	free(loaded);
	return total;
}

/* ---- ID3v1 -------------------------------------------------------------- */

static void id3v1_parse(MpStream *s, Slots *slots)
{
	uint8_t t[128];
	wchar_t text[MP_TAG_MAX];
	int64_t size = mp_stream_size(s);
	if (size < 128 || !mp_stream_read_at(s, size - 128, t, 128))
		return;
	if (t[0] != 'T' || t[1] != 'A' || t[2] != 'G')
		return;
	/* Fixed 30-byte fields, padded with NULs or (by some tools) spaces;
	 * the decoder stops at the NUL and offer() trims the spaces. */
	mp_cp1252_decode(t + 3, 30, text, MP_TAG_MAX);
	offer(slots->title, text);
	mp_cp1252_decode(t + 33, 30, text, MP_TAG_MAX);
	offer(slots->artist, text);
	/* ID3v1.1: the comment field (bytes 97..126) gave up its last byte for
	 * the track number, marked by a zero byte before it. In plain v1.0 a
	 * 30-character comment fills byte 125, so no zero, so no track. */
	if (t[125] == 0 && t[126] != 0) {
		wsprintfW(text, L"%u", (unsigned)t[126]);
		offer(slots->track, text);
	}
}

/* ---- FLAC / Vorbis comments --------------------------------------------- */

static int field_is(const uint8_t *key, size_t key_len, const char *name)
{
	return strlen(name) == key_len && mp_ascii_ieq_n((const char *)key, name, key_len);
}

static void vorbis_comments(const uint8_t *d, size_t n, Slots *slots)
{
	size_t p;
	uint32_t vendor_len, count, i;
	wchar_t text[MP_TAG_MAX];

	/* Everything in a Vorbis comment is little-endian, unlike the FLAC
	 * block header around it. */
	if (n < 4)
		return;
	vendor_len = le32(d);
	p = 4;
	if (vendor_len > n - p)
		return;
	p += vendor_len;
	if (n - p < 4)
		return;
	count = le32(d + p);
	p += 4;
	for (i = 0; i < count; i++) {
		uint32_t len;
		const uint8_t *field;
		size_t eq;
		if (n - p < 4)
			return;
		len = le32(d + p);
		p += 4;
		if (len > n - p)
			return;
		field = d + p;
		p += len;
		for (eq = 0; eq < len && field[eq] != '='; eq++)
			;
		if (eq == len)
			continue;
		mp_utf8_decode(field + eq + 1, len - eq - 1, text, MP_TAG_MAX);
		/* Field names are case-insensitive ASCII per the Vorbis spec. There
		 * is no standard album-artist name, so accept the common spellings. */
		if (field_is(field, eq, "TITLE"))
			offer(slots->title, text);
		else if (field_is(field, eq, "ARTIST"))
			offer(slots->artist, text);
		else if (field_is(field, eq, "ALBUMARTIST") || field_is(field, eq, "ALBUM ARTIST") ||
			field_is(field, eq, "ALBUM_ARTIST"))
			offer(slots->album_artist, text);
		/* TRACKNUMBER is the standard name; a few old taggers wrote TRACK. */
		else if (field_is(field, eq, "TRACKNUMBER") || field_is(field, eq, "TRACK"))
			offer(slots->track, text);
	}
}

/*
 * A FLAC PICTURE block (all big-endian):
 *   picture type (32), MIME type length (32) + MIME type, description
 *   length (32) + description (UTF-8), width, height, color depth, number
 *   of colors (32 each), data length (32) + the image file's bytes.
 * Every length is checked against what is left before it is used.
 */
static void flac_picture(const uint8_t *d, size_t n, Slots *slots)
{
	size_t p;
	uint32_t type, len;
	if (n < 8)
		return;
	type = be32(d);
	len = be32(d + 4);
	p = 8;
	if (len > n - p)
		return;
	p += len;
	if (n - p < 4)
		return;
	len = be32(d + p);
	p += 4;
	if (len > n - p)
		return;
	p += len;
	if (n - p < 20)
		return;
	p += 16;
	len = be32(d + p);
	p += 4;
	if (len == 0 || len > n - p)
		return;
	offer_picture(slots, d + p, len, (int)type);
}

static void flac_parse(MpStream *s, int64_t off, Slots *slots)
{
	int blocks;
	for (blocks = 0; blocks < MAX_FLAC_BLOCKS; blocks++) {
		uint8_t bh[4];
		int last, type;
		uint32_t len;
		if (!mp_stream_read_at(s, off, bh, 4))
			return;
		last = bh[0] & 0x80;
		type = bh[0] & 0x7F;
		len = be24(bh + 1);
		off += 4;
		if (type == 127)
			return; /* reserved as invalid by the spec */
		if (type == 4) {
			size_t want = len < VORBIS_BLOCK_MAX ? len : VORBIS_BLOCK_MAX;
			uint8_t *buf = (uint8_t *)malloc(want ? want : 1);
			if (buf != NULL) {
				size_t got = 0;
				/* A partial read (truncated file) still yields the fields
				 * that made it; vorbis_comments bounds-checks every one. */
				if (mp_stream_seek(s, off, MP_SEEK_SET))
					got = mp_stream_read(s, buf, want);
				vorbis_comments(buf, got, slots);
				free(buf);
			}
		} else if (type == 6 && wants_picture(slots) && len <= MP_PICTURE_MAX) {
			uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
			if (buf != NULL) {
				size_t got = 0;
				if (mp_stream_seek(s, off, MP_SEEK_SET))
					got = mp_stream_read(s, buf, len);
				flac_picture(buf, got, slots);
				free(buf);
			}
		}
		off += len;
		if (last)
			return;
	}
}

/* ---- WAV (RIFF) --------------------------------------------------------- */

static void riff_info(MpStream *s, int64_t off, int64_t end, Slots *slots)
{
	uint8_t buf[FIELD_READ_MAX];
	wchar_t text[MP_TAG_MAX];
	int i;
	for (i = 0; i < MAX_RIFF_CHUNKS && end - off >= 8; i++) {
		uint8_t ch[8];
		uint32_t len;
		wchar_t *slot = NULL;
		if (!mp_stream_read_at(s, off, ch, 8))
			return;
		len = le32(ch + 4);
		if (memcmp(ch, "INAM", 4) == 0)
			slot = slots->title;
		else if (memcmp(ch, "IART", 4) == 0)
			slot = slots->artist;
		else if (memcmp(ch, "ITRK", 4) == 0)
			slot = slots->track;
		if (slot != NULL) {
			size_t n = len < FIELD_READ_MAX ? len : FIELD_READ_MAX;
			if ((int64_t)n > end - off - 8)
				n = (size_t)(end - off - 8);
			if (mp_stream_read_at(s, off + 8, buf, n)) {
				/* The RIFF spec predates Unicode and just says "ZSTR".
				 * Modern tools write UTF-8, older ones the ANSI code page;
				 * valid UTF-8 is very unlikely to be accidental. */
				if (mp_utf8_is_valid(buf, n))
					mp_utf8_decode(buf, n, text, MP_TAG_MAX);
				else
					mp_cp1252_decode(buf, n, text, MP_TAG_MAX);
				offer(slot, text);
			}
		}
		/* Chunks are padded to an even length. */
		off += 8 + (int64_t)len + (len & 1);
	}
}

static void wav_parse(MpStream *s, Slots *id3_slots, Slots *info_slots)
{
	uint8_t h[12];
	int64_t size = mp_stream_size(s), end, off = 12;
	int i;
	if (!mp_stream_read_at(s, 0, h, 12))
		return;
	if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
		return;
	/* Trust the smaller of the RIFF size and the real file size: writers
	 * that crashed mid-recording often leave the RIFF size wrong. */
	end = 8 + (int64_t)le32(h + 4);
	if (end > size)
		end = size;
	for (i = 0; i < MAX_RIFF_CHUNKS && end - off >= 8; i++) {
		uint8_t ch[8];
		uint32_t len;
		int64_t data;
		if (!mp_stream_read_at(s, off, ch, 8))
			return;
		len = le32(ch + 4);
		data = off + 8;
		if (memcmp(ch, "LIST", 4) == 0 && len >= 4) {
			uint8_t type[4];
			if (mp_stream_read_at(s, data, type, 4) && memcmp(type, "INFO", 4) == 0) {
				int64_t list_end = data + len;
				if (list_end > end)
					list_end = end;
				riff_info(s, data + 4, list_end, info_slots);
			}
		} else if (memcmp(ch, "id3 ", 4) == 0 || memcmp(ch, "ID3 ", 4) == 0) {
			int64_t avail = end - data;
			if (avail > (int64_t)len)
				avail = len;
			id3v2_parse(s, data, avail, id3_slots);
		}
		off = data + (int64_t)len + (len & 1);
	}
}

/* ---- Entry points ------------------------------------------------------- */

/* How many tag sources there are; see parse_sources for their order. */
#define SOURCES 4

/*
 * Parses every tag source in the stream into all[0..SOURCES), in
 * precedence order: ID3v2 at the start, the format's own tags (FLAC
 * Vorbis comment and pictures, or the WAV "id3 " chunk), WAV INFO, ID3v1.
 * Returns NULL if out of memory. Free with free_sources.
 */
static Slots *parse_sources(MpStream *s, int want_picture)
{
	Slots *all;
	int64_t size, start = 0;
	uint8_t magic[4];
	int i;
	/* Slots are ~2 KB each; keep them off the (small, on XP) stack. */
	all = (Slots *)calloc(SOURCES, sizeof(Slots));
	if (all == NULL)
		return NULL;
	for (i = 0; i < SOURCES; i++)
		all[i].want_picture = want_picture;
	size = mp_stream_size(s);

	/* Some tools stack several ID3v2 tags; the first one is the real one,
	 * but we must skip them all to find the audio's own header. */
	for (i = 0; i < MAX_STACKED_ID3; i++) {
		int64_t len = id3v2_parse(s, start, size - start, &all[0]);
		if (len <= 0)
			break;
		start += len;
	}

	if (mp_stream_read_at(s, start, magic, 4) && memcmp(magic, "fLaC", 4) == 0) {
		flac_parse(s, start + 4, &all[1]);
	} else if (mp_stream_read_at(s, 0, magic, 4) && memcmp(magic, "RIFF", 4) == 0) {
		wav_parse(s, &all[1], &all[2]);
	} else {
		/* MP3 (or unknown): the only place ID3v1 is meaningful. */
		id3v1_parse(s, &all[3]);
	}
	return all;
}

static void free_sources(Slots *all)
{
	int i;
	if (all == NULL)
		return;
	for (i = 0; i < SOURCES; i++)
		free(all[i].pic);
	free(all);
}

int mp_tags_read_stream(MpStream *s, MpTags *tags)
{
	Slots *src;
	Slots *all;
	int i, n = SOURCES;

	tags->title[0] = 0;
	tags->artist[0] = 0;
	tags->track = 0;
	if (s == NULL)
		return 0;
	all = parse_sources(s, 0);
	if (all == NULL)
		return 0;

	for (i = 0; i < n; i++) {
		src = &all[i];
		if (tags->title[0] == 0 && src->title[0] != 0)
			mp_wcopy(tags->title, MP_TAG_MAX, src->title);
		if (tags->artist[0] == 0 && src->artist[0] != 0)
			mp_wcopy(tags->artist, MP_TAG_MAX, src->artist);
	}
	/* Album artist is only a fallback when no source has a track artist. */
	for (i = 0; i < n && tags->artist[0] == 0; i++) {
		if (all[i].album_artist[0] != 0)
			mp_wcopy(tags->artist, MP_TAG_MAX, all[i].album_artist);
	}
	for (i = 0; i < n && tags->track == 0; i++)
		tags->track = mp_tags_parse_track(all[i].track);
	free_sources(all);
	return tags->title[0] != 0 || tags->artist[0] != 0 || tags->track != 0;
}

int mp_tags_read_picture_stream(MpStream *s, uint8_t **data, size_t *size)
{
	Slots *all;
	int i, best = -1;
	*data = NULL;
	*size = 0;
	if (s == NULL)
		return 0;
	all = parse_sources(s, 1);
	if (all == NULL)
		return 0;
	/* The first source with a front cover; failing that, the first source
	 * with any picture at all. */
	for (i = 0; i < SOURCES && best < 0; i++) {
		if (all[i].pic != NULL && all[i].pic_front)
			best = i;
	}
	for (i = 0; i < SOURCES && best < 0; i++) {
		if (all[i].pic != NULL)
			best = i;
	}
	if (best >= 0) {
		/* Handed over rather than copied; free_sources then skips it. */
		*data = all[best].pic;
		*size = all[best].pic_size;
		all[best].pic = NULL;
	}
	free_sources(all);
	return *data != NULL;
}

int mp_tags_read_picture_file(const wchar_t *path, uint8_t **data, size_t *size)
{
	MpStream *s;
	int found;
	*data = NULL;
	*size = 0;
	s = mp_stream_open_file(path, NULL);
	if (s == NULL)
		return 0;
	found = mp_tags_read_picture_stream(s, data, size);
	mp_stream_close(s);
	return found;
}

int mp_tags_read_file(const wchar_t *path, MpTags *tags)
{
	MpStream *s;
	int found;
	tags->title[0] = 0;
	tags->artist[0] = 0;
	tags->track = 0;
	s = mp_stream_open_file(path, NULL);
	if (s == NULL)
		return 0;
	found = mp_tags_read_stream(s, tags);
	mp_stream_close(s);
	return found;
}

unsigned mp_tags_parse_track(const wchar_t *text)
{
	unsigned n = 0;
	int digits = 0;
	if (text == NULL)
		return 0;
	while (*text == L' ' || *text == L'\t')
		text++;
	for (; *text >= L'0' && *text <= L'9'; text++) {
		n = n * 10 + (unsigned)(*text - L'0');
		/* Stop before the value can overflow; it is garbage by now. */
		if (n > MP_TRACK_MAX)
			return 0;
		digits++;
	}
	if (digits == 0)
		return 0;
	while (*text == L' ' || *text == L'\t')
		text++;
	/* "7/12": whatever follows the slash is the track count, not ours to
	 * judge. Anything else after the number means it was not one. */
	if (*text != 0 && *text != L'/')
		return 0;
	return n;
}
