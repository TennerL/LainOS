#include "kernel.h"
#include "lainfs.h"
#include "storage.h"

#define LAINFS_BLOCK_SIZE 512u
#define LAINFS_DIR_BLOCKS 64u
#define LAINFS_ENTRY_SIZE 64u
#define LAINFS_MAX_FILES ((LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE) / LAINFS_ENTRY_SIZE)
#define LAINFS_MAX_FILE_BLOCKS (LAINFS_FILE_CAPACITY / LAINFS_BLOCK_SIZE)
#define LAINFS_MAGIC0 0x4E49414Cu
#define LAINFS_MAGIC1 0x315346u
#define LAINFS_ENTRY_FILE LAINFS_ENTRY_TYPE_FILE
#define LAINFS_ENTRY_DIR LAINFS_ENTRY_TYPE_DIR

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

static uint32_t blocks_for_size(uint32_t size) {
    return (size + LAINFS_BLOCK_SIZE - 1u) / LAINFS_BLOCK_SIZE;
}

static uint32_t allocated_blocks_for_entry(const lainfs_dirent_t *entry) {
    uint32_t blocks = blocks_for_size(entry->byte_size);
    return blocks == 0 ? 1u : blocks;
}

static int ranges_overlap(uint32_t a_start, uint32_t a_blocks, uint32_t b_start, uint32_t b_blocks) {
    uint32_t a_end = a_start + a_blocks;
    uint32_t b_end = b_start + b_blocks;

    return a_start < b_end && b_start < a_end;
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

static int format_partition_index(uint32_t partition_index) {
    const partition_t *part = storage_get_partition(partition_index);

    if (!part || part->block_count < 64u) {
        return -2;
    }

    if (!storage_partition_is_writable(partition_index)) {
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

    if (storage_write_partition(partition_index, 0, 1, sector) != 0) {
        return -5;
    }

    mem_zero(directory, sizeof(directory));
    if (storage_write_partition(partition_index, super->dir_start_lba, super->dir_blocks, directory) != 0) {
        return -5;
    }

    storage_discover_partitions();
    return 0;
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

static uint32_t entry_id_from_index(uint32_t index) {
    return index + 1u;
}

static uint32_t index_from_entry_id(uint32_t entry_id) {
    return entry_id - 1u;
}

static uint32_t entry_parent_id(const lainfs_dirent_t *entry) {
    return ((uint32_t)entry->reserved[0]) |
           ((uint32_t)entry->reserved[1] << 8) |
           ((uint32_t)entry->reserved[2] << 16) |
           ((uint32_t)entry->reserved[3] << 24);
}

static void set_entry_parent_id(lainfs_dirent_t *entry, uint32_t parent_id) {
    entry->reserved[0] = (uint8_t)(parent_id & 0xFFu);
    entry->reserved[1] = (uint8_t)((parent_id >> 8) & 0xFFu);
    entry->reserved[2] = (uint8_t)((parent_id >> 16) & 0xFFu);
    entry->reserved[3] = (uint8_t)((parent_id >> 24) & 0xFFu);
}

static int valid_dir_id(uint32_t dir_id) {
    lainfs_dirent_t *entry;

    if (dir_id == LAINFS_ROOT_DIR) {
        return 1;
    }

    if (dir_id > LAINFS_MAX_FILES) {
        return 0;
    }

    entry = dir_entry(index_from_entry_id(dir_id));
    return entry->used == LAINFS_ENTRY_DIR;
}

static int valid_name(const char *name) {
    uint32_t len = str_len(name);

    if (len == 0 || len > 30u) {
        return 0;
    }

    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':' ||
            name[i] == ' ' || name[i] == '\t') {
            return 0;
        }
    }

    return 1;
}

static lainfs_dirent_t *find_entry_in_dir(uint32_t parent_id, const char *name) {
    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (entry->used && entry_parent_id(entry) == parent_id && str_eq(entry->name, name)) {
            return entry;
        }
    }

    return 0;
}

static lainfs_dirent_t *find_free_entry(void) {
    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (!entry->used) {
            return entry;
        }
    }

    return 0;
}

static uint32_t entry_id(const lainfs_dirent_t *entry) {
    uint32_t byte_offset = (uint32_t)((const uint8_t *)entry - directory);
    return entry_id_from_index(byte_offset / LAINFS_ENTRY_SIZE);
}

static int extent_is_free(const lainfs_superblock_t *super,
                          uint32_t start_lba,
                          uint32_t blocks,
                          const lainfs_dirent_t *ignore) {
    if (blocks == 0 || start_lba < super->data_start_lba) {
        return 0;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);

        if (entry == ignore || entry->used != LAINFS_ENTRY_FILE) {
            continue;
        }

        if (ranges_overlap(start_lba, blocks, entry->start_lba, allocated_blocks_for_entry(entry))) {
            return 0;
        }
    }

    return 1;
}

static int find_free_extent(uint32_t partition_index,
                            const lainfs_superblock_t *super,
                            uint32_t blocks,
                            const lainfs_dirent_t *ignore,
                            uint32_t *out_start_lba) {
    const partition_t *part = storage_get_partition(partition_index);
    uint64_t data_end = part ? part->block_count : 0;

    if (!part || blocks == 0 || blocks > LAINFS_MAX_FILE_BLOCKS) {
        return -1;
    }

    for (uint64_t start = super->data_start_lba; start + blocks <= data_end; ++start) {
        if (extent_is_free(super, (uint32_t)start, blocks, ignore)) {
            *out_start_lba = (uint32_t)start;
            return 0;
        }
    }

    return -1;
}

static int clear_file_blocks(uint32_t partition_index, const lainfs_dirent_t *entry) {
    uint32_t blocks = allocated_blocks_for_entry(entry);

    mem_zero(sector, sizeof(sector));
    for (uint32_t i = 0; i < blocks; ++i) {
        if (storage_write_partition(partition_index, entry->start_lba + i, 1, sector) != 0) {
            return -1;
        }
    }

    return 0;
}

static int dir_has_children(uint32_t parent_id) {
    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (entry->used && entry_parent_id(entry) == parent_id) {
            return 1;
        }
    }

    return 0;
}

static int dir_is_descendant_of(uint32_t candidate_id, uint32_t ancestor_id) {
    uint32_t current = candidate_id;

    while (current != LAINFS_ROOT_DIR) {
        lainfs_dirent_t *entry;

        if (current > LAINFS_MAX_FILES) {
            return 0;
        }

        if (current == ancestor_id) {
            return 1;
        }

        entry = dir_entry(index_from_entry_id(current));
        if (entry->used != LAINFS_ENTRY_DIR) {
            return 0;
        }

        current = entry_parent_id(entry);
    }

    return ancestor_id == LAINFS_ROOT_DIR;
}

static void print_padded(const char *s, uint32_t width) {
    uint32_t len = str_len(s);

    console_puts(s);
    while (len < width) {
        console_puts(" ");
        ++len;
    }
}

int lainfs_format(char drive_letter) {
    const mount_t *mount = mount_for_drive(drive_letter);
    if (!mount) {
        return -1;
    }

    return format_partition_index(mount->partition_index);
}

int lainfs_format_partition(const char *partition_name) {
    uint32_t partition_index = 0;

    if (!storage_find_partition(partition_name, &partition_index)) {
        return -1;
    }

    return format_partition_index(partition_index);
}

int lainfs_format_block_device(const char *device_name, char *out_partition_name, uint32_t out_partition_name_size) {
    uint32_t device_index = 0;
    uint32_t partition_index = 0;
    const block_device_t *dev = storage_find_block_device(device_name, &device_index);
    const partition_t *part;

    if (out_partition_name && out_partition_name_size > 0) {
        out_partition_name[0] = '\0';
    }

    if (!dev) {
        return -1;
    }

    if (storage_create_mbr_partition(device_index, 0x99u, &partition_index) != 0) {
        return -2;
    }

    part = storage_get_partition(partition_index);
    if (!part) {
        return -3;
    }

    if (out_partition_name && out_partition_name_size > 0) {
        uint32_t i = 0;
        while (part->name[i] && i + 1 < out_partition_name_size) {
            out_partition_name[i] = part->name[i];
            ++i;
        }
        out_partition_name[i] = '\0';
    }

    return format_partition_index(partition_index);
}

int lainfs_list(char drive_letter) {
    return lainfs_list_dir(drive_letter, LAINFS_ROOT_DIR);
}

int lainfs_list_dir(char drive_letter, uint32_t parent_id) {
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

    if (!valid_dir_id(parent_id)) {
        return -5;
    }

    console_puts("TYPE  NAME                           SIZE\n");
    console_puts("----  -----------------------------  --------\n");

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (!entry->used) {
            continue;
        }

        if (entry_parent_id(entry) != parent_id) {
            continue;
        }

        found = 1;
        print_padded(entry->used == LAINFS_ENTRY_DIR ? "DIR" : "FILE", 6u);
        print_padded(entry->name, 31u);
        if (entry->used == LAINFS_ENTRY_DIR) {
            console_puts("-");
        } else {
            console_put_dec64(entry->byte_size);
        }
        console_puts("\n");
    }

    if (!found) {
        console_puts("empty\n");
    }

    return 0;
}

int lainfs_make_dir(char drive_letter, const char *name) {
    return lainfs_make_dir_in_dir(drive_letter, LAINFS_ROOT_DIR, name);
}

int lainfs_make_dir_in_dir(char drive_letter, uint32_t parent_id, const char *name) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *slot = 0;

    if (!mount) {
        return -1;
    }

    if (!valid_name(name)) {
        return -2;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        return -7;
    }

    if (find_entry_in_dir(parent_id, name)) {
        return -6;
    }

    slot = find_free_entry();
    if (!slot) {
        return -5;
    }

    mem_zero(slot, sizeof(*slot));
    slot->used = LAINFS_ENTRY_DIR;
    copy_name(slot->name, name);
    set_entry_parent_id(slot, parent_id);

    if (save_directory(mount->partition_index, &super) != 0) {
        return -7;
    }

    return 0;
}

int lainfs_delete(char drive_letter, const char *name) {
    return lainfs_delete_in_dir(drive_letter, LAINFS_ROOT_DIR, name);
}

int lainfs_delete_in_dir(char drive_letter, uint32_t parent_id, const char *name) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *entry = 0;

    if (!mount) {
        return -1;
    }

    if (!valid_name(name)) {
        return -2;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        return -7;
    }

    entry = find_entry_in_dir(parent_id, name);
    if (!entry) {
        return -5;
    }

    if (entry->used == LAINFS_ENTRY_DIR && dir_has_children(entry_id(entry))) {
        return -9;
    }

    if (entry->used == LAINFS_ENTRY_FILE) {
        if (clear_file_blocks(mount->partition_index, entry) != 0) {
            return -6;
        }
    }

    mem_zero(entry, sizeof(*entry));
    if (save_directory(mount->partition_index, &super) != 0) {
        return -6;
    }

    return 0;
}

int lainfs_rename(char drive_letter, const char *old_name, const char *new_name) {
    return lainfs_rename_in_dir(drive_letter, LAINFS_ROOT_DIR, old_name, LAINFS_ROOT_DIR, new_name);
}

int lainfs_rename_in_dir(char drive_letter,
                         uint32_t old_parent_id,
                         const char *old_name,
                         uint32_t new_parent_id,
                         const char *new_name) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *entry = 0;

    if (!mount) {
        return -1;
    }

    if (!valid_name(old_name) || !valid_name(new_name)) {
        return -2;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(old_parent_id) || !valid_dir_id(new_parent_id)) {
        return -7;
    }

    entry = find_entry_in_dir(old_parent_id, old_name);
    if (!entry) {
        return -5;
    }

    if (entry->used == LAINFS_ENTRY_DIR &&
        (entry_id(entry) == new_parent_id || dir_is_descendant_of(new_parent_id, entry_id(entry)))) {
        return -8;
    }

    if (find_entry_in_dir(new_parent_id, new_name)) {
        return -6;
    }

    copy_name(entry->name, new_name);
    set_entry_parent_id(entry, new_parent_id);
    if (save_directory(mount->partition_index, &super) != 0) {
        return -7;
    }

    return 0;
}

int lainfs_find_dir(char drive_letter, uint32_t parent_id, const char *name, uint32_t *out_dir_id) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *entry = 0;

    if (out_dir_id) {
        *out_dir_id = LAINFS_ROOT_DIR;
    }

    if (!mount) {
        return -1;
    }

    if (!valid_name(name)) {
        return -2;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        return -7;
    }

    entry = find_entry_in_dir(parent_id, name);
    if (!entry || entry->used != LAINFS_ENTRY_DIR) {
        return -5;
    }

    if (out_dir_id) {
        *out_dir_id = entry_id(entry);
    }

    return 0;
}

int lainfs_parent_dir(char drive_letter, uint32_t dir_id, uint32_t *out_parent_id) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *entry = 0;

    if (out_parent_id) {
        *out_parent_id = LAINFS_ROOT_DIR;
    }

    if (!mount) {
        return -1;
    }

    if (dir_id == LAINFS_ROOT_DIR) {
        return 0;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(dir_id)) {
        return -5;
    }

    entry = dir_entry(index_from_entry_id(dir_id));
    if (out_parent_id) {
        *out_parent_id = entry_parent_id(entry);
    }

    return 0;
}

int lainfs_child_count(char drive_letter, uint32_t parent_id, uint32_t *out_count) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    uint32_t count = 0;

    if (!mount || out_count == 0) {
        return -1;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -2;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (!valid_dir_id(parent_id)) {
        return -5;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (entry->used && entry_parent_id(entry) == parent_id) {
            ++count;
        }
    }

    *out_count = count;
    return 0;
}

int lainfs_child_info(char drive_letter,
                      uint32_t parent_id,
                      uint32_t child_index,
                      char *out_name,
                      uint32_t out_name_size,
                      uint32_t *out_type,
                      uint32_t *out_size) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    uint32_t seen = 0;

    if (!mount || out_name == 0 || out_name_size == 0 || out_type == 0 || out_size == 0) {
        return -1;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -2;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        return -3;
    }

    if (!valid_dir_id(parent_id)) {
        return -5;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (!entry->used || entry_parent_id(entry) != parent_id) {
            continue;
        }

        if (seen == child_index) {
            uint32_t n = 0;
            while (entry->name[n] && n + 1u < out_name_size) {
                out_name[n] = entry->name[n];
                ++n;
            }
            out_name[n] = '\0';
            *out_type = entry->used;
            *out_size = entry->used == LAINFS_ENTRY_DIR ? 0 : entry->byte_size;
            return 0;
        }

        ++seen;
    }

    return -6;
}

int lainfs_write_file(char drive_letter, const char *name, const char *text) {
    return lainfs_write_file_in_dir(drive_letter, LAINFS_ROOT_DIR, name, text);
}

int lainfs_write_file_in_dir(char drive_letter, uint32_t parent_id, const char *name, const char *text) {
    return lainfs_save_file_in_dir(drive_letter, parent_id, name, text, str_len(text));
}

int lainfs_read_file(char drive_letter, const char *name) {
    return lainfs_read_file_in_dir(drive_letter, LAINFS_ROOT_DIR, name);
}

int lainfs_read_file_in_dir(char drive_letter, uint32_t parent_id, const char *name) {
    static char buffer[LAINFS_FILE_CAPACITY + 1];
    uint32_t size = 0;

    int status = lainfs_load_file_in_dir(drive_letter, parent_id, name, buffer, LAINFS_FILE_CAPACITY, &size);
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
    return lainfs_save_file_in_dir(drive_letter, LAINFS_ROOT_DIR, name, buffer, size);
}

int lainfs_save_file_in_dir(char drive_letter,
                            uint32_t parent_id,
                            const char *name,
                            const char *buffer,
                            uint32_t size) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *slot = 0;

    if(!mount){
        return -1;
    }

    if(!valid_name(name) || size > LAINFS_FILE_CAPACITY) {
        return -2;
    }

    if (read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if(load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        return -7;
    }

    slot = find_entry_in_dir(parent_id, name);
    if (slot && slot->used == LAINFS_ENTRY_DIR) {
        return -8;
    }

    if(!slot) {
        for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
            lainfs_dirent_t *entry = dir_entry(i);
            if (!entry->used) {
                slot = entry;
                slot->used = LAINFS_ENTRY_FILE;
                set_entry_parent_id(slot, parent_id);
                break;
            }
        }
    }

    if(!slot) {
        return -5;
    }

    uint32_t new_blocks = blocks_for_size(size);
    uint32_t required_blocks = new_blocks == 0 ? 1u : new_blocks;
    uint32_t current_blocks = slot->start_lba ? allocated_blocks_for_entry(slot) : 0;
    if (current_blocks < required_blocks || !extent_is_free(&super, slot->start_lba, required_blocks, slot)) {
        uint32_t new_start_lba = 0;

        if (find_free_extent(mount->partition_index, &super, required_blocks, slot, &new_start_lba) != 0) {
            return -9;
        }

        if (slot->start_lba && clear_file_blocks(mount->partition_index, slot) != 0) {
            return -6;
        }

        slot->start_lba = new_start_lba;
    } else if (current_blocks > required_blocks) {
        lainfs_dirent_t old_entry = *slot;
        old_entry.byte_size = current_blocks * LAINFS_BLOCK_SIZE;
        if (clear_file_blocks(mount->partition_index, &old_entry) != 0) {
            return -6;
        }
    }

    copy_name(slot->name, name);
    slot->byte_size = size;

    for (uint32_t block = 0; block < required_blocks; ++block) {
        uint32_t base = block * LAINFS_BLOCK_SIZE;
        uint32_t bytes_left = size > base ? size - base : 0;
        uint32_t bytes_to_copy = bytes_left < LAINFS_BLOCK_SIZE ? bytes_left : LAINFS_BLOCK_SIZE;

        mem_zero(sector, sizeof(sector));
        for (uint32_t i = 0; i < bytes_to_copy; ++i) {
            sector[i] = (uint8_t)buffer[base + i];
        }

        if (storage_write_partition(mount->partition_index, slot->start_lba + block, 1, sector) != 0) {
            return -6;
        }
    }

    if (save_directory(mount->partition_index, &super) != 0) {
        return -6;
    }

    return 0;
}

int lainfs_load_file(char drive_letter, const char *name, char *buffer, uint32_t buffer_size, uint32_t *out_size) {
    return lainfs_load_file_in_dir(drive_letter, LAINFS_ROOT_DIR, name, buffer, buffer_size, out_size);
}

int lainfs_load_file_in_dir(char drive_letter,
                            uint32_t parent_id,
                            const char *name,
                            char *buffer,
                            uint32_t buffer_size,
                            uint32_t *out_size) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;

    if(out_size) {
        *out_size = 0;
    }

    if(!mount) {
        return -1;
    }

    if(!valid_name(name) || buffer_size == 0) {
        return -2;
    }

    if(read_super(mount->partition_index, &super) != 0) {
        return -3;
    }

    if(load_directory(mount->partition_index, &super) != 0) {
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        return -7;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);

        if (!entry->used ||
            entry->used == LAINFS_ENTRY_DIR ||
            entry_parent_id(entry) != parent_id ||
            !str_eq(entry->name, name)) {
            continue;
        }

        if(entry->byte_size > LAINFS_FILE_CAPACITY || entry->byte_size > buffer_size) {
            return -6;
        }

        uint32_t blocks = blocks_for_size(entry->byte_size);
        for (uint32_t block = 0; block < blocks; ++block) {
            uint32_t base = block * LAINFS_BLOCK_SIZE;
            uint32_t bytes_left = entry->byte_size - base;
            uint32_t bytes_to_copy = bytes_left < LAINFS_BLOCK_SIZE ? bytes_left : LAINFS_BLOCK_SIZE;

            if(storage_read_partition(mount->partition_index, entry->start_lba + block, 1, sector) != 0) {
                return -7;
            }

            for (uint32_t b = 0; b < bytes_to_copy; ++b) {
                buffer[base + b] = (char)sector[b];
            }
        }

        if(out_size) {
            *out_size = entry->byte_size;
        }

        return 0;
    }

    return -5;
}
