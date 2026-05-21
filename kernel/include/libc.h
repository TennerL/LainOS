#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *dst, int value, size_t len);
void *memmove(void *dst, const void *src, size_t len);
int memcmp(const void *a, const void *b, size_t len);

size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int ch);
char *strrchr(const char *s, int ch);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

int *__errno_location(void);
int *errno_location(void);

#endif
