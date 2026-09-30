/*
 * glyph.h - draws the playback buttons' symbols (previous, play, pause,
 * stop, next) as anti-aliased pixels.
 *
 * Why drawn in code rather than shipped as .ico resources: the window scales
 * with the system font (see main.c), so the symbols have to come in whatever
 * pixel size that works out to - 13 px with XP's Tahoma, 15 px with Segoe UI
 * at 100%, 30-odd at 200% - and a stretched fixed-size icon goes blurry or
 * blocky. They also take the theme's button text color (including high
 * contrast), which a fixed image cannot. The shapes are simple enough that
 * a few polygons describe them exactly.
 *
 * This file does only the arithmetic, with no GDI, so the tests can check
 * the pixels directly; main.c turns the result into an HICON.
 */
#ifndef MP_GLYPH_H
#define MP_GLYPH_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	MP_GLYPH_PREVIOUS = 0,
	MP_GLYPH_PLAY,
	MP_GLYPH_PAUSE,
	MP_GLYPH_STOP,
	MP_GLYPH_NEXT,
	MP_GLYPH_COUNT
} MpGlyph;

/* Large enough for a 400%-scaled window, and small enough that a caller's
 * size * size buffer can never be an absurd allocation. */
#define MP_GLYPH_MAX_SIZE 256

/*
 * Renders `glyph` into `out`: size * size pixels, top row first, each
 * 0xAARRGGBB with straight (not premultiplied) alpha, which is the layout
 * of a 32-bit top-down DIB and what a 32-bit alpha icon expects. `rgb` is
 * 0x00RRGGBB; its top byte is ignored. Fully transparent pixels are exactly
 * 0. Returns 1, or 0 (writing nothing) for a bad glyph, size or NULL out.
 */
int mp_glyph_render(MpGlyph glyph, int size, uint32_t rgb, uint32_t *out);

/*
 * Converts `count` 0xAARRGGBB pixels from straight to premultiplied alpha
 * in place (each color channel scaled by alpha / 255, rounded). Icons take
 * straight alpha, but a menu item's bitmap is drawn with AlphaBlend, which
 * expects premultiplied: given straight alpha, the anti-aliased edges would
 * come out too bright, a pale fringe around each symbol. NULL is a no-op.
 */
void mp_glyph_premultiply(uint32_t *pixels, size_t count);

#endif
