/*
 * playlist.c - track list, navigation, and playlist file reading/writing.
 * See playlist.h.
 */
#include "playlist.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "text.h"

/* Longest path we build. Win32 on XP cannot open anything past MAX_PATH
 * (260) without the \\?\ prefix anyway; this leaves generous headroom for
 * paths that GetFullPathNameW will shorten (e.g. lots of "..\"). */
#define PATH_BUF 4096

static wchar_t *wdup(const wchar_t *s)
{
	size_t n;
	wchar_t *d;
	if (s == NULL)
		s = L"";
	n = wcslen(s) + 1;
	d = (wchar_t *)malloc(n * sizeof(wchar_t));
	if (d != NULL)
		memcpy(d, s, n * sizeof(wchar_t));
	return d;
}

/* ---- The list ----------------------------------------------------------- */

void mp_playlist_init(MpPlaylist *pl)
{
	memset(pl, 0, sizeof(*pl));
	pl->current = MP_NONE;
}

void mp_playlist_clear(MpPlaylist *pl)
{
	size_t i;
	for (i = 0; i < pl->count; i++) {
		free(pl->items[i].path);
		free(pl->items[i].title);
		free(pl->items[i].artist);
	}
	pl->count = 0;
	pl->current = MP_NONE;
}

void mp_playlist_free(MpPlaylist *pl)
{
	mp_playlist_clear(pl);
	free(pl->items);
	mp_playlist_init(pl);
}

int mp_playlist_add(MpPlaylist *pl, const wchar_t *path, const wchar_t *title, const wchar_t *artist,
	unsigned track)
{
	MpEntry e;
	if (pl->count == pl->cap) {
		size_t cap = pl->cap ? pl->cap * 2 : 16;
		MpEntry *items = (MpEntry *)realloc(pl->items, cap * sizeof(MpEntry));
		if (items == NULL)
			return 0;
		pl->items = items;
		pl->cap = cap;
	}
	e.path = wdup(path);
	e.title = wdup(title);
	e.artist = wdup(artist);
	e.track = track;
	if (e.path == NULL || e.title == NULL || e.artist == NULL) {
		free(e.path);
		free(e.title);
		free(e.artist);
		return 0;
	}
	pl->items[pl->count++] = e;
	return 1;
}

int mp_playlist_remove(MpPlaylist *pl, size_t index)
{
	if (index >= pl->count)
		return 0;
	free(pl->items[index].path);
	free(pl->items[index].title);
	free(pl->items[index].artist);
	memmove(&pl->items[index], &pl->items[index + 1], (pl->count - index - 1) * sizeof(MpEntry));
	pl->count--;
	if (pl->current != MP_NONE) {
		if (pl->current == index)
			pl->current = MP_NONE;
		else if (pl->current > index)
			pl->current--;
	}
	return 1;
}

size_t mp_playlist_step(const MpPlaylist *pl, size_t from, int direction, int wrap)
{
	if (pl->count == 0)
		return MP_NONE;
	if (from == MP_NONE || from >= pl->count)
		return direction >= 0 ? 0 : pl->count - 1;
	if (direction >= 0) {
		if (from + 1 < pl->count)
			return from + 1;
		return wrap ? 0 : MP_NONE;
	}
	if (from > 0)
		return from - 1;
	return wrap ? pl->count - 1 : MP_NONE;
}

/* ---- Reordering --------------------------------------------------------- */

/* The file name part of a path. (track.c has the public version, but the
 * playlist code does not depend on the display code.) */
static const wchar_t *file_part(const wchar_t *path)
{
	const wchar_t *p, *base = path;
	for (p = path; *p != 0; p++) {
		if (*p == L'\\' || *p == L'/' || *p == L':')
			base = p + 1;
	}
	return base;
}

/* Language-aware, case-insensitive, as Explorer sorts names. CompareStringW
 * exists on every Windows version; the fancier CompareStringEx does not
 * exist on XP. */
static int text_cmp(const wchar_t *a, const wchar_t *b)
{
	int r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a, -1, b, -1);
	if (r == 0) /* cannot happen with valid arguments; be deterministic */
		return wcscmp(a, b) < 0 ? -1 : wcscmp(a, b) > 0;
	return r - CSTR_EQUAL; /* CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3 */
}

typedef struct {
	const MpEntry *items;
	MpSortKey key;
	int descending;
} SortSpec;

/* Negative, zero or positive, as a comes before, ties with, or comes after
 * b in the requested order. Blanks always come after non-blanks. */
static int entry_cmp(const SortSpec *spec, const MpEntry *a, const MpEntry *b)
{
	int r;
	switch (spec->key) {
	case MP_SORT_TRACK:
		if ((a->track == 0) != (b->track == 0))
			return a->track == 0 ? 1 : -1;
		r = a->track < b->track ? -1 : a->track > b->track;
		break;
	case MP_SORT_ARTIST:
		if ((a->artist[0] == 0) != (b->artist[0] == 0))
			return a->artist[0] == 0 ? 1 : -1;
		r = text_cmp(a->artist, b->artist);
		break;
	case MP_SORT_FILE:
		r = text_cmp(file_part(a->path), file_part(b->path));
		break;
	default:
		r = text_cmp(a->title, b->title);
		break;
	}
	return spec->descending ? -r : r;
}

/* Merge sort of an index array: stable (which qsort is not guaranteed to
 * be), and O(n log n) however large the playlist. */
static void merge_sort(const SortSpec *spec, size_t *idx, size_t *tmp, size_t n)
{
	size_t mid, i, j, k;
	if (n < 2)
		return;
	mid = n / 2;
	merge_sort(spec, idx, tmp, mid);
	merge_sort(spec, idx + mid, tmp, n - mid);
	i = 0;
	j = mid;
	k = 0;
	while (i < mid && j < n) {
		/* <= 0 takes from the left half on ties: that is the stability. */
		if (entry_cmp(spec, &spec->items[idx[i]], &spec->items[idx[j]]) <= 0)
			tmp[k++] = idx[i++];
		else
			tmp[k++] = idx[j++];
	}
	while (i < mid)
		tmp[k++] = idx[i++];
	while (j < n)
		tmp[k++] = idx[j++];
	memcpy(idx, tmp, n * sizeof(size_t));
}

/* Rearranges the entries so that new position i holds old entry order[i],
 * and moves `current` along. Fills new_index (old -> new) if not NULL.
 * Returns 0 if out of memory, with nothing changed. */
static int apply_order(MpPlaylist *pl, const size_t *order, size_t *new_index)
{
	size_t i;
	MpEntry *items;
	if (pl->count == 0)
		return 1;
	items = (MpEntry *)malloc(pl->count * sizeof(MpEntry));
	if (items == NULL)
		return 0;
	for (i = 0; i < pl->count; i++)
		items[i] = pl->items[order[i]];
	for (i = 0; i < pl->count; i++) {
		if (order[i] == pl->current) {
			pl->current = i;
			break;
		}
	}
	if (new_index != NULL) {
		for (i = 0; i < pl->count; i++)
			new_index[order[i]] = i;
	}
	/* The entries were copied whole (the strings are shared, not
	 * duplicated), so only the array itself is swapped. */
	memcpy(pl->items, items, pl->count * sizeof(MpEntry));
	free(items);
	return 1;
}

int mp_playlist_sort(MpPlaylist *pl, MpSortKey key, int descending, size_t *new_index)
{
	SortSpec spec;
	size_t *order, *tmp, i;
	int ok;
	if (pl->count == 0)
		return 1;
	order = (size_t *)malloc(pl->count * sizeof(size_t));
	tmp = (size_t *)malloc(pl->count * sizeof(size_t));
	if (order == NULL || tmp == NULL) {
		free(order);
		free(tmp);
		return 0;
	}
	for (i = 0; i < pl->count; i++)
		order[i] = i;
	spec.items = pl->items;
	spec.key = key;
	spec.descending = descending;
	merge_sort(&spec, order, tmp, pl->count);
	ok = apply_order(pl, order, new_index);
	free(order);
	free(tmp);
	return ok;
}

int mp_playlist_move_block(MpPlaylist *pl, const size_t *indices, size_t n, size_t to)
{
	size_t *order, i, k, u, s;
	int ok;
	if (n == 0 || n > pl->count || to > pl->count - n || indices == NULL)
		return 0;
	for (i = 0; i < n; i++) {
		if (indices[i] >= pl->count || (i > 0 && indices[i] <= indices[i - 1]))
			return 0;
	}
	order = (size_t *)malloc(pl->count * sizeof(size_t));
	if (order == NULL)
		return 0;
	/* Walk the unselected entries (u) in order, dropping the whole block in
	 * when the output reaches `to`. `s` steps through `indices` to tell
	 * selected entries apart without a lookup table. */
	k = 0;
	s = 0;
	for (u = 0; u <= pl->count; u++) {
		if (k == to) {
			for (i = 0; i < n; i++)
				order[k++] = indices[i];
		}
		if (u == pl->count)
			break;
		if (s < n && indices[s] == u) {
			s++;
			continue;
		}
		order[k++] = u;
	}
	ok = apply_order(pl, order, NULL);
	free(order);
	return ok;
}

static void swap_entries(MpPlaylist *pl, unsigned char *selected, size_t a, size_t b)
{
	MpEntry e = pl->items[a];
	unsigned char f = selected[a];
	pl->items[a] = pl->items[b];
	pl->items[b] = e;
	selected[a] = selected[b];
	selected[b] = f;
	if (pl->current == a)
		pl->current = b;
	else if (pl->current == b)
		pl->current = a;
}

size_t mp_playlist_shift(MpPlaylist *pl, unsigned char *selected, int direction)
{
	size_t i, moved = 0;
	if (selected == NULL || pl->count < 2 || direction == 0)
		return 0;
	/* Each selected entry trades places with the unselected neighbor on the
	 * side it is moving toward. Walking from that side means an entry never
	 * trades with one that has just moved, and a run of selected entries
	 * stuck against the end stays stuck (its neighbor is selected too). */
	if (direction < 0) {
		for (i = 1; i < pl->count; i++) {
			if (selected[i] && !selected[i - 1]) {
				swap_entries(pl, selected, i, i - 1);
				moved++;
			}
		}
	} else {
		for (i = pl->count - 1; i-- > 0;) {
			if (selected[i] && !selected[i + 1]) {
				swap_entries(pl, selected, i, i + 1);
				moved++;
			}
		}
	}
	return moved;
}

/* ---- Path lists --------------------------------------------------------- */

void mp_pathlist_free(MpPathList *list)
{
	size_t i;
	for (i = 0; i < list->count; i++)
		free(list->paths[i]);
	free(list->paths);
	memset(list, 0, sizeof(*list));
}

int mp_pathlist_add(MpPathList *list, const wchar_t *path)
{
	wchar_t *copy;
	if (list->count == list->cap) {
		size_t cap = list->cap ? list->cap * 2 : 16;
		wchar_t **paths = (wchar_t **)realloc(list->paths, cap * sizeof(wchar_t *));
		if (paths == NULL)
			return 0;
		list->paths = paths;
		list->cap = cap;
	}
	copy = wdup(path);
	if (copy == NULL)
		return 0;
	list->paths[list->count++] = copy;
	return 1;
}

/* ---- Paths -------------------------------------------------------------- */

void mp_path_dirname(const wchar_t *path, wchar_t *out, size_t cap)
{
	size_t len = 0, i;
	if (cap == 0)
		return;
	for (i = 0; path[i] != 0; i++) {
		if (path[i] == L'\\' || path[i] == L'/')
			len = i;
	}
	if (len >= cap)
		len = cap - 1;
	for (i = 0; i < len; i++)
		out[i] = path[i];
	out[len] = 0;
	/* "C:\song.mp3" -> "C:" would make "C:" + "\x" fine, but a bare "C:"
	 * alone means "current dir on C:", so keep the root backslash. */
	if (len == 2 && out[1] == L':' && cap > 3) {
		out[2] = L'\\';
		out[3] = 0;
	}
}

static int hex_value(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* file:///C:/My%20Music/x.mp3 -> C:\My Music\x.mp3
 * file://server/share/x.mp3   -> \\server\share\x.mp3
 * The %-escapes encode UTF-8 bytes (RFC 3986 / RFC 8089), so the URL is
 * taken to UTF-8, unescaped there, and decoded back. */
static int file_url_to_path(const wchar_t *url, wchar_t *out, size_t cap)
{
	char *utf8, *bytes;
	size_t n, i, o = 0;
	const char *p;
	int ok = 0;

	n = mp_utf8_encode(url, NULL, 0);
	utf8 = (char *)malloc(n + 1);
	bytes = (char *)malloc(n + 4);
	if (utf8 == NULL || bytes == NULL)
		goto done;
	mp_utf8_encode(url, utf8, n + 1);
	p = utf8 + 5; /* after "file:" */

	if (p[0] == '/' && p[1] == '/') {
		const char *host = p + 2, *slash = strchr(host, '/');
		size_t host_len = slash ? (size_t)(slash - host) : strlen(host);
		if (host_len == 0 || (host_len == 9 && mp_ascii_ieq_n(host, "localhost", 9))) {
			p = slash ? slash : host + host_len;
		} else {
			/* A real host name becomes a UNC path. */
			bytes[o++] = '\\';
			bytes[o++] = '\\';
			p = host;
		}
	}
	/* "/C:/x" -> "C:/x"; "/C|/x" is the pre-RFC spelling of the same. */
	if (o == 0 && p[0] == '/' && ((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z')) &&
		(p[2] == ':' || p[2] == '|'))
		p++;

	for (i = 0; p[i] != 0; i++) {
		int hi, lo;
		char c = p[i];
		if (c == '%' && (hi = hex_value((unsigned char)p[i + 1])) >= 0 &&
			(lo = hex_value((unsigned char)p[i + 2])) >= 0) {
			c = (char)(hi * 16 + lo);
			i += 2;
		} else if (c == '|' && o == 1 && i == 1) {
			c = ':';
		}
		if (c == 0)
			goto done; /* %00 is never legitimate in a path */
		bytes[o++] = (c == '/') ? '\\' : c;
	}
	bytes[o] = 0;
	mp_utf8_decode((const uint8_t *)bytes, o, out, cap);
	ok = out[0] != 0;
done:
	free(utf8);
	free(bytes);
	return ok;
}

static int is_drive_absolute(const wchar_t *p)
{
	return ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
		p[1] == L':' && p[2] == L'\\';
}

int mp_path_resolve(const wchar_t *base_dir, const wchar_t *ref, wchar_t *out, size_t cap)
{
	wchar_t *work = NULL, *combined = NULL;
	size_t i, n;
	DWORD got;
	int ok = 0;

	if (ref == NULL || ref[0] == 0 || cap == 0)
		return 0;
	out[0] = 0;
	work = (wchar_t *)malloc(PATH_BUF * sizeof(wchar_t));
	combined = (wchar_t *)malloc(PATH_BUF * 2 * sizeof(wchar_t));
	if (work == NULL || combined == NULL)
		goto done;

	if (mp_wascii_ieq_n(ref, L"file:", 5)) {
		if (!file_url_to_path(ref, work, PATH_BUF))
			goto done;
	} else if (wcsstr(ref, L"://") != NULL) {
		goto done; /* http:// and friends: streaming is not supported */
	} else {
		mp_wcopy(work, PATH_BUF, ref);
	}
	for (i = 0; work[i] != 0; i++) {
		if (work[i] == L'/')
			work[i] = L'\\';
	}

	if (is_drive_absolute(work) || (work[0] == L'\\' && work[1] == L'\\')) {
		mp_wcopy(combined, PATH_BUF * 2, work);
	} else if (work[0] == L'\\') {
		/* Rooted: same drive (or UNC share) as the playlist. */
		size_t root = 0;
		if (base_dir != NULL && base_dir[0] != 0 && base_dir[1] == L':') {
			root = 2;
		} else if (base_dir != NULL && base_dir[0] == L'\\' && base_dir[1] == L'\\') {
			/* \\server\share is the root of a UNC path. */
			int slashes = 0;
			for (root = 2; base_dir[root] != 0; root++) {
				if (base_dir[root] == L'\\' && ++slashes == 2)
					break;
			}
		}
		for (i = 0; i < root; i++)
			combined[i] = base_dir[i];
		mp_wcopy(combined + root, PATH_BUF * 2 - root, work);
	} else if (work[1] == L':') {
		/* "C:x.mp3" (drive-relative) - vanishingly rare; let Windows decide. */
		mp_wcopy(combined, PATH_BUF * 2, work);
	} else {
		n = mp_wcopy(combined, PATH_BUF, base_dir ? base_dir : L"");
		if (n > 0 && combined[n - 1] != L'\\')
			combined[n++] = L'\\';
		mp_wcopy(combined + n, PATH_BUF * 2 - n, work);
	}

	/* Collapses "." and ".." and double backslashes. It does not touch the
	 * disk, so nonexistent files resolve fine (they fail later, at play
	 * time, with a proper message). */
	got = GetFullPathNameW(combined, (DWORD)cap, out, NULL);
	ok = got > 0 && got < cap;
	if (!ok)
		out[0] = 0;
done:
	free(work);
	free(combined);
	return ok;
}

/* ---- Reading playlist files -------------------------------------------- */

static wchar_t *decode_playlist_text(const uint8_t *data, size_t size)
{
	wchar_t *text = (wchar_t *)malloc((size + 1) * sizeof(wchar_t));
	if (text == NULL)
		return NULL;
	text[0] = 0;
	if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		mp_utf8_decode(data + 3, size - 3, text, size + 1);
	} else if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
		mp_utf16_decode(data + 2, size - 2, 0, text, size + 1);
	} else if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
		mp_utf16_decode(data + 2, size - 2, 1, text, size + 1);
	} else if (mp_utf8_is_valid(data, size)) {
		mp_utf8_decode(data, size, text, size + 1);
	} else {
		/* Classic .m3u files from Winamp-era players are in the ANSI code
		 * page, which only Windows itself knows how to decode. */
		int n = MultiByteToWideChar(CP_ACP, 0, (const char *)data, (int)size, text, (int)size);
		text[n > 0 ? n : 0] = 0;
	}
	return text;
}

/* Walks the text line by line (LF, CRLF or bare CR), trimming each one.
 * Returns the next line, or NULL at the end; modifies the text in place. */
static wchar_t *next_line(wchar_t **cursor)
{
	wchar_t *start = *cursor, *p;
	if (start == NULL || *start == 0)
		return NULL;
	for (p = start; *p != 0 && *p != L'\n' && *p != L'\r'; p++)
		;
	if (*p == L'\r' && p[1] == L'\n') {
		*p = 0;
		*cursor = p + 2;
	} else if (*p != 0) {
		*p = 0;
		*cursor = p + 1;
	} else {
		*cursor = p;
	}
	mp_trim(start);
	return start;
}

static int add_resolved(MpPathList *out, const wchar_t *base_dir, const wchar_t *ref, wchar_t *buf)
{
	if (!mp_path_resolve(base_dir, ref, buf, PATH_BUF))
		return 1; /* not a local file: skip it, but that is not an error */
	return mp_pathlist_add(out, buf);
}

typedef struct {
	long number;
	size_t order;
	wchar_t *ref;
} PlsEntry;

static int pls_compare(const void *a, const void *b)
{
	const PlsEntry *x = (const PlsEntry *)a, *y = (const PlsEntry *)b;
	if (x->number != y->number)
		return x->number < y->number ? -1 : 1;
	/* qsort is not stable; tie-break on file order to make it so. */
	return x->order < y->order ? -1 : (x->order > y->order ? 1 : 0);
}

static int parse_pls(wchar_t *cursor, const wchar_t *base_dir, MpPathList *out, wchar_t *buf)
{
	PlsEntry *entries = NULL;
	size_t count = 0, cap = 0, i;
	wchar_t *line;
	int ok = 1;

	while ((line = next_line(&cursor)) != NULL) {
		long number = 0;
		wchar_t *p;
		if (!mp_wascii_ieq_n(line, L"file", 4) || line[4] < L'0' || line[4] > L'9')
			continue;
		for (p = line + 4; *p >= L'0' && *p <= L'9'; p++)
			number = number < 100000000 ? number * 10 + (*p - L'0') : number;
		if (*p != L'=')
			continue;
		p++;
		while (*p == L' ' || *p == L'\t')
			p++;
		if (*p == 0)
			continue;
		if (count == cap) {
			size_t ncap = cap ? cap * 2 : 16;
			PlsEntry *grown = (PlsEntry *)realloc(entries, ncap * sizeof(PlsEntry));
			if (grown == NULL) {
				ok = 0;
				break;
			}
			entries = grown;
			cap = ncap;
		}
		entries[count].number = number;
		entries[count].order = count;
		entries[count].ref = p;
		count++;
	}
	/* PLS entries are numbered and some writers do not emit them in order. */
	if (ok && count > 1)
		qsort(entries, count, sizeof(PlsEntry), pls_compare);
	for (i = 0; ok && i < count; i++)
		ok = add_resolved(out, base_dir, entries[i].ref, buf);
	free(entries);
	return ok;
}

static int parse_m3u(wchar_t *cursor, const wchar_t *base_dir, MpPathList *out, wchar_t *buf)
{
	wchar_t *line;
	while ((line = next_line(&cursor)) != NULL) {
		/* Blank lines and #EXTM3U / #EXTINF / other directives. We do not
		 * use #EXTINF titles: the file's own tags are the source of truth. */
		if (line[0] == 0 || line[0] == L'#')
			continue;
		if (!add_resolved(out, base_dir, line, buf))
			return 0;
	}
	return 1;
}

int mp_playlist_parse(const uint8_t *data, size_t size, const wchar_t *playlist_path, MpPathList *out)
{
	wchar_t *text, *cursor, *probe, *line;
	wchar_t *base_dir, *buf;
	int ok = 0, is_pls = 0;

	text = decode_playlist_text(data, size);
	base_dir = (wchar_t *)malloc(PATH_BUF * sizeof(wchar_t));
	buf = (wchar_t *)malloc(PATH_BUF * sizeof(wchar_t));
	if (text == NULL || base_dir == NULL || buf == NULL)
		goto done;
	mp_path_dirname(playlist_path ? playlist_path : L"", base_dir, PATH_BUF);

	/* Find the first non-blank line to decide the format, on a scratch copy
	 * because next_line() cuts the text up. */
	probe = (wchar_t *)malloc((wcslen(text) + 1) * sizeof(wchar_t));
	if (probe == NULL)
		goto done;
	wcscpy(probe, text);
	cursor = probe;
	while ((line = next_line(&cursor)) != NULL && line[0] == 0)
		;
	is_pls = line != NULL && mp_wascii_ieq(line, L"[playlist]");
	free(probe);

	cursor = text;
	ok = is_pls ? parse_pls(cursor, base_dir, out, buf) : parse_m3u(cursor, base_dir, out, buf);
done:
	free(text);
	free(base_dir);
	free(buf);
	return ok;
}

/* ---- Writing ------------------------------------------------------------ */

typedef struct {
	char *data;
	size_t len;
	size_t cap;
	int failed;
} ByteBuf;

static void bb_append(ByteBuf *b, const char *s, size_t n)
{
	if (b->failed)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 1024;
		char *grown;
		while (b->len + n + 1 > cap)
			cap *= 2;
		grown = (char *)realloc(b->data, cap);
		if (grown == NULL) {
			b->failed = 1;
			return;
		}
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = 0;
}

static void bb_str(ByteBuf *b, const char *s)
{
	bb_append(b, s, strlen(s));
}

static void bb_wide(ByteBuf *b, const wchar_t *s)
{
	size_t n = mp_utf8_encode(s, NULL, 0);
	char *tmp = (char *)malloc(n + 1);
	if (tmp == NULL) {
		b->failed = 1;
		return;
	}
	mp_utf8_encode(s, tmp, n + 1);
	bb_append(b, tmp, n);
	free(tmp);
}

int mp_playlist_write_m3u8(const MpPlaylist *pl, const wchar_t *playlist_path, char **out, size_t *out_size)
{
	ByteBuf b;
	wchar_t *dir;
	size_t dir_len, i;

	*out = NULL;
	*out_size = 0;
	memset(&b, 0, sizeof(b));
	dir = (wchar_t *)malloc(PATH_BUF * sizeof(wchar_t));
	if (dir == NULL)
		return 0;
	mp_path_dirname(playlist_path, dir, PATH_BUF - 1);
	dir_len = wcslen(dir);
	/* Match "dir\" so that C:\Music does not claim C:\Music2\x.mp3. */
	if (dir_len > 0 && dir[dir_len - 1] != L'\\') {
		dir[dir_len++] = L'\\';
		dir[dir_len] = 0;
	}

	bb_str(&b, "#EXTM3U\r\n");
	for (i = 0; i < pl->count; i++) {
		const MpEntry *e = &pl->items[i];
		const wchar_t *path = e->path;
		/* #EXTINF:<seconds>,<display>. -1 means "length unknown", which is
		 * allowed by the format and saves decoding every file. */
		bb_str(&b, "#EXTINF:-1,");
		if (e->artist != NULL && e->artist[0] != 0) {
			bb_wide(&b, e->artist);
			bb_str(&b, " - ");
		}
		bb_wide(&b, e->title ? e->title : L"");
		bb_str(&b, "\r\n");
		/* Windows paths compare case-insensitively. */
		if (dir_len > 0 && _wcsnicmp(path, dir, dir_len) == 0 && path[dir_len] != 0)
			path += dir_len;
		bb_wide(&b, path);
		bb_str(&b, "\r\n");
	}
	free(dir);
	if (b.failed) {
		free(b.data);
		return 0;
	}
	*out = b.data;
	*out_size = b.len;
	return 1;
}
