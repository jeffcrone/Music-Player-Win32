/*
 * stretch.h - changes the playback speed without changing the pitch.
 *
 * Simply playing the samples faster (opening the device at a higher sample
 * rate) would also raise the pitch - 2x would sound like chipmunks - which
 * is not what anyone means by "playback speed". This is a time stretcher
 * instead, using WSOLA (waveform-similarity overlap-add), the method most
 * media players use for speech and music at moderate speeds:
 *
 *   - The input is cut into short overlapping frames (30 ms, Hann-windowed)
 *     that are laid down at a fixed spacing in the output, but taken from
 *     the input at `speed` times that spacing. Faster skips a little of the
 *     input between frames; slower repeats a little. Each frame is still
 *     played at its original rate, so the pitch is unchanged.
 *   - Cutting at arbitrary points would put the frames out of phase with
 *     each other and the overlaps would partly cancel (a warbly, phasey
 *     sound). So each frame's start is nudged by up to 10 ms to where the
 *     input best matches what the previous frame was about to play next.
 *
 * Pure computation with no Windows calls, so the tests can feed it
 * synthetic tones and measure what comes out.
 */
#ifndef MP_STRETCH_H
#define MP_STRETCH_H

#include <stddef.h>
#include <stdint.h>

/* Speeds are whole percentages (100 = normal), which keeps them exact:
 * 0.75x is 75, never 0.7499999. */
#define MP_STRETCH_MIN_SPEED 25
#define MP_STRETCH_MAX_SPEED 400

typedef struct MpStretch MpStretch;

/* Where the stretcher gets its input: fills `buf` with up to `frames`
 * interleaved 16-bit frames and returns how many it wrote. 0 means the end
 * of the input. */
typedef uint64_t (*MpStretchSource)(void *ctx, int16_t *buf, uint64_t frames);

/* For audio of the given sample rate and channel count (1 to 8). Returns
 * NULL for bad arguments or if out of memory. Starts reset at 100%. */
MpStretch *mp_stretch_create(uint32_t rate, uint32_t channels);
void mp_stretch_destroy(MpStretch *s);

/* Forgets everything buffered and starts again at `speed_percent`
 * (clamped to MIN..MAX): the next output begins with whatever the source
 * gives next. Called after every seek and every speed change. */
void mp_stretch_reset(MpStretch *s, int speed_percent);

/*
 * Writes up to `frames` output frames into `out`, reading the source as
 * needed. Returns the number written, which is less than `frames` only
 * when the input has run out and everything from it has been produced.
 *
 * Output frame k (counted from the last reset) plays the input from
 * around frame k * speed / 100 (within the 10 ms search), which is what
 * the player uses to show the track position. Overall, n input frames
 * become about n * 100 / speed output frames.
 */
size_t mp_stretch_process(MpStretch *s, MpStretchSource src, void *ctx, int16_t *out, size_t frames);

#endif
