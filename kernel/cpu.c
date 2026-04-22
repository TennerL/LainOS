#include <stdint.h>
#include "kernel.h"

struct __attribute__((packed)) gdtr64 {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) idtr64 {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) idt_entry64 {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

extern void isr_0(void);  extern void isr_1(void);  extern void isr_2(void);  extern void isr_3(void);
extern void isr_4(void);  extern void isr_5(void);  extern void isr_6(void);  extern void isr_7(void);
extern void isr_8(void);  extern void isr_9(void);  extern void isr_10(void); extern void isr_11(void);
extern void isr_12(void); extern void isr_13(void); extern void isr_14(void); extern void isr_15(void);
extern void isr_16(void); extern void isr_17(void); extern void isr_18(void); extern void isr_19(void);
extern void isr_20(void); extern void isr_21(void); extern void isr_22(void); extern void isr_23(void);
extern void isr_24(void); extern void isr_25(void); extern void isr_26(void); extern void isr_27(void);
extern void isr_28(void); extern void isr_29(void); extern void isr_30(void); extern void isr_31(void);
extern void irq0_timer(void);

extern void cpu_load_gdt_and_segments(const struct gdtr64 *gdtr);
extern void cpu_load_idt(const struct idtr64 *idtr);

static uint64_t gdt64[] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00AF92000000FFFFULL,
};

static struct idt_entry64 idt64[256];

static void idt_set_gate(int vec, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    idt64[vec].offset_low = (uint16_t)(addr & 0xFFFF);
    idt64[vec].selector = 0x08;
    idt64[vec].ist = 0;
    idt64[vec].type_attr = 0x8E;
    idt64[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt64[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    idt64[vec].zero = 0;
}

void cpu_init_tables(void) {
    static const struct gdtr64 gdtr = {
        .limit = (uint16_t)(sizeof(gdt64) - 1),
        .base = (uint64_t)gdt64,
    };

    static void (*const isrs[32])(void) = {
        isr_0, isr_1, isr_2, isr_3, isr_4, isr_5, isr_6, isr_7,
        isr_8, isr_9, isr_10, isr_11, isr_12, isr_13, isr_14, isr_15,
        isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23,
        isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31,
    };

    for (int i = 0; i < 32; ++i) {
        idt_set_gate(i, isrs[i]);
    }
    idt_set_gate(32, irq0_timer);

    struct idtr64 idtr = {
        .limit = (uint16_t)(sizeof(idt64) - 1),
        .base = (uint64_t)idt64,
    };

    cpu_load_gdt_and_segments(&gdtr);
    cpu_load_idt(&idtr);
}
