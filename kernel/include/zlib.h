#ifndef LAINOS_ZLIB_COMPAT_H
#define LAINOS_ZLIB_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#define Z_NULL 0
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_NO_FLUSH 0
#define MAX_WBITS 15

typedef void *gzFile;

typedef struct z_stream_s {
    uint8_t *next_in;
    unsigned int avail_in;
    uint8_t *next_out;
    unsigned int avail_out;
    void *opaque;
    void *(*zalloc)(void *opaque, unsigned int items, unsigned int size);
    void (*zfree)(void *opaque, void *address);
} z_stream;

int inflateInit2(z_stream *stream, int window_bits);
int inflate(z_stream *stream, int flush);
int inflateEnd(z_stream *stream);
gzFile gzopen(const char *path, const char *mode);
char *gzgets(gzFile file, char *buffer, int length);
int gzclose(gzFile file);

#endif
