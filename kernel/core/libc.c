#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "bearssl.h"
#include "kernel.h"
#include "kmem.h"
#include "libc.h"
#include "shell.h"

#define LIBC_ENOMEM 12
#define LIBC_EINVAL 22
#define LIBC_ERANGE 34

static int libc_errno;

void *memcpy(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int value, size_t len) {
    uint8_t *d = (uint8_t *)dst;

    for (size_t i = 0; i < len; ++i) {
        d[i] = (uint8_t)value;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || len == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < len; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = len; i > 0; --i) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;

    while (s[len]) {
        ++len;
    }
    return len;
}

char *strcpy(char *dst, const char *src) {
    char *out = dst;

    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    for (; i < n; ++i) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *out = dst;

    while (*dst != '\0') {
        ++dst;
    }
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

int strcmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; ++i) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];

        if (ca != cb || ca == 0 || cb == 0) {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((uint8_t)*a);
        int cb = tolower((uint8_t)*b);

        if (ca != cb) {
            return ca - cb;
        }
        ++a;
        ++b;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; ++i) {
        int ca = tolower((uint8_t)a[i]);
        int cb = tolower((uint8_t)b[i]);

        if (ca != cb || ca == 0 || cb == 0) {
            return ca - cb;
        }
    }
    return 0;
}

char *strchr(const char *s, int ch) {
    char target = (char)ch;

    while (*s != '\0') {
        if (*s == target) {
            return (char *)s;
        }
        ++s;
    }
    return target == '\0' ? (char *)s : 0;
}

char *strrchr(const char *s, int ch) {
    char target = (char)ch;
    const char *last = 0;

    do {
        if (*s == target) {
            last = s;
        }
    } while (*s++ != '\0');

    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return (char *)haystack;
    }
    while (*haystack != '\0') {
        if (*haystack == *needle && memcmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        ++haystack;
    }
    return 0;
}

int tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

int toupper(int ch) {
    if (ch >= 'a' && ch <= 'z') {
        return ch - ('a' - 'A');
    }
    return ch;
}

int abs(int value) {
    if (value == (-2147483647 - 1)) {
        return value;
    }
    return value < 0 ? -value : value;
}

void abort(void) {
    uint64_t caller = (uint64_t)(uintptr_t)__builtin_return_address(0);
    void **frame = (void **)__builtin_frame_address(0);
    const char *module_name = 0;
    const char *export_name = 0;
    uint64_t module_base = 0;
    uint64_t export_value = 0;
    uint32_t module_size = 0;

    console_puts("abort caller=0x");
    console_put_hex64(caller);
    if (shell_module_resolve_address(caller,
                                     &module_name,
                                     &module_base,
                                     &module_size,
                                     &export_name,
                                     &export_value) == 0) {
        console_puts(" module=");
        console_puts(module_name != 0 ? module_name : "?");
        console_puts(" base=0x");
        console_put_hex64(module_base);
        console_puts(" offset=0x");
        console_put_hex64(caller - module_base);
        console_puts(" size=");
        console_put_dec64(module_size);
        if (export_name != 0) {
            console_puts(" nearest-export=");
            console_puts(export_name);
            console_puts("+0x");
            console_put_hex64(caller - export_value);
        }
    }
    console_puts("\n");
    for (uint32_t depth = 0; frame != 0 && depth < 8u; ++depth) {
        uint64_t ret = (uint64_t)(uintptr_t)frame[1];
        void **next = (void **)frame[0];

        console_puts("abort bt[");
        console_put_dec64(depth);
        console_puts("]=0x");
        console_put_hex64(ret);
        if (shell_module_resolve_address(ret,
                                         &module_name,
                                         &module_base,
                                         &module_size,
                                         &export_name,
                                         &export_value) == 0) {
            console_puts(" module=");
            console_puts(module_name != 0 ? module_name : "?");
            console_puts(" offset=0x");
            console_put_hex64(ret - module_base);
            if (export_name != 0) {
                console_puts(" nearest=");
                console_puts(export_name);
                console_puts("+0x");
                console_put_hex64(ret - export_value);
            }
        }
        console_puts("\n");
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    console_panic("abort");
    for (;;) {
    }
}

time_t time(time_t *out) {
    time_t now = (time_t)clock_unix_time();

    if (out != 0) {
        *out = now;
    }
    return now;
}

static int libc_isspace(int ch) {
    return ch == ' ' || ch == '\f' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\v';
}

static int libc_digit_value(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 10;
    }
    return -1;
}

static const char *libc_strto_prefix(const char *s, int *base) {
    if (*base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && libc_digit_value((uint8_t)s[2]) >= 0 &&
            libc_digit_value((uint8_t)s[2]) < 16) {
            *base = 16;
            return s + 2;
        }
        if (s[0] == '0') {
            *base = 8;
            return s;
        }
        *base = 10;
        return s;
    }
    if (*base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        libc_digit_value((uint8_t)s[2]) >= 0 && libc_digit_value((uint8_t)s[2]) < 16) {
        return s + 2;
    }
    return s;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    const char *digits_start;
    unsigned long acc = 0;
    unsigned long limit;
    unsigned long cutoff;
    unsigned int cutlim;
    int neg = 0;
    int any = 0;
    int overflow = 0;

    if (base != 0 && (base < 2 || base > 36)) {
        libc_errno = LIBC_EINVAL;
        if (endptr != 0) {
            *endptr = (char *)nptr;
        }
        return 0;
    }

    while (libc_isspace((uint8_t)*s)) {
        ++s;
    }
    if (*s == '-' || *s == '+') {
        neg = (*s == '-');
        ++s;
    }
    s = libc_strto_prefix(s, &base);
    digits_start = s;
    limit = neg ? ((unsigned long)LONG_MAX + 1ul) : (unsigned long)LONG_MAX;
    cutoff = limit / (unsigned long)base;
    cutlim = (unsigned int)(limit % (unsigned long)base);

    while (*s != '\0') {
        int digit = libc_digit_value((uint8_t)*s);

        if (digit < 0 || digit >= base) {
            break;
        }
        any = 1;
        if (acc > cutoff || (acc == cutoff && (unsigned int)digit > cutlim)) {
            overflow = 1;
        } else if (!overflow) {
            acc = acc * (unsigned long)base + (unsigned long)digit;
        }
        ++s;
    }

    if (endptr != 0) {
        *endptr = (char *)(any ? s : nptr);
    }
    if (!any) {
        (void)digits_start;
        return 0;
    }
    if (overflow) {
        libc_errno = LIBC_ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    if (neg) {
        if (acc == ((unsigned long)LONG_MAX + 1ul)) {
            return LONG_MIN;
        }
        return -(long)acc;
    }
    return (long)acc;
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    unsigned long cutoff;
    unsigned int cutlim;
    int neg = 0;
    int any = 0;
    int overflow = 0;

    if (base != 0 && (base < 2 || base > 36)) {
        libc_errno = LIBC_EINVAL;
        if (endptr != 0) {
            *endptr = (char *)nptr;
        }
        return 0;
    }

    while (libc_isspace((uint8_t)*s)) {
        ++s;
    }
    if (*s == '-' || *s == '+') {
        neg = (*s == '-');
        ++s;
    }
    s = libc_strto_prefix(s, &base);
    cutoff = ULONG_MAX / (unsigned long)base;
    cutlim = (unsigned int)(ULONG_MAX % (unsigned long)base);

    while (*s != '\0') {
        int digit = libc_digit_value((uint8_t)*s);

        if (digit < 0 || digit >= base) {
            break;
        }
        any = 1;
        if (acc > cutoff || (acc == cutoff && (unsigned int)digit > cutlim)) {
            overflow = 1;
        } else if (!overflow) {
            acc = acc * (unsigned long)base + (unsigned long)digit;
        }
        ++s;
    }

    if (endptr != 0) {
        *endptr = (char *)(any ? s : nptr);
    }
    if (!any) {
        return 0;
    }
    if (overflow) {
        libc_errno = LIBC_ERANGE;
        return ULONG_MAX;
    }
    return neg ? (unsigned long)(0ul - acc) : acc;
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

typedef struct libc_format_out {
    char *dst;
    size_t size;
    size_t written;
} libc_format_out_t;

static void libc_format_putc(libc_format_out_t *out, char ch) {
    if (out->dst != 0 && out->size != 0 && out->written < out->size - 1u) {
        out->dst[out->written] = ch;
    }
    ++out->written;
}

static void libc_format_pad(libc_format_out_t *out, char ch, int count) {
    while (count-- > 0) {
        libc_format_putc(out, ch);
    }
}

static int libc_format_uint(char *buf, uint64_t value, unsigned int base, int uppercase) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[65];
    int len = 0;

    if (value == 0) {
        buf[0] = '0';
        return 1;
    }
    while (value != 0) {
        tmp[len++] = digits[value % base];
        value /= base;
    }
    for (int i = 0; i < len; ++i) {
        buf[i] = tmp[len - 1 - i];
    }
    return len;
}

static int libc_format_strnlen(const char *s, int precision) {
    int len = 0;

    if (s == 0) {
        s = "(null)";
    }
    while (s[len] != '\0' && (precision < 0 || len < precision)) {
        ++len;
    }
    return len;
}

static void libc_format_write_number(libc_format_out_t *out,
                                     uint64_t value,
                                     int negative,
                                     unsigned int base,
                                     int uppercase,
                                     int alt,
                                     int pointer,
                                     int plus,
                                     int space,
                                     int left,
                                     int zero,
                                     int width,
                                     int precision) {
    char digits[65];
    char sign = 0;
    const char *prefix = "";
    int prefix_len = 0;
    int digit_len;
    int precision_zeroes = 0;
    int zero_width = 0;
    int body_len;
    int pad;

    digit_len = (precision == 0 && value == 0) ? 0 : libc_format_uint(digits, value, base, uppercase);
    if (negative) {
        sign = '-';
    } else if (plus) {
        sign = '+';
    } else if (space) {
        sign = ' ';
    }
    if (pointer) {
        prefix = "0x";
        prefix_len = 2;
    } else if (alt && value != 0) {
        if (base == 16) {
            prefix = uppercase ? "0X" : "0x";
            prefix_len = 2;
        } else if (base == 8) {
            prefix = "0";
            prefix_len = 1;
        }
    }
    if (precision > digit_len) {
        precision_zeroes = precision - digit_len;
    }
    body_len = (sign ? 1 : 0) + prefix_len + precision_zeroes + digit_len;
    if (zero && !left && precision < 0 && width > body_len) {
        zero_width = width - body_len;
    }
    pad = width - body_len - zero_width;
    if (!left) {
        libc_format_pad(out, ' ', pad);
    }
    if (sign) {
        libc_format_putc(out, sign);
    }
    for (int i = 0; i < prefix_len; ++i) {
        libc_format_putc(out, prefix[i]);
    }
    libc_format_pad(out, '0', zero_width + precision_zeroes);
    for (int i = 0; i < digit_len; ++i) {
        libc_format_putc(out, digits[i]);
    }
    if (left) {
        libc_format_pad(out, ' ', pad);
    }
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    libc_format_out_t out;

    out.dst = str;
    out.size = size;
    out.written = 0;

    while (*fmt != '\0') {
        int left = 0;
        int plus = 0;
        int space = 0;
        int alt = 0;
        int zero = 0;
        int width = 0;
        int precision = -1;
        int length = 0;
        char spec;

        if (*fmt != '%') {
            libc_format_putc(&out, *fmt++);
            continue;
        }
        ++fmt;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0') {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '+') {
                plus = 1;
            } else if (*fmt == ' ') {
                space = 1;
            } else if (*fmt == '#') {
                alt = 1;
            } else {
                zero = 1;
            }
            ++fmt;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            ++fmt;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        if (*fmt == '.') {
            ++fmt;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                if (precision < 0) {
                    precision = -1;
                }
                ++fmt;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }
        if (*fmt == 'h') {
            length = -1;
            ++fmt;
            if (*fmt == 'h') {
                length = -2;
                ++fmt;
            }
        } else if (*fmt == 'l') {
            length = 1;
            ++fmt;
            if (*fmt == 'l') {
                length = 2;
                ++fmt;
            }
        } else if (*fmt == 'z') {
            length = 3;
            ++fmt;
        } else if (*fmt == 't') {
            length = 4;
            ++fmt;
        }

        spec = *fmt == '\0' ? '\0' : *fmt++;
        if (spec == '\0') {
            break;
        }
        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            int len = libc_format_strnlen(s, precision);
            int pad = width - len;

            if (s == 0) {
                s = "(null)";
            }
            if (!left) {
                libc_format_pad(&out, ' ', pad);
            }
            for (int i = 0; i < len; ++i) {
                libc_format_putc(&out, s[i]);
            }
            if (left) {
                libc_format_pad(&out, ' ', pad);
            }
        } else if (spec == 'c') {
            int ch = va_arg(ap, int);
            int pad = width - 1;

            if (!left) {
                libc_format_pad(&out, ' ', pad);
            }
            libc_format_putc(&out, (char)ch);
            if (left) {
                libc_format_pad(&out, ' ', pad);
            }
        } else if (spec == 'd' || spec == 'i') {
            int64_t value;
            uint64_t mag;

            if (length == 2) {
                value = va_arg(ap, long long);
            } else if (length == 1) {
                value = va_arg(ap, long);
            } else if (length == 3) {
                value = (int64_t)va_arg(ap, size_t);
            } else if (length == 4) {
                value = (int64_t)va_arg(ap, ptrdiff_t);
            } else {
                value = va_arg(ap, int);
                if (length == -1) {
                    value = (short)value;
                } else if (length == -2) {
                    value = (signed char)value;
                }
            }
            mag = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
            libc_format_write_number(&out,
                                     mag,
                                     value < 0,
                                     10,
                                     0,
                                     alt,
                                     0,
                                     plus,
                                     space,
                                     left,
                                     zero,
                                     width,
                                     precision);
        } else if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {
            uint64_t value;
            unsigned int base = spec == 'o' ? 8u : (spec == 'u' ? 10u : 16u);

            if (length == 2) {
                value = va_arg(ap, unsigned long long);
            } else if (length == 1) {
                value = va_arg(ap, unsigned long);
            } else if (length == 3) {
                value = va_arg(ap, size_t);
            } else if (length == 4) {
                value = (uint64_t)va_arg(ap, ptrdiff_t);
            } else {
                value = va_arg(ap, unsigned int);
                if (length == -1) {
                    value = (unsigned short)value;
                } else if (length == -2) {
                    value = (unsigned char)value;
                }
            }
            libc_format_write_number(&out,
                                     value,
                                     0,
                                     base,
                                     spec == 'X',
                                     alt,
                                     0,
                                     0,
                                     0,
                                     left,
                                     zero,
                                     width,
                                     precision);
        } else if (spec == 'p') {
            uintptr_t value = (uintptr_t)va_arg(ap, void *);

            libc_format_write_number(&out, value, 0, 16, 0, 0, 1, 0, 0, left, zero, width, precision);
        } else if (spec == '%') {
            int pad = width - 1;

            if (!left && !zero) {
                libc_format_pad(&out, ' ', pad);
            }
            if (!left && zero) {
                libc_format_pad(&out, '0', pad);
            }
            libc_format_putc(&out, '%');
            if (left) {
                libc_format_pad(&out, ' ', pad);
            }
        } else {
            libc_format_putc(&out, '%');
            libc_format_putc(&out, spec);
        }
    }

    if (str != 0 && size != 0) {
        size_t nul_pos = out.written < size ? out.written : size - 1u;
        str[nul_pos] = '\0';
    }
    return out.written > (size_t)INT_MAX ? INT_MAX : (int)out.written;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(str, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double trunc(double x) {
    return (double)((int64_t)x);
}

double floor(double x) {
    int64_t i = (int64_t)x;

    if (x < 0.0 && (double)i != x) {
        --i;
    }
    return (double)i;
}

double fmod(double x, double y) {
    int64_t q;

    if (y == 0.0) {
        return 0.0;
    }
    q = (int64_t)(x / y);
    return x - (double)q * y;
}

double sqrt(double x) {
    double guess;

    if (x <= 0.0) {
        return 0.0;
    }
    guess = x > 1.0 ? x : 1.0;
    for (uint32_t i = 0; i < 32u; ++i) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

static double libc_wrap_pi(double x) {
    const double two_pi = 6.28318530717958647692;

    while (x > 3.14159265358979323846) {
        x -= two_pi;
    }
    while (x < -3.14159265358979323846) {
        x += two_pi;
    }
    return x;
}

double sin(double x) {
    double term;
    double sum;

    x = libc_wrap_pi(x);
    term = x;
    sum = x;
    for (uint32_t n = 1; n < 8u; ++n) {
        double denom = (double)((2u * n) * (2u * n + 1u));
        term *= -(x * x) / denom;
        sum += term;
    }
    return sum;
}

double cos(double x) {
    double term;
    double sum;

    x = libc_wrap_pi(x);
    term = 1.0;
    sum = 1.0;
    for (uint32_t n = 1; n < 8u; ++n) {
        double denom = (double)((2u * n - 1u) * (2u * n));
        term *= -(x * x) / denom;
        sum += term;
    }
    return sum;
}

double tan(double x) {
    double c = cos(x);

    return c == 0.0 ? 0.0 : sin(x) / c;
}

double atan(double x) {
    const double pi_over_2 = 1.57079632679489661923;
    double sign = 1.0;
    double term;
    double sum;

    if (x < 0.0) {
        sign = -1.0;
        x = -x;
    }
    if (x > 1.0) {
        return sign * (pi_over_2 - atan(1.0 / x));
    }
    term = x;
    sum = x;
    for (uint32_t n = 1; n < 24u; ++n) {
        term *= -(x * x);
        sum += term / (double)(2u * n + 1u);
    }
    return sign * sum;
}

double atan2(double y, double x) {
    const double pi = 3.14159265358979323846;

    if (x > 0.0) {
        return atan(y / x);
    }
    if (x < 0.0 && y >= 0.0) {
        return atan(y / x) + pi;
    }
    if (x < 0.0 && y < 0.0) {
        return atan(y / x) - pi;
    }
    if (y > 0.0) {
        return pi * 0.5;
    }
    if (y < 0.0) {
        return -pi * 0.5;
    }
    return 0.0;
}

double asin(double x) {
    if (x > 1.0) {
        x = 1.0;
    } else if (x < -1.0) {
        x = -1.0;
    }
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    return 1.57079632679489661923 - asin(x);
}

double exp(double x) {
    double term = 1.0;
    double sum = 1.0;

    for (uint32_t n = 1; n < 32u; ++n) {
        term *= x / (double)n;
        sum += term;
    }
    return sum;
}

double log(double x) {
    double y;
    double y2;
    double term;
    double sum;

    if (x <= 0.0) {
        return 0.0;
    }
    y = (x - 1.0) / (x + 1.0);
    y2 = y * y;
    term = y;
    sum = y;
    for (uint32_t n = 1; n < 32u; ++n) {
        term *= y2;
        sum += term / (double)(2u * n + 1u);
    }
    return 2.0 * sum;
}

double log2(double x) {
    return log(x) / 0.69314718055994530942;
}

double log10(double x) {
    return log(x) / 2.30258509299404568402;
}

double pow(double x, double y) {
    int64_t whole = (int64_t)y;
    double out = 1.0;
    int negative = 0;

    if ((double)whole != y) {
        return exp(log(x) * y);
    }
    if (whole < 0) {
        negative = 1;
        whole = -whole;
    }
    while (whole-- > 0) {
        out *= x;
    }
    return negative && out != 0.0 ? 1.0 / out : out;
}

double cbrt(double x) {
    double guess;
    int negative = 0;

    if (x == 0.0) {
        return 0.0;
    }
    if (x < 0.0) {
        negative = 1;
        x = -x;
    }
    guess = x > 1.0 ? x : 1.0;
    for (uint32_t i = 0; i < 32u; ++i) {
        guess = (2.0 * guess + x / (guess * guess)) / 3.0;
    }
    return negative ? -guess : guess;
}

static int libc_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int libc_days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 1 && libc_is_leap_year(year)) {
        return 29;
    }
    if (month < 0 || month >= 12) {
        return 30;
    }
    return days[month];
}

static int64_t libc_days_before_year(int year) {
    int64_t days = 0;

    if (year >= 1970) {
        for (int y = 1970; y < year; ++y) {
            days += libc_is_leap_year(y) ? 366 : 365;
        }
    } else {
        for (int y = year; y < 1970; ++y) {
            days -= libc_is_leap_year(y) ? 366 : 365;
        }
    }
    return days;
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

struct tm *gmtime_r(const time_t *timer, struct tm *result) {
    int64_t seconds;
    int64_t days;
    int64_t day_seconds;
    int year = 1970;

    if (timer == 0 || result == 0) {
        return 0;
    }
    seconds = *timer;
    days = seconds / 86400;
    day_seconds = seconds % 86400;
    if (day_seconds < 0) {
        day_seconds += 86400;
        --days;
    }
    while (days >= (libc_is_leap_year(year) ? 366 : 365)) {
        days -= libc_is_leap_year(year) ? 366 : 365;
        ++year;
    }
    while (days < 0) {
        --year;
        days += libc_is_leap_year(year) ? 366 : 365;
    }
    result->tm_year = year - 1900;
    result->tm_yday = (int)days;
    result->tm_mon = 0;
    while (days >= libc_days_in_month(year, result->tm_mon)) {
        days -= libc_days_in_month(year, result->tm_mon);
        ++result->tm_mon;
    }
    result->tm_mday = (int)days + 1;
    result->tm_hour = (int)(day_seconds / 3600);
    result->tm_min = (int)((day_seconds / 60) % 60);
    result->tm_sec = (int)(day_seconds % 60);
    result->tm_wday = (int)((4 + (*timer / 86400)) % 7);
    if (result->tm_wday < 0) {
        result->tm_wday += 7;
    }
    result->tm_isdst = 0;
    return result;
}

struct tm *localtime_r(const time_t *timer, struct tm *result) {
    return gmtime_r(timer, result);
}

time_t mktime(struct tm *tm) {
    int year;
    int64_t days;

    if (tm == 0) {
        return (time_t)-1;
    }
    year = tm->tm_year + 1900;
    days = libc_days_before_year(year);
    for (int month = 0; month < tm->tm_mon; ++month) {
        days += libc_days_in_month(year, month);
    }
    days += tm->tm_mday - 1;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

static size_t libc_strftime_append(char *s, size_t max, size_t pos, const char *text) {
    while (*text != '\0') {
        if (pos + 1u < max) {
            s[pos] = *text;
        }
        ++pos;
        ++text;
    }
    return pos;
}

static size_t libc_strftime_number(char *s, size_t max, size_t pos, int value, int width) {
    char tmp[16];
    int len = 0;

    if (value < 0) {
        value = -value;
    }
    do {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && len < (int)sizeof(tmp));
    while (len < width && len < (int)sizeof(tmp)) {
        tmp[len++] = '0';
    }
    while (len-- > 0) {
        if (pos + 1u < max) {
            s[pos] = tmp[len];
        }
        ++pos;
    }
    return pos;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    size_t pos = 0;

    if (max == 0) {
        return 0;
    }
    while (*format != '\0') {
        if (*format != '%') {
            if (pos + 1u < max) {
                s[pos] = *format;
            }
            ++pos;
            ++format;
            continue;
        }
        ++format;
        if (*format == 'Y') {
            pos = libc_strftime_number(s, max, pos, tm->tm_year + 1900, 4);
        } else if (*format == 'm') {
            pos = libc_strftime_number(s, max, pos, tm->tm_mon + 1, 2);
        } else if (*format == 'd') {
            pos = libc_strftime_number(s, max, pos, tm->tm_mday, 2);
        } else if (*format == 'H') {
            pos = libc_strftime_number(s, max, pos, tm->tm_hour, 2);
        } else if (*format == 'M') {
            pos = libc_strftime_number(s, max, pos, tm->tm_min, 2);
        } else if (*format == 'S') {
            pos = libc_strftime_number(s, max, pos, tm->tm_sec, 2);
        } else if (*format == 'F') {
            pos = libc_strftime_number(s, max, pos, tm->tm_year + 1900, 4);
            pos = libc_strftime_append(s, max, pos, "-");
            pos = libc_strftime_number(s, max, pos, tm->tm_mon + 1, 2);
            pos = libc_strftime_append(s, max, pos, "-");
            pos = libc_strftime_number(s, max, pos, tm->tm_mday, 2);
        } else if (*format == 'T') {
            pos = libc_strftime_number(s, max, pos, tm->tm_hour, 2);
            pos = libc_strftime_append(s, max, pos, ":");
            pos = libc_strftime_number(s, max, pos, tm->tm_min, 2);
            pos = libc_strftime_append(s, max, pos, ":");
            pos = libc_strftime_number(s, max, pos, tm->tm_sec, 2);
        } else if (*format == 's') {
            pos = libc_strftime_number(s, max, pos, (int)mktime((struct tm *)tm), 1);
        } else if (*format == '%') {
            pos = libc_strftime_append(s, max, pos, "%");
        } else if (*format != '\0') {
            pos = libc_strftime_append(s, max, pos, "%");
            if (pos + 1u < max) {
                s[pos] = *format;
            }
            ++pos;
        }
        if (*format != '\0') {
            ++format;
        }
    }
    if (pos >= max) {
        s[max - 1u] = '\0';
        return 0;
    }
    s[pos] = '\0';
    return pos;
}

char *strptime(const char *s, const char *format, struct tm *tm) {
    char *endptr;
    long seconds;
    time_t parsed_time;

    if (s == 0 || format == 0 || tm == 0) {
        return 0;
    }
    if (strcmp(format, "%s") != 0) {
        return (char *)s;
    }
    seconds = strtol(s, &endptr, 10);
    if (endptr == s) {
        return 0;
    }
    parsed_time = (time_t)seconds;
    if (gmtime_r(&parsed_time, tm) == 0) {
        return 0;
    }
    return endptr;
}

void *bsearch(const void *key,
              const void *base,
              size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *)) {
    const uint8_t *items = (const uint8_t *)base;
    size_t low = 0;
    size_t high = nmemb;

    if (key == 0 || base == 0 || compar == 0 || size == 0) {
        return 0;
    }

    while (low < high) {
        size_t mid = low + ((high - low) / 2u);
        const void *item = items + mid * size;
        int cmp = compar(key, item);

        if (cmp == 0) {
            return (void *)item;
        }
        if (cmp < 0) {
            high = mid;
        } else {
            low = mid + 1u;
        }
    }

    return 0;
}

void *malloc(size_t size) {
    void *ptr;

    if (size == 0 || size > 0xffffffffu) {
        if (size > 0xffffffffu) {
            libc_errno = LIBC_ENOMEM;
        }
        return 0;
    }
    ptr = kmalloc((uint32_t)size);
    if (ptr == 0) {
        libc_errno = LIBC_ENOMEM;
    }
    return ptr;
}

void free(void *ptr) {
    kfree(ptr);
}

void *calloc(size_t count, size_t size) {
    size_t bytes;
    void *ptr;

    if (count != 0 && size > ((size_t)-1) / count) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    bytes = count * size;
    if (bytes > 0xffffffffu) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    ptr = malloc(bytes);
    if (ptr != 0) {
        memset(ptr, 0, bytes);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    uint32_t old_size;
    void *new_ptr;

    if (ptr == 0) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return 0;
    }
    if (size > 0xffffffffu) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }

    old_size = kmalloc_size(ptr);
    if (old_size == 0) {
        return 0;
    }
    if (size <= old_size) {
        return ptr;
    }

    new_ptr = malloc(size);
    if (new_ptr == 0) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

char *strdup(const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1u);

    if (copy == 0) {
        libc_errno = LIBC_ENOMEM;
        return 0;
    }
    memcpy(copy, s, len + 1u);
    return copy;
}

int *__errno_location(void) {
    return &libc_errno;
}

int *errno_location(void) {
    return &libc_errno;
}

void __assert_fail(const char *assertion,
                   const char *file,
                   unsigned int line,
                   const char *function) {
    (void)assertion;
    (void)file;
    (void)line;
    (void)function;
    console_panic("assert failed");
    for (;;) {
    }
}

br_prng_seeder br_prng_seeder_system(const char **name) {
    if (name) {
        *name = "none";
    }
    return 0;
}
