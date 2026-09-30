/*
 * test_main.c - runs one test suite (by name) or all of them.
 *
 *   mp_tests            run every suite
 *   mp_tests tags       run one suite
 *   mp_tests --list     list the suite names
 *
 * Exit code: 0 = passed, 1 = failed, 77 = skipped (CTest's convention,
 * configured with SKIP_RETURN_CODE in CMakeLists.txt).
 */
#include <stdlib.h>

#include "mptest.h"

int mpt_checks = 0;
int mpt_failures = 0;

/* Non-ASCII test strings would print as mojibake in a console, so print
 * them as \uXXXX escapes instead. */
static void print_wide(const wchar_t *s)
{
	if (s == NULL) {
		fprintf(stderr, "(null)");
		return;
	}
	fputc('"', stderr);
	for (; *s != 0; s++) {
		if (*s >= 0x20 && *s < 0x7F)
			fputc((int)*s, stderr);
		else
			fprintf(stderr, "\\u%04X", (unsigned)*s);
	}
	fputc('"', stderr);
}

void mpt_fail(const char *file, int line, const char *expr)
{
	mpt_failures++;
	fprintf(stderr, "%s:%d: FAILED: %s\n", file, line, expr);
}

void mpt_fail_int(const char *file, int line, const char *expr, long long got, long long want)
{
	mpt_failures++;
	fprintf(stderr, "%s:%d: FAILED: %s (got %lld, want %lld)\n", file, line, expr, got, want);
}

void mpt_fail_wstr(const char *file, int line, const char *expr, const wchar_t *got, const wchar_t *want)
{
	mpt_failures++;
	fprintf(stderr, "%s:%d: FAILED: %s\n    got:  ", file, line, expr);
	print_wide(got);
	fprintf(stderr, "\n    want: ");
	print_wide(want);
	fprintf(stderr, "\n");
}

typedef struct {
	const char *name;
	int (*run)(void);
} Suite;

static const Suite suites[] = {
	{ "text", suite_text },
	{ "tags", suite_tags },
	{ "track", suite_track },
	{ "playlist", suite_playlist },
	{ "decoder", suite_decoder },
	{ "player", suite_player },
};

#define SUITE_COUNT (sizeof(suites) / sizeof(suites[0]))

int main(int argc, char **argv)
{
	size_t i;
	int ran = 0, skipped = 0;
	/* Unbuffered, so output interleaves correctly with CTest's capture
	 * even if a test crashes. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc > 1 && strcmp(argv[1], "--list") == 0) {
		for (i = 0; i < SUITE_COUNT; i++)
			printf("%s\n", suites[i].name);
		return 0;
	}
	for (i = 0; i < SUITE_COUNT; i++) {
		int before, result;
		if (argc > 1 && strcmp(argv[1], suites[i].name) != 0)
			continue;
		before = mpt_failures;
		printf("[%s]\n", suites[i].name);
		result = suites[i].run();
		ran++;
		if (result == MPT_SKIP) {
			printf("  skipped\n");
			skipped++;
		} else {
			printf("  %s\n", mpt_failures == before ? "ok" : "FAILED");
		}
	}
	if (ran == 0) {
		fprintf(stderr, "unknown suite '%s' (try --list)\n", argc > 1 ? argv[1] : "");
		return 2;
	}
	printf("%d checks, %d failures\n", mpt_checks, mpt_failures);
	if (mpt_failures > 0)
		return 1;
	/* Only report "skipped" when everything that was asked for skipped. */
	return skipped == ran ? MPT_SKIP : 0;
}
