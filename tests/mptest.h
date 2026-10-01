/*
 * mptest.h - a deliberately tiny test framework.
 *
 * No third-party framework, so the tests build with nothing but the same
 * MinGW toolchain as the app, and the test exe itself can be copied to an
 * old Windows XP machine and run there.
 *
 * CHECK records a failure and keeps going, so one run reports every broken
 * expectation in a suite, not just the first.
 */
#ifndef MP_TEST_H
#define MP_TEST_H

#include <stdio.h>
#include <string.h>
#include <wchar.h>

extern int mpt_checks;
extern int mpt_failures;

void mpt_fail(const char *file, int line, const char *expr);
void mpt_fail_wstr(const char *file, int line, const char *expr, const wchar_t *got, const wchar_t *want);
void mpt_fail_int(const char *file, int line, const char *expr, long long got, long long want);

#define CHECK(cond) \
	do { \
		mpt_checks++; \
		if (!(cond)) \
			mpt_fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define CHECK_INT(got, want) \
	do { \
		long long mpt_g_ = (long long)(got), mpt_w_ = (long long)(want); \
		mpt_checks++; \
		if (mpt_g_ != mpt_w_) \
			mpt_fail_int(__FILE__, __LINE__, #got " == " #want, mpt_g_, mpt_w_); \
	} while (0)

#define CHECK_WSTR(got, want) \
	do { \
		const wchar_t *mpt_g_ = (got), *mpt_w_ = (want); \
		mpt_checks++; \
		if (mpt_g_ == NULL || wcscmp(mpt_g_, mpt_w_) != 0) \
			mpt_fail_wstr(__FILE__, __LINE__, #got " == " #want, mpt_g_, mpt_w_); \
	} while (0)

/* Suite entry points, one per test_*.c file. Each returns 0 normally, or
 * MPT_SKIP if it could not run (e.g. no audio device). */
#define MPT_SKIP 77
int suite_text(void);
int suite_tags(void);
int suite_track(void);
int suite_playlist(void);
int suite_decoder(void);
int suite_player(void);
int suite_volume(void);
int suite_glyph(void);
int suite_stretch(void);

#endif
