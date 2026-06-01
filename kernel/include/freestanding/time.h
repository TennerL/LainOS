#ifndef LAINOS_FREESTANDING_TIME_H
#define LAINOS_FREESTANDING_TIME_H

#include <stddef.h>

typedef __INT64_TYPE__ time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
time_t time(time_t *timer);
double difftime(time_t time1, time_t time0);
size_t strftime(char *buffer, size_t size, const char *format, const struct tm *timeptr);
char *strptime(const char *buffer, const char *format, struct tm *timeptr);
time_t mktime(struct tm *timeptr);

#endif
