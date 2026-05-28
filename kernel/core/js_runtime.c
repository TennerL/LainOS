#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

static const double JS_PI = 3.14159265358979323846;
static const double JS_HALF_PI = 1.57079632679489661923;
static const double JS_TWO_PI = 6.28318530717958647692;
static const double JS_LN2 = 0.69314718055994530942;
static const double JS_LN10 = 2.30258509299404568402;

int sprintf(char *str, const char *fmt, ...) {
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vsnprintf(str, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

double floor(double x) {
    long long i;

    if (x != x || x > 9223372036854774784.0 || x < -9223372036854774784.0) {
        return x;
    }
    i = (long long)x;
    if ((double)i > x) {
        --i;
    }
    return (double)i;
}

double trunc(double x) {
    if (x != x || x > 9223372036854774784.0 || x < -9223372036854774784.0) {
        return x;
    }
    return (double)(long long)x;
}

double fmod(double x, double y) {
    double q;

    if (y == 0.0 || x != x || y != y) {
        return 0.0 / 0.0;
    }
    q = trunc(x / y);
    return x - q * y;
}

double sqrt(double x) {
    double guess;
    unsigned int i;

    if (x < 0.0) {
        return 0.0 / 0.0;
    }
    if (x == 0.0) {
        return 0.0;
    }
    guess = x > 1.0 ? x : 1.0;
    for (i = 0; i < 24u; ++i) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

static double js_reduce_angle(double x) {
    x = fmod(x, JS_TWO_PI);
    if (x > JS_PI) {
        x -= JS_TWO_PI;
    } else if (x < -JS_PI) {
        x += JS_TWO_PI;
    }
    return x;
}

double sin(double x) {
    double x2;
    double term;
    double sum;
    unsigned int n;

    x = js_reduce_angle(x);
    x2 = x * x;
    term = x;
    sum = x;
    for (n = 1u; n < 10u; ++n) {
        term *= -x2 / ((double)(2u * n) * (double)(2u * n + 1u));
        sum += term;
    }
    return sum;
}

double cos(double x) {
    double x2;
    double term = 1.0;
    double sum = 1.0;
    unsigned int n;

    x = js_reduce_angle(x);
    x2 = x * x;
    for (n = 1u; n < 10u; ++n) {
        term *= -x2 / ((double)(2u * n - 1u) * (double)(2u * n));
        sum += term;
    }
    return sum;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) {
        return x < 0.0 ? -1.0 / 0.0 : 1.0 / 0.0;
    }
    return sin(x) / c;
}

double atan(double x) {
    double ax = x < 0.0 ? -x : x;
    double term;
    double sum;
    double x2;
    unsigned int n;

    if (ax > 1.0) {
        double base = JS_HALF_PI - atan(1.0 / ax);
        return x < 0.0 ? -base : base;
    }
    x2 = x * x;
    term = x;
    sum = x;
    for (n = 1u; n < 18u; ++n) {
        term *= -x2;
        sum += term / (double)(2u * n + 1u);
    }
    return sum;
}

double atan2(double y, double x) {
    if (x > 0.0) {
        return atan(y / x);
    }
    if (x < 0.0 && y >= 0.0) {
        return atan(y / x) + JS_PI;
    }
    if (x < 0.0 && y < 0.0) {
        return atan(y / x) - JS_PI;
    }
    if (y > 0.0) {
        return JS_HALF_PI;
    }
    if (y < 0.0) {
        return -JS_HALF_PI;
    }
    return 0.0;
}

double asin(double x) {
    if (x > 1.0 || x < -1.0) {
        return 0.0 / 0.0;
    }
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x > 1.0 || x < -1.0) {
        return 0.0 / 0.0;
    }
    return JS_HALF_PI - asin(x);
}

double exp(double x) {
    double term = 1.0;
    double sum = 1.0;
    unsigned int n;
    bool neg = false;

    if (x < 0.0) {
        neg = true;
        x = -x;
    }
    while (x > 1.0) {
        double half = exp(x * 0.5);
        double out = half * half;
        return neg ? 1.0 / out : out;
    }
    for (n = 1u; n < 32u; ++n) {
        term *= x / (double)n;
        sum += term;
    }
    return neg ? 1.0 / sum : sum;
}

double log(double x) {
    double z;
    double z2;
    double term;
    double sum;
    int k = 0;
    unsigned int n;

    if (x < 0.0) {
        return 0.0 / 0.0;
    }
    if (x == 0.0) {
        return -1.0 / 0.0;
    }
    while (x > 1.5) {
        x *= 0.5;
        ++k;
    }
    while (x < 0.75) {
        x *= 2.0;
        --k;
    }
    z = (x - 1.0) / (x + 1.0);
    z2 = z * z;
    term = z;
    sum = z;
    for (n = 1u; n < 32u; ++n) {
        term *= z2;
        sum += term / (double)(2u * n + 1u);
    }
    return 2.0 * sum + (double)k * JS_LN2;
}

double log2(double x) {
    return log(x) / JS_LN2;
}

double log10(double x) {
    return log(x) / JS_LN10;
}

double pow(double x, double y) {
    long long yi;
    double result = 1.0;
    bool neg_exp = false;

    if (y == 0.0) {
        return 1.0;
    }
    yi = (long long)y;
    if ((double)yi == y && yi > -64 && yi < 64) {
        if (yi < 0) {
            neg_exp = true;
            yi = -yi;
        }
        while (yi-- > 0) {
            result *= x;
        }
        return neg_exp ? 1.0 / result : result;
    }
    if (x <= 0.0) {
        return 0.0 / 0.0;
    }
    return exp(y * log(x));
}

double cbrt(double x) {
    double guess;
    bool neg = false;
    unsigned int i;

    if (x == 0.0) {
        return 0.0;
    }
    if (x < 0.0) {
        neg = true;
        x = -x;
    }
    guess = x > 1.0 ? x : 1.0;
    for (i = 0; i < 32u; ++i) {
        guess = (2.0 * guess + x / (guess * guess)) / 3.0;
    }
    return neg ? -guess : guess;
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

time_t mktime(struct tm *tm) {
    (void)tm;
    return 0;
}

struct tm *gmtime_r(const time_t *timer, struct tm *result) {
    if (result == NULL) {
        return NULL;
    }
    (void)timer;
    result->tm_sec = 0;
    result->tm_min = 0;
    result->tm_hour = 0;
    result->tm_mday = 1;
    result->tm_mon = 0;
    result->tm_year = 70;
    result->tm_wday = 4;
    result->tm_yday = 0;
    result->tm_isdst = 0;
    return result;
}

struct tm *localtime_r(const time_t *timer, struct tm *result) {
    return gmtime_r(timer, result);
}

char *strptime(const char *s, const char *format, struct tm *tm) {
    (void)s;
    (void)format;
    (void)tm;
    return NULL;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)format;
    (void)tm;
    if (max != 0u) {
        s[0] = '\0';
    }
    return 0;
}
