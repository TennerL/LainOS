#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdint.h>

int assembler_assemble_source(const char *source,
                              uint32_t size,
                              unsigned char *out,
                              uint32_t out_capacity,
                              uint64_t base_address,
                              uint32_t *out_size);

int assembler_assemble_source_ex(const char *source,
                                 uint32_t size,
                                 unsigned char *out,
                                 uint32_t out_capacity,
                                 uint64_t base_address,
                                 uint32_t *out_size,
                                 uint32_t *error_line);

#endif
