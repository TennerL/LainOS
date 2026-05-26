#include "kernel.h"
#include "kmem.h"
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
#define LAINFS_DATA_CACHE_SLOTS 16u
#define LAINFS_EXTENT_BLOCK_MASK 0x00FFFFFFu
#define LAINFS_EXTENT_COUNT_SHIFT 24u

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

typedef struct {
    int valid;
    int dirty;
    uint32_t partition_index;
    uint32_t lba;
    uint64_t age;
    uint8_t data[LAINFS_BLOCK_SIZE];
} lainfs_data_cache_slot_t;

typedef struct {
    uint32_t start_lba;
    uint32_t blocks;
} lainfs_extent_t;

static uint8_t sector[LAINFS_BLOCK_SIZE];
static uint8_t directory[LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE];
static lainfs_superblock_t super_cache;
static int super_cache_valid;
static uint32_t super_cache_partition;
static int directory_cache_valid;
static int directory_cache_dirty;
static uint32_t directory_cache_partition;
static lainfs_data_cache_slot_t data_cache[LAINFS_DATA_CACHE_SLOTS];
static uint64_t cache_clock;
static lainfs_cache_stats_t cache_stats;
static volatile unsigned int lainfs_global_lock;

static int validate_directory(uint32_t partition_index,
                              const lainfs_superblock_t *super,
                              uint32_t *out_reason,
                              uint32_t *out_entry_id);
static int load_directory_unchecked(uint32_t partition_index, const lainfs_superblock_t *super);

static void lainfs_lock(void) {
    while (__sync_lock_test_and_set(&lainfs_global_lock, 1u) != 0u) {
        __asm__ volatile("pause");
    }
}

static void lainfs_unlock(void) {
    __sync_lock_release(&lainfs_global_lock);
}

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

static void mem_copy(void *dst, const void *src, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint32_t i = 0; i < size; ++i) {
        d[i] = s[i];
    }
}

static uint32_t blocks_for_size(uint32_t size) {
    return (size + LAINFS_BLOCK_SIZE - 1u) / LAINFS_BLOCK_SIZE;
}

static uint32_t allocated_blocks_for_entry(const lainfs_dirent_t *entry) {
    uint32_t blocks = blocks_for_size(entry->byte_size);
    return blocks == 0 ? 1u : blocks;
}

static uint32_t entry_extent_meta(const lainfs_dirent_t *entry) {
    return ((uint32_t)entry->reserved[4]) |
           ((uint32_t)entry->reserved[5] << 8) |
           ((uint32_t)entry->reserved[6] << 16) |
           ((uint32_t)entry->reserved[7] << 24);
}

static void set_entry_extent_meta(lainfs_dirent_t *entry, uint32_t meta) {
    entry->reserved[4] = (uint8_t)(meta & 0xFFu);
    entry->reserved[5] = (uint8_t)((meta >> 8) & 0xFFu);
    entry->reserved[6] = (uint8_t)((meta >> 16) & 0xFFu);
    entry->reserved[7] = (uint8_t)((meta >> 24) & 0xFFu);
}

static uint32_t entry_extra_extent_start(const lainfs_dirent_t *entry, uint32_t extra_index) {
    uint32_t offset = 8u + extra_index * 8u;
    return ((uint32_t)entry->reserved[offset]) |
           ((uint32_t)entry->reserved[offset + 1u] << 8) |
           ((uint32_t)entry->reserved[offset + 2u] << 16) |
           ((uint32_t)entry->reserved[offset + 3u] << 24);
}

static uint32_t entry_extra_extent_blocks(const lainfs_dirent_t *entry, uint32_t extra_index) {
    uint32_t offset = 12u + extra_index * 8u;
    return ((uint32_t)entry->reserved[offset]) |
           ((uint32_t)entry->reserved[offset + 1u] << 8) |
           ((uint32_t)entry->reserved[offset + 2u] << 16) |
           ((uint32_t)entry->reserved[offset + 3u] << 24);
}

static void set_entry_extra_extent(lainfs_dirent_t *entry, uint32_t extra_index, uint32_t start_lba, uint32_t blocks) {
    uint32_t offset = 8u + extra_index * 8u;

    entry->reserved[offset] = (uint8_t)(start_lba & 0xFFu);
    entry->reserved[offset + 1u] = (uint8_t)((start_lba >> 8) & 0xFFu);
    entry->reserved[offset + 2u] = (uint8_t)((start_lba >> 16) & 0xFFu);
    entry->reserved[offset + 3u] = (uint8_t)((start_lba >> 24) & 0xFFu);
    entry->reserved[offset + 4u] = (uint8_t)(blocks & 0xFFu);
    entry->reserved[offset + 5u] = (uint8_t)((blocks >> 8) & 0xFFu);
    entry->reserved[offset + 6u] = (uint8_t)((blocks >> 16) & 0xFFu);
    entry->reserved[offset + 7u] = (uint8_t)((blocks >> 24) & 0xFFu);
}

static int entry_extents(const lainfs_dirent_t *entry, lainfs_extent_t *extents, uint32_t *out_count) {
    uint32_t required_blocks = allocated_blocks_for_entry(entry);
    uint32_t meta = entry_extent_meta(entry);
    uint32_t count;
    uint32_t total = 0;

    if (meta == 0) {
        extents[0].start_lba = entry->start_lba;
        extents[0].blocks = required_blocks;
        *out_count = 1;
        return 0;
    }

    count = meta >> LAINFS_EXTENT_COUNT_SHIFT;
    if (count == 0 || count > LAINFS_MAX_EXTENTS) {
        return -1;
    }

    extents[0].start_lba = entry->start_lba;
    extents[0].blocks = meta & LAINFS_EXTENT_BLOCK_MASK;
    for (uint32_t i = 1; i < count; ++i) {
        extents[i].start_lba = entry_extra_extent_start(entry, i - 1u);
        extents[i].blocks = entry_extra_extent_blocks(entry, i - 1u);
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (extents[i].blocks == 0 || extents[i].blocks > required_blocks - total) {
            return -1;
        }
        total += extents[i].blocks;
    }

    if (total != required_blocks) {
        return -1;
    }

    *out_count = count;
    return 0;
}

static void set_entry_extents(lainfs_dirent_t *entry, const lainfs_extent_t *extents, uint32_t count) {
    for (uint32_t i = 4; i < sizeof(entry->reserved); ++i) {
        entry->reserved[i] = 0;
    }

    entry->start_lba = extents[0].start_lba;
    if (count <= 1) {
        return;
    }

    set_entry_extent_meta(entry,
                          ((count << LAINFS_EXTENT_COUNT_SHIFT) |
                           (extents[0].blocks & LAINFS_EXTENT_BLOCK_MASK)));
    for (uint32_t i = 1; i < count; ++i) {
        set_entry_extra_extent(entry, i - 1u, extents[i].start_lba, extents[i].blocks);
    }
}

static int ranges_overlap(uint32_t a_start, uint32_t a_blocks, uint32_t b_start, uint32_t b_blocks) {
    uint32_t a_end = a_start + a_blocks;
    uint32_t b_end = b_start + b_blocks;

    return a_start < b_end && b_start < a_end;
}

static int legacy_extent_fields_valid(uint32_t partition_index,
                                      const lainfs_superblock_t *super,
                                      const lainfs_dirent_t *entry) {
    const partition_t *part = storage_get_partition(partition_index);
    uint32_t blocks;
    uint64_t end_lba;

    if (!part || !super || entry->used != LAINFS_ENTRY_FILE || entry->byte_size > LAINFS_FILE_CAPACITY) {
        return 0;
    }

    blocks = allocated_blocks_for_entry(entry);
    end_lba = (uint64_t)entry->start_lba + blocks;
    return blocks != 0 &&
           blocks <= LAINFS_MAX_FILE_BLOCKS &&
           entry->start_lba >= super->data_start_lba &&
           end_lba <= part->block_count;
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

static int raw_read_super(uint32_t partition_index, lainfs_superblock_t *super) {
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

static int flush_data_cache_slot(uint32_t slot_index) {
    lainfs_data_cache_slot_t *slot;

    if (slot_index >= LAINFS_DATA_CACHE_SLOTS) {
        return -1;
    }

    slot = &data_cache[slot_index];
    if (!slot->valid || !slot->dirty) {
        return 0;
    }

    if (storage_write_partition(slot->partition_index, slot->lba, 1, slot->data) != 0) {
        return -1;
    }

    slot->dirty = 0;
    ++cache_stats.flushes;
    return 0;
}

static int flush_data_cache_partition(uint32_t partition_index) {
    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        if (data_cache[i].valid &&
            data_cache[i].dirty &&
            data_cache[i].partition_index == partition_index &&
            flush_data_cache_slot(i) != 0) {
            return -1;
        }
    }

    return 0;
}

static int flush_directory_cache(void) {
    lainfs_superblock_t super;

    if (!directory_cache_valid || !directory_cache_dirty) {
        return 0;
    }

    if (super_cache_valid && super_cache_partition == directory_cache_partition) {
        super = super_cache;
    } else if (raw_read_super(directory_cache_partition, &super) != 0) {
        return -1;
    }

    if (storage_write_partition(directory_cache_partition,
                                super.dir_start_lba,
                                super.dir_blocks,
                                directory) != 0) {
        return -1;
    }

    directory_cache_dirty = 0;
    ++cache_stats.flushes;
    return 0;
}

static int flush_partition_cache(uint32_t partition_index) {
    if (flush_data_cache_partition(partition_index) != 0) {
        return -1;
    }

    if (directory_cache_valid &&
        directory_cache_dirty &&
        directory_cache_partition == partition_index &&
        flush_directory_cache() != 0) {
        return -1;
    }

    return 0;
}

static void invalidate_partition_cache(uint32_t partition_index) {
    if (super_cache_valid && super_cache_partition == partition_index) {
        super_cache_valid = 0;
    }
    if (directory_cache_valid && directory_cache_partition == partition_index) {
        directory_cache_valid = 0;
        directory_cache_dirty = 0;
    }
    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        if (data_cache[i].valid && data_cache[i].partition_index == partition_index) {
            data_cache[i].valid = 0;
            data_cache[i].dirty = 0;
        }
    }
}

static int cached_read_block(uint32_t partition_index, uint32_t lba, void *buffer) {
    uint32_t victim = 0;
    uint64_t oldest = 0xffffffffffffffffull;

    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        lainfs_data_cache_slot_t *slot = &data_cache[i];
        if (slot->valid && slot->partition_index == partition_index && slot->lba == lba) {
            slot->age = ++cache_clock;
            mem_copy(buffer, slot->data, LAINFS_BLOCK_SIZE);
            ++cache_stats.data_cache_hits;
            return 0;
        }
    }

    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        if (!data_cache[i].valid) {
            victim = i;
            oldest = 0;
            break;
        }
        if (data_cache[i].age < oldest) {
            oldest = data_cache[i].age;
            victim = i;
        }
    }

    (void)oldest;
    if (flush_data_cache_slot(victim) != 0) {
        return -1;
    }

    if (storage_read_partition(partition_index, lba, 1, data_cache[victim].data) != 0) {
        data_cache[victim].valid = 0;
        data_cache[victim].dirty = 0;
        return -1;
    }

    data_cache[victim].valid = 1;
    data_cache[victim].dirty = 0;
    data_cache[victim].partition_index = partition_index;
    data_cache[victim].lba = lba;
    data_cache[victim].age = ++cache_clock;
    mem_copy(buffer, data_cache[victim].data, LAINFS_BLOCK_SIZE);
    ++cache_stats.data_reads;
    return 0;
}

static int cached_write_block(uint32_t partition_index, uint32_t lba, const void *buffer) {
    uint32_t victim = 0;
    uint64_t oldest = 0xffffffffffffffffull;

    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        lainfs_data_cache_slot_t *slot = &data_cache[i];
        if (slot->valid && slot->partition_index == partition_index && slot->lba == lba) {
            mem_copy(slot->data, buffer, LAINFS_BLOCK_SIZE);
            slot->dirty = 1;
            slot->age = ++cache_clock;
            ++cache_stats.data_cache_hits;
            ++cache_stats.data_writes;
            return 0;
        }
    }

    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        if (!data_cache[i].valid) {
            victim = i;
            oldest = 0;
            break;
        }
        if (data_cache[i].age < oldest) {
            oldest = data_cache[i].age;
            victim = i;
        }
    }

    (void)oldest;
    if (flush_data_cache_slot(victim) != 0) {
        return -1;
    }

    data_cache[victim].valid = 1;
    data_cache[victim].dirty = 1;
    data_cache[victim].partition_index = partition_index;
    data_cache[victim].lba = lba;
    data_cache[victim].age = ++cache_clock;
    mem_copy(data_cache[victim].data, buffer, LAINFS_BLOCK_SIZE);
    ++cache_stats.data_writes;
    return 0;
}

static int format_partition_index(uint32_t partition_index) {
    const partition_t *part = storage_get_partition(partition_index);

    if (!part || part->block_count < 64u) {
        return -2;
    }

    if (!storage_partition_is_writable(partition_index)) {
        return -4;
    }

    if (directory_cache_valid && directory_cache_dirty && flush_directory_cache() != 0) {
        return -5;
    }

    invalidate_partition_cache(partition_index);

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
    invalidate_partition_cache(partition_index);
    return 0;
}

static int read_super(uint32_t partition_index, lainfs_superblock_t *super) {
    if (super_cache_valid && super_cache_partition == partition_index) {
        mem_copy(super, &super_cache, sizeof(*super));
        return 0;
    }

    if (raw_read_super(partition_index, &super_cache) != 0) {
        super_cache_valid = 0;
        return -1;
    }

    super_cache_valid = 1;
    super_cache_partition = partition_index;
    mem_copy(super, &super_cache, sizeof(*super));
    return 0;
}

static int load_directory_unchecked(uint32_t partition_index, const lainfs_superblock_t *super) {
    if (directory_cache_valid && directory_cache_partition == partition_index) {
        ++cache_stats.directory_cache_hits;
        return 0;
    }

    if (directory_cache_valid && directory_cache_dirty && flush_directory_cache() != 0) {
        return -1;
    }

    if (storage_read_partition(partition_index, super->dir_start_lba, super->dir_blocks, directory) != 0) {
        directory_cache_valid = 0;
        directory_cache_dirty = 0;
        return -1;
    }

    directory_cache_valid = 1;
    directory_cache_dirty = 0;
    directory_cache_partition = partition_index;
    ++cache_stats.directory_reads;
    return 0;
}

static int load_directory(uint32_t partition_index, const lainfs_superblock_t *super) {
    if (load_directory_unchecked(partition_index, super) != 0) {
        return -1;
    }

    if (validate_directory(partition_index, super, 0, 0) != 0) {
        directory_cache_valid = 0;
        directory_cache_dirty = 0;
        return -1;
    }

    return 0;
}

static int save_directory(uint32_t partition_index, const lainfs_superblock_t *super) {
    (void)super;

    directory_cache_valid = 1;
    directory_cache_dirty = 1;
    directory_cache_partition = partition_index;
    ++cache_stats.directory_writes;
    return 0;
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

static int entry_name_is_valid(const char *name) {
    uint32_t len = 0;

    while (len < 31u && name[len] != '\0') {
        ++len;
    }

    if (len >= 31u) {
        return 0;
    }

    return valid_name(name);
}

static int validate_directory_fail(uint32_t reason,
                                   uint32_t entry_id,
                                   uint32_t *out_reason,
                                   uint32_t *out_entry_id) {
    if (out_reason) {
        *out_reason = reason;
    }
    if (out_entry_id) {
        *out_entry_id = entry_id;
    }
    return -1;
}

static int find_directory_parent_cycle(uint32_t dir_id, uint32_t *out_cycle_id) {
    uint8_t seen[LAINFS_MAX_FILES];
    uint32_t current = dir_id;

    if (out_cycle_id) {
        *out_cycle_id = 0;
    }

    if (dir_id == LAINFS_ROOT_DIR || dir_id > LAINFS_MAX_FILES) {
        return 0;
    }

    mem_zero(seen, sizeof(seen));
    while (current != LAINFS_ROOT_DIR) {
        lainfs_dirent_t *entry;
        uint32_t parent_id;

        if (current == 0 || current > LAINFS_MAX_FILES) {
            return 0;
        }

        if (seen[index_from_entry_id(current)] != 0) {
            if (out_cycle_id) {
                *out_cycle_id = current;
            }
            return 1;
        }
        seen[index_from_entry_id(current)] = 1;

        entry = dir_entry(index_from_entry_id(current));
        if (entry->used != LAINFS_ENTRY_DIR) {
            return 0;
        }

        parent_id = entry_parent_id(entry);
        if (parent_id == LAINFS_ROOT_DIR) {
            return 0;
        }
        if (parent_id > LAINFS_MAX_FILES) {
            return 0;
        }
        current = parent_id;
    }

    return 0;
}

static int validate_directory(uint32_t partition_index,
                              const lainfs_superblock_t *super,
                              uint32_t *out_reason,
                              uint32_t *out_entry_id) {
    const partition_t *part = storage_get_partition(partition_index);

    if (out_reason) {
        *out_reason = LAINFS_CHECK_OK;
    }
    if (out_entry_id) {
        *out_entry_id = 0;
    }

    if (!part || !super) {
        return validate_directory_fail(LAINFS_CHECK_NO_PARTITION, 0, out_reason, out_entry_id);
    }

    if (super->dir_start_lba == 0 ||
        super->dir_blocks == 0 ||
        super->data_start_lba <= super->dir_start_lba ||
        (uint64_t)super->dir_start_lba + super->dir_blocks > super->data_start_lba ||
        super->data_start_lba >= part->block_count) {
        return validate_directory_fail(LAINFS_CHECK_BAD_LAYOUT, 0, out_reason, out_entry_id);
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        uint32_t id = entry_id_from_index(i);
        uint32_t parent_id;

        if (entry->used == 0) {
            continue;
        }

        if (entry->used != LAINFS_ENTRY_FILE && entry->used != LAINFS_ENTRY_DIR) {
            return validate_directory_fail(LAINFS_CHECK_BAD_ENTRY_TYPE, id, out_reason, out_entry_id);
        }

        if (!entry_name_is_valid(entry->name)) {
            return validate_directory_fail(LAINFS_CHECK_BAD_NAME, id, out_reason, out_entry_id);
        }

        parent_id = entry_parent_id(entry);
        if (parent_id > LAINFS_MAX_FILES || parent_id == id) {
            return validate_directory_fail(LAINFS_CHECK_BAD_PARENT, id, out_reason, out_entry_id);
        }

        if (parent_id != LAINFS_ROOT_DIR) {
            lainfs_dirent_t *parent = dir_entry(index_from_entry_id(parent_id));
            if (parent->used != LAINFS_ENTRY_DIR) {
                return validate_directory_fail(LAINFS_CHECK_BAD_PARENT, id, out_reason, out_entry_id);
            }
        }

        if (entry->used == LAINFS_ENTRY_DIR) {
            uint32_t cycle_id = 0;

            if (entry->start_lba != 0 || entry->byte_size != 0) {
                return validate_directory_fail(LAINFS_CHECK_BAD_DIRECTORY, id, out_reason, out_entry_id);
            }
            if (find_directory_parent_cycle(id, &cycle_id)) {
                return validate_directory_fail(LAINFS_CHECK_BAD_PARENT,
                                               cycle_id ? cycle_id : id,
                                               out_reason,
                                               out_entry_id);
            }
        } else {
            lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
            uint32_t extent_count = 0;
            uint32_t total_blocks = 0;

            if (entry->byte_size > LAINFS_FILE_CAPACITY) {
                return validate_directory_fail(LAINFS_CHECK_BAD_FILE_SIZE, id, out_reason, out_entry_id);
            }

            if (entry_extents(entry, extents, &extent_count) != 0) {
                return validate_directory_fail(LAINFS_CHECK_BAD_FILE_EXTENT, id, out_reason, out_entry_id);
            }

            for (uint32_t extent_index = 0; extent_index < extent_count; ++extent_index) {
                uint64_t end_lba = (uint64_t)extents[extent_index].start_lba + extents[extent_index].blocks;

                if (extents[extent_index].blocks == 0 ||
                    total_blocks > LAINFS_MAX_FILE_BLOCKS - extents[extent_index].blocks ||
                    extents[extent_index].start_lba < super->data_start_lba ||
                    end_lba > part->block_count) {
                    return validate_directory_fail(LAINFS_CHECK_BAD_FILE_EXTENT, id, out_reason, out_entry_id);
                }

                for (uint32_t other_index = extent_index + 1u; other_index < extent_count; ++other_index) {
                    if (ranges_overlap(extents[extent_index].start_lba,
                                       extents[extent_index].blocks,
                                       extents[other_index].start_lba,
                                       extents[other_index].blocks)) {
                        return validate_directory_fail(LAINFS_CHECK_OVERLAPPING_EXTENTS,
                                                       id,
                                                       out_reason,
                                                       out_entry_id);
                    }
                }

                total_blocks += extents[extent_index].blocks;
            }
        }
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *a = dir_entry(i);

        if (a->used != LAINFS_ENTRY_FILE) {
            continue;
        }

        for (uint32_t j = i + 1u; j < LAINFS_MAX_FILES; ++j) {
            lainfs_dirent_t *b = dir_entry(j);

            if (b->used != LAINFS_ENTRY_FILE) {
                continue;
            }

            lainfs_extent_t a_extents[LAINFS_MAX_EXTENTS];
            lainfs_extent_t b_extents[LAINFS_MAX_EXTENTS];
            uint32_t a_extent_count = 0;
            uint32_t b_extent_count = 0;

            if (entry_extents(a, a_extents, &a_extent_count) != 0 ||
                entry_extents(b, b_extents, &b_extent_count) != 0) {
                return validate_directory_fail(LAINFS_CHECK_BAD_FILE_EXTENT,
                                               entry_id_from_index(j),
                                               out_reason,
                                               out_entry_id);
            }

            for (uint32_t ai = 0; ai < a_extent_count; ++ai) {
                for (uint32_t bi = 0; bi < b_extent_count; ++bi) {
                    if (ranges_overlap(a_extents[ai].start_lba,
                                       a_extents[ai].blocks,
                                       b_extents[bi].start_lba,
                                       b_extents[bi].blocks)) {
                        return validate_directory_fail(LAINFS_CHECK_OVERLAPPING_EXTENTS,
                                                       entry_id_from_index(j),
                                                       out_reason,
                                                       out_entry_id);
                    }
                }
            }
        }
    }

    return 0;
}

static int repair_directory_once(uint32_t partition_index,
                                 const lainfs_superblock_t *super,
                                 uint32_t reason,
                                 uint32_t entry_id) {
    lainfs_dirent_t *entry;
    uint32_t parent_id;

    if (entry_id == 0 || entry_id > LAINFS_MAX_FILES) {
        return 0;
    }

    entry = dir_entry(index_from_entry_id(entry_id));
    if (entry->used != LAINFS_ENTRY_FILE && entry->used != LAINFS_ENTRY_DIR) {
        return 0;
    }

    if (reason == LAINFS_CHECK_BAD_PARENT) {
        parent_id = entry_parent_id(entry);
        if (parent_id > LAINFS_MAX_FILES || parent_id == entry_id) {
            set_entry_parent_id(entry, LAINFS_ROOT_DIR);
            return 1;
        }

        if (parent_id != LAINFS_ROOT_DIR && dir_entry(index_from_entry_id(parent_id))->used != LAINFS_ENTRY_DIR) {
            set_entry_parent_id(entry, LAINFS_ROOT_DIR);
            return 1;
        }

        if (entry->used == LAINFS_ENTRY_DIR && find_directory_parent_cycle(entry_id, 0)) {
            set_entry_parent_id(entry, LAINFS_ROOT_DIR);
            return 1;
        }
    }

    if (reason == LAINFS_CHECK_BAD_DIRECTORY && entry->used == LAINFS_ENTRY_DIR) {
        entry->start_lba = 0;
        entry->byte_size = 0;
        return 1;
    }

    if (reason == LAINFS_CHECK_BAD_FILE_EXTENT &&
        entry->used == LAINFS_ENTRY_FILE &&
        entry_extent_meta(entry) != 0 &&
        legacy_extent_fields_valid(partition_index, super, entry)) {
        lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
        uint32_t extent_count = 0;

        if (entry_extents(entry, extents, &extent_count) != 0) {
            extents[0].start_lba = entry->start_lba;
            extents[0].blocks = allocated_blocks_for_entry(entry);
            set_entry_extents(entry, extents, 1);
            return 1;
        }
    }

    return 0;
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

        lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
        uint32_t extent_count = 0;
        if (entry_extents(entry, extents, &extent_count) != 0) {
            return 0;
        }

        for (uint32_t extent_index = 0; extent_index < extent_count; ++extent_index) {
            if (ranges_overlap(start_lba, blocks, extents[extent_index].start_lba, extents[extent_index].blocks)) {
                return 0;
            }
        }
    }

    return 1;
}

static int find_free_extents(uint32_t partition_index,
                             const lainfs_superblock_t *super,
                             uint32_t blocks,
                             const lainfs_dirent_t *ignore,
                             lainfs_extent_t *out_extents,
                             uint32_t *out_extent_count) {
    const partition_t *part = storage_get_partition(partition_index);
    uint64_t data_end = part ? part->block_count : 0;
    uint32_t remaining = blocks;
    uint32_t extent_count = 0;

    if (!part || blocks == 0 || blocks > LAINFS_MAX_FILE_BLOCKS) {
        return -1;
    }

    for (uint64_t start = super->data_start_lba; start < data_end && remaining != 0; ++start) {
        uint32_t run = 0;

        while (start + run < data_end &&
               run < remaining &&
               extent_is_free(super, (uint32_t)(start + run), 1, ignore)) {
            ++run;
        }

        if (run == 0) {
            continue;
        }

        if (extent_count >= LAINFS_MAX_EXTENTS) {
            return -1;
        }

        out_extents[extent_count].start_lba = (uint32_t)start;
        out_extents[extent_count].blocks = run;
        ++extent_count;
        remaining -= run;
        start += run - 1u;
    }

    if (remaining != 0) {
        return -1;
    }

    *out_extent_count = extent_count;
    return 0;
}

static int clear_file_blocks(uint32_t partition_index, const lainfs_dirent_t *entry) {
    lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
    uint32_t extent_count = 0;

    if (entry_extents(entry, extents, &extent_count) != 0) {
        return -1;
    }

    mem_zero(sector, sizeof(sector));
    for (uint32_t extent_index = 0; extent_index < extent_count; ++extent_index) {
        for (uint32_t i = 0; i < extents[extent_index].blocks; ++i) {
            if (cached_write_block(partition_index, extents[extent_index].start_lba + i, sector) != 0) {
                return -1;
            }
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
    int status;
    if (!mount) {
        return -1;
    }

    lainfs_lock();
    status = format_partition_index(mount->partition_index);
    lainfs_unlock();
    return status;
}

int lainfs_format_partition(const char *partition_name) {
    uint32_t partition_index = 0;
    int status;

    if (!storage_find_partition(partition_name, &partition_index)) {
        return -1;
    }

    lainfs_lock();
    status = format_partition_index(partition_index);
    lainfs_unlock();
    return status;
}

int lainfs_format_block_device(const char *device_name, char *out_partition_name, uint32_t out_partition_name_size) {
    uint32_t device_index = 0;
    uint32_t partition_index = 0;
    const block_device_t *dev = storage_find_block_device(device_name, &device_index);
    const partition_t *part;
    int status;

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

    lainfs_lock();
    status = format_partition_index(partition_index);
    lainfs_unlock();
    return status;
}

int lainfs_flush(char drive_letter) {
    const mount_t *mount = mount_for_drive(drive_letter);
    int status;

    if (!mount) {
        return -1;
    }

    lainfs_lock();
    status = flush_partition_cache(mount->partition_index);
    lainfs_unlock();
    return status;
}

int lainfs_flush_all(void) {
    lainfs_lock();
    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        if (data_cache[i].valid && data_cache[i].dirty && flush_data_cache_slot(i) != 0) {
            lainfs_unlock();
            return -1;
        }
    }

    if (directory_cache_valid && directory_cache_dirty && flush_directory_cache() != 0) {
        lainfs_unlock();
        return -1;
    }

    lainfs_unlock();
    return 0;
}

int lainfs_check(char drive_letter, uint32_t *out_reason, uint32_t *out_entry_id) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    int status;

    if (out_reason) {
        *out_reason = LAINFS_CHECK_OK;
    }
    if (out_entry_id) {
        *out_entry_id = 0;
    }

    if (!mount) {
        if (out_reason) {
            *out_reason = LAINFS_CHECK_NO_PARTITION;
        }
        return -1;
    }

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        if (out_reason) {
            *out_reason = LAINFS_CHECK_BAD_SUPERBLOCK;
        }
        lainfs_unlock();
        return -1;
    }

    if (!(directory_cache_valid && directory_cache_partition == mount->partition_index)) {
        if (directory_cache_valid && directory_cache_dirty && flush_directory_cache() != 0) {
            if (out_reason) {
                *out_reason = LAINFS_CHECK_BAD_LAYOUT;
            }
            lainfs_unlock();
            return -1;
        }

        if (storage_read_partition(mount->partition_index,
                                   super.dir_start_lba,
                                   super.dir_blocks,
                                   directory) != 0) {
            directory_cache_valid = 0;
            directory_cache_dirty = 0;
            if (out_reason) {
                *out_reason = LAINFS_CHECK_BAD_LAYOUT;
            }
            lainfs_unlock();
            return -1;
        }

        directory_cache_valid = 1;
        directory_cache_dirty = 0;
        directory_cache_partition = mount->partition_index;
        ++cache_stats.directory_reads;
    }

    status = validate_directory(mount->partition_index, &super, out_reason, out_entry_id);
    lainfs_unlock();
    return status;
}

int lainfs_entry_detail(char drive_letter, uint32_t entry_id, lainfs_entry_detail_t *out) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    lainfs_dirent_t *entry;
    int status = 0;

    if (!mount || !out || entry_id == 0 || entry_id > LAINFS_MAX_FILES) {
        return -1;
    }

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -2;
    }

    if (load_directory_unchecked(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    entry = dir_entry(index_from_entry_id(entry_id));
    if (entry->used == 0) {
        lainfs_unlock();
        return -4;
    }

    mem_zero(out, sizeof(*out));
    out->entry_id = entry_id;
    out->type = entry->used;
    out->parent_id = entry_parent_id(entry);
    out->size = entry->byte_size;
    out->legacy_start_lba = entry->start_lba;
    out->legacy_blocks = allocated_blocks_for_entry(entry);

    for (uint32_t i = 0; i + 1u < sizeof(out->name) && entry->name[i] != '\0'; ++i) {
        out->name[i] = entry->name[i];
    }

    if (entry->used == LAINFS_ENTRY_FILE) {
        lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
        uint32_t extent_count = 0;

        if (entry_extents(entry, extents, &extent_count) == 0) {
            out->extent_valid = 1;
            out->extent_count = extent_count;
            for (uint32_t i = 0; i < extent_count; ++i) {
                out->extent_start_lba[i] = extents[i].start_lba;
                out->extent_blocks[i] = extents[i].blocks;
            }
        }
    } else if (entry->used == LAINFS_ENTRY_DIR) {
        out->extent_valid = 1;
    }

    lainfs_unlock();
    return status;
}

int lainfs_repair(char drive_letter, uint32_t *out_reason, uint32_t *out_entry_id, uint32_t *out_repairs) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    uint32_t repairs = 0;
    uint32_t reason = LAINFS_CHECK_OK;
    uint32_t entry_id = 0;

    if (out_reason) {
        *out_reason = LAINFS_CHECK_OK;
    }
    if (out_entry_id) {
        *out_entry_id = 0;
    }
    if (out_repairs) {
        *out_repairs = 0;
    }

    if (!mount) {
        if (out_reason) {
            *out_reason = LAINFS_CHECK_NO_PARTITION;
        }
        return -1;
    }

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        if (out_reason) {
            *out_reason = LAINFS_CHECK_BAD_SUPERBLOCK;
        }
        lainfs_unlock();
        return -1;
    }

    if (load_directory_unchecked(mount->partition_index, &super) != 0) {
        if (out_reason) {
            *out_reason = LAINFS_CHECK_BAD_LAYOUT;
        }
        lainfs_unlock();
        return -1;
    }

    while (validate_directory(mount->partition_index, &super, &reason, &entry_id) != 0) {
        if (!repair_directory_once(mount->partition_index, &super, reason, entry_id)) {
            if (repairs != 0 && flush_directory_cache() != 0) {
                if (out_reason) {
                    *out_reason = LAINFS_CHECK_BAD_LAYOUT;
                }
                if (out_repairs) {
                    *out_repairs = repairs;
                }
                lainfs_unlock();
                return -1;
            }
            if (out_reason) {
                *out_reason = reason;
            }
            if (out_entry_id) {
                *out_entry_id = entry_id;
            }
            if (out_repairs) {
                *out_repairs = repairs;
            }
            lainfs_unlock();
            return repairs == 0 ? -2 : -3;
        }

        directory_cache_valid = 1;
        directory_cache_dirty = 1;
        directory_cache_partition = mount->partition_index;
        ++repairs;
        if (repairs > LAINFS_MAX_FILES) {
            if (flush_directory_cache() != 0) {
                if (out_reason) {
                    *out_reason = LAINFS_CHECK_BAD_LAYOUT;
                }
                if (out_repairs) {
                    *out_repairs = repairs;
                }
                lainfs_unlock();
                return -1;
            }
            if (out_reason) {
                *out_reason = reason;
            }
            if (out_entry_id) {
                *out_entry_id = entry_id;
            }
            if (out_repairs) {
                *out_repairs = repairs;
            }
            lainfs_unlock();
            return -4;
        }
    }

    if (directory_cache_dirty && flush_directory_cache() != 0) {
        if (out_reason) {
            *out_reason = LAINFS_CHECK_BAD_LAYOUT;
        }
        if (out_repairs) {
            *out_repairs = repairs;
        }
        lainfs_unlock();
        return -1;
    }

    if (out_repairs) {
        *out_repairs = repairs;
    }
    lainfs_unlock();
    return 0;
}

void lainfs_cache_stats(lainfs_cache_stats_t *out) {
    if (out == 0) {
        return;
    }

    lainfs_lock();
    *out = cache_stats;
    out->directory_valid = directory_cache_valid ? 1u : 0u;
    out->directory_dirty = directory_cache_dirty ? 1u : 0u;
    out->data_valid = 0;
    out->data_dirty = 0;

    for (uint32_t i = 0; i < LAINFS_DATA_CACHE_SLOTS; ++i) {
        if (data_cache[i].valid) {
            ++out->data_valid;
        }
        if (data_cache[i].valid && data_cache[i].dirty) {
            ++out->data_dirty;
        }
    }
    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -2;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
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

    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
        return -7;
    }

    if (find_entry_in_dir(parent_id, name)) {
        lainfs_unlock();
        return -6;
    }

    slot = find_free_entry();
    if (!slot) {
        lainfs_unlock();
        return -5;
    }

    mem_zero(slot, sizeof(*slot));
    slot->used = LAINFS_ENTRY_DIR;
    copy_name(slot->name, name);
    set_entry_parent_id(slot, parent_id);

    if (save_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -7;
    }

    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
        return -7;
    }

    entry = find_entry_in_dir(parent_id, name);
    if (!entry) {
        lainfs_unlock();
        return -5;
    }

    if (entry->used == LAINFS_ENTRY_DIR && dir_has_children(entry_id(entry))) {
        lainfs_unlock();
        return -9;
    }

    if (entry->used == LAINFS_ENTRY_FILE) {
        if (clear_file_blocks(mount->partition_index, entry) != 0) {
            lainfs_unlock();
            return -6;
        }
    }

    mem_zero(entry, sizeof(*entry));
    if (save_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -6;
    }

    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(old_parent_id) || !valid_dir_id(new_parent_id)) {
        lainfs_unlock();
        return -7;
    }

    entry = find_entry_in_dir(old_parent_id, old_name);
    if (!entry) {
        lainfs_unlock();
        return -5;
    }

    if (entry->used == LAINFS_ENTRY_DIR &&
        (entry_id(entry) == new_parent_id || dir_is_descendant_of(new_parent_id, entry_id(entry)))) {
        lainfs_unlock();
        return -8;
    }

    if (find_entry_in_dir(new_parent_id, new_name)) {
        lainfs_unlock();
        return -6;
    }

    copy_name(entry->name, new_name);
    set_entry_parent_id(entry, new_parent_id);
    if (save_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -7;
    }

    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
        return -7;
    }

    entry = find_entry_in_dir(parent_id, name);
    if (!entry || entry->used != LAINFS_ENTRY_DIR) {
        lainfs_unlock();
        return -5;
    }

    if (out_dir_id) {
        *out_dir_id = entry_id(entry);
    }

    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(dir_id)) {
        lainfs_unlock();
        return -5;
    }

    entry = dir_entry(index_from_entry_id(dir_id));
    if (out_parent_id) {
        *out_parent_id = entry_parent_id(entry);
    }

    lainfs_unlock();
    return 0;
}

int lainfs_child_count(char drive_letter, uint32_t parent_id, uint32_t *out_count) {
    const mount_t *mount = mount_for_drive(drive_letter);
    lainfs_superblock_t super;
    uint32_t count = 0;

    if (!mount || out_count == 0) {
        return -1;
    }

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -2;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
        return -5;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (entry->used && entry_parent_id(entry) == parent_id) {
            ++count;
        }
    }

    *out_count = count;
    lainfs_unlock();
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -2;
    }

    if (load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
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
            lainfs_unlock();
            return 0;
        }

        ++seen;
    }

    lainfs_unlock();
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
    char *buffer = (char *)kmalloc(LAINFS_FILE_CAPACITY + 1u);
    uint32_t size = 0;

    if (buffer == 0) {
        return -10;
    }

    int status = lainfs_load_file_in_dir(drive_letter, parent_id, name, buffer, LAINFS_FILE_CAPACITY, &size);
    if (status != 0) {
        kfree(buffer);
        return status;
    }

    buffer[size] = '\0';

    for (uint32_t i = 0; i < size; ++i) {
        char tmp[2] = { buffer[i], '\0' };
        console_puts(tmp);
    }

    console_puts("\n");
    kfree(buffer);
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

    lainfs_lock();
    if (read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if(load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
        return -7;
    }

    slot = find_entry_in_dir(parent_id, name);
    if (slot && slot->used == LAINFS_ENTRY_DIR) {
        lainfs_unlock();
        return -8;
    }

    if(!slot) {
        for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
            lainfs_dirent_t *entry = dir_entry(i);
            if (!entry->used) {
                slot = entry;
                mem_zero(slot, sizeof(*slot));
                slot->used = LAINFS_ENTRY_FILE;
                set_entry_parent_id(slot, parent_id);
                break;
            }
        }
    }

    if(!slot) {
        lainfs_unlock();
        return -5;
    }

    uint32_t new_blocks = blocks_for_size(size);
    uint32_t required_blocks = new_blocks == 0 ? 1u : new_blocks;
    lainfs_extent_t new_extents[LAINFS_MAX_EXTENTS];
    uint32_t new_extent_count = 0;
    lainfs_dirent_t old_entry = *slot;

    if (find_free_extents(mount->partition_index,
                          &super,
                          required_blocks,
                          slot,
                          new_extents,
                          &new_extent_count) != 0) {
        lainfs_unlock();
        return -9;
    }

    if (old_entry.start_lba && clear_file_blocks(mount->partition_index, &old_entry) != 0) {
        lainfs_unlock();
        return -6;
    }

    copy_name(slot->name, name);
    slot->byte_size = size;
    set_entry_extents(slot, new_extents, new_extent_count);

    uint32_t logical_block = 0;
    for (uint32_t extent_index = 0; extent_index < new_extent_count; ++extent_index) {
        for (uint32_t block = 0; block < new_extents[extent_index].blocks; ++block) {
            uint32_t base = logical_block * LAINFS_BLOCK_SIZE;
            uint32_t bytes_left = size > base ? size - base : 0;
            uint32_t bytes_to_copy = bytes_left < LAINFS_BLOCK_SIZE ? bytes_left : LAINFS_BLOCK_SIZE;

            mem_zero(sector, sizeof(sector));
            for (uint32_t i = 0; i < bytes_to_copy; ++i) {
                sector[i] = (uint8_t)buffer[base + i];
            }

            if (cached_write_block(mount->partition_index, new_extents[extent_index].start_lba + block, sector) != 0) {
                lainfs_unlock();
                return -6;
            }
            ++logical_block;
        }
    }

    if (save_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -6;
    }

    lainfs_unlock();
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

    lainfs_lock();
    if(read_super(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -3;
    }

    if(load_directory(mount->partition_index, &super) != 0) {
        lainfs_unlock();
        return -4;
    }

    if (!valid_dir_id(parent_id)) {
        lainfs_unlock();
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
            lainfs_unlock();
            return -6;
        }

        lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
        uint32_t extent_count = 0;
        uint32_t logical_block = 0;

        if (entry_extents(entry, extents, &extent_count) != 0) {
            lainfs_unlock();
            return -7;
        }

        for (uint32_t extent_index = 0; extent_index < extent_count; ++extent_index) {
            for (uint32_t block = 0; block < extents[extent_index].blocks; ++block) {
                uint32_t base = logical_block * LAINFS_BLOCK_SIZE;
                uint32_t bytes_left = entry->byte_size > base ? entry->byte_size - base : 0;
                uint32_t bytes_to_copy = bytes_left < LAINFS_BLOCK_SIZE ? bytes_left : LAINFS_BLOCK_SIZE;

                if(cached_read_block(mount->partition_index, extents[extent_index].start_lba + block, sector) != 0) {
                    lainfs_unlock();
                    return -7;
                }

                for (uint32_t b = 0; b < bytes_to_copy; ++b) {
                    buffer[base + b] = (char)sector[b];
                }
                ++logical_block;
            }
        }

        if(out_size) {
            *out_size = entry->byte_size;
        }

        lainfs_unlock();
        return 0;
    }

    lainfs_unlock();
    return -5;
}
