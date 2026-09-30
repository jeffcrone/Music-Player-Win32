/*
 * xpcheck.c - checks that an executable can start on Windows XP.
 *
 * Nobody has an XP machine in CI, so this catches the two usual ways a
 * build silently stops working there:
 *
 *   1. The PE header's subsystem / OS version. XP (5.1; 64-bit XP is 5.2)
 *      refuses anything stamped 6.0 or later with "not a valid Win32
 *      application". Newer GNU ld stamps 6.0 by default.
 *   2. Imports of functions that XP's DLLs do not export. The exe then dies
 *      at startup with "The procedure entry point ... could not be located".
 *      A new toolchain version, a dr_libs update or an innocent-looking
 *      API call can each introduce one.
 *
 * For (2) it checks every imported DLL against a list of the DLLs we
 * expect, and every imported function against a list of well-known
 * functions that XP lacks. That list cannot be complete - the real proof
 * is running the exe on XP - but it covers the functions that toolchains
 * and libraries in practice pull in, plus the whole "secure CRT" (*_s)
 * family, which XP's msvcrt.dll does not have.
 *
 * Usage: xpcheck <file.exe>...   or   xpcheck --self-test
 * Exit code 0 = OK, 1 = problems found (listed on stderr), 2 = bad input.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* DLLs that exist on XP and that we mean to use. Anything else (notably the
 * api-ms-win-crt-* Universal CRT DLLs) fails the check. */
static const char *const allowed_dlls[] = {
	"kernel32.dll", "user32.dll", "gdi32.dll", "comdlg32.dll", "comctl32.dll",
	"shell32.dll", "winmm.dll", "msvcrt.dll", "advapi32.dll", "ole32.dll",
};

/* Functions added after XP that toolchains commonly reference. */
static const char *const denied_functions[] = {
	/* kernel32: Vista+ */
	"GetTickCount64", "InitializeSRWLock", "AcquireSRWLockExclusive", "AcquireSRWLockShared",
	"ReleaseSRWLockExclusive", "ReleaseSRWLockShared", "TryAcquireSRWLockExclusive",
	"TryAcquireSRWLockShared", "InitializeConditionVariable", "SleepConditionVariableCS",
	"SleepConditionVariableSRW", "WakeConditionVariable", "WakeAllConditionVariable",
	"InitOnceExecuteOnce", "InitOnceBeginInitialize", "InitOnceComplete",
	"InitializeCriticalSectionEx", "CreateEventExA", "CreateEventExW", "CreateMutexExW",
	"CreateSemaphoreExW", "CreateWaitableTimerExW", "GetFileInformationByHandleEx",
	"SetFileInformationByHandle", "GetFinalPathNameByHandleW", "GetFinalPathNameByHandleA",
	"CreateSymbolicLinkW", "CreateSymbolicLinkA", "CompareStringEx", "CompareStringOrdinal",
	"LCIDToLocaleName", "LocaleNameToLCID", "GetLocaleInfoEx", "GetUserDefaultLocaleName",
	"LCMapStringEx", "FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue",
	"GetCurrentProcessorNumber", "GetSystemTimePreciseAsFileTime", "QueryFullProcessImageNameW",
	"CancelIoEx", "GetQueuedCompletionStatusEx", "SetThreadStackGuarantee",
	"GetErrorMode", "GetThreadId", "Wow64GetThreadContext",
	"GetEnabledXStateFeatures", "AddDllDirectory", "SetDefaultDllDirectories",
	"RaiseFailFastException", "GetNamedPipeClientProcessId", "IsWow64Process2",
	"CreateFile2", "GetOverlappedResultEx", "WaitOnAddress", "WakeByAddressSingle",
	/* advapi32 / shell32 / user32: Vista+ */
	"RegGetValueW", "RegGetValueA", "RegDeleteTreeW", "SHGetKnownFolderPath",
	"SHCreateItemFromParsingName", "TaskDialog", "TaskDialogIndirect", "SetProcessDPIAware",
	"GetDpiForWindow", "GetDpiForSystem", "SetProcessDpiAwarenessContext",
	"AddClipboardFormatListener", "ChangeWindowMessageFilter", "ChangeWindowMessageFilterEx",
	/* msvcrt: exported only by the Vista+ msvcrt.dll */
	"_ftelli64", "_fseeki64", "_ftelli64_nolock", "_fseeki64_nolock",
	"_wfopen_s", "fopen_s", "_localtime64_s", "_gmtime64_s", "_vscwprintf_p",
};

static int ieq(const char *a, const char *b)
{
	return _stricmp(a, b) == 0;
}

static int is_allowed_dll(const char *name)
{
	size_t i;
	for (i = 0; i < sizeof(allowed_dlls) / sizeof(allowed_dlls[0]); i++)
		if (ieq(name, allowed_dlls[i]))
			return 1;
	return 0;
}

static int is_denied(const char *dll, const char *fn)
{
	size_t i, n;
	for (i = 0; i < sizeof(denied_functions) / sizeof(denied_functions[0]); i++)
		if (strcmp(fn, denied_functions[i]) == 0)
			return 1;
	/* The "secure CRT" functions (strcpy_s, sprintf_s, ...) arrived in
	 * msvcrt.dll with Vista. */
	n = strlen(fn);
	if (ieq(dll, "msvcrt.dll") && n > 2 && fn[n - 2] == '_' && fn[n - 1] == 's')
		return 1;
	return 0;
}

typedef struct {
	uint8_t *data;
	size_t size;
	IMAGE_SECTION_HEADER *sections;
	int nsections;
} Image;

/* Converts a relative virtual address to a pointer into the file, or NULL
 * if it is not inside any section (or runs off the end of the file). */
static const void *rva(const Image *img, DWORD addr, size_t need)
{
	int i;
	for (i = 0; i < img->nsections; i++) {
		const IMAGE_SECTION_HEADER *s = &img->sections[i];
		DWORD span = s->SizeOfRawData > s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
		if (addr >= s->VirtualAddress && addr < s->VirtualAddress + span) {
			size_t off = s->PointerToRawData + (addr - s->VirtualAddress);
			if (off + need > img->size)
				return NULL;
			return img->data + off;
		}
	}
	return NULL;
}

static const char *rva_str(const Image *img, DWORD addr)
{
	const char *p = (const char *)rva(img, addr, 1);
	size_t max;
	if (p == NULL)
		return NULL;
	max = img->size - (size_t)((const uint8_t *)p - img->data);
	return memchr(p, 0, max) ? p : NULL;
}

/* Returns the number of problems found, or -1 if the file is unreadable.
 * `imports_seen` (optional) receives the number of functions imported. */
static int check_file(const char *path, int verbose, int *imports_seen)
{
	FILE *f = fopen(path, "rb");
	Image img;
	IMAGE_DOS_HEADER *dos;
	IMAGE_NT_HEADERS32 *nt;
	int is64, problems = 0, count = 0;
	WORD major_ss, minor_ss, major_os, minor_os, max_minor;
	DWORD import_rva, import_size;
	long len;

	memset(&img, 0, sizeof(img));
	if (f == NULL) {
		fprintf(stderr, "%s: cannot open\n", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	img.size = len > 0 ? (size_t)len : 0;
	img.data = (uint8_t *)malloc(img.size + 1);
	if (img.data == NULL || fread(img.data, 1, img.size, f) != img.size) {
		fclose(f);
		free(img.data);
		fprintf(stderr, "%s: cannot read\n", path);
		return -1;
	}
	fclose(f);

	dos = (IMAGE_DOS_HEADER *)img.data;
	if (img.size < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
		(size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > img.size) {
		fprintf(stderr, "%s: not a PE file\n", path);
		free(img.data);
		return -1;
	}
	nt = (IMAGE_NT_HEADERS32 *)(img.data + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		fprintf(stderr, "%s: not a PE file\n", path);
		free(img.data);
		return -1;
	}
	is64 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
	if (is64) {
		IMAGE_NT_HEADERS64 *nt64 = (IMAGE_NT_HEADERS64 *)nt;
		major_ss = nt64->OptionalHeader.MajorSubsystemVersion;
		minor_ss = nt64->OptionalHeader.MinorSubsystemVersion;
		major_os = nt64->OptionalHeader.MajorOperatingSystemVersion;
		minor_os = nt64->OptionalHeader.MinorOperatingSystemVersion;
		import_rva = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		import_size = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
	} else {
		major_ss = nt->OptionalHeader.MajorSubsystemVersion;
		minor_ss = nt->OptionalHeader.MinorSubsystemVersion;
		major_os = nt->OptionalHeader.MajorOperatingSystemVersion;
		minor_os = nt->OptionalHeader.MinorOperatingSystemVersion;
		import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		import_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
	}
	img.sections = (IMAGE_SECTION_HEADER *)((uint8_t *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
	img.nsections = nt->FileHeader.NumberOfSections;
	if ((uint8_t *)(img.sections + img.nsections) > img.data + img.size) {
		fprintf(stderr, "%s: truncated section table\n", path);
		free(img.data);
		return -1;
	}

	max_minor = is64 ? 2 : 1;
	if (major_ss > 5 || (major_ss == 5 && minor_ss > max_minor)) {
		fprintf(stderr, "%s: subsystem version %u.%u is newer than Windows XP (5.%u)\n",
			path, major_ss, minor_ss, max_minor);
		problems++;
	}
	if (major_os > 5 || (major_os == 5 && minor_os > max_minor)) {
		fprintf(stderr, "%s: OS version %u.%u is newer than Windows XP (5.%u)\n",
			path, major_os, minor_os, max_minor);
		problems++;
	}

	if (import_rva != 0 && import_size != 0) {
		const IMAGE_IMPORT_DESCRIPTOR *d;
		for (d = (const IMAGE_IMPORT_DESCRIPTOR *)rva(&img, import_rva, sizeof(*d));
			d != NULL && d->Name != 0;
			d = (const IMAGE_IMPORT_DESCRIPTOR *)((const uint8_t *)d + sizeof(*d))) {
			const char *dll = rva_str(&img, d->Name);
			DWORD thunk_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
			size_t step = is64 ? 8 : 4, k;
			if (dll == NULL)
				break;
			if (!is_allowed_dll(dll)) {
				fprintf(stderr, "%s: imports from unexpected DLL %s\n", path, dll);
				problems++;
			}
			for (k = 0;; k++) {
				const uint8_t *t = (const uint8_t *)rva(&img, thunk_rva + (DWORD)(k * step), step);
				uint64_t v;
				const char *fn;
				if (t == NULL)
					break;
				v = is64 ? *(const uint64_t *)t : *(const uint32_t *)t;
				if (v == 0)
					break;
				count++;
				/* Import by ordinal: nothing to check by name. */
				if (is64 ? (v >> 63) != 0 : (v & 0x80000000u) != 0)
					continue;
				fn = rva_str(&img, (DWORD)v + 2); /* skip the 2-byte hint */
				if (fn == NULL)
					continue;
				if (verbose)
					printf("  %s!%s\n", dll, fn);
				if (is_denied(dll, fn)) {
					fprintf(stderr, "%s: imports %s!%s, which Windows XP does not have\n", path, dll, fn);
					problems++;
				}
			}
		}
	}
	free(img.data);
	if (imports_seen)
		*imports_seen = count;
	return problems;
}

static int self_test(const char *argv0)
{
	int failures = 0, imports = 0;
	char self[MAX_PATH];
	(void)argv0;
	/* The denylist logic. */
	failures += !is_denied("KERNEL32.dll", "GetTickCount64");
	failures += is_denied("KERNEL32.dll", "GetTickCount");
	failures += !is_denied("msvcrt.dll", "strcpy_s");
	failures += !is_denied("MSVCRT.DLL", "_wfopen_s");
	failures += is_denied("msvcrt.dll", "_wfopen");
	failures += is_denied("KERNEL32.dll", "Foo_s"); /* the _s rule is for msvcrt only */
	failures += !is_allowed_dll("KERNEL32.dll");
	failures += is_allowed_dll("api-ms-win-crt-runtime-l1-1-0.dll");
	failures += is_allowed_dll("ucrtbase.dll");
	/* The PE parser, on this very executable: it must parse, import some
	 * functions, and (being built with the same flags as the app) pass. */
	if (!GetModuleFileNameA(NULL, self, MAX_PATH))
		return 1;
	if (check_file(self, 0, &imports) != 0)
		failures++;
	if (imports < 5)
		failures++;
	printf("self-test: %s (%d imports parsed)\n", failures ? "FAILED" : "ok", imports);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	int i, total = 0, verbose = 0, files = 0;
	if (argc > 1 && strcmp(argv[1], "--self-test") == 0)
		return self_test(argv[0]);
	for (i = 1; i < argc; i++) {
		int r;
		if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
			continue;
		}
		files++;
		r = check_file(argv[i], verbose, NULL);
		if (r < 0)
			return 2;
		total += r;
		if (r == 0)
			printf("%s: OK for Windows XP\n", argv[i]);
	}
	if (files == 0) {
		fprintf(stderr, "usage: xpcheck [-v] <file.exe>...  |  xpcheck --self-test\n");
		return 2;
	}
	return total ? 1 : 0;
}
