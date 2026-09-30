/*
 * track.c - display rules for a track. See track.h.
 */
#include "track.h"

#include "text.h"

const wchar_t *mp_path_basename(const wchar_t *path)
{
	const wchar_t *base = path, *p;
	if (path == NULL)
		return L"";
	for (p = path; *p != 0; p++) {
		/* ':' covers drive-relative paths such as "C:song.mp3". */
		if (*p == L'\\' || *p == L'/' || *p == L':')
			base = p + 1;
	}
	return base;
}

void mp_track_display(const MpTags *tags, const wchar_t *path,
	wchar_t *title, size_t title_cap, wchar_t *artist, size_t artist_cap)
{
	if (title != NULL && title_cap > 0) {
		title[0] = 0;
		if (tags != NULL) {
			mp_wcopy(title, title_cap, tags->title);
			mp_trim(title);
		}
		if (title[0] == 0)
			mp_wcopy(title, title_cap, mp_path_basename(path));
	}
	if (artist != NULL && artist_cap > 0) {
		artist[0] = 0;
		if (tags != NULL) {
			mp_wcopy(artist, artist_cap, tags->artist);
			mp_trim(artist);
		}
	}
}

/* Appends the decimal digits of v, zero-padded to at least `width`. */
static void put_number(wchar_t *out, size_t cap, size_t *len, uint32_t v, int width)
{
	wchar_t digits[10];
	int n = 0;
	do {
		digits[n++] = (wchar_t)(L'0' + v % 10);
		v /= 10;
	} while (v != 0);
	while (n < width)
		digits[n++] = L'0';
	while (n > 0 && *len + 1 < cap)
		out[(*len)++] = digits[--n];
	out[*len] = 0;
}

static void put_char(wchar_t *out, size_t cap, size_t *len, wchar_t c)
{
	if (*len + 1 < cap)
		out[(*len)++] = c;
	out[*len] = 0;
}

void mp_format_time(uint32_t ms, wchar_t *out, size_t cap)
{
	uint32_t total = ms / 1000; /* truncate, as every player does */
	uint32_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
	size_t len = 0;
	if (out == NULL || cap == 0)
		return;
	out[0] = 0;
	if (h > 0) {
		put_number(out, cap, &len, h, 1);
		put_char(out, cap, &len, L':');
		put_number(out, cap, &len, m, 2);
	} else {
		put_number(out, cap, &len, m, 1);
	}
	put_char(out, cap, &len, L':');
	put_number(out, cap, &len, s, 2);
}
