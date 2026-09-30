/*
 * dr_libs_config.h - how we build the vendored dr_libs decoders.
 *
 * Include this instead of the dr_*.h headers directly so that every file
 * sees the same configuration.
 *
 * DR_*_NO_STDIO removes the libraries' own file-opening helpers. We feed
 * them through our MpStream callbacks instead, because their stdio code
 * calls _ftelli64/_fseeki64/_wfopen_s, which Windows XP's msvcrt.dll does
 * not export - with those in the binary, the exe would refuse to start on
 * XP with an "entry point not found" error.
 */
#ifndef MP_DR_LIBS_CONFIG_H
#define MP_DR_LIBS_CONFIG_H

#define DR_MP3_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_WAV_NO_STDIO

#include "dr_mp3.h"
#include "dr_flac.h"
#include "dr_wav.h"

#endif
