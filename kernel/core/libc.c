#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"

void *memcpy(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int value, size_t len) {
    uint8_t *d = (uint8_t *)dst;

    for (size_t i = 0; i < len; ++i) {
        d[i] = (uint8_t)value;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || len == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < len; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = len; i > 0; --i) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;

    while (s[len]) {
        ++len;
    }
    return len;
}

br_prng_seeder br_prng_seeder_system(const char **name) {
    if (name) {
        *name = "none";
    }
    return 0;
}
