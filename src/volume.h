/*
 * volume.h - the player's own volume control, done in software.
 *
 * Why not waveOutSetVolume: on Windows XP it changes the sound card's wave
 * volume, which every program shares, so turning the music down would turn
 * World of Warcraft down with it. (On Vista and later it moves this app's
 * slider in the Volume Mixer instead, and some drivers do not support it at
 * all.) Scaling the samples ourselves affects only this program and behaves
 * the same on every Windows version.
 *
 * The volume is a percentage, 0 to 100, as shown on the slider. It maps to a
 * gain on a squared curve rather than a straight line, because loudness is
 * heard roughly logarithmically: with a linear gain, everything from 20% to
 * 100% sounds nearly the same and all the change is bunched up at the
 * bottom of the slider. Squared puts 50% at about -12 dB and 10% at -40 dB,
 * which feels even across the whole range.
 *
 * Gains are 16.16 fixed point (MP_VOLUME_UNITY = 1.0), so the audio thread
 * does no floating point.
 */
#ifndef MP_VOLUME_H
#define MP_VOLUME_H

#include <stddef.h>
#include <stdint.h>

#define MP_VOLUME_MAX 100
#define MP_VOLUME_UNITY 65536u

/* Clamps any integer to 0..MP_VOLUME_MAX. */
int mp_volume_clamp(int percent);

/* The 16.16 gain for a volume percentage (clamped first). 100 gives exactly
 * MP_VOLUME_UNITY and 0 gives exactly 0. */
uint32_t mp_volume_gain(int percent);

/* Scales `count` 16-bit samples in place by `gain`. A gain of
 * MP_VOLUME_UNITY (or more) leaves them bit-for-bit unchanged; the gain is
 * never allowed to amplify, so nothing can clip. */
void mp_volume_apply(int16_t *samples, size_t count, uint32_t gain);

#endif
