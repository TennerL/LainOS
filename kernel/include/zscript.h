#ifndef ZSCRIPT_H
#define ZSCRIPT_H

#include <stdint.h>

#define ZSCRIPT_ERROR_NONE 0
#define ZSCRIPT_ERROR_SYNTAX 1
#define ZSCRIPT_ERROR_OUTPUT_FULL 2

int zscript_compile_source(const char *source,
                           uint32_t size,
                           char *out,
                           uint32_t out_capacity,
                           uint32_t *out_size,
                           uint32_t *error_line);

int zscript_compile_source_object(const char *source,
                                  uint32_t size,
                                  const char *label_prefix,
                                  char *out,
                                  uint32_t out_capacity,
                                  uint32_t *out_size,
                                  uint32_t *error_line,
                                  char *entry_label,
                                  uint32_t entry_label_capacity);

int zscript_last_error(void);

#endif
