/*
 * player.h - plays one decoded track through the Windows waveOut API.
 *
 * waveOut is the oldest Windows audio API, which is exactly why it is used:
 * it behaves the same from Windows XP through Windows 11 (on Vista and later
 * it is layered on top of WASAPI by Windows itself), needs no extra DLLs,
 * and supports true pause and resume.
 *
 * Threading: all functions are called from the UI thread. A worker thread
 * keeps the device's buffers topped up; when a track finishes on its own,
 * the player posts `notify_msg` to `notify_hwnd` with wParam set to the
 * load generation (see mp_player_generation), so a notice that was already
 * in flight when the user skipped to another track can be recognized as
 * stale and ignored.
 */
#ifndef MP_PLAYER_H
#define MP_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>

typedef enum {
	MP_PLAYER_EMPTY = 0, /* nothing loaded */
	MP_PLAYER_STOPPED,   /* loaded, not playing (at the start, or finished) */
	MP_PLAYER_PLAYING,
	MP_PLAYER_PAUSED
} MpPlayerState;

typedef struct MpPlayer MpPlayer;

/* Returns NULL if the worker thread cannot be created. */
MpPlayer *mp_player_create(HWND notify_hwnd, UINT notify_msg);
void mp_player_destroy(MpPlayer *p);

/* Loads a file, replacing whatever was loaded, and leaves it STOPPED at the
 * start. Returns 1 on success; on failure the player is EMPTY and err holds
 * a readable reason (file problems and audio device problems alike). */
int mp_player_load(MpPlayer *p, const wchar_t *path, wchar_t *err, size_t err_cap);

void mp_player_unload(MpPlayer *p);

/* Start or resume. If the track had finished, starts it from the top. */
void mp_player_play(MpPlayer *p);
/* Pause, keeping the position; mp_player_play resumes from there. */
void mp_player_pause(MpPlayer *p);
/* Stop and rewind to the start. */
void mp_player_stop(MpPlayer *p);
/* Jump to a position; keeps playing or stays paused as before. */
void mp_player_seek_ms(MpPlayer *p, uint32_t ms);

MpPlayerState mp_player_state(MpPlayer *p);
uint32_t mp_player_position_ms(MpPlayer *p);
/* 0 if unknown (possible for FLAC files written by streaming encoders). */
uint32_t mp_player_duration_ms(MpPlayer *p);
/* Incremented by every successful load. */
UINT mp_player_generation(MpPlayer *p);

/* Volume, 0 to 100 (out-of-range values are clamped); see volume.h. It
 * starts at 100, which plays the file's samples unchanged, and it belongs
 * to the player rather than the track, so it carries over from one load to
 * the next. Mute is simply a volume of 0.
 *
 * A change is heard after the audio already queued to the device has
 * played: up to about a third of a second. Rewriting queued buffers is not
 * safe (the driver may be reading them), and resetting the device to
 * requeue would click and, for MP3, cost a re-seek. */
void mp_player_set_volume(MpPlayer *p, int percent);
int mp_player_volume(MpPlayer *p);

#endif
