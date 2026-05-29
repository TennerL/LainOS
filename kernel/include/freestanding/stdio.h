#ifndef LAINOS_FREESTANDING_STDIO_H
#define LAINOS_FREESTANDING_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct FILE FILE;

#define EOF (-1)

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif

#ifndef SEEK_END
#define SEEK_END 2
#endif

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int feof(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
int fprintf(FILE *stream, const char *format, ...);
int fflush(FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
long ftell(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fputc(int ch, FILE *stream);
int fputs(const char *text, FILE *stream);
int sscanf(const char *buffer, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int vsnprintf(char *buffer, size_t size, const char *format, va_list ap);

#endif
