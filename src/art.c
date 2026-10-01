/*
 * art.c - where a track's album art comes from. See art.h.
 */
#include "art.h"

#include <stdlib.h>
#include <windows.h>

#include "image.h"
#include "playlist.h"
#include "stream.h"
#include "tags.h"
#include "text.h"

/* Longest folder path handled; anything longer cannot be opened on XP
 * without the \\?\ prefix anyway. */
#define DIR_MAX 1024

/* Reads `name` from `dir`. Returns 1 if it exists and was read. */
static int try_file(const wchar_t *dir, const wchar_t *name, uint8_t **data, size_t *size)
{
	wchar_t path[DIR_MAX + MAX_PATH];
	size_t len;
	mp_wcopy(path, DIR_MAX + MAX_PATH, dir);
	len = wcslen(path);
	if (len > 0 && path[len - 1] != L'\\' && len + 1 < DIR_MAX + MAX_PATH) {
		path[len++] = L'\\';
		path[len] = 0;
	}
	if (len + wcslen(name) + 1 > DIR_MAX + MAX_PATH)
		return 0;
	mp_wcopy(path + len, DIR_MAX + MAX_PATH - len, name);
	if (!mp_file_read_all(path, MP_IMAGE_MAX_BYTES, data, size))
		return 0;
	/* An empty file is not a picture; free whatever the read allocated
	 * and let the next name be tried. */
	if (*size == 0) {
		free(*data);
		*data = NULL;
		return 0;
	}
	return 1;
}

int mp_art_find_folder_image(const wchar_t *track_path, uint8_t **data, size_t *size)
{
	/* In order of how surely the name means "this album's cover". */
	static const wchar_t *const names[] = {
		L"cover", L"folder", L"front", L"album", L"albumart"
	};
	static const wchar_t *const exts[] = { L".jpg", L".jpeg", L".png" };
	wchar_t dir[DIR_MAX], name[64];
	WIN32_FIND_DATAW fd;
	HANDLE find;
	size_t i, j;
	*data = NULL;
	*size = 0;
	if (track_path == NULL || track_path[0] == 0)
		return 0;
	mp_path_dirname(track_path, dir, DIR_MAX);
	if (dir[0] == 0)
		return 0;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		for (j = 0; j < sizeof(exts) / sizeof(exts[0]); j++) {
			mp_wcopy(name, 64, names[i]);
			wcscat(name, exts[j]);
			if (try_file(dir, name, data, size))
				return 1;
		}
	}
	/* Windows Media Player names its art after the album's ID, so it has
	 * to be searched for. A folder normally holds one album and so one
	 * such file; one that mixes albums has several, and then no single
	 * picture is right for every track anyway, so the first will do. */
	{
		wchar_t pattern[DIR_MAX + 32];
		mp_wcopy(pattern, DIR_MAX + 32, dir);
		if (wcslen(pattern) + 24 < DIR_MAX + 32) {
			wcscat(pattern, L"\\AlbumArt_*_Large.jpg");
			find = FindFirstFileW(pattern, &fd);
			if (find != INVALID_HANDLE_VALUE) {
				int ok = try_file(dir, fd.cFileName, data, size);
				FindClose(find);
				if (ok)
					return 1;
			}
		}
	}
	return try_file(dir, L"AlbumArtSmall.jpg", data, size);
}

int mp_art_load(const wchar_t *track_path, uint8_t **data, size_t *size)
{
	if (mp_tags_read_picture_file(track_path, data, size))
		return 1;
	return mp_art_find_folder_image(track_path, data, size);
}
