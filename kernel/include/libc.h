#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

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
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int ch);
char *strrchr(const char *s, int ch);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);

int tolower(int ch);
int toupper(int ch);
int abs(int value);
void abort(void);
time_t time(time_t *out);

long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int snprintf(char *str, size_t size, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);

double floor(double x);
double trunc(double x);
double fmod(double x, double y);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double cbrt(double x);
double fabs(double x);
double difftime(time_t time1, time_t time0);
struct tm *gmtime_r(const time_t *timer, struct tm *result);
struct tm *localtime_r(const time_t *timer, struct tm *result);
time_t mktime(struct tm *tm);
char *strptime(const char *s, const char *format, struct tm *tm);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
int setjmp(unsigned long env[8]);
void longjmp(unsigned long env[8], int value);

void *bsearch(const void *key,
              const void *base,
              size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *));

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

int *__errno_location(void);
int *errno_location(void);

#endif
