#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"
#include "kmem.h"
#include "libc.h"

#define LIBC_ENOMEM 12

static int libc_errno;

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

char *strcpy(char *dst, const char *src) {
    char *out = dst;

    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    for (; i < n; ++i) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *out = dst;

    while (*dst != '\0') {
        ++dst;
    }
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

int strcmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; ++i) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];

        if (ca != cb || ca == 0 || cb == 0) {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((uint8_t)*a);
        int cb = tolower((uint8_t)*b);

        if (ca != cb) {
            return ca - cb;
        }
        ++a;
        ++b;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; ++i) {
        int ca = tolower((uint8_t)a[i]);
        int cb = tolower((uint8_t)b[i]);

        if (ca != cb || ca == 0 || cb == 0) {
            return ca - cb;
        }
    }
    return 0;
}

char *strchr(const char *s, int ch) {
    char target = (char)ch;

    while (*s != '\0') {
        if (*s == target) {
            return (char *)s;
        }
        ++s;
    }
    return target == '\0' ? (char *)s : 0;
}

char *strrchr(const char *s, int ch) {
    char target = (char)ch;
    const char *last = 0;

    do {
        if (*s == target) {
            last = s;
        }
    } while (*s++ != '\0');

    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return (char *)haystack;
    }
    while (*haystack != '\0') {
        if (*haystack == *needle && memcmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        ++haystack;
    }
    return 0;
}

int tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

int toupper(int ch) {
    if (ch >= 'a' && ch <= 'z') {
        return ch - ('a' - 'A');
    }
    return ch;
}

void *bsearch(const void *key,
              const void *base,
              size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *)) {
    const uint8_t *items = (const uint8_t *)base;
    size_t low = 0;
    size_t high = nmemb;

    if (key == 0 || base == 0 || compar == 0 || size == 0) {
        return 0;
    }

    while (low < high) {
        size_t mid = low + ((high - low) / 2u);
        const void *item = items + mid * size;
        int cmp = compar(key, item);

        if (cmp == 0) {
            return (void *)item;
        }
        if (cmp < 0) {
            high = mid;
        } else {
            low = mid + 1u;
        }
    }

    return 0;
}

void *malloc(size_t size) {
    void *ptr;

    if (size == 0 || size > 0xffffffffu) {
        if (size > 0xffffffffu) {
            libc_errno = LIBC_ENOMEM;
        }
        return 0;
    }
    ptr = kmalloc((uint32_t)size);
    if (ptr == 0) {
        libc_errno = LIBC_ENOMEM;
    }
    return ptr;
}

void free(void *ptr) {
    kfree(ptr);
}

void *calloc(size_t count, size_t size) {
    size_t bytes;
    void *ptr;

    if (count != 0 && size > ((size_t)-1) / count) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    bytes = count * size;
    if (bytes > 0xffffffffu) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    ptr = malloc(bytes);
    if (ptr != 0) {
        memset(ptr, 0, bytes);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    uint32_t old_size;
    void *new_ptr;

    if (ptr == 0) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return 0;
    }
    if (size > 0xffffffffu) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }

    old_size = kmalloc_size(ptr);
    if (old_size == 0) {
        return 0;
    }
    if (size <= old_size) {
        return ptr;
    }

    new_ptr = malloc(size);
    if (new_ptr == 0) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

char *strdup(const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1u);

    if (copy == 0) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    memcpy(copy, s, len + 1u);
    return copy;
}

int *__errno_location(void) {
    return &libc_errno;
}

int *errno_location(void) {
    return &libc_errno;
}

br_prng_seeder br_prng_seeder_system(const char **name) {
    if (name) {
        *name = "none";
    }
    return 0;
}
