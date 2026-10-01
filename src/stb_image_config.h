/*
 * stb_image_config.h - how we build the vendored stb_image decoder. See
 * stb_image_impl.c for the reasons behind each setting.
 *
 * Include this instead of stb_image.h directly, so that every file sees the
 * same configuration.
 */
#ifndef MP_STB_IMAGE_CONFIG_H
#define MP_STB_IMAGE_CONFIG_H

#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 8192

#include "stb_image.h"

#endif
