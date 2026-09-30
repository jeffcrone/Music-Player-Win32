/*
 * test_glyph.c - the playback buttons' symbols, checked pixel by pixel.
 *
 * At 16 px one design-grid unit is exactly one pixel, so the straight-edged
 * shapes (stop, pause) come out with exact, predictable pixels; the other
 * sizes are checked through properties that must hold at any scale:
 * margins, symmetry, and area.
 */
#include <stdint.h>
#include <stdlib.h>

#include "mptest.h"

#include "glyph.h"

#define RGB_TEST 0x123456u

static uint32_t px[MP_GLYPH_MAX_SIZE * MP_GLYPH_MAX_SIZE];

static uint32_t alpha_at(int size, int x, int y)
{
	return px[y * size + x] >> 24;
}

/* Total coverage in whole pixels (sum of alpha / 255). */
static double area(int size)
{
	int i;
	double sum = 0;
	for (i = 0; i < size * size; i++)
		sum += (double)(px[i] >> 24) / 255.0;
	return sum;
}

static void bad_arguments(void)
{
	px[0] = 0xDEADBEEFu;
	CHECK_INT(mp_glyph_render(MP_GLYPH_PLAY, 16, 0, NULL), 0);
	CHECK_INT(mp_glyph_render(MP_GLYPH_PLAY, 0, 0, px), 0);
	CHECK_INT(mp_glyph_render(MP_GLYPH_PLAY, -16, 0, px), 0);
	CHECK_INT(mp_glyph_render(MP_GLYPH_PLAY, MP_GLYPH_MAX_SIZE + 1, 0, px), 0);
	CHECK_INT(mp_glyph_render(MP_GLYPH_COUNT, 16, 0, px), 0);
	CHECK_INT(mp_glyph_render((MpGlyph)-1, 16, 0, px), 0);
	/* A refused call writes nothing. */
	CHECK(px[0] == 0xDEADBEEFu);
	/* The limits themselves are fine. */
	CHECK_INT(mp_glyph_render(MP_GLYPH_STOP, 1, 0, px), 1);
	CHECK_INT(mp_glyph_render(MP_GLYPH_STOP, MP_GLYPH_MAX_SIZE, 0, px), 1);
}

static void stop_is_exact_at_16(void)
{
	int x, y, bad = 0;
	CHECK_INT(mp_glyph_render(MP_GLYPH_STOP, 16, RGB_TEST, px), 1);
	/* The square covers grid 3..13, which is pixels 3..12 exactly. */
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			int in = x >= 3 && x <= 12 && y >= 3 && y <= 12;
			if (px[y * 16 + x] != (in ? 0xFF000000u | RGB_TEST : 0u))
				bad++;
		}
	}
	CHECK_INT(bad, 0);
}

static void pause_is_exact_at_16(void)
{
	int x, y, bad = 0;
	CHECK_INT(mp_glyph_render(MP_GLYPH_PAUSE, 16, RGB_TEST, px), 1);
	/* Bars on pixels 3..6 and 9..12, rows 2..13, with a clear 2 px gap. */
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			int in = y >= 2 && y <= 13 && ((x >= 3 && x <= 6) || (x >= 9 && x <= 12));
			if (px[y * 16 + x] != (in ? 0xFF000000u | RGB_TEST : 0u))
				bad++;
		}
	}
	CHECK_INT(bad, 0);
}

static void color_is_applied(void)
{
	int i, bad = 0, opaque = 0;
	/* The top byte of rgb is ignored; it must not leak into the alpha. */
	CHECK_INT(mp_glyph_render(MP_GLYPH_PLAY, 24, 0xFFABCDEFu, px), 1);
	for (i = 0; i < 24 * 24; i++) {
		if (px[i] == 0)
			continue;
		/* Every visible pixel carries exactly the color... */
		if ((px[i] & 0x00FFFFFFu) != 0xABCDEFu)
			bad++;
		if ((px[i] >> 24) == 255)
			opaque++;
	}
	CHECK_INT(bad, 0);
	/* ...and the solid middle of the triangle really is solid. */
	CHECK(opaque > 24 * 24 / 8);
	/* Black works too: visible pixels are then pure alpha. */
	CHECK_INT(mp_glyph_render(MP_GLYPH_STOP, 16, 0, px), 1);
	CHECK(px[8 * 16 + 8] == 0xFF000000u);
}

/* The sizes the window actually asks for (the message font height at
 * 100%..300% with Tahoma and Segoe UI, less an eighth) and some awkward ones. */
static const int sizes[] = { 1, 2, 7, 8, 11, 12, 13, 14, 15, 16, 17, 20, 23, 24, 31, 32, 40, 48, 64, 100 };
#define NSIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

static void edges_stay_clear(void)
{
	int s, gl, i, bad = 0;
	/* Every symbol keeps a margin, so no edge pixel is ever touched (from
	 * 8 px up; below that a pixel is wider than the margin). */
	for (gl = 0; gl < MP_GLYPH_COUNT; gl++) {
		for (s = 0; s < NSIZES; s++) {
			int n = sizes[s];
			if (n < 8)
				continue;
			mp_glyph_render((MpGlyph)gl, n, RGB_TEST, px);
			for (i = 0; i < n; i++) {
				if (alpha_at(n, i, 0) || alpha_at(n, i, n - 1) || alpha_at(n, 0, i) || alpha_at(n, n - 1, i))
					bad++;
			}
		}
	}
	CHECK_INT(bad, 0);
}

static void previous_mirrors_next(void)
{
	static uint32_t next[MP_GLYPH_MAX_SIZE * MP_GLYPH_MAX_SIZE];
	int s, x, y, bad = 0;
	for (s = 0; s < NSIZES; s++) {
		int n = sizes[s];
		mp_glyph_render(MP_GLYPH_NEXT, n, RGB_TEST, next);
		mp_glyph_render(MP_GLYPH_PREVIOUS, n, RGB_TEST, px);
		for (y = 0; y < n; y++) {
			for (x = 0; x < n; x++) {
				if (px[y * n + x] != next[y * n + (n - 1 - x)])
					bad++;
			}
		}
	}
	CHECK_INT(bad, 0);
}

static void top_bottom_symmetry(void)
{
	int s, gl, x, y, bad = 0;
	/* All five shapes are symmetric top to bottom; an asymmetric result
	 * would mean an off-by-one in the sampling. */
	for (gl = 0; gl < MP_GLYPH_COUNT; gl++) {
		for (s = 0; s < NSIZES; s++) {
			int n = sizes[s];
			mp_glyph_render((MpGlyph)gl, n, RGB_TEST, px);
			for (y = 0; y < n / 2; y++) {
				for (x = 0; x < n; x++) {
					if (px[y * n + x] != px[(n - 1 - y) * n + x])
						bad++;
				}
			}
		}
	}
	CHECK_INT(bad, 0);
}

static void areas_scale(void)
{
	int s;
	/* Coverage matches the true area to within a pixel's worth along each
	 * edge (the edges are where anti-aliasing rounds). On the 16 grid:
	 * play 10 x 12 / 2 = 60, stop 100, pause 2 x 48 = 96, next 48 + 24 =
	 * 72. */
	for (s = 0; s < NSIZES; s++) {
		int n = sizes[s];
		double k = (double)n * n / 256.0, slack = 0.5 * n + 1;
		double a;
		mp_glyph_render(MP_GLYPH_PLAY, n, 0, px);
		a = area(n);
		CHECK(a > 60 * k - slack && a < 60 * k + slack);
		mp_glyph_render(MP_GLYPH_STOP, n, 0, px);
		a = area(n);
		CHECK(a > 100 * k - slack && a < 100 * k + slack);
		mp_glyph_render(MP_GLYPH_PAUSE, n, 0, px);
		a = area(n);
		CHECK(a > 96 * k - slack && a < 96 * k + slack);
		mp_glyph_render(MP_GLYPH_NEXT, n, 0, px);
		a = area(n);
		CHECK(a > 72 * k - slack && a < 72 * k + slack);
	}
}

static void play_points_right(void)
{
	int y, n = 32;
	/* Rows narrow toward the tip: the middle row is the widest, and the
	 * triangle's flat side is on the left. */
	mp_glyph_render(MP_GLYPH_PLAY, n, 0, px);
	CHECK_INT(alpha_at(n, 9, 16), 255);   /* just inside the flat side */
	CHECK_INT(alpha_at(n, 25, 16), 255);  /* near the tip, on the axis */
	CHECK_INT(alpha_at(n, 26, 6), 0);     /* near the tip, off the axis */
	CHECK_INT(alpha_at(n, 7, 16), 0);     /* left of the flat side */
	for (y = 5; y < 16; y++) {
		/* Each row down to the middle reaches at least as far right. */
		int x, w_this = 0, w_next = 0;
		for (x = 0; x < n; x++) {
			w_this += alpha_at(n, x, y) > 0;
			w_next += alpha_at(n, x, y + 1) > 0;
		}
		CHECK(w_next >= w_this);
	}
}

static void anti_aliased_edges(void)
{
	int i, partial = 0;
	/* The diagonal edges of play get in-between alphas; without them the
	 * symbol would look jagged at small sizes. */
	mp_glyph_render(MP_GLYPH_PLAY, 16, 0, px);
	for (i = 0; i < 256; i++) {
		uint32_t a = px[i] >> 24;
		if (a > 0 && a < 255)
			partial++;
	}
	CHECK(partial >= 10);
}

static void premultiply(void)
{
	uint32_t p[] = {
		0xFF123456u, /* opaque: unchanged */
		0x00FFFFFFu, /* transparent: color dropped too */
		0x80FFFFFFu, /* half-transparent white: 255 * 128 / 255 = 128 */
		0x80FF8000u, /* 255 -> 128, 128 -> 64 (64.25), 0 -> 0 */
		0x01FFFFFFu, /* nearly transparent: 255 * 1 / 255 = 1 */
		0x40010101u, /* dark: 1 * 64 / 255 = 0.25 -> 0 */
		0x7F030303u, /* 3 * 127 / 255 = 1.494 -> 1 (rounds to nearest) */
		0x00000000u,
	};
	int i, bad = 0;
	mp_glyph_premultiply(p, 8);
	CHECK(p[0] == 0xFF123456u);
	CHECK(p[1] == 0x00000000u);
	CHECK(p[2] == 0x80808080u);
	CHECK(p[3] == 0x80804000u);
	CHECK(p[4] == 0x01010101u);
	CHECK(p[5] == 0x40000000u);
	CHECK(p[6] == 0x7F010101u);
	CHECK(p[7] == 0x00000000u);

	/* Only `count` pixels are touched, and NULL is harmless. */
	p[0] = 0x80FFFFFFu;
	p[1] = 0x80FFFFFFu;
	mp_glyph_premultiply(p, 1);
	CHECK(p[0] == 0x80808080u);
	CHECK(p[1] == 0x80FFFFFFu);
	mp_glyph_premultiply(p, 0);
	CHECK(p[1] == 0x80FFFFFFu);
	mp_glyph_premultiply(NULL, 5);

	/* On a real rendered symbol in white (the worst case), no channel ever
	 * ends up above its alpha, which AlphaBlend requires, and the alpha
	 * itself is never changed. */
	mp_glyph_render(MP_GLYPH_PLAY, 24, 0xFFFFFFu, px);
	{
		static uint32_t before[24 * 24];
		memcpy(before, px, sizeof(before));
		mp_glyph_premultiply(px, 24 * 24);
		for (i = 0; i < 24 * 24; i++) {
			uint32_t a = px[i] >> 24;
			if (((px[i] >> 16) & 0xFF) > a || ((px[i] >> 8) & 0xFF) > a || (px[i] & 0xFF) > a)
				bad++;
			if (a != before[i] >> 24)
				bad++;
		}
	}
	CHECK_INT(bad, 0);

	/* Every possible alpha against full white: exactly the alpha back. */
	bad = 0;
	for (i = 0; i < 256; i++) {
		uint32_t q = ((uint32_t)i << 24) | 0xFFFFFFu;
		mp_glyph_premultiply(&q, 1);
		if (q != ((uint32_t)i * 0x01010101u))
			bad++;
	}
	CHECK_INT(bad, 0);
}

int suite_glyph(void)
{
	bad_arguments();
	stop_is_exact_at_16();
	pause_is_exact_at_16();
	color_is_applied();
	edges_stay_clear();
	previous_mirrors_next();
	top_bottom_symmetry();
	areas_scale();
	play_points_right();
	anti_aliased_edges();
	premultiply();
	return 0;
}
