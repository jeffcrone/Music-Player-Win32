/*
 * text.h - character-set conversion helpers.
 *
 * Audio tags and playlists arrive in half a dozen encodings (UTF-8, UTF-16
 * in either byte order, Windows-1252, ...). Everything is converted to
 * UTF-16 wchar_t strings, which is what the Win32 "W" APIs want.
 *
 * These are written by hand rather than with MultiByteToWideChar so that the
 * results do not depend on the Windows version or the user's code page,
 * which keeps the tag parser's behavior identical on XP and on Windows 11
 * and makes it testable with exact expected strings.
 *
 * Conventions for every decoder below:
 *   - `cap` is the size of `out` in wchar_t units, including the terminator.
 *   - The output is always NUL-terminated when cap > 0.
 *   - Decoding stops at the first NUL code unit in the input (tag fields are
 *     routinely NUL-padded or NUL-separated).
 *   - If the output fills up, it is truncated, never mid surrogate pair.
 *   - The return value is the number of wchar_t written, not counting the NUL.
 */
#ifndef MP_TEXT_H
#define MP_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* Returns 1 if the bytes (up to n, or the first NUL) are well-formed UTF-8. */
int mp_utf8_is_valid(const uint8_t *s, size_t n);

/* Invalid or overlong sequences, encoded surrogates and values above
 * U+10FFFF each become U+FFFD (REPLACEMENT CHARACTER). */
size_t mp_utf8_decode(const uint8_t *s, size_t n, wchar_t *out, size_t cap);

/* Windows-1252, the superset of ISO-8859-1 that Windows software actually
 * writes when a format says "Latin-1" (ID3v1, ID3v2 encoding 0, RIFF INFO).
 * The five byte values 1252 leaves undefined map to the C1 control code
 * with the same number, exactly as MultiByteToWideChar does. */
size_t mp_cp1252_decode(const uint8_t *s, size_t n, wchar_t *out, size_t cap);

/* UTF-16 of the given byte order. n is in bytes; an odd trailing byte is
 * ignored. Unpaired surrogates become U+FFFD. */
size_t mp_utf16_decode(const uint8_t *s, size_t n, int big_endian, wchar_t *out, size_t cap);

/* Encode a NUL-terminated wide string as UTF-8. Writes at most cap bytes
 * including the terminator (pass out = NULL, cap = 0 to measure). Returns
 * the full encoded length in bytes, excluding the terminator, even if the
 * output was truncated. Unpaired surrogates are encoded as U+FFFD. */
size_t mp_utf8_encode(const wchar_t *s, char *out, size_t cap);

/* Strip leading and trailing whitespace and turn any remaining control
 * characters into spaces, in place. Returns the new length. */
size_t mp_trim(wchar_t *s);

/* Copy src into dst (cap in wchar_t, including the NUL), truncating safely.
 * Returns the number of wchar_t copied. */
size_t mp_wcopy(wchar_t *dst, size_t cap, const wchar_t *src);

/* Case-insensitive comparison of ASCII letters only; everything else must
 * match exactly. Used for tag field names and file extensions, which are
 * ASCII by specification, so no locale can change the answer. */
int mp_ascii_ieq(const char *a, const char *b);
int mp_ascii_ieq_n(const char *a, const char *b, size_t n);
int mp_wascii_ieq(const wchar_t *a, const wchar_t *b);
int mp_wascii_ieq_n(const wchar_t *a, const wchar_t *b, size_t n);

#endif
