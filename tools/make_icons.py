#!/usr/bin/env python3
"""Generate the Music Player icon set (SVG, PNGs and a Windows .ico).

The icon is a single eighth note. Its geometry is defined once, below, and
every output is produced from that definition, so the SVG and the raster
images cannot drift apart.

Usage:
	python tools/make_icons.py [output_dir]

output_dir defaults to the repository's Music_Icons folder. Requires Pillow
(pip install Pillow).

Why the .ico is written by hand instead of with Pillow's ICO writer:
Pillow stores every image in an .ico as PNG data. Windows Vista and later
understand PNG-compressed icon images, but Windows XP does not - it only
reads the classic DIB (BITMAPINFOHEADER) format and shows a blank or broken
icon otherwise. Since the app has to run on XP, every image in our .ico is
an uncompressed 32-bit DIB with an alpha channel plus a 1-bit AND mask. The
256x256 entry costs about 256 KB that way, which is an acceptable price.
"""

import math
import os
import struct
import sys

from PIL import Image, ImageDraw

# --- The design, in a 256 x 256 coordinate space ---------------------------

VIEW = 256

# Note head: an ellipse tilted the way handwritten and engraved notes are.
HEAD_CX, HEAD_CY = 100.0, 184.0
HEAD_RX, HEAD_RY = 54.0, 38.0
HEAD_ROTATE_DEG = -22.0

# Stem: a plain rectangle whose right edge meets the rightmost point of the
# tilted head (x = 152), so the two read as one shape.
STEM_X0, STEM_X1 = 134.0, 152.0
STEM_Y0, STEM_Y1 = 32.0, 176.0

# Flag: starts at the top-right of the stem, sweeps out to a tip, and curves
# back to the stem. Two cubic Bezier segments, then a straight line back up
# the stem (hidden inside the stem, so it never shows).
FLAG_START = (152.0, 32.0)
FLAG_CURVES = [
	((160.0, 74.0), (222.0, 92.0), (206.0, 156.0)),
	((200.0, 124.0), (180.0, 106.0), (152.0, 102.0)),
]

# A vertical blue gradient. Mid-tone blues stay visible on both the light
# and the dark taskbar / title bar themes, which a black note would not.
COLOR_TOP = (0x4A, 0x90, 0xE2)
COLOR_BOTTOM = (0x1F, 0x4E, 0x9A)

PNG_SIZES = [16, 24, 32, 48, 64, 128, 256, 512]
# The sizes Windows actually asks an .ico for at the various DPI settings
# (16/20/24/32/40/48 for small and large icons, 64 and 256 for Explorer's
# bigger views).
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]

# Each raster image is drawn this many times larger and then box-filtered
# down, which gives exact-coverage anti-aliasing (ImageDraw has none).
SUPERSAMPLE = 8


def _hex(rgb):
	return "#%02X%02X%02X" % rgb


def ellipse_points(steps=180):
	"""The tilted note head as a polygon, in view coordinates."""
	angle = math.radians(HEAD_ROTATE_DEG)
	cos_a, sin_a = math.cos(angle), math.sin(angle)
	points = []
	for i in range(steps):
		t = 2.0 * math.pi * i / steps
		x = HEAD_RX * math.cos(t)
		y = HEAD_RY * math.sin(t)
		points.append((HEAD_CX + x * cos_a - y * sin_a, HEAD_CY + x * sin_a + y * cos_a))
	return points


def flag_points(steps=64):
	"""The flag as a polygon, flattening each Bezier into short lines."""
	points = [FLAG_START]
	current = FLAG_START
	for c1, c2, end in FLAG_CURVES:
		for i in range(1, steps + 1):
			t = i / steps
			mt = 1.0 - t
			x = mt ** 3 * current[0] + 3 * mt * mt * t * c1[0] + 3 * mt * t * t * c2[0] + t ** 3 * end[0]
			y = mt ** 3 * current[1] + 3 * mt * mt * t * c1[1] + 3 * mt * t * t * c2[1] + t ** 3 * end[1]
			points.append((x, y))
		current = end
	return points


def stem_points():
	return [(STEM_X0, STEM_Y0), (STEM_X1, STEM_Y0), (STEM_X1, STEM_Y1), (STEM_X0, STEM_Y1)]


def render(size):
	"""Render the note as an RGBA image of size x size pixels."""
	big = size * SUPERSAMPLE
	scale = big / VIEW
	mask = Image.new("L", (big, big), 0)
	draw = ImageDraw.Draw(mask)
	for shape in (ellipse_points(), stem_points(), flag_points()):
		draw.polygon([(x * scale, y * scale) for x, y in shape], fill=255)
	# reduce() averages each SUPERSAMPLE x SUPERSAMPLE block, i.e. the alpha
	# becomes the fraction of the pixel the shape covers.
	mask = mask.reduce(SUPERSAMPLE)

	gradient = Image.new("RGB", (size, size))
	pixels = gradient.load()
	for y in range(size):
		t = (y + 0.5) / size
		row = tuple(round(a + (b - a) * t) for a, b in zip(COLOR_TOP, COLOR_BOTTOM))
		for x in range(size):
			pixels[x, y] = row
	image = gradient.convert("RGBA")
	image.putalpha(mask)
	return image


def svg_text():
	"""The same design as a standalone SVG document."""
	curves = " ".join(
		"C %g %g, %g %g, %g %g" % (c1[0], c1[1], c2[0], c2[1], end[0], end[1])
		for c1, c2, end in FLAG_CURVES
	)
	return (
		'<?xml version="1.0" encoding="UTF-8"?>\n'
		'<svg xmlns="http://www.w3.org/2000/svg" width="{v}" height="{v}" viewBox="0 0 {v} {v}">\n'
		'\t<title>Music Player</title>\n'
		'\t<defs>\n'
		'\t\t<linearGradient id="noteFill" x1="0" y1="0" x2="0" y2="{v}" gradientUnits="userSpaceOnUse">\n'
		'\t\t\t<stop offset="0" stop-color="{top}"/>\n'
		'\t\t\t<stop offset="1" stop-color="{bottom}"/>\n'
		'\t\t</linearGradient>\n'
		'\t</defs>\n'
		'\t<g fill="url(#noteFill)">\n'
		'\t\t<ellipse cx="{cx:g}" cy="{cy:g}" rx="{rx:g}" ry="{ry:g}" transform="rotate({rot:g} {cx:g} {cy:g})"/>\n'
		'\t\t<rect x="{sx:g}" y="{sy:g}" width="{sw:g}" height="{sh:g}"/>\n'
		'\t\t<path d="M {fx:g} {fy:g} {curves} Z"/>\n'
		'\t</g>\n'
		'</svg>\n'
	).format(
		v=VIEW, top=_hex(COLOR_TOP), bottom=_hex(COLOR_BOTTOM),
		cx=HEAD_CX, cy=HEAD_CY, rx=HEAD_RX, ry=HEAD_RY, rot=HEAD_ROTATE_DEG,
		sx=STEM_X0, sy=STEM_Y0, sw=STEM_X1 - STEM_X0, sh=STEM_Y1 - STEM_Y0,
		fx=FLAG_START[0], fy=FLAG_START[1], curves=curves,
	)


def dib_icon_image(image):
	"""Encode one RGBA image as an .ico DIB entry (header + XOR + AND mask)."""
	width, height = image.size
	# BITMAPINFOHEADER. The height is doubled because an icon DIB holds the
	# color (XOR) bitmap and the 1-bit AND mask stacked on top of each other.
	header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, 0, 0, 0, 0, 0)
	rgba = image.tobytes()
	xor_rows = []
	and_rows = []
	and_stride = ((width + 31) // 32) * 4  # rows are padded to 32 bits
	# DIBs are stored bottom-up.
	for y in range(height - 1, -1, -1):
		row = bytearray()
		mask = bytearray(and_stride)
		for x in range(width):
			r, g, b, a = rgba[(y * width + x) * 4:(y * width + x) * 4 + 4]
			row += bytes((b, g, r, a))
			if a == 0:
				# AND bit set = "transparent" for code that ignores alpha.
				mask[x // 8] |= 0x80 >> (x % 8)
		xor_rows.append(bytes(row))
		and_rows.append(bytes(mask))
	return header + b"".join(xor_rows) + b"".join(and_rows)


def ico_bytes(images):
	"""Build a complete .ico file from a list of square RGBA images."""
	entries = [dib_icon_image(image) for image in images]
	out = struct.pack("<HHH", 0, 1, len(images))
	offset = 6 + 16 * len(images)
	for image, data in zip(images, entries):
		size = image.size[0]
		dim = 0 if size >= 256 else size  # 0 means 256 in an ICONDIRENTRY
		out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
		offset += len(data)
	return out + b"".join(entries)


def generate(output_dir):
	"""Write every icon file into output_dir and return their paths."""
	os.makedirs(output_dir, exist_ok=True)
	written = []

	svg_path = os.path.join(output_dir, "music_note.svg")
	with open(svg_path, "w", encoding="utf-8", newline="\n") as f:
		f.write(svg_text())
	written.append(svg_path)

	cache = {}

	def image_for(size):
		if size not in cache:
			cache[size] = render(size)
		return cache[size]

	for size in PNG_SIZES:
		path = os.path.join(output_dir, "music_note_%dx%d.png" % (size, size))
		image_for(size).save(path, optimize=True)
		written.append(path)

	ico_path = os.path.join(output_dir, "music_note.ico")
	with open(ico_path, "wb") as f:
		f.write(ico_bytes([image_for(size) for size in ICO_SIZES]))
	written.append(ico_path)
	return written


def main(argv):
	repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
	output_dir = argv[1] if len(argv) > 1 else os.path.join(repo_root, "Music_Icons")
	for path in generate(output_dir):
		print(path)
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
