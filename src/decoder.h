/*
 * decoder.h - one interface over the MP3, FLAC and WAV decoders.
 *
 * Output is always interleaved signed 16-bit PCM, mono or stereo, at the
 * file's own sample rate - the one format every waveOut device on every
 * Windows version back to XP accepts. Files with more than two channels
 * are downmixed (see mp_downmix_s16).
 *
 * The decoding itself is done by dr_mp3, dr_flac and dr_wav (David Reid,
 * public domain / MIT-0, vendored in third_party/dr_libs).
 */
#ifndef MP_DECODER_H
#define MP_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "stream.h"

typedef enum {
	MP_FORMAT_UNKNOWN = 0,
	MP_FORMAT_MP3,
	MP_FORMAT_FLAC,
	MP_FORMAT_WAV
} MpFormat;

typedef struct MpDecoder MpDecoder;

/*
 * Works out the format from the content, not the name: "fLaC" (possibly
 * behind an ID3v2 tag), "RIFF....WAVE" / "RF64" / Sony Wave64, or an MPEG
 * audio frame header. Only when the content is inconclusive (an MP3 with
 * junk before its first frame, say) is name_hint's extension consulted.
 * name_hint may be NULL.
 */
MpFormat mp_detect_format(MpStream *s, const wchar_t *name_hint);

/* Opens a file. On failure returns NULL and writes a readable reason into
 * err (if err is non-NULL). */
MpDecoder *mp_decoder_open_file(const wchar_t *path, wchar_t *err, size_t err_cap);

/* Opens a stream; on success the decoder takes ownership of it and closes
 * it in mp_decoder_close. On failure the stream is left open for the
 * caller. */
MpDecoder *mp_decoder_open_stream(MpStream *s, const wchar_t *name_hint, wchar_t *err, size_t err_cap);

void mp_decoder_close(MpDecoder *d);

MpFormat mp_decoder_format(const MpDecoder *d);
uint32_t mp_decoder_sample_rate(const MpDecoder *d);
/* Output channels: 1 or 2. */
uint32_t mp_decoder_channels(const MpDecoder *d);
/* Channels in the file itself, before any downmix. */
uint32_t mp_decoder_source_channels(const MpDecoder *d);
/* Length in PCM frames (one frame = one sample per channel). */
uint64_t mp_decoder_total_frames(const MpDecoder *d);

/* Decodes up to `frames` frames into out (frames * channels samples).
 * Returns the number of frames produced; 0 means end of track (or a decode
 * error that makes going on pointless). */
uint64_t mp_decoder_read(MpDecoder *d, int16_t *out, uint64_t frames);

/* Moves to a frame index (clamped to the length). Returns 1 on success. */
int mp_decoder_seek(MpDecoder *d, uint64_t frame);

/*
 * Mixes `in_channels`-channel interleaved audio down to stereo (or copies
 * it unchanged for mono/stereo). Even-numbered channels are averaged into
 * the left output and odd-numbered ones into the right, which for the
 * standard WAVE channel order puts front-left/center/rear-left on the left
 * and front-right/LFE/rear-right on the right. Averaging (rather than
 * summing) means the result can never clip.
 * Returns the number of output channels (in_channels if <= 2, else 2).
 */
uint32_t mp_downmix_s16(const int16_t *in, uint32_t in_channels, int16_t *out, size_t frames);

#endif
