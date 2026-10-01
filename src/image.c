/*
 * image.c - album art decoding and scaling. See image.h.
 */
#include "image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image_config.h"

/*
 * How one output pixel along an axis is made from the source: the source
 * pixels it covers, starting at `first`, and the share of the output pixel
 * each one covers (weights adding up to 1). Output pixel i covers the
 * source span [i * src / dst, (i + 1) * src / dst).
 */
typedef struct {
	int first, count;
	float *w;
} Span;

/* Builds the spans for scaling `src` pixels to `dst`. `weights` must have
 * room for src + 2 * dst entries, which is the most the spans can need
 * (each source pixel appears in at most two spans, except when enlarging,
 * where each output pixel covers at most two source pixels). */
static void make_spans(int src, int dst, Span *spans, float *weights)
{
	int i, j;
	double scale = (double)src / (double)dst;
	for (i = 0; i < dst; i++) {
		double a = i * scale, b = (i + 1) * scale;
		int first = (int)floor(a), last = (int)ceil(b) - 1;
		if (last >= src)
			last = src - 1;
		if (first > last)
			first = last;
		spans[i].first = first;
		spans[i].count = last - first + 1;
		spans[i].w = weights;
		for (j = first; j <= last; j++) {
			double lo = j > a ? j : a, hi = j + 1 < b ? j + 1 : b;
			*weights++ = (float)((hi - lo) / (b - a));
		}
	}
}

static uint32_t to_byte(float v)
{
	if (v <= 0.0f)
		return 0;
	if (v >= 255.0f)
		return 255;
	return (uint32_t)(v + 0.5f);
}

int mp_image_fit(const uint8_t *rgba, int w, int h, int box, uint32_t bg_rgb, uint32_t *out)
{
	double scale;
	int dw, dh, ox, oy, x, y, k;
	Span *xs, *ys;
	float *xw, *yw, *row;
	float bg[3];
	if (rgba == NULL || out == NULL || w <= 0 || h <= 0 || box <= 0 || box > MP_IMAGE_MAX_BOX)
		return 0;

	/* The picture's size inside the box: as big as fits, same proportions,
	 * at least a pixel each way. */
	scale = (double)box / (w > h ? w : h);
	dw = (int)(w * scale + 0.5);
	dh = (int)(h * scale + 0.5);
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	if (dw > box)
		dw = box;
	if (dh > box)
		dh = box;
	ox = (box - dw) / 2;
	oy = (box - dh) / 2;

	xs = (Span *)malloc((size_t)dw * sizeof(Span));
	ys = (Span *)malloc((size_t)dh * sizeof(Span));
	xw = (float *)malloc(((size_t)w + 2 * (size_t)dw) * sizeof(float));
	yw = (float *)malloc(((size_t)h + 2 * (size_t)dh) * sizeof(float));
	/* One row of the source, already averaged vertically for the output
	 * row being made, as premultiplied R, G, B and alpha. Working a row at
	 * a time keeps the memory small (a 8192-wide picture needs 128 KB
	 * here) instead of a whole intermediate image. */
	row = (float *)malloc((size_t)w * 4 * sizeof(float));
	if (xs == NULL || ys == NULL || xw == NULL || yw == NULL || row == NULL) {
		free(xs);
		free(ys);
		free(xw);
		free(yw);
		free(row);
		return 0;
	}
	make_spans(w, dw, xs, xw);
	make_spans(h, dh, ys, yw);

	bg[0] = (float)((bg_rgb >> 16) & 0xFF);
	bg[1] = (float)((bg_rgb >> 8) & 0xFF);
	bg[2] = (float)(bg_rgb & 0xFF);
	for (k = 0; k < box * box; k++)
		out[k] = bg_rgb & 0x00FFFFFFu;

	for (y = 0; y < dh; y++) {
		const Span *ys_ = &ys[y];
		memset(row, 0, (size_t)w * 4 * sizeof(float));
		for (k = 0; k < ys_->count; k++) {
			const uint8_t *src = rgba + (size_t)(ys_->first + k) * (size_t)w * 4;
			float wt = ys_->w[k];
			for (x = 0; x < w; x++) {
				/* Premultiplied, so a transparent pixel's (meaningless)
				 * color cannot bleed into its opaque neighbors. */
				float a = src[x * 4 + 3] * (1.0f / 255.0f) * wt;
				row[x * 4 + 0] += src[x * 4 + 0] * a;
				row[x * 4 + 1] += src[x * 4 + 1] * a;
				row[x * 4 + 2] += src[x * 4 + 2] * a;
				row[x * 4 + 3] += a;
			}
		}
		for (x = 0; x < dw; x++) {
			const Span *xs_ = &xs[x];
			float r = 0, g = 0, b = 0, a = 0;
			for (k = 0; k < xs_->count; k++) {
				const float *p = row + (size_t)(xs_->first + k) * 4;
				float wt = xs_->w[k];
				r += p[0] * wt;
				g += p[1] * wt;
				b += p[2] * wt;
				a += p[3] * wt;
			}
			/* Onto the background: what shows through is (1 - a). */
			out[(size_t)(oy + y) * box + (size_t)(ox + x)] =
				to_byte(r + bg[0] * (1.0f - a)) << 16 |
				to_byte(g + bg[1] * (1.0f - a)) << 8 |
				to_byte(b + bg[2] * (1.0f - a));
		}
	}
	free(xs);
	free(ys);
	free(xw);
	free(yw);
	free(row);
	return 1;
}

uint32_t *mp_image_thumbnail(const uint8_t *data, size_t size, int box, uint32_t bg_rgb)
{
	int w = 0, h = 0, channels = 0;
	uint8_t *rgba;
	uint32_t *out;
	if (data == NULL || size == 0 || size > MP_IMAGE_MAX_BYTES || box <= 0 || box > MP_IMAGE_MAX_BOX)
		return NULL;
	/* Always 4 channels out, whatever the file has: gray, gray + alpha,
	 * RGB and RGBA all arrive as RGBA. */
	rgba = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
	if (rgba == NULL)
		return NULL;
	out = (uint32_t *)malloc((size_t)box * (size_t)box * sizeof(uint32_t));
	if (out != NULL && !mp_image_fit(rgba, w, h, box, bg_rgb, out)) {
		free(out);
		out = NULL;
	}
	stbi_image_free(rgba);
	return out;
}
