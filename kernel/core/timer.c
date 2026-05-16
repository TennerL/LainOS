#include <stdint.h>
#include "kernel.h"

#define PIC1_COMMAND 0x20u
#define PIC1_DATA 0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA 0xA1u
#define PIC_EOI 0x20u
#define PIT_COMMAND 0x43u
#define PIT_CHANNEL0 0x40u
#define PIT_BASE_HZ 1193182u
#define TIMER_HZ 1000u

static volatile uint64_t ticks;

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void pic_remap_and_mask(void) {
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();
    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    master_mask |= 0xFFu;
    master_mask &= (uint8_t)~0x01u;
    slave_mask |= 0xFFu;

    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

static void pit_set_frequency(uint32_t hz) {
    uint32_t divisor = PIT_BASE_HZ / hz;

    if (divisor == 0) {
        divisor = 1;
    }

    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFFu));
}

void timer_init(void) {
    ticks = 0;
    pic_remap_and_mask();
    pit_set_frequency(TIMER_HZ);
    __asm__ __volatile__("sti");
}

void timer_irq_handler(void) {
    ++ticks;
    status_cpu_timer_tick();
    outb(PIC1_COMMAND, PIC_EOI);
}

unsigned long long timer_ticks(void) {
    return ticks;
}

unsigned int timer_frequency(void) {
    return TIMER_HZ;
}
