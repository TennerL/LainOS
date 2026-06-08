#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CCOMPAT_SHIFT 1
#define CCOMPAT_BIAS 3

/* This file is intentionally tiny, but it is plain C-shaped source. */
static uint32_t ccompat_mix(uint32_t value)
{
	return (value << CCOMPAT_SHIFT) + CCOMPAT_BIAS;
}

bool ccompat_is_even(uint32_t value)
{
	return (value & 1) == 0;
}

uint32_t ccompat_sum_bytes(const uint8_t *data, size_t len)
{
	size_t i;
	uint32_t sum;

	i = 0;
	sum = 0;
	while (i < len) {
		sum = sum + data[i];
		i = i + 1;
	}
	return ccompat_mix(sum);
}
