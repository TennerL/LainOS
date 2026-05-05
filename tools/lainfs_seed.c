#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LAINFS_BLOCK_SIZE 512u
#define LAINFS_DIR_BLOCKS 64u
#define LAINFS_ENTRY_SIZE 64u
#define LAINFS_MAX_FILES ((LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE) / LAINFS_ENTRY_SIZE)
#define LAINFS_FILE_CAPACITY 524288u
#define LAINFS_MAX_FILE_BLOCKS (LAINFS_FILE_CAPACITY / LAINFS_BLOCK_SIZE)
#define LAINFS_MAGIC0 0x4E49414Cu
#define LAINFS_MAGIC1 0x00315346u
#define LAINFS_ENTRY_FILE 1u
#define LAINFS_ENTRY_DIR 2u
#define DATA_PARTITION_START_LBA 2048u
#define DATA_PARTITION_TYPE 0x99u
#define MBR_PARTITION_TABLE_OFFSET 446u
#define MBR_SIGNATURE_OFFSET 510u

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

static FILE *image;
static uint64_t partition_start_lba;
static uint64_t partition_block_count;
static lainfs_superblock_t super;
static uint8_t directory[LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE];
static uint8_t sector[LAINFS_BLOCK_SIZE];

static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int seek_lba(uint64_t lba) {
    uint64_t offset = lba * LAINFS_BLOCK_SIZE;
    return fseek(image, (long)offset, SEEK_SET);
}

static int read_blocks(uint64_t lba, uint32_t blocks, void *buffer) {
    if (seek_lba(partition_start_lba + lba) != 0) {
        return -1;
    }
    return fread(buffer, LAINFS_BLOCK_SIZE, blocks, image) == blocks ? 0 : -1;
}

static int write_blocks(uint64_t lba, uint32_t blocks, const void *buffer) {
    if (seek_lba(partition_start_lba + lba) != 0) {
        return -1;
    }
    return fwrite(buffer, LAINFS_BLOCK_SIZE, blocks, image) == blocks ? 0 : -1;
}

static lainfs_dirent_t *dir_entry(uint32_t index) {
    return (lainfs_dirent_t *)&directory[index * LAINFS_ENTRY_SIZE];
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

static uint32_t entry_id(const lainfs_dirent_t *entry) {
    return (uint32_t)(((const uint8_t *)entry - directory) / LAINFS_ENTRY_SIZE) + 1u;
}

static uint32_t blocks_for_size(uint32_t size) {
    return (size + LAINFS_BLOCK_SIZE - 1u) / LAINFS_BLOCK_SIZE;
}

static uint32_t allocated_blocks_for_entry(const lainfs_dirent_t *entry) {
    uint32_t blocks = blocks_for_size(entry->byte_size);
    return blocks == 0u ? 1u : blocks;
}

static int ranges_overlap(uint32_t a_start, uint32_t a_blocks, uint32_t b_start, uint32_t b_blocks) {
    uint32_t a_end = a_start + a_blocks;
    uint32_t b_end = b_start + b_blocks;
    return a_start < b_end && b_start < a_end;
}

static int valid_name(const char *name) {
    size_t len = strlen(name);

    if (len == 0 || len > 30u) {
        return 0;
    }

    for (size_t i = 0; i < len; ++i) {
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':' ||
            name[i] == ' ' || name[i] == '\t') {
            return 0;
        }
    }

    return 1;
}

static void copy_name(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] && i < 30u) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static lainfs_dirent_t *find_entry_in_dir(uint32_t parent_id, const char *name) {
    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (entry->used && entry_parent_id(entry) == parent_id && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static lainfs_dirent_t *find_free_entry(void) {
    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (!entry->used) {
            return entry;
        }
    }
    return NULL;
}

static int extent_is_free(uint32_t start_lba, uint32_t blocks) {
    if (blocks == 0 || start_lba < super.data_start_lba) {
        return 0;
    }

    for (uint32_t i = 0; i < LAINFS_MAX_FILES; ++i) {
        lainfs_dirent_t *entry = dir_entry(i);
        if (entry->used != LAINFS_ENTRY_FILE) {
            continue;
        }
        if (ranges_overlap(start_lba, blocks, entry->start_lba, allocated_blocks_for_entry(entry))) {
            return 0;
        }
    }

    return 1;
}

static int find_free_extent(uint32_t blocks, uint32_t *out_start_lba) {
    if (blocks == 0 || blocks > LAINFS_MAX_FILE_BLOCKS) {
        return -1;
    }

    for (uint64_t start = super.data_start_lba; start + blocks <= partition_block_count; ++start) {
        if (extent_is_free((uint32_t)start, blocks)) {
            *out_start_lba = (uint32_t)start;
            return 0;
        }
    }

    return -1;
}

static int save_directory(void) {
    return write_blocks(super.dir_start_lba, super.dir_blocks, directory);
}

static int make_dir(uint32_t parent_id, const char *name, uint32_t *out_id) {
    lainfs_dirent_t *entry;

    if (!valid_name(name)) {
        fprintf(stderr, "lainfs_seed: invalid directory name '%s'\n", name);
        return -1;
    }

    entry = find_entry_in_dir(parent_id, name);
    if (entry) {
        if (entry->used != LAINFS_ENTRY_DIR) {
            fprintf(stderr, "lainfs_seed: '%s' already exists as a file\n", name);
            return -1;
        }
        *out_id = entry_id(entry);
        return 0;
    }

    entry = find_free_entry();
    if (!entry) {
        fprintf(stderr, "lainfs_seed: directory table is full\n");
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    entry->used = LAINFS_ENTRY_DIR;
    copy_name(entry->name, name);
    set_entry_parent_id(entry, parent_id);
    *out_id = entry_id(entry);
    return 0;
}

static int save_file(uint32_t parent_id, const char *name, const uint8_t *data, uint32_t size) {
    lainfs_dirent_t *entry;
    uint32_t required_blocks = blocks_for_size(size);
    uint32_t start_lba = 0;

    if (required_blocks == 0u) {
        required_blocks = 1u;
    }

    if (!valid_name(name)) {
        fprintf(stderr, "lainfs_seed: invalid file name '%s'\n", name);
        return -1;
    }
    if (size > LAINFS_FILE_CAPACITY) {
        fprintf(stderr, "lainfs_seed: %s is too large for lainfs (%u bytes)\n", name, size);
        return -1;
    }
    if (find_entry_in_dir(parent_id, name)) {
        fprintf(stderr, "lainfs_seed: duplicate entry '%s'\n", name);
        return -1;
    }

    entry = find_free_entry();
    if (!entry) {
        fprintf(stderr, "lainfs_seed: directory table is full\n");
        return -1;
    }
    if (find_free_extent(required_blocks, &start_lba) != 0) {
        fprintf(stderr, "lainfs_seed: partition is full while writing %s\n", name);
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    entry->used = LAINFS_ENTRY_FILE;
    copy_name(entry->name, name);
    entry->start_lba = start_lba;
    entry->byte_size = size;
    set_entry_parent_id(entry, parent_id);

    for (uint32_t block = 0; block < required_blocks; ++block) {
        uint32_t base = block * LAINFS_BLOCK_SIZE;
        uint32_t left = size > base ? size - base : 0u;
        uint32_t chunk = left < LAINFS_BLOCK_SIZE ? left : LAINFS_BLOCK_SIZE;

        memset(sector, 0, sizeof(sector));
        if (chunk != 0u) {
            memcpy(sector, data + base, chunk);
        }
        if (write_blocks(start_lba + block, 1, sector) != 0) {
            fprintf(stderr, "lainfs_seed: could not write %s\n", name);
            return -1;
        }
    }

    return 0;
}

static const char *base_name(const char *path) {
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static int read_host_file(const char *path, uint8_t **out_data, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (!f) {
        fprintf(stderr, "lainfs_seed: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "lainfs_seed: cannot measure %s\n", path);
        return -1;
    }
    if ((uint64_t)size > LAINFS_FILE_CAPACITY) {
        fclose(f);
        fprintf(stderr, "lainfs_seed: %s is too large for lainfs (%ld bytes)\n", path, size);
        return -1;
    }

    data = malloc((size_t)size == 0u ? 1u : (size_t)size);
    if (!data) {
        fclose(f);
        fprintf(stderr, "lainfs_seed: out of memory\n");
        return -1;
    }
    if (size != 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        fprintf(stderr, "lainfs_seed: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    *out_data = data;
    *out_size = (uint32_t)size;
    return 0;
}

static int import_path_at(uint32_t parent_id, const char *host_path);

static int import_directory_contents(uint32_t parent_id, const char *host_path) {
    DIR *dir = opendir(host_path);
    struct dirent *ent;
    int status = 0;

    if (!dir) {
        fprintf(stderr, "lainfs_seed: cannot open directory %s: %s\n", host_path, strerror(errno));
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        char child[512];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", host_path, ent->d_name) >= (int)sizeof(child)) {
            fprintf(stderr, "lainfs_seed: path too long under %s\n", host_path);
            status = -1;
            break;
        }
        if (import_path_at(parent_id, child) != 0) {
            status = -1;
            break;
        }
    }

    closedir(dir);
    return status;
}

static int import_path_at(uint32_t parent_id, const char *host_path) {
    struct stat st;
    const char *name = base_name(host_path);

    if (stat(host_path, &st) != 0) {
        fprintf(stderr, "lainfs_seed: cannot stat %s: %s\n", host_path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        uint32_t dir_id = 0;
        if (make_dir(parent_id, name, &dir_id) != 0) {
            return -1;
        }
        return import_directory_contents(dir_id, host_path);
    }

    if (S_ISREG(st.st_mode)) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        int status;

        if (read_host_file(host_path, &data, &size) != 0) {
            return -1;
        }
        status = save_file(parent_id, name, data, size);
        free(data);
        return status;
    }

    return 0;
}

static int import_default_tree(void) {
    const char *paths[] = {
        "Makefile",
        "README.md",
        "SELFHOSTING.md",
        "boot",
        "kernel",
        "examples",
    };

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (import_path_at(0, paths[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static int create_mbr(void) {
    uint8_t mbr[LAINFS_BLOCK_SIZE];
    uint64_t image_size;
    uint32_t block_count;
    uint8_t *entry = &mbr[MBR_PARTITION_TABLE_OFFSET];

    if (fseek(image, 0, SEEK_END) != 0 || (image_size = (uint64_t)ftell(image)) < LAINFS_BLOCK_SIZE) {
        fprintf(stderr, "lainfs_seed: could not measure image\n");
        return -1;
    }
    if (image_size / LAINFS_BLOCK_SIZE <= DATA_PARTITION_START_LBA) {
        fprintf(stderr, "lainfs_seed: image is too small\n");
        return -1;
    }

    partition_start_lba = DATA_PARTITION_START_LBA;
    partition_block_count = image_size / LAINFS_BLOCK_SIZE - partition_start_lba;
    if (partition_block_count > 0xFFFFFFFFu) {
        fprintf(stderr, "lainfs_seed: image is too large for MBR seed tool\n");
        return -1;
    }
    block_count = (uint32_t)partition_block_count;

    memset(mbr, 0, sizeof(mbr));
    entry[4] = DATA_PARTITION_TYPE;
    write_le32(&entry[8], (uint32_t)partition_start_lba);
    write_le32(&entry[12], block_count);
    mbr[MBR_SIGNATURE_OFFSET] = 0x55u;
    mbr[MBR_SIGNATURE_OFFSET + 1u] = 0xAAu;

    if (fseek(image, 0, SEEK_SET) != 0 || fwrite(mbr, 1, sizeof(mbr), image) != sizeof(mbr)) {
        fprintf(stderr, "lainfs_seed: could not write MBR\n");
        return -1;
    }

    return 0;
}

static int format_lainfs(void) {
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
    if (write_blocks(0, 1, &super) != 0 || save_directory() != 0) {
        fprintf(stderr, "lainfs_seed: could not format lainfs\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    const char *image_path;
    int status = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s image [paths...]\n", argv[0]);
        return 2;
    }

    image_path = argv[1];
    image = fopen(image_path, "r+b");
    if (!image) {
        fprintf(stderr, "lainfs_seed: cannot open %s: %s\n", image_path, strerror(errno));
        return 1;
    }

    if (create_mbr() != 0 || format_lainfs() != 0) {
        fclose(image);
        return 1;
    }

    if (argc == 2) {
        status = import_default_tree();
    } else {
        for (int i = 2; i < argc; ++i) {
            if (import_path_at(0, argv[i]) != 0) {
                status = -1;
                break;
            }
        }
    }

    if (status == 0 && save_directory() != 0) {
        fprintf(stderr, "lainfs_seed: could not save directory\n");
        status = -1;
    }

    fclose(image);
    return status == 0 ? 0 : 1;
}
