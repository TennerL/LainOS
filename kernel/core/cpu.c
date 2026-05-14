#include <stdint.h>
#include "kernel.h"

#define CPU_MAX_CORES 32u
#define CPU_AP_STACK_SIZE (6u * 1024u)
#define CPU_AP_TRAMPOLINE_BASE 0x8000ull
#define CPU_AP_TRAMPOLINE_VECTOR 0x08u
#define CPU_SMP_IPI_VECTOR 0xF1u
#define CPU_SMP_WORK_SLOTS 64u
#define CPU_SMP_WORK_FREE 0u
#define CPU_SMP_WORK_QUEUED 1u
#define CPU_SMP_WORK_RUNNING 2u
#define CPU_SMP_WORK_DONE 3u
#define ACPI_MADT_TYPE_LOCAL_APIC 0u
#define ACPI_MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE 5u
#define ACPI_MADT_TYPE_LOCAL_X2APIC 9u
#define ACPI_CPU_ENABLED 0x1u
#define ACPI_CPU_ONLINE_CAPABLE 0x2u
#define ACPI_MAX_TABLE_LENGTH (1024u * 1024u)
#define IA32_APIC_BASE_MSR 0x1Bu
#define IA32_APIC_BASE_ENABLE 0x800u
#define LAPIC_REG_ID 0x20u
#define LAPIC_REG_EOI 0xB0u
#define LAPIC_REG_SVR 0xF0u
#define LAPIC_REG_ICR_LOW 0x300u
#define LAPIC_REG_ICR_HIGH 0x310u
#define LAPIC_SVR_ENABLE 0x100u
#define LAPIC_ICR_DEST_ALL_BUT_SELF 0x000C0000u
#define LAPIC_ICR_DELIVERY_STATUS 0x1000u
#define LAPIC_ICR_FIXED 0x000u
#define LAPIC_ICR_INIT 0x500u
#define LAPIC_ICR_STARTUP 0x600u
#define LAPIC_ICR_LEVEL_ASSERT 0x4000u
#define LAPIC_ICR_TRIGGER_LEVEL 0x8000u

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

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} acpi_rsdp_v1_t;

typedef struct __attribute__((packed)) {
    acpi_rsdp_v1_t v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_v2_t;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
} acpi_madt_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} acpi_madt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint8_t acpi_processor_uid;
    uint8_t apic_id;
    uint32_t flags;
} acpi_madt_local_apic_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint16_t reserved;
    uint64_t local_apic_address;
} acpi_madt_lapic_address_override_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_processor_uid;
} acpi_madt_local_x2apic_t;

extern void isr_0(void);  extern void isr_1(void);  extern void isr_2(void);  extern void isr_3(void);
extern void isr_4(void);  extern void isr_5(void);  extern void isr_6(void);  extern void isr_7(void);
extern void isr_8(void);  extern void isr_9(void);  extern void isr_10(void); extern void isr_11(void);
extern void isr_12(void); extern void isr_13(void); extern void isr_14(void); extern void isr_15(void);
extern void isr_16(void); extern void isr_17(void); extern void isr_18(void); extern void isr_19(void);
extern void isr_20(void); extern void isr_21(void); extern void isr_22(void); extern void isr_23(void);
extern void isr_24(void); extern void isr_25(void); extern void isr_26(void); extern void isr_27(void);
extern void isr_28(void); extern void isr_29(void); extern void isr_30(void); extern void isr_31(void);
extern void irq0_timer(void);
extern void irq12_mouse(void);
extern void irq_smp_ipi(void);

extern void cpu_load_gdt_and_segments(const struct gdtr64 *gdtr);
extern void cpu_load_idt(const struct idtr64 *idtr);
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];
extern uint8_t ap_trampoline_cr3[];
extern uint8_t ap_trampoline_stack[];
extern uint8_t ap_trampoline_entry[];

static uint64_t gdt64[] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00AF92000000FFFFULL,
};

static struct idt_entry64 idt64[256];
static uint8_t ap_stacks[CPU_MAX_CORES][CPU_AP_STACK_SIZE] __attribute__((aligned(16)));
static unsigned int detected_core_count = 1u;
static unsigned int detected_lapic_ids[CPU_MAX_CORES];
static unsigned long long detected_lapic_base;
static unsigned int bsp_lapic_id;
static volatile unsigned int online_core_count = 1u;
static volatile unsigned int smp_work_lock;
static volatile unsigned int smp_next_work_id = 1u;

typedef struct {
    volatile unsigned int online;
    unsigned int lapic_id;
    volatile uint64_t busy_ticks;
    volatile uint64_t local_timer_ticks;
} cpu_core_state_t;

typedef struct {
    volatile unsigned int state;
    unsigned int id;
    smp_work_fn_t fn;
    void *arg;
} smp_work_slot_t;

static cpu_core_state_t cpu_cores[CPU_MAX_CORES];
static smp_work_slot_t smp_work_slots[CPU_SMP_WORK_SLOTS];

static void cpu_relax(void) {
    __asm__ __volatile__("pause");
}

static uint64_t cpu_read_msr(uint32_t msr) {
    uint32_t low;
    uint32_t high;

    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void cpu_write_msr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         :
                         : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                         : "memory");
}

static uint64_t cpu_read_cr3(void) {
    uint64_t value;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));
    return value;
}

static volatile uint32_t *lapic_reg(uint32_t reg) {
    return (volatile uint32_t *)(uintptr_t)(detected_lapic_base + reg);
}

static uint32_t lapic_read(uint32_t reg) {
    return *lapic_reg(reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    *lapic_reg(reg) = value;
    (void)lapic_read(LAPIC_REG_ID);
}

static void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0u);
}

static void lapic_wait_delivery(void) {
    for (unsigned int i = 0; i < 1000000u; ++i) {
        if ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS) == 0u) {
            return;
        }
        cpu_relax();
    }
}

static void cpu_delay(unsigned int iterations) {
    for (unsigned int i = 0; i < iterations; ++i) {
        cpu_relax();
    }
}

static void lapic_enable(void) {
    uint64_t apic_base = cpu_read_msr(IA32_APIC_BASE_MSR);

    if (detected_lapic_base == 0) {
        detected_lapic_base = apic_base & 0xFFFFF000ull;
    }
    cpu_write_msr(IA32_APIC_BASE_MSR, apic_base | IA32_APIC_BASE_ENABLE);
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | LAPIC_SVR_ENABLE | 0xFFu);
}

static unsigned int lapic_current_id(void) {
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu;
}

static unsigned int cpu_index_for_lapic_id(unsigned int apic_id) {
    for (unsigned int i = 0; i < detected_core_count; ++i) {
        if (detected_lapic_ids[i] == apic_id) {
            return i;
        }
    }

    return 0u;
}

static int cpu_lapic_id_known(unsigned int apic_id) {
    for (unsigned int i = 0; i < detected_core_count; ++i) {
        if (detected_lapic_ids[i] == apic_id) {
            return 1;
        }
    }

    return 0;
}

static void cpu_reset_core_states(void) {
    for (unsigned int i = 0; i < CPU_MAX_CORES; ++i) {
        cpu_cores[i].online = 0;
        cpu_cores[i].lapic_id = i < detected_core_count ? detected_lapic_ids[i] : 0xffffffffu;
        cpu_cores[i].busy_ticks = 0;
        cpu_cores[i].local_timer_ticks = 0;
    }
}

static void cpu_mark_online(unsigned int apic_id) {
    unsigned int index = cpu_index_for_lapic_id(apic_id);

    if (index >= CPU_MAX_CORES) {
        return;
    }
    cpu_cores[index].lapic_id = apic_id;
    if (__sync_bool_compare_and_swap(&cpu_cores[index].online, 0u, 1u)) {
        __sync_fetch_and_add(&online_core_count, 1u);
    }
}

static void cpu_copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint64_t i = 0; i < size; ++i) {
        d[i] = s[i];
    }
}

static void cpu_patch_u64(uint8_t *image, uint64_t offset, uint64_t value) {
    for (unsigned int i = 0; i < 8u; ++i) {
        image[offset + i] = (uint8_t)(value >> (i * 8u));
    }
}

static void cpu_send_init_sipi(unsigned int apic_id) {
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
    lapic_wait_delivery();
    cpu_delay(1000000u);

    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_TRIGGER_LEVEL);
    lapic_wait_delivery();
    cpu_delay(1000000u);

    for (unsigned int attempt = 0; attempt < 2u; ++attempt) {
        lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
        lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_STARTUP | CPU_AP_TRAMPOLINE_VECTOR);
        lapic_wait_delivery();
        cpu_delay(200000u);
    }
}

static void cpu_send_smp_ipi(void) {
    if (online_core_count <= 1u) {
        return;
    }

    lapic_write(LAPIC_REG_ICR_HIGH, 0u);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DEST_ALL_BUT_SELF | LAPIC_ICR_FIXED | CPU_SMP_IPI_VECTOR);
    lapic_wait_delivery();
}

static void smp_lock(void) {
    while (__sync_lock_test_and_set(&smp_work_lock, 1u) != 0u) {
        cpu_relax();
    }
}

static void smp_unlock(void) {
    __sync_lock_release(&smp_work_lock);
}

static int smp_take_work(smp_work_fn_t *out_fn, void **out_arg, unsigned int *out_slot) {
    int found = 0;

    smp_lock();
    for (unsigned int i = 0; i < CPU_SMP_WORK_SLOTS; ++i) {
        if (smp_work_slots[i].state == CPU_SMP_WORK_QUEUED && smp_work_slots[i].fn != 0) {
            smp_work_slots[i].state = CPU_SMP_WORK_RUNNING;
            *out_fn = smp_work_slots[i].fn;
            *out_arg = smp_work_slots[i].arg;
            *out_slot = i;
            found = 1;
            break;
        }
    }
    smp_unlock();

    return found;
}

static void cpu_record_busy_ticks(unsigned int core, uint64_t ticks) {
    if (core >= CPU_MAX_CORES || ticks == 0) {
        return;
    }

    __sync_fetch_and_add(&cpu_cores[core].busy_ticks, ticks);
}

static int smp_run_one_work_item(unsigned int core_index) {
    smp_work_fn_t fn = 0;
    void *arg = 0;
    unsigned int slot = 0;
    uint64_t start_ticks;
    uint64_t end_ticks;
    uint64_t elapsed_ticks;

    if (!smp_take_work(&fn, &arg, &slot)) {
        return 0;
    }

    start_ticks = timer_ticks();
    fn(arg);
    end_ticks = timer_ticks();
    elapsed_ticks = end_ticks - start_ticks;
    if (elapsed_ticks == 0u) {
        elapsed_ticks = 1u;
    }
    cpu_record_busy_ticks(core_index, elapsed_ticks);
    __sync_synchronize();
    smp_work_slots[slot].state = CPU_SMP_WORK_DONE;
    return 1;
}

void cpu_ipi_handler(void) {
    lapic_eoi();
}

void cpu_ap_entry(void) {
    unsigned int core_index;
    unsigned int apic_id;

    cpu_init_tables();
    lapic_enable();
    apic_id = lapic_current_id();
    core_index = cpu_index_for_lapic_id(apic_id);
    if (cpu_lapic_id_known(apic_id)) {
        cpu_mark_online(apic_id);
    }

    for (;;) {
        while (smp_run_one_work_item(core_index)) {
        }
        __asm__ __volatile__("sti; hlt; cli");
    }
}

static void cpu_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(leaf), "c"(subleaf));

    if (a != 0) {
        *a = eax;
    }
    if (b != 0) {
        *b = ebx;
    }
    if (c != 0) {
        *c = ecx;
    }
    if (d != 0) {
        *d = edx;
    }
}

static unsigned int cpu_cpuid_logical_count(void) {
    uint32_t max_leaf;
    uint32_t ebx;
    uint32_t edx;
    unsigned int count;

    cpu_cpuid(0u, 0u, &max_leaf, 0, 0, 0);
    if (max_leaf < 1u) {
        return 1u;
    }

    cpu_cpuid(1u, 0u, 0, &ebx, 0, &edx);
    if ((edx & (1u << 9)) == 0u) {
        return 1u;
    }

    count = (ebx >> 16) & 0xFFu;
    if (count == 0u) {
        count = 1u;
    }
    if (count > CPU_MAX_CORES) {
        count = CPU_MAX_CORES;
    }
    return count;
}

static void cpu_use_cpuid_fallback_topology(void) {
    unsigned int count = cpu_cpuid_logical_count();

    detected_core_count = count;
    for (unsigned int i = 0; i < CPU_MAX_CORES; ++i) {
        detected_lapic_ids[i] = i < count ? i : 0u;
    }
}

static int acpi_mem_equals(const char *a, const char *b, unsigned int len) {
    for (unsigned int i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t acpi_checksum(const void *ptr, uint32_t len) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum;
}

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} efi_memory_descriptor_t;

static int acpi_range_in_memory_map(const boot_info_t *info, uint64_t address, uint64_t length) {
    uint64_t end;

    if (address == 0 || length == 0 || info == 0 ||
        info->memory_map == 0 ||
        info->memory_map_size == 0 ||
        info->memory_map_descriptor_size < sizeof(efi_memory_descriptor_t)) {
        return 0;
    }

    end = address + length;
    if (end < address) {
        return 0;
    }

    for (uint64_t offset = 0;
         offset + sizeof(efi_memory_descriptor_t) <= info->memory_map_size;
         offset += info->memory_map_descriptor_size) {
        const efi_memory_descriptor_t *desc =
            (const efi_memory_descriptor_t *)(const void *)((const uint8_t *)(uintptr_t)info->memory_map + offset);
        uint64_t start = desc->physical_start;
        uint64_t desc_end = start + desc->number_of_pages * 4096ull;

        if (desc_end < start) {
            continue;
        }
        if (address >= start && end <= desc_end) {
            return 1;
        }
    }

    return 0;
}

static int acpi_pointer_valid(const boot_info_t *info, const void *ptr, uint64_t length) {
    return acpi_range_in_memory_map(info, (uint64_t)(uintptr_t)ptr, length);
}

static int acpi_sdt_valid(const boot_info_t *info, const acpi_sdt_header_t *header, const char *signature) {
    if (header == 0 ||
        !acpi_pointer_valid(info, header, sizeof(acpi_sdt_header_t)) ||
        header->length < sizeof(acpi_sdt_header_t)) {
        return 0;
    }
    if (header->length > ACPI_MAX_TABLE_LENGTH ||
        !acpi_pointer_valid(info, header, header->length)) {
        return 0;
    }
    if (!acpi_mem_equals(header->signature, signature, 4u)) {
        return 0;
    }
    return acpi_checksum(header, header->length) == 0;
}

static const acpi_sdt_header_t *acpi_find_table_in_xsdt(const boot_info_t *info,
                                                        const acpi_sdt_header_t *xsdt,
                                                        const char *signature) {
    uint32_t entries;
    const uint64_t *table;

    if (!acpi_sdt_valid(info, xsdt, "XSDT")) {
        return 0;
    }

    entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
    table = (const uint64_t *)(const void *)((const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < entries; ++i) {
        const acpi_sdt_header_t *header = (const acpi_sdt_header_t *)(uintptr_t)table[i];
        if (acpi_pointer_valid(info, header, sizeof(acpi_sdt_header_t)) &&
            acpi_sdt_valid(info, header, signature)) {
            return header;
        }
    }

    return 0;
}

static const acpi_sdt_header_t *acpi_find_table_in_rsdt(const boot_info_t *info,
                                                        const acpi_sdt_header_t *rsdt,
                                                        const char *signature) {
    uint32_t entries;
    const uint32_t *table;

    if (!acpi_sdt_valid(info, rsdt, "RSDT")) {
        return 0;
    }

    entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
    table = (const uint32_t *)(const void *)((const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < entries; ++i) {
        const acpi_sdt_header_t *header = (const acpi_sdt_header_t *)(uintptr_t)table[i];
        if (acpi_pointer_valid(info, header, sizeof(acpi_sdt_header_t)) &&
            acpi_sdt_valid(info, header, signature)) {
            return header;
        }
    }

    return 0;
}

static const acpi_sdt_header_t *acpi_find_table(const boot_info_t *info, const char *signature) {
    const acpi_rsdp_v2_t *rsdp;

    if (info == 0 || info->rsdp == 0) {
        return 0;
    }

    rsdp = (const acpi_rsdp_v2_t *)(uintptr_t)info->rsdp;
    if (!acpi_pointer_valid(info, rsdp, sizeof(acpi_rsdp_v1_t))) {
        return 0;
    }
    if (!acpi_mem_equals(rsdp->v1.signature, "RSD PTR ", 8u) ||
        acpi_checksum(rsdp, sizeof(acpi_rsdp_v1_t)) != 0) {
        return 0;
    }

    if (rsdp->v1.revision >= 2u &&
        rsdp->length >= sizeof(acpi_rsdp_v2_t) &&
        rsdp->length <= 4096u &&
        acpi_pointer_valid(info, rsdp, rsdp->length) &&
        acpi_checksum(rsdp, rsdp->length) == 0 &&
        rsdp->xsdt_address != 0) {
        const acpi_sdt_header_t *found =
            acpi_find_table_in_xsdt(info,
                                    (const acpi_sdt_header_t *)(uintptr_t)rsdp->xsdt_address,
                                    signature);
        if (found != 0) {
            return found;
        }
    }

    if (rsdp->v1.rsdt_address != 0) {
        return acpi_find_table_in_rsdt(info,
                                       (const acpi_sdt_header_t *)(uintptr_t)rsdp->v1.rsdt_address,
                                       signature);
    }

    return 0;
}

static void cpu_record_lapic_id(uint32_t id) {
    if (detected_core_count >= CPU_MAX_CORES) {
        return;
    }

    for (unsigned int i = 0; i < detected_core_count; ++i) {
        if (detected_lapic_ids[i] == id) {
            return;
        }
    }

    detected_lapic_ids[detected_core_count++] = id;
}

static int cpu_acpi_cpu_usable(uint32_t flags) {
    return (flags & ACPI_CPU_ENABLED) != 0u;
}

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
    idt_set_gate(44, irq12_mouse);
    idt_set_gate(CPU_SMP_IPI_VECTOR, irq_smp_ipi);

    struct idtr64 idtr = {
        .limit = (uint16_t)(sizeof(idt64) - 1),
        .base = (uint64_t)idt64,
    };

    cpu_load_gdt_and_segments(&gdtr);
    cpu_load_idt(&idtr);
}

void cpu_detect_topology(const boot_info_t *info) {
    const acpi_madt_t *madt = (const acpi_madt_t *)acpi_find_table(info, "APIC");
    const uint8_t *entry;
    const uint8_t *end;

    detected_core_count = 1u;
    detected_lapic_ids[0] = 0u;
    detected_lapic_base = 0xFEE00000ull;

    if (madt == 0 || madt->header.length < sizeof(acpi_madt_t)) {
        cpu_use_cpuid_fallback_topology();
        return;
    }

    detected_core_count = 0u;
    detected_lapic_base = madt->local_apic_address;
    entry = (const uint8_t *)madt + sizeof(acpi_madt_t);
    end = (const uint8_t *)madt + madt->header.length;

    while (entry + sizeof(acpi_madt_entry_t) <= end) {
        const acpi_madt_entry_t *header = (const acpi_madt_entry_t *)(const void *)entry;

        if (header->length < sizeof(acpi_madt_entry_t) || entry + header->length > end) {
            break;
        }

        if (header->type == ACPI_MADT_TYPE_LOCAL_APIC &&
            header->length >= sizeof(acpi_madt_local_apic_t)) {
            const acpi_madt_local_apic_t *lapic = (const acpi_madt_local_apic_t *)(const void *)entry;
            if (cpu_acpi_cpu_usable(lapic->flags)) {
                cpu_record_lapic_id(lapic->apic_id);
            }
        } else if (header->type == ACPI_MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE &&
                   header->length >= sizeof(acpi_madt_lapic_address_override_t)) {
            const acpi_madt_lapic_address_override_t *override =
                (const acpi_madt_lapic_address_override_t *)(const void *)entry;
            detected_lapic_base = override->local_apic_address;
        } else if (header->type == ACPI_MADT_TYPE_LOCAL_X2APIC &&
                   header->length >= sizeof(acpi_madt_local_x2apic_t)) {
            const acpi_madt_local_x2apic_t *x2apic = (const acpi_madt_local_x2apic_t *)(const void *)entry;
            if (cpu_acpi_cpu_usable(x2apic->flags)) {
                cpu_record_lapic_id(x2apic->x2apic_id);
            }
        }

        entry += header->length;
    }

    if (detected_core_count == 0u) {
        cpu_use_cpuid_fallback_topology();
    }
}

unsigned int cpu_core_count(void) {
    return detected_core_count;
}

unsigned int cpu_online_core_count(void) {
    return online_core_count;
}

unsigned long long cpu_core_busy_ticks(unsigned int core) {
    if (core >= CPU_MAX_CORES) {
        return 0;
    }

    return cpu_cores[core].busy_ticks;
}

unsigned int cpu_lapic_id(unsigned int index) {
    if (index >= detected_core_count) {
        return 0;
    }
    return detected_lapic_ids[index];
}

unsigned long long cpu_lapic_base(void) {
    return detected_lapic_base;
}

unsigned int cpu_start_secondary_cores(void) {
    uint8_t *trampoline = (uint8_t *)(uintptr_t)CPU_AP_TRAMPOLINE_BASE;
    uint64_t trampoline_size = (uint64_t)(uintptr_t)(ap_trampoline_end - ap_trampoline_start);
    uint64_t cr3_offset = (uint64_t)(uintptr_t)(ap_trampoline_cr3 - ap_trampoline_start);
    uint64_t stack_offset = (uint64_t)(uintptr_t)(ap_trampoline_stack - ap_trampoline_start);
    uint64_t entry_offset = (uint64_t)(uintptr_t)(ap_trampoline_entry - ap_trampoline_start);
    uint64_t cr3 = cpu_read_cr3();
    unsigned int started_before;

    online_core_count = 1u;
    cpu_reset_core_states();
    if (detected_core_count <= 1u || trampoline_size == 0 || trampoline_size > 4096u) {
        cpu_cores[0].online = 1u;
        return online_core_count;
    }

    lapic_enable();
    bsp_lapic_id = lapic_current_id();
    cpu_cores[cpu_index_for_lapic_id(bsp_lapic_id)].online = 1u;
    cpu_cores[cpu_index_for_lapic_id(bsp_lapic_id)].lapic_id = bsp_lapic_id;
    cpu_copy_bytes(trampoline, ap_trampoline_start, trampoline_size);
    cpu_patch_u64(trampoline, cr3_offset, cr3);
    cpu_patch_u64(trampoline, entry_offset, (uint64_t)(uintptr_t)cpu_ap_entry);

    for (unsigned int i = 0; i < detected_core_count; ++i) {
        unsigned int apic_id = detected_lapic_ids[i];

        if (apic_id == bsp_lapic_id || apic_id > 0xFFu) {
            continue;
        }

        started_before = online_core_count;
        cpu_patch_u64(trampoline,
                      stack_offset,
                      (uint64_t)(uintptr_t)&ap_stacks[i][CPU_AP_STACK_SIZE]);
        cpu_send_init_sipi(apic_id);

        for (unsigned int wait = 0; wait < 10000000u; ++wait) {
            if (online_core_count != started_before) {
                break;
            }
            cpu_relax();
        }
    }

    return online_core_count;
}

unsigned int smp_submit_work(smp_work_fn_t fn, void *arg) {
    unsigned int id = 0;

    if (fn == 0 || online_core_count <= 1u) {
        return 0u;
    }

    smp_lock();
    for (unsigned int i = 0; i < CPU_SMP_WORK_SLOTS; ++i) {
        if (smp_work_slots[i].state == CPU_SMP_WORK_FREE) {
            id = smp_next_work_id++;
            if (id == 0u) {
                id = smp_next_work_id++;
            }
            smp_work_slots[i].fn = fn;
            smp_work_slots[i].arg = arg;
            smp_work_slots[i].id = id;
            __sync_synchronize();
            smp_work_slots[i].state = CPU_SMP_WORK_QUEUED;
            break;
        }
    }
    smp_unlock();

    if (id != 0u) {
        cpu_send_smp_ipi();
    }
    return id;
}

int smp_work_done(unsigned int id) {
    int done = 0;

    if (id == 0u) {
        return 1;
    }

    smp_lock();
    for (unsigned int i = 0; i < CPU_SMP_WORK_SLOTS; ++i) {
        if (smp_work_slots[i].id == id) {
            done = smp_work_slots[i].state == CPU_SMP_WORK_DONE;
            break;
        }
    }
    smp_unlock();

    return done;
}

static void smp_release_work(unsigned int id) {
    if (id == 0u) {
        return;
    }

    smp_lock();
    for (unsigned int i = 0; i < CPU_SMP_WORK_SLOTS; ++i) {
        if (smp_work_slots[i].id == id && smp_work_slots[i].state == CPU_SMP_WORK_DONE) {
            smp_work_slots[i].fn = 0;
            smp_work_slots[i].arg = 0;
            smp_work_slots[i].id = 0;
            __sync_synchronize();
            smp_work_slots[i].state = CPU_SMP_WORK_FREE;
            break;
        }
    }
    smp_unlock();
}

void smp_wait_work(unsigned int id) {
    if (id == 0u) {
        return;
    }

    while (!smp_work_done(id)) {
        smp_run_one_work_item(0u);
        cpu_send_smp_ipi();
        cpu_relax();
    }
    smp_release_work(id);
}

unsigned int smp_pending_work_count(void) {
    unsigned int count = 0;

    smp_lock();
    for (unsigned int i = 0; i < CPU_SMP_WORK_SLOTS; ++i) {
        if (smp_work_slots[i].state == CPU_SMP_WORK_QUEUED ||
            smp_work_slots[i].state == CPU_SMP_WORK_RUNNING) {
            ++count;
        }
    }
    smp_unlock();

    return count;
}
