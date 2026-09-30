/*
 * player.c - waveOut playback. See player.h for the threading contract.
 *
 * How it works: the device is opened in the paused state and given
 * NUM_BUFFERS buffers of decoded audio. waveOut signals `event` each time it
 * finishes one (CALLBACK_EVENT); the worker thread wakes, refills every
 * finished buffer and queues it again. Pause/resume are waveOutPause and
 * waveOutRestart, which stop and start the device without losing anything
 * that is queued. Seeking resets the device (returning all buffers),
 * repositions the decoder and refills.
 *
 * Why CALLBACK_EVENT rather than a callback function: a waveOut callback
 * runs on a system thread and is not allowed to call other waveOut
 * functions (doing so can deadlock on some drivers), so the refill would
 * have to be handed to another thread anyway. An event does that directly.
 */
#include "player.h"

#include <process.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "text.h"

#define NUM_BUFFERS 4
/* ~93 ms per buffer at 44.1 kHz, ~370 ms queued in total: enough to ride
 * out a busy moment (a game loading a zone, say) without a dropout, small
 * enough that seeking feels immediate. */
#define BUFFER_FRAMES 4096

struct MpPlayer {
	CRITICAL_SECTION lock;
	HANDLE thread;
	HANDLE event;
	volatile LONG quit;
	HWND notify_hwnd;
	UINT notify_msg;

	MpDecoder *dec;
	HWAVEOUT wo;
	WAVEHDR hdr[NUM_BUFFERS];
	int queued[NUM_BUFFERS];
	int nqueued;
	int16_t *pcm; /* NUM_BUFFERS * BUFFER_FRAMES * channels samples */

	MpPlayerState state;
	int eof;        /* decoder has nothing more to give */
	int finished;   /* ...and the device has played all of it */
	uint64_t base_frame; /* track frame at device position 0 */
	uint32_t rate;
	uint32_t channels;
	UINT generation;
};

/* Refill every buffer the device has finished with. Caller holds the lock. */
static void service_locked(MpPlayer *p)
{
	int i;
	if (p->wo == NULL)
		return;
	for (i = 0; i < NUM_BUFFERS; i++) {
		if (p->queued[i] && (p->hdr[i].dwFlags & WHDR_DONE)) {
			p->queued[i] = 0;
			p->nqueued--;
		}
	}
	for (i = 0; i < NUM_BUFFERS && !p->eof; i++) {
		uint64_t got;
		if (p->queued[i])
			continue;
		got = mp_decoder_read(p->dec, (int16_t *)p->hdr[i].lpData, BUFFER_FRAMES);
		if (got == 0) {
			p->eof = 1;
			break;
		}
		p->hdr[i].dwBufferLength = (DWORD)(got * p->channels * sizeof(int16_t));
		if (waveOutWrite(p->wo, &p->hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
			/* The device went away (USB headset unplugged...). Treat it as
			 * the end of the track rather than spinning. */
			p->eof = 1;
			break;
		}
		p->queued[i] = 1;
		p->nqueued++;
	}
	if (p->eof && p->nqueued == 0 && p->state == MP_PLAYER_PLAYING) {
		p->state = MP_PLAYER_STOPPED;
		p->finished = 1;
		PostMessageW(p->notify_hwnd, p->notify_msg, (WPARAM)p->generation, 0);
	}
}

static unsigned __stdcall worker(void *arg)
{
	MpPlayer *p = (MpPlayer *)arg;
	for (;;) {
		WaitForSingleObject(p->event, INFINITE);
		if (p->quit)
			break;
		EnterCriticalSection(&p->lock);
		service_locked(p);
		LeaveCriticalSection(&p->lock);
	}
	return 0;
}

static void close_device_locked(MpPlayer *p)
{
	int i;
	if (p->wo != NULL) {
		/* Reset first: waveOutClose refuses while buffers are queued. */
		waveOutReset(p->wo);
		for (i = 0; i < NUM_BUFFERS; i++) {
			if (p->hdr[i].dwFlags & WHDR_PREPARED)
				waveOutUnprepareHeader(p->wo, &p->hdr[i], sizeof(WAVEHDR));
		}
		waveOutClose(p->wo);
		p->wo = NULL;
	}
	memset(p->hdr, 0, sizeof(p->hdr));
	memset(p->queued, 0, sizeof(p->queued));
	p->nqueued = 0;
	if (p->dec != NULL) {
		mp_decoder_close(p->dec);
		p->dec = NULL;
	}
	p->state = MP_PLAYER_EMPTY;
	p->eof = 0;
	p->finished = 0;
	p->base_frame = 0;
}

/* Repositions to `frame`. Keeps the play/pause state. Caller holds the lock. */
static void seek_locked(MpPlayer *p, uint64_t frame)
{
	uint64_t total;
	if (p->wo == NULL)
		return;
	/* Returns every queued buffer (marked done) and zeroes the position. */
	waveOutReset(p->wo);
	/* Put the device in the right state before queueing anything, so a
	 * paused player does not blip out the first buffer. */
	if (p->state == MP_PLAYER_PLAYING)
		waveOutRestart(p->wo);
	else
		waveOutPause(p->wo);
	memset(p->queued, 0, sizeof(p->queued));
	p->nqueued = 0;
	total = mp_decoder_total_frames(p->dec);
	if (total > 0 && frame > total)
		frame = total;
	mp_decoder_seek(p->dec, frame);
	p->base_frame = frame;
	p->eof = 0;
	p->finished = 0;
	service_locked(p);
}

MpPlayer *mp_player_create(HWND notify_hwnd, UINT notify_msg)
{
	MpPlayer *p = (MpPlayer *)calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;
	InitializeCriticalSection(&p->lock);
	p->notify_hwnd = notify_hwnd;
	p->notify_msg = notify_msg;
	/* Auto-reset: one wake-up services every finished buffer, so there is
	 * nothing to lose if several completions collapse into one signal. */
	p->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (p->event == NULL)
		goto fail;
	/* _beginthreadex rather than CreateThread so the C runtime sets up its
	 * per-thread data (the decoders call malloc on this thread). */
	p->thread = (HANDLE)_beginthreadex(NULL, 0, worker, p, 0, NULL);
	if (p->thread == NULL)
		goto fail;
	return p;
fail:
	if (p->event != NULL)
		CloseHandle(p->event);
	DeleteCriticalSection(&p->lock);
	free(p);
	return NULL;
}

void mp_player_destroy(MpPlayer *p)
{
	if (p == NULL)
		return;
	InterlockedExchange(&p->quit, 1);
	SetEvent(p->event);
	WaitForSingleObject(p->thread, INFINITE);
	CloseHandle(p->thread);
	EnterCriticalSection(&p->lock);
	close_device_locked(p);
	LeaveCriticalSection(&p->lock);
	CloseHandle(p->event);
	DeleteCriticalSection(&p->lock);
	free(p->pcm);
	free(p);
}

int mp_player_load(MpPlayer *p, const wchar_t *path, wchar_t *err, size_t err_cap)
{
	MpDecoder *dec;
	WAVEFORMATEX fmt;
	MMRESULT r;
	size_t per_buffer;
	int i;

	/* Decode setup can take a moment for a long MP3 (it scans the file for
	 * its length), so do it before taking the lock the worker needs. */
	dec = mp_decoder_open_file(path, err, err_cap);

	EnterCriticalSection(&p->lock);
	close_device_locked(p);
	if (dec == NULL) {
		LeaveCriticalSection(&p->lock);
		return 0;
	}

	p->rate = mp_decoder_sample_rate(dec);
	p->channels = mp_decoder_channels(dec);
	memset(&fmt, 0, sizeof(fmt));
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = (WORD)p->channels;
	fmt.nSamplesPerSec = p->rate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = (WORD)(p->channels * 2);
	fmt.nAvgBytesPerSec = p->rate * fmt.nBlockAlign;

	r = waveOutOpen(&p->wo, WAVE_MAPPER, &fmt, (DWORD_PTR)p->event, 0, CALLBACK_EVENT);
	if (r != MMSYSERR_NOERROR) {
		p->wo = NULL;
		if (err != NULL && err_cap > 0) {
			wchar_t text[MAXERRORLENGTH];
			if (waveOutGetErrorTextW(r, text, MAXERRORLENGTH) != MMSYSERR_NOERROR)
				text[0] = 0;
			mp_wcopy(err, err_cap, r == MMSYSERR_NODRIVER || r == MMSYSERR_BADDEVICEID ?
				L"No audio output device is available." : (text[0] ? text : L"The audio device could not be opened."));
		}
		mp_decoder_close(dec);
		LeaveCriticalSection(&p->lock);
		return 0;
	}
	/* Hold the device paused until mp_player_play. */
	waveOutPause(p->wo);

	per_buffer = (size_t)BUFFER_FRAMES * p->channels;
	free(p->pcm);
	p->pcm = (int16_t *)malloc(per_buffer * NUM_BUFFERS * sizeof(int16_t));
	if (p->pcm == NULL) {
		mp_wcopy(err, err_cap, L"Out of memory.");
		p->dec = dec;
		close_device_locked(p);
		LeaveCriticalSection(&p->lock);
		return 0;
	}
	for (i = 0; i < NUM_BUFFERS; i++) {
		p->hdr[i].lpData = (LPSTR)(p->pcm + per_buffer * i);
		p->hdr[i].dwBufferLength = (DWORD)(per_buffer * sizeof(int16_t));
		waveOutPrepareHeader(p->wo, &p->hdr[i], sizeof(WAVEHDR));
	}

	p->dec = dec;
	p->state = MP_PLAYER_STOPPED;
	p->generation++;
	service_locked(p); /* pre-fill so play starts instantly */
	LeaveCriticalSection(&p->lock);
	return 1;
}

void mp_player_unload(MpPlayer *p)
{
	EnterCriticalSection(&p->lock);
	close_device_locked(p);
	LeaveCriticalSection(&p->lock);
}

void mp_player_play(MpPlayer *p)
{
	EnterCriticalSection(&p->lock);
	if (p->wo != NULL && p->state != MP_PLAYER_PLAYING) {
		if (p->finished) {
			p->state = MP_PLAYER_STOPPED;
			seek_locked(p, 0);
		}
		p->state = MP_PLAYER_PLAYING;
		waveOutRestart(p->wo);
		/* Wake the worker in case the track was already fully queued (or
		 * empty), so the end of it is noticed. */
		SetEvent(p->event);
	}
	LeaveCriticalSection(&p->lock);
}

void mp_player_pause(MpPlayer *p)
{
	EnterCriticalSection(&p->lock);
	if (p->wo != NULL && p->state == MP_PLAYER_PLAYING) {
		waveOutPause(p->wo);
		p->state = MP_PLAYER_PAUSED;
	}
	LeaveCriticalSection(&p->lock);
}

void mp_player_stop(MpPlayer *p)
{
	EnterCriticalSection(&p->lock);
	if (p->wo != NULL) {
		p->state = MP_PLAYER_STOPPED;
		seek_locked(p, 0);
	}
	LeaveCriticalSection(&p->lock);
}

void mp_player_seek_ms(MpPlayer *p, uint32_t ms)
{
	EnterCriticalSection(&p->lock);
	if (p->wo != NULL)
		seek_locked(p, (uint64_t)ms * p->rate / 1000);
	LeaveCriticalSection(&p->lock);
}

MpPlayerState mp_player_state(MpPlayer *p)
{
	MpPlayerState s;
	EnterCriticalSection(&p->lock);
	s = p->state;
	LeaveCriticalSection(&p->lock);
	return s;
}

uint32_t mp_player_position_ms(MpPlayer *p)
{
	uint64_t frame = 0, total;
	EnterCriticalSection(&p->lock);
	if (p->wo != NULL) {
		total = mp_decoder_total_frames(p->dec);
		if (p->finished) {
			frame = total;
		} else {
			MMTIME t;
			memset(&t, 0, sizeof(t));
			t.wType = TIME_SAMPLES;
			frame = p->base_frame;
			if (waveOutGetPosition(p->wo, &t, sizeof(t)) == MMSYSERR_NOERROR) {
				/* Drivers may answer in another unit than the one asked. */
				if (t.wType == TIME_SAMPLES)
					frame += t.u.sample;
				else if (t.wType == TIME_BYTES)
					frame += t.u.cb / (p->channels * 2);
			}
		}
		if (total > 0 && frame > total)
			frame = total;
	}
	LeaveCriticalSection(&p->lock);
	return p->rate ? (uint32_t)(frame * 1000 / p->rate) : 0;
}

uint32_t mp_player_duration_ms(MpPlayer *p)
{
	uint32_t ms = 0;
	EnterCriticalSection(&p->lock);
	if (p->dec != NULL && p->rate != 0)
		ms = (uint32_t)(mp_decoder_total_frames(p->dec) * 1000 / p->rate);
	LeaveCriticalSection(&p->lock);
	return ms;
}

UINT mp_player_generation(MpPlayer *p)
{
	UINT g;
	EnterCriticalSection(&p->lock);
	g = p->generation;
	LeaveCriticalSection(&p->lock);
	return g;
}
