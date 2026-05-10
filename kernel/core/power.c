#include <stdint.h>
#include "kernel.h"

#define ACPI_MAX_TABLE_LENGTH (1024u * 1024u)
#define ACPI_PM1_SCI_EN 0x0001u
#define ACPI_PM1_SLP_EN 0x2000u
#define ACPI_PM1_SLP_TYP_SHIFT 10u
#define ACPI_AML_NAME_OP 0x08u
#define ACPI_AML_PACKAGE_OP 0x12u
#define ACPI_AML_ZERO_OP 0x00u
#define ACPI_AML_ONE_OP 0x01u
#define ACPI_AML_BYTE_PREFIX 0x0Au
#define ACPI_AML_WORD_PREFIX 0x0Bu
#define ACPI_AML_DWORD_PREFIX 0x0Cu
#define ACPI_AML_QWORD_PREFIX 0x0Eu

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
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    uint8_t reset_reg[12];
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
} acpi_fadt_t;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static int mem_equals(const char *a, const char *b, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t checksum(const void *ptr, uint32_t len) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum;
}

static int sdt_valid(const acpi_sdt_header_t *header, const char *signature) {
    if (header == 0 ||
        header->length < sizeof(acpi_sdt_header_t) ||
        header->length > ACPI_MAX_TABLE_LENGTH ||
        !mem_equals(header->signature, signature, 4u)) {
        return 0;
    }
    return checksum(header, header->length) == 0;
}

static const acpi_sdt_header_t *find_table_xsdt(const acpi_sdt_header_t *xsdt, const char *signature) {
    const uint64_t *entries;
    uint32_t count;

    if (!sdt_valid(xsdt, "XSDT")) {
        return 0;
    }
    count = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
    entries = (const uint64_t *)(const void *)((const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < count; ++i) {
        const acpi_sdt_header_t *table = (const acpi_sdt_header_t *)(uintptr_t)entries[i];
        if (sdt_valid(table, signature)) {
            return table;
        }
    }
    return 0;
}

static const acpi_sdt_header_t *find_table_rsdt(const acpi_sdt_header_t *rsdt, const char *signature) {
    const uint32_t *entries;
    uint32_t count;

    if (!sdt_valid(rsdt, "RSDT")) {
        return 0;
    }
    count = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
    entries = (const uint32_t *)(const void *)((const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < count; ++i) {
        const acpi_sdt_header_t *table = (const acpi_sdt_header_t *)(uintptr_t)entries[i];
        if (sdt_valid(table, signature)) {
            return table;
        }
    }
    return 0;
}

static const acpi_sdt_header_t *find_acpi_table(const boot_info_t *info, const char *signature) {
    const acpi_rsdp_v2_t *rsdp;

    if (info == 0 || info->rsdp == 0) {
        return 0;
    }
    rsdp = (const acpi_rsdp_v2_t *)(uintptr_t)info->rsdp;
    if (!mem_equals(rsdp->v1.signature, "RSD PTR ", 8u) ||
        checksum(rsdp, sizeof(acpi_rsdp_v1_t)) != 0) {
        return 0;
    }
    if (rsdp->v1.revision >= 2u &&
        rsdp->length >= sizeof(acpi_rsdp_v2_t) &&
        rsdp->length <= 4096u &&
        checksum(rsdp, rsdp->length) == 0 &&
        rsdp->xsdt_address != 0) {
        const acpi_sdt_header_t *found =
            find_table_xsdt((const acpi_sdt_header_t *)(uintptr_t)rsdp->xsdt_address, signature);
        if (found != 0) {
            return found;
        }
    }
    if (rsdp->v1.rsdt_address != 0) {
        return find_table_rsdt((const acpi_sdt_header_t *)(uintptr_t)rsdp->v1.rsdt_address, signature);
    }
    return 0;
}

static const acpi_sdt_header_t *fadt_dsdt(const acpi_fadt_t *fadt) {
    if (fadt->header.length >= sizeof(acpi_fadt_t) && fadt->x_dsdt != 0) {
        return (const acpi_sdt_header_t *)(uintptr_t)fadt->x_dsdt;
    }
    if (fadt->dsdt != 0) {
        return (const acpi_sdt_header_t *)(uintptr_t)fadt->dsdt;
    }
    return 0;
}

static uint32_t aml_pkg_length_size(uint8_t lead) {
    return 1u + (uint32_t)(lead >> 6);
}

static int aml_read_integer(const uint8_t **cursor, const uint8_t *end, uint8_t *out) {
    const uint8_t *p = *cursor;

    if (p >= end) {
        return 0;
    }
    if (*p == ACPI_AML_ZERO_OP) {
        *out = 0;
        *cursor = p + 1;
        return 1;
    }
    if (*p == ACPI_AML_ONE_OP) {
        *out = 1;
        *cursor = p + 1;
        return 1;
    }
    if (*p == ACPI_AML_BYTE_PREFIX && p + 1 < end) {
        *out = p[1];
        *cursor = p + 2;
        return 1;
    }
    if (*p == ACPI_AML_WORD_PREFIX && p + 2 < end) {
        *out = p[1];
        *cursor = p + 3;
        return 1;
    }
    if (*p == ACPI_AML_DWORD_PREFIX && p + 4 < end) {
        *out = p[1];
        *cursor = p + 5;
        return 1;
    }
    if (*p == ACPI_AML_QWORD_PREFIX && p + 8 < end) {
        *out = p[1];
        *cursor = p + 9;
        return 1;
    }
    return 0;
}

static int find_s5_types(const acpi_sdt_header_t *dsdt, uint8_t *slp_typa, uint8_t *slp_typb) {
    const uint8_t *bytes;
    const uint8_t *end;

    if (!sdt_valid(dsdt, "DSDT")) {
        return 0;
    }
    bytes = (const uint8_t *)dsdt;
    end = bytes + dsdt->length;
    for (const uint8_t *p = bytes + sizeof(acpi_sdt_header_t); p + 6 < end; ++p) {
        const uint8_t *cursor;
        uint8_t a;
        uint8_t b;

        if (p[0] == ACPI_AML_NAME_OP) {
            ++p;
        }
        if (p + 5 >= end ||
            p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_' ||
            p[4] != ACPI_AML_PACKAGE_OP) {
            continue;
        }

        cursor = p + 5;
        cursor += aml_pkg_length_size(*cursor);
        if (cursor >= end) {
            continue;
        }
        ++cursor;
        if (aml_read_integer(&cursor, end, &a) &&
            aml_read_integer(&cursor, end, &b)) {
            *slp_typa = a;
            *slp_typb = b;
            return 1;
        }
    }
    return 0;
}

void power_reboot(void) {
    __asm__ __volatile__("cli");

    outb(0xCF9u, 0x02u);
    outb(0xCF9u, 0x06u);

    for (uint32_t i = 0; i < 1000000u; ++i) {
        if ((inb(0x64u) & 0x02u) == 0) {
            break;
        }
    }
    outb(0x64u, 0xFEu);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

int power_poweroff(const boot_info_t *info) {
    const acpi_fadt_t *fadt = (const acpi_fadt_t *)find_acpi_table(info, "FACP");
    const acpi_sdt_header_t *dsdt;
    uint8_t slp_typa = 0;
    uint8_t slp_typb = 0;
    uint16_t pm1a;
    uint16_t pm1b;

    if (fadt == 0 || fadt->header.length < 72u || fadt->pm1a_cnt_blk == 0) {
        return -1;
    }
    dsdt = fadt_dsdt(fadt);
    if (!find_s5_types(dsdt, &slp_typa, &slp_typb)) {
        return -1;
    }

    if (fadt->smi_cmd != 0 && fadt->acpi_enable != 0 &&
        (inw((uint16_t)fadt->pm1a_cnt_blk) & ACPI_PM1_SCI_EN) == 0) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
        for (uint32_t i = 0; i < 1000000u; ++i) {
            if ((inw((uint16_t)fadt->pm1a_cnt_blk) & ACPI_PM1_SCI_EN) != 0) {
                break;
            }
            __asm__ __volatile__("pause");
        }
    }

    __asm__ __volatile__("cli");
    pm1a = (uint16_t)((slp_typa << ACPI_PM1_SLP_TYP_SHIFT) | ACPI_PM1_SLP_EN);
    pm1b = (uint16_t)((slp_typb << ACPI_PM1_SLP_TYP_SHIFT) | ACPI_PM1_SLP_EN);
    outw((uint16_t)fadt->pm1a_cnt_blk, pm1a);
    if (fadt->pm1b_cnt_blk != 0) {
        outw((uint16_t)fadt->pm1b_cnt_blk, pm1b);
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
