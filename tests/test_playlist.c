/*
 * test_playlist.c - the track list, navigation, and playlist files.
 *
 * Path resolution uses GetFullPathNameW, which is purely textual for the
 * absolute paths used here, so none of these files need to exist.
 */
#include <stdlib.h>

#include "mptest.h"
#include "playlist.h"

#define PL L"C:\\Music\\Lists\\my.m3u"

static int parse_bytes(const void *data, size_t size, const wchar_t *playlist_path, MpPathList *out)
{
	memset(out, 0, sizeof(*out));
	return mp_playlist_parse((const uint8_t *)data, size, playlist_path, out);
}

static int parse_str(const char *text, const wchar_t *playlist_path, MpPathList *out)
{
	return parse_bytes(text, strlen(text), playlist_path, out);
}

/* ---- M3U ---------------------------------------------------------------- */

static void m3u_basics(void)
{
	MpPathList l;
	/* Directives, blank lines, all three line-ending styles, and padding. */
	CHECK(parse_str(
		"#EXTM3U\r\n"
		"#EXTINF:123,Artist - Title\r\n"
		"C:\\Music\\a.mp3\r\n"
		"\r\n"
		"   D:\\b.flac   \n"
		"#comment\r"
		"E:\\c.wav", PL, &l));
	CHECK_INT(l.count, 3);
	if (l.count == 3) {
		CHECK_WSTR(l.paths[0], L"C:\\Music\\a.mp3");
		CHECK_WSTR(l.paths[1], L"D:\\b.flac");
		CHECK_WSTR(l.paths[2], L"E:\\c.wav");
	}
	mp_pathlist_free(&l);

	/* Empty and comment-only playlists parse fine to nothing. */
	CHECK(parse_str("", PL, &l));
	CHECK_INT(l.count, 0);
	mp_pathlist_free(&l);
	CHECK(parse_str("#EXTM3U\n\n#EXTINF:1,x\n", PL, &l));
	CHECK_INT(l.count, 0);
	mp_pathlist_free(&l);
}

static void m3u_relative_paths(void)
{
	MpPathList l;
	CHECK(parse_str(
		"song.mp3\n"
		"sub/dir/a.mp3\n"
		"..\\Other\\b.mp3\n"
		".\\c.mp3\n"
		"\\Rooted\\d.mp3\n"
		"D:/forward/e.mp3\n"
		"sub\\\\double\\f.mp3\n", PL, &l));
	CHECK_INT(l.count, 7);
	if (l.count == 7) {
		CHECK_WSTR(l.paths[0], L"C:\\Music\\Lists\\song.mp3");
		CHECK_WSTR(l.paths[1], L"C:\\Music\\Lists\\sub\\dir\\a.mp3");
		CHECK_WSTR(l.paths[2], L"C:\\Music\\Other\\b.mp3");
		CHECK_WSTR(l.paths[3], L"C:\\Music\\Lists\\c.mp3");
		CHECK_WSTR(l.paths[4], L"C:\\Rooted\\d.mp3");
		CHECK_WSTR(l.paths[5], L"D:\\forward\\e.mp3");
		CHECK_WSTR(l.paths[6], L"C:\\Music\\Lists\\sub\\double\\f.mp3");
	}
	mp_pathlist_free(&l);

	/* Playlists on network shares resolve against the share. */
	CHECK(parse_str("x.mp3\n\\top.mp3\n", L"\\\\server\\share\\lists\\p.m3u", &l));
	CHECK_INT(l.count, 2);
	if (l.count == 2) {
		CHECK_WSTR(l.paths[0], L"\\\\server\\share\\lists\\x.mp3");
		CHECK_WSTR(l.paths[1], L"\\\\server\\share\\top.mp3");
	}
	mp_pathlist_free(&l);

	/* A playlist in a drive root. */
	CHECK(parse_str("x.mp3\n", L"C:\\root.m3u", &l));
	CHECK_INT(l.count, 1);
	if (l.count == 1)
		CHECK_WSTR(l.paths[0], L"C:\\x.mp3");
	mp_pathlist_free(&l);
}

static void m3u_urls(void)
{
	MpPathList l;
	CHECK(parse_str(
		"file:///C:/My%20Music/%C3%A9t%C3%A9.mp3\n"
		"file://server/share/a%20b.mp3\n"
		"FILE://localhost/D:/x.mp3\n"
		"file:///E|/legacy.mp3\n"
		"http://radio.example.com/stream.mp3\n"
		"rtsp://example.com/x\n"
		"file:///C:/bad%00name.mp3\n"
		"file:///C:/100%.mp3\n", PL, &l));
	CHECK_INT(l.count, 5);
	if (l.count == 5) {
		CHECK_WSTR(l.paths[0], L"C:\\My Music\\\x00E9t\x00E9.mp3");
		CHECK_WSTR(l.paths[1], L"\\\\server\\share\\a b.mp3");
		CHECK_WSTR(l.paths[2], L"D:\\x.mp3");
		CHECK_WSTR(l.paths[3], L"E:\\legacy.mp3");
		/* A '%' not followed by two hex digits is kept literally. */
		CHECK_WSTR(l.paths[4], L"C:\\100%.mp3");
	}
	mp_pathlist_free(&l);
}

static void m3u_encodings(void)
{
	MpPathList l;
	/* UTF-8 with BOM. */
	CHECK(parse_str("\xEF\xBB\xBF" "C:\\Caf\xC3\xA9.mp3\n", PL, &l));
	CHECK_INT(l.count, 1);
	if (l.count == 1)
		CHECK_WSTR(l.paths[0], L"C:\\Caf\x00E9.mp3");
	mp_pathlist_free(&l);

	/* UTF-8 without BOM (typical .m3u8). */
	CHECK(parse_str("C:\\\xE6\x9D\xB1.mp3\n", PL, &l));
	CHECK_INT(l.count, 1);
	if (l.count == 1)
		CHECK_WSTR(l.paths[0], L"C:\\\x6771.mp3");
	mp_pathlist_free(&l);

	/* UTF-16, both byte orders. */
	{
		static const uint8_t le[] = { 0xFF, 0xFE, 'C', 0, ':', 0, '\\', 0, 'u', 0, '.', 0, 'm', 0, 'p', 0, '3', 0, '\n', 0 };
		static const uint8_t be[] = { 0xFE, 0xFF, 0, 'C', 0, ':', 0, '\\', 0, 'v', 0, '.', 0, 'w', 0, 'a', 0, 'v' };
		CHECK(parse_bytes(le, sizeof(le), PL, &l));
		CHECK_INT(l.count, 1);
		if (l.count == 1)
			CHECK_WSTR(l.paths[0], L"C:\\u.mp3");
		mp_pathlist_free(&l);
		CHECK(parse_bytes(be, sizeof(be), PL, &l));
		CHECK_INT(l.count, 1);
		if (l.count == 1)
			CHECK_WSTR(l.paths[0], L"C:\\v.wav");
		mp_pathlist_free(&l);
	}

	/* Not valid UTF-8: decoded with the ANSI code page. What 0xE9 becomes
	 * depends on the machine's code page, so check only that the entry is
	 * there and the ASCII parts are intact. */
	CHECK(parse_str("C:\\Caf\xE9.mp3\n", PL, &l));
	CHECK_INT(l.count, 1);
	if (l.count == 1) {
		CHECK(wcsncmp(l.paths[0], L"C:\\Caf", 6) == 0);
		CHECK(wcscmp(l.paths[0] + wcslen(l.paths[0]) - 4, L".mp3") == 0);
	}
	mp_pathlist_free(&l);
}

/* ---- PLS ---------------------------------------------------------------- */

static void pls(void)
{
	MpPathList l;
	/* Out-of-order and multi-digit numbers are sorted numerically; other
	 * keys are ignored; key names and the header are case-insensitive. */
	CHECK(parse_str(
		"\n[Playlist]\r\n"
		"NumberOfEntries=4\r\n"
		"File2=two.mp3\r\n"
		"Title2=Ignored\r\n"
		"FILE10=ten.mp3\r\n"
		"file1 =bad-key.mp3\r\n"
		"File1=  one.mp3\r\n"
		"File9=http://stream.example.com/\r\n"
		"File3=\r\n"
		"Length1=-1\r\n"
		"Version=2\r\n", PL, &l));
	CHECK_INT(l.count, 3);
	if (l.count == 3) {
		CHECK_WSTR(l.paths[0], L"C:\\Music\\Lists\\one.mp3");
		CHECK_WSTR(l.paths[1], L"C:\\Music\\Lists\\two.mp3");
		CHECK_WSTR(l.paths[2], L"C:\\Music\\Lists\\ten.mp3");
	}
	mp_pathlist_free(&l);

	/* Without the [playlist] header, "File1=" lines are just odd M3U
	 * entries (relative file names containing '='). */
	CHECK(parse_str("File1=x.mp3\n", PL, &l));
	CHECK_INT(l.count, 1);
	if (l.count == 1)
		CHECK_WSTR(l.paths[0], L"C:\\Music\\Lists\\File1=x.mp3");
	mp_pathlist_free(&l);
}

/* ---- Paths -------------------------------------------------------------- */

static void path_helpers(void)
{
	wchar_t out[260];
	mp_path_dirname(L"C:\\a\\b.mp3", out, 260);
	CHECK_WSTR(out, L"C:\\a");
	mp_path_dirname(L"C:\\b.mp3", out, 260);
	CHECK_WSTR(out, L"C:\\");
	mp_path_dirname(L"b.mp3", out, 260);
	CHECK_WSTR(out, L"");
	mp_path_dirname(L"C:/fwd/x.m3u", out, 260);
	CHECK_WSTR(out, L"C:/fwd");

	CHECK(mp_path_resolve(L"C:\\base", L"x.mp3", out, 260));
	CHECK_WSTR(out, L"C:\\base\\x.mp3");
	CHECK(mp_path_resolve(L"C:\\base\\", L"x.mp3", out, 260));
	CHECK_WSTR(out, L"C:\\base\\x.mp3");
	CHECK(!mp_path_resolve(L"C:\\base", L"", out, 260));
	CHECK(!mp_path_resolve(L"C:\\base", NULL, out, 260));
	CHECK(!mp_path_resolve(L"C:\\base", L"https://x/y.mp3", out, 260));
	/* Output too small: fails cleanly rather than truncating a path. */
	CHECK(!mp_path_resolve(L"C:\\base", L"x.mp3", out, 5));
	CHECK_WSTR(out, L"");
}

/* ---- The list and navigation -------------------------------------------- */

static void list_and_navigation(void)
{
	MpPlaylist pl;
	mp_playlist_init(&pl);

	/* Empty list: nowhere to go. */
	CHECK(mp_playlist_step(&pl, MP_NONE, 1, 1) == MP_NONE);
	CHECK(mp_playlist_step(&pl, MP_NONE, -1, 1) == MP_NONE);
	CHECK(mp_playlist_step(&pl, 0, 1, 1) == MP_NONE);

	CHECK(mp_playlist_add(&pl, L"C:\\a.mp3", L"A", L"Artist A", 0));
	/* NULL title/artist are stored as empty strings. */
	CHECK(mp_playlist_add(&pl, L"C:\\b.mp3", NULL, NULL, 0));
	CHECK(mp_playlist_add(&pl, L"C:\\c.mp3", L"C", L"", 0));
	CHECK_INT(pl.count, 3);
	CHECK_WSTR(pl.items[1].title, L"");
	CHECK_WSTR(pl.items[1].artist, L"");
	CHECK(pl.current == MP_NONE);

	/* From nothing: forward starts at the top, back at the bottom. */
	CHECK_INT(mp_playlist_step(&pl, MP_NONE, 1, 0), 0);
	CHECK_INT(mp_playlist_step(&pl, MP_NONE, -1, 0), 2);
	/* Middle. */
	CHECK_INT(mp_playlist_step(&pl, 1, 1, 0), 2);
	CHECK_INT(mp_playlist_step(&pl, 1, -1, 0), 0);
	/* Ends, with and without wrapping. */
	CHECK(mp_playlist_step(&pl, 2, 1, 0) == MP_NONE);
	CHECK_INT(mp_playlist_step(&pl, 2, 1, 1), 0);
	CHECK(mp_playlist_step(&pl, 0, -1, 0) == MP_NONE);
	CHECK_INT(mp_playlist_step(&pl, 0, -1, 1), 2);
	/* A stale out-of-range index is treated like "nothing". */
	CHECK_INT(mp_playlist_step(&pl, 99, 1, 0), 0);

	/* Removing keeps `current` on the same track. */
	pl.current = 2;
	CHECK(mp_playlist_remove(&pl, 0)); /* before current: shifts down */
	CHECK_INT(pl.current, 1);
	CHECK_WSTR(pl.items[1].path, L"C:\\c.mp3");
	CHECK(mp_playlist_remove(&pl, 1)); /* current itself: none */
	CHECK(pl.current == MP_NONE);
	CHECK_INT(pl.count, 1);
	CHECK(!mp_playlist_remove(&pl, 5)); /* out of range */
	pl.current = 0;
	CHECK(mp_playlist_add(&pl, L"C:\\d.mp3", L"D", L"", 0));
	CHECK(mp_playlist_remove(&pl, 1)); /* after current: unchanged */
	CHECK_INT(pl.current, 0);

	/* One track: wrapping comes back to itself. */
	CHECK_INT(mp_playlist_step(&pl, 0, 1, 1), 0);
	CHECK(mp_playlist_step(&pl, 0, 1, 0) == MP_NONE);

	mp_playlist_clear(&pl);
	CHECK_INT(pl.count, 0);
	CHECK(pl.current == MP_NONE);
	/* Usable again after clear, and growth past the initial capacity. */
	{
		int i;
		for (i = 0; i < 100; i++)
			CHECK(mp_playlist_add(&pl, L"C:\\x.mp3", L"x", L"", 0));
		CHECK_INT(pl.count, 100);
	}
	mp_playlist_free(&pl);
	CHECK_INT(pl.count, 0);
}

/* ---- Reordering --------------------------------------------------------- */

/* The list's order, as the first letter of each title: "BAC" means B, A, C. */
static int order_is(const MpPlaylist *pl, const wchar_t *letters)
{
	size_t i;
	if (wcslen(letters) != pl->count)
		return 0;
	for (i = 0; i < pl->count; i++) {
		if (pl->items[i].title[0] != letters[i])
			return 0;
	}
	return 1;
}

/* Six tracks, A to F, with nothing else to them. */
static void six(MpPlaylist *pl)
{
	static const wchar_t *const names[] = { L"A", L"B", L"C", L"D", L"E", L"F" };
	int i;
	mp_playlist_init(pl);
	for (i = 0; i < 6; i++)
		mp_playlist_add(pl, L"C:\\x.mp3", names[i], L"", 0);
}

/*
 * Five tracks for sorting, identified by their file names a..e (the titles
 * are what gets sorted, so they cannot identify the entries):
 *   c.mp3   "banana"  artist "Zed"    track 3
 *   a.mp3   "Apple"   no artist       no track
 *   B.flac  "cherry"  artist "alpha"  track 1   (in another folder)
 *   d.wav   "apple"   artist "Alpha"  track 2   (same title as a, but case)
 *   e.mp3   "Eclair"  artist "zed"    no track  (with an accented E)
 */
static void five(MpPlaylist *pl)
{
	mp_playlist_init(pl);
	mp_playlist_add(pl, L"C:\\m\\c.mp3", L"banana", L"Zed", 3);
	mp_playlist_add(pl, L"C:\\m\\a.mp3", L"Apple", L"", 0);
	mp_playlist_add(pl, L"D:\\other\\B.flac", L"cherry", L"alpha", 1);
	mp_playlist_add(pl, L"C:\\m\\d.wav", L"apple", L"Alpha", 2);
	mp_playlist_add(pl, L"C:\\m\\e.mp3", L"\x00C9" L"clair", L"zed", 0);
}

/* The order as the file names' first letters, lowercased: "cabde". */
static int files_are(const MpPlaylist *pl, const char *letters)
{
	size_t i;
	if (strlen(letters) != pl->count)
		return 0;
	for (i = 0; i < pl->count; i++) {
		const wchar_t *p = pl->items[i].path, *base = p;
		for (; *p != 0; p++) {
			if (*p == L'\\')
				base = p + 1;
		}
		if ((char)(*base | 0x20) != letters[i])
			return 0;
	}
	return 1;
}

static void sorting(void)
{
	MpPlaylist pl;
	size_t new_index[5];

	/* Title: case and accents are ignored the way Explorer ignores them,
	 * so Eclair (with its accent) comes after cherry, not after Z. "Apple"
	 * and "apple" tie, and the tie keeps the existing order (a before d). */
	five(&pl);
	pl.current = 0; /* banana is playing */
	CHECK(mp_playlist_sort(&pl, MP_SORT_TITLE, 0, new_index));
	CHECK(files_are(&pl, "adcbe"));
	/* current follows banana to its new place... */
	CHECK_INT(pl.current, 2);
	/* ...and new_index says where every old entry went. */
	CHECK_INT(new_index[0], 2);
	CHECK_INT(new_index[1], 0);
	CHECK_INT(new_index[2], 3);
	CHECK_INT(new_index[3], 1);
	CHECK_INT(new_index[4], 4);
	mp_playlist_free(&pl);

	/* Descending reverses the order, but ties still keep their existing
	 * order (a before d): a stable sort is stable in both directions. */
	five(&pl);
	CHECK(mp_playlist_sort(&pl, MP_SORT_TITLE, 1, NULL));
	CHECK(files_are(&pl, "ebcad"));
	CHECK(pl.current == MP_NONE); /* nothing playing stays nothing */
	mp_playlist_free(&pl);

	/* Track number: blanks go last, in both directions. */
	five(&pl);
	CHECK(mp_playlist_sort(&pl, MP_SORT_TRACK, 0, NULL));
	CHECK(files_are(&pl, "bdcae"));
	mp_playlist_free(&pl);
	five(&pl);
	CHECK(mp_playlist_sort(&pl, MP_SORT_TRACK, 1, NULL));
	CHECK(files_are(&pl, "cdbae"));
	mp_playlist_free(&pl);

	/* Artist: case-insensitive ties keep their order; no artist goes last,
	 * in both directions. */
	five(&pl);
	CHECK(mp_playlist_sort(&pl, MP_SORT_ARTIST, 0, NULL));
	CHECK(files_are(&pl, "bdcea"));
	mp_playlist_free(&pl);
	five(&pl);
	CHECK(mp_playlist_sort(&pl, MP_SORT_ARTIST, 1, NULL));
	CHECK(files_are(&pl, "cebda"));
	mp_playlist_free(&pl);

	/* File: the file name only, not the folder (B.flac is on D:) and not
	 * the case. */
	five(&pl);
	pl.current = 4;
	CHECK(mp_playlist_sort(&pl, MP_SORT_FILE, 0, NULL));
	CHECK(files_are(&pl, "abcde"));
	CHECK_INT(pl.current, 4);
	mp_playlist_free(&pl);

	/* Stability put to use: by track, then by artist, gives each artist's
	 * tracks in track order. */
	mp_playlist_init(&pl);
	mp_playlist_add(&pl, L"C:\\3.mp3", L"X3", L"Xenon", 3);
	mp_playlist_add(&pl, L"C:\\1.mp3", L"X1", L"Xenon", 1);
	mp_playlist_add(&pl, L"C:\\5.mp3", L"A5", L"Argon", 5);
	mp_playlist_add(&pl, L"C:\\2.mp3", L"X2", L"Xenon", 2);
	CHECK(mp_playlist_sort(&pl, MP_SORT_TRACK, 0, NULL));
	CHECK(mp_playlist_sort(&pl, MP_SORT_ARTIST, 0, NULL));
	CHECK_WSTR(pl.items[0].title, L"A5");
	CHECK_WSTR(pl.items[1].title, L"X1");
	CHECK_WSTR(pl.items[2].title, L"X2");
	CHECK_WSTR(pl.items[3].title, L"X3");
	mp_playlist_free(&pl);

	/* Empty and single-entry lists are fine. */
	mp_playlist_init(&pl);
	CHECK(mp_playlist_sort(&pl, MP_SORT_TITLE, 0, NULL));
	mp_playlist_add(&pl, L"C:\\only.mp3", L"Only", L"", 0);
	pl.current = 0;
	CHECK(mp_playlist_sort(&pl, MP_SORT_ARTIST, 1, new_index));
	CHECK_INT(new_index[0], 0);
	CHECK_INT(pl.current, 0);
	mp_playlist_free(&pl);

	/* A big list, sorted and reverse-sorted: the merge sort's recursion
	 * and every element ends up in order. */
	{
		int i, ok = 1;
		mp_playlist_init(&pl);
		for (i = 0; i < 1000; i++) {
			wchar_t title[5];
			/* 7919 is prime, so this visits 0..999 in a scrambled order.
			 * Formatted by hand: swprintf's signature differs between the
			 * C runtimes the two toolchains use. */
			int v = (i * 7919) % 1000;
			title[0] = L'0';
			title[1] = (wchar_t)(L'0' + v / 100);
			title[2] = (wchar_t)(L'0' + v / 10 % 10);
			title[3] = (wchar_t)(L'0' + v % 10);
			title[4] = 0;
			mp_playlist_add(&pl, L"C:\\x.mp3", title, L"", (unsigned)((i * 7919) % 1000) + 1);
		}
		CHECK(mp_playlist_sort(&pl, MP_SORT_TITLE, 0, NULL));
		for (i = 1; i < 1000; i++)
			ok &= wcscmp(pl.items[i - 1].title, pl.items[i].title) < 0;
		CHECK(ok);
		CHECK(mp_playlist_sort(&pl, MP_SORT_TRACK, 1, NULL));
		for (i = 1; i < 1000; i++)
			ok &= pl.items[i - 1].track > pl.items[i].track;
		CHECK(ok);
		mp_playlist_free(&pl);
	}
}

static void move_block(void)
{
	MpPlaylist pl;
	size_t idx[6];

	/* Two apart, gathered at the top, keeping their order; D was playing
	 * and stays the playing track. */
	six(&pl);
	pl.current = 3;
	idx[0] = 1;
	idx[1] = 3;
	CHECK(mp_playlist_move_block(&pl, idx, 2, 0));
	CHECK(order_is(&pl, L"BDACEF"));
	CHECK_INT(pl.current, 1);
	mp_playlist_free(&pl);

	/* To the very end (to = count - n). */
	six(&pl);
	pl.current = 1; /* B, which is not moving, still moves up one */
	idx[0] = 0;
	idx[1] = 2;
	CHECK(mp_playlist_move_block(&pl, idx, 2, 4));
	CHECK(order_is(&pl, L"BDEFAC"));
	CHECK_INT(pl.current, 0);
	mp_playlist_free(&pl);

	/* Into the middle, from both sides of it. */
	six(&pl);
	idx[0] = 0;
	idx[1] = 5;
	CHECK(mp_playlist_move_block(&pl, idx, 2, 2));
	CHECK(order_is(&pl, L"BCAFDE"));
	mp_playlist_free(&pl);

	/* One entry down by one, the smallest drag. */
	six(&pl);
	idx[0] = 2;
	CHECK(mp_playlist_move_block(&pl, idx, 1, 3));
	CHECK(order_is(&pl, L"ABDCEF"));
	mp_playlist_free(&pl);

	/* Already there, and everything at once: no change. */
	six(&pl);
	idx[0] = 2;
	idx[1] = 3;
	CHECK(mp_playlist_move_block(&pl, idx, 2, 2));
	CHECK(order_is(&pl, L"ABCDEF"));
	{
		int i;
		for (i = 0; i < 6; i++)
			idx[i] = (size_t)i;
	}
	CHECK(mp_playlist_move_block(&pl, idx, 6, 0));
	CHECK(order_is(&pl, L"ABCDEF"));
	mp_playlist_free(&pl);

	/* Bad arguments are refused and change nothing. */
	six(&pl);
	pl.current = 2;
	idx[0] = 3;
	idx[1] = 1; /* not increasing */
	CHECK(!mp_playlist_move_block(&pl, idx, 2, 0));
	idx[0] = 1;
	idx[1] = 1; /* repeated */
	CHECK(!mp_playlist_move_block(&pl, idx, 2, 0));
	idx[0] = 1;
	idx[1] = 6; /* out of range */
	CHECK(!mp_playlist_move_block(&pl, idx, 2, 0));
	idx[1] = 2;
	CHECK(!mp_playlist_move_block(&pl, idx, 2, 5)); /* would run off the end */
	CHECK(!mp_playlist_move_block(&pl, idx, 0, 0)); /* nothing to move */
	CHECK(!mp_playlist_move_block(&pl, NULL, 1, 0));
	CHECK(!mp_playlist_move_block(&pl, idx, 7, 0)); /* more than there are */
	CHECK(order_is(&pl, L"ABCDEF"));
	CHECK_INT(pl.current, 2);
	mp_playlist_free(&pl);
}

static void shift(void)
{
	MpPlaylist pl;
	unsigned char sel[6];

	/* Two apart, up: each passes its unselected neighbor. */
	six(&pl);
	memset(sel, 0, sizeof(sel));
	sel[1] = sel[3] = 1;
	pl.current = 3; /* D */
	CHECK_INT(mp_playlist_shift(&pl, sel, -1), 2);
	CHECK(order_is(&pl, L"BADCEF"));
	/* The flags followed the entries. */
	CHECK(sel[0] && !sel[1] && sel[2] && !sel[3] && !sel[4] && !sel[5]);
	CHECK_INT(pl.current, 2);
	/* Again: B is at the top and stays; D closes up behind it. */
	CHECK_INT(mp_playlist_shift(&pl, sel, -1), 1);
	CHECK(order_is(&pl, L"BDACEF"));
	CHECK(sel[0] && sel[1] && !sel[2]);
	CHECK_INT(pl.current, 1);
	/* And again: packed at the top, nothing moves (and nothing wraps). */
	CHECK_INT(mp_playlist_shift(&pl, sel, -1), 0);
	CHECK(order_is(&pl, L"BDACEF"));
	mp_playlist_free(&pl);

	/* A run of two moves down as a block, the unselected track passing
	 * over both. */
	six(&pl);
	memset(sel, 0, sizeof(sel));
	sel[0] = sel[1] = 1;
	pl.current = 2; /* C, the one being passed over */
	CHECK_INT(mp_playlist_shift(&pl, sel, 1), 2);
	CHECK(order_is(&pl, L"CABDEF"));
	CHECK(!sel[0] && sel[1] && sel[2] && !sel[3]);
	CHECK_INT(pl.current, 0);
	mp_playlist_free(&pl);

	/* Already at the bottom: nothing to do. */
	six(&pl);
	memset(sel, 0, sizeof(sel));
	sel[4] = sel[5] = 1;
	CHECK_INT(mp_playlist_shift(&pl, sel, 1), 0);
	CHECK(order_is(&pl, L"ABCDEF"));
	/* But up is fine. */
	CHECK_INT(mp_playlist_shift(&pl, sel, -1), 2);
	CHECK(order_is(&pl, L"ABCEFD"));
	mp_playlist_free(&pl);

	/* Nothing selected, no direction, no flags, too short a list. */
	six(&pl);
	memset(sel, 0, sizeof(sel));
	CHECK_INT(mp_playlist_shift(&pl, sel, 1), 0);
	sel[2] = 1;
	CHECK_INT(mp_playlist_shift(&pl, sel, 0), 0);
	CHECK_INT(mp_playlist_shift(&pl, NULL, 1), 0);
	CHECK(order_is(&pl, L"ABCDEF"));
	mp_playlist_free(&pl);
	mp_playlist_init(&pl);
	mp_playlist_add(&pl, L"C:\\x.mp3", L"A", L"", 0);
	sel[0] = 1;
	CHECK_INT(mp_playlist_shift(&pl, sel, 1), 0);
	CHECK_INT(mp_playlist_shift(&pl, sel, -1), 0);
	mp_playlist_free(&pl);
}

/* The track number is stored with the entry. */
static void track_numbers_kept(void)
{
	MpPlaylist pl;
	mp_playlist_init(&pl);
	CHECK(mp_playlist_add(&pl, L"C:\\a.mp3", L"A", L"", 7));
	CHECK(mp_playlist_add(&pl, L"C:\\b.mp3", L"B", L"", 0));
	CHECK_INT(pl.items[0].track, 7);
	CHECK_INT(pl.items[1].track, 0);
	mp_playlist_free(&pl);
}

/* ---- Writing ------------------------------------------------------------ */

static void write_m3u8(void)
{
	MpPlaylist pl;
	MpPathList back;
	char *data;
	size_t size;
	mp_playlist_init(&pl);
	mp_playlist_add(&pl, L"C:\\Music\\Lists\\here.mp3", L"Here", L"Band", 0);
	mp_playlist_add(&pl, L"C:\\Music\\Lists\\sub\\deeper.flac", L"Deep", L"", 0);
	mp_playlist_add(&pl, L"c:\\music\\lists\\case.mp3", L"Case", L"", 0);
	mp_playlist_add(&pl, L"C:\\Music\\ListsTwo\\sibling.mp3", L"Sibling", L"", 0);
	mp_playlist_add(&pl, L"D:\\Elsewhere\\\x00E9t\x00E9.wav", L"\x00C9t\x00E9", L"Bj\x00F6rk", 0);

	CHECK(mp_playlist_write_m3u8(&pl, L"C:\\Music\\Lists\\out.m3u8", &data, &size));
	CHECK_INT(size, strlen(data));
	CHECK(strcmp(data,
		"#EXTM3U\r\n"
		"#EXTINF:-1,Band - Here\r\n"
		"here.mp3\r\n"
		"#EXTINF:-1,Deep\r\n"
		"sub\\deeper.flac\r\n"
		/* Windows paths are case-insensitive, so this is "inside" too. */
		"#EXTINF:-1,Case\r\n"
		"case.mp3\r\n"
		/* ...but a folder that merely starts with the same name is not. */
		"#EXTINF:-1,Sibling\r\n"
		"C:\\Music\\ListsTwo\\sibling.mp3\r\n"
		"#EXTINF:-1,Bj\xC3\xB6rk - \xC3\x89t\xC3\xA9\r\n"
		"D:\\Elsewhere\\\xC3\xA9t\xC3\xA9.wav\r\n") == 0);

	/* Reading it back yields the same tracks (as absolute paths). */
	CHECK(parse_bytes(data, size, L"C:\\Music\\Lists\\out.m3u8", &back));
	CHECK_INT(back.count, 5);
	if (back.count == 5) {
		CHECK_WSTR(back.paths[0], L"C:\\Music\\Lists\\here.mp3");
		CHECK_WSTR(back.paths[1], L"C:\\Music\\Lists\\sub\\deeper.flac");
		CHECK_WSTR(back.paths[3], L"C:\\Music\\ListsTwo\\sibling.mp3");
		CHECK_WSTR(back.paths[4], L"D:\\Elsewhere\\\x00E9t\x00E9.wav");
	}
	mp_pathlist_free(&back);
	free(data);

	/* A playlist saved in a drive root. */
	CHECK(mp_playlist_write_m3u8(&pl, L"C:\\root.m3u8", &data, &size));
	CHECK(strstr(data, "\r\nMusic\\Lists\\here.mp3\r\n") != NULL);
	free(data);

	/* An empty playlist is just the header. */
	mp_playlist_clear(&pl);
	CHECK(mp_playlist_write_m3u8(&pl, L"C:\\x.m3u8", &data, &size));
	CHECK(strcmp(data, "#EXTM3U\r\n") == 0);
	free(data);
	mp_playlist_free(&pl);
}

int suite_playlist(void)
{
	m3u_basics();
	m3u_relative_paths();
	m3u_urls();
	m3u_encodings();
	pls();
	path_helpers();
	list_and_navigation();
	track_numbers_kept();
	sorting();
	move_block();
	shift();
	write_m3u8();
	return 0;
}
