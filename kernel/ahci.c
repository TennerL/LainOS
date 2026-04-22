#include "ahci.h"
#include "pci.h"
#include "storage.h"

#define AHCI_MAX_DISKS 4
#define AHCI_SECTOR_SIZE 512u
#define AHCI_DEFAULT_BLOCKS 131072ull

#define PCI_CLASS_MASS_STORAGE 0x01u
#define PCI_SUBCLASS_SATA 0x06u
#define PCI_PROGIF_AHCI 0x01u

#define HBA_PORT_DET_PRESENT 0x03u
#define HBA_PORT_IPM_ACTIVE 0x01u
#define HBA_SIG_ATA 0x00000101u
#define HBA_PxCMD_ST 0x0001u
#define HBA_PxCMD_FRE 0x0010u
#define HBA_PxCMD_FR 0x4000u
#define HBA_PxCMD_CR 0x8000u
#define HBA_PxIS_TFES (1u << 30)
#define HBA_GHC_AE (1u << 31)

#define FIS_TYPE_REG_H2D 0x27u
#define ATA_CMD_READ_DMA_EXT 0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t reserved[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
} hba_mem_t;

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint8_t reserved1[0x80 - 0x48];
    uint8_t vendor[0x80 - 0x70];
} hba_port_t;

typedef struct __attribute__((packed)) {
    uint8_t command_fis_length:5;
    uint8_t atapi:1;
    uint8_t write:1;
    uint8_t prefetchable:1;
    uint8_t reset:1;
    uint8_t bist:1;
    uint8_t clear_busy:1;
    uint8_t reserved0:1;
    uint8_t port_multiplier:4;
    uint16_t prdt_length;
    volatile uint32_t prdb_count;
    uint32_t command_table_base;
    uint32_t command_table_base_upper;
    uint32_t reserved1[4];
} hba_cmd_header_t;

typedef struct __attribute__((packed)) {
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved0;
    uint32_t byte_count_interrupt;
} hba_prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    hba_prdt_entry_t prdt_entry[1];
} hba_cmd_table_t;

typedef struct __attribute__((packed)) {
    uint8_t fis_type;
    uint8_t port_multiplier:4;
    uint8_t reserved0:3;
    uint8_t command_control:1;
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t feature_high;
    uint8_t count_low;
    uint8_t count_high;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved1[4];
} fis_reg_h2d_t;

typedef struct {
    hba_mem_t *hba;
    hba_port_t *port;
    uint32_t port_index;
} ahci_disk_t;

static ahci_disk_t disks[AHCI_MAX_DISKS];
static uint32_t controller_count;
static uint32_t disk_count;

static uint8_t command_lists[AHCI_MAX_DISKS][1024] __attribute__((aligned(1024)));
static uint8_t fis_buffers[AHCI_MAX_DISKS][256] __attribute__((aligned(256)));
static hba_cmd_table_t command_tables[AHCI_MAX_DISKS] __attribute__((aligned(128)));

static void mem_zero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void copy_name(char *dst, uint32_t index) {
    dst[0] = 's';
    dst[1] = 'd';
    dst[2] = (char)('0' + index);
    dst[3] = '\0';
}

static uint64_t phys_addr(const void *ptr) {
    return (uint64_t)(uintptr_t)ptr;
}

static hba_port_t *hba_port(hba_mem_t *hba, uint32_t index) {
    return (hba_port_t *)((uintptr_t)hba + 0x100u + (uintptr_t)index * 0x80u);
}

static int port_has_ata_disk(hba_port_t *port) {
    uint32_t ssts = port->ssts;
    uint32_t det = ssts & 0x0Fu;
    uint32_t ipm = (ssts >> 8) & 0x0Fu;

    return det == HBA_PORT_DET_PRESENT &&
           ipm == HBA_PORT_IPM_ACTIVE &&
           port->sig == HBA_SIG_ATA;
}

static void stop_port(hba_port_t *port) {
    port->cmd &= ~(uint32_t)HBA_PxCMD_ST;
    port->cmd &= ~(uint32_t)HBA_PxCMD_FRE;

    for (uint32_t i = 0; i < 1000000u; ++i) {
        if ((port->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR)) == 0) {
            break;
        }
    }
}

static void start_port(hba_port_t *port) {
    while (port->cmd & HBA_PxCMD_CR) {
    }

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

static int find_command_slot(hba_port_t *port) {
    uint32_t slots = port->sact | port->ci;

    for (int i = 0; i < 32; ++i) {
        if ((slots & (1u << i)) == 0) {
            return i;
        }
    }

    return -1;
}

static int ahci_transfer(void *ctx, uint64_t lba, uint32_t count, void *buffer, int write) {
    ahci_disk_t *disk = (ahci_disk_t *)ctx;
    hba_port_t *port = disk->port;
    int slot = find_command_slot(port);

    if (slot < 0 || count == 0 || count > 128u || lba > 0x0000FFFFFFFFFFFFull) {
        return -1;
    }

    port->is = 0xFFFFFFFFu;

    hba_cmd_header_t *headers = (hba_cmd_header_t *)(uintptr_t)command_lists[disk->port_index % AHCI_MAX_DISKS];
    hba_cmd_header_t *header = &headers[slot];
    hba_cmd_table_t *table = &command_tables[disk->port_index % AHCI_MAX_DISKS];

    mem_zero(header, sizeof(*header));
    mem_zero(table, sizeof(*table));

    header->command_fis_length = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    header->write = write ? 1u : 0u;
    header->prdt_length = 1;
    header->command_table_base = (uint32_t)(phys_addr(table) & 0xFFFFFFFFu);
    header->command_table_base_upper = (uint32_t)(phys_addr(table) >> 32);

    table->prdt_entry[0].data_base = (uint32_t)(phys_addr(buffer) & 0xFFFFFFFFu);
    table->prdt_entry[0].data_base_upper = (uint32_t)(phys_addr(buffer) >> 32);
    table->prdt_entry[0].byte_count_interrupt = (count * AHCI_SECTOR_SIZE - 1u);

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)table->command_fis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->command_control = 1;
    fis->command = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->device = 1u << 6;
    fis->lba0 = (uint8_t)(lba & 0xFFu);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFFu);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFFu);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFFu);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFFu);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFFu);
    fis->count_low = (uint8_t)(count & 0xFFu);
    fis->count_high = (uint8_t)((count >> 8) & 0xFFu);

    for (uint32_t i = 0; i < 1000000u; ++i) {
        if ((port->tfd & (0x80u | 0x08u)) == 0) {
            break;
        }

        if (i == 999999u) {
            return -1;
        }
    }

    port->ci = 1u << slot;

    for (uint32_t i = 0; i < 10000000u; ++i) {
        if ((port->ci & (1u << slot)) == 0) {
            return (port->is & HBA_PxIS_TFES) ? -1 : 0;
        }

        if (port->is & HBA_PxIS_TFES) {
            return -1;
        }
    }

    return -1;
}

static int ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buffer) {
    return ahci_transfer(ctx, lba, count, buffer, 0);
}

static int ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buffer) {
    return ahci_transfer(ctx, lba, count, (void *)buffer, 1);
}

static void configure_port(hba_mem_t *hba, uint32_t port_index) {
    if (disk_count >= AHCI_MAX_DISKS) {
        return;
    }

    hba_port_t *port = hba_port(hba, port_index);
    if (!port_has_ata_disk(port)) {
        return;
    }

    uint32_t disk_index = disk_count;
    stop_port(port);
    mem_zero(command_lists[disk_index], sizeof(command_lists[disk_index]));
    mem_zero(fis_buffers[disk_index], sizeof(fis_buffers[disk_index]));
    mem_zero(&command_tables[disk_index], sizeof(command_tables[disk_index]));

    uint64_t clb = phys_addr(command_lists[disk_index]);
    uint64_t fb = phys_addr(fis_buffers[disk_index]);
    port->clb = (uint32_t)(clb & 0xFFFFFFFFu);
    port->clbu = (uint32_t)(clb >> 32);
    port->fb = (uint32_t)(fb & 0xFFFFFFFFu);
    port->fbu = (uint32_t)(fb >> 32);
    port->is = 0xFFFFFFFFu;
    port->serr = 0xFFFFFFFFu;
    start_port(port);

    disks[disk_index].hba = hba;
    disks[disk_index].port = port;
    disks[disk_index].port_index = disk_index;

    char name[8];
    copy_name(name, disk_index);
    if (storage_register_block_device(name, AHCI_SECTOR_SIZE, AHCI_DEFAULT_BLOCKS, ahci_read, ahci_write, &disks[disk_index]) == 0) {
        ++disk_count;
    }
}

static void visit_pci(uint8_t bus, uint8_t device, uint8_t function, void *ctx) {
    (void)ctx;

    uint8_t class_code = pci_read_config8(bus, device, function, 0x0B);
    uint8_t subclass = pci_read_config8(bus, device, function, 0x0A);
    uint8_t prog_if = pci_read_config8(bus, device, function, 0x09);

    if (class_code != PCI_CLASS_MASS_STORAGE ||
        subclass != PCI_SUBCLASS_SATA ||
        prog_if != PCI_PROGIF_AHCI) {
        return;
    }

    uint16_t command = pci_read_config16(bus, device, function, 0x04);
    command |= 0x0006u;
    pci_write_config32(bus, device, function, 0x04,
        (pci_read_config32(bus, device, function, 0x04) & 0xFFFF0000u) | command);

    uint32_t bar5 = pci_read_config32(bus, device, function, 0x24);
    uintptr_t abar = (uintptr_t)(bar5 & 0xFFFFFFF0u);
    if (abar == 0) {
        return;
    }

    hba_mem_t *hba = (hba_mem_t *)abar;
    hba->ghc |= HBA_GHC_AE;
    ++controller_count;

    uint32_t ports_implemented = hba->pi;
    for (uint32_t i = 0; i < 32; ++i) {
        if (ports_implemented & (1u << i)) {
            configure_port(hba, i);
        }
    }
}

void ahci_init(void) {
    controller_count = 0;
    disk_count = 0;
    pci_scan(visit_pci, 0);
}

uint32_t ahci_controller_count(void) {
    return controller_count;
}

uint32_t ahci_disk_count(void) {
    return disk_count;
}
