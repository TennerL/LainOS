#include "kernel.h"
#include "lainfs.h"
#include "storage.h"

#define LAINFS_BLOCK_SIZE 512u
#define LAINFS_DIR_BLOCKS 4u
#define LAINFS_MAX_FILES 32u
#define LAINFS_ENTRY_SIZE 64u
#define LAINFS_MAGIC0 0x4E49414Cu
#define LAINFS_MAGIC1 0x315346u

typedef struct __attribute__((packed)) {
    uint32_t magic0;
    uint32_t magic1;
    uint32_t version;
    uint32_t block_size;
    uint32_t dir_start_lba;
    uint32_t dir_blocks;
    uint32_t data_start_lba;
    uint32_t max_files;
    uint8_t reserved[LAINFS_BLOCK_SIZE - 32u];
} lainfs_superblock_t;

typedef struct __attribute__((packed)) {
    uint8_t used;
    char name[31];
    uint32_t start_lba;
    uint32_t byte_size;
    uint8_t reserved[24];
} lainfs_dirent_t;

static uint8_t sector[LAINFS_BLOCK_SIZE];
static uint8_t directory[LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE];

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }

    return c;
}

static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (s[len]) {
        ++len;
    }

    return len;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }

    return *a == '\0' && *b == '\0';
}

static void mem_zero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void copy_name(char *dst, const char *src) {
    uint32_t i = 0;

    while (src[i] && i < 30u) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static const mount_t *mount_for_drive(char drive_letter) {
    return storage_get_mount_by_drive(to_upper(drive_letter));
}

static int read_super(uint32_t partition_index, lainfs_superblock_t *super) {
    if (storage_read_partition(partition_index, 0, 1, super) != 0) {
        return -1;
    }

    if (super->magic0 != LAINFS_MAGIC0 ||
        super->magic1 != LAINFS_MAGIC1 ||
        super->version != 1 ||
        super->block_size != LAINFS_BLOCK_SIZE ||
        super->dir_blocks != LAINFS_DIR_BLOCKS ||
        super->max_files != LAINFS_MAX_FILES) {
        return -2;
    }

    return 0;
}

static int load_directory(uint32_t partition_index, const lainfs_superblock_t *super) {
    return storage_read_partition(partition_index, super->dir_start_lba, super->dir_blocks, directory);
}

static int save_directory(uint32_t partition_index, const lainfs_superblock_t *super) {
    return storage_write_partition(partition_index, super->dir_start_lba, super->dir_blocks, directory);
}

static lainfs_dirent_t *dir_entry(uint32_t index) {
    return (lainfs_dirent_t *)&directory[index * LAINFS_ENTRY_SIZE];
}

int lainfs_format(char drive_letter) {
    const mount_t *mount = mount_for_drive(drive_letter);
    if (!mount) {
        return -1;
    }

    const partition_t *part = storage_get_partition(mount->partition_index);
    if (!part || part->block_count < 64u) {
        return -2;
    }

    if (!storage_partition_is_writable(mount->partition_index)) {
        return -4;
    }

    mem_zero(sector, sizeof(sector));
    lainfs_superblock_t *super = (lainfs_superblock_t *)sector;
    super->magic0 = LAINFS_MAGIC0;
    super->magic1 = LAINFS_MAGIC1;
    super->version = 1;
    super->block_size = LAINFS_BLOCK_SIZE;
    super->dir_start_lba = 1;
    super->dir_blocks = LAINFS_DIR_BLOCKS;
    super->data_start_lba = 1 + LAINFS_DIR_BLOCKS;
    super->max_files = LAINFS_MAX_FILES;

    if (storage_write_partition(mount->partition_index, 0, 1, sector) != 0) {
        return -5;
    }

    mem_zero(directory, sizeof(directory));
    if (storage_write_partition(mount->partition_index, super->dir_start_lba, super->dir_blocks, directory) != 0) {
        return -5;
    }

    return 0;
}

int lainfs_list(char drive_letter) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    int found = 0;

    if (!mount) {
        return -1;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -2;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -3;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (!entry->used) {
            continue;
        }

        found = 1;
        console_puts(entry->name);
        console_puts(" ");
        console_put_dec64(entry->byte_size);
        console_puts(" bytes\n");
    }

    if (!found) {
        console_puts("empty\n");
    }

    return 0;
}

int lainfs_write_file(char drive_letter, const char *name, const char *text) {
    return lainfs_save_file(drive_letter, name, text, str_len(text));
}

int lainfs_read_file(char drive_letter, const char *name) {
    char buffer[LAINFS_BLOCK_SIZE + 1];
    uint32_t size = 0;

    int status = lainfs_load_file(drive_letter, name, buffer, LAINFS_BLOCK_SIZE, &size);
    if (status != 0) {
        return status;
    }

    buffer[size] = '\0';

    for (uint32_t i = 0; i < size; ++i) {
        char tmp[2] = { buffer[i], '\0' };
        console_puts(tmp);
    }

    console_puts("\n");
    return 0;
}


int lainfs_save_file(char drive_letter, const char *name, const char *buffer, uint32_t size) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *slot = 0;

    if(!mount){
        return -1;
    }

    if(name[0] == '\0' || size > LAINFS_BLOCK_SIZE) {
        return -2;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if(load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if(entry->used && str_eq(entry->name, name)) {
            slot = entry;
            break;
        }
    }

    if(!slot) {
        for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
            lainfs_dirent_t *entry = dir_entry(i);
            if (!entry->used) {
                slot = entry;
                slot->used = 1;
                slot->start_lba = super.data_start_lba + i;
                break;
            }
        }
    }

    if(!slot) {
        return -5;
    }

    copy_name(slot->name, name);
    slot->byte_size = size;

    mem_zero(sector, sizeof(sector));

    for (uint32_t i = 0; i < size; ++i) {
        sector[i] = (uint8_t)buffer[i];
    }

    if (storage_write_partition(mount->partition_index, slot->start_lba, 1, sector) != 0) {
        return -6;
    }

    if (save_directory(mount->partition_index, &super) != 0) {
        return -6;
    }

    return 0;
}

int lainfs_load_file(char drive_letter, const char *name, char *buffer, uint32_t buffer_size, uint32_t *out_size) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;

    if(out_size) {
        *out_size = 0;
    }

    if(!mount) {
        return -1;
    }

    if(name[0] == '\0' || buffer_size == 0) {
        return -2;
    }

    if(read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if(load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);

        if (!entry->used || !str_eq(entry->name, name)) {
            continue;
        }

        if(entry->byte_size > LAINFS_BLOCK_SIZE || entry->byte_size > buffer_size) {
            return -6;
        }

        if(storage_read_partition(mount->partition_index, entry->start_lba, 1, sector) != 0) {
            return -7;
        }

        for (uint32_t b = 0; b < entry->byte_size && b < LAINFS_BLOCK_SIZE; ++b) {
            buffer[b] = (char)sector[b];
        }

        if(out_size) {
            *out_size = entry->byte_size;
        }

        return 0;
    }

    return -5;
}
