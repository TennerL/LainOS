#ifndef LAINOS_FREESTANDING_SYS_SELECT_H
#define LAINOS_FREESTANDING_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>

#define FD_SETSIZE 64

typedef struct fd_set {
    unsigned long fds_bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) /
                           (8 * sizeof(unsigned long))];
} fd_set;

int select(int nfds,
           fd_set *readfds,
           fd_set *writefds,
           fd_set *exceptfds,
           struct timeval *timeout);

#endif
