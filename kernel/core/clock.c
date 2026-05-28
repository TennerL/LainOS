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
