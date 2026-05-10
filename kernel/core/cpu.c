#include <stdint.h>
#include "kernel.h"

#define CPU_MAX_CORES 32u
#define ACPI_MADT_TYPE_LOCAL_APIC 0u
#define ACPI_MADT_TYPE_LOCAL_X2APIC 9u
#define ACPI_CPU_ENABLED 0x1u
#define ACPI_CPU_ONLINE_CAPABLE 0x2u
#define ACPI_MAX_TABLE_LENGTH (1024u * 1024u)

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

extern void cpu_load_gdt_and_segments(const struct gdtr64 *gdtr);
extern void cpu_load_idt(const struct idtr64 *idtr);

static uint64_t gdt64[] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00AF92000000FFFFULL,
};

static struct idt_entry64 idt64[256];
static unsigned int detected_core_count = 1u;
static unsigned int detected_lapic_ids[CPU_MAX_CORES];
static unsigned long long detected_lapic_base;

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
            if ((lapic->flags & ACPI_CPU_ENABLED) != 0u) {
                cpu_record_lapic_id(lapic->apic_id);
            }
        } else if (header->type == ACPI_MADT_TYPE_LOCAL_X2APIC &&
                   header->length >= sizeof(acpi_madt_local_x2apic_t)) {
            const acpi_madt_local_x2apic_t *x2apic = (const acpi_madt_local_x2apic_t *)(const void *)entry;
            if ((x2apic->flags & ACPI_CPU_ENABLED) != 0u) {
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

unsigned int cpu_lapic_id(unsigned int index) {
    if (index >= detected_core_count) {
        return 0;
    }
    return detected_lapic_ids[index];
}

unsigned long long cpu_lapic_base(void) {
    return detected_lapic_base;
}
