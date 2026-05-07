#ifndef LAINFS_H
#define LAINFS_H

#include <stdint.h>

#define LAINFS_ROOT_DIR 0u
//#define LAINFS_FILE_CAPACITY 65536u
#define LAINFS_FILE_CAPACITY 524288u
#define LAINFS_ENTRY_TYPE_FILE 1u
#define LAINFS_ENTRY_TYPE_DIR 2u

int lainfs_format(char drive_letter);
int lainfs_format_partition(const char *partition_name);
int lainfs_format_block_device(const char *device_name, char *out_partition_name, uint32_t out_partition_name_size);
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
