/*
 * stb_image_impl.c - compiles the vendored stb_image decoder exactly once,
 * configured for album art.
 *
 * Like dr_libs_impl.c, this file is built with warnings off (see
 * CMakeLists.txt): the vendored code is not ours to fix.
 *
 * The configuration, and why:
 *   - STBI_NO_STDIO: we always decode from memory. The stdio helpers would
 *     also pull in C runtime functions that need checking against what
 *     Windows XP's msvcrt.dll exports.
 *   - STBI_ONLY_JPEG/PNG/BMP/GIF: the formats cover art actually comes in.
 *     Every decoder left out is less code reading untrusted bytes (the
 *     pictures come from whatever music files the user opens).
 *   - STBI_NO_LINEAR, STBI_NO_HDR: no floating-point image paths, which
 *     cover art never needs.
 *   - STBI_NO_THREAD_LOCALS: the error-reason string is a plain global.
 *     We only ever decode on the UI thread, and it avoids thread-local
 *     storage, whose support on XP depends on how the toolchain does it.
 *   - STBI_MAX_DIMENSIONS: refuse anything over 8192 pixels a side before
 *     allocating for it. Real cover art is at most a few thousand pixels;
 *     a file claiming 60000 x 60000 is broken or hostile, and would ask for
 *     gigabytes.
 */
#include "stb_image_config.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
