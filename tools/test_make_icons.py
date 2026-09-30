#!/usr/bin/env python3
"""Tests for tools/make_icons.py and the committed Music_Icons folder.

Run directly (python tools/test_make_icons.py) or through CTest. Exits with
77 - CTest's "skipped" code, see CMakeLists.txt - when Pillow is missing.
"""

import os
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

try:
	from PIL import Image
except ImportError:
	print("Pillow is not installed; skipping icon tests (pip install Pillow)")
	sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ICON_DIR = os.path.join(REPO, "Music_Icons")
sys.path.insert(0, HERE)

import make_icons  # noqa: E402  (needs the sys.path tweak above)


def read_ico(path):
	"""Parse an .ico into a list of (width, height, bitcount, image bytes)."""
	with open(path, "rb") as f:
		data = f.read()
	reserved, kind, count = struct.unpack_from("<HHH", data, 0)
	assert reserved == 0 and kind == 1, "not an icon file"
	entries = []
	for i in range(count):
		w, h, colors, res, planes, bits, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
		entries.append((w or 256, h or 256, bits, data[offset:offset + size]))
	return entries


class GeneratedIcons(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.tmp = tempfile.TemporaryDirectory()
		cls.paths = make_icons.generate(cls.tmp.name)

	@classmethod
	def tearDownClass(cls):
		cls.tmp.cleanup()

	def path(self, name):
		return os.path.join(self.tmp.name, name)

	def test_writes_every_file(self):
		names = sorted(os.path.basename(p) for p in self.paths)
		expected = sorted(
			["music_note.svg", "music_note.ico"]
			+ ["music_note_%dx%d.png" % (s, s) for s in make_icons.PNG_SIZES]
		)
		self.assertEqual(names, expected)

	def test_pngs_have_right_size_and_transparency(self):
		for size in make_icons.PNG_SIZES:
			with Image.open(self.path("music_note_%dx%d.png" % (size, size))) as im:
				self.assertEqual(im.size, (size, size))
				self.assertEqual(im.mode, "RGBA")
				alpha = im.getchannel("A")
				# Corners are background: fully transparent.
				for corner in [(0, 0), (size - 1, 0), (0, size - 1), (size - 1, size - 1)]:
					self.assertEqual(alpha.getpixel(corner), 0, "size %d corner %s" % (size, corner))
				# The note itself is there and solid somewhere.
				self.assertEqual(alpha.getextrema()[1], 255, "size %d has no opaque pixel" % size)

	def test_note_covers_a_sensible_share_of_the_icon(self):
		with Image.open(self.path("music_note_256x256.png")) as im:
			alpha = im.getchannel("A")
			coverage = sum(alpha.getdata()) / (255.0 * 256 * 256)
			bbox = alpha.getbbox()
		# A note glyph: neither a speck nor a filled square.
		self.assertGreater(coverage, 0.15)
		self.assertLess(coverage, 0.5)
		# Roughly centered, with a margin on every side.
		left, top, right, bottom = bbox
		self.assertGreater(left, 16)
		self.assertGreater(top, 16)
		self.assertLess(right, 256 - 16)
		self.assertLess(bottom, 256 - 16)

	def test_ico_is_xp_compatible(self):
		entries = read_ico(self.path("music_note.ico"))
		self.assertEqual(sorted(e[0] for e in entries), sorted(make_icons.ICO_SIZES))
		for w, h, bits, blob in entries:
			self.assertEqual(w, h)
			self.assertEqual(bits, 32)
			# Every image must be a BITMAPINFOHEADER DIB - not PNG, which
			# Windows XP cannot read inside an .ico.
			self.assertNotEqual(blob[:8], b"\x89PNG\r\n\x1a\n", "%dx%d is PNG-compressed" % (w, w))
			header_size, dib_w, dib_h, planes, dib_bits = struct.unpack_from("<IiiHH", blob, 0)
			self.assertEqual(header_size, 40)
			self.assertEqual(dib_w, w)
			self.assertEqual(dib_h, 2 * h)  # XOR + AND masks stacked
			self.assertEqual(dib_bits, 32)
			and_stride = ((w + 31) // 32) * 4
			self.assertEqual(len(blob), 40 + w * h * 4 + and_stride * h)

	def test_ico_pixels_match_the_pngs(self):
		# The DIB is stored bottom-up in BGRA; decode one entry by hand and
		# compare with a fresh render.
		entries = {e[0]: e for e in read_ico(self.path("music_note.ico"))}
		w = 32
		blob = entries[w][3]
		expected = make_icons.render(w).tobytes()
		for y in range(w):
			row = blob[40 + (w - 1 - y) * w * 4: 40 + (w - y) * w * 4]
			for x in range(w):
				b, g, r, a = row[x * 4: x * 4 + 4]
				self.assertEqual((r, g, b, a), tuple(expected[(y * w + x) * 4:(y * w + x) * 4 + 4]))

	def test_ico_and_mask_marks_transparent_pixels(self):
		entries = {e[0]: e for e in read_ico(self.path("music_note.ico"))}
		w = 16
		blob = entries[w][3]
		and_start = 40 + w * w * 4
		# Bottom-up: DIB row 0 is image row 15. Pixel (0, 15) is a corner.
		self.assertTrue(blob[and_start] & 0x80)

	def test_svg_is_well_formed(self):
		tree = ET.parse(self.path("music_note.svg"))
		root = tree.getroot()
		self.assertTrue(root.tag.endswith("svg"))
		self.assertEqual(root.get("viewBox"), "0 0 256 256")
		ns = {"s": "http://www.w3.org/2000/svg"}
		self.assertEqual(len(root.findall(".//s:ellipse", ns)), 1)
		self.assertEqual(len(root.findall(".//s:rect", ns)), 1)
		self.assertEqual(len(root.findall(".//s:path", ns)), 1)
		self.assertIsNotNone(root.find(".//s:linearGradient", ns))


class CommittedIcons(unittest.TestCase):
	"""The files in Music_Icons must be what the script currently makes, so
	nobody edits one by hand and lets the set drift apart."""

	def test_committed_pngs_match_the_script(self):
		for size in make_icons.PNG_SIZES:
			path = os.path.join(ICON_DIR, "music_note_%dx%d.png" % (size, size))
			self.assertTrue(os.path.exists(path), path)
			with Image.open(path) as im:
				committed = im.convert("RGBA").tobytes()
			fresh = make_icons.render(size).tobytes()
			# Allow a 1-level difference per channel in case another
			# Pillow version rounds a filter differently.
			worst = max(abs(a - b) for a, b in zip(committed, fresh))
			self.assertLessEqual(worst, 1, "Music_Icons is stale for %dpx: run tools/make_icons.py" % size)

	def test_committed_svg_matches_the_script(self):
		with open(os.path.join(ICON_DIR, "music_note.svg"), encoding="utf-8") as f:
			self.assertEqual(f.read(), make_icons.svg_text())

	def test_committed_ico_has_every_size(self):
		entries = read_ico(os.path.join(ICON_DIR, "music_note.ico"))
		self.assertEqual(sorted(e[0] for e in entries), sorted(make_icons.ICO_SIZES))


if __name__ == "__main__":
	result = unittest.main(exit=False, verbosity=2).result
	sys.exit(0 if result.wasSuccessful() else 1)
