# Running Music Player

- [Requirements](#requirements)
- [Installing (there's nothing to install)](#installing-theres-nothing-to-install)
- [Playing music](#playing-music)
- [Playlists](#playlists)
- [What the player shows](#what-the-player-shows)
- [Controls and keyboard shortcuts](#controls-and-keyboard-shortcuts)
- [Using it alongside World of Warcraft](#using-it-alongside-world-of-warcraft)
- [Supported formats in detail](#supported-formats-in-detail)
- [Troubleshooting](#troubleshooting)

## Requirements

| | x86 build | x64 build |
| --- | --- | --- |
| Windows | XP SP3, Vista, 7, 8, 8.1, 10, 11 (32- or 64-bit) | 64-bit Windows only (XP x64 and later) |
| Processor | Any with SSE2 (Pentium 4 / Athlon 64 or newer) | Any 64-bit x86 processor |
| Other | A sound device | A sound device |

The x86 build is the safe choice for everyone. On 64-bit Windows the two
play music identically.

About Windows XP: the build is set up for XP and a test checks that it
imports nothing XP lacks (see [TESTING.md](TESTING.md#the-windows-xp-check)).
But the tests are run on current Windows. Trying it on real XP (or an XP
virtual machine) is still the final proof. See
[TESTING.md](TESTING.md#testing-on-windows-xp) for how.

## Installing (there's nothing to install)

Unzip the release anywhere (Desktop, `C:\Tools`, a USB stick) and run
`MusicPlayer.exe`. To remove it, delete the folder.

The program writes nothing to the registry, sets up no file associations
and saves no settings files. One thing to know: the standard Windows
**Open/Save file dialog** remembers the last folder you used, and Windows
does that itself, for every program that uses that dialog. The player turns
off the part it can control: files you open are **not** added to the Start
menu's Recent Documents.

On Windows 10/11, SmartScreen may warn you the first time, because the exe
is new and not code-signed. Click **More info > Run anyway**.

## Playing music

There are four ways to get music in:

| How | What happens |
| --- | --- |
| **File > Open Files...** (Ctrl+O) | Pick one or more files (Ctrl+click, Shift+click or Ctrl+A in the dialog). **Replaces** the playlist and starts playing the first one. |
| **File > Add Files...** (Ctrl+Shift+O) or the **Add Files...** button | **Appends** to the playlist. Nothing is interrupted, but if nothing was loaded yet, the first added file starts. |
| **Drag and drop** files onto the window | Appends. Audio files and playlist files can be mixed. Starts playing if nothing was loaded. |
| **Command line**: `MusicPlayer.exe song.mp3 list.m3u8` | Same as dropping those files. Dropping files onto `MusicPlayer.exe` in Explorer works the same way. |

The Open dialog's "All Files" filter can also pick files with an unusual
extension. They're accepted if their content really is MP3, FLAC or WAV.

**Double-click** a track in the list (or select it and press **Enter**) to
play it. Select tracks and press **Delete** to remove them from the playlist.
The files on disk are not touched. **File > Clear Playlist** empties the list.

### Sorting and reordering

- **Sort:** click the **Track**, **Title**, **Artist** or **File** column
  header. Click the same header again to reverse the order. The header shows
  an arrow while the list is in that order. Text sorts the way Explorer sorts
  it (ignoring case, accented letters beside their plain ones). Tracks with
  no track number, or no artist, go at the end either way.
- Sorting keeps tracks that are equal in their current order. So to get each
  artist's tracks in album order, click **Track** first, then **Artist**.
- **Drag** tracks up or down with the mouse. Select several first
  (Ctrl+click, Shift+click) to move them together; they close up into one
  group where you drop them. Holding them above or below the list scrolls
  it. Press **Escape** before letting go to put everything back.
- **File > Move Up** / **Move Down** (**Alt+Up** / **Alt+Down**) move the
  selected tracks one place. The items are grayed out when there's nowhere
  to move.
- Sorting and moving change the playlist's order itself: **Next** and
  **Previous** follow the new order, the playing track keeps playing, and
  **Save Playlist As...** saves the order you see. Adding tracks puts them at
  the end, and any reordering after a sort takes the header's arrow away,
  since the list is no longer in that order.
- The **#** column is the place in the playlist, so it can't be sorted by.

## Playlists

**Opening:** **File > Open Playlist...** (Ctrl+L), the **Open Playlist...**
button, or drag a playlist onto the window. Opening replaces the current
playlist and starts the first track. Dropping appends.

Supported:

- **M3U / M3U8** (`.m3u`, `.m3u8`): one file per line. `#EXTM3U`,
  `#EXTINF` and other `#` lines are read past. The titles shown come from
  the music files' own metadata, not from `#EXTINF`.
- **PLS** (`.pls`): `File1=`, `File2=`, ... entries, used in number order.

Entries can be:

- absolute paths: `C:\Music\song.mp3`, `\\server\share\song.flac`;
- relative paths, resolved from the playlist's own folder:
  `song.mp3`, `Album\01.flac`, `..\Other\x.wav`;
- rooted paths, meaning the same drive as the playlist: `\Music\song.mp3`;
- forward slashes, which work too: `Album/01.flac`;
- `file:///` URLs with `%20`-style escapes.

Internet radio (`http://...`) entries are skipped: the player only plays
local files. Text encoding is detected automatically: UTF-8 (with or
without BOM), UTF-16, or for old playlists the Windows ANSI code page.

A playlist entry whose file is missing still appears in the list (showing
the file name). When its turn comes, the status bar says the file could not
be found and the player moves on to the next track.

**Saving:** **File > Save Playlist As...** (Ctrl+Shift+S) writes an `.m3u8`
file (UTF-8, readable by nearly every player). Tracks in the playlist's
folder, or in folders below it, are saved as relative paths, so you can
move or copy the music folder together with the playlist. Other tracks are
saved with their full path.

## What the player shows

At the top of the window:

- **Title:** the title from the file's metadata. If the file has no title,
  the **file name** (including its extension) is shown instead.
- **Artist:** the artist from the metadata. If there is none, the line is
  left blank. When a file has an "album artist" but no track artist, the
  album artist is shown.
- The **seek bar** and **elapsed / total time**.
- The **volume** slider, labeled with the current level ("Volume 80%"), or
  "Muted".

The playlist columns show the place in the playlist (**#**), the **track
number** from the metadata (blank if the file has none), the same title and
artist, and the file name.
The playing track is shown in **bold**. The window's title bar reads
"Title - Artist - Music Player", which is also what you see on the taskbar
and in Alt+Tab. The status bar shows "Track 3 of 12", "Paused", "Stopped",
"End of playlist", or an explanation when a file couldn't be played.

Where the metadata comes from:

| Format | Title | Artist | Track number |
| --- | --- | --- | --- |
| MP3 | ID3v2 `TIT2` (v2.2 `TT2`), else ID3v1 | ID3v2 `TPE1` (v2.2 `TP1`), else ID3v1, else album artist `TPE2` | ID3v2 `TRCK` (v2.2 `TRK`), else ID3v1.1's track byte |
| FLAC | Vorbis comment `TITLE` | `ARTIST`, else `ALBUMARTIST` / `ALBUM ARTIST` | `TRACKNUMBER` (or the older `TRACK`) |
| WAV | `id3 ` chunk (as MP3), else RIFF INFO `INAM` | `id3 ` chunk, else RIFF INFO `IART` | `id3 ` chunk, else RIFF INFO `ITRK` |

Track numbers written as "7/12" (track 7 of 12) show as 7. A value that
isn't a plain number, such as "A1" from a vinyl rip, is left blank.

A FLAC file with an ID3v2 tag in front, which some taggers write, has that
tag read first.

## Controls and keyboard shortcuts

The playback shortcuts are the same as Windows Media Player's.

| Action | Button | Menu | Keyboard |
| --- | --- | --- | --- |
| Play / Pause | **Play** / **Pause** | Playback > Play/Pause | Ctrl+P |
| Stop (and rewind) | **Stop** | Playback > Stop | Ctrl+S |
| Previous track | **Previous** | Playback > Previous | Ctrl+B |
| Next track | **Next** | Playback > Next | Ctrl+F |
| Seek | drag the seek bar, or click it | | Tab to the seek bar, then arrow keys (1%) or Page Up/Down (5%) |
| Volume up / down | drag the volume slider, or click it | Playback > Volume Up / Volume Down | F10 / F9 (5% a press), or Tab to the volume slider, then arrow keys (5%) or Page Up/Down (20%) |
| Mute / unmute | | Playback > Mute | F8 |
| Open files (replace) | | File > Open Files... | Ctrl+O |
| Add files | **Add Files...** | File > Add Files... | Ctrl+Shift+O |
| Open playlist | **Open Playlist...** | File > Open Playlist... | Ctrl+L |
| Save playlist | | File > Save Playlist As... | Ctrl+Shift+S |
| Play the selected track | double-click it | | Enter |
| Remove selected tracks | | File > Remove Selected Tracks | Delete |
| Move selected tracks up / down | drag them | File > Move Up / Move Down | Alt+Up / Alt+Down |
| Sort the playlist | click a column header (again to reverse) | | |
| Keyboard media keys | | | Play/Pause, Stop, Next, Previous (when the player window is active) |

How playback moves along:

- **Pause** keeps your place and **Play** resumes from it. **Stop** goes back
  to the start of the track.
- **Next** and **Previous** wrap around: Next on the last track goes to the
  first, and Previous on the first goes to the last.
- When a track ends, the next one starts. After the last track the player
  stops ("End of playlist"), unless **Playback > Repeat Playlist** is
  checked, in which case it starts again from the top.
- **Play** with nothing loaded starts the selected track, or the first one.
- **Volume** starts at 100% every time the player starts, which plays the
  files exactly as they are. Changing the volume while muted unmutes, at
  the new level. A change takes up to about a third of a second to be
  heard, because that much audio is already queued up ahead.
- When the player moves on by itself, or with Next/Previous, it skips files
  that can't be played (missing, damaged, not really audio) and puts the
  reason in the status bar. Double-clicking such a file just shows the
  reason and doesn't jump elsewhere.

The window can be resized and remembers nothing between runs. It scales with
Windows' text size / DPI setting.

## Using it alongside World of Warcraft

The player is a completely separate program and never touches the game. A
few tips:

- Run WoW in **windowed** or **borderless windowed** mode (System > Graphics
  > Display Mode) so you can Alt+Tab between the game and the player freely.
  Exclusive fullscreen also works, but Alt+Tab is slower.
- Turn WoW's own **Music** volume down or off (System > Sound) so the two
  don't play over each other. Leave the effects and ambience on.
- Use the player's own **volume slider** (or F9/F10, and F8 to mute) to
  balance it against the game. It only changes the player's volume, never
  the game's or Windows', on every Windows version including XP. On Vista
  and later the **Volume Mixer** (right-click the speaker icon on the
  taskbar) works too.
- The keyboard's **volume keys** still change the Windows volume, as they
  do everywhere else, not the player's.
- Keyboard media keys go to whichever window is active. While you are in
  the game they go to WoW, not to the player, so switch to the player (or
  use its shortcuts) to change tracks.

## Supported formats in detail

| Format | Supported |
| --- | --- |
| **MP3** | MPEG-1, 2 and 2.5, Layer III (plus Layers I and II), any bitrate, CBR or VBR, mono or stereo. |
| **FLAC** | All standard FLAC: 8 to 32 bits, any sample rate, 1 to 8 channels. |
| **WAV** | PCM 8/16/24/32-bit, 32/64-bit float, A-law, µ-law, Microsoft ADPCM and IMA ADPCM. Also RF64 and Wave64 (for files over 4 GB). |

Playback is 16-bit at the file's own sample rate. Files with more than two
channels (5.1 and so on) are mixed down to stereo. Other formats (OGG, AAC,
M4A, WMA, ...) are not supported.

## Troubleshooting

**"No audio output device is available."** Windows reports no playback
device. Check that speakers or headphones are connected and enabled (Sound
settings > Output), then play the track again.

**"The file could not be found. It may have been moved, renamed or deleted."**
The path in the playlist no longer points at a file. Remove the entry and
add the file again from its new location.

**"This is not an MP3, FLAC or WAV file."** The file's content isn't one of
the supported formats, whatever its extension says (a renamed `.m4a`, say).

**"The ... data in this file could not be read."** The file is damaged or
truncated, or (for WAV) uses a rare compression scheme.

**The title shows the file name.** The file has no title in its metadata.
You can add one with any tag editor, such as Mp3tag or foobar2000.

**Accented or non-Latin characters look wrong.** The tag was probably
written in a legacy code page by an old tool. Re-save the tags as UTF-8 or
UTF-16 in a modern tag editor.

**Nothing happens when I press a media key.** Media keys go to the active
window. Click the player first. See
[Using it alongside World of Warcraft](#using-it-alongside-world-of-warcraft).

**Windows XP says "... is not a valid Win32 application".** You are using
the x64 build on 32-bit XP. Use the x86 build.

**Windows XP says "The procedure entry point ... could not be located".**
The build uses a function XP doesn't have. Please report it, with the
function name from the message. See the Windows XP section of
[TESTING.md](TESTING.md#the-windows-xp-check).
