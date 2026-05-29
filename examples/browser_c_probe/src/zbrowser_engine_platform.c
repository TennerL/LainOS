#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include "zlib.h"

extern time_t time(time_t *timer);

int errno;
FILE *stderr;
void *guit;

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv == 0) {
        errno = 22;
        return -1;
    }
    tv->tv_sec = time(0);
    tv->tv_usec = 0;
    return 0;
}

int atexit(void (*function)(void)) {
    (void)function;
    return 0;
}

char *getenv(const char *name) {
    (void)name;
    return 0;
}

int atoi(const char *nptr) {
    int value = 0;
    int sign = 1;

    if (nptr == 0) {
        return 0;
    }
    while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || *nptr == '\r') {
        ++nptr;
    }
    if (*nptr == '-') {
        sign = -1;
        ++nptr;
    } else if (*nptr == '+') {
        ++nptr;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        value = value * 10 + (*nptr - '0');
        ++nptr;
    }
    return value * sign;
}

int access(const char *path, int mode) {
    (void)path;
    (void)mode;
    errno = 2;
    return -1;
}

char *realpath(const char *path, char *resolved_path) {
    (void)path;
    (void)resolved_path;
    errno = 2;
    return 0;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

long long strtoll(const char *nptr, char **endptr, int base) {
    long value = strtol(nptr, endptr, base);
    return (long long)value;
}

int isascii(int c) {
    return (c & ~0x7f) == 0;
}

FILE *fopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    errno = 2;
    return 0;
}

int fclose(FILE *stream) {
    (void)stream;
    return 0;
}

int feof(FILE *stream) {
    (void)stream;
    return 1;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream;
    (void)offset;
    (void)whence;
    errno = 22;
    return -1;
}

long ftell(FILE *stream) {
    (void)stream;
    errno = 22;
    return -1;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)stream;
    return 0;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

char *fgets(char *buffer, int size, FILE *stream) {
    (void)buffer;
    (void)size;
    (void)stream;
    return 0;
}

int fputc(int ch, FILE *stream) {
    (void)stream;
    return ch;
}

int fputs(const char *text, FILE *stream) {
    int count = 0;
    (void)stream;
    if (text == 0) {
        return -1;
    }
    while (text[count] != 0) {
        ++count;
    }
    return count;
}

int fprintf(FILE *stream, const char *format, ...) {
    (void)stream;
    (void)format;
    return 0;
}

int printf(const char *format, ...) {
    (void)format;
    return 0;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    (void)stream;
    (void)format;
    (void)ap;
    return 0;
}

int sscanf(const char *buffer, const char *format, ...) {
    (void)buffer;
    (void)format;
    return 0;
}

void *opendir(const char *name) {
    (void)name;
    errno = 2;
    return 0;
}

int closedir(void *dirp) {
    (void)dirp;
    return 0;
}

void *readdir(void *dirp) {
    (void)dirp;
    return 0;
}

int dirfd(void *dirp) {
    (void)dirp;
    errno = 22;
    return -1;
}

int mkdir(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = 22;
    return -1;
}

int rmdir(const char *path) {
    (void)path;
    errno = 22;
    return -1;
}

long pread(int fd, void *buf, unsigned long count, long offset) {
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    errno = 22;
    return -1;
}

long pwrite(int fd, const void *buf, unsigned long count, long offset) {
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    errno = 22;
    return -1;
}

int fstatat(int dirfd_value, const char *path, struct stat *statbuf, int flags) {
    (void)dirfd_value;
    (void)path;
    (void)statbuf;
    (void)flags;
    errno = 2;
    return -1;
}

int stat(const char *path, struct stat *statbuf) {
    (void)path;
    (void)statbuf;
    errno = 2;
    return -1;
}

int unlinkat(int dirfd_value, const char *path, int flags) {
    (void)dirfd_value;
    (void)path;
    (void)flags;
    errno = 2;
    return -1;
}

int uname(struct utsname *buf) {
    uint32_t i;
    if (buf == 0) {
        errno = 22;
        return -1;
    }
    for (i = 0; i < sizeof(*buf); ++i) {
        ((uint8_t *)buf)[i] = 0;
    }
    buf->sysname[0] = 'L';
    buf->sysname[1] = 'a';
    buf->sysname[2] = 'i';
    buf->sysname[3] = 'n';
    return 0;
}

struct tm *gmtime(const time_t *timep) {
    static struct tm tm_value;
    (void)timep;
    return &tm_value;
}

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    (void)tocode;
    (void)fromcode;
    errno = 22;
    return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft) {
    (void)cd;
    (void)inbuf;
    (void)inbytesleft;
    (void)outbuf;
    (void)outbytesleft;
    errno = 22;
    return (size_t)-1;
}

int iconv_close(iconv_t cd) {
    (void)cd;
    return 0;
}

int inflateInit2(z_stream *stream, int window_bits) {
    (void)stream;
    (void)window_bits;
    return -1;
}

int inflate(z_stream *stream, int flush) {
    (void)stream;
    (void)flush;
    return -1;
}

int inflateEnd(z_stream *stream) {
    (void)stream;
    return 0;
}

gzFile gzopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    return 0;
}

char *gzgets(gzFile file, char *buffer, int length) {
    (void)file;
    (void)buffer;
    (void)length;
    return 0;
}

int gzclose(gzFile file) {
    (void)file;
    return 0;
}

void *content_get_bitmap(void *handle) {
    (void)handle;
    return 0;
}

int content_textsearch(void *handle, void *context, unsigned int flags, const char *string) {
    (void)handle;
    (void)context;
    (void)flags;
    (void)string;
    return 0;
}

int content_textsearch_clear(void *handle) {
    (void)handle;
    return 0;
}

void *hlcache_handle_get_url(void *handle) {
    (void)handle;
    return 0;
}

void hlcache_handle_release(void *handle) {
    (void)handle;
}

int hlcache_handle_retrieve() {
    return -1;
}
