/*
 * glyph.c - the playback symbols. See glyph.h for why they are drawn here.
 *
 * Each symbol is one or two convex polygons on a 16 x 16 design grid (the
 * size of a small icon at 100%), scaled to the requested pixel size.
 * Anti-aliasing is by supersampling: each pixel is tested at a 4 x 4 grid of
 * points, and the fraction inside becomes its alpha. Sixteen levels is
 * plenty at these sizes, and it needs nothing beyond point-in-polygon tests
 * - no GDI+, which XP only has as an optional extra.
 *
 * All of it is exact integer arithmetic. With floating point, a sample that
 * lands exactly on a diagonal edge can round inside on one side of the
 * symbol and outside on the other, so Previous stops being a perfect mirror
 * of Next (the tests caught exactly that). Every corner is on a whole grid
 * unit, so measuring in steps of 1 / (8 * size) of a grid unit makes every
 * sample point and every corner an integer, and the edge test exact.
 */
#include "glyph.h"

#include <stddef.h>

#define GRID 16
#define SUBSAMPLES 4

typedef struct {
	int n;               /* 3 or 4 points, 0 = unused */
	int pt[4][2];        /* x, y on the 16 x 16 grid */
} Poly;

typedef struct {
	int mirror;          /* draw another glyph flipped left to right */
	Poly poly[2];
} Shape;

/*
 * Everything keeps two grid units clear on every side, so the symbols have
 * the same margins as a system icon and never touch the edge pixels.
 * Previous is drawn as a mirrored Next, so the pair is exactly symmetric.
 * The play triangle is centered on its bounding box, not on its area,
 * which puts it a little right of the middle: that is where it looks
 * centered, and it is how media players draw it.
 */
static const Shape shapes[MP_GLYPH_COUNT] = {
	/* PREVIOUS: see NEXT */
	{ 1, { { 3, { { 3, 2 }, { 11, 8 }, { 3, 14 } } },
	       { 4, { { 11, 2 }, { 13, 2 }, { 13, 14 }, { 11, 14 } } } } },
	/* PLAY: a right-pointing triangle */
	{ 0, { { 3, { { 4, 2 }, { 14, 8 }, { 4, 14 } } },
	       { 0, { { 0, 0 } } } } },
	/* PAUSE: two bars */
	{ 0, { { 4, { { 3, 2 }, { 7, 2 }, { 7, 14 }, { 3, 14 } } },
	       { 4, { { 9, 2 }, { 13, 2 }, { 13, 14 }, { 9, 14 } } } } },
	/* STOP: a square */
	{ 0, { { 4, { { 3, 3 }, { 13, 3 }, { 13, 13 }, { 3, 13 } } },
	       { 0, { { 0, 0 } } } } },
	/* NEXT: a triangle running into a bar */
	{ 0, { { 3, { { 3, 2 }, { 11, 8 }, { 3, 14 } } },
	       { 4, { { 11, 2 }, { 13, 2 }, { 13, 14 }, { 11, 14 } } } } },
};

/* Inside (or on the edge of) a convex polygon of either winding: the point
 * is on the same side of every edge. (x, y) are in steps of 1 / `unit` of
 * a grid unit, and the corners are scaled to match. The largest values are
 * 16 * 8 * 256 = 2^15, so the products fit easily in 64 bits. */
static int inside(const Poly *poly, int64_t unit, int64_t x, int64_t y)
{
	int i, pos = 0, neg = 0;
	for (i = 0; i < poly->n; i++) {
		const int *a = poly->pt[i], *b = poly->pt[(i + 1) % poly->n];
		int64_t ax = a[0] * unit, ay = a[1] * unit, bx = b[0] * unit, by = b[1] * unit;
		int64_t cross = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
		if (cross > 0)
			pos = 1;
		else if (cross < 0)
			neg = 1;
	}
	return !(pos && neg);
}

int mp_glyph_render(MpGlyph glyph, int size, uint32_t rgb, uint32_t *out)
{
	const Shape *shape;
	int64_t unit;
	int x, y, sx, sy, k;
	if (out == NULL || size <= 0 || size > MP_GLYPH_MAX_SIZE || (int)glyph < 0 || glyph >= MP_GLYPH_COUNT)
		return 0;
	shape = &shapes[glyph];
	/* Sample (sx, sy) of pixel (x, y) is at grid position
	 * (x + (sx + 0.5) / 4) * GRID / size, which is (8x + 2sx + 1) * GRID
	 * over a denominator of 8 * size: so work in units of 1 / (8 * size). */
	unit = (int64_t)2 * SUBSAMPLES * size;
	rgb &= 0x00FFFFFFu;
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			int covered = 0;
			uint32_t alpha;
			for (sy = 0; sy < SUBSAMPLES; sy++) {
				for (sx = 0; sx < SUBSAMPLES; sx++) {
					/* Sample points sit at the centers of the sub-cells,
					 * so none lands on a pixel edge. */
					int64_t gx = ((int64_t)2 * SUBSAMPLES * x + 2 * sx + 1) * GRID;
					int64_t gy = ((int64_t)2 * SUBSAMPLES * y + 2 * sy + 1) * GRID;
					if (shape->mirror)
						gx = GRID * unit - gx;
					/* A sample counts once even where polygons meet (the
					 * Next triangle's tip touches its bar). */
					for (k = 0; k < 2; k++) {
						if (shape->poly[k].n > 0 && inside(&shape->poly[k], unit, gx, gy)) {
							covered++;
							break;
						}
					}
				}
			}
			alpha = ((uint32_t)covered * 255u + SUBSAMPLES * SUBSAMPLES / 2) / (SUBSAMPLES * SUBSAMPLES);
			out[(size_t)y * size + x] = alpha ? (alpha << 24) | rgb : 0;
		}
	}
	return 1;
}

void mp_glyph_premultiply(uint32_t *pixels, size_t count)
{
	size_t i;
	if (pixels == NULL)
		return;
	for (i = 0; i < count; i++) {
		uint32_t p = pixels[i], a = p >> 24;
		uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
		/* +127 rounds to nearest; the result never exceeds a, which is
		 * what premultiplied data must satisfy. Opaque and fully
		 * transparent pixels come out unchanged apart from zeroing the
		 * color of the transparent ones. */
		r = (r * a + 127) / 255;
		g = (g * a + 127) / 255;
		b = (b * a + 127) / 255;
		pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
	}
}
