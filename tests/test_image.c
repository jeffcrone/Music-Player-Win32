/*
 * test_image.c - album art: scaling pictures into the thumbnail, decoding
 * real JPEG/PNG/GIF/BMP bytes, and finding art in a track's folder.
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "mptest.h"

#include "art.h"
#include "builders.h"
#include "image.h"

#define WHITE 0xFFFFFFu
#define RGB3(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

/*
 * Real image files, made with Pillow (Python) and embedded byte for byte:
 *
 *   im = Image.new("RGBA", (2, 2))
 *   im.putdata([(255,0,0,255), (0,255,0,255), (0,0,255,255), (0,0,0,0)])
 *   im.save(f, "PNG")                                         -> png_2x2
 *   Image.new("RGB", (16, 16), (240,120,20)).save(f, "JPEG", quality=95)
 *                                                             -> jpeg_orange
 *   im = Image.new("P", (1, 1)); im.putpalette([0,128,128] + [0]*765)
 *   im.save(f, "GIF")                                         -> gif_teal
 */
static const uint8_t png_2x2[77] = {
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
	0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
	0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00,
	0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
	0x1F, 0x0C, 0x19, 0x18, 0xFE, 0x83, 0x08, 0x06, 0x00, 0x3F, 0xD2, 0x05,
	0xFB, 0xDC, 0x02, 0xA3, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
	0x44, 0xAE, 0x42, 0x60, 0x82,
};
static const uint8_t jpeg_orange[635] = {
	0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
	0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
	0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06,
	0x07, 0x09, 0x08, 0x06, 0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0B, 0x08,
	0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x06, 0x08, 0x0B, 0x0C, 0x0B, 0x0A,
	0x0C, 0x09, 0x0A, 0x0A, 0x0A, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0A, 0x07, 0x06, 0x07,
	0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
	0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
	0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
	0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
	0x0A, 0x0A, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03,
	0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00,
	0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00,
	0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
	0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
	0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
	0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24,
	0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
	0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56,
	0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
	0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86,
	0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3,
	0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
	0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
	0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
	0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00,
	0x1F, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00,
	0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
	0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
	0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
	0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15,
	0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18,
	0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84,
	0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
	0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
	0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
	0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
	0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00,
	0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xF4,
	0x0A, 0x28, 0xA2, 0xBF, 0xCE, 0x73, 0xFD, 0x10, 0x3F, 0xFF, 0xD9,
};
static const uint8_t gif_teal[43] = {
	0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x01, 0x00, 0x01, 0x00, 0x81, 0x00,
	0x00, 0x00, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x08,
	0x04, 0x00, 0x01, 0x04, 0x04, 0x00, 0x3B,
};

/* Whether every pixel of `px` (n of them) is `want`. */
static int all_are(const uint32_t *px, size_t n, uint32_t want)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (px[i] != want)
			return 0;
	}
	return 1;
}

/* Fills a w x h RGBA buffer with one color. */
static uint8_t *solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint8_t *p = (uint8_t *)malloc((size_t)w * h * 4);
	int i;
	for (i = 0; i < w * h; i++) {
		p[i * 4] = r;
		p[i * 4 + 1] = g;
		p[i * 4 + 2] = b;
		p[i * 4 + 3] = a;
	}
	return p;
}

static int channel(uint32_t px, int shift)
{
	return (int)((px >> shift) & 0xFF);
}

/* Each channel of `got` within `tol` of `want`. (Not called `near`:
 * windows.h defines that as an empty macro, a leftover from 16-bit
 * Windows.) */
static int close_to(uint32_t got, uint32_t want, int tol)
{
	return abs(channel(got, 16) - channel(want, 16)) <= tol &&
		abs(channel(got, 8) - channel(want, 8)) <= tol &&
		abs(channel(got, 0) - channel(want, 0)) <= tol;
}

static void fit_arguments(void)
{
	uint8_t px[4] = { 1, 2, 3, 255 };
	uint32_t out[16];
	out[0] = 0xDEADBEEFu;
	CHECK(!mp_image_fit(NULL, 1, 1, 4, 0, out));
	CHECK(!mp_image_fit(px, 1, 1, 4, 0, NULL));
	CHECK(!mp_image_fit(px, 0, 1, 4, 0, out));
	CHECK(!mp_image_fit(px, 1, -1, 4, 0, out));
	CHECK(!mp_image_fit(px, 1, 1, 0, 0, out));
	CHECK(!mp_image_fit(px, 1, 1, MP_IMAGE_MAX_BOX + 1, 0, out));
	CHECK(out[0] == 0xDEADBEEFu); /* nothing written */
	CHECK(mp_image_fit(px, 1, 1, 1, 0, out));
	CHECK(out[0] == RGB3(1, 2, 3));
}

static void fit_solid_and_scaling(void)
{
	uint32_t out[64 * 64];
	uint8_t *p;

	/* Shrinking, enlarging and same size: a solid color stays exactly
	 * that color everywhere. */
	p = solid(100, 100, 10, 200, 30, 255);
	CHECK(mp_image_fit(p, 100, 100, 7, WHITE, out));
	CHECK(all_are(out, 49, RGB3(10, 200, 30)));
	CHECK(mp_image_fit(p, 100, 100, 64, WHITE, out));
	CHECK(all_are(out, 64 * 64, RGB3(10, 200, 30)));
	free(p);
	p = solid(3, 3, 10, 200, 30, 255);
	CHECK(mp_image_fit(p, 3, 3, 64, WHITE, out));
	CHECK(all_are(out, 64 * 64, RGB3(10, 200, 30)));
	CHECK(mp_image_fit(p, 3, 3, 3, WHITE, out));
	CHECK(all_are(out, 9, RGB3(10, 200, 30)));
	free(p);

	/* Averaging: a black and white checkerboard shrunk to one pixel is
	 * mid gray (127.5 rounds to 128), not black or white - which is what
	 * picking a single source pixel would give. */
	{
		uint8_t c[16] = {
			0, 0, 0, 255, 255, 255, 255, 255,
			255, 255, 255, 255, 0, 0, 0, 255
		};
		CHECK(mp_image_fit(c, 2, 2, 1, WHITE, out));
		CHECK(out[0] == RGB3(128, 128, 128));
	}

	/* A shrink by a ratio that is not a whole number keeps the overall
	 * brightness: the average of the output matches the input's. A
	 * horizontal ramp, 0..255 over 7 pixels, down to 3. */
	{
		uint8_t ramp[7 * 4];
		int i;
		double in_mean = 0, out_mean = 0;
		for (i = 0; i < 7; i++) {
			ramp[i * 4] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = (uint8_t)(i * 255 / 6);
			ramp[i * 4 + 3] = 255;
			in_mean += i * 255 / 6;
		}
		in_mean /= 7;
		CHECK(mp_image_fit(ramp, 7, 1, 3, 0, out));
		/* 7 x 1 into a 3-box: one row, in the middle row. */
		for (i = 0; i < 3; i++)
			out_mean += channel(out[3 + i], 0);
		out_mean /= 3;
		CHECK(out_mean > in_mean - 1.5 && out_mean < in_mean + 1.5);
		/* And it is still a ramp, darkest on the left. */
		CHECK(channel(out[3], 0) < channel(out[4], 0));
		CHECK(channel(out[4], 0) < channel(out[5], 0));
	}
}

static void fit_letterboxing(void)
{
	/* Room for the largest box used below (5 x 5). */
	uint32_t out[25];
	uint8_t *p;
	int x, y, bad = 0;

	/* Twice as wide as tall, into a 4-box: 4 x 2 in rows 1 and 2, the
	 * background above and below. */
	p = solid(40, 20, 255, 0, 0, 255);
	CHECK(mp_image_fit(p, 40, 20, 4, RGB3(1, 2, 3), out));
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			uint32_t want = (y == 1 || y == 2) ? RGB3(255, 0, 0) : RGB3(1, 2, 3);
			bad += out[y * 4 + x] != want;
		}
	}
	CHECK_INT(bad, 0);
	free(p);

	/* Twice as tall as wide: bands left and right instead. */
	p = solid(20, 40, 0, 0, 255, 255);
	CHECK(mp_image_fit(p, 20, 40, 4, WHITE, out));
	bad = 0;
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			uint32_t want = (x == 1 || x == 2) ? RGB3(0, 0, 255) : WHITE;
			bad += out[y * 4 + x] != want;
		}
	}
	CHECK_INT(bad, 0);
	free(p);

	/* Very wide: still at least one pixel tall, centered. */
	p = solid(1000, 10, 9, 9, 9, 255);
	CHECK(mp_image_fit(p, 1000, 10, 5, WHITE, out));
	CHECK(out[2 * 5 + 0] == RGB3(9, 9, 9));
	CHECK(out[0] == WHITE);
	free(p);
}

static void fit_transparency(void)
{
	uint32_t out[4];
	uint8_t *p;

	/* Fully transparent: just the background. */
	p = solid(4, 4, 255, 0, 0, 0);
	CHECK(mp_image_fit(p, 4, 4, 2, RGB3(7, 8, 9), out));
	CHECK(all_are(out, 4, RGB3(7, 8, 9)));
	free(p);

	/* Half-transparent red on white: pink. 255 * (1 - 128/255) = 127. */
	p = solid(2, 2, 255, 0, 0, 128);
	CHECK(mp_image_fit(p, 2, 2, 2, WHITE, out));
	CHECK(close_to(out[0], RGB3(255, 127, 127), 1));
	free(p);

	/* An opaque red pixel beside a transparent *green* one, averaged into
	 * one: half red, half background. The transparent pixel's green must
	 * not leak in (it would, without premultiplying). */
	{
		uint8_t two[8] = { 255, 0, 0, 255, 0, 255, 0, 0 };
		CHECK(mp_image_fit(two, 2, 1, 1, WHITE, out));
		CHECK(out[0] == RGB3(255, 128, 128));
	}
}

static void thumbnail_decodes_formats(void)
{
	uint32_t *px;

	/* PNG with transparency, exact: one output pixel per source pixel. */
	px = mp_image_thumbnail(png_2x2, sizeof(png_2x2), 2, RGB3(1, 2, 3));
	CHECK(px != NULL);
	if (px != NULL) {
		CHECK(px[0] == RGB3(255, 0, 0));
		CHECK(px[1] == RGB3(0, 255, 0));
		CHECK(px[2] == RGB3(0, 0, 255));
		CHECK(px[3] == RGB3(1, 2, 3)); /* the transparent one */
		free(px);
	}

	/* JPEG is lossy: the solid orange comes back within a few levels. */
	px = mp_image_thumbnail(jpeg_orange, sizeof(jpeg_orange), 4, WHITE);
	CHECK(px != NULL);
	if (px != NULL) {
		int i, ok = 1;
		for (i = 0; i < 16; i++)
			ok &= close_to(px[i], RGB3(240, 120, 20), 4);
		CHECK(ok);
		free(px);
	}

	/* GIF, enlarged from 1 pixel. */
	px = mp_image_thumbnail(gif_teal, sizeof(gif_teal), 3, WHITE);
	CHECK(px != NULL);
	if (px != NULL) {
		CHECK(all_are(px, 9, RGB3(0, 128, 128)));
		free(px);
	}

	/* BMP, built by hand: 2 x 1, 24-bit, bottom-up, blue-green-red order,
	 * rows padded to 4 bytes. */
	{
		Buf b;
		buf_init(&b);
		buf_str(&b, "BM");
		buf_le32(&b, 14 + 40 + 8); /* file size */
		buf_le32(&b, 0);
		buf_le32(&b, 14 + 40);     /* where the pixels start */
		buf_le32(&b, 40);          /* BITMAPINFOHEADER */
		buf_le32(&b, 2);
		buf_le32(&b, 1);
		buf_le16(&b, 1);
		buf_le16(&b, 24);
		buf_le32(&b, 0);           /* BI_RGB */
		buf_le32(&b, 8);
		buf_le32(&b, 2835);
		buf_le32(&b, 2835);
		buf_le32(&b, 0);
		buf_le32(&b, 0);
		buf_u8(&b, 0x30); buf_u8(&b, 0x20); buf_u8(&b, 0x10); /* (16, 32, 48) */
		buf_u8(&b, 0x00); buf_u8(&b, 0xFF); buf_u8(&b, 0x00); /* green */
		buf_zeros(&b, 2);
		px = mp_image_thumbnail(b.data, b.len, 2, WHITE);
		CHECK(px != NULL);
		if (px != NULL) {
			/* Wider than tall: one row of pixels, in the box's top row
			 * (centering 1 row in 2 rounds down to row 0). */
			CHECK(px[0] == RGB3(16, 32, 48));
			CHECK(px[1] == RGB3(0, 255, 0));
			CHECK(px[2] == WHITE);
			free(px);
		}
		buf_free(&b);
	}
}

static void thumbnail_refuses_bad_input(void)
{
	static const uint8_t garbage[] = "this is not a picture, just some text";
	Buf b;
	/* Not a picture, empty, NULL. */
	CHECK(mp_image_thumbnail(garbage, sizeof(garbage), 8, WHITE) == NULL);
	CHECK(mp_image_thumbnail(png_2x2, 0, 8, WHITE) == NULL);
	CHECK(mp_image_thumbnail(NULL, 100, 8, WHITE) == NULL);
	/* Truncated part way through the image data. */
	CHECK(mp_image_thumbnail(png_2x2, 40, 8, WHITE) == NULL);
	/* A JPEG cut in half may still decode (stb_image fills in what is
	 * missing); all that matters is that it does not crash. */
	free(mp_image_thumbnail(jpeg_orange, sizeof(jpeg_orange) / 2, 8, WHITE));
	/* Box out of range. */
	CHECK(mp_image_thumbnail(png_2x2, sizeof(png_2x2), 0, WHITE) == NULL);
	CHECK(mp_image_thumbnail(png_2x2, sizeof(png_2x2), MP_IMAGE_MAX_BOX + 1, WHITE) == NULL);
	/* Over the size limit: refused before a byte is read (so a short
	 * buffer with a huge claimed size is safe here). */
	CHECK(mp_image_thumbnail(png_2x2, (size_t)MP_IMAGE_MAX_BYTES + 1, 8, WHITE) == NULL);

	/* A PNG header claiming 10000 x 10000 pixels (400 MB decoded) is
	 * refused from the header alone, before allocating anything. */
	buf_init(&b);
	buf_bytes(&b, png_2x2, 16);  /* signature + IHDR length and type */
	buf_be32(&b, 10000);
	buf_be32(&b, 10000);
	buf_bytes(&b, png_2x2 + 24, sizeof(png_2x2) - 24);
	CHECK(mp_image_thumbnail(b.data, b.len, 8, WHITE) == NULL);
	buf_free(&b);
}

/* ---- Folder art --------------------------------------------------------- */

/* A fresh, empty folder in %TEMP%. */
static int make_dir(wchar_t *dir, size_t cap)
{
	static unsigned counter;
	wchar_t tmp[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, tmp))
		return 0;
	if (_snwprintf(dir, cap, L"%lsmp_art_%lu_%u", tmp, (unsigned long)GetCurrentProcessId(), ++counter) < 0)
		return 0;
	return CreateDirectoryW(dir, NULL) != 0;
}

static int put(const wchar_t *dir, const wchar_t *name, const void *data, size_t n)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE h;
	DWORD written = 0;
	_snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", dir, name);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	WriteFile(h, data, (DWORD)n, &written, NULL);
	CloseHandle(h);
	return written == n;
}

static void del(const wchar_t *dir, const wchar_t *name)
{
	wchar_t path[MAX_PATH * 2];
	_snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", dir, name);
	DeleteFileW(path);
}

/* Whether *data / *size hold exactly `want`. Frees *data. */
static int got_bytes(uint8_t **data, size_t *size, const char *want)
{
	int same = *data != NULL && *size == strlen(want) && memcmp(*data, want, *size) == 0;
	free(*data);
	*data = NULL;
	return same;
}

static void folder_art(void)
{
	wchar_t dir[MAX_PATH], track[MAX_PATH * 2];
	uint8_t *data = NULL;
	size_t size = 0;
	Buf mp3, tag, frames, apic;
	if (!make_dir(dir, MAX_PATH)) {
		CHECK(!"could not make a temp folder");
		return;
	}
	/* A track with no art of its own. */
	buf_init(&mp3);
	mp3_silence(&mp3, 2);
	put(dir, L"song.mp3", mp3.data, mp3.len);
	_snwprintf(track, MAX_PATH * 2, L"%ls\\song.mp3", dir);

	/* Nothing in the folder: no art. */
	CHECK(!mp_art_load(track, &data, &size));
	CHECK(data == NULL);

	/* WMP's leftovers are a last resort... */
	put(dir, L"AlbumArtSmall.jpg", "small", 5);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "small"));
	put(dir, L"AlbumArt_{0A1B}_Large.jpg", "large", 5);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "large"));
	/* ...beaten by the plain names, in their order. The case of the name
	 * does not matter. */
	put(dir, L"ALBUM.PNG", "album", 5);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "album"));
	put(dir, L"Folder.jpeg", "folder", 6);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "folder"));
	put(dir, L"cover.jpg", "cover", 5);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "cover"));
	/* An empty file is no art; the next name is tried. */
	put(dir, L"cover.jpg", "", 0);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "folder"));

	/* Art inside the file beats anything in the folder. */
	buf_init(&tag);
	buf_init(&frames);
	buf_init(&apic);
	buf_u8(&apic, 0);
	buf_str(&apic, "image/jpeg");
	buf_u8(&apic, 0);
	buf_u8(&apic, 3);
	buf_u8(&apic, 0);
	buf_str(&apic, "embedded");
	id3_raw_frame(&frames, 3, "APIC", 0, apic.data, apic.len);
	id3_tag(&tag, 3, 0, &frames, 0);
	buf_bytes(&tag, mp3.data, mp3.len);
	put(dir, L"song.mp3", tag.data, tag.len);
	CHECK(mp_art_load(track, &data, &size));
	CHECK(got_bytes(&data, &size, "embedded"));

	/* Bad paths. */
	CHECK(!mp_art_find_folder_image(NULL, &data, &size));
	CHECK(!mp_art_find_folder_image(L"", &data, &size));
	/* No folder part at all. */
	CHECK(!mp_art_find_folder_image(L"song.mp3", &data, &size));

	del(dir, L"song.mp3");
	del(dir, L"AlbumArtSmall.jpg");
	del(dir, L"AlbumArt_{0A1B}_Large.jpg");
	del(dir, L"ALBUM.PNG");
	del(dir, L"Folder.jpeg");
	del(dir, L"cover.jpg");
	CHECK(RemoveDirectoryW(dir));
	buf_free(&mp3);
	buf_free(&tag);
	buf_free(&frames);
	buf_free(&apic);
}

int suite_image(void)
{
	fit_arguments();
	fit_solid_and_scaling();
	fit_letterboxing();
	fit_transparency();
	thumbnail_decodes_formats();
	thumbnail_refuses_bad_input();
	folder_art();
	return 0;
}
