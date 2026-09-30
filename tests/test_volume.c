/*
 * test_volume.c - the software volume: the percentage-to-gain curve and the
 * sample scaling. Pure arithmetic, so unlike the player suite this runs
 * everywhere, including CI machines with no sound device.
 */
#include <stdint.h>

#include "mptest.h"

#include "volume.h"

static void clamping(void)
{
	CHECK_INT(mp_volume_clamp(0), 0);
	CHECK_INT(mp_volume_clamp(1), 1);
	CHECK_INT(mp_volume_clamp(55), 55);
	CHECK_INT(mp_volume_clamp(99), 99);
	CHECK_INT(mp_volume_clamp(100), 100);
	/* Just outside, far outside, and the int extremes. */
	CHECK_INT(mp_volume_clamp(-1), 0);
	CHECK_INT(mp_volume_clamp(101), 100);
	CHECK_INT(mp_volume_clamp(-5000), 0);
	CHECK_INT(mp_volume_clamp(5000), 100);
	CHECK_INT(mp_volume_clamp(INT32_MIN), 0);
	CHECK_INT(mp_volume_clamp(INT32_MAX), 100);
}

static void gain_curve(void)
{
	int p;
	uint32_t prev;

	/* The ends are exact: full volume must be bit-exact playback, and zero
	 * must be true silence. */
	CHECK_INT(mp_volume_gain(100), MP_VOLUME_UNITY);
	CHECK_INT(mp_volume_gain(0), 0);

	/* Squared: 50% is a quarter (-12 dB), 10% a hundredth (-40 dB). */
	CHECK_INT(mp_volume_gain(50), MP_VOLUME_UNITY / 4);
	CHECK_INT(mp_volume_gain(10), 655);      /* 655.36 rounded */
	CHECK_INT(mp_volume_gain(1), 7);         /* 6.5536 rounded up, not to 0 */
	CHECK_INT(mp_volume_gain(99), 64232);    /* 64231.8 rounded */

	/* Out-of-range input is clamped, never wrapped. */
	CHECK_INT(mp_volume_gain(-1), 0);
	CHECK_INT(mp_volume_gain(INT32_MIN), 0);
	CHECK_INT(mp_volume_gain(101), MP_VOLUME_UNITY);
	CHECK_INT(mp_volume_gain(INT32_MAX), MP_VOLUME_UNITY);

	/* Every step up the slider is strictly louder, so no two neighboring
	 * positions sound the same, and nothing goes above unity. */
	prev = mp_volume_gain(0);
	for (p = 1; p <= 100; p++) {
		uint32_t g = mp_volume_gain(p);
		CHECK(g > prev);
		CHECK(g <= MP_VOLUME_UNITY);
		prev = g;
	}
}

static void apply_unity_is_exact(void)
{
	int16_t s[] = { 0, 1, -1, 12345, -12345, 32767, -32768 };
	int16_t want[] = { 0, 1, -1, 12345, -12345, 32767, -32768 };
	size_t i;
	mp_volume_apply(s, 7, MP_VOLUME_UNITY);
	for (i = 0; i < 7; i++)
		CHECK_INT(s[i], want[i]);
	/* A gain above unity is treated as unity: never amplify, never clip. */
	mp_volume_apply(s, 7, MP_VOLUME_UNITY * 2);
	for (i = 0; i < 7; i++)
		CHECK_INT(s[i], want[i]);
	mp_volume_apply(s, 7, 0xFFFFFFFFu);
	for (i = 0; i < 7; i++)
		CHECK_INT(s[i], want[i]);
}

static void apply_zero_is_silence(void)
{
	int16_t s[] = { 1, -1, 32767, -32768, 500 };
	size_t i;
	mp_volume_apply(s, 5, 0);
	for (i = 0; i < 5; i++)
		CHECK_INT(s[i], 0);
}

static void apply_scaling(void)
{
	int16_t s[] = { 1000, -1000, 32767, -32768, 3, -3, 1, -1, 0 };

	/* Half and quarter gain. */
	mp_volume_apply(s, 9, MP_VOLUME_UNITY / 2);
	CHECK_INT(s[0], 500);
	CHECK_INT(s[1], -500);
	CHECK_INT(s[2], 16383);   /* 16383.5 toward zero */
	CHECK_INT(s[3], -16384);  /* the extreme: no overflow */
	/* Rounding is toward zero on both sides, so the result is symmetric:
	 * a negative sample is never pushed further than its positive twin. */
	CHECK_INT(s[4], 1);
	CHECK_INT(s[5], -1);
	CHECK_INT(s[6], 0);
	CHECK_INT(s[7], 0);
	CHECK_INT(s[8], 0);

	/* One step under unity, on the extremes: the largest possible products,
	 * which must not overflow 32 bits. */
	{
		int16_t e[] = { 32767, -32768 };
		mp_volume_apply(e, 2, MP_VOLUME_UNITY - 1);
		CHECK_INT(e[0], 32766);
		CHECK_INT(e[1], -32767);
	}

	/* The smallest audible gain still passes loud samples through. */
	{
		int16_t e[] = { 32767, -32768, 9363, 9362 };
		mp_volume_apply(e, 4, mp_volume_gain(1)); /* 7 / 65536 */
		CHECK_INT(e[0], 3);
		CHECK_INT(e[1], -3); /* -3.5 toward zero */
		CHECK_INT(e[2], 1);  /* 9363 * 7 = 65541, just over one unit */
		CHECK_INT(e[3], 0);  /* 9362 * 7 = 65534, just under */
	}
}

static void apply_count_and_null(void)
{
	int16_t s[] = { 1000, 1000, 1000 };
	/* Only `count` samples are touched: the third is past the end. */
	mp_volume_apply(s, 2, MP_VOLUME_UNITY / 2);
	CHECK_INT(s[0], 500);
	CHECK_INT(s[1], 500);
	CHECK_INT(s[2], 1000);
	/* Zero samples, and a NULL buffer, are harmless no-ops. */
	mp_volume_apply(s, 0, 0);
	CHECK_INT(s[0], 500);
	mp_volume_apply(NULL, 0, 0);
	mp_volume_apply(NULL, 100, MP_VOLUME_UNITY / 2);
}

int suite_volume(void)
{
	clamping();
	gain_curve();
	apply_unity_is_exact();
	apply_zero_is_silence();
	apply_scaling();
	apply_count_and_null();
	return 0;
}
