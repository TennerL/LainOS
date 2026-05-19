#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAINFS_BLOCK_SIZE 512u
#define LAINFS_DIR_BLOCKS 64u
#define LAINFS_ENTRY_SIZE 64u
#define LAINFS_MAX_FILES ((LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE) / LAINFS_ENTRY_SIZE)
#define LAINFS_FILE_CAPACITY (4u * 1024u * 1024u)
#define LAINFS_MAX_FILE_BLOCKS (LAINFS_FILE_CAPACITY / LAINFS_BLOCK_SIZE)
#define LAINFS_MAX_EXTENTS 3u
#define LAINFS_EXTENT_BLOCK_MASK 0x00FFFFFFu
#define LAINFS_EXTENT_COUNT_SHIFT 24u
#define LAINFS_MAGIC0 0x4E49414Cu
#define LAINFS_MAGIC1 0x00315346u
#define LAINFS_ENTRY_FILE 1u
#define LAINFS_ENTRY_DIR 2u
#define DATA_PARTITION_START_LBA 2048u
#define DATA_PARTITION_TYPE 0x99u
#define MBR_PARTITION_TABLE_OFFSET 446u
#define MBR_SIGNATURE_OFFSET 510u
#define SMOKE_IMAGE_BLOCKS 4096u

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
    uint32_t start_lba;
    uint32_t blocks;
} lainfs_extent_t;

typedef struct {
    uint32_t reason;
    uint32_t entry_id;
} check_result_t;

enum {
    CHECK_OK = 0,
    CHECK_BAD_SUPERBLOCK = 2,
    CHECK_BAD_LAYOUT = 3,
    CHECK_BAD_ENTRY_TYPE = 4,
    CHECK_BAD_NAME = 5,
    CHECK_BAD_PARENT = 6,
    CHECK_BAD_DIRECTORY = 7,
    CHECK_BAD_FILE_SIZE = 8,
    CHECK_BAD_FILE_EXTENT = 9,
    CHECK_OVERLAPPING_EXTENTS = 10,
};

static FILE *image;
static uint32_t partition_start_lba;
static uint32_t partition_block_count;
static lainfs_superblock_t super;
static uint8_t directory[LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE];

static uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
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

static int seek_lba(uint32_t lba) {
    return fseek(image, (long)((uint64_t)(partition_start_lba + lba) * LAINFS_BLOCK_SIZE), SEEK_SET);
}

static int read_blocks(uint32_t lba, uint32_t blocks, void *buffer) {
    if (seek_lba(lba) != 0) {
        return -1;
    }
    return fread(buffer, LAINFS_BLOCK_SIZE, blocks, image) == blocks ? 0 : -1;
}

static int write_blocks(uint32_t lba, uint32_t blocks, const void *buffer) {
    if (seek_lba(lba) != 0) {
        return -1;
    }
    return fwrite(buffer, LAINFS_BLOCK_SIZE, blocks, image) == blocks ? 0 : -1;
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

static uint32_t blocks_for_size(uint32_t size) {
    return (size + LAINFS_BLOCK_SIZE - 1u) / LAINFS_BLOCK_SIZE;
}

static uint32_t allocated_blocks_for_entry(const lainfs_dirent_t *entry) {
    uint32_t blocks = blocks_for_size(entry->byte_size);
    return blocks == 0u ? 1u : blocks;
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
    return read_le32(&entry->reserved[offset]);
}

static uint32_t entry_extra_extent_blocks(const lainfs_dirent_t *entry, uint32_t extra_index) {
    uint32_t offset = 12u + extra_index * 8u;
    return read_le32(&entry->reserved[offset]);
}

static int ranges_overlap(uint32_t a_start, uint32_t a_blocks, uint32_t b_start, uint32_t b_blocks) {
    uint32_t a_end = a_start + a_blocks;
    uint32_t b_end = b_start + b_blocks;
    return a_start < b_end && b_start < a_end;
}

static int valid_name(const char *name) {
    uint32_t len = 0;
    while (len < 31u && name[len] != '\0') {
        if (name[len] == '/' || name[len] == '\\' || name[len] == ':' ||
            name[len] == ' ' || name[len] == '\t') {
            return 0;
        }
        ++len;
    }
    return len != 0 && len < 31u;
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

static int load_image(const char *path, int writable) {
    uint8_t mbr[LAINFS_BLOCK_SIZE];
    uint8_t *part = &mbr[MBR_PARTITION_TABLE_OFFSET];

    image = fopen(path, writable ? "r+b" : "rb");
    if (!image) {
        perror(path);
        return -1;
    }

    partition_start_lba = 0;
    if (fread(mbr, 1, sizeof(mbr), image) != sizeof(mbr)) {
        return -1;
    }
    if (mbr[MBR_SIGNATURE_OFFSET] == 0x55u &&
        mbr[MBR_SIGNATURE_OFFSET + 1u] == 0xAAu &&
        part[4] == DATA_PARTITION_TYPE) {
        partition_start_lba = read_le32(&part[8]);
        partition_block_count = read_le32(&part[12]);
    } else {
        if (fseek(image, 0, SEEK_END) != 0) {
            return -1;
        }
        partition_block_count = (uint32_t)((uint64_t)ftell(image) / LAINFS_BLOCK_SIZE);
        if (fseek(image, 0, SEEK_SET) != 0) {
            return -1;
        }
    }

    if (read_blocks(0, 1, &super) != 0 ||
        super.magic0 != LAINFS_MAGIC0 ||
        super.magic1 != LAINFS_MAGIC1 ||
        super.version != 1u ||
        super.block_size != LAINFS_BLOCK_SIZE ||
        super.dir_blocks != LAINFS_DIR_BLOCKS ||
        super.max_files != LAINFS_MAX_FILES) {
        return -1;
    }

    if (read_blocks(super.dir_start_lba, super.dir_blocks, directory) != 0) {
        return -1;
    }

    return 0;
}

static int save_directory(void) {
    return write_blocks(super.dir_start_lba, super.dir_blocks, directory);
}

static int fail(check_result_t *result, uint32_t reason, uint32_t entry_id) {
    result->reason = reason;
    result->entry_id = entry_id;
    return -1;
}

static int find_parent_cycle(uint32_t dir_id, uint32_t *out_cycle_id) {
    uint8_t seen[LAINFS_MAX_FILES];
    uint32_t current = dir_id;

    memset(seen, 0, sizeof(seen));
    while (current != 0) {
        lainfs_dirent_t *entry;
        uint32_t parent_id;

        if (current > LAINFS_MAX_FILES) {
            return 0;
        }
        if (seen[index_from_entry_id(current)] != 0) {
            *out_cycle_id = current;
            return 1;
        }
        seen[index_from_entry_id(current)] = 1;
        entry = dir_entry(index_from_entry_id(current));
        if (entry->used != LAINFS_ENTRY_DIR) {
            return 0;
        }
        parent_id = entry_parent_id(entry);
        if (parent_id > LAINFS_MAX_FILES) {
            return 0;
        }
        current = parent_id;
    }
    return 0;
}

static int check_directory(check_result_t *result) {
    result->reason = CHECK_OK;
    result->entry_id = 0;

    if (super.dir_start_lba == 0 ||
        super.dir_blocks == 0 ||
        super.data_start_lba <= super.dir_start_lba ||
        (uint64_t)super.dir_start_lba + super.dir_blocks > super.data_start_lba ||
        super.data_start_lba >= partition_block_count) {
        return fail(result, CHECK_BAD_LAYOUT, 0);
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        uint32_t id = entry_id_from_index(i);
        uint32_t parent_id;

        if (entry->used == 0) {
            continue;
        }
        if (entry->used != LAINFS_ENTRY_FILE && entry->used != LAINFS_ENTRY_DIR) {
            return fail(result, CHECK_BAD_ENTRY_TYPE, id);
        }
        if (!valid_name(entry->name)) {
            return fail(result, CHECK_BAD_NAME, id);
        }
        parent_id = entry_parent_id(entry);
        if (parent_id > LAINFS_MAX_FILES || parent_id == id) {
            return fail(result, CHECK_BAD_PARENT, id);
        }
        if (parent_id != 0 && dir_entry(index_from_entry_id(parent_id))->used != LAINFS_ENTRY_DIR) {
            return fail(result, CHECK_BAD_PARENT, id);
        }

        if (entry->used == LAINFS_ENTRY_DIR) {
            uint32_t cycle_id = 0;
            if (entry->start_lba != 0 || entry->byte_size != 0) {
                return fail(result, CHECK_BAD_DIRECTORY, id);
            }
            if (find_parent_cycle(id, &cycle_id)) {
                return fail(result, CHECK_BAD_PARENT, cycle_id ? cycle_id : id);
            }
        } else {
            lainfs_extent_t extents[LAINFS_MAX_EXTENTS];
            uint32_t extent_count = 0;
            uint32_t total_blocks = 0;

            if (entry->byte_size > LAINFS_FILE_CAPACITY ||
                entry_extents(entry, extents, &extent_count) != 0) {
                return fail(result, CHECK_BAD_FILE_EXTENT, id);
            }
            for (uint32_t e = 0; e < extent_count; ++e) {
                uint64_t end_lba = (uint64_t)extents[e].start_lba + extents[e].blocks;
                if (extents[e].blocks == 0 ||
                    total_blocks > LAINFS_MAX_FILE_BLOCKS - extents[e].blocks ||
                    extents[e].start_lba < super.data_start_lba ||
                    end_lba > partition_block_count) {
                    return fail(result, CHECK_BAD_FILE_EXTENT, id);
                }
                for (uint32_t other = e + 1u; other < extent_count; ++other) {
                    if (ranges_overlap(extents[e].start_lba, extents[e].blocks,
                                       extents[other].start_lba, extents[other].blocks)) {
                        return fail(result, CHECK_OVERLAPPING_EXTENTS, id);
                    }
                }
                total_blocks += extents[e].blocks;
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
            lainfs_extent_t ae[LAINFS_MAX_EXTENTS];
            lainfs_extent_t be[LAINFS_MAX_EXTENTS];
            uint32_t ac = 0;
            uint32_t bc = 0;

            if (b->used != LAINFS_ENTRY_FILE) {
                continue;
            }
            if (entry_extents(a, ae, &ac) != 0 || entry_extents(b, be, &bc) != 0) {
                return fail(result, CHECK_BAD_FILE_EXTENT, entry_id_from_index(j));
            }
            for (uint32_t ai = 0; ai < ac; ++ai) {
                for (uint32_t bi = 0; bi < bc; ++bi) {
                    if (ranges_overlap(ae[ai].start_lba, ae[ai].blocks, be[bi].start_lba, be[bi].blocks)) {
                        return fail(result, CHECK_OVERLAPPING_EXTENTS, entry_id_from_index(j));
                    }
                }
            }
        }
    }

    return 0;
}

static int legacy_fields_valid(const lainfs_dirent_t *entry) {
    uint32_t blocks = allocated_blocks_for_entry(entry);
    uint64_t end_lba = (uint64_t)entry->start_lba + blocks;
    return entry->used == LAINFS_ENTRY_FILE &&
           entry->byte_size <= LAINFS_FILE_CAPACITY &&
           blocks <= LAINFS_MAX_FILE_BLOCKS &&
           entry->start_lba >= super.data_start_lba &&
           end_lba <= partition_block_count;
}

static int repair_once(const check_result_t *result) {
    lainfs_dirent_t *entry;
    uint32_t parent_id;

    if (result->entry_id == 0 || result->entry_id > LAINFS_MAX_FILES) {
        return 0;
    }

    entry = dir_entry(index_from_entry_id(result->entry_id));
    if (entry->used != LAINFS_ENTRY_FILE && entry->used != LAINFS_ENTRY_DIR) {
        return 0;
    }

    if (result->reason == CHECK_BAD_PARENT) {
        parent_id = entry_parent_id(entry);
        if (parent_id > LAINFS_MAX_FILES || parent_id == result->entry_id ||
            (parent_id != 0 && dir_entry(index_from_entry_id(parent_id))->used != LAINFS_ENTRY_DIR) ||
            (entry->used == LAINFS_ENTRY_DIR && find_parent_cycle(result->entry_id, &parent_id))) {
            set_entry_parent_id(entry, 0);
            return 1;
        }
    }

    if (result->reason == CHECK_BAD_DIRECTORY && entry->used == LAINFS_ENTRY_DIR) {
        entry->start_lba = 0;
        entry->byte_size = 0;
        return 1;
    }

    if (result->reason == CHECK_BAD_FILE_EXTENT &&
        entry_extent_meta(entry) != 0 &&
        legacy_fields_valid(entry)) {
        set_entry_extent_meta(entry, 0);
        memset(&entry->reserved[8], 0, 16u);
        return 1;
    }

    return 0;
}

static const char *reason_text(uint32_t reason) {
    switch (reason) {
        case CHECK_OK: return "ok";
        case CHECK_BAD_SUPERBLOCK: return "bad superblock";
        case CHECK_BAD_LAYOUT: return "bad layout";
        case CHECK_BAD_ENTRY_TYPE: return "bad entry type";
        case CHECK_BAD_NAME: return "bad name";
        case CHECK_BAD_PARENT: return "bad parent";
        case CHECK_BAD_DIRECTORY: return "bad directory";
        case CHECK_BAD_FILE_SIZE: return "bad file size";
        case CHECK_BAD_FILE_EXTENT: return "bad file extent";
        case CHECK_OVERLAPPING_EXTENTS: return "overlapping extents";
        default: return "unknown";
    }
}

static int command_check(const char *path) {
    check_result_t result;
    if (load_image(path, 0) != 0) {
        fprintf(stderr, "lainfs_check_host: could not load %s\n", path);
        return 1;
    }
    if (check_directory(&result) == 0) {
        printf("%s: ok\n", path);
        return 0;
    }
    printf("%s: %s entry=%u\n", path, reason_text(result.reason), result.entry_id);
    return 1;
}

static int command_repair(const char *path) {
    check_result_t result;
    uint32_t repairs = 0;

    if (load_image(path, 1) != 0) {
        fprintf(stderr, "lainfs_check_host: could not load %s\n", path);
        return 1;
    }

    while (check_directory(&result) != 0) {
        if (!repair_once(&result)) {
            printf("%s: partial repairs=%u remaining=%s entry=%u\n",
                   path, repairs, reason_text(result.reason), result.entry_id);
            return 1;
        }
        ++repairs;
        if (repairs > LAINFS_MAX_FILES) {
            fprintf(stderr, "lainfs_check_host: too many repairs\n");
            return 1;
        }
    }

    if (repairs != 0 && save_directory() != 0) {
        fprintf(stderr, "lainfs_check_host: could not save repairs\n");
        return 1;
    }

    printf("%s: ok repairs=%u\n", path, repairs);
    return 0;
}

static void init_empty_image_state(void) {
    partition_start_lba = DATA_PARTITION_START_LBA;
    partition_block_count = SMOKE_IMAGE_BLOCKS - DATA_PARTITION_START_LBA;
    memset(&super, 0, sizeof(super));
    super.magic0 = LAINFS_MAGIC0;
    super.magic1 = LAINFS_MAGIC1;
    super.version = 1u;
    super.block_size = LAINFS_BLOCK_SIZE;
    super.dir_start_lba = 1u;
    super.dir_blocks = LAINFS_DIR_BLOCKS;
    super.data_start_lba = 1u + LAINFS_DIR_BLOCKS;
    super.max_files = LAINFS_MAX_FILES;
    memset(directory, 0, sizeof(directory));
}

static int create_smoke_image(const char *path) {
    uint8_t zero[LAINFS_BLOCK_SIZE];
    uint8_t mbr[LAINFS_BLOCK_SIZE];
    uint8_t *part = &mbr[MBR_PARTITION_TABLE_OFFSET];

    image = fopen(path, "w+b");
    if (!image) {
        perror(path);
        return -1;
    }
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < SMOKE_IMAGE_BLOCKS; ++i) {
        if (fwrite(zero, 1, sizeof(zero), image) != sizeof(zero)) {
            return -1;
        }
    }

    memset(mbr, 0, sizeof(mbr));
    part[4] = DATA_PARTITION_TYPE;
    write_le32(&part[8], DATA_PARTITION_START_LBA);
    write_le32(&part[12], SMOKE_IMAGE_BLOCKS - DATA_PARTITION_START_LBA);
    mbr[MBR_SIGNATURE_OFFSET] = 0x55u;
    mbr[MBR_SIGNATURE_OFFSET + 1u] = 0xAAu;
    if (fseek(image, 0, SEEK_SET) != 0 || fwrite(mbr, 1, sizeof(mbr), image) != sizeof(mbr)) {
        return -1;
    }

    init_empty_image_state();
    if (write_blocks(0, 1, &super) != 0 || save_directory() != 0) {
        return -1;
    }
    fclose(image);
    image = NULL;
    return 0;
}

static int prepare_corruption(const char *path, uint32_t kind) {
    if (load_image(path, 1) != 0) {
        return -1;
    }

    memset(directory, 0, sizeof(directory));
    lainfs_dirent_t *a = dir_entry(0);
    lainfs_dirent_t *b = dir_entry(1);
    lainfs_dirent_t *f = dir_entry(2);
    lainfs_dirent_t *g = dir_entry(3);

    a->used = LAINFS_ENTRY_DIR;
    strcpy(a->name, "a");
    b->used = LAINFS_ENTRY_DIR;
    strcpy(b->name, "b");
    f->used = LAINFS_ENTRY_FILE;
    strcpy(f->name, "f");
    f->start_lba = super.data_start_lba;
    f->byte_size = 16u;

    if (kind == 0) {
        set_entry_parent_id(a, 2u);
        set_entry_parent_id(b, 1u);
        set_entry_parent_id(f, 0);
    } else if (kind == 1) {
        a->start_lba = 99u;
        a->byte_size = 7u;
        set_entry_parent_id(a, 0);
        set_entry_parent_id(b, 0);
        set_entry_parent_id(f, 0);
    } else if (kind == 2) {
        set_entry_parent_id(a, 0);
        set_entry_parent_id(b, 0);
        set_entry_parent_id(f, 0);
        set_entry_extent_meta(f, 2u << LAINFS_EXTENT_COUNT_SHIFT);
    } else if (kind == 3) {
        g->used = LAINFS_ENTRY_FILE;
        strcpy(g->name, "g");
        g->start_lba = f->start_lba;
        g->byte_size = f->byte_size;
        set_entry_parent_id(a, 0);
        set_entry_parent_id(b, 0);
        set_entry_parent_id(f, 0);
        set_entry_parent_id(g, 0);
    } else {
        f->start_lba = partition_block_count;
        f->byte_size = LAINFS_BLOCK_SIZE;
        set_entry_parent_id(a, 0);
        set_entry_parent_id(b, 0);
        set_entry_parent_id(f, 0);
    }

    if (save_directory() != 0) {
        return -1;
    }
    fclose(image);
    image = NULL;
    return 0;
}

static int smoke_case(const char *path, uint32_t kind, const char *name) {
    check_result_t result;

    if (create_smoke_image(path) != 0 || prepare_corruption(path, kind) != 0) {
        fprintf(stderr, "smoke %s: setup failed\n", name);
        return 1;
    }
    if (load_image(path, 0) != 0 || check_directory(&result) == 0) {
        fprintf(stderr, "smoke %s: expected corruption\n", name);
        return 1;
    }
    fclose(image);
    image = NULL;
    if (command_repair(path) != 0 || load_image(path, 0) != 0 || check_directory(&result) != 0) {
        fprintf(stderr, "smoke %s: repair failed\n", name);
        return 1;
    }
    fclose(image);
    image = NULL;
    printf("smoke %s: ok\n", name);
    return 0;
}

static int smoke_unrepairable_case(const char *path, uint32_t kind, const char *name, uint32_t expected_reason) {
    check_result_t result;

    if (create_smoke_image(path) != 0 || prepare_corruption(path, kind) != 0) {
        fprintf(stderr, "smoke %s: setup failed\n", name);
        return 1;
    }
    if (load_image(path, 0) != 0 || check_directory(&result) == 0) {
        fprintf(stderr, "smoke %s: expected corruption\n", name);
        return 1;
    }
    if (result.reason != expected_reason) {
        fprintf(stderr, "smoke %s: expected %s got %s entry=%u\n",
                name, reason_text(expected_reason), reason_text(result.reason), result.entry_id);
        return 1;
    }
    fclose(image);
    image = NULL;

    if (command_repair(path) == 0) {
        fprintf(stderr, "smoke %s: repair unexpectedly succeeded\n", name);
        return 1;
    }
    if (load_image(path, 0) != 0 || check_directory(&result) == 0 || result.reason != expected_reason) {
        fprintf(stderr, "smoke %s: expected remaining %s\n", name, reason_text(expected_reason));
        return 1;
    }
    fclose(image);
    image = NULL;
    printf("smoke %s: ok\n", name);
    return 0;
}

static int command_smoke(void) {
    int status = 0;
    status |= smoke_case("build/lainfs-smoke-cycle.img", 0, "parent-cycle");
    status |= smoke_case("build/lainfs-smoke-dir.img", 1, "directory-metadata");
    status |= smoke_case("build/lainfs-smoke-extent.img", 2, "extent-collapse");
    status |= smoke_unrepairable_case("build/lainfs-smoke-overlap.img",
                                      3,
                                      "overlapping-extents",
                                      CHECK_OVERLAPPING_EXTENTS);
    status |= smoke_unrepairable_case("build/lainfs-smoke-out-of-bounds.img",
                                      4,
                                      "out-of-bounds-extent",
                                      CHECK_BAD_FILE_EXTENT);
    return status == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int status;

    if (argc == 2 && strcmp(argv[1], "smoke") == 0) {
        return command_smoke();
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s check|repair image\n       %s smoke\n", argv[0], argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "check") == 0) {
        status = command_check(argv[2]);
    } else if (strcmp(argv[1], "repair") == 0) {
        status = command_repair(argv[2]);
    } else {
        fprintf(stderr, "usage: %s check|repair image\n       %s smoke\n", argv[0], argv[0]);
        return 2;
    }

    if (image) {
        fclose(image);
    }
    return status;
}
