#include "ahci.h"
#include "storage.h"

#define SECTOR_SIZE 512u
#define RAMDISK_BLOCKS 131072ull
#define MBR_PARTITION_TABLE_OFFSET 446u
#define MBR_PARTITION_ENTRY_SIZE 16u
#define MBR_SIGNATURE_OFFSET 510u
#define ATA_PRIMARY_IO 0x1F0u
#define ATA_PRIMARY_CTRL 0x3F6u
#define ATA_SR_BSY 0x80u
#define ATA_SR_DF 0x20u
#define ATA_SR_DRQ 0x08u
#define ATA_SR_ERR 0x01u
#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_CACHE_FLUSH 0xE7u

static block_device_t block_devices[STORAGE_MAX_BLOCK_DEVICES];
static partition_t partitions[STORAGE_MAX_PARTITIONS];
static mount_t mounts[STORAGE_MAX_MOUNTS];
static uint32_t block_device_count;
static uint32_t partition_count;
static uint8_t ramdisk_mbr[SECTOR_SIZE];
static uint8_t zero_sector[SECTOR_SIZE];

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void mem_zero(void *ptr, uint64_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void copy_str(char *dst, const char *src, uint32_t max_len) {
    uint32_t i = 0;

    if (max_len == 0) {
        return;
    }

    while (src[i] && i + 1 < max_len) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }

    return *a == '\0' && *b == '\0';
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }

    return c;
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static const char *fs_hint_from_mbr_type(uint8_t type) {
    switch (type) {
        case 0x01:
        case 0x04:
        case 0x06:
        case 0x0B:
        case 0x0C:
        case 0x0E:
            return "fat";
        case 0x07:
            return "ntfs";
        case 0x83:
            return "linux";
        case 0x99:
            return "lainfs";
        case 0xEE:
            return "gpt";
        default:
            return "raw";
    }
}

static void make_partition_name(char *dst, const char *device_name, uint32_t partition_number) {
    uint32_t i = 0;

    while (device_name[i] && i < 7) {
        dst[i] = device_name[i];
        ++i;
    }

    dst[i++] = 'p';
    dst[i++] = (char)('0' + partition_number);
    dst[i] = '\0';
}

static void create_demo_mbr(void) {
    uint8_t *entry = &ramdisk_mbr[MBR_PARTITION_TABLE_OFFSET];

    mem_zero(ramdisk_mbr, sizeof(ramdisk_mbr));
    entry[0] = 0x80;
    entry[4] = 0x07;
    write_le32(&entry[8], 2048u);
    write_le32(&entry[12], 32768u);
    ramdisk_mbr[MBR_SIGNATURE_OFFSET] = 0x55;
    ramdisk_mbr[MBR_SIGNATURE_OFFSET + 1] = 0xAA;
}

static int ramdisk_read(void *ctx, uint64_t lba, uint32_t count, void *buffer) {
    (void)ctx;

    if (count == 0) {
        return 0;
    }

    if (lba + count > RAMDISK_BLOCKS) {
        return -1;
    }

    uint8_t *out = (uint8_t *)buffer;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *src = (lba + i == 0) ? ramdisk_mbr : zero_sector;
        for (uint32_t b = 0; b < SECTOR_SIZE; ++b) {
            out[(uint64_t)i * SECTOR_SIZE + b] = src[b];
        }
    }

    return 0;
}

typedef struct {
    uint8_t slave;
} ata_device_t;

static ata_device_t ata_devices[2];

static int ata_wait_ready(void) {
    for (uint32_t i = 0; i < 1000000u; ++i) {
        uint8_t status = inb(ATA_PRIMARY_IO + 7);
        if (status == 0xFFu) {
            continue;
        }

        if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
            return -1;
        }

        if ((status & ATA_SR_BSY) == 0) {
            return 0;
        }
    }

    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000u; ++i) {
        uint8_t status = inb(ATA_PRIMARY_IO + 7);
        if (status == 0xFFu) {
            continue;
        }

        if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
            return -1;
        }

        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ) != 0) {
            return 0;
        }
    }

    return -1;
}

static void ata_soft_reset(void) {
    outb(ATA_PRIMARY_CTRL, 0x04);
    for (uint32_t i = 0; i < 8; ++i) {
        io_wait();
    }

    outb(ATA_PRIMARY_CTRL, 0x00);
    for (uint32_t i = 0; i < 8; ++i) {
        io_wait();
    }

    ata_wait_ready();
}

static int ata_select(ata_device_t *dev, uint64_t lba) {
    if (lba > 0x0FFFFFFFull) {
        return -1;
    }

    if (ata_wait_ready() != 0) {
        return -1;
    }

    outb(ATA_PRIMARY_IO + 6, (uint8_t)(0xE0u | (dev->slave ? 0x10u : 0u) | ((lba >> 24) & 0x0Fu)));
    io_wait();
    return ata_wait_ready();
}

static int ata_pio_read(void *ctx, uint64_t lba, uint32_t count, void *buffer) {
    ata_device_t *dev = (ata_device_t *)ctx;
    uint8_t *out = (uint8_t *)buffer;

    if (count == 0) {
        return 0;
    }

    for (uint32_t sector = 0; sector < count; ++sector) {
        uint64_t current_lba = lba + sector;

        if (ata_select(dev, current_lba) != 0) {
            return -1;
        }

        outb(ATA_PRIMARY_IO + 2, 1);
        outb(ATA_PRIMARY_IO + 3, (uint8_t)(current_lba & 0xFFu));
        outb(ATA_PRIMARY_IO + 4, (uint8_t)((current_lba >> 8) & 0xFFu));
        outb(ATA_PRIMARY_IO + 5, (uint8_t)((current_lba >> 16) & 0xFFu));
        outb(ATA_PRIMARY_IO + 7, ATA_CMD_READ_SECTORS);

        if (ata_wait_drq() != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < SECTOR_SIZE / 2; ++i) {
            uint16_t word = inw(ATA_PRIMARY_IO);
            out[(uint64_t)sector * SECTOR_SIZE + i * 2] = (uint8_t)(word & 0xFFu);
            out[(uint64_t)sector * SECTOR_SIZE + i * 2 + 1] = (uint8_t)(word >> 8);
        }
    }

    return 0;
}

static int ata_pio_write(void *ctx, uint64_t lba, uint32_t count, const void *buffer) {
    ata_device_t *dev = (ata_device_t *)ctx;
    const uint8_t *in = (const uint8_t *)buffer;

    if (count == 0) {
        return 0;
    }

    for (uint32_t sector = 0; sector < count; ++sector) {
        uint64_t current_lba = lba + sector;

        if (ata_select(dev, current_lba) != 0) {
            return -1;
        }

        outb(ATA_PRIMARY_IO + 2, 1);
        outb(ATA_PRIMARY_IO + 3, (uint8_t)(current_lba & 0xFFu));
        outb(ATA_PRIMARY_IO + 4, (uint8_t)((current_lba >> 8) & 0xFFu));
        outb(ATA_PRIMARY_IO + 5, (uint8_t)((current_lba >> 16) & 0xFFu));
        outb(ATA_PRIMARY_IO + 7, ATA_CMD_WRITE_SECTORS);

        if (ata_wait_drq() != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < SECTOR_SIZE / 2; ++i) {
            uint16_t word = (uint16_t)in[(uint64_t)sector * SECTOR_SIZE + i * 2] |
                            ((uint16_t)in[(uint64_t)sector * SECTOR_SIZE + i * 2 + 1] << 8);
            outw(ATA_PRIMARY_IO, word);
        }

        if (ata_wait_ready() != 0) {
            return -1;
        }

        outb(ATA_PRIMARY_IO + 7, ATA_CMD_CACHE_FLUSH);
        if (ata_wait_ready() != 0) {
            return -1;
        }
    }

    return 0;
}

static int ata_probe(ata_device_t *dev) {
    uint8_t sector[SECTOR_SIZE];

    if (ata_pio_read(dev, 0, 1, sector) != 0) {
        return -1;
    }

    return 0;
}

int storage_register_block_device(const char *name, uint32_t block_size, uint64_t block_count, block_read_t read, block_write_t write, void *ctx) {
    if (block_device_count >= STORAGE_MAX_BLOCK_DEVICES) {
        return -1;
    }

    block_device_t *dev = &block_devices[block_device_count++];
    dev->present = 1;
    copy_str(dev->name, name, sizeof(dev->name));
    dev->block_size = block_size;
    dev->block_count = block_count;
    dev->read = read;
    dev->write = write;
    dev->ctx = ctx;
    return 0;
}

static void register_partition(uint32_t device_index, uint32_t partition_number, uint64_t start_lba, uint64_t block_count, uint8_t mbr_type) {
    if (partition_count >= STORAGE_MAX_PARTITIONS) {
        return;
    }

    const block_device_t *dev = &block_devices[device_index];
    partition_t *part = &partitions[partition_count++];

    part->present = 1;
    make_partition_name(part->name, dev->name, partition_number);
    part->device_index = device_index;
    part->partition_number = partition_number;
    part->start_lba = start_lba;
    part->block_count = block_count;
    part->mbr_type = mbr_type;
    copy_str(part->fs_hint, fs_hint_from_mbr_type(mbr_type), sizeof(part->fs_hint));
}

static void discover_mbr_partitions(uint32_t device_index) {
    uint8_t sector[SECTOR_SIZE];
    block_device_t *dev = &block_devices[device_index];

    if (!dev->present || dev->block_size != SECTOR_SIZE || dev->read == 0) {
        return;
    }

    if (dev->read(dev->ctx, 0, 1, sector) != 0) {
        return;
    }

    if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        return;
    }

    for (uint32_t i = 0; i < 4; ++i) {
        uint8_t *entry = &sector[MBR_PARTITION_TABLE_OFFSET + i * MBR_PARTITION_ENTRY_SIZE];
        uint8_t type = entry[4];
        uint32_t start_lba = read_le32(&entry[8]);
        uint32_t blocks = read_le32(&entry[12]);

        if (type == 0 || blocks == 0) {
            continue;
        }

        register_partition(device_index, i + 1, start_lba, blocks, type);
    }
}

void storage_discover_partitions(void) {
    partition_count = 0;

    for (uint32_t i = 0; i < block_device_count; ++i) {
        discover_mbr_partitions(i);
    }
}

void storage_init(void) {
    mem_zero(block_devices, sizeof(block_devices));
    mem_zero(partitions, sizeof(partitions));
    mem_zero(mounts, sizeof(mounts));
    mem_zero(zero_sector, sizeof(zero_sector));
    block_device_count = 0;
    partition_count = 0;

    create_demo_mbr();
    storage_register_block_device("rd0", SECTOR_SIZE, RAMDISK_BLOCKS, ramdisk_read, 0, 0);

    ata_soft_reset();
    ata_devices[0].slave = 0;
    ata_devices[1].slave = 1;
    if (ata_probe(&ata_devices[1]) == 0) {
        storage_register_block_device("hd1", SECTOR_SIZE, 131072ull, ata_pio_read, ata_pio_write, &ata_devices[1]);
    }

    ahci_init();
    storage_discover_partitions();
}

uint32_t storage_block_device_count(void) {
    return block_device_count;
}

const block_device_t *storage_get_block_device(uint32_t index) {
    if (index >= block_device_count || !block_devices[index].present) {
        return 0;
    }

    return &block_devices[index];
}

uint32_t storage_partition_count(void) {
    return partition_count;
}

const partition_t *storage_get_partition(uint32_t index) {
    if (index >= partition_count || !partitions[index].present) {
        return 0;
    }

    return &partitions[index];
}

const partition_t *storage_find_partition(const char *name, uint32_t *out_index) {
    for (uint32_t i = 0; i < partition_count; ++i) {
        if (partitions[i].present && str_eq(partitions[i].name, name)) {
            if (out_index) {
                *out_index = i;
            }

            return &partitions[i];
        }
    }

    return 0;
}

int storage_mount(char drive_letter, const char *partition_name) {
    uint32_t partition_index = 0;
    const partition_t *part = storage_find_partition(partition_name, &partition_index);

    if (!part) {
        return -1;
    }

    drive_letter = to_upper(drive_letter);
    if (drive_letter < 'C' || drive_letter > 'Z') {
        return -2;
    }

    uint32_t mount_index = (uint32_t)(drive_letter - 'A');
    mounts[mount_index].present = 1;
    mounts[mount_index].drive_letter = drive_letter;
    mounts[mount_index].partition_index = partition_index;
    copy_str(mounts[mount_index].fs_name, part->fs_hint, sizeof(mounts[mount_index].fs_name));
    return 0;
}

const mount_t *storage_get_mount_by_drive(char drive_letter) {
    drive_letter = to_upper(drive_letter);
    if (drive_letter < 'A' || drive_letter > 'Z') {
        return 0;
    }

    mount_t *mount = &mounts[(uint32_t)(drive_letter - 'A')];
    return mount->present ? mount : 0;
}

const mount_t *storage_get_mount(uint32_t index) {
    if (index >= STORAGE_MAX_MOUNTS || !mounts[index].present) {
        return 0;
    }

    return &mounts[index];
}

int storage_drive_is_mounted(char drive_letter) {
    return storage_get_mount_by_drive(drive_letter) != 0;
}

int storage_read_partition(uint32_t partition_index, uint64_t lba, uint32_t count, void *buffer) {
    if (partition_index >= partition_count || !partitions[partition_index].present) {
        return -1;
    }

    const partition_t *part = &partitions[partition_index];
    if (lba + count > part->block_count) {
        return -1;
    }

    block_device_t *dev = &block_devices[part->device_index];
    if (!dev->present || dev->read == 0) {
        return -1;
    }

    return dev->read(dev->ctx, part->start_lba + lba, count, buffer);
}

int storage_write_partition(uint32_t partition_index, uint64_t lba, uint32_t count, const void *buffer) {
    if (partition_index >= partition_count || !partitions[partition_index].present) {
        return -1;
    }

    const partition_t *part = &partitions[partition_index];
    if (lba + count > part->block_count) {
        return -1;
    }

    block_device_t *dev = &block_devices[part->device_index];
    if (!dev->present || dev->write == 0) {
        return -1;
    }

    return dev->write(dev->ctx, part->start_lba + lba, count, buffer);
}

int storage_partition_is_writable(uint32_t partition_index) {
    if (partition_index >= partition_count || !partitions[partition_index].present) {
        return 0;
    }

    block_device_t *dev = &block_devices[partitions[partition_index].device_index];
    return dev->present && dev->write != 0;
}
