#ifndef ZSCRIPT_H
#define ZSCRIPT_H

#include <stdint.h>

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

#endif
