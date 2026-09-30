/*
 * track.h - what the player shows for a track.
 *
 * The display rules, in one place so they can be unit tested:
 *   - Title: the title from the file's metadata; if there is none (or it is
 *     only whitespace), the file name, extension included.
 *   - Artist: the artist from the metadata, or an empty string if there is
 *     none, in which case the UI leaves the artist line out entirely.
 */
#ifndef MP_TRACK_H
#define MP_TRACK_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "tags.h"

/* Pointer to the file-name part of a path (after the last '\\', '/' or
 * drive colon). Returns the path itself if it has no directory part. */
const wchar_t *mp_path_basename(const wchar_t *path);

/* Applies the rules above. tags may be NULL (treated as "no metadata").
 * Either output may be NULL if the caller does not need it. */
void mp_track_display(const MpTags *tags, const wchar_t *path,
	wchar_t *title, size_t title_cap, wchar_t *artist, size_t artist_cap);

/* Formats a duration as "m:ss", or "h:mm:ss" from one hour up. */
void mp_format_time(uint32_t ms, wchar_t *out, size_t cap);

#endif
