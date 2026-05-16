#include <stdint.h>
#include "kernel.h"

#define CMOS_INDEX 0x70u
#define CMOS_DATA 0x71u
#define CMOS_REG_SECONDS 0x00u
#define CMOS_REG_MINUTES 0x02u
#define CMOS_REG_HOURS 0x04u
#define CMOS_REG_DAY 0x07u
#define CMOS_REG_MONTH 0x08u
#define CMOS_REG_YEAR 0x09u
#define CMOS_REG_STATUS_A 0x0Au
#define CMOS_REG_STATUS_B 0x0Bu
#define CMOS_REG_CENTURY 0x32u
#define CMOS_UPDATE_IN_PROGRESS 0x80u
#define UNIX_EPOCH_DAYS 719528u

static rtc_time_t boot_rtc_time;
static int boot_rtc_valid;

static uint8_t clock_inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void clock_outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t cmos_read(uint8_t reg) {
    clock_outb(CMOS_INDEX, (uint8_t)(reg | 0x80u));
    return clock_inb(CMOS_DATA);
}

static int cmos_update_in_progress(void) {
    return (cmos_read(CMOS_REG_STATUS_A) & CMOS_UPDATE_IN_PROGRESS) != 0;
}

static uint32_t bcd_to_binary(uint32_t value) {
    return (value & 0x0Fu) + ((value >> 4) * 10u);
}

static int is_leap_year(uint32_t year) {
    return (year % 4u == 0u) && ((year % 100u) != 0u || (year % 400u) == 0u);
}

static uint32_t days_before_year(uint32_t year) {
    if (year == 0) {
        return 0;
    }
    return year * 365u + ((year - 1u) / 4u) - ((year - 1u) / 100u) + ((year - 1u) / 400u) + 1u;
}

static uint32_t days_before_month(uint32_t year, uint32_t month) {
    static const uint16_t month_days[12] = {
        0u, 31u, 59u, 90u, 120u, 151u, 181u, 212u, 243u, 273u, 304u, 334u
    };
    uint32_t days;

    if (month == 0 || month > 12u) {
        return 0;
    }
    days = month_days[month - 1u];
    if (month > 2u && is_leap_year(year)) {
        ++days;
    }
    return days;
}

uint32_t clock_days_since_year0(uint32_t year, uint32_t month, uint32_t day) {
    if (month < 1u || month > 12u || day < 1u || day > 31u) {
        return 0;
    }
    return days_before_year(year) + days_before_month(year, month) + day - 1u;
}

uint32_t clock_seconds_since_midnight(uint32_t hour, uint32_t minute, uint32_t second) {
    if (hour > 23u || minute > 59u || second > 60u) {
        return 0;
    }
    return hour * 3600u + minute * 60u + second;
}

uint64_t clock_unix_time_from_rtc(const rtc_time_t *time) {
    uint32_t days;
    uint32_t seconds;

    if (!time || !clock_rtc_time_valid(time)) {
        return 0;
    }
    days = clock_days_since_year0(time->year, time->month, time->day);
    if (days < UNIX_EPOCH_DAYS) {
        return 0;
    }
    seconds = clock_seconds_since_midnight(time->hour, time->minute, time->second);
    return ((uint64_t)(days - UNIX_EPOCH_DAYS) * 86400ull) + seconds;
}

int clock_rtc_time_valid(const rtc_time_t *time) {
    static const uint8_t month_lengths[12] = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
    };
    uint32_t max_day;

    if (!time || time->year < 1970u || time->year > 9999u ||
        time->month < 1u || time->month > 12u ||
        time->hour > 23u || time->minute > 59u || time->second > 60u) {
        return 0;
    }
    max_day = month_lengths[time->month - 1u];
    if (time->month == 2u && is_leap_year(time->year)) {
        ++max_day;
    }
    return time->day >= 1u && time->day <= max_day;
}

int clock_read_rtc(rtc_time_t *out) {
    rtc_time_t first;
    rtc_time_t second;
    uint8_t status_b;
    uint32_t century;

    if (!out) {
        return -1;
    }

    for (uint32_t i = 0; i < 100000u && cmos_update_in_progress(); ++i) {
        __asm__ __volatile__("pause");
    }

    first.second = cmos_read(CMOS_REG_SECONDS);
    first.minute = cmos_read(CMOS_REG_MINUTES);
    first.hour = cmos_read(CMOS_REG_HOURS);
    first.day = cmos_read(CMOS_REG_DAY);
    first.month = cmos_read(CMOS_REG_MONTH);
    first.year = cmos_read(CMOS_REG_YEAR);
    century = cmos_read(CMOS_REG_CENTURY);

    for (uint32_t i = 0; i < 100000u && cmos_update_in_progress(); ++i) {
        __asm__ __volatile__("pause");
    }

    second.second = cmos_read(CMOS_REG_SECONDS);
    second.minute = cmos_read(CMOS_REG_MINUTES);
    second.hour = cmos_read(CMOS_REG_HOURS);
    second.day = cmos_read(CMOS_REG_DAY);
    second.month = cmos_read(CMOS_REG_MONTH);
    second.year = cmos_read(CMOS_REG_YEAR);

    if (first.second != second.second || first.minute != second.minute ||
        first.hour != second.hour || first.day != second.day ||
        first.month != second.month || first.year != second.year) {
        first = second;
    }

    status_b = cmos_read(CMOS_REG_STATUS_B);
    if ((status_b & 0x04u) == 0) {
        first.second = bcd_to_binary(first.second);
        first.minute = bcd_to_binary(first.minute);
        first.hour = (first.hour & 0x80u) | bcd_to_binary(first.hour & 0x7Fu);
        first.day = bcd_to_binary(first.day);
        first.month = bcd_to_binary(first.month);
        first.year = bcd_to_binary(first.year);
        century = bcd_to_binary(century);
    }

    if ((status_b & 0x02u) == 0 && (first.hour & 0x80u) != 0) {
        first.hour = ((first.hour & 0x7Fu) + 12u) % 24u;
    }

    if (century >= 19u && century <= 99u) {
        first.year += century * 100u;
    } else if (first.year < 70u) {
        first.year += 2000u;
    } else if (first.year < 100u) {
        first.year += 1900u;
    }

    if (!clock_rtc_time_valid(&first)) {
        return -1;
    }

    *out = first;
    return 0;
}

void clock_init(void) {
    boot_rtc_valid = clock_read_rtc(&boot_rtc_time) == 0;
}

int clock_get_rtc_time(rtc_time_t *out) {
    if (clock_read_rtc(out) == 0) {
        return 0;
    }
    if (boot_rtc_valid && out) {
        *out = boot_rtc_time;
        return 0;
    }
    return -1;
}

uint64_t clock_unix_time(void) {
    rtc_time_t now;

    if (clock_get_rtc_time(&now) != 0) {
        return 0;
    }
    return clock_unix_time_from_rtc(&now);
}
