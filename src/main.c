/*
 * main.c - the Music Player window.
 *
 * A plain Win32 window built from standard controls (static text, a
 * trackbar, push buttons, a report-view ListView and a status bar), so it
 * takes on whatever theme the running Windows version uses. Everything is
 * laid out from the size of the system message font, which is what makes
 * the window scale correctly at 125%/150%/200% DPI.
 *
 * The playlist ListView is a "virtual" list (LVS_OWNERDATA): it stores no
 * text itself and asks us for each cell as it paints (LVN_GETDISPINFO).
 * The MpPlaylist is then the one and only copy of the data, so the list
 * and the model can never disagree, and very long playlists stay fast.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "player.h"
#include "playlist.h"
#include "resource.h"
#include "tags.h"
#include "text.h"
#include "track.h"
#include "version.h"
#include "volume.h"

#define APP_NAME L"Music Player"
#define WINDOW_CLASS L"JeffCroneMusicPlayerWindow"

/* Posted by the player when a track plays to the end (wParam = generation). */
#define WM_APP_TRACK_END (WM_APP + 1)

#define TIMER_POSITION 1
#define TIMER_INTERVAL_MS 250

/* Seek bar resolution. 1000 steps is finer than a pixel on any screen. */
#define SEEK_RANGE 1000

/* How far F9/F10 and the Volume Up/Down menu items move the volume, and the
 * slider's arrow-key step to match. 5% is 20 presses from silent to full,
 * which the squared volume curve (volume.h) makes feel even. */
#define VOLUME_STEP 5

/* The multi-select file dialog returns every chosen name in one buffer; this
 * holds roughly 1,500 typical file names. */
#define PICK_BUFFER_CHARS 65536

#define PLAYLIST_FILE_MAX (16u * 1024u * 1024u)
#define MSG_MAX 1024

typedef struct {
	HINSTANCE inst;
	HWND hwnd;
	HWND title, artist, seek, time, list, status;
	HWND vol_label, vol_bar;
	HWND btn_prev, btn_play, btn_stop, btn_next, btn_add, btn_playlist;
	HFONT font, title_font, bold_font;
	HACCEL accel;
	int unit;       /* message font line height in pixels */
	int title_height;

	MpPlayer *player;
	MpPlaylist pl;
	int repeat;
	int dragging_seek;
	/* The slider's level, kept while muted so unmuting goes back to it. The
	 * player itself is only ever told the level actually heard. Neither is
	 * saved between runs: the player keeps no settings (see RUNNING.md). */
	int volume;
	int muted;
	MpPlayerState shown_state; /* what the Play button currently says */
} App;

static App g;

/* ---- Small helpers ------------------------------------------------------ */

static void wcat(wchar_t *dst, size_t cap, const wchar_t *src)
{
	size_t len = wcslen(dst);
	if (len < cap)
		mp_wcopy(dst + len, cap - len, src);
}

static void set_status(const wchar_t *text)
{
	SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)text);
}

static int text_width(HFONT font, const wchar_t *text)
{
	HDC dc = GetDC(g.hwnd);
	HGDIOBJ old = SelectObject(dc, font);
	SIZE size = { 0, 0 };
	GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
	SelectObject(dc, old);
	ReleaseDC(g.hwnd, dc);
	return size.cx;
}

static int font_height(HFONT font)
{
	HDC dc = GetDC(g.hwnd);
	HGDIOBJ old = SelectObject(dc, font);
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	ReleaseDC(g.hwnd, dc);
	return tm.tmHeight;
}

static const wchar_t *extension_of(const wchar_t *path)
{
	const wchar_t *base = mp_path_basename(path), *dot = NULL, *p;
	for (p = base; *p != 0; p++) {
		if (*p == L'.')
			dot = p;
	}
	return dot ? dot : L"";
}

static int is_playlist_file(const wchar_t *path)
{
	const wchar_t *ext = extension_of(path);
	return mp_wascii_ieq(ext, L".m3u") || mp_wascii_ieq(ext, L".m3u8") || mp_wascii_ieq(ext, L".pls");
}

static int is_audio_file(const wchar_t *path)
{
	const wchar_t *ext = extension_of(path);
	return mp_wascii_ieq(ext, L".mp3") || mp_wascii_ieq(ext, L".flac") || mp_wascii_ieq(ext, L".wav") ||
		mp_wascii_ieq(ext, L".wave") || mp_wascii_ieq(ext, L".fla") || mp_wascii_ieq(ext, L".mp2");
}

/* ---- Playlist <-> list view --------------------------------------------- */

static void refresh_list(void)
{
	/* NOSCROLL keeps the view where it was when tracks are appended. */
	ListView_SetItemCountEx(g.list, (int)g.pl.count, LVSICF_NOSCROLL);
	InvalidateRect(g.list, NULL, FALSE);
}

/* Reads the file's tags and appends it with its display title/artist. */
static int add_track(const wchar_t *path)
{
	MpTags tags;
	wchar_t title[MP_TAG_MAX], artist[MP_TAG_MAX];
	mp_tags_read_file(path, &tags);
	mp_track_display(&tags, path, title, MP_TAG_MAX, artist, MP_TAG_MAX);
	return mp_playlist_add(&g.pl, path, title, artist);
}

/* Appends every track listed in a playlist file. Returns the number added,
 * or -1 if the file could not be read. */
static int add_playlist_file(const wchar_t *path)
{
	uint8_t *data;
	size_t size, i;
	MpPathList paths;
	int added = 0;
	if (!mp_file_read_all(path, PLAYLIST_FILE_MAX, &data, &size))
		return -1;
	memset(&paths, 0, sizeof(paths));
	if (mp_playlist_parse(data, size, path, &paths)) {
		for (i = 0; i < paths.count; i++)
			added += add_track(paths.paths[i]);
	}
	mp_pathlist_free(&paths);
	free(data);
	return added;
}

/* ---- Now playing -------------------------------------------------------- */

static void update_play_button(int force)
{
	MpPlayerState state = mp_player_state(g.player);
	if (force || (state == MP_PLAYER_PLAYING) != (g.shown_state == MP_PLAYER_PLAYING)) {
		SetWindowTextW(g.btn_play, state == MP_PLAYER_PLAYING ? L"Pause" : L"Play");
		g.shown_state = state;
	}
}

static void update_position(void)
{
	uint32_t pos = mp_player_position_ms(g.player);
	uint32_t dur = mp_player_duration_ms(g.player);
	wchar_t a[32], b[32], text[80];
	int loaded = mp_player_state(g.player) != MP_PLAYER_EMPTY;

	/* While the user drags the thumb, leave it where they put it; the time
	 * label then previews the drag position instead (see WM_HSCROLL). */
	if (!g.dragging_seek) {
		SendMessageW(g.seek, TBM_SETPOS, TRUE, dur ? (LPARAM)((uint64_t)pos * SEEK_RANGE / dur) : 0);
		text[0] = 0;
		if (loaded) {
			mp_format_time(pos, a, 32);
			mp_wcopy(text, 80, a);
			if (dur) {
				mp_format_time(dur, b, 32);
				wcat(text, 80, L" / ");
				wcat(text, 80, b);
			}
		}
		SetWindowTextW(g.time, text);
	}
	/* Files with an unknown length (rare, streamed FLAC) cannot be seeked
	 * from a bar; the bar would have no scale. */
	EnableWindow(g.seek, loaded && dur > 0);
	update_play_button(0);
}

static void update_now_playing(void)
{
	wchar_t caption[MP_TAG_MAX * 2 + 32], status[128];
	const MpEntry *e = NULL;
	if (g.pl.current != MP_NONE && mp_player_state(g.player) != MP_PLAYER_EMPTY)
		e = &g.pl.items[g.pl.current];
	if (e != NULL) {
		SetWindowTextW(g.title, e->title);
		/* No artist in the metadata: the artist line is simply left blank. */
		SetWindowTextW(g.artist, e->artist);
		mp_wcopy(caption, sizeof(caption) / sizeof(caption[0]), e->title);
		if (e->artist[0] != 0) {
			wcat(caption, sizeof(caption) / sizeof(caption[0]), L" - ");
			wcat(caption, sizeof(caption) / sizeof(caption[0]), e->artist);
		}
		wcat(caption, sizeof(caption) / sizeof(caption[0]), L" - " APP_NAME);
		SetWindowTextW(g.hwnd, caption);
		wsprintfW(status, L"Track %d of %d", (int)g.pl.current + 1, (int)g.pl.count);
		set_status(status);
		ListView_EnsureVisible(g.list, (int)g.pl.current, FALSE);
	} else {
		SetWindowTextW(g.title, g.pl.count ? L"Nothing playing" : L"No tracks loaded");
		SetWindowTextW(g.artist, g.pl.count ? L"" : L"Use File > Open Files, or drop music files here.");
		SetWindowTextW(g.hwnd, APP_NAME);
	}
	/* The current row is drawn in bold (NM_CUSTOMDRAW). */
	InvalidateRect(g.list, NULL, FALSE);
	update_position();
	update_play_button(1);
}

/* Loads and starts one track. On failure shows why in the status bar. */
static int start_track(size_t index)
{
	wchar_t err[512], msg[MSG_MAX];
	if (index >= g.pl.count)
		return 0;
	if (!mp_player_load(g.player, g.pl.items[index].path, err, 512)) {
		/* Keep the failed track as "current" (nothing is loaded, so it is not
		 * shown as playing) so that Next goes on from here rather than
		 * jumping back to the top of the list. */
		g.pl.current = index;
		update_now_playing();
		mp_wcopy(msg, MSG_MAX, L"Could not play \"");
		wcat(msg, MSG_MAX, mp_path_basename(g.pl.items[index].path));
		wcat(msg, MSG_MAX, L"\": ");
		wcat(msg, MSG_MAX, err);
		set_status(msg);
		return 0;
	}
	g.pl.current = index;
	mp_player_play(g.player);
	update_now_playing();
	return 1;
}

/*
 * Plays `index`, and if that track cannot be played, keeps going in
 * `direction` (0 = don't) until something plays or every track has been
 * tried once - so one missing file in a playlist does not stop the music.
 */
static void play_from(size_t index, int direction, int wrap)
{
	size_t tries;
	for (tries = 0; index != MP_NONE && tries < g.pl.count; tries++) {
		if (start_track(index)) {
			/* start_track replaced the error with "Track n of m"; say that
			 * something was skipped, or it looks like a track vanished. */
			if (tries > 0) {
				wchar_t status[160];
				wsprintfW(status, L"Track %d of %d (skipped %d track(s) that could not be played)",
					(int)index + 1, (int)g.pl.count, (int)tries);
				set_status(status);
			}
			return;
		}
		if (direction == 0)
			return;
		index = mp_playlist_step(&g.pl, index, direction, wrap);
	}
}

static void skip(int direction)
{
	size_t next;
	if (g.pl.count == 0)
		return;
	/* The buttons always wrap around; only automatic advancing at the end
	 * of the list obeys Repeat. */
	next = mp_playlist_step(&g.pl, g.pl.current, direction, 1);
	play_from(next, direction, 1);
}

static size_t selected_or_first(void)
{
	int sel = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
	if (sel >= 0)
		return (size_t)sel;
	return g.pl.count ? 0 : MP_NONE;
}

static void play_pause(void)
{
	switch (mp_player_state(g.player)) {
	case MP_PLAYER_PLAYING:
		mp_player_pause(g.player);
		set_status(L"Paused");
		break;
	case MP_PLAYER_PAUSED:
	case MP_PLAYER_STOPPED:
		mp_player_play(g.player);
		update_now_playing();
		break;
	default:
		play_from(selected_or_first(), 1, 1);
		break;
	}
	update_play_button(1);
}

static void stop(void)
{
	if (mp_player_state(g.player) == MP_PLAYER_EMPTY)
		return;
	mp_player_stop(g.player);
	set_status(L"Stopped");
	update_position();
}

static void on_track_end(UINT generation)
{
	size_t next;
	/* A notice from a track the user already skipped away from. */
	if (generation != mp_player_generation(g.player))
		return;
	next = mp_playlist_step(&g.pl, g.pl.current, 1, g.repeat);
	if (next == MP_NONE) {
		update_position();
		set_status(L"End of playlist");
		return;
	}
	play_from(next, 1, g.repeat);
}

/* ---- Volume ------------------------------------------------------------- */

/* Pushes g.volume/g.muted out to the player, the slider, its label and the
 * Mute menu check, so every way of changing the volume looks the same. */
static void apply_volume(void)
{
	wchar_t text[32];
	mp_player_set_volume(g.player, g.muted ? 0 : g.volume);
	SendMessageW(g.vol_bar, TBM_SETPOS, TRUE, g.volume);
	if (g.muted)
		mp_wcopy(text, 32, L"Muted");
	else
		wsprintfW(text, L"Volume %d%%", g.volume);
	SetWindowTextW(g.vol_label, text);
	CheckMenuItem(GetMenu(g.hwnd), IDM_MUTE, g.muted ? MF_CHECKED : MF_UNCHECKED);
}

/* Any deliberate volume change unmutes, as in Windows' own volume control:
 * otherwise turning it up while muted would seem to do nothing. */
static void set_volume(int percent)
{
	g.volume = mp_volume_clamp(percent);
	g.muted = 0;
	apply_volume();
}

static void toggle_mute(void)
{
	g.muted = !g.muted;
	apply_volume();
}

static void on_volume_scroll(void)
{
	int pos = (int)SendMessageW(g.vol_bar, TBM_GETPOS, 0, 0);
	/* Unlike seeking, applied on every notification, including each step of
	 * a drag, so the level can be set by ear. Only a real change counts:
	 * the TB_ENDTRACK that ends every gesture, or clicking the thumb
	 * without moving it, must not unmute. */
	if (pos != g.volume)
		set_volume(pos);
}

/* ---- File dialogs ------------------------------------------------------- */

/* Shows the Open dialog (multi-select) and appends the chosen paths. */
static int pick_audio_files(MpPathList *out)
{
	OPENFILENAMEW ofn;
	wchar_t *buf = (wchar_t *)calloc(PICK_BUFFER_CHARS, sizeof(wchar_t));
	wchar_t *dir, *name, full[MAX_PATH * 2];
	int ok;
	if (buf == NULL)
		return 0;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g.hwnd;
	ofn.lpstrFilter =
		L"Audio Files (*.mp3; *.flac; *.wav)\0*.mp3;*.flac;*.wav\0"
		L"MP3 Files (*.mp3)\0*.mp3\0"
		L"FLAC Files (*.flac)\0*.flac\0"
		L"WAV Files (*.wav)\0*.wav\0"
		L"All Files (*.*)\0*.*\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = PICK_BUFFER_CHARS;
	ofn.lpstrTitle = L"Open Music Files";
	/* OFN_DONTADDTORECENT: do not put every song into the Start menu's
	 * Recent Documents. */
	ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
		OFN_HIDEREADONLY | OFN_DONTADDTORECENT;
	if (!GetOpenFileNameW(&ofn)) {
		if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
			MessageBoxW(g.hwnd, L"Too many files were selected at once. Please add them in smaller groups.",
				APP_NAME, MB_OK | MB_ICONWARNING);
		free(buf);
		return 0;
	}
	/* One file: buf holds its full path. Several: buf holds the folder,
	 * then each file name, each NUL-terminated, then an extra NUL. */
	dir = buf;
	name = buf + wcslen(buf) + 1;
	ok = 1;
	if (*name == 0) {
		ok = mp_pathlist_add(out, dir);
	} else {
		for (; *name != 0 && ok; name += wcslen(name) + 1) {
			mp_wcopy(full, MAX_PATH * 2, dir);
			/* The folder is "C:\" (with the slash) when it is a drive root. */
			if (full[0] != 0 && full[wcslen(full) - 1] != L'\\')
				wcat(full, MAX_PATH * 2, L"\\");
			wcat(full, MAX_PATH * 2, name);
			ok = mp_pathlist_add(out, full);
		}
	}
	free(buf);
	return ok && out->count > 0;
}

static int pick_playlist_file(wchar_t *path, size_t cap, int save)
{
	OPENFILENAMEW ofn;
	path[0] = 0;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g.hwnd;
	ofn.lpstrFile = path;
	ofn.nMaxFile = (DWORD)cap;
	if (save) {
		ofn.lpstrFilter = L"M3U8 Playlist (*.m3u8)\0*.m3u8\0";
		ofn.lpstrDefExt = L"m3u8";
		ofn.lpstrTitle = L"Save Playlist";
		ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
			OFN_DONTADDTORECENT;
		return GetSaveFileNameW(&ofn) != 0;
	}
	ofn.lpstrFilter =
		L"Playlists (*.m3u; *.m3u8; *.pls)\0*.m3u;*.m3u8;*.pls\0"
		L"All Files (*.*)\0*.*\0";
	ofn.lpstrTitle = L"Open Playlist";
	ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_DONTADDTORECENT;
	return GetOpenFileNameW(&ofn) != 0;
}

/* ---- Commands ----------------------------------------------------------- */

static void clear_playlist(void)
{
	mp_player_unload(g.player);
	mp_playlist_clear(&g.pl);
	refresh_list();
	update_now_playing();
	set_status(L"");
}

/* Adds a mix of audio files and playlist files (from the Open dialog, drag
 * and drop, or the command line). Returns the index of the first track
 * added, or MP_NONE. */
static size_t add_paths(wchar_t **paths, size_t count)
{
	size_t i, first = g.pl.count, skipped = 0;
	HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
	for (i = 0; i < count; i++) {
		if (is_playlist_file(paths[i])) {
			if (add_playlist_file(paths[i]) < 0)
				skipped++;
		} else if (is_audio_file(paths[i])) {
			add_track(paths[i]);
		} else {
			/* Unknown extension: accept it only if the content really is
			 * one of our formats, so "All Files" picks still work. */
			MpStream *s = mp_stream_open_file(paths[i], NULL);
			if (s != NULL && mp_detect_format(s, NULL) != MP_FORMAT_UNKNOWN)
				add_track(paths[i]);
			else
				skipped++;
			mp_stream_close(s);
		}
	}
	SetCursor(old);
	refresh_list();
	if (skipped > 0) {
		wchar_t msg[128];
		wsprintfW(msg, L"%d item(s) could not be added (not a playlist, MP3, FLAC or WAV file).", (int)skipped);
		set_status(msg);
	}
	return g.pl.count > first ? first : MP_NONE;
}

static void cmd_open_files(int replace)
{
	MpPathList paths;
	size_t first;
	memset(&paths, 0, sizeof(paths));
	if (!pick_audio_files(&paths))
		return;
	if (replace)
		clear_playlist();
	first = add_paths(paths.paths, paths.count);
	mp_pathlist_free(&paths);
	/* Open plays right away; Add only plays if nothing is loaded yet. */
	if (first != MP_NONE && (replace || mp_player_state(g.player) == MP_PLAYER_EMPTY))
		play_from(first, 1, 0);
	else
		update_now_playing();
}

static void cmd_open_playlist(void)
{
	wchar_t path[MAX_PATH * 2];
	int added;
	if (!pick_playlist_file(path, MAX_PATH * 2, 0))
		return;
	clear_playlist();
	added = add_playlist_file(path);
	refresh_list();
	if (added < 0) {
		MessageBoxW(g.hwnd, L"The playlist file could not be read.", APP_NAME, MB_OK | MB_ICONERROR);
	} else if (added == 0) {
		set_status(L"The playlist does not list any tracks.");
	} else {
		play_from(0, 1, 0);
	}
}

static void cmd_save_playlist(void)
{
	wchar_t path[MAX_PATH * 2];
	char *data;
	size_t size;
	if (g.pl.count == 0) {
		MessageBoxW(g.hwnd, L"The playlist is empty, so there is nothing to save.", APP_NAME,
			MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (!pick_playlist_file(path, MAX_PATH * 2, 1))
		return;
	if (!mp_playlist_write_m3u8(&g.pl, path, &data, &size) || !mp_file_write_all(path, data, size)) {
		MessageBoxW(g.hwnd, L"The playlist could not be saved.", APP_NAME, MB_OK | MB_ICONERROR);
	} else {
		set_status(L"Playlist saved.");
	}
	free(data);
}

static void cmd_remove_selected(void)
{
	int *rows, n = 0, i, sel = -1, total = ListView_GetSelectedCount(g.list);
	int removed_current = 0;
	if (total <= 0)
		return;
	rows = (int *)malloc((size_t)total * sizeof(int));
	if (rows == NULL)
		return;
	while (n < total && (sel = ListView_GetNextItem(g.list, sel, LVNI_SELECTED)) >= 0)
		rows[n++] = sel;
	/* Highest first, so the remaining indexes stay valid as we go. */
	for (i = n - 1; i >= 0; i--) {
		if ((size_t)rows[i] == g.pl.current)
			removed_current = 1;
		mp_playlist_remove(&g.pl, (size_t)rows[i]);
	}
	free(rows);
	if (removed_current)
		mp_player_unload(g.player);
	ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED);
	refresh_list();
	update_now_playing();
}

static void cmd_about(void)
{
	MessageBoxW(g.hwnd,
		APP_NAME L" " MP_VERSION_WSTRING L"\n\n"
		L"Plays MP3, FLAC and WAV files and M3U, M3U8 and PLS playlists.\n\n"
		L"Audio decoding by dr_mp3, dr_flac and dr_wav by David Reid "
		L"(public domain / MIT No Attribution).\n\n"
		L"Copyright (c) 2026 jeffcrone. MIT No Attribution License.",
		L"About " APP_NAME, MB_OK | MB_ICONINFORMATION);
}

static void on_command(int id)
{
	switch (id) {
	case IDM_OPEN_FILES: cmd_open_files(1); break;
	case IDM_ADD_FILES:
	case IDC_BTN_ADD: cmd_open_files(0); break;
	case IDM_OPEN_PLAYLIST:
	case IDC_BTN_PLAYLIST: cmd_open_playlist(); break;
	case IDM_SAVE_PLAYLIST: cmd_save_playlist(); break;
	case IDM_REMOVE_SELECTED: cmd_remove_selected(); break;
	case IDM_CLEAR_PLAYLIST: clear_playlist(); break;
	case IDM_EXIT: DestroyWindow(g.hwnd); break;
	case IDM_PLAY_PAUSE:
	case IDC_BTN_PLAY: play_pause(); break;
	case IDM_STOP:
	case IDC_BTN_STOP: stop(); break;
	case IDM_PREVIOUS:
	case IDC_BTN_PREV: skip(-1); break;
	case IDM_NEXT:
	case IDC_BTN_NEXT: skip(1); break;
	case IDM_REPEAT:
		g.repeat = !g.repeat;
		CheckMenuItem(GetMenu(g.hwnd), IDM_REPEAT, g.repeat ? MF_CHECKED : MF_UNCHECKED);
		break;
	case IDM_VOLUME_UP: set_volume(g.volume + VOLUME_STEP); break;
	case IDM_VOLUME_DOWN: set_volume(g.volume - VOLUME_STEP); break;
	case IDM_MUTE: toggle_mute(); break;
	case IDM_ABOUT: cmd_about(); break;
	case IDOK:
		/* IsDialogMessage turns Enter into IDOK. In the list it means "play
		 * this one", like double-clicking. */
		if (GetFocus() == g.list) {
			int focused = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
			if (focused >= 0)
				play_from((size_t)focused, 0, 0);
		}
		break;
	default:
		break;
	}
}

/* ---- Layout ------------------------------------------------------------- */

/* Buttons are sized to their (longest) caption, which matters for the
 * Play/Pause button: it must not change size when its text changes. */
static int button_width(const wchar_t *longest_text)
{
	int w = text_width(g.font, longest_text) + g.unit * 2;
	return w < g.unit * 5 ? g.unit * 5 : w;
}

/* The volume label is sized for its widest text, so it does not resize (and
 * shift the seek bar) as the numbers change. */
static int volume_label_width(void)
{
	int a = text_width(g.font, L"Volume 100%"), b = text_width(g.font, L"Muted");
	return (a > b ? a : b) + g.unit / 2;
}

static int volume_bar_width(void)
{
	return g.unit * 6;
}

static int time_width(void)
{
	return text_width(g.font, L"00:00:00 / 00:00:00") + g.unit;
}

static void layout(void)
{
	RECT rc, sr;
	int m = g.unit * 2 / 3, gap = g.unit / 2;
	int x, y, w, h, status_h, btn_h, time_w, vl_w, vb_w, seek_w;
	int wp = button_width(L"Previous"), wplay = button_width(L"Pause");
	int ws = button_width(L"Stop"), wn = button_width(L"Next");
	int wa = button_width(L"Add Files..."), wl = button_width(L"Open Playlist...");

	GetClientRect(g.hwnd, &rc);
	/* The status bar positions itself on WM_SIZE; we only need its height. */
	SendMessageW(g.status, WM_SIZE, 0, 0);
	GetWindowRect(g.status, &sr);
	status_h = sr.bottom - sr.top;

	w = rc.right - 2 * m;
	x = m;
	y = m;
	MoveWindow(g.title, x, y, w, g.title_height, TRUE);
	y += g.title_height;
	MoveWindow(g.artist, x, y, w, g.unit + 2, TRUE);
	y += g.unit + 2 + gap;

	/* The seek row: seek bar (taking whatever width is left), time, then
	 * the volume label and slider at the right-hand end, where the volume
	 * sits in most players. */
	time_w = time_width();
	vl_w = volume_label_width();
	vb_w = volume_bar_width();
	seek_w = w - time_w - vl_w - vb_w;
	h = g.unit * 2;
	MoveWindow(g.seek, x, y, seek_w > 0 ? seek_w : 0, h, TRUE);
	MoveWindow(g.time, x + seek_w, y + (h - g.unit) / 2, time_w, g.unit + 2, TRUE);
	MoveWindow(g.vol_label, x + seek_w + time_w, y + (h - g.unit) / 2, vl_w, g.unit + 2, TRUE);
	MoveWindow(g.vol_bar, x + w - vb_w, y, vb_w, h, TRUE);
	y += h + gap;

	btn_h = g.unit * 2 - g.unit / 4;
	if (btn_h < 23)
		btn_h = 23;
	MoveWindow(g.btn_prev, x, y, wp, btn_h, TRUE);
	MoveWindow(g.btn_play, x + wp + gap, y, wplay, btn_h, TRUE);
	MoveWindow(g.btn_stop, x + wp + wplay + 2 * gap, y, ws, btn_h, TRUE);
	MoveWindow(g.btn_next, x + wp + wplay + ws + 3 * gap, y, wn, btn_h, TRUE);
	MoveWindow(g.btn_playlist, x + w - wl, y, wl, btn_h, TRUE);
	MoveWindow(g.btn_add, x + w - wl - gap - wa, y, wa, btn_h, TRUE);
	y += btn_h + gap;

	h = rc.bottom - status_h - m - y;
	MoveWindow(g.list, x, y, w, h > 0 ? h : 0, TRUE);
}

static int min_client_width(void)
{
	int gap = g.unit / 2, margins = g.unit * 4 / 3;
	int buttons = button_width(L"Previous") + button_width(L"Pause") + button_width(L"Stop") +
		button_width(L"Next") + button_width(L"Add Files...") +
		button_width(L"Open Playlist...") + 5 * gap + g.unit * 2;
	/* The seek row must also fit, with a seek bar still wide enough to use.
	 * The button row is normally the wider, but that depends on the font. */
	int seek_row = g.unit * 8 + time_width() + volume_label_width() + volume_bar_width();
	return (buttons > seek_row ? buttons : seek_row) + margins;
}

/* ---- Window creation ---------------------------------------------------- */

static HWND make_child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex_style, int id)
{
	HWND h = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
		g.hwnd, (HMENU)(INT_PTR)id, g.inst, NULL);
	SendMessageW(h, WM_SETFONT, (WPARAM)g.font, FALSE);
	return h;
}

static void add_column(int index, const wchar_t *name, int width, int fmt)
{
	LVCOLUMNW col;
	memset(&col, 0, sizeof(col));
	col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
	col.fmt = fmt;
	col.pszText = (LPWSTR)name;
	col.cx = width;
	ListView_InsertColumn(g.list, index, &col);
}

static void create_fonts(void)
{
	NONCLIENTMETRICSW ncm;
	LOGFONTW lf;
	/* _WIN32_WINNT is 0x0501, so this is the XP-sized structure (without
	 * iPaddedBorderWidth). Vista and later accept it too; the larger Vista
	 * size would make the call fail on XP. */
	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
		lf = ncm.lfMessageFont;
	} else {
		GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
	}
	g.font = CreateFontIndirectW(&lf);
	lf.lfWeight = FW_BOLD;
	g.bold_font = CreateFontIndirectW(&lf);
	lf.lfHeight = lf.lfHeight * 3 / 2;
	g.title_font = CreateFontIndirectW(&lf);
}

static void on_create(HWND hwnd)
{
	g.hwnd = hwnd;
	create_fonts();
	g.unit = font_height(g.font);
	g.title_height = font_height(g.title_font) + 2;

	g.title = make_child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, IDC_TITLE);
	SendMessageW(g.title, WM_SETFONT, (WPARAM)g.title_font, FALSE);
	g.artist = make_child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, IDC_ARTIST);
	g.seek = make_child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, IDC_SEEK);
	SendMessageW(g.seek, TBM_SETRANGE, FALSE, MAKELPARAM(0, SEEK_RANGE));
	SendMessageW(g.seek, TBM_SETPAGESIZE, 0, SEEK_RANGE / 20);
	SendMessageW(g.seek, TBM_SETLINESIZE, 0, SEEK_RANGE / 100);
	g.time = make_child(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX, 0, IDC_TIME);
	/* The label is created just before the slider on purpose: screen readers
	 * name an unlabeled control after the static text preceding it, so the
	 * slider is announced as "Volume 80%". Creation order is also the Tab
	 * order: seek bar, volume, then the buttons. */
	g.vol_label = make_child(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX, 0, IDC_VOLUME_LABEL);
	g.vol_bar = make_child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, IDC_VOLUME);
	SendMessageW(g.vol_bar, TBM_SETRANGE, FALSE, MAKELPARAM(0, MP_VOLUME_MAX));
	SendMessageW(g.vol_bar, TBM_SETLINESIZE, 0, VOLUME_STEP);
	SendMessageW(g.vol_bar, TBM_SETPAGESIZE, 0, VOLUME_STEP * 4);

	g.btn_prev = make_child(L"BUTTON", L"Previous", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_PREV);
	g.btn_play = make_child(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_PLAY);
	g.btn_stop = make_child(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_STOP);
	g.btn_next = make_child(L"BUTTON", L"Next", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_NEXT);
	g.btn_add = make_child(L"BUTTON", L"Add Files...", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_ADD);
	g.btn_playlist = make_child(L"BUTTON", L"Open Playlist...", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_PLAYLIST);

	g.list = make_child(WC_LISTVIEWW, L"",
		LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_LIST);
	ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
	add_column(0, L"#", g.unit * 3, LVCFMT_RIGHT);
	add_column(1, L"Title", g.unit * 14, LVCFMT_LEFT);
	add_column(2, L"Artist", g.unit * 10, LVCFMT_LEFT);
	add_column(3, L"File", g.unit * 14, LVCFMT_LEFT);

	g.status = make_child(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, 0, IDC_STATUS);

	DragAcceptFiles(hwnd, TRUE);
	SetTimer(hwnd, TIMER_POSITION, TIMER_INTERVAL_MS, NULL);
	layout();
	/* No update_now_playing() here: the player does not exist yet (see
	 * wWinMain), and the labels are filled in right after it does. */
}

/* ---- Notifications ------------------------------------------------------ */

static LRESULT on_list_notify(NMHDR *nm)
{
	switch (nm->code) {
	case LVN_GETDISPINFOW: {
		NMLVDISPINFOW *di = (NMLVDISPINFOW *)nm;
		/* The ListView copies the text before the next call, so a static
		 * buffer is fine for the one computed column. */
		static wchar_t number[16];
		size_t i = (size_t)di->item.iItem;
		if ((di->item.mask & LVIF_TEXT) && i < g.pl.count) {
			const MpEntry *e = &g.pl.items[i];
			switch (di->item.iSubItem) {
			case 0:
				wsprintfW(number, L"%d", (int)i + 1);
				di->item.pszText = number;
				break;
			case 1: di->item.pszText = e->title; break;
			case 2: di->item.pszText = e->artist; break;
			default: di->item.pszText = (LPWSTR)mp_path_basename(e->path); break;
			}
		}
		return 0;
	}
	case NM_DBLCLK: {
		NMITEMACTIVATE *ia = (NMITEMACTIVATE *)nm;
		if (ia->iItem >= 0)
			play_from((size_t)ia->iItem, 0, 0);
		return 0;
	}
	case NM_CUSTOMDRAW: {
		NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)nm;
		if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
			return CDRF_NOTIFYITEMDRAW;
		if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
			if ((size_t)cd->nmcd.dwItemSpec == g.pl.current &&
				mp_player_state(g.player) != MP_PLAYER_EMPTY) {
				SelectObject(cd->nmcd.hdc, g.bold_font);
				return CDRF_NEWFONT;
			}
		}
		return CDRF_DODEFAULT;
	}
	default:
		return 0;
	}
}

static void on_seek_scroll(int code)
{
	uint32_t dur = mp_player_duration_ms(g.player);
	int pos = (int)SendMessageW(g.seek, TBM_GETPOS, 0, 0);
	if (dur == 0)
		return;
	if (code == TB_THUMBTRACK) {
		wchar_t a[32], b[32], text[80];
		g.dragging_seek = 1;
		mp_format_time((uint32_t)((uint64_t)pos * dur / SEEK_RANGE), a, 32);
		mp_format_time(dur, b, 32);
		mp_wcopy(text, 80, a);
		wcat(text, 80, L" / ");
		wcat(text, 80, b);
		SetWindowTextW(g.time, text);
	} else if (code == TB_ENDTRACK) {
		/* Every way of moving the thumb (drag, click, arrow keys, Page
		 * Up/Down) ends with TB_ENDTRACK, so seeking only here means one
		 * seek per gesture instead of dozens during a drag. */
		g.dragging_seek = 0;
		mp_player_seek_ms(g.player, (uint32_t)((uint64_t)pos * dur / SEEK_RANGE));
		update_position();
	}
}

static void on_drop(HDROP drop)
{
	UINT i, n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
	MpPathList paths;
	size_t first;
	memset(&paths, 0, sizeof(paths));
	for (i = 0; i < n; i++) {
		wchar_t path[MAX_PATH * 2];
		if (DragQueryFileW(drop, i, path, MAX_PATH * 2))
			mp_pathlist_add(&paths, path);
	}
	DragFinish(drop);
	first = add_paths(paths.paths, paths.count);
	mp_pathlist_free(&paths);
	if (first != MP_NONE && mp_player_state(g.player) == MP_PLAYER_EMPTY)
		play_from(first, 1, 0);
	else
		update_now_playing();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		on_create(hwnd);
		return 0;
	case WM_SIZE:
		layout();
		return 0;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO *)lp;
		if (g.unit > 0) {
			RECT r = { 0, 0, min_client_width(), g.unit * 20 };
			AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_CONTROLPARENT);
			mm->ptMinTrackSize.x = r.right - r.left;
			mm->ptMinTrackSize.y = r.bottom - r.top;
		}
		return 0;
	}
	case WM_COMMAND:
		on_command(LOWORD(wp));
		return 0;
	case WM_NOTIFY:
		if (((NMHDR *)lp)->hwndFrom == g.list)
			return on_list_notify((NMHDR *)lp);
		return 0;
	case WM_HSCROLL:
		if ((HWND)lp == g.seek)
			on_seek_scroll(LOWORD(wp));
		else if ((HWND)lp == g.vol_bar)
			on_volume_scroll();
		return 0;
	case WM_TIMER:
		if (wp == TIMER_POSITION)
			update_position();
		return 0;
	case WM_APP_TRACK_END:
		on_track_end((UINT)wp);
		return 0;
	case WM_DROPFILES:
		on_drop((HDROP)wp);
		return 0;
	case WM_APPCOMMAND:
		/* Keyboard media keys, when this window has the focus. */
		switch (GET_APPCOMMAND_LPARAM(lp)) {
		case APPCOMMAND_MEDIA_PLAY_PAUSE: play_pause(); return TRUE;
		case APPCOMMAND_MEDIA_STOP: stop(); return TRUE;
		case APPCOMMAND_MEDIA_NEXTTRACK: skip(1); return TRUE;
		case APPCOMMAND_MEDIA_PREVIOUSTRACK: skip(-1); return TRUE;
		default: break;
		}
		break;
	case WM_DESTROY:
		KillTimer(hwnd, TIMER_POSITION);
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Opens anything given on the command line (e.g. files dropped onto the
 * exe's icon in Explorer). */
static void open_command_line(void)
{
	int argc = 0, i;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	MpPathList paths;
	size_t first;
	if (argv == NULL)
		return;
	memset(&paths, 0, sizeof(paths));
	for (i = 1; i < argc; i++) {
		wchar_t full[MAX_PATH * 2];
		/* Relative paths are relative to wherever we were started from. */
		if (GetFullPathNameW(argv[i], MAX_PATH * 2, full, NULL))
			mp_pathlist_add(&paths, full);
	}
	LocalFree(argv);
	if (paths.count > 0) {
		first = add_paths(paths.paths, paths.count);
		if (first != MP_NONE)
			play_from(first, 1, 0);
	}
	mp_pathlist_free(&paths);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
	INITCOMMONCONTROLSEX icc;
	WNDCLASSEXW wc;
	MSG msg;
	RECT r;
	(void)prev;
	(void)cmd;

	g.inst = inst;
	mp_playlist_init(&g.pl);

	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);

	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	/* LoadImage at the exact system sizes picks the matching image from the
	 * .ico instead of scaling the 32x32 one. */
	wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
		GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
	wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	/* The standard dialog face color, like every other Windows utility. */
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
	wc.lpszClassName = WINDOW_CLASS;
	if (!RegisterClassExW(&wc))
		return 1;

	/* The player needs a window to notify, and the window's WM_CREATE needs
	 * the player, so create the window hidden, then the player. */
	g.hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, WINDOW_CLASS, APP_NAME,
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
		NULL, NULL, inst, NULL);
	if (g.hwnd == NULL)
		return 1;
	g.player = mp_player_create(g.hwnd, WM_APP_TRACK_END);
	if (g.player == NULL) {
		MessageBoxW(g.hwnd, L"The audio engine could not be started.", APP_NAME, MB_OK | MB_ICONERROR);
		return 1;
	}
	/* Full volume: the files play exactly as they are, as before this
	 * program had a volume control. */
	set_volume(MP_VOLUME_MAX);
	update_now_playing();

	/* A comfortable starting size in font units, so it looks the same at
	 * every DPI. */
	r.left = 0;
	r.top = 0;
	r.right = g.unit * 44;
	r.bottom = g.unit * 34;
	AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_CONTROLPARENT);
	SetWindowPos(g.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);

	g.accel = LoadAcceleratorsW(inst, MAKEINTRESOURCEW(IDR_ACCEL));
	ShowWindow(g.hwnd, show);
	UpdateWindow(g.hwnd);
	SetFocus(g.list);
	open_command_line();

	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (TranslateAcceleratorW(g.hwnd, g.accel, &msg))
			continue;
		/* Gives Tab / Shift+Tab / arrow-key navigation between controls. */
		if (IsDialogMessageW(g.hwnd, &msg))
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	mp_player_destroy(g.player);
	mp_playlist_free(&g.pl);
	DeleteObject(g.font);
	DeleteObject(g.bold_font);
	DeleteObject(g.title_font);
	return (int)msg.wParam;
}
