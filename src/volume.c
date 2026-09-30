/*
 * volume.c - software volume. See volume.h for why it is done this way.
 */
#include "volume.h"

int mp_volume_clamp(int percent)
{
	if (percent < 0)
		return 0;
	if (percent > MP_VOLUME_MAX)
		return MP_VOLUME_MAX;
	return percent;
}

uint32_t mp_volume_gain(int percent)
{
	uint32_t p = (uint32_t)mp_volume_clamp(percent);
	/* UNITY * p^2 / 100^2, rounded. The largest intermediate value is
	 * 65536 * 10000 + 5000, comfortably inside 32 bits. */
	return (MP_VOLUME_UNITY * p * p + 5000u) / 10000u;
}

void mp_volume_apply(int16_t *samples, size_t count, uint32_t gain)
{
	size_t i;
	int32_t g;
	if (samples == NULL || gain >= MP_VOLUME_UNITY)
		return;
	g = (int32_t)gain;
	for (i = 0; i < count; i++) {
		/* |sample| <= 32768 and g < 65536, so the product stays below 2^31.
		 * Division (not >> 16) because it rounds toward zero, which keeps
		 * the result symmetric for negative samples and is fully defined
		 * by the C standard, where right-shifting a negative is not. */
		samples[i] = (int16_t)(((int32_t)samples[i] * g) / (int32_t)MP_VOLUME_UNITY);
	}
}
