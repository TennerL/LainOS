#ifndef LAINOS_FREESTANDING_SYS_TIME_H
#define LAINOS_FREESTANDING_SYS_TIME_H

#include <time.h>

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);

#ifndef timerclear
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#endif

#ifndef timerisset
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#endif

#endif
