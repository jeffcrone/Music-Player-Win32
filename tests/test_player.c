/*
 * test_player.c - real playback through waveOut.
 *
 * These tests play actual audio (digital silence, so nothing is heard) and
 * therefore need an audio output device. Build servers usually have none,
 * in which case the suite reports itself as skipped (exit code 77) rather
 * than failing.
 *
 * Timing checks use generous margins: audio devices report their position
 * in chunks, and a busy machine can be late to deliver timer messages.
 */
#include <stdlib.h>
#include <windows.h>

#include "builders.h"
#include "mptest.h"
#include "player.h"

#define WM_TEST_END (WM_APP + 7)

static int end_count;
static WPARAM end_generation;

static LRESULT CALLBACK test_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TEST_END) {
		end_count++;
		end_generation = wp;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Pumps messages for `ms` milliseconds, or until `stop_at_end` and a
 * track-end notice arrives. */
static void pump(DWORD ms, int stop_at_end)
{
	DWORD start = GetTickCount();
	int ends_before = end_count;
	while (GetTickCount() - start < ms) {
		MSG m;
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&m);
		if (stop_at_end && end_count != ends_before)
			return;
		Sleep(10);
	}
}

static int make_silent_wav(uint32_t ms, wchar_t *path)
{
	Buf b;
	size_t frames = (size_t)44100 * ms / 1000;
	int16_t *pcm = (int16_t *)calloc(frames * 2, sizeof(int16_t));
	int ok;
	buf_init(&b);
	wav_file(&b, 1, 2, 44100, 16, pcm, frames * 4, NULL);
	ok = temp_file(&b, L".wav", path, MAX_PATH);
	buf_free(&b);
	free(pcm);
	return ok;
}

int suite_player(void)
{
	WNDCLASSW wc;
	HWND hwnd;
	MpPlayer *p;
	wchar_t path[MAX_PATH], path2[MAX_PATH], err[256];
	uint32_t pos, pos2;
	UINT gen;

	/* Some build machines list a device that cannot actually be opened (the
	 * Windows Audio service is stopped, or it is a remote-desktop stub), so
	 * ask whether the format we use would open, not just whether a device
	 * exists. */
	{
		WAVEFORMATEX fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.wFormatTag = WAVE_FORMAT_PCM;
		fmt.nChannels = 2;
		fmt.nSamplesPerSec = 44100;
		fmt.wBitsPerSample = 16;
		fmt.nBlockAlign = 4;
		fmt.nAvgBytesPerSec = 44100 * 4;
		if (waveOutGetNumDevs() == 0 ||
			waveOutOpen(NULL, WAVE_MAPPER, &fmt, 0, 0, WAVE_FORMAT_QUERY) != MMSYSERR_NOERROR) {
			printf("  no usable audio output device; skipping playback tests\n");
			return MPT_SKIP;
		}
	}

	/* A message-only window to receive the player's notifications. */
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = test_wnd_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"MpTestPlayerSink";
	RegisterClassW(&wc);
	hwnd = CreateWindowW(L"MpTestPlayerSink", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
	CHECK(hwnd != NULL);

	p = mp_player_create(hwnd, WM_TEST_END);
	CHECK(p != NULL);
	if (p == NULL)
		return 0;

	/* Fresh player: empty, and every control is a harmless no-op. */
	CHECK_INT(mp_player_state(p), MP_PLAYER_EMPTY);
	CHECK_INT(mp_player_position_ms(p), 0);
	CHECK_INT(mp_player_duration_ms(p), 0);
	mp_player_play(p);
	mp_player_pause(p);
	mp_player_stop(p);
	mp_player_seek_ms(p, 1000);
	CHECK_INT(mp_player_state(p), MP_PLAYER_EMPTY);

	/* A file that does not exist: fails with a reason, stays empty. */
	err[0] = 0;
	CHECK(!mp_player_load(p, L"C:\\nope\\missing.wav", err, 256));
	CHECK(err[0] != 0);
	CHECK_INT(mp_player_state(p), MP_PLAYER_EMPTY);

	if (!make_silent_wav(1500, path) || !make_silent_wav(400, path2)) {
		CHECK(!"could not write temp files");
		mp_player_destroy(p);
		return 0;
	}

	/* Load: stopped at 0 with the right length. */
	CHECK(mp_player_load(p, path, err, 256));
	gen = mp_player_generation(p);
	CHECK_INT(mp_player_state(p), MP_PLAYER_STOPPED);
	CHECK_INT(mp_player_duration_ms(p), 1500);
	CHECK_INT(mp_player_position_ms(p), 0);

	/* Nothing moves until play. */
	pump(200, 0);
	CHECK_INT(mp_player_position_ms(p), 0);

	/* Play: the position advances. */
	mp_player_play(p);
	CHECK_INT(mp_player_state(p), MP_PLAYER_PLAYING);
	pump(400, 0);
	pos = mp_player_position_ms(p);
	CHECK(pos > 100 && pos < 1200);

	/* Pause: the position holds still. */
	mp_player_pause(p);
	CHECK_INT(mp_player_state(p), MP_PLAYER_PAUSED);
	pos = mp_player_position_ms(p);
	pump(300, 0);
	pos2 = mp_player_position_ms(p);
	CHECK(pos2 == pos);

	/* Seek while paused: jumps, stays paused. */
	mp_player_seek_ms(p, 1000);
	CHECK_INT(mp_player_state(p), MP_PLAYER_PAUSED);
	pos = mp_player_position_ms(p);
	CHECK(pos >= 990 && pos <= 1010);

	/* Resume and play to the end: exactly one notice, for this track. */
	end_count = 0;
	mp_player_play(p);
	pump(3000, 1);
	CHECK_INT(end_count, 1);
	CHECK(end_generation == gen);
	CHECK_INT(mp_player_state(p), MP_PLAYER_STOPPED);
	CHECK_INT(mp_player_position_ms(p), 1500);
	pump(200, 0);
	CHECK_INT(end_count, 1);

	/* Play after the end restarts from the top. */
	mp_player_play(p);
	CHECK_INT(mp_player_state(p), MP_PLAYER_PLAYING);
	pump(100, 0);
	CHECK(mp_player_position_ms(p) < 1000);

	/* Stop: rewinds and stops. */
	mp_player_stop(p);
	CHECK_INT(mp_player_state(p), MP_PLAYER_STOPPED);
	CHECK_INT(mp_player_position_ms(p), 0);

	/* Loading another file replaces the first and bumps the generation. */
	CHECK(mp_player_load(p, path2, err, 256));
	CHECK(mp_player_generation(p) != gen);
	CHECK_INT(mp_player_duration_ms(p), 400);

	/* Loading a bad file after a good one leaves the player empty. */
	CHECK(!mp_player_load(p, L"C:\\nope\\missing.wav", err, 256));
	CHECK_INT(mp_player_state(p), MP_PLAYER_EMPTY);

	/* Unload, and destroying while playing, are both clean. */
	CHECK(mp_player_load(p, path, err, 256));
	mp_player_unload(p);
	CHECK_INT(mp_player_state(p), MP_PLAYER_EMPTY);
	CHECK(mp_player_load(p, path, err, 256));
	mp_player_play(p);
	pump(50, 0);
	mp_player_destroy(p);

	DeleteFileW(path);
	DeleteFileW(path2);
	DestroyWindow(hwnd);
	return 0;
}
