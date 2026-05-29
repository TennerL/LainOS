#ifndef LAINOS_FREESTANDING_STDIO_H
#define LAINOS_FREESTANDING_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct FILE FILE;

#define EOF (-1)

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
int fflush(FILE *stream);
int fputc(int ch, FILE *stream);
int fputs(const char *text, FILE *stream);
int sscanf(const char *buffer, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int vsnprintf(char *buffer, size_t size, const char *format, va_list ap);

#endif
