#ifndef LAINOS_FREESTANDING_UNISTD_H
#define LAINOS_FREESTANDING_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif

#ifndef SEEK_END
#define SEEK_END 2
#endif

int close(int fd);
int ftruncate(int fd, off_t length);
pid_t getpid(void);
off_t lseek(int fd, off_t offset, int whence);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t read(int fd, void *buf, size_t count);
int unlink(const char *path);
ssize_t write(int fd, const void *buf, size_t count);

#endif
