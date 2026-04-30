#include "zobject.h"
#include "assembler.h"

#define ZOBJECT_MAGIC 0x4F5A4E49u
#define ZOBJECT_VERSION 3u
#define ZOBJECT_VERSION_LEGACY 2u
#define ZOBJECT_ENTRY_SIZE 32u
#define ZOBJECT_SYMBOL_RECORD_SIZE (4u + ZOBJECT_SYMBOL_NAME_SIZE)
#define ZOBJECT_HEADER_SIZE (16u + ZOBJECT_ENTRY_SIZE)
#define ZOBJECT_LINK_ASM_SIZE 65536u
#define ZOBJECT_MAX_SYMBOLS 64u
#define ZOBJECT_MAX_OBJECTS 8u

#define ZOBJECT_SYMBOL_EXPORT 1u
#define ZOBJECT_SYMBOL_EXTERN 2u

static char zo_link_asm[ZOBJECT_LINK_ASM_SIZE];

typedef struct {
    uint32_t type;
    char name[ZOBJECT_SYMBOL_NAME_SIZE];
} zo_symbol_t;

typedef struct {
    uint32_t asm_size;
    uint32_t asm_offset;
    uint32_t symbol_count;
    const char *entry_label;
    const unsigned char *symbols;
} zo_object_info_t;

static void zo_write_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static uint32_t zo_read_u32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int zo_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }

    return *a == '\0' && *b == '\0';
}

static int zo_starts_with(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) {
            return 0;
        }
    }

    return 1;
}

static int zo_name_char(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static void zo_copy_name(char *dst, uint32_t dst_size, const char *src) {
    uint32_t i = 0;

    if (dst_size == 0) {
        return;
    }

    while (src[i] && i + 1u < dst_size) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static int zo_emit_char(uint32_t *size, char ch) {
    if (*size >= ZOBJECT_LINK_ASM_SIZE) {
        return -1;
    }

    zo_link_asm[(*size)++] = ch;
    return 0;
}

static int zo_emit_text(uint32_t *size, const char *text) {
    while (*text) {
        if (zo_emit_char(size, *text++) != 0) {
            return -1;
        }
    }

    return 0;
}

static int zo_append_bytes(uint32_t *size, const unsigned char *bytes, uint32_t byte_count) {
    uint32_t i;

    for (i = 0; i < byte_count; ++i) {
        if (zo_emit_char(size, (char)bytes[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static int zo_add_symbol(zo_symbol_t *symbols,
                         uint32_t *symbol_count,
                         uint32_t type,
                         const char *name) {
    uint32_t i;

    for (i = 0; i < *symbol_count; ++i) {
        if (symbols[i].type == type && zo_streq(symbols[i].name, name)) {
            return 0;
        }
    }

    if (*symbol_count >= ZOBJECT_MAX_SYMBOLS) {
        return -1;
    }

    symbols[*symbol_count].type = type;
    zo_copy_name(symbols[*symbol_count].name, sizeof(symbols[*symbol_count].name), name);
    ++(*symbol_count);
    return 0;
}

static int zo_parse_metadata_name(const char *text, char *name, uint32_t name_size) {
    uint32_t i = 0;

    while (*text == ' ' || *text == '\t') {
        ++text;
    }

    if (!zo_name_char(*text)) {
        return -1;
    }

    while (zo_name_char(*text)) {
        if (i + 1u >= name_size) {
            return -1;
        }
        name[i++] = *text++;
    }
    name[i] = '\0';

    return 0;
}

static int zo_collect_metadata_symbols(const char *asm_source,
                                       uint32_t asm_size,
                                       zo_symbol_t *symbols,
                                       uint32_t *symbol_count) {
    uint32_t pos = 0;

    *symbol_count = 0;
    while (pos < asm_size) {
        char line[96];
        uint32_t len = 0;
        const char *s;

        while (pos < asm_size && asm_source[pos] != '\n' && len + 1u < sizeof(line)) {
            line[len++] = asm_source[pos++];
        }
        while (pos < asm_size && asm_source[pos] != '\n') {
            ++pos;
        }
        if (pos < asm_size && asm_source[pos] == '\n') {
            ++pos;
        }

        line[len] = '\0';
        s = line;
        while (*s == ' ' || *s == '\t') {
            ++s;
        }

        if (zo_starts_with(s, "; zexport ")) {
            char name[ZOBJECT_SYMBOL_NAME_SIZE];
            s += 10;
            if (zo_parse_metadata_name(s, name, sizeof(name)) != 0) {
                return -1;
            }
            if (zo_add_symbol(symbols, symbol_count, ZOBJECT_SYMBOL_EXPORT, name) != 0) {
                return -1;
            }
        } else if (zo_starts_with(s, "; zextern ")) {
            char name[ZOBJECT_SYMBOL_NAME_SIZE];
            s += 10;
            if (zo_parse_metadata_name(s, name, sizeof(name)) != 0) {
                return -1;
            }
            if (zo_add_symbol(symbols, symbol_count, ZOBJECT_SYMBOL_EXTERN, name) != 0) {
                return -1;
            }
        } else if (zo_starts_with(s, "; zo_export ")) {
            char name[ZOBJECT_SYMBOL_NAME_SIZE];
            s += 12;
            if (zo_parse_metadata_name(s, name, sizeof(name)) != 0) {
                return -1;
            }
            if (zo_add_symbol(symbols, symbol_count, ZOBJECT_SYMBOL_EXPORT, name) != 0) {
                return -1;
            }
        } else if (zo_starts_with(s, "; zo_extern ")) {
            char name[ZOBJECT_SYMBOL_NAME_SIZE];
            s += 12;
            if (zo_parse_metadata_name(s, name, sizeof(name)) != 0) {
                return -1;
            }
            if (zo_add_symbol(symbols, symbol_count, ZOBJECT_SYMBOL_EXTERN, name) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int zo_validate_object(const unsigned char *object,
                              uint32_t object_size,
                              zo_object_info_t *info) {
    uint32_t version;
    uint32_t symbol_bytes;

    if (object == 0 || info == 0 || object_size < ZOBJECT_HEADER_SIZE) {
        return -1;
    }

    version = zo_read_u32(object + 4);
    if (zo_read_u32(object + 0) != ZOBJECT_MAGIC ||
        (version != ZOBJECT_VERSION && version != ZOBJECT_VERSION_LEGACY)) {
        return -1;
    }

    info->asm_size = zo_read_u32(object + 8);
    info->symbol_count = (version == ZOBJECT_VERSION) ? zo_read_u32(object + 12) : 0;
    if (info->symbol_count > ZOBJECT_MAX_SYMBOLS) {
        return -1;
    }

    symbol_bytes = info->symbol_count * ZOBJECT_SYMBOL_RECORD_SIZE;
    if (symbol_bytes / ZOBJECT_SYMBOL_RECORD_SIZE != info->symbol_count ||
        symbol_bytes > object_size ||
        ZOBJECT_HEADER_SIZE > object_size - symbol_bytes) {
        return -1;
    }

    info->asm_offset = ZOBJECT_HEADER_SIZE + symbol_bytes;
    if (info->asm_size > object_size - info->asm_offset) {
        return -1;
    }

    info->entry_label = (const char *)(object + 16u);
    info->symbols = object + ZOBJECT_HEADER_SIZE;
    if (info->entry_label[0] == '\0') {
        return -1;
    }

    return 0;
}

static int zo_symbol_at(const zo_object_info_t *info, uint32_t index, uint32_t *type, const char **name) {
    const unsigned char *record;

    if (info == 0 || index >= info->symbol_count || type == 0 || name == 0) {
        return -1;
    }

    record = info->symbols + index * ZOBJECT_SYMBOL_RECORD_SIZE;
    *type = zo_read_u32(record);
    *name = (const char *)(record + 4u);
    if ((*type != ZOBJECT_SYMBOL_EXPORT && *type != ZOBJECT_SYMBOL_EXTERN) ||
        (*name)[0] == '\0') {
        return -1;
    }

    return 0;
}

int zobject_from_asm(const char *asm_source,
                     uint32_t asm_size,
                     const char *entry_label,
                     unsigned char *out,
                     uint32_t out_capacity,
                     uint32_t *out_size) {
    uint32_t i;
    zo_symbol_t symbols[ZOBJECT_MAX_SYMBOLS];
    uint32_t symbol_count = 0;
    uint32_t symbol_bytes = 0;

    if (asm_source == 0 || entry_label == 0 || out == 0 || out_size == 0) {
        return -1;
    }

    if (zo_collect_metadata_symbols(asm_source, asm_size, symbols, &symbol_count) != 0) {
        return -1;
    }

    symbol_bytes = symbol_count * ZOBJECT_SYMBOL_RECORD_SIZE;
    if (asm_size > out_capacity ||
        symbol_bytes > out_capacity - asm_size ||
        ZOBJECT_HEADER_SIZE > out_capacity - asm_size - symbol_bytes) {
        return -1;
    }

    zo_write_u32(out + 0, ZOBJECT_MAGIC);
    zo_write_u32(out + 4, ZOBJECT_VERSION);
    zo_write_u32(out + 8, asm_size);
    zo_write_u32(out + 12, symbol_count);

    for (i = 0; i < ZOBJECT_ENTRY_SIZE; ++i) {
        out[16u + i] = 0;
    }

    i = 0;
    while (entry_label[i] && i + 1u < ZOBJECT_ENTRY_SIZE) {
        out[16u + i] = (unsigned char)entry_label[i];
        ++i;
    }

    for (i = 0; i < symbol_count; ++i) {
        uint32_t j;
        unsigned char *record = out + ZOBJECT_HEADER_SIZE + i * ZOBJECT_SYMBOL_RECORD_SIZE;

        zo_write_u32(record, symbols[i].type);
        for (j = 0; j < ZOBJECT_SYMBOL_NAME_SIZE; ++j) {
            record[4u + j] = 0;
        }
        j = 0;
        while (symbols[i].name[j] && j + 1u < ZOBJECT_SYMBOL_NAME_SIZE) {
            record[4u + j] = (unsigned char)symbols[i].name[j];
            ++j;
        }
    }

    for (i = 0; i < asm_size; ++i) {
        out[ZOBJECT_HEADER_SIZE + symbol_bytes + i] = (unsigned char)asm_source[i];
    }

    *out_size = ZOBJECT_HEADER_SIZE + symbol_bytes + asm_size;
    return 0;
}

static int zo_export_exists(const zo_object_info_t *infos,
                            uint32_t object_count,
                            const zobject_resolved_symbol_t *external_symbols,
                            uint32_t external_symbol_count,
                            const char *name,
                            uint32_t *owner) {
    uint32_t i;

    for (i = 0; i < external_symbol_count; ++i) {
        if (zo_streq(external_symbols[i].name, name)) {
            if (owner) {
                *owner = ZOBJECT_MAX_OBJECTS;
            }
            return 1;
        }
    }

    for (i = 0; i < object_count; ++i) {
        uint32_t j;

        for (j = 0; j < infos[i].symbol_count; ++j) {
            uint32_t type = 0;
            const char *symbol_name = 0;

            if (zo_symbol_at(&infos[i], j, &type, &symbol_name) != 0) {
                return -1;
            }

            if (type == ZOBJECT_SYMBOL_EXPORT && zo_streq(symbol_name, name)) {
                if (owner) {
                    *owner = i;
                }
                return 1;
            }
        }
    }

    return 0;
}

static int zo_validate_link_symbols(const zo_object_info_t *infos,
                                    uint32_t object_count,
                                    const zobject_resolved_symbol_t *external_symbols,
                                    uint32_t external_symbol_count) {
    uint32_t i;

    for (i = 0; i < object_count; ++i) {
        uint32_t j;

        for (j = 0; j < infos[i].symbol_count; ++j) {
            uint32_t type = 0;
            const char *name = 0;

            if (zo_symbol_at(&infos[i], j, &type, &name) != 0) {
                return -1;
            }

            if (type == ZOBJECT_SYMBOL_EXPORT) {
                uint32_t owner = 0;
                uint64_t kernel_value = 0;
                int found = zo_export_exists(infos,
                                             object_count,
                                             external_symbols,
                                             external_symbol_count,
                                             name,
                                             &owner);

                if (found < 0 ||
                    (found > 0 && owner != i) ||
                    assembler_symbol_value(name, &kernel_value) == 0) {
                    return -1;
                }
            } else if (type == ZOBJECT_SYMBOL_EXTERN) {
                uint64_t kernel_value = 0;
                if (zo_export_exists(infos,
                                     object_count,
                                     external_symbols,
                                     external_symbol_count,
                                     name,
                                     0) <= 0 &&
                    assembler_symbol_value(name, &kernel_value) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

int zobject_link_flat_many(const unsigned char *const *objects,
                           const uint32_t *object_sizes,
                           uint32_t object_count,
                           unsigned char *out,
                           uint32_t out_capacity,
                           uint64_t base_address,
                           uint32_t *out_size,
                           uint32_t *error_line) {
    return zobject_link_flat_many_ex(objects,
                                     object_sizes,
                                     object_count,
                                     out,
                                     out_capacity,
                                     base_address,
                                     out_size,
                                     error_line,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0);
}

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
                              uint32_t *export_symbol_count) {
    uint32_t link_size = 0;
    uint32_t i;
    zo_object_info_t infos[ZOBJECT_MAX_OBJECTS];
    const char *export_names[ZOBJECT_MAX_RESOLVED_SYMBOLS];
    uint64_t export_values[ZOBJECT_MAX_RESOLVED_SYMBOLS];
    uint32_t local_export_count = 0;
    assembler_symbol_t assembler_external_symbols[ZOBJECT_MAX_RESOLVED_SYMBOLS];

    if (objects == 0 || object_sizes == 0 || object_count == 0 || out == 0 || out_size == 0) {
        return -1;
    }

    if (object_count > ZOBJECT_MAX_OBJECTS ||
        external_symbol_count > ZOBJECT_MAX_RESOLVED_SYMBOLS ||
        (external_symbol_count != 0 && external_symbols == 0) ||
        (export_symbol_count != 0 && export_symbols == 0)) {
        return -1;
    }

    if (export_symbol_count) {
        *export_symbol_count = 0;
    }

    for (i = 0; i < object_count; ++i) {
        if (zo_validate_object(objects[i], object_sizes[i], &infos[i]) != 0) {
            return -1;
        }
    }

    if (zo_validate_link_symbols(infos, object_count, external_symbols, external_symbol_count) != 0) {
        return -1;
    }

    for (i = 0; i < external_symbol_count; ++i) {
        assembler_external_symbols[i].name = external_symbols[i].name;
        assembler_external_symbols[i].value = external_symbols[i].value;
    }

    if (zo_emit_text(&link_size, "bits 64\n") != 0 ||
        zo_emit_text(&link_size, "default rel\n") != 0 ||
        zo_emit_text(&link_size, "section .text\n") != 0 ||
        zo_emit_text(&link_size, "start:\n") != 0) {
        return -1;
    }

    for (i = 0; i < object_count; ++i) {
        if (zo_emit_text(&link_size, "    call ") != 0 ||
            zo_emit_text(&link_size, infos[i].entry_label) != 0 ||
            zo_emit_char(&link_size, '\n') != 0) {
            return -1;
        }
    }

    if (zo_emit_text(&link_size, "    ret\n") != 0) {
        return -1;
    }

    for (i = 0; i < object_count; ++i) {
        if (zo_append_bytes(&link_size, objects[i] + infos[i].asm_offset, infos[i].asm_size) != 0 ||
            zo_emit_char(&link_size, '\n') != 0) {
            return -1;
        }
    }

    for (i = 0; i < object_count; ++i) {
        uint32_t j;

        for (j = 0; j < infos[i].symbol_count; ++j) {
            uint32_t type = 0;
            const char *name = 0;

            if (zo_symbol_at(&infos[i], j, &type, &name) != 0) {
                return -1;
            }

            if (type != ZOBJECT_SYMBOL_EXPORT) {
                continue;
            }

            if (local_export_count >= ZOBJECT_MAX_RESOLVED_SYMBOLS) {
                return -1;
            }

            export_names[local_export_count++] = name;
        }
    }

    if (assembler_assemble_source_ex_symbols(zo_link_asm,
                                             link_size,
                                             out,
                                             out_capacity,
                                             base_address,
                                             out_size,
                                             error_line,
                                             assembler_external_symbols,
                                             external_symbol_count,
                                             export_names,
                                             export_values,
                                             local_export_count) != 0) {
        return -1;
    }

    if (export_symbol_count) {
        if (local_export_count > export_symbol_capacity) {
            return -1;
        }

        for (i = 0; i < local_export_count; ++i) {
            zo_copy_name(export_symbols[i].name, sizeof(export_symbols[i].name), export_names[i]);
            export_symbols[i].value = export_values[i];
        }
        *export_symbol_count = local_export_count;
    }

    return 0;
}

int zobject_link_flat(const unsigned char *object,
                      uint32_t object_size,
                      unsigned char *out,
                      uint32_t out_capacity,
                      uint64_t base_address,
                      uint32_t *out_size,
                      uint32_t *error_line) {
    const unsigned char *objects[1];
    uint32_t object_sizes[1];

    objects[0] = object;
    object_sizes[0] = object_size;
    return zobject_link_flat_many(objects,
                                  object_sizes,
                                  1,
                                  out,
                                  out_capacity,
                                  base_address,
                                  out_size,
                                  error_line);
}
