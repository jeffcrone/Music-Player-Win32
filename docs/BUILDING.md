# Building Music Player

This guide goes from a fresh Windows machine to a built `MusicPlayer.exe`.
It takes about 10 minutes, most of it downloading the compiler.

- [Why this toolchain](#why-this-toolchain)
- [1. Install MSYS2](#1-install-msys2)
- [2. Install the compiler and build tools](#2-install-the-compiler-and-build-tools)
- [3. Get the source](#3-get-the-source)
- [4. Build the 32-bit (Windows XP and later) version](#4-build-the-32-bit-windows-xp-and-later-version)
- [5. Build the 64-bit version](#5-build-the-64-bit-version)
- [Building from PowerShell or Command Prompt](#building-from-powershell-or-command-prompt)
- [Build options](#build-options)
- [Debug builds](#debug-builds)
- [Other toolchains](#other-toolchains)
- [Changing the version number](#changing-the-version-number)
- [Regenerating the icons](#regenerating-the-icons)
- [Updating the dr_libs decoders](#updating-the-dr_libs-decoders)
- [Troubleshooting](#troubleshooting)

## Why this toolchain

Music Player is plain C99 using only the Win32 API, built with **MinGW-w64
GCC from MSYS2**, with **CMake** and **Ninja**.

The reason is Windows XP. To start on XP, an exe must:

1. link against the old `msvcrt.dll` C runtime, which every Windows version
   since 2000 has. The newer "Universal CRT" only exists on Windows 10+ (or
   Vista/7/8 with an update), and Visual Studio can only target XP
   through its deprecated VS 2017 "v141_xp" toolset;
2. be stamped with subsystem version 5.1 (XP) instead of the 6.0 (Vista) that
   modern linkers default to;
3. import no function that XP's DLLs lack.

MSYS2's **mingw32** (32-bit) and **mingw64** (64-bit) environments link
against `msvcrt.dll`. The build handles points 2 and 3 itself: it sets the
subsystem version, and a test checks the finished exe's imports (see
[TESTING.md](TESTING.md#the-windows-xp-check)).

## 1. Install MSYS2

1. Download the installer from <https://www.msys2.org> and run it. Accept
   the default location, `C:\msys64`. The rest of this guide assumes it.
2. When it finishes, an "MSYS2 UCRT64" terminal opens. Bring the base
   system up to date:

   ```sh
   pacman -Syu
   ```

   If it says the terminal must close, let it, reopen **MSYS2 MSYS** from
   the Start menu, and run `pacman -Syu` again until nothing is left to
   update.

## 2. Install the compiler and build tools

In any MSYS2 terminal:

```sh
# 32-bit toolchain: builds the exe that runs on Windows XP through 11.
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja

# 64-bit toolchain (optional): builds the x64 exe.
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

# Optional, for the icon tests and tools/make_icons.py:
pacman -S --needed mingw-w64-x86_64-python mingw-w64-x86_64-python-pillow
```

(A Windows Python with `pip install Pillow` works for the icon scripts too.
Nothing else needs Python.)

## 3. Get the source

With Git for Windows (<https://git-scm.com>), or `pacman -S git` in MSYS2:

```sh
git clone https://github.com/jeffcrone/WoW-Music-Player-Addon.git
cd WoW-Music-Player-Addon
```

Everything the build needs is in the repository. The decoders are vendored
in `third_party/dr_libs`, so nothing is downloaded during the build.

## 4. Build the 32-bit (Windows XP and later) version

Open **MSYS2 MINGW32** from the Start menu. The environment matters: in the
MINGW32 terminal, `gcc` is the 32-bit msvcrt compiler. Then:

```sh
cd /c/path/to/WoW-Music-Player-Addon     # C:\ is /c/ in MSYS2
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86
```

The result is **`build-x86/MusicPlayer.exe`**, a single self-contained exe of
about 800 KB. The C runtime pieces GCC needs are linked in statically, so
there are no DLLs to ship with it. Copy it anywhere and run it.

During configuration, look for this line:

```
-- Performing Test MP_CRT_IS_MSVCRT - Success
```

That means the toolchain links against `msvcrt.dll` and the XP settings are
on. Then run the tests. They include the XP import check:

```sh
ctest --test-dir build-x86 --output-on-failure
```

## 5. Build the 64-bit version

Open **MSYS2 MINGW64** and do the same with another build folder:

```sh
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-x64
ctest --test-dir build-x64 --output-on-failure
```

The 64-bit exe runs on 64-bit Windows only (XP x64 and later). The 32-bit
exe runs on 32- and 64-bit Windows alike, so it's the one to give people
unless they want 64-bit specifically.

## Building from PowerShell or Command Prompt

You don't need the MSYS2 terminal. Put the toolchain's `bin` folder first
on `PATH` for the session.

PowerShell:

```powershell
$env:PATH = "C:\msys64\mingw32\bin;$env:PATH"
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86
ctest --test-dir build-x86 --output-on-failure
```

Command Prompt:

```bat
set PATH=C:\msys64\mingw32\bin;%PATH%
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86
```

Use `C:\msys64\mingw64\bin` for 64-bit. Keep one build folder per
toolchain: CMake remembers the compiler a folder was configured with.

## Build options

Pass these to the first `cmake` command as `-DNAME=VALUE`:

| Option | Default | Meaning |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | *(none)* | `Release` (optimized, what you normally want), `Debug` (no optimization, full debug info), `RelWithDebInfo` (both). |
| `MP_BUILD_TESTS` | `ON` | Build `mp_tests.exe` and `xpcheck.exe` and register them with CTest. `OFF` builds just the app. |
| `MP_XP_COMPAT` | `ON` if the toolchain uses `msvcrt.dll` | Stamps the exe for Windows XP and adds the XP import checks to the tests. Turn it on with a UCRT toolchain and configuration stops with an explanation, because that exe could never run on XP. |

To change an option, re-run the configure command with the new value, or
delete the build folder and start again.

## Debug builds

```sh
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

To debug with GDB (`pacman -S mingw-w64-i686-gdb`):

```sh
gdb build-debug/MusicPlayer.exe
(gdb) run
```

`MusicPlayer.exe` is a GUI program, so it has no console. `mp_tests.exe` is
a console program and prints to the terminal, which usually makes it the
easier thing to debug.

## Other toolchains

- **MSYS2 UCRT64 / CLANG64:** the build works and the tests pass, but the
  exe needs the Universal CRT (Windows 10+, or an older Windows with the
  UCRT update). `MP_XP_COMPAT` turns itself off. Fine for development, not
  for release builds.
- **Visual Studio (MSVC):** not supported. `CMakeLists.txt` stops with a
  message. The code is plain C, but the build relies on GCC options
  (`-municode`, `-static`, the subsystem version flags). For XP, Visual
  Studio would also need its deprecated VS 2017 `v141_xp` toolset.
- **Cross-compiling from Linux/macOS:** the `i686-w64-mingw32` packages
  should work with a CMake toolchain file, but that isn't tested. The
  playback tests would also need Wine.

## Changing the version number

Edit the one line at the top of `CMakeLists.txt`:

```cmake
project(MusicPlayer VERSION 0.2.0 LANGUAGES C RC)
```

Everything else follows from it: the exe's version resource (Explorer's
Properties > Details tab), the About box, and the GitHub release. Pushing
a version change to `main` makes the Release workflow build, test, tag
`vX.Y.Z` and publish. See the main [README](../README.md#releasing).

## Regenerating the icons

The icon is drawn by `tools/make_icons.py` from one definition of the shape
at the top of that file. To change it, edit the numbers or colors there, then:

```sh
python tools/make_icons.py          # rewrites everything in Music_Icons/
```

and rebuild. The exe embeds `Music_Icons/music_note.ico`, and the build
tracks it, so the resources recompile. The `icons` test fails if the
committed files no longer match the script, which keeps the set consistent.

Don't swap in an `.ico` saved by another tool without checking it. Most
tools save the 256 px image PNG-compressed, and Windows XP can't read that.
The script writes every size as an uncompressed bitmap.

## Updating the dr_libs decoders

`third_party/dr_libs/` holds `dr_mp3.h`, `dr_flac.h`, `dr_wav.h` and their
`LICENSE`, from commit `dfe8377631000664666519fdb83da193fd8037f4` of
<https://github.com/mackron/dr_libs>. To update, replace those four files
with a newer commit's copies, then build **both** architectures and run the
tests. Pay attention to:

- `xp_compat_app`: new library code can pull in a function XP lacks.
- The `decoder` suite, especially "ID3v2 in front of a FLAC file". That
  case works around an upstream dr_flac bug (explained at `flac_on_seek` in
  `src/decoder.c`). If upstream fixes it, the workaround is harmless and can
  stay.

The libraries' own file functions are compiled out (`DR_*_NO_STDIO`, in
`src/dr_libs_config.h`) because they call C runtime functions XP's
`msvcrt.dll` doesn't have. Keep it that way.

## Troubleshooting

**`cmake: command not found` / `gcc: command not found`.** You are in the
wrong MSYS2 terminal (plain "MSYS2 MSYS" has neither), or the packages from
step 2 aren't installed. Use **MSYS2 MINGW32** or **MINGW64**.

**"Music Player is built with MinGW-w64 GCC..."** CMake found a different
compiler, usually Visual Studio's. Build from a MINGW32/MINGW64 terminal, or
put `C:\msys64\mingw32\bin` first on `PATH` and delete the build folder.

**"MP_XP_COMPAT needs a toolchain that links against msvcrt.dll".** You set
`-DMP_XP_COMPAT=ON` with a UCRT64 or CLANG64 compiler. Use MINGW32/MINGW64,
or leave the option at its default.

**The build folder picked up the wrong compiler.** Delete the folder
(`rm -rf build-x86`) and configure again from the right terminal. CMake
never switches compilers in an existing folder.

**`windres: can't open icon file 'music_note.ico'`.** `Music_Icons/` is
missing or incomplete. Restore it from Git, or regenerate it with
`python tools/make_icons.py`.

**The exe starts on Windows 10 but not on XP.** Run
`ctest --test-dir build-x86 -R xp` and read what it reports. The usual
causes are a UCRT toolchain, or a newly used API that XP lacks. If you get
"not a valid Win32 application", check that you copied the 32-bit (x86)
build to a 32-bit XP.

**Antivirus flags the fresh exe.** Unsigned new executables sometimes get
flagged heuristically. Rebuilding usually changes nothing. Submit the file
to the antivirus vendor as a false positive if it keeps happening.
