#ifndef LAINOS_FREESTANDING_STDIO_H
#define LAINOS_FREESTANDING_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct FILE FILE;

extern FILE *stderr;

int fprintf(FILE *stream, const char *format, ...);
int fflush(FILE *stream);
int sscanf(const char *buffer, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list ap);

#endif
