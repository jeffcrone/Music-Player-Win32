/*
 * dr_libs_impl.c - compiles the implementations of the single-header
 * dr_libs decoders exactly once.
 *
 * This file is built with compiler warnings turned off (see CMakeLists.txt)
 * because the vendored code is not ours to fix; our own files are built
 * with -Wall -Wextra.
 */
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#include "dr_libs_config.h"
