#ifndef LAINOS_FREESTANDING_STDLIB_H
#define LAINOS_FREESTANDING_STDLIB_H

#include <stddef.h>

void abort(void) __attribute__((noreturn));
int atexit(void (*function)(void));
int atoi(const char *nptr);
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
char *getenv(const char *name);
void *bsearch(const void *key,
              const void *base,
              size_t count,
              size_t size,
              int (*compar)(const void *, const void *));
void qsort(void *base,
           size_t count,
           size_t size,
           int (*compar)(const void *, const void *));
char *realpath(const char *path, char *resolved_path);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

#endif
