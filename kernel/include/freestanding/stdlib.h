#ifndef LAINOS_FREESTANDING_STDLIB_H
#define LAINOS_FREESTANDING_STDLIB_H

#include <stddef.h>

void abort(void) __attribute__((noreturn));
int atexit(void (*function)(void));
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
long long strtoll(const char *nptr, char **endptr, int base);

#endif
