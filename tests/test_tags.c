/*
 * test_tags.c - title/artist extraction from ID3v2, ID3v1, FLAC and WAV.
 */
#include <stdlib.h>
#include <windows.h>

#include "builders.h"
#include "mptest.h"
#include "tags.h"

/* Reads tags from an in-memory file. Returns what mp_tags_read_stream did. */
static int read_tags(const Buf *b, MpTags *t)
{
	MpStream *s = mp_stream_open_memory(b->data, b->len);
	int found = mp_tags_read_stream(s, t);
	mp_stream_close(s);
	return found;
}

/* A tag followed by a few MP3 frames, like a real tagged MP3. */
static void mp3_with_tag(Buf *out, int major, uint8_t flags, const Buf *frames)
{
	id3_tag(out, major, flags, frames, 64);
	mp3_silence(out, 3);
}

/* ---- ID3v2 basics ------------------------------------------------------- */

static void id3v23_latin1(void)
{
	Buf f, file;
	MpTags t;
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "Caf\xE9 Song", 9);
	id3_frame(&f, 3, "TPE1", 0, 0, "The Band", 8);
	mp3_with_tag(&file, 3, 0, &f);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"Caf\x00E9 Song");
	CHECK_WSTR(t.artist, L"The Band");
	buf_free(&f);
	buf_free(&file);
}

static void id3v24_utf8(void)
{
	Buf f, file;
	MpTags t;
	buf_init(&f);
	buf_init(&file);
	/* Japanese title, and a stray UTF-8 BOM some tools write. */
	id3_frame(&f, 4, "TIT2", 0, 3, "\xE6\x9D\xB1\xE4\xBA\xAC", 6);
	id3_frame(&f, 4, "TPE1", 0, 3, "\xEF\xBB\xBF" "Bj\xC3\xB6rk", 9);
	mp3_with_tag(&file, 4, 0, &f);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"\x6771\x4EAC");
	CHECK_WSTR(t.artist, L"Bj\x00F6rk");
	buf_free(&f);
	buf_free(&file);
}

static void id3_utf16_variants(void)
{
	static const uint8_t le_bom[] = { 0xFF, 0xFE, 'L', 0, 'E', 0 };
	static const uint8_t be_bom[] = { 0xFE, 0xFF, 0, 'B', 0, 'E' };
	static const uint8_t no_bom[] = { 'N', 0, 'o', 0 };
	static const uint8_t be_plain[] = { 0, 'X', 0, 0xE9 };
	Buf f, file;
	MpTags t;

	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 1, le_bom, sizeof(le_bom));
	id3_frame(&f, 3, "TPE1", 0, 1, be_bom, sizeof(be_bom));
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"LE");
	CHECK_WSTR(t.artist, L"BE");
	buf_free(&f);
	buf_free(&file);

	/* Encoding 1 without the required BOM: assume little-endian. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 1, no_bom, sizeof(no_bom));
	/* Encoding 2 is UTF-16BE without a BOM (v2.4 only). */
	id3_frame(&f, 3, "TPE1", 0, 2, be_plain, sizeof(be_plain));
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"No");
	CHECK_WSTR(t.artist, L"X\x00E9");
	buf_free(&f);
	buf_free(&file);
}

static void id3v22(void)
{
	Buf f, file;
	MpTags t;
	buf_init(&f);
	buf_init(&file);
	/* v2.2 has 3-letter IDs and 6-byte frame headers. */
	id3_frame(&f, 2, "TT2", 0, 0, "Old Title", 9);
	id3_frame(&f, 2, "TP1", 0, 0, "Old Artist", 10);
	mp3_with_tag(&file, 2, 0, &f);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"Old Title");
	CHECK_WSTR(t.artist, L"Old Artist");
	buf_free(&f);
	buf_free(&file);

	/* v2.2 with the "compression" flag has no defined format: skipped, and
	 * the file still counts as having no tags. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 2, "TT2", 0, 0, "Nope", 4);
	mp3_with_tag(&file, 2, 0x40, &f);
	CHECK(!read_tags(&file, &t));
	CHECK_WSTR(t.title, L"");
	buf_free(&f);
	buf_free(&file);
}

static void album_artist_fallback(void)
{
	Buf f, file;
	MpTags t;

	/* Only TPE2 (album artist): used as the artist. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "T", 1);
	id3_frame(&f, 3, "TPE2", 0, 0, "Album Artist", 12);
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"Album Artist");
	buf_free(&f);
	buf_free(&file);

	/* Both: the track artist wins, whatever the frame order. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TPE2", 0, 0, "Album Artist", 12);
	id3_frame(&f, 3, "TPE1", 0, 0, "Track Artist", 12);
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"Track Artist");
	buf_free(&f);
	buf_free(&file);
}

static void partial_and_missing(void)
{
	Buf f, file;
	MpTags t;

	/* Artist but no title: the title stays empty (the display code then
	 * falls back to the file name). */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TPE1", 0, 0, "Just Artist", 11);
	mp3_with_tag(&file, 3, 0, &f);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"");
	CHECK_WSTR(t.artist, L"Just Artist");
	buf_free(&f);
	buf_free(&file);

	/* Title but no artist. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "Just Title", 10);
	mp3_with_tag(&file, 3, 0, &f);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"Just Title");
	CHECK_WSTR(t.artist, L"");
	buf_free(&f);
	buf_free(&file);

	/* An untagged MP3. */
	buf_init(&file);
	mp3_silence(&file, 3);
	CHECK(!read_tags(&file, &t));
	CHECK_WSTR(t.title, L"");
	CHECK_WSTR(t.artist, L"");
	buf_free(&file);

	/* An empty file, and a file shorter than any header. */
	buf_init(&file);
	CHECK(!read_tags(&file, &t));
	buf_str(&file, "ID");
	CHECK(!read_tags(&file, &t));
	buf_free(&file);

	/* Whitespace-only and empty frames count as missing. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "   ", 3);
	id3_frame(&f, 3, "TPE1", 0, 1, "", 0);
	mp3_with_tag(&file, 3, 0, &f);
	CHECK(!read_tags(&file, &t));
	buf_free(&f);
	buf_free(&file);
}

static void values_are_cleaned(void)
{
	Buf f, file;
	MpTags t;
	char longtitle[600];
	wchar_t expect[MP_TAG_MAX];
	int i;

	buf_init(&f);
	buf_init(&file);
	/* v2.4 multiple values are NUL-separated: keep the first. Padding
	 * spaces and a trailing NUL are trimmed. */
	id3_frame(&f, 4, "TPE1", 0, 3, "  First\0Second\0", 15);
	for (i = 0; i < 600; i++)
		longtitle[i] = (char)('a' + i % 26);
	id3_frame(&f, 4, "TIT2", 0, 0, longtitle, 600);
	mp3_with_tag(&file, 4, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"First");
	/* Truncated to the buffer: MP_TAG_MAX - 1 characters. */
	for (i = 0; i < MP_TAG_MAX - 1; i++)
		expect[i] = (wchar_t)('a' + i % 26);
	expect[MP_TAG_MAX - 1] = 0;
	CHECK_WSTR(t.title, expect);
	buf_free(&f);
	buf_free(&file);
}

/* ---- ID3v2 structure: headers, flags, unsynchronization ----------------- */

static void cover_art_is_skipped(void)
{
	Buf f, file;
	MpTags t;
	uint8_t *art = (uint8_t *)malloc(300000);
	memset(art, 0xFF, 300000); /* 0xFF bytes look like MP3 sync - must not confuse us */
	buf_init(&f);
	buf_init(&file);
	id3_raw_frame(&f, 3, "APIC", 0, art, 300000);
	id3_frame(&f, 3, "TIT2", 0, 0, "After Art", 9);
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"After Art");
	buf_free(&f);
	buf_free(&file);
	free(art);
}

static void extended_headers(void)
{
	Buf f, body, file;
	MpTags t;

	/* v2.3: 4-byte size (6 or 10), not counting itself, then 6 bytes. */
	buf_init(&f);
	buf_init(&body);
	buf_init(&file);
	buf_be32(&body, 6);
	buf_zeros(&body, 6);
	id3_frame(&f, 3, "TIT2", 0, 0, "Ext23", 5);
	buf_bytes(&body, f.data, f.len);
	mp3_with_tag(&file, 3, 0x40, &body);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Ext23");
	buf_free(&f);
	buf_free(&body);
	buf_free(&file);

	/* v2.4: syncsafe size that includes itself (here 6 bytes total). */
	buf_init(&f);
	buf_init(&body);
	buf_init(&file);
	buf_syncsafe(&body, 6);
	buf_u8(&body, 1);
	buf_u8(&body, 0);
	id3_frame(&f, 4, "TIT2", 0, 0, "Ext24", 5);
	buf_bytes(&body, f.data, f.len);
	mp3_with_tag(&file, 4, 0x40, &body);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Ext24");
	buf_free(&f);
	buf_free(&body);
	buf_free(&file);

	/* An extended header size that runs past the tag: give up gracefully. */
	buf_init(&body);
	buf_init(&file);
	buf_be32(&body, 5000);
	buf_zeros(&body, 20);
	mp3_with_tag(&file, 3, 0x40, &body);
	CHECK(!read_tags(&file, &t));
	buf_free(&body);
	buf_free(&file);
}

/* Applies ID3 unsynchronization: a 0x00 after every 0xFF. */
static void unsync(const uint8_t *in, size_t n, Buf *out)
{
	size_t i;
	for (i = 0; i < n; i++) {
		buf_u8(out, in[i]);
		if (in[i] == 0xFF)
			buf_u8(out, 0x00);
	}
}

static void tag_level_unsync_v23(void)
{
	Buf f, body, file;
	MpTags t;
	buf_init(&f);
	buf_init(&body);
	buf_init(&file);
	/* 0xFF is y-diaeresis in 1252. The title's FF bytes gain inserted
	 * zeros, which shifts the TPE1 frame: reading it only works if the
	 * whole tag is de-unsynchronized before frames are located. */
	id3_frame(&f, 3, "TIT2", 0, 0, "\xFF\xFFy", 3);
	id3_frame(&f, 3, "TPE1", 0, 0, "After", 5);
	unsync(f.data, f.len, &body);
	mp3_with_tag(&file, 3, 0x80, &body);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"\x00FF\x00FFy");
	CHECK_WSTR(t.artist, L"After");
	buf_free(&f);
	buf_free(&body);
	buf_free(&file);
}

static void frame_flags_v24(void)
{
	Buf f, content, raw, file;
	MpTags t;
	static const uint8_t text[] = { 0x00, 0xFF, 'Z' }; /* encoding 0, then y-diaeresis, Z */

	/* Frame-level unsync (0x0002) plus a data length indicator (0x0001):
	 * content = 4-byte syncsafe original length + unsynchronized data. */
	buf_init(&f);
	buf_init(&content);
	buf_init(&raw);
	buf_init(&file);
	buf_syncsafe(&content, sizeof(text));
	unsync(text, sizeof(text), &content);
	id3_raw_frame(&f, 4, "TIT2", 0x0003, content.data, content.len);
	/* Grouping (0x0040): one group-ID byte before the content. */
	buf_u8(&raw, 0x42);
	buf_u8(&raw, 0);
	buf_str(&raw, "Grouped");
	id3_raw_frame(&f, 4, "TPE1", 0x0040, raw.data, raw.len);
	mp3_with_tag(&file, 4, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"\x00FFZ");
	CHECK_WSTR(t.artist, L"Grouped");
	buf_free(&f);
	buf_free(&content);
	buf_free(&raw);
	buf_free(&file);

	/* v2.4 tag-level unsync flag applies to every frame. */
	buf_init(&f);
	buf_init(&content);
	buf_init(&file);
	unsync(text, sizeof(text), &content);
	id3_raw_frame(&f, 4, "TIT2", 0, content.data, content.len);
	mp3_with_tag(&file, 4, 0x80, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"\x00FFZ");
	buf_free(&f);
	buf_free(&content);
	buf_free(&file);
}

static void compressed_and_encrypted_frames(void)
{
	Buf f, file;
	MpTags t;

	/* A compressed (v2.3 0x0080) TIT2 is skipped, so a later plain TIT2 is
	 * the one used. Encrypted (0x0040) likewise. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0x0080, 0, "\x78\x9C garbage", 10);
	id3_frame(&f, 3, "TPE1", 0x0040, 0, "secret", 6);
	id3_frame(&f, 3, "TIT2", 0, 0, "Plain", 5);
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Plain");
	CHECK_WSTR(t.artist, L"");
	buf_free(&f);
	buf_free(&file);

	/* v2.4: compression is 0x0008, encryption 0x0004. v2.3 grouping 0x0020. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 4, "TIT2", 0x0008, 0, "zz", 2);
	id3_frame(&f, 4, "TPE1", 0x0004, 0, "zz", 2);
	mp3_with_tag(&file, 4, 0, &f);
	CHECK(!read_tags(&file, &t));
	buf_free(&f);
	buf_free(&file);

	buf_init(&f);
	buf_init(&file);
	{
		static const uint8_t grouped[] = { 0x07, 0x00, 'G', '3' };
		id3_raw_frame(&f, 3, "TIT2", 0x0020, grouped, sizeof(grouped));
	}
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"G3");
	buf_free(&f);
	buf_free(&file);
}

static void itunes_nonsyncsafe_v24_sizes(void)
{
	Buf f, file;
	MpTags t;
	uint8_t content[200];
	/* Old iTunes wrote plain 32-bit sizes in v2.4 tags. 200 = 0x000000C8;
	 * 0xC8 has its high bit set so it cannot be syncsafe. Read as syncsafe
	 * it would be 0x48 = 72 and the next frame would be lost. */
	memset(content, 0, sizeof(content));
	memcpy(content + 1, "iTunes Title", 12);
	buf_init(&f);
	buf_init(&file);
	buf_str(&f, "TIT2");
	buf_be32(&f, sizeof(content));
	buf_be16(&f, 0);
	buf_bytes(&f, content, sizeof(content));
	id3_frame(&f, 4, "TPE1", 0, 0, "Next Frame", 10);
	mp3_with_tag(&file, 4, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"iTunes Title");
	CHECK_WSTR(t.artist, L"Next Frame");
	buf_free(&f);
	buf_free(&file);
}

static void corrupt_id3v2(void)
{
	Buf f, file;
	MpTags t;
	size_t cut;

	/* The tag claims 10 KB but the file ends early: read what is there. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "Truncated", 9);
	id3_tag(&file, 3, 0, &f, 10000);
	cut = file.len - 9000;
	file.len = cut;
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Truncated");
	buf_free(&f);
	buf_free(&file);

	/* A frame that claims to be longer than the tag ends the walk, but
	 * earlier frames are kept. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "Kept", 4);
	buf_str(&f, "TPE1");
	buf_be32(&f, 0x7FFFFFF0);
	buf_be16(&f, 0);
	buf_str(&f, "xx");
	mp3_with_tag(&file, 3, 0, &f);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Kept");
	CHECK_WSTR(t.artist, L"");
	buf_free(&f);
	buf_free(&file);

	/* A garbage frame ID stops parsing rather than guessing. */
	buf_init(&f);
	buf_init(&file);
	buf_str(&f, "t!t2");
	buf_be32(&f, 4);
	buf_be16(&f, 0);
	buf_str(&f, "\0bad");
	id3_frame(&f, 3, "TIT2", 0, 0, "Unreached", 9);
	mp3_with_tag(&file, 3, 0, &f);
	CHECK(!read_tags(&file, &t));
	buf_free(&f);
	buf_free(&file);

	/* A header whose size bytes are not syncsafe is not an ID3 tag. */
	buf_init(&file);
	buf_str(&file, "ID3");
	buf_u8(&file, 3);
	buf_u8(&file, 0);
	buf_u8(&file, 0);
	buf_be32(&file, 0x80808080);
	buf_zeros(&file, 100);
	CHECK(!read_tags(&file, &t));
	buf_free(&file);

	/* An unknown major version (2.5) is skipped cleanly; the ID3v1 tag at
	 * the end of the file is still found. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 4, "TIT2", 0, 0, "Future", 6);
	id3_tag(&file, 5, 0, &f, 0);
	mp3_silence(&file, 2);
	id3v1_tag(&file, "V1 Title", "V1 Artist");
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"V1 Title");
	buf_free(&f);
	buf_free(&file);
}

static void v24_footer_and_stacked_tags(void)
{
	Buf f1, f2, file;
	MpTags t;
	buf_init(&f1);
	buf_init(&f2);
	buf_init(&file);
	/* A v2.4 tag with a footer (flag 0x10): the footer's 10 bytes must be
	 * skipped too. Then a second tag (some tools stack them): its fields
	 * only fill what the first lacked. */
	id3_frame(&f1, 4, "TIT2", 0, 0, "First Tag", 9);
	id3_tag(&file, 4, 0x10, &f1, 0);
	buf_str(&file, "3DI");
	buf_zeros(&file, 7);
	id3_frame(&f2, 3, "TIT2", 0, 0, "Second Tag", 10);
	id3_frame(&f2, 3, "TPE1", 0, 0, "Second Artist", 13);
	id3_tag(&file, 3, 0, &f2, 0);
	mp3_silence(&file, 2);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"First Tag");
	CHECK_WSTR(t.artist, L"Second Artist");
	buf_free(&f1);
	buf_free(&f2);
	buf_free(&file);
}

/* ---- ID3v1 -------------------------------------------------------------- */

static void id3v1(void)
{
	Buf f, file;
	MpTags t;

	/* v1 only, with space padding (some tools) and a 1252 character. */
	buf_init(&file);
	mp3_silence(&file, 2);
	id3v1_tag(&file, "Space Padded      ", "Beyonc\xE9");
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"Space Padded");
	CHECK_WSTR(t.artist, L"Beyonc\x00E9");
	buf_free(&file);

	/* A full 30-character field has no terminator at all. */
	buf_init(&file);
	mp3_silence(&file, 1);
	id3v1_tag(&file, "123456789012345678901234567890", "A");
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"123456789012345678901234567890");
	buf_free(&file);

	/* v2 wins over v1; v1 fills in a field v2 lacks. */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "V2 Title", 8);
	id3_tag(&file, 3, 0, &f, 0);
	mp3_silence(&file, 2);
	id3v1_tag(&file, "V1 Title", "V1 Artist");
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"V2 Title");
	CHECK_WSTR(t.artist, L"V1 Artist");
	buf_free(&f);
	buf_free(&file);

	/* A v1 track artist beats a v2 album artist (the fallback is only for
	 * when no source has a track artist at all). */
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TPE2", 0, 0, "V2 Album Artist", 15);
	id3_tag(&file, 3, 0, &f, 0);
	mp3_silence(&file, 2);
	id3v1_tag(&file, "", "V1 Artist");
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"V1 Artist");
	buf_free(&f);
	buf_free(&file);
}

/* ---- FLAC --------------------------------------------------------------- */

static void flac_with_comment(Buf *file, const char *const *fields, size_t count)
{
	Buf vc;
	static const int16_t pcm[8] = { 0 };
	buf_init(&vc);
	vorbis_comment(&vc, "test vendor", fields, count);
	flac_file(file, 44100, 2, pcm, 4, 4096, &vc);
	buf_free(&vc);
}

static void flac_vorbis_comments(void)
{
	Buf file;
	MpTags t;
	static const char *const mixed_case[] = { "tItLe=Flac \xC3\x89t\xC3\xA9", "Artist=FLAC Artist", "GENRE=x" };
	static const char *const album_only[] = { "TITLE=T", "ALBUMARTIST=AA" };
	static const char *const album_space[] = { "ALBUM ARTIST=Spaced" };
	static const char *const odd_fields[] = { "NOEQUALS", "=novalue", "TITLE=", "TITLE=Second Try" };

	buf_init(&file);
	flac_with_comment(&file, mixed_case, 3);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"Flac \x00C9t\x00E9");
	CHECK_WSTR(t.artist, L"FLAC Artist");
	buf_free(&file);

	buf_init(&file);
	flac_with_comment(&file, album_only, 2);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"AA");
	buf_free(&file);

	buf_init(&file);
	flac_with_comment(&file, album_space, 1);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"Spaced");
	CHECK_WSTR(t.title, L"");
	buf_free(&file);

	/* Fields without '=' are ignored; an empty TITLE does not block a
	 * later non-empty one. */
	buf_init(&file);
	flac_with_comment(&file, odd_fields, 4);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Second Try");
	buf_free(&file);
}

static void flac_edge_cases(void)
{
	Buf file, vc, id3f;
	MpTags t;
	static const int16_t pcm[8] = { 0 };
	static const char *const fields[] = { "TITLE=Vorbis Title", "ARTIST=Vorbis Artist" };

	/* No comment block at all. */
	buf_init(&file);
	flac_file(&file, 44100, 2, pcm, 4, 4096, NULL);
	CHECK(!read_tags(&file, &t));
	buf_free(&file);

	/* A comment count far larger than the block: stop at the end safely. */
	buf_init(&vc);
	buf_init(&file);
	vorbis_comment(&vc, "v", fields, 2);
	vc.data[5] = 0xFF; /* count's low byte (after 4-byte length + "v") */
	vc.data[6] = 0xFF;
	flac_file(&file, 44100, 2, pcm, 4, 4096, &vc);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Vorbis Title");
	buf_free(&vc);
	buf_free(&file);

	/* An ID3v2 tag in front of a FLAC file: its title wins, but the
	 * Vorbis comment still supplies the artist it lacks. */
	buf_init(&vc);
	buf_init(&id3f);
	buf_init(&file);
	id3_frame(&id3f, 3, "TIT2", 0, 0, "ID3 Title", 9);
	id3_tag(&file, 3, 0, &id3f, 0);
	vorbis_comment(&vc, "v", fields, 2);
	flac_file(&file, 44100, 2, pcm, 4, 4096, &vc);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"ID3 Title");
	CHECK_WSTR(t.artist, L"Vorbis Artist");
	buf_free(&vc);
	buf_free(&id3f);
	buf_free(&file);

	/* A PADDING block before the comment, built by hand. */
	buf_init(&vc);
	buf_init(&file);
	vorbis_comment(&vc, "v", fields, 2);
	buf_str(&file, "fLaC");
	buf_u8(&file, 0); /* STREAMINFO, not last */
	buf_be24(&file, 34);
	buf_zeros(&file, 34);
	buf_u8(&file, 1); /* PADDING, not last */
	buf_be24(&file, 100);
	buf_zeros(&file, 100);
	buf_u8(&file, 0x84); /* VORBIS_COMMENT, last */
	buf_be24(&file, (uint32_t)vc.len);
	buf_bytes(&file, vc.data, vc.len);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"Vorbis Artist");
	buf_free(&vc);
	buf_free(&file);

	/* A trailing "TAG" block is NOT treated as ID3v1 in a FLAC file. */
	buf_init(&file);
	flac_file(&file, 44100, 2, pcm, 4, 4096, NULL);
	id3v1_tag(&file, "Not For FLAC", "Nope");
	CHECK(!read_tags(&file, &t));
	buf_free(&file);
}

/* ---- WAV ---------------------------------------------------------------- */

static void info_list(Buf *out, const char *inam, size_t inam_len, const char *iart, size_t iart_len)
{
	Buf list;
	buf_init(&list);
	buf_str(&list, "INFO");
	if (inam)
		riff_chunk(&list, "INAM", inam, inam_len);
	if (iart)
		riff_chunk(&list, "IART", iart, iart_len);
	riff_chunk(out, "LIST", list.data, list.len);
	buf_free(&list);
}

static void wav_info(void)
{
	Buf extra, file;
	MpTags t;
	static const int16_t pcm[4] = { 0 };

	/* NUL-terminated ASCII, the classic form. The odd length (8 bytes +
	 * NUL = 9) exercises the pad byte before IART. */
	buf_init(&extra);
	buf_init(&file);
	info_list(&extra, "WAV Name", 9, "WAV Art", 8);
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	CHECK(read_tags(&file, &t));
	CHECK_WSTR(t.title, L"WAV Name");
	CHECK_WSTR(t.artist, L"WAV Art");
	buf_free(&extra);
	buf_free(&file);

	/* UTF-8 is recognized; bytes that are not valid UTF-8 are read as 1252. */
	buf_init(&extra);
	buf_init(&file);
	info_list(&extra, "Gr\xC3\xBC\xC3\x9F" "e", 7, "Caf\xE9", 4);
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Gr\x00FC\x00DF" L"e");
	CHECK_WSTR(t.artist, L"Caf\x00E9");
	buf_free(&extra);
	buf_free(&file);

	/* A LIST that is not INFO (e.g. "adtl" cue labels) is ignored. */
	buf_init(&extra);
	buf_init(&file);
	{
		Buf list;
		buf_init(&list);
		buf_str(&list, "adtl");
		riff_chunk(&list, "INAM", "Not A Title", 11);
		riff_chunk(&extra, "LIST", list.data, list.len);
		buf_free(&list);
	}
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	CHECK(!read_tags(&file, &t));
	buf_free(&extra);
	buf_free(&file);
}

static void wav_id3_chunk(void)
{
	Buf extra, frames, tag, file;
	MpTags t;
	static const int16_t pcm[4] = { 0 };

	/* Both an "id3 " chunk and INFO: the ID3 values win, INFO fills gaps. */
	buf_init(&extra);
	buf_init(&frames);
	buf_init(&tag);
	buf_init(&file);
	id3_frame(&frames, 3, "TIT2", 0, 0, "ID3 In WAV", 10);
	id3_tag(&tag, 3, 0, &frames, 0);
	info_list(&extra, "Info Title", 11, "Info Artist", 12);
	riff_chunk(&extra, "id3 ", tag.data, tag.len);
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"ID3 In WAV");
	CHECK_WSTR(t.artist, L"Info Artist");
	buf_free(&extra);
	buf_free(&frames);
	buf_free(&tag);
	buf_free(&file);

	/* Upper-case "ID3 " is used by some tools too. */
	buf_init(&extra);
	buf_init(&frames);
	buf_init(&tag);
	buf_init(&file);
	id3_frame(&frames, 4, "TPE1", 0, 3, "Upper", 5);
	id3_tag(&tag, 4, 0, &frames, 0);
	riff_chunk(&extra, "ID3 ", tag.data, tag.len);
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	read_tags(&file, &t);
	CHECK_WSTR(t.artist, L"Upper");
	buf_free(&extra);
	buf_free(&frames);
	buf_free(&tag);
	buf_free(&file);
}

static void wav_corrupt(void)
{
	Buf extra, file;
	MpTags t;
	static const int16_t pcm[4] = { 0 };

	/* RIFF size larger than the file (a crashed recorder). */
	buf_init(&extra);
	buf_init(&file);
	info_list(&extra, "Still Found", 12, NULL, 0);
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	file.data[4] = 0xF0;
	file.data[5] = 0xFF;
	file.data[6] = 0xFF;
	file.data[7] = 0x7F;
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Still Found");
	buf_free(&extra);
	buf_free(&file);

	/* A chunk with an absurd size ends the walk without crashing. */
	buf_init(&extra);
	buf_init(&file);
	buf_str(&extra, "junk");
	buf_le32(&extra, 0xFFFFFFF0);
	buf_zeros(&extra, 8);
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	CHECK(!read_tags(&file, &t));
	buf_free(&extra);
	buf_free(&file);

	/* An INFO subchunk claiming more bytes than the list holds. */
	buf_init(&extra);
	buf_init(&file);
	{
		Buf list;
		buf_init(&list);
		buf_str(&list, "INFO");
		buf_str(&list, "INAM");
		buf_le32(&list, 1000);
		buf_str(&list, "Short");
		riff_chunk(&extra, "LIST", list.data, list.len);
		buf_free(&list);
	}
	wav_file(&file, 1, 2, 44100, 16, pcm, sizeof(pcm), &extra);
	read_tags(&file, &t);
	CHECK_WSTR(t.title, L"Short");
	buf_free(&extra);
	buf_free(&file);
}

/* ---- Real files --------------------------------------------------------- */

static void from_disk(void)
{
	Buf f, file;
	MpTags t;
	wchar_t path[MAX_PATH];
	buf_init(&f);
	buf_init(&file);
	id3_frame(&f, 3, "TIT2", 0, 0, "On Disk", 7);
	mp3_with_tag(&file, 3, 0, &f);
	CHECK(temp_file(&file, L".mp3", path, MAX_PATH));
	CHECK(mp_tags_read_file(path, &t));
	CHECK_WSTR(t.title, L"On Disk");
	DeleteFileW(path);
	/* Missing file: no tags, empty strings, no crash. */
	CHECK(!mp_tags_read_file(path, &t));
	CHECK_WSTR(t.title, L"");
	CHECK_WSTR(t.artist, L"");
	buf_free(&f);
	buf_free(&file);
}

int suite_tags(void)
{
	id3v23_latin1();
	id3v24_utf8();
	id3_utf16_variants();
	id3v22();
	album_artist_fallback();
	partial_and_missing();
	values_are_cleaned();
	cover_art_is_skipped();
	extended_headers();
	tag_level_unsync_v23();
	frame_flags_v24();
	compressed_and_encrypted_frames();
	itunes_nonsyncsafe_v24_sizes();
	corrupt_id3v2();
	v24_footer_and_stacked_tags();
	id3v1();
	flac_vorbis_comments();
	flac_edge_cases();
	wav_info();
	wav_id3_chunk();
	wav_corrupt();
	from_disk();
	return 0;
}
