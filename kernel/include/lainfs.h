#ifndef LAINFS_H
#define LAINFS_H

#include <stdint.h>

#define LAINFS_ROOT_DIR 0u
#define LAINFS_FILE_CAPACITY (4u * 1024u * 1024u)
#define LAINFS_ENTRY_TYPE_FILE 1u
#define LAINFS_ENTRY_TYPE_DIR 2u
#define LAINFS_MAX_EXTENTS 3u

/*
 * LainFS v1 dirent reserved bytes are now part of the on-disk format:
 * bytes 0..3 hold the parent entry id, bytes 4..7 hold extent metadata,
 * and bytes 8..23 hold up to two extra extents as little-endian
 * start_lba/block_count pairs. A zero extent metadata word means the legacy
 * contiguous start_lba + byte_size layout is used. Otherwise, metadata bits
 * 31..24 hold the extent count and bits 23..0 hold extent 0's block count.
 */

#define LAINFS_CHECK_OK 0u
#define LAINFS_CHECK_NO_PARTITION 1u
#define LAINFS_CHECK_BAD_SUPERBLOCK 2u
#define LAINFS_CHECK_BAD_LAYOUT 3u
#define LAINFS_CHECK_BAD_ENTRY_TYPE 4u
#define LAINFS_CHECK_BAD_NAME 5u
#define LAINFS_CHECK_BAD_PARENT 6u
#define LAINFS_CHECK_BAD_DIRECTORY 7u
#define LAINFS_CHECK_BAD_FILE_SIZE 8u
#define LAINFS_CHECK_BAD_FILE_EXTENT 9u
#define LAINFS_CHECK_OVERLAPPING_EXTENTS 10u

typedef struct {
    uint32_t directory_valid;
    uint32_t directory_dirty;
    uint32_t data_valid;
    uint32_t data_dirty;
    uint64_t directory_reads;
    uint64_t directory_writes;
    uint64_t directory_cache_hits;
    uint64_t data_reads;
    uint64_t data_writes;
    uint64_t data_cache_hits;
    uint64_t flushes;
} lainfs_cache_stats_t;

typedef struct {
    uint32_t entry_id;
    uint32_t type;
    uint32_t parent_id;
    uint32_t size;
    uint32_t extent_valid;
    uint32_t extent_count;
    uint32_t extent_start_lba[LAINFS_MAX_EXTENTS];
    uint32_t extent_blocks[LAINFS_MAX_EXTENTS];
    uint32_t legacy_start_lba;
    uint32_t legacy_blocks;
    char name[32];
} lainfs_entry_detail_t;

int lainfs_format(char drive_letter);
int lainfs_format_partition(const char *partition_name);
int lainfs_format_block_device(const char *device_name, char *out_partition_name, uint32_t out_partition_name_size);
int lainfs_flush(char drive_letter);
int lainfs_flush_all(void);
int lainfs_check(char drive_letter, uint32_t *out_reason, uint32_t *out_entry_id);
int lainfs_repair(char drive_letter, uint32_t *out_reason, uint32_t *out_entry_id, uint32_t *out_repairs);
int lainfs_entry_detail(char drive_letter, uint32_t entry_id, lainfs_entry_detail_t *out);
void lainfs_cache_stats(lainfs_cache_stats_t *out);
int lainfs_list(char drive_letter);
int lainfs_list_dir(char drive_letter, uint32_t parent_id);
int lainfs_make_dir(char drive_letter, const char *name);
int lainfs_make_dir_in_dir(char drive_letter, uint32_t parent_id, const char *name);
int lainfs_delete(char drive_letter, const char *name);
int lainfs_delete_in_dir(char drive_letter, uint32_t parent_id, const char *name);
int lainfs_rename(char drive_letter, const char *old_name, const char *new_name);
int lainfs_rename_in_dir(char drive_letter,
                         uint32_t old_parent_id,
                         const char *old_name,
                         uint32_t new_parent_id,
                         const char *new_name);
int lainfs_find_dir(char drive_letter, uint32_t parent_id, const char *name, uint32_t *out_dir_id);
int lainfs_parent_dir(char drive_letter, uint32_t dir_id, uint32_t *out_parent_id);
int lainfs_child_count(char drive_letter, uint32_t parent_id, uint32_t *out_count);
int lainfs_child_info(char drive_letter,
                      uint32_t parent_id,
                      uint32_t child_index,
                      char *out_name,
                      uint32_t out_name_size,
                      uint32_t *out_type,
                      uint32_t *out_size);
int lainfs_write_file(char drive_letter, const char *name, const char *text);
int lainfs_write_file_in_dir(char drive_letter, uint32_t parent_id, const char *name, const char *text);
int lainfs_read_file(char drive_letter, const char *name);
int lainfs_read_file_in_dir(char drive_letter, uint32_t parent_id, const char *name);
int lainfs_load_file(char drive_letter,
                     const char *name,
                     char *buffer,
                     uint32_t buffer_size,
                     uint32_t *out_size);
int lainfs_load_file_in_dir(char drive_letter,
                            uint32_t parent_id,
                            const char *name,
                            char *buffer,
                            uint32_t buffer_size,
                            uint32_t *out_size);
int lainfs_save_file(char drive_letter,
                     const char *name,
                     const char *buffer,
                     uint32_t size);
int lainfs_save_file_in_dir(char drive_letter,
                            uint32_t parent_id,
                            const char *name,
                            const char *buffer,
                            uint32_t size);

#endif
