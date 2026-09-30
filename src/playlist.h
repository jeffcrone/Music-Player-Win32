/*
 * playlist.h - the in-memory track list, navigation, and playlist files.
 *
 * Playlist file formats read: M3U / extended M3U (.m3u, .m3u8) and PLS.
 * Written: extended M3U in UTF-8 (.m3u8).
 */
#ifndef MP_PLAYLIST_H
#define MP_PLAYLIST_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* "No track" marker for indexes. */
#define MP_NONE ((size_t)-1)

typedef struct {
	wchar_t *path;
	wchar_t *title;  /* display title (see track.h rules) */
	wchar_t *artist; /* display artist, "" if none */
} MpEntry;

typedef struct {
	MpEntry *items;
	size_t count;
	size_t cap;
	size_t current; /* index of the loaded track, or MP_NONE */
} MpPlaylist;

void mp_playlist_init(MpPlaylist *pl);
void mp_playlist_free(MpPlaylist *pl);
void mp_playlist_clear(MpPlaylist *pl);

/* Appends a copy of the strings. title/artist may be NULL. Returns 1 on
 * success, 0 if out of memory. */
int mp_playlist_add(MpPlaylist *pl, const wchar_t *path, const wchar_t *title, const wchar_t *artist);

/* Removes one entry and keeps `current` pointing at the same track: it
 * moves down when an earlier entry goes, and becomes MP_NONE if the
 * current entry itself is removed. Returns 0 if index is out of range. */
int mp_playlist_remove(MpPlaylist *pl, size_t index);

/*
 * The index `direction` (+1 or -1) steps away from `from`.
 *   - from == MP_NONE starts before the first entry (+1 gives 0) or after
 *     the last (-1 gives count - 1).
 *   - Past either end: wraps around if `wrap`, otherwise MP_NONE.
 *   - An empty list always gives MP_NONE.
 */
size_t mp_playlist_step(const MpPlaylist *pl, size_t from, int direction, int wrap);

/* A growable list of paths, as produced by parsing a playlist file. */
typedef struct {
	wchar_t **paths;
	size_t count;
	size_t cap;
} MpPathList;

void mp_pathlist_free(MpPathList *list);
int mp_pathlist_add(MpPathList *list, const wchar_t *path);

/*
 * Parses playlist file contents into absolute paths, appended to *out.
 * playlist_path is where the file lives; relative entries are resolved
 * against its folder. Detects the format from the content (a "[playlist]"
 * header means PLS, anything else is treated as M3U), and the text encoding
 * from the byte order mark, falling back to UTF-8 if the bytes are valid
 * UTF-8 and the system ANSI code page otherwise (what old .m3u files use).
 * Entries that are URLs other than file:// are skipped (no streaming).
 * Returns 1 on success, 0 if out of memory.
 */
int mp_playlist_parse(const uint8_t *data, size_t size, const wchar_t *playlist_path, MpPathList *out);

/*
 * Serializes the playlist as extended M3U, UTF-8 without BOM, CRLF line
 * endings. Tracks inside the playlist's folder (or below it) are written as
 * relative paths so the folder can be moved or copied as a unit; others are
 * written as absolute paths. *out is malloc'd; free it with free().
 * Returns 1 on success, 0 if out of memory.
 */
int mp_playlist_write_m3u8(const MpPlaylist *pl, const wchar_t *playlist_path, char **out, size_t *out_size);

/*
 * Resolves a playlist entry to a full path. `ref` may be absolute
 * ("C:\\x.mp3", "\\\\server\\share\\x.mp3"), rooted ("\\Music\\x.mp3",
 * taken to be on the playlist's drive), relative ("x.mp3", "..\\x.mp3"),
 * use forward slashes, or be a file:// URL with %-escapes.
 * Returns 1 on success, 0 if the entry is not a local file.
 */
int mp_path_resolve(const wchar_t *base_dir, const wchar_t *ref, wchar_t *out, size_t cap);

/* Copies the folder part of path (without a trailing backslash) into out. */
void mp_path_dirname(const wchar_t *path, wchar_t *out, size_t cap);

#endif
