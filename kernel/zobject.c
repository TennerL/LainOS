#include "zobject.h"
#include "assembler.h"
#include "kernel_exports.h"

#define ZOBJECT_MAGIC 0x4F5A4E49u
#define ZOBJECT_VERSION 5u
#define ZOBJECT_VERSION_RELOC 4u
#define ZOBJECT_VERSION_ASM 3u
#define ZOBJECT_VERSION_LEGACY 2u
#define ZOBJECT_ENTRY_SIZE 32u
#define ZOBJECT_SYMBOL_RECORD_SIZE (4u + ZOBJECT_SYMBOL_NAME_SIZE)
#define ZOBJECT_SYMBOL_RECORD_V4_SIZE (12u + ZOBJECT_SYMBOL_NAME_SIZE)
#define ZOBJECT_RELOCATION_RECORD_SIZE (8u + ZOBJECT_SYMBOL_NAME_SIZE)
#define ZOBJECT_SECTION_NAME_SIZE 8u
#define ZOBJECT_SECTION_RECORD_SIZE (20u + ZOBJECT_SECTION_NAME_SIZE)
#define ZOBJECT_HEADER_SIZE (16u + ZOBJECT_ENTRY_SIZE)
#define ZOBJECT_V4_HEADER_SIZE 32u
#define ZOBJECT_V5_HEADER_SIZE 36u
#define ZOBJECT_LINK_ASM_SIZE 65536u
#define ZOBJECT_MAX_SYMBOLS 64u
#define ZOBJECT_MAX_RELOCATIONS 512u
#define ZOBJECT_MAX_OBJECTS 16u
#define ZOBJECT_MAX_SECTIONS 3u

#define ZOBJECT_SYMBOL_EXPORT 1u
#define ZOBJECT_SYMBOL_EXTERN 2u

#define ZOBJECT_RELOC_ABS64 1u
#define ZOBJECT_RELOC_RELATIVE64 2u
#define ZOBJECT_RELOC_RIP32 3u

#define ZOBJECT_SECTION_TEXT 1u
#define ZOBJECT_SECTION_DATA 2u
#define ZOBJECT_SECTION_BSS 3u

static char zo_link_asm[ZOBJECT_LINK_ASM_SIZE];
static unsigned char zo_image[ZOBJECT_LINK_ASM_SIZE];

typedef struct {
    uint32_t type;
    char name[ZOBJECT_SYMBOL_NAME_SIZE];
    uint64_t value;
} zo_symbol_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    char name[ZOBJECT_SYMBOL_NAME_SIZE];
} zo_relocation_t;

typedef struct {
    uint32_t type;
    uint32_t file_offset;
    uint32_t load_offset;
    uint32_t file_size;
    uint32_t mem_size;
    char name[ZOBJECT_SECTION_NAME_SIZE];
} zo_section_t;

typedef struct {
    uint32_t version;
    uint32_t asm_size;
    uint32_t asm_offset;
    uint32_t image_size;
    uint32_t image_offset;
    uint32_t entry_offset;
    uint32_t reloc_count;
    uint32_t symbol_count;
    uint32_t section_count;
    const char *entry_label;
    const unsigned char *symbols;
    const unsigned char *relocations;
    const unsigned char *sections;
} zo_object_info_t;

static void zo_write_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void zo_write_u64(unsigned char *p, uint64_t value) {
    for (uint32_t i = 0; i < 8u; ++i) {
        p[i] = (unsigned char)((value >> (i * 8u)) & 0xFFu);
    }
}

static uint32_t zo_read_u32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t zo_read_u64(const unsigned char *p) {
    uint64_t value = 0;

    for (uint32_t i = 0; i < 8u; ++i) {
        value |= ((uint64_t)p[i]) << (i * 8u);
    }

    return value;
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
    symbols[*symbol_count].value = 0;
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

static int zo_line_is_section_data(const char *line) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }

    if (!zo_starts_with(line, "section") ||
        (line[7] != ' ' && line[7] != '\t')) {
        return 0;
    }

    line += 8;
    while (*line == ' ' || *line == '\t') {
        ++line;
    }

    return zo_starts_with(line, ".data") || zo_starts_with(line, "data");
}

static int zo_find_data_section_source_offset(const char *asm_source,
                                              uint32_t asm_size,
                                              uint32_t *out_offset) {
    uint32_t pos = 0;

    if (asm_source == 0 || out_offset == 0) {
        return -1;
    }

    while (pos < asm_size) {
        char line[96];
        uint32_t line_start = pos;
        uint32_t len = 0;

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
        if (zo_line_is_section_data(line)) {
            *out_offset = line_start;
            return 1;
        }
    }

    *out_offset = asm_size;
    return 0;
}

static void zo_write_section_record(unsigned char *record,
                                    uint32_t type,
                                    const char *name,
                                    uint32_t file_offset,
                                    uint32_t load_offset,
                                    uint32_t file_size,
                                    uint32_t mem_size) {
    uint32_t i;

    zo_write_u32(record + 0u, type);
    zo_write_u32(record + 4u, file_offset);
    zo_write_u32(record + 8u, load_offset);
    zo_write_u32(record + 12u, file_size);
    zo_write_u32(record + 16u, mem_size);

    for (i = 0; i < ZOBJECT_SECTION_NAME_SIZE; ++i) {
        record[20u + i] = 0;
    }
    i = 0;
    while (name[i] && i + 1u < ZOBJECT_SECTION_NAME_SIZE) {
        record[20u + i] = (unsigned char)name[i];
        ++i;
    }
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
        (version != ZOBJECT_VERSION &&
         version != ZOBJECT_VERSION_RELOC &&
         version != ZOBJECT_VERSION_ASM &&
         version != ZOBJECT_VERSION_LEGACY)) {
        return -1;
    }

    info->version = version;
    info->asm_size = 0;
    info->asm_offset = 0;
    info->image_size = 0;
    info->image_offset = 0;
    info->entry_offset = 0;
    info->reloc_count = 0;
    info->symbol_count = 0;
    info->section_count = 0;
    info->entry_label = 0;
    info->symbols = 0;
    info->relocations = 0;
    info->sections = 0;

    if (version == ZOBJECT_VERSION || version == ZOBJECT_VERSION_RELOC) {
        uint32_t header_size = (version == ZOBJECT_VERSION) ? ZOBJECT_V5_HEADER_SIZE : ZOBJECT_V4_HEADER_SIZE;
        uint32_t relocation_bytes;
        uint32_t section_bytes = 0;

        if (object_size < header_size) {
            return -1;
        }

        info->image_size = zo_read_u32(object + 8);
        info->symbol_count = zo_read_u32(object + 12);
        info->reloc_count = zo_read_u32(object + 16);
        info->entry_offset = zo_read_u32(object + 20);
        info->section_count = (version == ZOBJECT_VERSION) ? zo_read_u32(object + 24) : 0;

        if (info->symbol_count > ZOBJECT_MAX_SYMBOLS ||
            info->reloc_count > ZOBJECT_MAX_RELOCATIONS ||
            info->section_count > ZOBJECT_MAX_SECTIONS ||
            info->entry_offset >= info->image_size) {
            return -1;
        }

        symbol_bytes = info->symbol_count * ZOBJECT_SYMBOL_RECORD_V4_SIZE;
        relocation_bytes = info->reloc_count * ZOBJECT_RELOCATION_RECORD_SIZE;
        section_bytes = info->section_count * ZOBJECT_SECTION_RECORD_SIZE;
        if (symbol_bytes / ZOBJECT_SYMBOL_RECORD_V4_SIZE != info->symbol_count ||
            relocation_bytes / ZOBJECT_RELOCATION_RECORD_SIZE != info->reloc_count ||
            (info->section_count != 0 && section_bytes / ZOBJECT_SECTION_RECORD_SIZE != info->section_count) ||
            symbol_bytes > object_size ||
            relocation_bytes > object_size - symbol_bytes ||
            section_bytes > object_size - symbol_bytes - relocation_bytes ||
            header_size > object_size - symbol_bytes - relocation_bytes - section_bytes) {
            return -1;
        }

        info->symbols = object + header_size;
        info->relocations = object + header_size + symbol_bytes;
        info->sections = object + header_size + symbol_bytes + relocation_bytes;
        info->image_offset = header_size + symbol_bytes + relocation_bytes + section_bytes;
        if (info->image_size > object_size - info->image_offset) {
            return -1;
        }

        return 0;
    }

    info->asm_size = zo_read_u32(object + 8);
    info->symbol_count = (version == ZOBJECT_VERSION_ASM) ? zo_read_u32(object + 12) : 0;
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
    return info->entry_label[0] == '\0' ? -1 : 0;
}

static int zo_symbol_at(const zo_object_info_t *info, uint32_t index, uint32_t *type, const char **name) {
    const unsigned char *record;

    if (info == 0 || index >= info->symbol_count || type == 0 || name == 0) {
        return -1;
    }

    if (info->version == ZOBJECT_VERSION || info->version == ZOBJECT_VERSION_RELOC) {
        record = info->symbols + index * ZOBJECT_SYMBOL_RECORD_V4_SIZE;
        *type = zo_read_u32(record);
        *name = (const char *)(record + 12u);
    } else {
        record = info->symbols + index * ZOBJECT_SYMBOL_RECORD_SIZE;
        *type = zo_read_u32(record);
        *name = (const char *)(record + 4u);
    }
    if ((*type != ZOBJECT_SYMBOL_EXPORT && *type != ZOBJECT_SYMBOL_EXTERN) ||
        (*name)[0] == '\0') {
        return -1;
    }

    return 0;
}

static int zo_symbol_value_at(const zo_object_info_t *info, uint32_t index, uint64_t *value) {
    if (info == 0 || value == 0 || index >= info->symbol_count) {
        return -1;
    }

    if (info->version != ZOBJECT_VERSION && info->version != ZOBJECT_VERSION_RELOC) {
        *value = 0;
        return 0;
    }

    *value = zo_read_u64(info->symbols + index * ZOBJECT_SYMBOL_RECORD_V4_SIZE + 4u);
    return 0;
}

static int zo_relocation_at(const zo_object_info_t *info,
                            uint32_t index,
                            uint32_t *type,
                            uint32_t *offset,
                            const char **name) {
    const unsigned char *record;

    if (info == 0 ||
        (info->version != ZOBJECT_VERSION && info->version != ZOBJECT_VERSION_RELOC) ||
        index >= info->reloc_count || type == 0 || offset == 0 || name == 0) {
        return -1;
    }

    record = info->relocations + index * ZOBJECT_RELOCATION_RECORD_SIZE;
    *type = zo_read_u32(record);
    *offset = zo_read_u32(record + 4u);
    *name = (const char *)(record + 8u);
    if (*type != ZOBJECT_RELOC_ABS64 &&
        *type != ZOBJECT_RELOC_RELATIVE64 &&
        *type != ZOBJECT_RELOC_RIP32) {
        return -1;
    }
    if (*offset > info->image_size ||
        info->image_size - *offset < ((*type == ZOBJECT_RELOC_RIP32) ? 4u : 8u)) {
        return -1;
    }

    return 0;
}

static int zo_section_at(const zo_object_info_t *info, uint32_t index, zo_section_t *section) {
    const unsigned char *record;

    if (info == 0 || section == 0 || index >= info->section_count) {
        return -1;
    }

    record = info->sections + index * ZOBJECT_SECTION_RECORD_SIZE;
    section->type = zo_read_u32(record + 0u);
    section->file_offset = zo_read_u32(record + 4u);
    section->load_offset = zo_read_u32(record + 8u);
    section->file_size = zo_read_u32(record + 12u);
    section->mem_size = zo_read_u32(record + 16u);
    for (uint32_t i = 0; i < ZOBJECT_SECTION_NAME_SIZE; ++i) {
        section->name[i] = (char)record[20u + i];
    }
    section->name[ZOBJECT_SECTION_NAME_SIZE - 1u] = '\0';

    if (section->type != ZOBJECT_SECTION_TEXT &&
        section->type != ZOBJECT_SECTION_DATA &&
        section->type != ZOBJECT_SECTION_BSS) {
        return -1;
    }
    if (section->file_size > section->mem_size ||
        section->file_offset > info->image_size ||
        info->image_size - section->file_offset < section->file_size) {
        return -1;
    }
    if (section->load_offset > info->image_size ||
        section->mem_size > 0xFFFFFFFFu - section->load_offset) {
        return -1;
    }

    return 0;
}

static int zo_map_section_offset(const zo_section_t *sections,
                                 const uint32_t *section_offsets,
                                 uint32_t section_count,
                                 uint64_t original,
                                 uint32_t *mapped) {
    uint32_t original32;

    if (sections == 0 || section_offsets == 0 || mapped == 0 || original > 0xFFFFFFFFull) {
        return -1;
    }

    original32 = (uint32_t)original;
    for (uint32_t i = 0; i < section_count; ++i) {
        uint32_t start = sections[i].load_offset;
        uint32_t size = sections[i].mem_size;

        if (size == 0) {
            continue;
        }
        if (original32 >= start && original32 < start + size) {
            *mapped = section_offsets[i] + (original32 - start);
            return 0;
        }
    }

    return -1;
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
    uint32_t reloc_bytes = 0;
    uint32_t section_bytes = 0;
    uint32_t section_count = 3u;
    uint32_t data_source_offset = 0;
    uint32_t text_size = 0;
    uint32_t data_size = 0;
    assembler_symbol_t external_symbols[ZOBJECT_MAX_SYMBOLS];
    uint32_t external_symbol_count = 0;
    assembler_symbol_t measure_symbols[ZOBJECT_MAX_SYMBOLS];
    uint32_t measure_symbol_count = 0;
    const char *export_names[ZOBJECT_MAX_SYMBOLS + 1u];
    uint64_t export_values[ZOBJECT_MAX_SYMBOLS + 1u];
    uint32_t export_name_count = 0;
    assembler_relocation_t relocations[ZOBJECT_MAX_RELOCATIONS];
    uint32_t relocation_count = 0;
    uint32_t asm_error_line = 0;
    uint32_t image_size = 0;
    uint64_t entry_value = 0;

    if (asm_source == 0 || entry_label == 0 || out == 0 || out_size == 0) {
        return -1;
    }

    if (zo_collect_metadata_symbols(asm_source, asm_size, symbols, &symbol_count) != 0) {
        return -10;
    }

    export_names[export_name_count++] = entry_label;
    for (i = 0; i < symbol_count; ++i) {
        if (symbols[i].type == ZOBJECT_SYMBOL_EXPORT) {
            if (export_name_count >= ZOBJECT_MAX_SYMBOLS + 1u) {
                return -11;
            }
            export_names[export_name_count++] = symbols[i].name;
        } else if (symbols[i].type == ZOBJECT_SYMBOL_EXTERN) {
            if (external_symbol_count >= ZOBJECT_MAX_SYMBOLS) {
                return -12;
            }
            external_symbols[external_symbol_count].name = symbols[i].name;
            external_symbols[external_symbol_count].value = 0;
            ++external_symbol_count;
        }
    }

    for (i = 0; i < external_symbol_count; ++i) {
        measure_symbols[measure_symbol_count++] = external_symbols[i];
    }
    for (i = 0; i < symbol_count && measure_symbol_count < ZOBJECT_MAX_SYMBOLS; ++i) {
        if (symbols[i].type == ZOBJECT_SYMBOL_EXPORT) {
            measure_symbols[measure_symbol_count].name = symbols[i].name;
            measure_symbols[measure_symbol_count].value = 0;
            ++measure_symbol_count;
        }
    }

    if (zo_find_data_section_source_offset(asm_source, asm_size, &data_source_offset) < 0) {
        return -20;
    }
    if (assembler_measure_source_ex_symbols(asm_source,
                                            data_source_offset,
                                            &text_size,
                                            0,
                                            measure_symbols,
                                            measure_symbol_count) != 0) {
        return -21;
    }

    if (assembler_assemble_source_ex_relocs(asm_source,
                                            asm_size,
                                            zo_image,
                                            sizeof(zo_image),
                                            0,
                                            &image_size,
                                            &asm_error_line,
                                            external_symbols,
                                            external_symbol_count,
                                            export_names,
                                            export_values,
                                            export_name_count,
                                            relocations,
                                            ZOBJECT_MAX_RELOCATIONS,
                                            &relocation_count) != 0) {
        return asm_error_line != 0 ? -(int)(3000u + asm_error_line) : -30;
    }

    if (text_size > image_size) {
        return -31;
    }
    data_size = image_size - text_size;

    entry_value = export_values[0];
    if (entry_value > 0xFFFFFFFFull) {
        return -40;
    }

    for (i = 0; i < symbol_count; ++i) {
        if (symbols[i].type == ZOBJECT_SYMBOL_EXPORT) {
            uint32_t export_index = 1u;

            while (export_index < export_name_count &&
                   !zo_streq(export_names[export_index], symbols[i].name)) {
                ++export_index;
            }

            if (export_index >= export_name_count) {
                return -50;
            }
            symbols[i].value = export_values[export_index];
        }
    }

    symbol_bytes = symbol_count * ZOBJECT_SYMBOL_RECORD_V4_SIZE;
    reloc_bytes = relocation_count * ZOBJECT_RELOCATION_RECORD_SIZE;
    section_bytes = section_count * ZOBJECT_SECTION_RECORD_SIZE;
    if (symbol_bytes / ZOBJECT_SYMBOL_RECORD_V4_SIZE != symbol_count ||
        reloc_bytes / ZOBJECT_RELOCATION_RECORD_SIZE != relocation_count ||
        section_bytes / ZOBJECT_SECTION_RECORD_SIZE != section_count ||
        symbol_bytes > out_capacity ||
        reloc_bytes > out_capacity - symbol_bytes ||
        section_bytes > out_capacity - symbol_bytes - reloc_bytes ||
        image_size > out_capacity - symbol_bytes - reloc_bytes - section_bytes ||
        ZOBJECT_V5_HEADER_SIZE > out_capacity - symbol_bytes - reloc_bytes - section_bytes - image_size) {
        return -60;
    }

    zo_write_u32(out + 0, ZOBJECT_MAGIC);
    zo_write_u32(out + 4, ZOBJECT_VERSION);
    zo_write_u32(out + 8, image_size);
    zo_write_u32(out + 12, symbol_count);
    zo_write_u32(out + 16, relocation_count);
    zo_write_u32(out + 20, (uint32_t)entry_value);
    zo_write_u32(out + 24, section_count);
    zo_write_u32(out + 28, 0);
    zo_write_u32(out + 32, 0);

    for (i = 0; i < symbol_count; ++i) {
        uint32_t j;
        unsigned char *record = out + ZOBJECT_V5_HEADER_SIZE + i * ZOBJECT_SYMBOL_RECORD_V4_SIZE;

        zo_write_u32(record, symbols[i].type);
        zo_write_u64(record + 4u, symbols[i].value);
        for (j = 0; j < ZOBJECT_SYMBOL_NAME_SIZE; ++j) {
            record[12u + j] = 0;
        }
        j = 0;
        while (symbols[i].name[j] && j + 1u < ZOBJECT_SYMBOL_NAME_SIZE) {
            record[12u + j] = (unsigned char)symbols[i].name[j];
            ++j;
        }
    }

    for (i = 0; i < relocation_count; ++i) {
        uint32_t j;
        unsigned char *record = out + ZOBJECT_V5_HEADER_SIZE +
                                symbol_bytes +
                                i * ZOBJECT_RELOCATION_RECORD_SIZE;

        zo_write_u32(record, relocations[i].type == ASSEMBLER_RELOC_RELATIVE64
            ? ZOBJECT_RELOC_RELATIVE64
            : (relocations[i].type == ASSEMBLER_RELOC_RIP32
                ? ZOBJECT_RELOC_RIP32
                : ZOBJECT_RELOC_ABS64));
        zo_write_u32(record + 4u, relocations[i].offset);
        for (j = 0; j < ZOBJECT_SYMBOL_NAME_SIZE; ++j) {
            record[8u + j] = 0;
        }
        if (relocations[i].name[0] != '\0') {
            j = 0;
            while (relocations[i].name[j] && j + 1u < ZOBJECT_SYMBOL_NAME_SIZE) {
                record[8u + j] = (unsigned char)relocations[i].name[j];
                ++j;
            }
        }
    }

    zo_write_section_record(out + ZOBJECT_V5_HEADER_SIZE + symbol_bytes + reloc_bytes,
                            ZOBJECT_SECTION_TEXT,
                            ".text",
                            0,
                            0,
                            text_size,
                            text_size);
    zo_write_section_record(out + ZOBJECT_V5_HEADER_SIZE + symbol_bytes + reloc_bytes + ZOBJECT_SECTION_RECORD_SIZE,
                            ZOBJECT_SECTION_DATA,
                            ".data",
                            text_size,
                            text_size,
                            data_size,
                            data_size);
    zo_write_section_record(out + ZOBJECT_V5_HEADER_SIZE + symbol_bytes + reloc_bytes + 2u * ZOBJECT_SECTION_RECORD_SIZE,
                            ZOBJECT_SECTION_BSS,
                            ".bss",
                            image_size,
                            image_size,
                            0,
                            0);

    for (i = 0; i < image_size; ++i) {
        out[ZOBJECT_V5_HEADER_SIZE + symbol_bytes + reloc_bytes + section_bytes + i] = zo_image[i];
    }

    *out_size = ZOBJECT_V5_HEADER_SIZE + symbol_bytes + reloc_bytes + section_bytes + image_size;
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
                    kernel_export_value(name, &kernel_value) == 0) {
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
                    kernel_export_value(name, &kernel_value) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int zo_resolve_link_symbol(const zobject_resolved_symbol_t *exports,
                                  uint32_t export_count,
                                  const zobject_resolved_symbol_t *external_symbols,
                                  uint32_t external_symbol_count,
                                  const char *name,
                                  uint64_t *value) {
    uint32_t i;

    for (i = 0; i < export_count; ++i) {
        if (zo_streq(exports[i].name, name)) {
            *value = exports[i].value;
            return 0;
        }
    }

    for (i = 0; i < external_symbol_count; ++i) {
        if (zo_streq(external_symbols[i].name, name)) {
            *value = external_symbols[i].value;
            return 0;
        }
    }

    return kernel_export_value(name, value);
}

static uint32_t zo_align_up_u32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
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
    zobject_resolved_symbol_t local_exports[ZOBJECT_MAX_RESOLVED_SYMBOLS];
    assembler_symbol_t assembler_external_symbols[ZOBJECT_MAX_RESOLVED_SYMBOLS];
    uint32_t image_offsets[ZOBJECT_MAX_OBJECTS];
    zo_section_t object_sections[ZOBJECT_MAX_OBJECTS][ZOBJECT_MAX_SECTIONS];
    uint32_t section_offsets[ZOBJECT_MAX_OBJECTS][ZOBJECT_MAX_SECTIONS];
    int have_v5 = 0;
    int have_v4_reloc = 0;
    int have_legacy = 0;

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
        if (infos[i].version == ZOBJECT_VERSION) {
            have_v5 = 1;
        } else if (infos[i].version == ZOBJECT_VERSION_RELOC) {
            have_v4_reloc = 1;
        } else {
            have_legacy = 1;
        }
    }

    if ((have_v5 && (have_v4_reloc || have_legacy)) ||
        (have_v4_reloc && have_legacy)) {
        return -1;
    }

    if (zo_validate_link_symbols(infos, object_count, external_symbols, external_symbol_count) != 0) {
        return -1;
    }

    if (have_v5) {
        uint32_t output_offset = object_count * 5u + 1u;

        for (i = 0; i < object_count; ++i) {
            if (infos[i].section_count == 0) {
                return -1;
            }
            for (uint32_t j = 0; j < infos[i].section_count; ++j) {
                if (zo_section_at(&infos[i], j, &object_sections[i][j]) != 0) {
                    return -1;
                }
                section_offsets[i][j] = 0;
            }
        }

        for (uint32_t section_type = ZOBJECT_SECTION_TEXT;
             section_type <= ZOBJECT_SECTION_BSS;
             ++section_type) {
            for (i = 0; i < object_count; ++i) {
                for (uint32_t j = 0; j < infos[i].section_count; ++j) {
                    const zo_section_t *section = &object_sections[i][j];

                    if (section->type != section_type || section->mem_size == 0) {
                        continue;
                    }

                    {
                        uint32_t aligned_offset = zo_align_up_u32(output_offset, 16u);
                        if (aligned_offset > out_capacity) {
                            return -1;
                        }
                        while (output_offset < aligned_offset) {
                            out[output_offset++] = 0;
                        }
                    }
                    section_offsets[i][j] = output_offset;
                    if (section->mem_size > out_capacity ||
                        output_offset > out_capacity - section->mem_size) {
                        return -1;
                    }
                    for (uint32_t k = 0; k < section->mem_size; ++k) {
                        out[output_offset + k] = 0;
                    }
                    for (uint32_t k = 0; k < section->file_size; ++k) {
                        out[output_offset + k] =
                            objects[i][infos[i].image_offset + section->file_offset + k];
                    }
                    output_offset += section->mem_size;
                }
            }
        }

        for (i = 0; i < object_count; ++i) {
            uint32_t mapped_entry = 0;
            uint32_t call_offset = i * 5u;
            uint32_t next_offset = call_offset + 5u;
            int64_t diff;

            if (zo_map_section_offset(object_sections[i],
                                      section_offsets[i],
                                      infos[i].section_count,
                                      infos[i].entry_offset,
                                      &mapped_entry) != 0) {
                return -1;
            }

            diff = (int64_t)(base_address + mapped_entry) -
                   (int64_t)(base_address + next_offset);

            if (diff < -2147483648ll || diff > 2147483647ll) {
                return -1;
            }

            out[call_offset] = 0xE8u;
            zo_write_u32(out + call_offset + 1u, (uint32_t)diff);
        }
        out[object_count * 5u] = 0xC3u;

        for (i = 0; i < object_count; ++i) {
            for (uint32_t j = 0; j < infos[i].symbol_count; ++j) {
                uint32_t type = 0;
                const char *name = 0;
                uint64_t value = 0;
                uint32_t mapped_value = 0;

                if (zo_symbol_at(&infos[i], j, &type, &name) != 0 ||
                    zo_symbol_value_at(&infos[i], j, &value) != 0) {
                    return -1;
                }

                if (type != ZOBJECT_SYMBOL_EXPORT) {
                    continue;
                }

                if (local_export_count >= ZOBJECT_MAX_RESOLVED_SYMBOLS ||
                    zo_map_section_offset(object_sections[i],
                                          section_offsets[i],
                                          infos[i].section_count,
                                          value,
                                          &mapped_value) != 0) {
                    return -1;
                }

                zo_copy_name(local_exports[local_export_count].name,
                             sizeof(local_exports[local_export_count].name),
                             name);
                local_exports[local_export_count].value = base_address + mapped_value;
                ++local_export_count;
            }
        }

        if (export_symbol_count) {
            if (local_export_count > export_symbol_capacity) {
                return -1;
            }
            for (i = 0; i < local_export_count; ++i) {
                export_symbols[i] = local_exports[i];
            }
            *export_symbol_count = local_export_count;
        }

        for (i = 0; i < object_count; ++i) {
            for (uint32_t j = 0; j < infos[i].reloc_count; ++j) {
                uint32_t type = 0;
                uint32_t reloc_offset = 0;
                const char *name = 0;
                uint64_t value = 0;
                uint32_t patch_offset = 0;

                if (zo_relocation_at(&infos[i], j, &type, &reloc_offset, &name) != 0 ||
                    zo_map_section_offset(object_sections[i],
                                          section_offsets[i],
                                          infos[i].section_count,
                                          reloc_offset,
                                          &patch_offset) != 0) {
                    return -1;
                }

                if (type == ZOBJECT_RELOC_RELATIVE64) {
                    uint32_t mapped_value = 0;

                    if (zo_map_section_offset(object_sections[i],
                                              section_offsets[i],
                                              infos[i].section_count,
                                              zo_read_u64(out + patch_offset),
                                              &mapped_value) != 0) {
                        return -1;
                    }
                    value = base_address + mapped_value;
                    zo_write_u64(out + patch_offset, value);
                } else if (type == ZOBJECT_RELOC_RIP32) {
                    int32_t old_disp = (int32_t)zo_read_u32(out + patch_offset);
                    int64_t original_target = (int64_t)reloc_offset + 4ll + (int64_t)old_disp;
                    uint32_t mapped_target = 0;
                    int64_t new_disp;

                    if (original_target < 0 ||
                        zo_map_section_offset(object_sections[i],
                                              section_offsets[i],
                                              infos[i].section_count,
                                              (uint64_t)original_target,
                                              &mapped_target) != 0) {
                        return -1;
                    }

                    new_disp = (int64_t)mapped_target - (int64_t)(patch_offset + 4u);
                    if (new_disp < -2147483648ll || new_disp > 2147483647ll) {
                        return -1;
                    }
                    zo_write_u32(out + patch_offset, (uint32_t)new_disp);
                } else {
                    if (zo_resolve_link_symbol(local_exports,
                                               local_export_count,
                                               external_symbols,
                                               external_symbol_count,
                                               name,
                                               &value) != 0) {
                        return -1;
                    }
                    zo_write_u64(out + patch_offset, value);
                }
            }
        }

        *out_size = output_offset;
        return 0;
    }

    if (have_v4_reloc) {
        uint32_t output_offset = object_count * 5u + 1u;

        for (i = 0; i < object_count; ++i) {
            output_offset = zo_align_up_u32(output_offset, 16u);
            image_offsets[i] = output_offset;
            if (infos[i].image_size > out_capacity ||
                output_offset > out_capacity - infos[i].image_size) {
                return -1;
            }
            output_offset += infos[i].image_size;
        }

        for (i = 0; i < object_count; ++i) {
            uint32_t call_offset = i * 5u;
            uint32_t next_offset = call_offset + 5u;
            int64_t diff = (int64_t)(base_address + image_offsets[i] + infos[i].entry_offset) -
                           (int64_t)(base_address + next_offset);

            if (diff < -2147483648ll || diff > 2147483647ll) {
                return -1;
            }

            out[call_offset] = 0xE8u;
            zo_write_u32(out + call_offset + 1u, (uint32_t)diff);
        }
        out[object_count * 5u] = 0xC3u;
        for (i = object_count * 5u + 1u; i < output_offset; ++i) {
            out[i] = 0;
        }

        for (i = 0; i < object_count; ++i) {
            for (uint32_t j = 0; j < infos[i].image_size; ++j) {
                out[image_offsets[i] + j] = objects[i][infos[i].image_offset + j];
            }
        }

        for (i = 0; i < object_count; ++i) {
            for (uint32_t j = 0; j < infos[i].symbol_count; ++j) {
                uint32_t type = 0;
                const char *name = 0;
                uint64_t value = 0;

                if (zo_symbol_at(&infos[i], j, &type, &name) != 0 ||
                    zo_symbol_value_at(&infos[i], j, &value) != 0) {
                    return -1;
                }

                if (type != ZOBJECT_SYMBOL_EXPORT) {
                    continue;
                }

                if (local_export_count >= ZOBJECT_MAX_RESOLVED_SYMBOLS) {
                    return -1;
                }

                zo_copy_name(local_exports[local_export_count].name,
                             sizeof(local_exports[local_export_count].name),
                             name);
                local_exports[local_export_count].value = base_address + image_offsets[i] + value;
                ++local_export_count;
            }
        }

        if (export_symbol_count) {
            if (local_export_count > export_symbol_capacity) {
                return -1;
            }
            for (i = 0; i < local_export_count; ++i) {
                export_symbols[i] = local_exports[i];
            }
            *export_symbol_count = local_export_count;
        }

        for (i = 0; i < object_count; ++i) {
            for (uint32_t j = 0; j < infos[i].reloc_count; ++j) {
                uint32_t type = 0;
                uint32_t reloc_offset = 0;
                const char *name = 0;
                uint64_t value = 0;
                uint32_t patch_offset;

                if (zo_relocation_at(&infos[i], j, &type, &reloc_offset, &name) != 0) {
                    return -1;
                }

                patch_offset = image_offsets[i] + reloc_offset;
                if (type == ZOBJECT_RELOC_RELATIVE64) {
                    value = base_address + image_offsets[i] + zo_read_u64(out + patch_offset);
                } else {
                    if (zo_resolve_link_symbol(local_exports,
                                               local_export_count,
                                               external_symbols,
                                               external_symbol_count,
                                               name,
                                               &value) != 0) {
                        return -1;
                    }
                }

                zo_write_u64(out + patch_offset, value);
            }
        }

        *out_size = output_offset;
        return 0;
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
