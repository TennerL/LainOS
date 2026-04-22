#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

#define STORAGE_MAX_BLOCK_DEVICES 8
#define STORAGE_MAX_PARTITIONS 16
#define STORAGE_MAX_MOUNTS 26

typedef int (*block_read_t)(void *ctx, uint64_t lba, uint32_t count, void *buffer);
typedef int (*block_write_t)(void *ctx, uint64_t lba, uint32_t count, const void *buffer);

typedef struct {
    int present;
    char name[8];
    uint32_t block_size;
    uint64_t block_count;
    block_read_t read;
    block_write_t write;
    void *ctx;
} block_device_t;

typedef struct {
    int present;
    char name[12];
    uint32_t device_index;
    uint32_t partition_number;
    uint64_t start_lba;
    uint64_t block_count;
    uint8_t mbr_type;
    char fs_hint[8];
} partition_t;

typedef struct {
    int present;
    char drive_letter;
    uint32_t partition_index;
    char fs_name[8];
} mount_t;

void storage_init(void);
int storage_register_block_device(const char *name,
                                  uint32_t block_size,
                                  uint64_t block_count,
                                  block_read_t read,
                                  block_write_t write,
                                  void *ctx);
void storage_discover_partitions(void);

uint32_t storage_block_device_count(void);
const block_device_t *storage_get_block_device(uint32_t index);

uint32_t storage_partition_count(void);
const partition_t *storage_get_partition(uint32_t index);
const partition_t *storage_find_partition(const char *name, uint32_t *out_index);

int storage_mount(char drive_letter, const char *partition_name);
const mount_t *storage_get_mount_by_drive(char drive_letter);
const mount_t *storage_get_mount(uint32_t index);
int storage_drive_is_mounted(char drive_letter);
int storage_read_partition(uint32_t partition_index, uint64_t lba, uint32_t count, void *buffer);
int storage_write_partition(uint32_t partition_index, uint64_t lba, uint32_t count, const void *buffer);
int storage_partition_is_writable(uint32_t partition_index);

#endif
