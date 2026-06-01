#ifndef LAINOS_FREESTANDING_SYS_SELECT_H
#define LAINOS_FREESTANDING_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>

#define FD_SETSIZE 64

typedef struct fd_set {
    unsigned long fds_bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) /
                           (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set) do { \
    fd_set *_fd_set = (set); \
    unsigned int _fd_i; \
    for (_fd_i = 0; _fd_i < (unsigned int)(sizeof(_fd_set->fds_bits) / sizeof(_fd_set->fds_bits[0])); ++_fd_i) { \
        _fd_set->fds_bits[_fd_i] = 0; \
    } \
} while (0)

#define FD_SET(fd, set) do { \
    fd_set *_fd_set = (set); \
    unsigned int _fd_fd = (unsigned int)(fd); \
    if (_fd_fd < FD_SETSIZE) { \
        _fd_set->fds_bits[_fd_fd / (8u * sizeof(unsigned long))] |= \
            (1ul << (_fd_fd % (8u * sizeof(unsigned long)))); \
    } \
} while (0)

#define FD_CLR(fd, set) do { \
    fd_set *_fd_set = (set); \
    unsigned int _fd_fd = (unsigned int)(fd); \
    if (_fd_fd < FD_SETSIZE) { \
        _fd_set->fds_bits[_fd_fd / (8u * sizeof(unsigned long))] &= \
            ~(1ul << (_fd_fd % (8u * sizeof(unsigned long)))); \
    } \
} while (0)

#define FD_ISSET(fd, set) ( \
    ((unsigned int)(fd) < FD_SETSIZE) && \
    (((set)->fds_bits[(unsigned int)(fd) / (8u * sizeof(unsigned long))] & \
      (1ul << ((unsigned int)(fd) % (8u * sizeof(unsigned long))))) != 0) \
)

int select(int nfds,
           fd_set *readfds,
           fd_set *writefds,
           fd_set *exceptfds,
           struct timeval *timeout);

#endif
