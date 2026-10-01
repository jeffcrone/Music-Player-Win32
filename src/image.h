/*
 * image.h - turns an album-art picture (JPEG, PNG, BMP or GIF bytes) into
 * the small square the window shows.
 *
 * No Windows calls, so the tests can check the pixels directly; main.c
 * puts the result into a bitmap.
 */
#ifndef MP_IMAGE_H
#define MP_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* Pictures bigger than this many bytes are not even looked at. Real cover
 * art is well under a megabyte or two; 16 MB covers the most lavish
 * uncompressed scans. */
#define MP_IMAGE_MAX_BYTES (16u * 1024u * 1024u)

/* Largest square mp_image_thumbnail will make: room for a 400%-scaled
 * window. */
#define MP_IMAGE_MAX_BOX 1024

/*
 * Decodes `data` (any of the supported formats, detected from the bytes)
 * to `box` x `box` pixels, 0x00RRGGBB, top row first - the layout of a
 * 32-bit top-down DIB.
 *
 *   - The picture keeps its proportions and is centered; a wide or tall one
 *     leaves bands of `bg_rgb` (0x00RRGGBB) above and below, or beside it.
 *   - Shrinking averages every source pixel that falls in each output
 *     pixel (an area average), so detail like text on a cover turns into a
 *     smooth smaller version instead of the jagged, shimmering result of
 *     just picking every Nth pixel. Enlarging (a tiny picture in a bigger
 *     box) uses the same averaging, which comes out as clean square blocks.
 *   - Transparency (PNG, GIF) is blended onto `bg_rgb`, so a transparent
 *     cover looks right on the window behind it.
 *
 * Returns a malloc'd array of box * box pixels, or NULL if the data is not
 * a picture we can read, is too big (MP_IMAGE_MAX_BYTES, or more than 8192
 * pixels a side), `box` is out of range (1..MP_IMAGE_MAX_BOX), or memory
 * runs out.
 */
uint32_t *mp_image_thumbnail(const uint8_t *data, size_t size, int box, uint32_t bg_rgb);

/*
 * The scaling step on its own, for pixels already decoded: `rgba` is
 * w x h pixels of 4 bytes each, R, G, B, A in that order (what stb_image
 * produces). Writes box * box pixels to `out` as described above. Returns
 * 0, writing nothing, for bad arguments.
 */
int mp_image_fit(const uint8_t *rgba, int w, int h, int box, uint32_t bg_rgb, uint32_t *out);

#endif
