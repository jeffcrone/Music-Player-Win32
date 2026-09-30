/*
 * text.c - character-set conversion helpers. See text.h for the contract.
 */
#include "text.h"

#define REPLACEMENT 0xFFFDu

/* Accumulates code points into a bounded UTF-16 buffer. Once a code point
 * does not fit, the writer is "full" and ignores everything after it, so a
 * surrogate pair is never split and later short characters never sneak in
 * after a dropped long one. */
typedef struct {
	wchar_t *out;
	size_t cap;
	size_t len;
	int full;
} WideWriter;

static void ww_init(WideWriter *w, wchar_t *out, size_t cap)
{
	w->out = out;
	w->cap = cap;
	w->len = 0;
	w->full = (out == NULL || cap == 0);
	if (!w->full)
		out[0] = 0;
}

static void ww_put(WideWriter *w, uint32_t cp)
{
	size_t units = cp >= 0x10000u ? 2 : 1;
	if (w->full)
		return;
	/* Leave room for the terminator. */
	if (w->len + units + 1 > w->cap) {
		w->full = 1;
		return;
	}
	if (units == 2) {
		cp -= 0x10000u;
		w->out[w->len++] = (wchar_t)(0xD800u + (cp >> 10));
		w->out[w->len++] = (wchar_t)(0xDC00u + (cp & 0x3FFu));
	} else {
		w->out[w->len++] = (wchar_t)cp;
	}
	w->out[w->len] = 0;
}

/* Decodes one UTF-8 sequence starting at s[*i]. Returns the code point, or
 * REPLACEMENT for anything malformed (consuming a single byte, so that the
 * following bytes get their own chance to start a valid sequence). */
static uint32_t utf8_next(const uint8_t *s, size_t n, size_t *i, int *valid)
{
	uint8_t b0 = s[*i];
	uint32_t cp;
	size_t need, k;
	uint32_t min;

	if (b0 < 0x80) {
		(*i)++;
		return b0;
	} else if (b0 >= 0xC2 && b0 <= 0xDF) {
		need = 1; cp = b0 & 0x1Fu; min = 0x80;
	} else if (b0 >= 0xE0 && b0 <= 0xEF) {
		need = 2; cp = b0 & 0x0Fu; min = 0x800;
	} else if (b0 >= 0xF0 && b0 <= 0xF4) {
		need = 3; cp = b0 & 0x07u; min = 0x10000;
	} else {
		/* 0x80-0xC1 (stray continuation or overlong lead) and 0xF5-0xFF. */
		(*i)++;
		*valid = 0;
		return REPLACEMENT;
	}
	if (*i + need >= n) {
		/* Not enough bytes left for the whole sequence (truncated field). */
		(*i)++;
		*valid = 0;
		return REPLACEMENT;
	}
	for (k = 1; k <= need; k++) {
		uint8_t b = s[*i + k];
		if ((b & 0xC0u) != 0x80u) {
			(*i)++;
			*valid = 0;
			return REPLACEMENT;
		}
		cp = (cp << 6) | (b & 0x3Fu);
	}
	if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
		(*i)++;
		*valid = 0;
		return REPLACEMENT;
	}
	*i += need + 1;
	return cp;
}

/* Length of s up to n bytes or the first NUL, whichever comes first. */
static size_t bounded_len(const uint8_t *s, size_t n)
{
	size_t i = 0;
	while (i < n && s[i] != 0)
		i++;
	return i;
}

int mp_utf8_is_valid(const uint8_t *s, size_t n)
{
	size_t i = 0;
	int valid = 1;
	if (s == NULL)
		return 1;
	n = bounded_len(s, n);
	while (i < n && valid)
		utf8_next(s, n, &i, &valid);
	return valid;
}

size_t mp_utf8_decode(const uint8_t *s, size_t n, wchar_t *out, size_t cap)
{
	WideWriter w;
	size_t i = 0;
	int valid = 1;
	ww_init(&w, out, cap);
	if (s == NULL)
		return 0;
	n = bounded_len(s, n);
	while (i < n && !w.full)
		ww_put(&w, utf8_next(s, n, &i, &valid));
	return w.len;
}

/* Windows-1252 bytes 0x80-0x9F. Zero marks the five undefined positions,
 * which pass through as the C1 control of the same value. */
static const uint16_t cp1252_high[32] = {
	0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
	0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

size_t mp_cp1252_decode(const uint8_t *s, size_t n, wchar_t *out, size_t cap)
{
	WideWriter w;
	size_t i;
	ww_init(&w, out, cap);
	if (s == NULL)
		return 0;
	n = bounded_len(s, n);
	for (i = 0; i < n && !w.full; i++) {
		uint32_t cp = s[i];
		if (cp >= 0x80 && cp <= 0x9F && cp1252_high[cp - 0x80] != 0)
			cp = cp1252_high[cp - 0x80];
		ww_put(&w, cp);
	}
	return w.len;
}

size_t mp_utf16_decode(const uint8_t *s, size_t n, int big_endian, wchar_t *out, size_t cap)
{
	WideWriter w;
	size_t i;
	ww_init(&w, out, cap);
	if (s == NULL)
		return 0;
	n &= ~(size_t)1;
	for (i = 0; i < n && !w.full; i += 2) {
		uint32_t u = big_endian ? ((uint32_t)s[i] << 8 | s[i + 1]) : ((uint32_t)s[i + 1] << 8 | s[i]);
		if (u == 0)
			break;
		if (u >= 0xD800u && u <= 0xDBFFu) {
			uint32_t lo = 0;
			if (i + 3 < n)
				lo = big_endian ? ((uint32_t)s[i + 2] << 8 | s[i + 3]) : ((uint32_t)s[i + 3] << 8 | s[i + 2]);
			if (lo >= 0xDC00u && lo <= 0xDFFFu) {
				ww_put(&w, 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u));
				i += 2;
			} else {
				ww_put(&w, REPLACEMENT);
			}
		} else if (u >= 0xDC00u && u <= 0xDFFFu) {
			ww_put(&w, REPLACEMENT);
		} else {
			ww_put(&w, u);
		}
	}
	return w.len;
}

size_t mp_utf8_encode(const wchar_t *s, char *out, size_t cap)
{
	size_t total = 0, written = 0, i;
	int full = (out == NULL || cap == 0);
	if (!full)
		out[0] = 0;
	if (s == NULL)
		return 0;
	for (i = 0; s[i] != 0; i++) {
		uint32_t cp = (uint16_t)s[i];
		char buf[4];
		size_t len, k;
		if (cp >= 0xD800u && cp <= 0xDBFFu && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
			cp = 0x10000u + ((cp - 0xD800u) << 10) + ((uint16_t)s[i + 1] - 0xDC00u);
			i++;
		} else if (cp >= 0xD800u && cp <= 0xDFFFu) {
			cp = REPLACEMENT;
		}
		if (cp < 0x80) {
			buf[0] = (char)cp; len = 1;
		} else if (cp < 0x800) {
			buf[0] = (char)(0xC0 | (cp >> 6));
			buf[1] = (char)(0x80 | (cp & 0x3F)); len = 2;
		} else if (cp < 0x10000) {
			buf[0] = (char)(0xE0 | (cp >> 12));
			buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
			buf[2] = (char)(0x80 | (cp & 0x3F)); len = 3;
		} else {
			buf[0] = (char)(0xF0 | (cp >> 18));
			buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
			buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
			buf[3] = (char)(0x80 | (cp & 0x3F)); len = 4;
		}
		total += len;
		/* Same rule as the wide writer: never emit part of a character. */
		if (!full && written + len + 1 <= cap) {
			for (k = 0; k < len; k++)
				out[written++] = buf[k];
			out[written] = 0;
		} else {
			full = 1;
		}
	}
	return total;
}

static int is_trim_space(wchar_t c)
{
	/* Control characters, ordinary spaces and the no-break space. Tag
	 * editors are fond of padding with any of these. */
	return c <= 0x20 || c == 0x7F || c == 0xA0 || c == 0xFEFF;
}

size_t mp_trim(wchar_t *s)
{
	size_t start = 0, end, i;
	if (s == NULL)
		return 0;
	while (s[start] != 0 && is_trim_space(s[start]))
		start++;
	end = start;
	while (s[end] != 0)
		end++;
	while (end > start && is_trim_space(s[end - 1]))
		end--;
	for (i = start; i < end; i++) {
		wchar_t c = s[i];
		/* Embedded tabs/newlines would draw as boxes in a one-line label. */
		s[i - start] = (c < 0x20 || c == 0x7F) ? L' ' : c;
	}
	s[end - start] = 0;
	return end - start;
}

size_t mp_wcopy(wchar_t *dst, size_t cap, const wchar_t *src)
{
	size_t i = 0;
	if (dst == NULL || cap == 0)
		return 0;
	if (src != NULL) {
		while (src[i] != 0 && i + 1 < cap)
			i++;
		/* Don't leave a lone high surrogate at the cut. */
		if (src[i] != 0 && i > 0 && src[i - 1] >= 0xD800 && src[i - 1] <= 0xDBFF)
			i--;
		{
			size_t k;
			for (k = 0; k < i; k++)
				dst[k] = src[k];
		}
	}
	dst[i] = 0;
	return i;
}

static int ascii_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int mp_ascii_ieq_n(const char *a, const char *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i]))
			return 0;
		if (a[i] == 0)
			return 1;
	}
	return 1;
}

int mp_ascii_ieq(const char *a, const char *b)
{
	return mp_ascii_ieq_n(a, b, (size_t)-1);
}

int mp_wascii_ieq_n(const wchar_t *a, const wchar_t *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (ascii_lower(a[i]) != ascii_lower(b[i]))
			return 0;
		if (a[i] == 0)
			return 1;
	}
	return 1;
}

int mp_wascii_ieq(const wchar_t *a, const wchar_t *b)
{
	return mp_wascii_ieq_n(a, b, (size_t)-1);
}
