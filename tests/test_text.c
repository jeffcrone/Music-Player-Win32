/*
 * test_text.c - character-set conversion.
 */
#include "mptest.h"

#include "text.h"

#define U8(s) ((const uint8_t *)(s))

static void utf8_decoding(void)
{
	wchar_t out[64];

	CHECK_INT(mp_utf8_decode(U8("Hello"), 5, out, 64), 5);
	CHECK_WSTR(out, L"Hello");

	/* 2-, 3- and 4-byte sequences: e-acute, euro sign, U+1F3B5 MUSICAL NOTE
	 * (which needs a surrogate pair in UTF-16). */
	mp_utf8_decode(U8("Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5"), 14, out, 64);
	CHECK_WSTR(out, L"Caf\x00E9 \x20AC \xD83C\xDFB5");

	/* Stops at a NUL even if n says there is more. */
	mp_utf8_decode(U8("ab\0cd"), 5, out, 64);
	CHECK_WSTR(out, L"ab");

	/* n shorter than the string. */
	mp_utf8_decode(U8("abcdef"), 3, out, 64);
	CHECK_WSTR(out, L"abc");

	/* Malformed input: each bad byte becomes one U+FFFD and decoding goes
	 * on with the next byte. */
	mp_utf8_decode(U8("a\x80z"), 3, out, 64);          /* stray continuation */
	CHECK_WSTR(out, L"a\xFFFDz");
	mp_utf8_decode(U8("\xC0\xAF"), 2, out, 64);        /* overlong '/' */
	CHECK_WSTR(out, L"\xFFFD\xFFFD");
	mp_utf8_decode(U8("\xE0\x80\xAF"), 3, out, 64);    /* overlong 3-byte */
	CHECK_WSTR(out, L"\xFFFD\xFFFD\xFFFD");
	mp_utf8_decode(U8("\xED\xA0\x80"), 3, out, 64);    /* encoded surrogate */
	CHECK_WSTR(out, L"\xFFFD\xFFFD\xFFFD");
	mp_utf8_decode(U8("\xF4\x90\x80\x80"), 4, out, 64); /* above U+10FFFF */
	CHECK_WSTR(out, L"\xFFFD\xFFFD\xFFFD\xFFFD");
	mp_utf8_decode(U8("x\xE2\x82"), 3, out, 64);       /* truncated at the end */
	CHECK_WSTR(out, L"x\xFFFD\xFFFD");
	mp_utf8_decode(U8("\xFF"), 1, out, 64);
	CHECK_WSTR(out, L"\xFFFD");

	/* NULL input and zero-length output are harmless. */
	CHECK_INT(mp_utf8_decode(NULL, 5, out, 64), 0);
	CHECK_WSTR(out, L"");
	CHECK_INT(mp_utf8_decode(U8("abc"), 3, NULL, 0), 0);
}

static void utf8_truncation(void)
{
	wchar_t out[4];
	/* cap 4 = 3 characters + NUL. */
	CHECK_INT(mp_utf8_decode(U8("abcdef"), 6, out, 4), 3);
	CHECK_WSTR(out, L"abc");

	/* "ab" + a surrogate pair needs 4 units + NUL = 5; the pair must be
	 * dropped whole, never split. */
	CHECK_INT(mp_utf8_decode(U8("ab\xF0\x9F\x8E\xB5"), 6, out, 4), 2);
	CHECK_WSTR(out, L"ab");

	/* ...and nothing may sneak in after the dropped character. */
	CHECK_INT(mp_utf8_decode(U8("ab\xF0\x9F\x8E\xB5x"), 7, out, 4), 2);
	CHECK_WSTR(out, L"ab");

	/* cap 1 leaves room only for the terminator. */
	CHECK_INT(mp_utf8_decode(U8("abc"), 3, out, 1), 0);
	CHECK_WSTR(out, L"");
}

static void utf8_validation(void)
{
	CHECK(mp_utf8_is_valid(U8("plain ascii"), 11));
	CHECK(mp_utf8_is_valid(U8("Caf\xC3\xA9"), 5));
	CHECK(mp_utf8_is_valid(U8("\xF0\x9F\x8E\xB5"), 4));
	CHECK(mp_utf8_is_valid(U8(""), 0));
	CHECK(mp_utf8_is_valid(NULL, 0));
	/* Windows-1252 "Cafe" with e-acute is NOT valid UTF-8. */
	CHECK(!mp_utf8_is_valid(U8("Caf\xE9"), 4));
	CHECK(!mp_utf8_is_valid(U8("\xC0\xAF"), 2));
	CHECK(!mp_utf8_is_valid(U8("\xED\xA0\x80"), 3));
	/* Only the part before a NUL counts. */
	CHECK(mp_utf8_is_valid(U8("ok\0\xFF"), 4));
}

static void cp1252_decoding(void)
{
	wchar_t out[32];
	/* Plain Latin-1 range. */
	mp_cp1252_decode(U8("Caf\xE9 \xFC\xDF"), 7, out, 32);
	CHECK_WSTR(out, L"Caf\x00E9 \x00FC\x00DF");
	/* The 0x80-0x9F range is where 1252 differs from Latin-1: euro sign,
	 * curly quotes, en dash. */
	mp_cp1252_decode(U8("\x80\x93q\x94\x96"), 5, out, 32);
	CHECK_WSTR(out, L"\x20AC\x201Cq\x201D\x2013");
	/* The five undefined bytes pass through as C1 controls. */
	mp_cp1252_decode(U8("\x81\x8D\x8F\x90\x9D"), 5, out, 32);
	CHECK_WSTR(out, L"\x0081\x008D\x008F\x0090\x009D");
	/* Stops at NUL. */
	mp_cp1252_decode(U8("ab\0c"), 4, out, 32);
	CHECK_WSTR(out, L"ab");
}

static void utf16_decoding(void)
{
	wchar_t out[32];
	/* "Hi" + U+1F3B5 as a surrogate pair, both byte orders. */
	static const uint8_t le[] = { 'H', 0, 'i', 0, 0x3C, 0xD8, 0xB5, 0xDF };
	static const uint8_t be[] = { 0, 'H', 0, 'i', 0xD8, 0x3C, 0xDF, 0xB5 };
	CHECK_INT(mp_utf16_decode(le, sizeof(le), 0, out, 32), 4);
	CHECK_WSTR(out, L"Hi\xD83C\xDFB5");
	mp_utf16_decode(be, sizeof(be), 1, out, 32);
	CHECK_WSTR(out, L"Hi\xD83C\xDFB5");
	/* Odd trailing byte ignored. */
	mp_utf16_decode(le, 3, 0, out, 32);
	CHECK_WSTR(out, L"H");
	/* Stops at a 16-bit NUL. */
	{
		static const uint8_t z[] = { 'a', 0, 0, 0, 'b', 0 };
		mp_utf16_decode(z, sizeof(z), 0, out, 32);
		CHECK_WSTR(out, L"a");
	}
	/* Unpaired surrogates become U+FFFD. */
	{
		static const uint8_t lone_hi[] = { 0x3C, 0xD8, 'x', 0 };
		static const uint8_t lone_lo[] = { 0xB5, 0xDF, 'x', 0 };
		static const uint8_t hi_at_end[] = { 'x', 0, 0x3C, 0xD8 };
		mp_utf16_decode(lone_hi, sizeof(lone_hi), 0, out, 32);
		CHECK_WSTR(out, L"\xFFFDx");
		mp_utf16_decode(lone_lo, sizeof(lone_lo), 0, out, 32);
		CHECK_WSTR(out, L"\xFFFDx");
		mp_utf16_decode(hi_at_end, sizeof(hi_at_end), 0, out, 32);
		CHECK_WSTR(out, L"x\xFFFD");
	}
}

static void utf8_encoding(void)
{
	char out[32];
	CHECK_INT(mp_utf8_encode(L"abc", out, 32), 3);
	CHECK(strcmp(out, "abc") == 0);
	CHECK_INT(mp_utf8_encode(L"Caf\x00E9 \x20AC \xD83C\xDFB5", out, 32), 14);
	CHECK(strcmp(out, "Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5") == 0);
	/* Measuring mode. */
	CHECK_INT(mp_utf8_encode(L"\x20AC", NULL, 0), 3);
	/* Truncation never splits a character, but still reports the full length. */
	CHECK_INT(mp_utf8_encode(L"a\x20AC", out, 3), 4);
	CHECK(strcmp(out, "a") == 0);
	/* Lone surrogates are encoded as U+FFFD. */
	mp_utf8_encode(L"\xD83Cx", out, 32);
	CHECK(strcmp(out, "\xEF\xBF\xBDx") == 0);
	/* Round trip through the decoder. */
	{
		wchar_t back[32];
		mp_utf8_encode(L"\x00C5ngstr\x00F6m \xD83C\xDFB5", out, 32);
		mp_utf8_decode((const uint8_t *)out, strlen(out), back, 32);
		CHECK_WSTR(back, L"\x00C5ngstr\x00F6m \xD83C\xDFB5");
	}
}

static void trimming(void)
{
	wchar_t s[64];
	wcscpy(s, L"  padded  ");
	CHECK_INT(mp_trim(s), 6);
	CHECK_WSTR(s, L"padded");
	wcscpy(s, L"\t\r\n x \x00A0\xFEFF");
	mp_trim(s);
	CHECK_WSTR(s, L"x");
	/* Inner control characters become spaces; inner spaces stay. */
	wcscpy(s, L"a\tb\nc  d");
	mp_trim(s);
	CHECK_WSTR(s, L"a b c  d");
	wcscpy(s, L"   ");
	CHECK_INT(mp_trim(s), 0);
	CHECK_WSTR(s, L"");
	wcscpy(s, L"");
	CHECK_INT(mp_trim(s), 0);
	CHECK_INT(mp_trim(NULL), 0);
}

static void copying(void)
{
	wchar_t s[4];
	CHECK_INT(mp_wcopy(s, 4, L"abcdef"), 3);
	CHECK_WSTR(s, L"abc");
	CHECK_INT(mp_wcopy(s, 4, L"ab"), 2);
	CHECK_WSTR(s, L"ab");
	/* Never leaves half a surrogate pair at the cut. */
	CHECK_INT(mp_wcopy(s, 4, L"ab\xD83C\xDFB5"), 2);
	CHECK_WSTR(s, L"ab");
	CHECK_INT(mp_wcopy(s, 4, NULL), 0);
	CHECK_WSTR(s, L"");
	CHECK_INT(mp_wcopy(NULL, 0, L"x"), 0);
}

static void ascii_compare(void)
{
	CHECK(mp_ascii_ieq("TITLE", "title"));
	CHECK(mp_ascii_ieq("Artist", "ARTIST"));
	CHECK(!mp_ascii_ieq("ARTIST", "ARTISTS"));
	CHECK(mp_ascii_ieq_n("ARTISTS", "artist", 6));
	CHECK(mp_wascii_ieq(L".MP3", L".mp3"));
	CHECK(!mp_wascii_ieq(L".mp3", L".mp4"));
	/* Non-ASCII letters are compared exactly, never case-folded. */
	CHECK(!mp_wascii_ieq(L"\x00C9", L"\x00E9"));
	CHECK(mp_wascii_ieq_n(L"file:///x", L"FILE:", 5));
}

int suite_text(void)
{
	utf8_decoding();
	utf8_truncation();
	utf8_validation();
	cp1252_decoding();
	utf16_decoding();
	utf8_encoding();
	trimming();
	copying();
	ascii_compare();
	return 0;
}
