# Testing Music Player

- [Running the tests](#running-the-tests)
- [What is tested](#what-is-tested)
- [Running one suite, and reading failures](#running-one-suite-and-reading-failures)
- [The playback tests](#the-playback-tests)
- [The Windows XP check](#the-windows-xp-check)
- [The icon tests](#the-icon-tests)
- [Continuous integration](#continuous-integration)
- [Writing new tests](#writing-new-tests)
- [Testing by hand](#testing-by-hand)
- [Testing on Windows XP](#testing-on-windows-xp)

## Running the tests

After building (see [BUILDING.md](BUILDING.md)), from the repository folder:

```sh
ctest --test-dir build-x86 --output-on-failure
```

A passing run ends like this (about 3 seconds):

```
100% tests passed, 0 tests failed out of 10
```

`--output-on-failure` prints a failing test's output (what it expected and
what it got) instead of just its name. The tests are part of the default
build (`MP_BUILD_TESTS=ON`), so `cmake --build` builds them too.

## What is tested

The unit tests are one program, `mp_tests.exe`, holding six suites. CTest
runs each suite as its own test, so a failure tells you which area broke.
Test inputs (MP3, FLAC and WAV files, ID3 tags, playlists) are built byte by
byte in memory by `tests/builders.c`, so every input is visible in the test
that uses it. There are no binary fixture files.

| CTest name | Source | What it covers |
| --- | --- | --- |
| `text` | `tests/test_text.c` | UTF-8 decoding and validation (overlong forms, encoded surrogates, truncated sequences, values above U+10FFFF), Windows-1252 including its 0x80 to 0x9F range, UTF-16 in both byte orders with surrogate pairs and unpaired surrogates, UTF-8 encoding, truncation that never splits a character, trimming, ASCII-only case-insensitive comparison. |
| `tags` | `tests/test_tags.c` | Title/artist extraction. ID3v2.2/2.3/2.4 in all four text encodings, with or without BOMs. Extended headers. Tag-level and frame-level unsynchronization. The data length indicator and grouping bytes. Compressed and encrypted frames skipped. iTunes' non-syncsafe v2.4 sizes. Footers and stacked tags. Cover art skipped. ID3v1 and v1-vs-v2 precedence. Album artist fallback. FLAC Vorbis comments (case-insensitive keys, odd fields, corrupt counts, ID3 in front, padding blocks). WAV `LIST/INFO` (UTF-8 vs 1252, odd-length padding) and `id3 ` chunks. Truncated, oversized and garbage structures, which must never crash or read out of bounds. |
| `track` | `tests/test_track.c` | The display rules (title falls back to the file name, a missing artist stays empty, whitespace counts as missing), file-name extraction, and time formatting from `0:00` up to `1193:02:47`. |
| `playlist` | `tests/test_playlist.c` | M3U parsing (directives, blank lines, LF/CRLF/CR, padding). Relative, rooted, absolute, UNC and forward-slash paths. `file://` URLs with %-escapes, UNC hosts and the legacy `C\|` form. Stream URLs skipped. UTF-8/UTF-16/ANSI detection. PLS numbering and ordering. Previous/Next stepping and wrap-around. Removing tracks while one is playing. `.m3u8` saving with relative paths and a round trip back through the parser. |
| `decoder` | `tests/test_decoder.c` | Format detection by content (and by extension only as a fallback). Exact sample-for-sample decoding of WAV (8/16/24-bit, float, 6-channel downmix) and FLAC (stereo and mono, multiple frames, seeking), with MP3 checked as known silence (length, seeking, tags at both ends). The workaround for dr_flac's ID3-in-front bug. Error messages for missing, empty, corrupt and unsupported files. |
| `player` | `tests/test_player.c` | Real playback through waveOut: load, play, pause (position holds), seek while paused, play to the end (exactly one end-of-track notice), replay after the end, stop, replace, unload, and shutting down while playing. [Needs a sound device.](#the-playback-tests) |
| `xpcheck_self_test`, `xp_compat_app`, `xp_compat_tests` | `tests/xpcheck.c` | The Windows XP check, on both `MusicPlayer.exe` and `mp_tests.exe`. Only in XP-compatible (msvcrt) builds. [Details below.](#the-windows-xp-check) |
| `icons` | `tools/test_make_icons.py` | The icon generator, and that `Music_Icons/` matches it. Only if CMake finds Python. [Details below.](#the-icon-tests) |

What is **not** covered automatically: the window itself (layout, menus,
dialogs, drag and drop). That's what [Testing by hand](#testing-by-hand) is
for.

## Running one suite, and reading failures

Through CTest, by name (a regular expression):

```sh
ctest --test-dir build-x86 -R tags --output-on-failure
ctest --test-dir build-x86 -R "xp" --output-on-failure    # all three XP checks
```

Or run the test program directly, which is faster and easier to debug:

```sh
build-x86/mp_tests.exe            # every suite
build-x86/mp_tests.exe decoder    # one suite
build-x86/mp_tests.exe --list     # suite names
```

A failure names the file, line and expectation, and shows both values.
Non-ASCII characters are printed as `\uXXXX` so they survive any console:

```
tests/test_tags.c:52: FAILED: t.title == L"Caf\x00E9 Song"
    got:  "Café Son"
    want: "Café Song"
```

A suite keeps going after a failed check, so one run reports every broken
expectation. `mp_tests.exe` exits with **0** when everything passed, **1**
if anything failed, **2** for an unknown suite name, and **77** when the
suites it ran were all skipped. CTest treats 77 as "skipped", not "failed".

## The playback tests

The `player` suite plays real audio through the default output device. It
plays **digital silence**, so nothing is heard, and takes about 2 seconds.

A machine with no usable sound device (most CI servers, or a PC with the
Windows Audio service stopped) can't run it. The suite then reports
**skipped** rather than failed:

```
6/10 Test  #6: player ...........................***Skipped   0.01 sec
```

If it skips on your own PC, check that a playback device is enabled in
Windows' Sound settings.

Timing checks have generous margins (a busy machine can be late delivering
messages), but a heavily loaded machine could still fail one. If the
`player` test fails intermittently, re-run it on its own before assuming a
bug.

## The Windows XP check

Most people don't have an XP machine, so the build checks the two things
that silently break XP support:

1. **The version stamp in the exe's header.** XP (5.1; 5.2 for 64-bit XP)
   refuses to start anything stamped 6.0 or later, which is what current
   linkers produce by default.
2. **Imported functions.** If the exe imports a function XP's DLLs don't
   export, XP won't start it ("The procedure entry point ... could not be
   located"). `xpcheck` reads the exe's import table and fails if it finds
   a DLL outside the expected set (for example the Universal CRT's
   `api-ms-win-crt-*.dll`) or any of about 80 functions known to be newer
   than XP, including the whole `msvcrt.dll` "secure CRT" (`*_s`) family.

To see everything the exe imports:

```sh
build-x86/xpcheck.exe -v build-x86/MusicPlayer.exe
```

This check is a safety net, not a guarantee. The list of post-XP functions
can't be complete. Running on real XP is the only full proof: see
[Testing on Windows XP](#testing-on-windows-xp).

These tests exist only in builds with `MP_XP_COMPAT=ON`, which is the
default for the MINGW32/MINGW64 toolchains.

## The icon tests

`tools/test_make_icons.py` checks that the icon generator:

- writes every file;
- produces PNGs of the right size with transparent corners;
- writes an `.ico` whose images are all uncompressed bitmaps, which is what
  Windows XP needs, with pixels matching the PNGs and a correct
  transparency mask;
- writes a well-formed SVG.

It also checks that the files committed in `Music_Icons/` match what the
script produces, so a hand-edited icon can't drift from the others.

It needs Python 3 and Pillow. CTest registers it when it finds Python, and
it reports **skipped** if Pillow is missing. To run it directly:

```sh
python tools/test_make_icons.py
```

## Continuous integration

`.github/workflows/tests.yml` builds and tests both the x86 (MINGW32) and x64
(MINGW64) versions on GitHub's Windows machines:

- on every push to `main`;
- on every pull request targeting `main`, when it is opened and on every
  update;
- by hand: **Actions > Tests > Run workflow**.

On pull requests it posts a comment saying whether the tests passed, and
later runs **edit that same comment** instead of adding new ones.
GitHub's machines have no sound device, so `player` shows as skipped there.
Everything else runs for real, including the XP check.

Pull requests from **forks** get a read-only token from GitHub, so the comment
can't be posted there. The test results still show in the pull request's
checks.

The Release workflow (`.github/workflows/release.yml`) runs the same build
and tests. A failing test stops the release: no tag and no GitHub release
are created.

## Writing new tests

- Put the test in the matching `tests/test_*.c` file, as a `static void`
  function called from that file's `suite_*()` function at the bottom.
- Check things with `CHECK(condition)`, `CHECK_INT(got, want)` and
  `CHECK_WSTR(got, want)` from `tests/mptest.h`.
- Build inputs with the helpers in `tests/builders.h`: `id3_frame`/`id3_tag`,
  `id3v1_tag`, `wav_file`/`riff_chunk`, `flac_file`/`vorbis_comment` (real,
  decodable FLAC), `mp3_silence` (real, decodable MP3), and `temp_file` to put
  one on disk.
- To add a whole new suite: create `tests/test_<name>.c` with an
  `int suite_<name>(void)`, declare it in `mptest.h`, add it to the table in
  `test_main.c`, and add it to the `mp_tests` source list and the `foreach`
  list in `CMakeLists.txt`.
- Cover the awkward cases: empty input, truncated input, sizes that lie,
  boundaries, non-ASCII text.

## Testing by hand

The window isn't covered by automated tests. Before a release, a
five-minute run through this list is worthwhile.

Test files: any MP3/FLAC/WAV files. Include at least one with full tags, one
with a title but no artist, one with no tags at all, and one with non-English
characters in its tags.

1. Start `MusicPlayer.exe`. The window opens with "No tracks loaded", the
   note icon shows in the title bar and taskbar, and nothing is playing.
2. **File > Open Files...**, select several files. The first plays. Title and
   artist show at the top, the time counts up, and the button says **Pause**.
3. A file without tags shows its **file name** as the title and **no artist**.
4. **Pause**: the sound stops and the time holds. **Play**: it resumes from
   the same place, not the start.
5. Drag the seek bar: playback jumps there. Try it while paused too.
6. **Next** / **Previous**, including wrapping at both ends of the list.
7. Let a track play to its end: the next one starts by itself. At the end of
   the list it stops with "End of playlist". Turn on **Playback > Repeat
   Playlist** and check that it starts over instead.
8. **Stop**: the time goes back to 0:00.
9. **File > Save Playlist As...**, then **File > Clear Playlist**, then
   **File > Open Playlist...** with the saved file: the same tracks return.
10. Drag files and a playlist from Explorer onto the window.
11. Select two tracks and press **Delete**. Delete the playing track: playback
    stops cleanly.
12. Rename a file that's in the playlist, then play it. The status bar
    explains it can't be found. With Next, it's skipped.
13. Resize the window small and large. Controls stay tidy, and long titles
    end in "...".
14. Tab through the controls. Ctrl+P, Ctrl+S, Ctrl+B, Ctrl+F and the media keys
    work while the window is active.
15. Play music with World of Warcraft (or any game) running in windowed
    mode. Alt+Tab between them, and set their volumes separately in the
    Volume Mixer.
16. At 150% display scaling (Settings > Display > Scale), the window is
    sharp and proportioned, not blurry or cramped.

## Testing on Windows XP

The final check for XP support is running on XP. A virtual machine is the
easiest way:

1. Create a VM in VirtualBox, VMware or Hyper-V from a Windows XP SP3
   installation disc (you need your own licensed copy). Give it a sound card:
   in VirtualBox, Audio > Enable Audio, controller "ICH AC97", which XP has
   drivers for.
2. Copy over `build-x86\MusicPlayer.exe` and `build-x86\mp_tests.exe` (a
   shared folder or a small ISO works) together with some music files.
3. On XP, run `mp_tests.exe` from a Command Prompt. It's a console program
   with no dependencies, and every suite should report `ok`, including
   `player` if the VM has sound.
4. Run `MusicPlayer.exe` and go through [Testing by hand](#testing-by-hand).
   With the default XP theme the controls should look like Luna, not
   Windows 98.

If XP reports a missing entry point, add that function's name to the list
in `tests/xpcheck.c` (so it can never come back unnoticed), then find and
replace the call.
