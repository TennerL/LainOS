#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint64_t value;
} assembler_symbol_t;

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

int assembler_assemble_source_ex_symbols(const char *source,
                                         uint32_t size,
                                         unsigned char *out,
                                         uint32_t out_capacity,
                                         uint64_t base_address,
                                         uint32_t *out_size,
                                         uint32_t *error_line,
                                         const assembler_symbol_t *external_symbols,
                                         uint32_t external_symbol_count,
                                         const char *const *export_names,
                                         uint64_t *export_values,
                                         uint32_t export_count);

int assembler_symbol_value(const char *name, uint64_t *out);

#endif
