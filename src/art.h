/*
 * art.h - finds a track's album art: the picture embedded in the file, or
 * failing that an image file next to it in its folder.
 *
 * Many people keep their art as a file beside the music rather than inside
 * every track ("cover.jpg", "folder.jpg"), and Windows Media Player and
 * Explorer save it that way too, so both places are looked in.
 */
#ifndef MP_ART_H
#define MP_ART_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/*
 * The first of these that exists in the track's folder (names ignore case,
 * as Windows does):
 *   cover, folder, front, album, albumart - each as .jpg, .jpeg, .png
 *   AlbumArt_*_Large.jpg, then AlbumArtSmall.jpg (what Windows Media
 *   Player writes)
 * Returns its bytes in a malloc'd *data of *size bytes, or 0 if there is
 * none (or it is bigger than MP_IMAGE_MAX_BYTES).
 */
int mp_art_find_folder_image(const wchar_t *track_path, uint8_t **data, size_t *size);

/* The track's art: embedded first (mp_tags_read_picture_file), then the
 * folder. Returns 0, with *data NULL, if it has none. */
int mp_art_load(const wchar_t *track_path, uint8_t **data, size_t *size);

#endif
