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
#include "glyph.h"
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
/* Scrolls the list while a dragged track is held above or below it. */
#define TIMER_DRAG_SCROLL 2
#define DRAG_SCROLL_MS 60

/* The playlist's columns, in display order. */
enum { COL_NUMBER, COL_TRACK, COL_TITLE, COL_ARTIST, COL_FILE };

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
	HWND speed_label, speed_box;
	HWND btn_prev, btn_play, btn_stop, btn_next, btn_add, btn_playlist;
	HFONT font, title_font, bold_font;
	HICON icons[MP_GLYPH_COUNT]; /* the playback buttons' symbols */
	int icon_size;
	/* The same symbols for the Playback menu. All NULL on Windows XP, which
	 * shows that menu as text only (see create_menu_bitmaps). */
	HBITMAP menu_bitmaps[MP_GLYPH_COUNT];
	HACCEL accel;
	int unit;       /* message font line height in pixels */
	int title_height;

	MpPlayer *player;
	MpPlaylist pl;
	int repeat;
	int dragging_seek;

	/* The column the list was last sorted by (with its header arrow), or -1
	 * once the order has been changed by anything else. It is a record of
	 * what was done, not a live sorted view: the tracks can be dragged
	 * around afterwards, and new ones are appended at the end. */
	int sort_column;
	int sort_desc;

	/* Dragging tracks to reorder them. The selection moves with the mouse
	 * as it goes; the order from before the drag is kept so Escape can put
	 * everything back. */
	int dragging;
	size_t drag_offset;         /* the grabbed track's place in the selection */
	POINT drag_pt;              /* last mouse position, list client coordinates */
	MpEntry *drag_saved;        /* the entries as they were (shallow copies) */
	size_t drag_saved_current;
	unsigned char *drag_saved_sel;
	int drag_saved_focus;
	/* The slider's level, kept while muted so unmuting goes back to it. The
	 * player itself is only ever told the level actually heard. Neither is
	 * saved between runs: the player keeps no settings (see RUNNING.md). */
	int volume;
	int muted;
	int speed_index; /* into `speeds`; also not saved between runs */
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
	return mp_playlist_add(&g.pl, path, title, artist, tags.track);
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

static void set_menu_bitmap(UINT id, HBITMAP bmp);

static void update_play_button(int force)
{
	MpPlayerState state = mp_player_state(g.player);
	if (force || (state == MP_PLAYER_PLAYING) != (g.shown_state == MP_PLAYER_PLAYING)) {
		int playing = state == MP_PLAYER_PLAYING;
		SetWindowTextW(g.btn_play, playing ? L"Pause" : L"Play");
		/* The symbol says what pressing the button will do, like the text. */
		SendMessageW(g.btn_play, BM_SETIMAGE, IMAGE_ICON,
			(LPARAM)g.icons[playing ? MP_GLYPH_PAUSE : MP_GLYPH_PLAY]);
		/* The menu item is always called "Play/Pause", but its symbol
		 * follows the button's, so the two never disagree. */
		set_menu_bitmap(IDM_PLAY_PAUSE, g.menu_bitmaps[playing ? MP_GLYPH_PAUSE : MP_GLYPH_PLAY]);
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

/* ---- Speed -------------------------------------------------------------- */

/* The speeds on offer, as percentages (see player.h), with their labels.
 * The menu IDs are IDM_SPEED_25 + index. */
static const int speeds[] = { 25, 50, 75, 100, 125, 150, 175, 200, 300 };
static const wchar_t *const speed_names[] = {
	L"0.25x", L"0.5x", L"0.75x", L"1x", L"1.25x", L"1.5x", L"1.75x", L"2x", L"3x"
};
#define SPEED_COUNT ((int)(sizeof(speeds) / sizeof(speeds[0])))
#define SPEED_NORMAL 3 /* 1x */

/* Sets the speed and shows it in both places it can be chosen: the drop-
 * down and the Playback > Speed menu (as a radio check). */
static void set_speed_index(int index)
{
	if (index < 0 || index >= SPEED_COUNT)
		return;
	g.speed_index = index;
	mp_player_set_speed(g.player, speeds[index]);
	SendMessageW(g.speed_box, CB_SETCURSEL, (WPARAM)index, 0);
	CheckMenuRadioItem(GetMenu(g.hwnd), IDM_SPEED_25, IDM_SPEED_25 + SPEED_COUNT - 1,
		IDM_SPEED_25 + index, MF_BYCOMMAND);
	/* The position was just recalculated at the new speed. */
	update_position();
}

/*
 * The drop-down's notifications. While its list is open, moving through it
 * (with the arrow keys or the mouse) already sends CBN_SELCHANGE for every
 * item passed; each change of speed restarts the audio from the current
 * point, so applying those would stutter. So a change is applied when the
 * list closes (CBN_CLOSEUP; if it was canceled the selection is back where
 * it was and nothing changes), or straight away when the list is closed
 * and the arrow keys step the selection.
 */
static void on_speed_box(int code)
{
	int sel = (int)SendMessageW(g.speed_box, CB_GETCURSEL, 0, 0);
	if (sel == CB_ERR || sel == g.speed_index)
		return;
	if (code == CBN_CLOSEUP ||
		(code == CBN_SELCHANGE && !SendMessageW(g.speed_box, CB_GETDROPPEDSTATE, 0, 0)))
		set_speed_index(sel);
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

/* ---- Reordering --------------------------------------------------------- */

/* One flag per playlist entry, 1 where the list shows it selected. Returns
 * NULL for an empty list or if out of memory; free() it. */
static unsigned char *selection_flags(void)
{
	unsigned char *flags;
	int i = -1;
	if (g.pl.count == 0)
		return NULL;
	flags = (unsigned char *)calloc(g.pl.count, 1);
	if (flags == NULL)
		return NULL;
	while ((i = ListView_GetNextItem(g.list, i, LVNI_SELECTED)) >= 0) {
		if ((size_t)i < g.pl.count)
			flags[i] = 1;
	}
	return flags;
}

/* Selects exactly the flagged rows and puts the focus (and the Shift+click
 * anchor) on `focus`, if it is a row. The list is owner-data, so it keeps
 * the selection by row number: after the tracks move, the selection has to
 * be moved to match or it would stay on the old row numbers. */
static void apply_selection(const unsigned char *flags, int focus)
{
	size_t i;
	ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	for (i = 0; i < g.pl.count; i++) {
		if (flags[i])
			ListView_SetItemState(g.list, (int)i, LVIS_SELECTED, LVIS_SELECTED);
	}
	if (focus >= 0 && (size_t)focus < g.pl.count) {
		ListView_SetItemState(g.list, focus, LVIS_FOCUSED, LVIS_FOCUSED);
		ListView_SetSelectionMark(g.list, focus);
	}
}

/* The playing track's number in the list changes when tracks move. The
 * status bar only shows it while playing; "Paused", "Stopped" and messages
 * about files are left alone. */
static void update_track_status(void)
{
	wchar_t status[64];
	if (g.pl.current == MP_NONE || mp_player_state(g.player) != MP_PLAYER_PLAYING)
		return;
	wsprintfW(status, L"Track %d of %d", (int)g.pl.current + 1, (int)g.pl.count);
	set_status(status);
}

/* Shows the sort arrow on g.sort_column's header, and on no other. The
 * arrows are Common Controls 6 header formats, available from XP on. */
static void show_sort_arrow(void)
{
	HWND header = ListView_GetHeader(g.list);
	int i, n = Header_GetItemCount(header);
	for (i = 0; i < n; i++) {
		HDITEMW hd;
		memset(&hd, 0, sizeof(hd));
		hd.mask = HDI_FORMAT;
		if (!Header_GetItem(header, i, &hd))
			continue;
		hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
		if (i == g.sort_column)
			hd.fmt |= g.sort_desc ? HDF_SORTDOWN : HDF_SORTUP;
		Header_SetItem(header, i, &hd);
	}
}

/* The order is no longer the sorted one: drop the arrow, so the next click
 * on that header sorts ascending again rather than reversing. */
static void forget_sort(void)
{
	if (g.sort_column >= 0) {
		g.sort_column = -1;
		show_sort_arrow();
	}
}

static void end_drag(int cancel);

/* Anything that changes the list (adding, removing, sorting, Move Up/Down)
 * first ends a drag in progress, keeping what it did, so the drag never
 * works on indexes that have changed under it. */
static void finish_drag(void)
{
	end_drag(0);
}

static void sort_by_column(int column)
{
	MpSortKey key;
	unsigned char *sel, *moved;
	size_t *new_index, i;
	int focus, desc;
	switch (column) {
	case COL_TRACK: key = MP_SORT_TRACK; break;
	case COL_TITLE: key = MP_SORT_TITLE; break;
	case COL_ARTIST: key = MP_SORT_ARTIST; break;
	case COL_FILE: key = MP_SORT_FILE; break;
	default: return; /* "#" is the position in the list: nothing to sort by */
	}
	finish_drag();
	if (g.pl.count == 0)
		return;
	/* A second click on the same header reverses it, as in Explorer. */
	desc = column == g.sort_column ? !g.sort_desc : 0;
	sel = selection_flags();
	moved = (unsigned char *)calloc(g.pl.count, 1);
	new_index = (size_t *)malloc(g.pl.count * sizeof(size_t));
	focus = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
	if (sel != NULL && moved != NULL && new_index != NULL &&
		mp_playlist_sort(&g.pl, key, desc, new_index)) {
		/* The same tracks stay selected, wherever they went. */
		for (i = 0; i < g.pl.count; i++) {
			if (sel[i])
				moved[new_index[i]] = 1;
		}
		focus = focus >= 0 ? (int)new_index[focus] : -1;
		apply_selection(moved, focus);
		g.sort_column = column;
		g.sort_desc = desc;
		show_sort_arrow();
		refresh_list();
		if (focus >= 0)
			ListView_EnsureVisible(g.list, focus, FALSE);
		update_track_status();
	}
	free(sel);
	free(moved);
	free(new_index);
}

/* Move Up / Move Down: the selected tracks, one place. */
static void cmd_move_selected(int direction)
{
	unsigned char *sel;
	size_t i, first = MP_NONE, last = 0;
	finish_drag();
	sel = selection_flags();
	if (sel == NULL)
		return;
	if (mp_playlist_shift(&g.pl, sel, direction) > 0) {
		for (i = 0; i < g.pl.count; i++) {
			if (sel[i]) {
				if (first == MP_NONE)
					first = i;
				last = i;
			}
		}
		/* The focus goes to the leading edge of the selection, and that
		 * is what is scrolled into view, so repeated presses can be
		 * followed all the way to the end of a long list. */
		apply_selection(sel, direction < 0 ? (int)first : (int)last);
		ListView_EnsureVisible(g.list, direction < 0 ? (int)first : (int)last, FALSE);
		forget_sort();
		refresh_list();
		update_track_status();
	}
	free(sel);
}

/* Whether Move Up / Move Down (and Remove) have anything to do, for
 * graying out the menu items. Move Up can do something when some unselected
 * track sits above the last selected one: that is, when not all the rows
 * down to the last selected one are selected. Move Down likewise. */
static void update_edit_menu(void)
{
	HMENU menu = GetMenu(g.hwnd);
	int n = ListView_GetSelectedCount(g.list), first = -1, last = -1, i = -1;
	int can_up, can_down;
	while ((i = ListView_GetNextItem(g.list, i, LVNI_SELECTED)) >= 0) {
		if (first < 0)
			first = i;
		last = i;
	}
	can_up = n > 0 && last + 1 > n;
	can_down = n > 0 && first < (int)g.pl.count - n;
	EnableMenuItem(menu, IDM_MOVE_UP, MF_BYCOMMAND | (can_up ? MF_ENABLED : MF_GRAYED));
	EnableMenuItem(menu, IDM_MOVE_DOWN, MF_BYCOMMAND | (can_down ? MF_ENABLED : MF_GRAYED));
	EnableMenuItem(menu, IDM_REMOVE_SELECTED, MF_BYCOMMAND | (n > 0 ? MF_ENABLED : MF_GRAYED));
}

/* Called on LVN_BEGINDRAG: the list has seen the mouse go down on a row and
 * move far enough to count as a drag. From here the main window captures
 * the mouse and does the rest (drag_move, end_drag). */
static void begin_drag(int item)
{
	unsigned char *sel;
	size_t i, rank = 0;
	if (g.dragging || item < 0 || (size_t)item >= g.pl.count)
		return;
	sel = selection_flags();
	if (sel == NULL)
		return;
	/* The list normally selects a row as the mouse goes down on it, so the
	 * grabbed row is already selected (along with any others, which then
	 * come along). If it is not, drag just that row, as Explorer does. */
	if (!sel[item]) {
		memset(sel, 0, g.pl.count);
		sel[item] = 1;
		apply_selection(sel, item);
	}
	for (i = 0; i < (size_t)item; i++)
		rank += sel[i];
	g.drag_saved = (MpEntry *)malloc(g.pl.count * sizeof(MpEntry));
	if (g.drag_saved == NULL) {
		free(sel);
		return;
	}
	memcpy(g.drag_saved, g.pl.items, g.pl.count * sizeof(MpEntry));
	g.drag_saved_current = g.pl.current;
	g.drag_saved_sel = sel;
	g.drag_saved_focus = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
	g.drag_offset = rank;
	g.dragging = 1;
	SetCapture(g.hwnd);
	/* While the mouse is captured nobody asks for a cursor (no
	 * WM_SETCURSOR), so this one stays until the drag ends. */
	SetCursor(LoadCursor(NULL, IDC_SIZENS));
}

/* The row under list-client y, clamped to the list. Worked out from the
 * row height rather than by hit-testing, so it also works to the right of
 * the last column and above or below the rows. */
static int row_at(int y)
{
	RECT r;
	int top = ListView_GetTopIndex(g.list), h, row;
	if (g.pl.count == 0 || !ListView_GetItemRect(g.list, top, &r, LVIR_BOUNDS))
		return -1;
	h = r.bottom - r.top;
	if (h <= 0)
		return -1;
	row = y >= r.top ? top + (y - r.top) / h : top - 1 - (r.top - y - 1) / h;
	if (row < 0)
		row = 0;
	if ((size_t)row >= g.pl.count)
		row = (int)g.pl.count - 1;
	return row;
}

/* Moves the dragged tracks so the grabbed one is on the row under the
 * mouse, with the rest of the selection gathered around it. */
static void drag_move(void)
{
	size_t *idx, n = 0, to, i;
	unsigned char *flags;
	int row = row_at(g.drag_pt.y), sel = -1;
	int total = ListView_GetSelectedCount(g.list);
	if (row < 0 || total <= 0)
		return;
	idx = (size_t *)malloc((size_t)total * sizeof(size_t));
	flags = (unsigned char *)calloc(g.pl.count, 1);
	if (idx == NULL || flags == NULL) {
		free(idx);
		free(flags);
		return;
	}
	while ((int)n < total && (sel = ListView_GetNextItem(g.list, sel, LVNI_SELECTED)) >= 0)
		idx[n++] = (size_t)sel;
	to = (size_t)row < g.drag_offset ? 0 : (size_t)row - g.drag_offset;
	if (to > g.pl.count - n)
		to = g.pl.count - n;
	/* Already there (and gathered together): nothing to do, which is the
	 * usual case - most mouse moves stay within one row. */
	if (n > 0 && !(idx[0] == to && idx[n - 1] == to + n - 1) &&
		mp_playlist_move_block(&g.pl, idx, n, to)) {
		for (i = 0; i < n; i++)
			flags[to + i] = 1;
		apply_selection(flags, (int)(to + g.drag_offset));
		refresh_list();
		update_track_status();
	}
	free(idx);
	free(flags);
}

/* Holding the dragged tracks above the first visible row or below the list
 * scrolls it, a row at a time, for as long as the mouse stays there. */
static void drag_autoscroll(void)
{
	RECT client, first;
	int top = ListView_GetTopIndex(g.list), dir = 0;
	GetClientRect(g.list, &client);
	if (g.pl.count > 0 && ListView_GetItemRect(g.list, top, &first, LVIR_BOUNDS)) {
		/* The header sits inside the list's client area, so "above the
		 * list" means above the first visible row, not above the client. */
		if (g.drag_pt.y < first.top)
			dir = -1;
		else if (g.drag_pt.y >= client.bottom)
			dir = 1;
		if (dir != 0) {
			ListView_Scroll(g.list, 0, dir * (first.bottom - first.top));
			drag_move();
			return;
		}
	}
	KillTimer(g.hwnd, TIMER_DRAG_SCROLL);
}

static void on_drag_mouse_move(LPARAM lp)
{
	RECT client;
	g.drag_pt.x = (short)LOWORD(lp);
	g.drag_pt.y = (short)HIWORD(lp);
	MapWindowPoints(g.hwnd, g.list, &g.drag_pt, 1);
	drag_move();
	GetClientRect(g.list, &client);
	/* Start scrolling if the mouse has left the rows; the timer stops
	 * itself once it is back over them. */
	{
		RECT first;
		int top = ListView_GetTopIndex(g.list);
		if (ListView_GetItemRect(g.list, top, &first, LVIR_BOUNDS) &&
			(g.drag_pt.y < first.top || g.drag_pt.y >= client.bottom))
			SetTimer(g.hwnd, TIMER_DRAG_SCROLL, DRAG_SCROLL_MS, NULL);
	}
}

/* Ends a drag: keeping the new order, or (cancel) restoring the old one. */
static void end_drag(int cancel)
{
	size_t i;
	int changed = 0;
	if (!g.dragging)
		return;
	/* Cleared before ReleaseCapture, which sends WM_CAPTURECHANGED, which
	 * would otherwise come back here. */
	g.dragging = 0;
	KillTimer(g.hwnd, TIMER_DRAG_SCROLL);
	if (GetCapture() == g.hwnd)
		ReleaseCapture();
	if (cancel) {
		/* Nothing can add or remove tracks during a drag (every command
		 * that would ends it first), so the saved entries are exactly the
		 * current ones in the old order. */
		memcpy(g.pl.items, g.drag_saved, g.pl.count * sizeof(MpEntry));
		g.pl.current = g.drag_saved_current;
		apply_selection(g.drag_saved_sel, g.drag_saved_focus);
		refresh_list();
		update_track_status();
	} else {
		/* Compared by the path pointers, which identify each entry. */
		for (i = 0; i < g.pl.count && !changed; i++)
			changed = g.pl.items[i].path != g.drag_saved[i].path;
		if (changed)
			forget_sort();
	}
	free(g.drag_saved);
	free(g.drag_saved_sel);
	g.drag_saved = NULL;
	g.drag_saved_sel = NULL;
	SetCursor(LoadCursor(NULL, IDC_ARROW));
}

/* ---- Commands ----------------------------------------------------------- */

static void clear_playlist(void)
{
	finish_drag();
	forget_sort();
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
	HCURSOR old;
	finish_drag();
	old = SetCursor(LoadCursor(NULL, IDC_WAIT));
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
	/* New tracks go on the end, so the list is no longer in sorted order. */
	if (g.pl.count > first)
		forget_sort();
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
	int *rows, n = 0, i, sel = -1, total;
	int removed_current = 0;
	finish_drag();
	total = ListView_GetSelectedCount(g.list);
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
	case IDM_MOVE_UP: cmd_move_selected(-1); break;
	case IDM_MOVE_DOWN: cmd_move_selected(1); break;
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
	case IDM_SPEED_25:
	case IDM_SPEED_50:
	case IDM_SPEED_75:
	case IDM_SPEED_100:
	case IDM_SPEED_125:
	case IDM_SPEED_150:
	case IDM_SPEED_175:
	case IDM_SPEED_200:
	case IDM_SPEED_300:
		set_speed_index(id - IDM_SPEED_25);
		break;
	case IDM_ABOUT: cmd_about(); break;
	case IDCANCEL:
		/* IsDialogMessage turns Escape into IDCANCEL: during a drag it
		 * puts the tracks back where they were. */
		end_drag(1);
		break;
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

/* The same for a button with a symbol: room for the icon and the space the
 * button control leaves between it and the text. */
static int icon_button_width(const wchar_t *longest_text)
{
	return button_width(longest_text) + g.icon_size + g.unit / 2;
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

/* The speed label and drop-down, sized for their widest text. */
static int speed_label_width(void)
{
	return text_width(g.font, L"Speed:") + g.unit / 3;
}

static int speed_box_width(void)
{
	/* The text, the drop-down arrow, and the box's own margins. */
	return text_width(g.font, L"0.25x") + GetSystemMetrics(SM_CXVSCROLL) + g.unit;
}

static void layout(void)
{
	RECT rc, sr;
	int m = g.unit * 2 / 3, gap = g.unit / 2;
	int x, y, w, h, status_h, btn_h, time_w, vl_w, vb_w, seek_w;
	int wp = icon_button_width(L"Previous"), wplay = icon_button_width(L"Pause");
	int ws = icon_button_width(L"Stop"), wn = icon_button_width(L"Next");
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
	/* Speed goes after Next, with the playback controls it belongs to. */
	{
		RECT cr;
		int sx = x + wp + wplay + ws + wn + 5 * gap, box_h;
		MoveWindow(g.speed_label, sx, y + (btn_h - g.unit) / 2, speed_label_width(), g.unit + 2, TRUE);
		sx += speed_label_width();
		/* A drop-down list keeps its own height (set by its font); the
		 * height given here is how tall the opened list may be. So place
		 * it first, then center it on the buttons by its real height. */
		MoveWindow(g.speed_box, sx, y, speed_box_width(), g.unit * 16, TRUE);
		GetWindowRect(g.speed_box, &cr);
		box_h = cr.bottom - cr.top;
		MoveWindow(g.speed_box, sx, y + (btn_h - box_h) / 2, speed_box_width(), g.unit * 16, TRUE);
	}
	MoveWindow(g.btn_playlist, x + w - wl, y, wl, btn_h, TRUE);
	MoveWindow(g.btn_add, x + w - wl - gap - wa, y, wa, btn_h, TRUE);
	y += btn_h + gap;

	h = rc.bottom - status_h - m - y;
	MoveWindow(g.list, x, y, w, h > 0 ? h : 0, TRUE);
}

static int min_client_width(void)
{
	int gap = g.unit / 2, margins = g.unit * 4 / 3;
	int buttons = icon_button_width(L"Previous") + icon_button_width(L"Pause") +
		icon_button_width(L"Stop") + icon_button_width(L"Next") + button_width(L"Add Files...") +
		button_width(L"Open Playlist...") + speed_label_width() + speed_box_width() + 7 * gap + g.unit * 2;
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

/* Renders one symbol into a new 32-bit top-down DIB section, with straight
 * or premultiplied alpha. Returns NULL on failure. */
static HBITMAP make_glyph_bitmap(MpGlyph glyph, int size, COLORREF color, int premultiplied)
{
	BITMAPINFO bi;
	void *bits = NULL;
	HBITMAP bmp;
	/* COLORREF is 0x00BBGGRR; the glyph wants 0x00RRGGBB. */
	uint32_t rgb = ((uint32_t)GetRValue(color) << 16) | ((uint32_t)GetGValue(color) << 8) | GetBValue(color);

	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = size;
	bi.bmiHeader.biHeight = -size; /* negative = top row first, as rendered */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (bmp == NULL || bits == NULL)
		return NULL;
	/* The DIB's rows are exactly size * 4 bytes (32-bit rows need no
	 * padding), so the glyph renders straight into it. */
	if (!mp_glyph_render(glyph, size, rgb, (uint32_t *)bits)) {
		DeleteObject(bmp);
		return NULL;
	}
	if (premultiplied)
		mp_glyph_premultiply((uint32_t *)bits, (size_t)size * size);
	return bmp;
}

/* Builds one symbol as a 32-bit alpha icon. 32-bit icons with an alpha
 * channel work from XP on; the monochrome mask is still required by
 * CreateIconIndirect but is ignored when the color bitmap has alpha, so it
 * is left all zeros (all "opaque"). */
static HICON make_glyph_icon(MpGlyph glyph, int size, COLORREF color)
{
	uint8_t *mask_bits;
	HBITMAP color_bmp, mask_bmp = NULL;
	ICONINFO ii;
	HICON icon = NULL;

	color_bmp = make_glyph_bitmap(glyph, size, color, 0);
	/* Monochrome bitmap rows are padded to 16 bits. */
	mask_bits = (uint8_t *)calloc((size_t)((size + 15) / 16) * 2 * size, 1);
	if (mask_bits != NULL)
		mask_bmp = CreateBitmap(size, size, 1, 1, mask_bits);
	if (color_bmp != NULL && mask_bmp != NULL) {
		memset(&ii, 0, sizeof(ii));
		ii.fIcon = TRUE;
		ii.hbmColor = color_bmp;
		ii.hbmMask = mask_bmp;
		/* The icon gets its own copies, so the bitmaps are freed below. */
		icon = CreateIconIndirect(&ii);
	}
	if (color_bmp != NULL)
		DeleteObject(color_bmp);
	if (mask_bmp != NULL)
		DeleteObject(mask_bmp);
	free(mask_bits);
	return icon;
}

/* Puts a bitmap beside a menu item (NULL removes it). */
static void set_menu_bitmap(UINT id, HBITMAP bmp)
{
	MENUITEMINFOW mii;
	HMENU menu = GetMenu(g.hwnd);
	if (menu == NULL)
		return;
	memset(&mii, 0, sizeof(mii));
	mii.cbSize = sizeof(mii);
	mii.fMask = MIIM_BITMAP;
	mii.hbmpItem = bmp;
	SetMenuItemInfoW(menu, id, FALSE, &mii);
}

/*
 * (Re)creates the Playback menu's symbols in the current menu text color.
 *
 * Only on Windows Vista and later. Their menus draw an item's bitmap with
 * AlphaBlend, so a premultiplied 32-bit bitmap gets smooth, transparent
 * edges on the menu's own background. Windows XP ignores the alpha channel
 * there and would draw each symbol on a black square. XP's alternative is
 * to owner-draw the bitmaps (HBMMENU_CALLBACK), but that cannot be tried
 * without an XP machine, and on Vista and later it would switch the menu
 * out of the visual style. So on XP the menu simply stays text only, as it
 * always was. GetVersion is used rather than GetVersionEx because only the
 * major version matters, and it needs no manifest entry to report it: every
 * version the manifest does not list still reports 6 or more.
 */
/*
 * Gives the menu bar the window's background color.
 *
 * On Windows 10 and 11 this changes nothing visible: the theme paints the
 * menu bar itself, in white, and ignores a menu's background brush (tried
 * and confirmed). That white is why the window is white too. On Windows XP
 * and with the classic theme the brush is used, and makes the bar match
 * the window there as well.
 *
 * The bar only: with MIM_APPLYTOSUBMENUS the drop-down menus also take
 * the brush, but on Windows 10/11 only partly - their icon column and the
 * highlight stay the theme's - which looked two-toned. GetSysColorBrush is
 * a system-owned brush that follows color changes (high contrast too) by
 * itself, so there is nothing to free or recreate.
 */
static void match_menu_bar_to_window(void)
{
	MENUINFO mi;
	HMENU menu = GetMenu(g.hwnd);
	if (menu == NULL)
		return;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	mi.fMask = MIM_BACKGROUND;
	mi.hbrBack = GetSysColorBrush(COLOR_WINDOW);
	SetMenuInfo(menu, &mi);
	/* The bar is part of the window frame and only repaints when told. */
	DrawMenuBar(g.hwnd);
}

static void create_menu_bitmaps(void)
{
	HBITMAP old[MP_GLYPH_COUNT];
	COLORREF color = GetSysColor(COLOR_MENUTEXT);
	int i, size = GetSystemMetrics(SM_CXSMICON);
	if (LOBYTE(LOWORD(GetVersion())) < 6)
		return;
	/* The small-icon size (16 px at 100%, 24 at 150%) is what menus leave
	 * room for; at 16 px each design-grid unit is exactly one pixel. */
	for (i = 0; i < MP_GLYPH_COUNT; i++) {
		old[i] = g.menu_bitmaps[i];
		g.menu_bitmaps[i] = make_glyph_bitmap((MpGlyph)i, size, color, 1);
	}
	set_menu_bitmap(IDM_PREVIOUS, g.menu_bitmaps[MP_GLYPH_PREVIOUS]);
	set_menu_bitmap(IDM_STOP, g.menu_bitmaps[MP_GLYPH_STOP]);
	set_menu_bitmap(IDM_NEXT, g.menu_bitmaps[MP_GLYPH_NEXT]);
	set_menu_bitmap(IDM_PLAY_PAUSE,
		g.menu_bitmaps[g.shown_state == MP_PLAYER_PLAYING ? MP_GLYPH_PAUSE : MP_GLYPH_PLAY]);
	/* Menus do not own their item bitmaps either. */
	for (i = 0; i < MP_GLYPH_COUNT; i++) {
		if (old[i] != NULL)
			DeleteObject(old[i]);
	}
}

static void destroy_menu_bitmaps(void)
{
	int i;
	for (i = 0; i < MP_GLYPH_COUNT; i++) {
		if (g.menu_bitmaps[i] != NULL)
			DeleteObject(g.menu_bitmaps[i]);
		g.menu_bitmaps[i] = NULL;
	}
}

/*
 * (Re)creates the playback buttons' symbols in the current button text
 * color and puts them on the buttons. Called at startup and again when the
 * system colors change (switching to or from high contrast, say), so the
 * symbols always match the captions beside them.
 */
static void create_button_icons(void)
{
	HICON old[MP_GLYPH_COUNT];
	COLORREF color = GetSysColor(COLOR_BTNTEXT);
	int i;
	/* A little under the text height, like the icons on system buttons.
	 * Never below 8 px, where the shapes would stop being readable. */
	g.icon_size = g.unit - g.unit / 8;
	if (g.icon_size < 8)
		g.icon_size = 8;
	for (i = 0; i < MP_GLYPH_COUNT; i++) {
		old[i] = g.icons[i];
		g.icons[i] = make_glyph_icon((MpGlyph)i, g.icon_size, color);
	}
	/* BM_SETIMAGE on a button that keeps its text shows both, icon on the
	 * left (Common Controls 6, which the manifest asks for). A NULL icon,
	 * if creation failed, just leaves the text. */
	SendMessageW(g.btn_prev, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g.icons[MP_GLYPH_PREVIOUS]);
	SendMessageW(g.btn_stop, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g.icons[MP_GLYPH_STOP]);
	SendMessageW(g.btn_next, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g.icons[MP_GLYPH_NEXT]);
	SendMessageW(g.btn_play, BM_SETIMAGE, IMAGE_ICON,
		(LPARAM)g.icons[g.shown_state == MP_PLAYER_PLAYING ? MP_GLYPH_PAUSE : MP_GLYPH_PLAY]);
	/* Buttons do not own their images, so the old ones are ours to free,
	 * but only once nothing shows them any more. */
	for (i = 0; i < MP_GLYPH_COUNT; i++) {
		if (old[i] != NULL)
			DestroyIcon(old[i]);
	}
}

static void destroy_button_icons(void)
{
	int i;
	for (i = 0; i < MP_GLYPH_COUNT; i++) {
		if (g.icons[i] != NULL)
			DestroyIcon(g.icons[i]);
		g.icons[i] = NULL;
	}
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
	/* Created here, between Next and Add Files, so Tab reaches it in the
	 * order it appears; the label first, so screen readers name the box
	 * after it ("Speed"). */
	g.speed_label = make_child(L"STATIC", L"Speed:", SS_LEFT | SS_NOPREFIX, 0, IDC_SPEED_LABEL);
	g.speed_box = make_child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, IDC_SPEED);
	{
		int i;
		for (i = 0; i < SPEED_COUNT; i++)
			SendMessageW(g.speed_box, CB_ADDSTRING, 0, (LPARAM)speed_names[i]);
	}
	g.btn_add = make_child(L"BUTTON", L"Add Files...", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_ADD);
	g.btn_playlist = make_child(L"BUTTON", L"Open Playlist...", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BTN_PLAYLIST);
	create_button_icons();
	/* The class's menu is already attached by the time WM_CREATE arrives. */
	create_menu_bitmaps();
	match_menu_bar_to_window();

	g.list = make_child(WC_LISTVIEWW, L"",
		LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_LIST);
	ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
	/* "#" is the place in the playlist; "Track" is the track number from
	 * the file's metadata (the number on the album). */
	add_column(COL_NUMBER, L"#", g.unit * 3, LVCFMT_RIGHT);
	add_column(COL_TRACK, L"Track", g.unit * 3 + g.unit / 2, LVCFMT_RIGHT);
	add_column(COL_TITLE, L"Title", g.unit * 14, LVCFMT_LEFT);
	add_column(COL_ARTIST, L"Artist", g.unit * 10, LVCFMT_LEFT);
	add_column(COL_FILE, L"File", g.unit * 14, LVCFMT_LEFT);
	g.sort_column = -1;

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
		 * buffer is fine for the computed columns. */
		static wchar_t number[16];
		size_t i = (size_t)di->item.iItem;
		if ((di->item.mask & LVIF_TEXT) && i < g.pl.count) {
			const MpEntry *e = &g.pl.items[i];
			switch (di->item.iSubItem) {
			case COL_NUMBER:
				wsprintfW(number, L"%d", (int)i + 1);
				di->item.pszText = number;
				break;
			case COL_TRACK:
				/* No track number in the file: left blank, not "0". */
				if (e->track != 0)
					wsprintfW(number, L"%u", e->track);
				else
					number[0] = 0;
				di->item.pszText = number;
				break;
			case COL_TITLE: di->item.pszText = e->title; break;
			case COL_ARTIST: di->item.pszText = e->artist; break;
			default: di->item.pszText = (LPWSTR)mp_path_basename(e->path); break;
			}
		}
		return 0;
	}
	case LVN_COLUMNCLICK:
		sort_by_column(((NMLISTVIEW *)nm)->iSubItem);
		return 0;
	case LVN_BEGINDRAG:
		begin_drag(((NMLISTVIEW *)nm)->iItem);
		return 0;
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
		/* The drop-down's notifications carry its ID too; they are told
		 * apart from menu commands by the notification code. */
		if ((HWND)lp == g.speed_box && g.speed_box != NULL) {
			on_speed_box(HIWORD(wp));
			return 0;
		}
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
		else if (wp == TIMER_DRAG_SCROLL && g.dragging)
			drag_autoscroll();
		return 0;
	case WM_MOUSEMOVE:
		if (g.dragging) {
			on_drag_mouse_move(lp);
			return 0;
		}
		break;
	case WM_LBUTTONUP:
		if (g.dragging) {
			end_drag(0);
			return 0;
		}
		break;
	case WM_CAPTURECHANGED:
		/* Something else took the mouse (Alt+Tab, a message box): keep
		 * the order as it is now, like letting go. */
		if (g.dragging && (HWND)lp != hwnd)
			end_drag(0);
		break;
	case WM_INITMENUPOPUP:
		update_edit_menu();
		break;
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
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN: {
		/*
		 * Static text and trackbars (the seek and volume sliders) ask their
		 * parent for a background brush before painting, and by default
		 * get the dialog gray (COLOR_BTNFACE): on the white window each
		 * would sit in a gray box. Answering with the window color makes
		 * them blend in. Buttons ask too (WM_CTLCOLORBTN); with visual
		 * styles they paint their corners from the parent's background
		 * anyway, but in the classic theme this brush fills them.
		 */
		HDC dc = (HDC)wp;
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		SetBkColor(dc, GetSysColor(COLOR_WINDOW));
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
	}
	case WM_SYSCOLORCHANGE:
		create_button_icons();
		create_menu_bitmaps();
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
	/* The standard window color (white, unless a high-contrast theme says
	 * otherwise), so the window matches its menu bar: on Windows 10 and 11
	 * the theme paints the menu bar white and cannot be told otherwise (see
	 * match_menu_bar_to_window). The labels and sliders are told to match
	 * in WM_CTLCOLORSTATIC. */
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
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
	set_speed_index(SPEED_NORMAL);
	update_now_playing();

	/* A comfortable starting size in font units, so it looks the same at
	 * every DPI. */
	r.left = 0;
	r.top = 0;
	r.right = g.unit * 54;
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
	destroy_button_icons();
	/* After the window (and with it the menu) is gone, so no menu still
	 * refers to the bitmaps. */
	destroy_menu_bitmaps();
	return (int)msg.wParam;
}
