# Music Player - a small Win32 music player for Windows XP through Windows 11.
#
# See docs/BUILDING.md for step-by-step build instructions and
# docs/TESTING.md for running the tests.

cmake_minimum_required(VERSION 3.16)

# THE VERSION NUMBER. This line is the single source of truth: version.h and
# the exe's version resource are generated from it, and the GitHub Release
# workflow cuts a release (tag + GitHub release) whenever a push to main
# changes it. Bump it here and nowhere else.
project(MusicPlayer VERSION 0.3.0 LANGUAGES C RC)

if(NOT WIN32 OR NOT MINGW)
	message(FATAL_ERROR
		"Music Player is built with MinGW-w64 GCC (MSYS2's mingw32 or mingw64 "
		"environment). See docs/BUILDING.md.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(MP_BUILD_TESTS "Build the unit tests" ON)

# --- Windows XP compatibility ------------------------------------------------
#
# To run on XP, the exe must link against the old msvcrt.dll C runtime (which
# every Windows version ships), not the Universal CRT (Windows 10+, or an
# update that XP cannot install). MSYS2's mingw32/mingw64 toolchains use
# msvcrt.dll; its ucrt64/clang64 toolchains use the UCRT. Detect which one we
# have instead of guessing from the compiler name.
include(CheckCSourceCompiles)
check_c_source_compiles("
#include <stdio.h>
#ifdef _UCRT
#error built against the Universal CRT
#endif
int main(void) { return 0; }
" MP_CRT_IS_MSVCRT)

option(MP_XP_COMPAT
	"Mark the exe as runnable on Windows XP and check its imports (needs an msvcrt.dll toolchain)"
	${MP_CRT_IS_MSVCRT})
if(MP_XP_COMPAT AND NOT MP_CRT_IS_MSVCRT)
	message(FATAL_ERROR
		"MP_XP_COMPAT needs a toolchain that links against msvcrt.dll, but this one uses the "
		"Universal CRT. Use MSYS2's mingw32 (32-bit) or mingw64 (64-bit) environment, or "
		"configure with -DMP_XP_COMPAT=OFF for a Windows 10/11-only build.")
endif()

configure_file(src/version.h.in "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

# --- Core library: everything except the window ------------------------------
#
# Split out so the tests link exactly the code the app runs.
add_library(mpcore STATIC
	src/text.c
	src/stream.c
	src/tags.c
	src/track.c
	src/playlist.c
	src/decoder.c
	src/player.c
	src/volume.c
	src/glyph.c
	src/stretch.c
	src/image.c
	src/art.c
	src/dr_libs_impl.c
	src/stb_image_impl.c
)
target_include_directories(mpcore PUBLIC
	src
	third_party/dr_libs
	third_party/stb
	"${CMAKE_BINARY_DIR}/generated"
)
# _WIN32_WINNT/WINVER 0x0501 = Windows XP. With these, the Windows headers
# only declare APIs that exist on XP, so accidentally calling a Vista-only
# function is a compile error rather than a crash on an XP machine. This is
# set for every build, not only MP_XP_COMPAT ones, so the code stays
# XP-clean whatever toolchain a developer happens to use.
target_compile_definitions(mpcore PUBLIC
	UNICODE
	_UNICODE
	_WIN32_WINNT=0x0501
	WINVER=0x0501
	_WIN32_IE=0x0600
)
# Warnings for our C code only (windres does not understand -Wall).
target_compile_options(mpcore PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)
# The vendored decoders are not ours to fix; silence their warnings.
set_source_files_properties(src/dr_libs_impl.c src/stb_image_impl.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(mpcore PUBLIC winmm)

# Link flags shared by every executable.
function(mp_executable_settings target)
	# -static: fold libgcc (and anything else GCC would need) into the exe,
	# so it runs without libgcc_s_dw2-1.dll or libwinpthread-1.dll beside it.
	target_link_options(${target} PRIVATE -static)
	target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)
	if(MP_XP_COMPAT)
		# Recent GNU ld stamps executables as needing Windows Vista (6.0) or
		# later, and XP refuses to start them ("not a valid Win32
		# application"). XP is 5.1; 64-bit XP is 5.2.
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
			set(minor 2)
		else()
			set(minor 1)
		endif()
		target_link_options(${target} PRIVATE
			-Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,${minor}
			-Wl,--major-os-version,5 -Wl,--minor-os-version,${minor})
	endif()
endfunction()

# --- The app ------------------------------------------------------------------
add_executable(MusicPlayer WIN32 src/main.c src/app.rc)
# Music_Icons is on the include path so app.rc can find music_note.ico.
target_include_directories(MusicPlayer PRIVATE Music_Icons)
target_link_libraries(MusicPlayer PRIVATE mpcore comctl32 comdlg32 shell32 gdi32 user32)
# -municode: the entry point is wWinMain (Unicode command line).
target_link_options(MusicPlayer PRIVATE -municode)
mp_executable_settings(MusicPlayer)
# Rebuild the resources when the icon or manifest change.
set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS
	"${CMAKE_SOURCE_DIR}/Music_Icons/music_note.ico;${CMAKE_SOURCE_DIR}/src/app.manifest")

# --- Tests --------------------------------------------------------------------
if(MP_BUILD_TESTS)
	enable_testing()

	add_executable(mp_tests
		tests/test_main.c
		tests/builders.c
		tests/test_text.c
		tests/test_tags.c
		tests/test_track.c
		tests/test_playlist.c
		tests/test_decoder.c
		tests/test_player.c
		tests/test_volume.c
		tests/test_glyph.c
		tests/test_stretch.c
		tests/test_image.c
	)
	target_include_directories(mp_tests PRIVATE tests)
	target_link_libraries(mp_tests PRIVATE mpcore user32)
	mp_executable_settings(mp_tests)

	# One CTest test per suite, so a failure names the area that broke.
	foreach(suite text tags track playlist decoder player volume glyph stretch image)
		add_test(NAME ${suite} COMMAND mp_tests ${suite})
	endforeach()
	# Exit code 77 = skipped. The player suite needs a real audio output
	# device, which CI machines do not have.
	set_tests_properties(player PROPERTIES SKIP_RETURN_CODE 77)

	if(MP_XP_COMPAT)
		# Inspects the built executables' import tables for functions that do
		# not exist on Windows XP. See tests/xpcheck.c.
		add_executable(xpcheck tests/xpcheck.c)
		mp_executable_settings(xpcheck)
		add_test(NAME xpcheck_self_test COMMAND xpcheck --self-test)
		add_test(NAME xp_compat_app COMMAND xpcheck $<TARGET_FILE:MusicPlayer>)
		add_test(NAME xp_compat_tests COMMAND xpcheck $<TARGET_FILE:mp_tests>)
	endif()

	# The icon generator's tests, when Python is around. The script exits
	# with 77 (skipped) if Pillow is not installed.
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_Interpreter_FOUND)
		add_test(NAME icons COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/test_make_icons.py")
		set_tests_properties(icons PROPERTIES SKIP_RETURN_CODE 77)
	endif()
endif()
