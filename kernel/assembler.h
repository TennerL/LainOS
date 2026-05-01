#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint64_t value;
} assembler_symbol_t;

#define ASSEMBLER_RELOC_ABS64 1u
#define ASSEMBLER_RELOC_RELATIVE64 2u
#define ASSEMBLER_RELOC_RIP32 3u

typedef struct {
    uint32_t offset;
    uint32_t type;
    char name[32];
} assembler_relocation_t;

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

int assembler_assemble_source_ex_relocs(const char *source,
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
                                        uint32_t export_count,
                                        assembler_relocation_t *relocations,
                                        uint32_t relocation_capacity,
                                        uint32_t *relocation_count);

int assembler_measure_source(const char *source,
                             uint32_t size,
                             uint32_t *out_size,
                             uint32_t *error_line);

int assembler_measure_source_ex_symbols(const char *source,
                                        uint32_t size,
                                        uint32_t *out_size,
                                        uint32_t *error_line,
                                        const assembler_symbol_t *external_symbols,
                                        uint32_t external_symbol_count);

int assembler_symbol_value(const char *name, uint64_t *out);

#endif
