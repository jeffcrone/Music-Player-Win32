/*
 * test_track.c - what the player shows for a track, and time formatting.
 */
#include "mptest.h"

#include "text.h"
#include "track.h"

static void set_tags(MpTags *t, const wchar_t *title, const wchar_t *artist)
{
	mp_wcopy(t->title, MP_TAG_MAX, title);
	mp_wcopy(t->artist, MP_TAG_MAX, artist);
}

static void basenames(void)
{
	CHECK_WSTR(mp_path_basename(L"C:\\Music\\Song.mp3"), L"Song.mp3");
	CHECK_WSTR(mp_path_basename(L"C:/Music/Song.flac"), L"Song.flac");
	CHECK_WSTR(mp_path_basename(L"\\\\server\\share\\a.wav"), L"a.wav");
	CHECK_WSTR(mp_path_basename(L"C:song.mp3"), L"song.mp3");
	CHECK_WSTR(mp_path_basename(L"bare.mp3"), L"bare.mp3");
	CHECK_WSTR(mp_path_basename(L"C:\\Music\\"), L"");
	CHECK_WSTR(mp_path_basename(L""), L"");
	CHECK_WSTR(mp_path_basename(NULL), L"");
}

static void display_rules(void)
{
	MpTags t;
	wchar_t title[MP_TAG_MAX], artist[MP_TAG_MAX];

	/* Both present: shown as-is. */
	set_tags(&t, L"Song Title", L"Artist Name");
	mp_track_display(&t, L"C:\\Music\\file.mp3", title, MP_TAG_MAX, artist, MP_TAG_MAX);
	CHECK_WSTR(title, L"Song Title");
	CHECK_WSTR(artist, L"Artist Name");

	/* No title: the file name, extension and all. */
	set_tags(&t, L"", L"Artist Name");
	mp_track_display(&t, L"C:\\Music\\01 - Track.flac", title, MP_TAG_MAX, artist, MP_TAG_MAX);
	CHECK_WSTR(title, L"01 - Track.flac");
	CHECK_WSTR(artist, L"Artist Name");

	/* No artist: left out (empty). */
	set_tags(&t, L"Song Title", L"");
	mp_track_display(&t, L"C:\\Music\\file.mp3", title, MP_TAG_MAX, artist, MP_TAG_MAX);
	CHECK_WSTR(title, L"Song Title");
	CHECK_WSTR(artist, L"");

	/* No metadata at all. */
	mp_track_display(NULL, L"D:\\x\\y\\recording.wav", title, MP_TAG_MAX, artist, MP_TAG_MAX);
	CHECK_WSTR(title, L"recording.wav");
	CHECK_WSTR(artist, L"");

	/* Whitespace-only values count as missing. */
	set_tags(&t, L"   ", L" \t ");
	mp_track_display(&t, L"C:\\a.mp3", title, MP_TAG_MAX, artist, MP_TAG_MAX);
	CHECK_WSTR(title, L"a.mp3");
	CHECK_WSTR(artist, L"");

	/* Surrounding whitespace is trimmed from real values. */
	set_tags(&t, L"  Padded  ", L" Name ");
	mp_track_display(&t, L"C:\\a.mp3", title, MP_TAG_MAX, artist, MP_TAG_MAX);
	CHECK_WSTR(title, L"Padded");
	CHECK_WSTR(artist, L"Name");

	/* Either output may be omitted. */
	set_tags(&t, L"T", L"A");
	mp_track_display(&t, L"C:\\a.mp3", NULL, 0, artist, MP_TAG_MAX);
	CHECK_WSTR(artist, L"A");
	mp_track_display(&t, L"C:\\a.mp3", title, MP_TAG_MAX, NULL, 0);
	CHECK_WSTR(title, L"T");

	/* A small output buffer truncates instead of overflowing. */
	{
		wchar_t small[5];
		mp_track_display(NULL, L"C:\\longfilename.mp3", small, 5, NULL, 0);
		CHECK_WSTR(small, L"long");
	}
}

static void time_format(void)
{
	wchar_t s[32];
	mp_format_time(0, s, 32);
	CHECK_WSTR(s, L"0:00");
	mp_format_time(999, s, 32); /* truncates, never rounds up */
	CHECK_WSTR(s, L"0:00");
	mp_format_time(1000, s, 32);
	CHECK_WSTR(s, L"0:01");
	mp_format_time(59999, s, 32);
	CHECK_WSTR(s, L"0:59");
	mp_format_time(60000, s, 32);
	CHECK_WSTR(s, L"1:00");
	mp_format_time(754000, s, 32);
	CHECK_WSTR(s, L"12:34");
	mp_format_time(3599999, s, 32);
	CHECK_WSTR(s, L"59:59");
	mp_format_time(3600000, s, 32); /* the hour appears at exactly one hour */
	CHECK_WSTR(s, L"1:00:00");
	mp_format_time(3723000, s, 32);
	CHECK_WSTR(s, L"1:02:03");
	mp_format_time(36000000u * 10, s, 32);
	CHECK_WSTR(s, L"100:00:00");
	mp_format_time(0xFFFFFFFFu, s, 32); /* ~49.7 days, the largest input */
	CHECK_WSTR(s, L"1193:02:47");
	/* Tiny buffers truncate safely. */
	mp_format_time(754000, s, 3);
	CHECK_WSTR(s, L"12");
	mp_format_time(754000, NULL, 0);
}

int suite_track(void)
{
	basenames();
	display_rules();
	time_format();
	return 0;
}
