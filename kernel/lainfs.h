#ifndef LAINFS_H
#define LAINFS_H

#include <stdint.h>

int lainfs_format(char drive_letter);
int lainfs_list(char drive_letter);
int lainfs_write_file(char drive_letter, const char *name, const char *text);
int lainfs_read_file(char drive_letter, const char *name);
int lainfs_load_file(char drive_letter,
                     const char *name,
                     char *buffer,
                     uint32_t buffer_size,
                     uint32_t *out_size);
int lainfs_save_file(char drive_letter,
                     const char *name,
                     const char *buffer,
                     uint32_t size);

#endif
