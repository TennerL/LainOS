#ifndef ZOBJECT_H
#define ZOBJECT_H

#include <stdint.h>

#define ZOBJECT_SYMBOL_NAME_SIZE 32u
#define ZOBJECT_MAX_RESOLVED_SYMBOLS 64u

typedef struct {
    char name[ZOBJECT_SYMBOL_NAME_SIZE];
    uint64_t value;
} zobject_resolved_symbol_t;

int zobject_from_asm(const char *asm_source,
                     uint32_t asm_size,
                     const char *entry_label,
                     unsigned char *out,
                     uint32_t out_capacity,
                     uint32_t *out_size);

int zobject_link_flat(const unsigned char *object,
                      uint32_t object_size,
                      unsigned char *out,
                      uint32_t out_capacity,
                      uint64_t base_address,
                      uint32_t *out_size,
                      uint32_t *error_line);

int zobject_link_flat_many(const unsigned char *const *objects,
                           const uint32_t *object_sizes,
                           uint32_t object_count,
                           unsigned char *out,
                           uint32_t out_capacity,
                           uint64_t base_address,
                           uint32_t *out_size,
                           uint32_t *error_line);

int zobject_link_flat_many_ex(const unsigned char *const *objects,
                              const uint32_t *object_sizes,
                              uint32_t object_count,
                              unsigned char *out,
                              uint32_t out_capacity,
                              uint64_t base_address,
                              uint32_t *out_size,
                              uint32_t *error_line,
                              const zobject_resolved_symbol_t *external_symbols,
                              uint32_t external_symbol_count,
                              zobject_resolved_symbol_t *export_symbols,
                              uint32_t export_symbol_capacity,
                              uint32_t *export_symbol_count);

#endif
