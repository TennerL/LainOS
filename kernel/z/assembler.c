#include "assembler.h"
#include "kernel.h"
#include "kernel_exports.h"

#define ASM_LINE_SIZE 256u
#define ASM_MAX_LABELS 8192u
#define ASM_LABEL_NAME_SIZE ASSEMBLER_SYMBOL_NAME_SIZE

typedef struct {
    char name[ASM_LABEL_NAME_SIZE];
    uint32_t offset;
} asm_label_t;

static asm_label_t asm_work_labels[ASM_MAX_LABELS];

typedef struct {
    unsigned char *out;
    uint32_t out_capacity;
    uint64_t base_address;
    const assembler_symbol_t *external_symbols;
    uint32_t external_symbol_count;
    assembler_relocation_t *relocations;
    uint32_t relocation_capacity;
    uint32_t *relocation_count;
} assembler_context_t;

typedef struct {
    int code;
    int bits;
} asm_reg_t;

typedef struct {
    int is_label;
    char label[ASM_LABEL_NAME_SIZE];
    asm_reg_t base;
    int32_t displacement;
    int bits;
} asm_mem_t;

static int char_is_space(char ch) {
    return ch == ' ' || ch == '\t';
}

static int char_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static int char_is_hex_digit(char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }

    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }

    return -1;
}

static const char *skip_const_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim_mutable(char *s) {
    char *end;

    while (char_is_space(*s)) {
        ++s;
    }

    end = s;
    while (*end) {
        ++end;
    }

    while (end > s && char_is_space(end[-1])) {
        --end;
    }

    *end = '\0';
    return s;
}

static void strip_asm_comment(char *line) {
    int in_string = 0;

    while (*line) {
        if (*line == '"') {
            in_string = !in_string;
        } else if (*line == ';' && !in_string) {
            *line = '\0';
            return;
        }

        ++line;
    }
}

static int copy_name_limited(char *dst, const char *src, unsigned int max_len) {
    unsigned int i = 0;

    if (max_len == 0) {
        return -1;
    }

    while (src[i]) {
        if (i + 1 >= max_len) {
            return -1;
        }

        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
    return 0;
}

static int parse_u64(const char *s, uint64_t *out) {
    uint64_t value = 0;
    int base = 10;

    s = skip_const_spaces(s);
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    if (*s == '\0') {
        return -1;
    }

    while (*s) {
        int digit;

        if (base == 16) {
            if (!char_is_hex_digit(*s)) {
                return -1;
            }

            digit = hex_value(*s);
        } else {
            if (!char_is_digit(*s)) {
                return -1;
            }

            digit = *s - '0';
        }

        value = value * (uint64_t)base + (uint64_t)digit;
        ++s;
    }

    *out = value;
    return 0;
}

static int asm_find_label(const asm_label_t *labels, uint32_t label_count, const char *name, uint32_t *out_offset) {
    for (uint32_t i = 0; i < label_count; ++i) {
        if (streq(labels[i].name, name)) {
            *out_offset = labels[i].offset;
            return 0;
        }
    }

    return -1;
}

static int asm_add_label(asm_label_t *labels, uint32_t *label_count, const char *name, uint32_t offset) {
    uint32_t existing_offset = 0;

    if (*name == '\0') {
        return -1;
    }

    if (asm_find_label(labels, *label_count, name, &existing_offset) == 0) {
        return -1;
    }

    if (*label_count >= ASM_MAX_LABELS) {
        return -1;
    }

    if (copy_name_limited(labels[*label_count].name, name, ASM_LABEL_NAME_SIZE) != 0) {
        return -1;
    }

    labels[*label_count].offset = offset;
    ++(*label_count);
    return 0;
}

static int asm_register(const char *name, asm_reg_t *out) {
    static const char *const r64[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    };
    static const char *const r32[] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
    };
    static const char *const r16[] = {
        "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
        "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
    };
    static const char *const r8[] = {
        "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
        "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
    };

    for (int i = 0; i < 16; ++i) {
        if (streq(name, r64[i])) {
            out->code = i;
            out->bits = 64;
            return 0;
        }
        if (streq(name, r32[i])) {
            out->code = i;
            out->bits = 32;
            return 0;
        }
        if (streq(name, r16[i])) {
            out->code = i;
            out->bits = 16;
            return 0;
        }
        if (streq(name, r8[i])) {
            out->code = i;
            out->bits = 8;
            return 0;
        }
    }

    return -1;
}

int assembler_symbol_value(const char *name, uint64_t *out) {
    return kernel_export_value(name, out);
}

static int asm_external_symbol_value(const assembler_context_t *ctx, const char *name, uint64_t *out) {
    uint32_t i;

    if (ctx == 0 || name == 0 || out == 0) {
        return -1;
    }

    for (i = 0; i < ctx->external_symbol_count; ++i) {
        if (ctx->external_symbols[i].name != 0 &&
            streq(ctx->external_symbols[i].name, name)) {
            *out = ctx->external_symbols[i].value;
            return 0;
        }
    }

    return -1;
}

static int asm_add_relocation(const assembler_context_t *ctx,
                              uint32_t offset,
                              uint32_t type,
                              const char *name) {
    uint32_t index;

    if (ctx == 0 || ctx->relocations == 0 || ctx->relocation_count == 0) {
        return 0;
    }

    if (*ctx->relocation_count >= ctx->relocation_capacity) {
        return -1;
    }

    index = *ctx->relocation_count;
    ctx->relocations[index].offset = offset;
    ctx->relocations[index].type = type;
    ctx->relocations[index].name[0] = '\0';
    if (name != 0) {
        if (copy_name_limited(ctx->relocations[index].name,
                              name,
                              sizeof(ctx->relocations[index].name)) != 0) {
            return -1;
        }
    }
    ++(*ctx->relocation_count);
    return 0;
}

static int asm_value(const assembler_context_t *ctx,
                     const char *s,
                     const asm_label_t *labels,
                     uint32_t label_count,
                     uint64_t *out) {
    uint32_t label_offset = 0;

    s = skip_const_spaces(s);
    if (parse_u64(s, out) == 0) {
        return 0;
    }

    if (asm_find_label(labels, label_count, s, &label_offset) == 0) {
        *out = ctx->base_address + label_offset;
        return 0;
    }

    if (asm_external_symbol_value(ctx, s, out) == 0 ||
        assembler_symbol_value(s, out) == 0) {
        return 0;
    }

    return -1;
}

static int asm_emit_byte(const assembler_context_t *ctx, uint32_t *offset, int emit, uint8_t value) {
    if (*offset >= ctx->out_capacity) {
        return -1;
    }

    if (emit) {
        ctx->out[*offset] = value;
    }

    ++(*offset);
    return 0;
}

static int asm_emit_u64(const assembler_context_t *ctx, uint32_t *offset, int emit, uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) {
        if (asm_emit_byte(ctx, offset, emit, (uint8_t)((value >> (i * 8u)) & 0xFFu)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int asm_emit_u32(const assembler_context_t *ctx, uint32_t *offset, int emit, uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) {
        if (asm_emit_byte(ctx, offset, emit, (uint8_t)((value >> (i * 8u)) & 0xFFu)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int asm_emit_u16(const assembler_context_t *ctx, uint32_t *offset, int emit, uint16_t value) {
    for (unsigned int i = 0; i < 2; ++i) {
        if (asm_emit_byte(ctx, offset, emit, (uint8_t)((value >> (i * 8u)) & 0xFFu)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int asm_emit_rex(const assembler_context_t *ctx,
                        uint32_t *offset,
                        int emit,
                        int w,
                        int r,
                        int b) {
    uint8_t rex = (uint8_t)(0x40 |
                            (w ? 0x08 : 0) |
                            ((r & 8) ? 0x04 : 0) |
                            ((b & 8) ? 0x01 : 0));

    if (rex == 0x40) {
        return 0;
    }

    return asm_emit_byte(ctx, offset, emit, rex);
}

static uint32_t asm_rex_size(int w, int r, int b) {
    uint8_t rex = (uint8_t)(0x40 |
                            (w ? 0x08 : 0) |
                            ((r & 8) ? 0x04 : 0) |
                            ((b & 8) ? 0x01 : 0));

    return rex == 0x40 ? 0u : 1u;
}

static int asm_emit_modrm_reg(const assembler_context_t *ctx,
                              uint32_t *offset,
                              int emit,
                              int reg,
                              int rm) {
    return asm_emit_byte(ctx, offset, emit, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

static int asm_emit_modrm_rip_relative(const assembler_context_t *ctx,
                                        uint32_t *offset,
                                        int emit,
                                        int reg,
                                        uint32_t displacement) {
    if (asm_emit_byte(ctx, offset, emit, (uint8_t)(((reg & 7) << 3) | 0x05)) != 0 ||
        (emit && asm_add_relocation(ctx, *offset, ASSEMBLER_RELOC_RIP32, 0) != 0) ||
        asm_emit_u32(ctx, offset, emit, displacement) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_modrm_base_disp32(const assembler_context_t *ctx,
                                      uint32_t *offset,
                                      int emit,
                                      int reg,
                                      asm_reg_t base,
                                      int32_t displacement) {
    uint8_t modrm = (uint8_t)(0x80 | ((reg & 7) << 3) | (base.code & 7));

    if (asm_emit_byte(ctx, offset, emit, modrm) != 0) {
        return -1;
    }

    if ((base.code & 7) == 4) {
        if (asm_emit_byte(ctx, offset, emit, (uint8_t)(0x20 | (base.code & 7))) != 0) {
            return -1;
        }
    }

    return asm_emit_u32(ctx, offset, emit, (uint32_t)displacement);
}

static int asm_emit_reg_reg_op(const assembler_context_t *ctx,
                               uint32_t *offset,
                               int emit,
                               uint8_t opcode,
                               int w,
                               asm_reg_t dst,
                               asm_reg_t src) {
    if (dst.bits != src.bits) {
        return -1;
    }

    if (asm_emit_rex(ctx, offset, emit, w, src.code, dst.code) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode) != 0 ||
        asm_emit_modrm_reg(ctx, offset, emit, src.code, dst.code) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_reg_reg_two_byte_op(const assembler_context_t *ctx,
                                        uint32_t *offset,
                                        int emit,
                                        uint8_t opcode2,
                                        int w,
                                        asm_reg_t dst,
                                        asm_reg_t src) {
    if (dst.bits != src.bits) {
        return -1;
    }

    if (asm_emit_rex(ctx, offset, emit, w, dst.code, src.code) != 0 ||
        asm_emit_byte(ctx, offset, emit, 0x0F) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode2) != 0 ||
        asm_emit_modrm_reg(ctx, offset, emit, dst.code, src.code) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_group1_imm32(const assembler_context_t *ctx,
                                 uint32_t *offset,
                                 int emit,
                                 int group,
                                 int w,
                                 asm_reg_t dst,
                                 uint64_t value) {
    if (value > 0xFFFFFFFFull) {
        return -1;
    }

    if (asm_emit_rex(ctx, offset, emit, w, 0, dst.code) != 0 ||
        asm_emit_byte(ctx, offset, emit, 0x81) != 0 ||
        asm_emit_modrm_reg(ctx, offset, emit, group, dst.code) != 0 ||
        asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
        return -1;
    }

    return 0;
}

static int asm_split_two_args(char *args, char **left, char **right) {
    char *comma = args;

    while (*comma && *comma != ',') {
        ++comma;
    }

    if (*comma != ',') {
        return -1;
    }

    *comma = '\0';
    *left = trim_mutable(args);
    *right = trim_mutable(comma + 1);
    return (**left != '\0' && **right != '\0') ? 0 : -1;
}

static int asm_split_three_args(char *args, char **first, char **second, char **third) {
    char *comma1 = args;
    char *comma2;

    while (*comma1 && *comma1 != ',') {
        ++comma1;
    }

    if (*comma1 != ',') {
        return -1;
    }

    comma2 = comma1 + 1;
    while (*comma2 && *comma2 != ',') {
        ++comma2;
    }

    if (*comma2 != ',') {
        return -1;
    }

    *comma1 = '\0';
    *comma2 = '\0';
    *first = trim_mutable(args);
    *second = trim_mutable(comma1 + 1);
    *third = trim_mutable(comma2 + 1);
    return (**first != '\0' && **second != '\0' && **third != '\0') ? 0 : -1;
}

static const char *asm_parse_mem_size_prefix(const char *text, int *out_bits) {
    const char *s = skip_const_spaces(text);

    *out_bits = 0;
    if (s[0] == 'b' && s[1] == 'y' && s[2] == 't' && s[3] == 'e' && char_is_space(s[4])) {
        *out_bits = 8;
        return skip_const_spaces(s + 4);
    }

    if (s[0] == 'w' && s[1] == 'o' && s[2] == 'r' && s[3] == 'd' && char_is_space(s[4])) {
        *out_bits = 16;
        return skip_const_spaces(s + 4);
    }

    if (s[0] == 'd' && s[1] == 'w' && s[2] == 'o' && s[3] == 'r' && s[4] == 'd' && char_is_space(s[5])) {
        *out_bits = 32;
        return skip_const_spaces(s + 5);
    }

    if (s[0] == 'q' && s[1] == 'w' && s[2] == 'o' && s[3] == 'r' && s[4] == 'd' && char_is_space(s[5])) {
        *out_bits = 64;
        return skip_const_spaces(s + 5);
    }

    return s;
}

static int asm_parse_label_memory(const char *text, char *label, unsigned int label_size, int *out_bits) {
    const char *s = skip_const_spaces(text);
    const char *start;
    const char *end;
    unsigned int len = 0;

    s = asm_parse_mem_size_prefix(s, out_bits);

    if (*s != '[') {
        return -1;
    }

    ++s;
    s = skip_const_spaces(s);
    start = s;
    while (*s && *s != ']' && !char_is_space(*s)) {
        ++s;
    }

    end = s;
    s = skip_const_spaces(s);
    if (*s != ']') {
        return -1;
    }

    ++s;
    s = skip_const_spaces(s);
    if (*s != '\0' || end == start) {
        return -1;
    }

    len = (unsigned int)(end - start);
    if (len + 1u > label_size) {
        return -1;
    }

    for (unsigned int i = 0; i < len; ++i) {
        label[i] = start[i];
    }
    label[len] = '\0';
    return 0;
}

static int asm_parse_signed_i32(const char *text, int32_t *out) {
    uint64_t value = 0;
    int negative = 0;
    const char *s = skip_const_spaces(text);

    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        ++s;
    }

    s = skip_const_spaces(s);
    if (parse_u64(s, &value) != 0) {
        return -1;
    }

    if ((!negative && value > 2147483647ull) ||
        (negative && value > 2147483648ull)) {
        return -1;
    }

    *out = negative ? -(int32_t)value : (int32_t)value;
    return 0;
}

static int asm_parse_memory_operand(const char *text, asm_mem_t *out) {
    const char *s = skip_const_spaces(text);
    char inside[ASM_LINE_SIZE];
    unsigned int len = 0;
    char *op = 0;
    char reg_name[16];
    unsigned int reg_len = 0;
    int reg_name_too_long = 0;
    int bits = 0;

    s = asm_parse_mem_size_prefix(s, &bits);
    out->bits = bits;

    if (*s != '[') {
        return -1;
    }

    ++s;
    s = skip_const_spaces(s);
    while (*s && *s != ']') {
        if (len + 1u >= sizeof(inside)) {
            return -1;
        }
        inside[len++] = *s++;
    }

    if (*s != ']') {
        return -1;
    }

    inside[len] = '\0';
    ++s;
    s = skip_const_spaces(s);
    if (*s != '\0') {
        return -1;
    }

    op = inside;
    while (*op && *op != '+' && *op != '-') {
        ++op;
    }

    while (inside[reg_len] && inside + reg_len != op && !char_is_space(inside[reg_len])) {
        if (reg_len + 1u >= sizeof(reg_name)) {
            reg_name_too_long = 1;
            break;
        }
        reg_name[reg_len] = inside[reg_len];
        ++reg_len;
    }
    reg_name[reg_len] = '\0';

    while (reg_len > 0 && char_is_space(reg_name[reg_len - 1])) {
        reg_name[--reg_len] = '\0';
    }

    if (!reg_name_too_long && asm_register(reg_name, &out->base) == 0 && out->base.bits == 64) {
        out->is_label = 0;
        out->displacement = 0;
        if (*op == '\0') {
            return 0;
        }

        return asm_parse_signed_i32(op, &out->displacement);
    }

    if (asm_parse_label_memory(text, out->label, sizeof(out->label), &out->bits) == 0) {
        out->is_label = 1;
        out->displacement = 0;
        return 0;
    }

    return -1;
}

static int asm_rip_relative_disp32(const assembler_context_t *ctx,
                                   uint32_t next_offset,
                                   const asm_label_t *labels,
                                   uint32_t label_count,
                                   const char *target,
                                   uint32_t *out) {
    uint32_t label_offset = 0;
    int64_t diff;

    if (asm_find_label(labels, label_count, target, &label_offset) != 0) {
        return -1;
    }

    diff = (int64_t)(ctx->base_address + label_offset) -
           (int64_t)(ctx->base_address + next_offset);
    if (diff < -2147483648ll || diff > 2147483647ll) {
        return -1;
    }

    *out = (uint32_t)diff;
    return 0;
}

static int asm_emit_mov_reg_mem_label(const assembler_context_t *ctx,
                                      uint32_t *offset,
                                      int emit,
                                      const asm_label_t *labels,
                                      uint32_t label_count,
                                      asm_reg_t dst,
                                      const char *label) {
    int bits = dst.bits;
    uint32_t displacement = 0;
    uint32_t next_offset = *offset +
                           ((bits == 16) ? 1u : 0u) +
                           asm_rex_size(bits == 64, dst.code, 0) +
                           1u + 1u + 4u;
    uint8_t opcode = (bits == 8) ? 0x8A : 0x8B;

    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        return -1;
    }

    if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, label, &displacement) != 0) {
        return -1;
    }

    if ((bits == 16 && asm_emit_byte(ctx, offset, emit, 0x66) != 0) ||
        asm_emit_rex(ctx, offset, emit, bits == 64, dst.code, 0) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode) != 0 ||
        asm_emit_modrm_rip_relative(ctx, offset, emit, dst.code, displacement) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_mov_reg_mem_base(const assembler_context_t *ctx,
                                     uint32_t *offset,
                                     int emit,
                                     asm_reg_t dst,
                                     asm_mem_t src) {
    int bits = src.bits ? src.bits : dst.bits;
    uint8_t opcode = (bits == 8) ? 0x8A : 0x8B;

    if (src.base.bits != 64 || bits != dst.bits) {
        return -1;
    }

    if ((bits == 16 && asm_emit_byte(ctx, offset, emit, 0x66) != 0) ||
        asm_emit_rex(ctx, offset, emit, bits == 64, dst.code, src.base.code) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode) != 0 ||
        asm_emit_modrm_base_disp32(ctx, offset, emit, dst.code, src.base, src.displacement) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_mov_mem_label_reg(const assembler_context_t *ctx,
                                      uint32_t *offset,
                                      int emit,
                                      const asm_label_t *labels,
                                      uint32_t label_count,
                                      const char *label,
                                      asm_reg_t src) {
    int bits = src.bits;
    uint32_t displacement = 0;
    uint32_t next_offset = *offset +
                           ((bits == 16) ? 1u : 0u) +
                           asm_rex_size(bits == 64, src.code, 0) +
                           1u + 1u + 4u;
    uint8_t opcode = (bits == 8) ? 0x88 : 0x89;

    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        return -1;
    }

    if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, label, &displacement) != 0) {
        return -1;
    }

    if ((bits == 16 && asm_emit_byte(ctx, offset, emit, 0x66) != 0) ||
        asm_emit_rex(ctx, offset, emit, bits == 64, src.code, 0) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode) != 0 ||
        asm_emit_modrm_rip_relative(ctx, offset, emit, src.code, displacement) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_mov_mem_base_reg(const assembler_context_t *ctx,
                                     uint32_t *offset,
                                     int emit,
                                     asm_mem_t dst,
                                     asm_reg_t src) {
    int bits = dst.bits ? dst.bits : src.bits;
    uint8_t opcode = (bits == 8) ? 0x88 : 0x89;

    if (dst.base.bits != 64 || bits != src.bits) {
        return -1;
    }

    if ((bits == 16 && asm_emit_byte(ctx, offset, emit, 0x66) != 0) ||
        asm_emit_rex(ctx, offset, emit, bits == 64, src.code, dst.base.code) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode) != 0 ||
        asm_emit_modrm_base_disp32(ctx, offset, emit, src.code, dst.base, dst.displacement) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_mov_mem_label_imm(const assembler_context_t *ctx,
                                      uint32_t *offset,
                                      int emit,
                                      const asm_label_t *labels,
                                      uint32_t label_count,
                                      const char *label,
                                      asm_mem_t dst,
                                      uint64_t value) {
    int bits = dst.bits ? dst.bits : 64;
    uint32_t displacement = 0;
    uint32_t next_offset = *offset +
                           ((bits == 16) ? 1u : 0u) +
                           asm_rex_size(bits == 64, 0, 0) +
                           1u + 1u + 4u +
                           ((bits == 8) ? 1u : ((bits == 16) ? 2u : 4u));

    if ((bits == 8 && value > 0xFFull) ||
        (bits == 16 && value > 0xFFFFull) ||
        ((bits == 32 || bits == 64) && value > 0xFFFFFFFFull)) {
        return -1;
    }

    if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, label, &displacement) != 0) {
        return -1;
    }

    if ((bits == 16 && asm_emit_byte(ctx, offset, emit, 0x66) != 0) ||
        asm_emit_rex(ctx, offset, emit, bits == 64, 0, 0) != 0 ||
        asm_emit_byte(ctx, offset, emit, (bits == 8) ? 0xC6 : 0xC7) != 0 ||
        asm_emit_modrm_rip_relative(ctx, offset, emit, 0, displacement) != 0) {
        return -1;
    }

    if (bits == 8) {
        return asm_emit_byte(ctx, offset, emit, (uint8_t)value);
    }

    if (bits == 16) {
        return asm_emit_u16(ctx, offset, emit, (uint16_t)value);
    }

    if (asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_mov_mem_base_imm(const assembler_context_t *ctx,
                                     uint32_t *offset,
                                     int emit,
                                     asm_mem_t dst,
                                     uint64_t value) {
    int bits = dst.bits ? dst.bits : 64;

    if (dst.base.bits != 64 ||
        (bits == 8 && value > 0xFFull) ||
        (bits == 16 && value > 0xFFFFull) ||
        ((bits == 32 || bits == 64) && value > 0xFFFFFFFFull)) {
        return -1;
    }

    if ((bits == 16 && asm_emit_byte(ctx, offset, emit, 0x66) != 0) ||
        asm_emit_rex(ctx, offset, emit, bits == 64, 0, dst.base.code) != 0 ||
        asm_emit_byte(ctx, offset, emit, (bits == 8) ? 0xC6 : 0xC7) != 0 ||
        asm_emit_modrm_base_disp32(ctx, offset, emit, 0, dst.base, dst.displacement) != 0) {
        return -1;
    }

    if (bits == 8) {
        return asm_emit_byte(ctx, offset, emit, (uint8_t)value);
    }

    if (bits == 16) {
        return asm_emit_u16(ctx, offset, emit, (uint16_t)value);
    }

    if (asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
        return -1;
    }

    return 0;
}

static int asm_emit_reg_mem_label_op(const assembler_context_t *ctx,
                                     uint32_t *offset,
                                     int emit,
                                     const asm_label_t *labels,
                                     uint32_t label_count,
                                     uint8_t opcode,
                                     asm_reg_t dst,
                                     const char *label) {
    uint32_t displacement = 0;
    uint32_t next_offset = *offset + 7u;

    if (dst.bits != 64) {
        return -1;
    }

    if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, label, &displacement) != 0) {
        return -1;
    }

    if (asm_emit_rex(ctx, offset, emit, 1, dst.code, 0) != 0 ||
        asm_emit_byte(ctx, offset, emit, opcode) != 0 ||
        asm_emit_modrm_rip_relative(ctx, offset, emit, dst.code, displacement) != 0) {
        return -1;
    }

    return 0;
}

static int asm_rel32(const assembler_context_t *ctx,
                     uint32_t next_offset,
                     const asm_label_t *labels,
                     uint32_t label_count,
                     const char *target,
                     uint32_t *out) {
    uint64_t value = 0;
    int64_t diff;

    if (asm_value(ctx, target, labels, label_count, &value) != 0) {
        return -1;
    }

    diff = (int64_t)value - (int64_t)(ctx->base_address + next_offset);
    if (diff < -2147483648ll || diff > 2147483647ll) {
        return -1;
    }

    *out = (uint32_t)diff;
    return 0;
}

static int asm_emit_call_jmp_target(const assembler_context_t *ctx,
                                    uint32_t *offset,
                                    int emit,
                                    const asm_label_t *labels,
                                    uint32_t label_count,
                                    const char *target,
                                    uint8_t rel_opcode,
                                    int reg_group) {
    asm_reg_t reg;
    uint64_t symbol_value = 0;

    if (asm_register(target, &reg) == 0) {
        if (reg.bits != 64) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, 1, 0, reg.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xFF) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, reg_group, reg.code) != 0) {
            return -1;
        }

        return 0;
    }

    if (asm_external_symbol_value(ctx, target, &symbol_value) == 0 ||
        assembler_symbol_value(target, &symbol_value) == 0) {
        uint32_t reloc_offset = *offset + 2u;
        if (asm_emit_rex(ctx, offset, emit, 1, 0, 0) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xB8) != 0 ||
            (emit && asm_external_symbol_value(ctx, target, &symbol_value) == 0 &&
             asm_add_relocation(ctx, reloc_offset, ASSEMBLER_RELOC_ABS64, target) != 0) ||
            asm_emit_u64(ctx, offset, emit, symbol_value) != 0 ||
            asm_emit_rex(ctx, offset, emit, 1, 0, 0) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xFF) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, reg_group, 0) != 0) {
            return -1;
        }

        return 0;
    }

    if (emit) {
        uint32_t rel = 0;
        if (asm_rel32(ctx, *offset + 5u, labels, label_count, target, &rel) != 0) {
            return -1;
        }

        if (asm_emit_byte(ctx, offset, emit, rel_opcode) != 0 ||
            asm_emit_u32(ctx, offset, emit, rel) != 0) {
            return -1;
        }

        return 0;
    }

    *offset += 5u;
    return *offset <= ctx->out_capacity ? 0 : -1;
}

static int asm_jcc_opcode(const char *name, uint8_t *out) {
    if (streq(name, "jo"))  { *out = 0x80; return 0; }
    if (streq(name, "jno")) { *out = 0x81; return 0; }
    if (streq(name, "jb") || streq(name, "jc") || streq(name, "jnae")) { *out = 0x82; return 0; }
    if (streq(name, "jae") || streq(name, "jnb") || streq(name, "jnc")) { *out = 0x83; return 0; }
    if (streq(name, "je") || streq(name, "jz")) { *out = 0x84; return 0; }
    if (streq(name, "jne") || streq(name, "jnz")) { *out = 0x85; return 0; }
    if (streq(name, "jbe") || streq(name, "jna")) { *out = 0x86; return 0; }
    if (streq(name, "ja") || streq(name, "jnbe")) { *out = 0x87; return 0; }
    if (streq(name, "js"))  { *out = 0x88; return 0; }
    if (streq(name, "jns")) { *out = 0x89; return 0; }
    if (streq(name, "jp") || streq(name, "jpe")) { *out = 0x8A; return 0; }
    if (streq(name, "jnp") || streq(name, "jpo")) { *out = 0x8B; return 0; }
    if (streq(name, "jl") || streq(name, "jnge")) { *out = 0x8C; return 0; }
    if (streq(name, "jge") || streq(name, "jnl")) { *out = 0x8D; return 0; }
    if (streq(name, "jle") || streq(name, "jng")) { *out = 0x8E; return 0; }
    if (streq(name, "jg") || streq(name, "jnle")) { *out = 0x8F; return 0; }
    return -1;
}

static int asm_parse_db_byte(char **cursor, uint8_t *out) {
    uint64_t value = 0;
    char *s = trim_mutable(*cursor);
    char *end = s;

    while (*end && *end != ',') {
        ++end;
    }

    if (*end == ',') {
        *end++ = '\0';
    }

    *cursor = end;
    s = trim_mutable(s);

    if (parse_u64(s, &value) != 0 || value > 255) {
        return -1;
    }

    *out = (uint8_t)value;
    return 0;
}

static int asm_emit_db(char *args, const assembler_context_t *ctx, uint32_t *offset, int emit) {
    char *s = trim_mutable(args);

    while (*s) {
        if (*s == '"') {
            ++s;
            while (*s && *s != '"') {
                uint8_t ch;

                if (*s == '\\') {
                    ++s;
                    if (*s == 'n') {
                        ch = '\n';
                    } else if (*s == 'r') {
                        ch = '\r';
                    } else if (*s == 't') {
                        ch = '\t';
                    } else if (*s == '0') {
                        ch = '\0';
                    } else if (*s == '"' || *s == '\\') {
                        ch = (uint8_t)*s;
                    } else {
                        return -1;
                    }
                } else {
                    ch = (uint8_t)*s;
                }

                if (asm_emit_byte(ctx, offset, emit, ch) != 0) {
                    return -1;
                }

                ++s;
            }

            if (*s != '"') {
                return -1;
            }

            ++s;
            s = trim_mutable(s);
            if (*s == ',') {
                ++s;
                s = trim_mutable(s);
            } else if (*s != '\0') {
                return -1;
            }
        } else {
            uint8_t value = 0;
            if (asm_parse_db_byte(&s, &value) != 0) {
                return -1;
            }

            if (asm_emit_byte(ctx, offset, emit, value) != 0) {
                return -1;
            }

            s = trim_mutable(s);
        }
    }

    return 0;
}

static int asm_emit_dq(char *args,
                       const assembler_context_t *ctx,
                       const asm_label_t *labels,
                       uint32_t label_count,
                       uint32_t *offset,
                       int emit) {
    char *s = trim_mutable(args);

    while (*s) {
        char *value_text = s;
        char *end = s;
        uint64_t value = 0;

        while (*end && *end != ',') {
            ++end;
        }

        if (*end == ',') {
            *end++ = '\0';
        }

        s = trim_mutable(end);
        value_text = trim_mutable(value_text);
        if (*value_text == '\0') {
            return -1;
        }

        if (emit && asm_value(ctx, value_text, labels, label_count, &value) != 0) {
            return -1;
        }

        if (asm_emit_u64(ctx, offset, emit, value) != 0) {
            return -1;
        }
    }

    return 0;
}

static int asm_assemble_instruction(char *line,
                                    const asm_label_t *labels,
                                    uint32_t label_count,
                                    const assembler_context_t *ctx,
                                    uint32_t *offset,
                                    int emit) {
    char *space = line;
    char *args;

    while (*space && !char_is_space(*space)) {
        ++space;
    }

    args = space;
    if (*args) {
        *args++ = '\0';
    }
    args = trim_mutable(args);

    if (streq(line, "nop")) {
        return asm_emit_byte(ctx, offset, emit, 0x90);
    }

    if (streq(line, "ret")) {
        uint64_t value = 0;

        if (*args == '\0') {
            return asm_emit_byte(ctx, offset, emit, 0xC3);
        }

        if ((emit && parse_u64(args, &value) != 0) || value > 0xFFFFull) {
            return -1;
        }

        if (asm_emit_byte(ctx, offset, emit, 0xC2) != 0 ||
            asm_emit_u16(ctx, offset, emit, (uint16_t)value) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "iretq")) {
        if (asm_emit_byte(ctx, offset, emit, 0x48) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xCF) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "int")) {
        uint64_t value = 0;

        if ((emit && parse_u64(args, &value) != 0) || value > 0xFFull) {
            return -1;
        }

        if (asm_emit_byte(ctx, offset, emit, 0xCD) != 0 ||
            asm_emit_byte(ctx, offset, emit, (uint8_t)value) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "hlt")) {
        return asm_emit_byte(ctx, offset, emit, 0xF4);
    }

    if (streq(line, "leave")) {
        return asm_emit_byte(ctx, offset, emit, 0xC9);
    }

    if (streq(line, "cqo")) {
        return asm_emit_byte(ctx, offset, emit, 0x48) ||
               asm_emit_byte(ctx, offset, emit, 0x99);
    }

    if (streq(line, "syscall")) {
        return asm_emit_byte(ctx, offset, emit, 0x0F) ||
               asm_emit_byte(ctx, offset, emit, 0x05);
    }

    if (streq(line, "cli")) {
        return asm_emit_byte(ctx, offset, emit, 0xFA);
    }

    if (streq(line, "sti")) {
        return asm_emit_byte(ctx, offset, emit, 0xFB);
    }

    if (streq(line, "db")) {
        return asm_emit_db(args, ctx, offset, emit);
    }

    if (streq(line, "dq")) {
        return asm_emit_dq(args, ctx, labels, label_count, offset, emit);
    }

    if (streq(line, "bits")) {
        return streq(args, "64") ? 0 : -1;
    }

    if (streq(line, "default")) {
        return streq(args, "rel") ? 0 : -1;
    }

    if (streq(line, "global") || streq(line, "section")) {
        return 0;
    }

    if (streq(line, "mov")) {
        char *dst_text;
        char *src_text;
        asm_mem_t dst_mem;
        asm_mem_t src_mem;
        asm_reg_t dst;
        asm_reg_t src;
        uint64_t value = 0;

        if (asm_split_two_args(args, &dst_text, &src_text) != 0) {
            return -1;
        }

        if (asm_parse_memory_operand(dst_text, &dst_mem) == 0) {
            if (asm_register(src_text, &src) == 0) {
                if (dst_mem.is_label) {
                    return asm_emit_mov_mem_label_reg(ctx, offset, emit, labels, label_count, dst_mem.label, src);
                }
                return asm_emit_mov_mem_base_reg(ctx, offset, emit, dst_mem, src);
            }

            if (emit && asm_value(ctx, src_text, labels, label_count, &value) != 0) {
                return -1;
            }

            if (dst_mem.is_label) {
                return asm_emit_mov_mem_label_imm(ctx, offset, emit, labels, label_count, dst_mem.label, dst_mem, value);
            }
            return asm_emit_mov_mem_base_imm(ctx, offset, emit, dst_mem, value);
        }

        if (asm_register(dst_text, &dst) != 0) {
            return -1;
        }

        if (asm_register(src_text, &src) == 0) {
            return asm_emit_reg_reg_op(ctx, offset, emit, 0x89, dst.bits == 64, dst, src);
        }

        if (asm_parse_memory_operand(src_text, &src_mem) == 0) {
            if (src_mem.is_label) {
                return asm_emit_mov_reg_mem_label(ctx, offset, emit, labels, label_count, dst, src_mem.label);
            }
            return asm_emit_mov_reg_mem_base(ctx, offset, emit, dst, src_mem);
        }

        if (emit && asm_value(ctx, src_text, labels, label_count, &value) != 0) {
            return -1;
        }

        if (dst.bits == 64) {
            uint32_t local_label_offset = 0;
            uint32_t reloc_offset = *offset + 2u;
            int is_external = asm_external_symbol_value(ctx, src_text, &value) == 0;
            int is_relative = !is_external && asm_find_label(labels, label_count, src_text, &local_label_offset) == 0;

            if (asm_emit_rex(ctx, offset, emit, 1, 0, dst.code) != 0 ||
                asm_emit_byte(ctx, offset, emit, (uint8_t)(0xB8 + (dst.code & 7))) != 0 ||
                (emit && is_external &&
                 asm_add_relocation(ctx, reloc_offset, ASSEMBLER_RELOC_ABS64, src_text) != 0) ||
                (emit && is_relative &&
                 asm_add_relocation(ctx, reloc_offset, ASSEMBLER_RELOC_RELATIVE64, 0) != 0) ||
                asm_emit_u64(ctx, offset, emit, value) != 0) {
                return -1;
            }

            return 0;
        }

        if (emit && value > 0xFFFFFFFFull) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, 0, 0, dst.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, (uint8_t)(0xB8 + (dst.code & 7))) != 0 ||
            asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "movzx") || streq(line, "movsx") || streq(line, "movsxd")) {
        char *dst_text;
        char *src_text;
        asm_reg_t dst;
        asm_mem_t src_mem;
        uint8_t opcode2 = 0xB6;

        if (asm_split_two_args(args, &dst_text, &src_text) != 0 ||
            asm_register(dst_text, &dst) != 0 ||
            asm_parse_memory_operand(src_text, &src_mem) != 0) {
            return -1;
        }

        if (streq(line, "movzx")) {
            if (dst.bits != 32 || (src_mem.bits != 8 && src_mem.bits != 16)) {
                return -1;
            }
            opcode2 = (src_mem.bits == 8) ? 0xB6 : 0xB7;
            if (src_mem.is_label) {
                uint32_t displacement = 0;
                uint32_t next_offset = *offset +
                                       asm_rex_size(0, dst.code, 0) +
                                       2u + 1u + 4u;

                if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, src_mem.label, &displacement) != 0) {
                    return -1;
                }

                if (asm_emit_rex(ctx, offset, emit, 0, dst.code, 0) != 0 ||
                    asm_emit_byte(ctx, offset, emit, 0x0F) != 0 ||
                    asm_emit_byte(ctx, offset, emit, opcode2) != 0 ||
                    asm_emit_modrm_rip_relative(ctx, offset, emit, dst.code, displacement) != 0) {
                    return -1;
                }
            } else {
                if (asm_emit_rex(ctx, offset, emit, 0, dst.code, src_mem.base.code) != 0 ||
                    asm_emit_byte(ctx, offset, emit, 0x0F) != 0 ||
                    asm_emit_byte(ctx, offset, emit, opcode2) != 0 ||
                    asm_emit_modrm_base_disp32(ctx, offset, emit, dst.code, src_mem.base, src_mem.displacement) != 0) {
                    return -1;
                }
            }
            return 0;
        }

        if (streq(line, "movsx")) {
            if (dst.bits != 64 || (src_mem.bits != 8 && src_mem.bits != 16)) {
                return -1;
            }
            opcode2 = (src_mem.bits == 8) ? 0xBE : 0xBF;
            if (src_mem.is_label) {
                uint32_t displacement = 0;
                uint32_t next_offset = *offset +
                                       asm_rex_size(1, dst.code, 0) +
                                       2u + 1u + 4u;

                if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, src_mem.label, &displacement) != 0) {
                    return -1;
                }

                if (asm_emit_rex(ctx, offset, emit, 1, dst.code, 0) != 0 ||
                    asm_emit_byte(ctx, offset, emit, 0x0F) != 0 ||
                    asm_emit_byte(ctx, offset, emit, opcode2) != 0 ||
                    asm_emit_modrm_rip_relative(ctx, offset, emit, dst.code, displacement) != 0) {
                    return -1;
                }
            } else {
                if (asm_emit_rex(ctx, offset, emit, 1, dst.code, src_mem.base.code) != 0 ||
                    asm_emit_byte(ctx, offset, emit, 0x0F) != 0 ||
                    asm_emit_byte(ctx, offset, emit, opcode2) != 0 ||
                    asm_emit_modrm_base_disp32(ctx, offset, emit, dst.code, src_mem.base, src_mem.displacement) != 0) {
                    return -1;
                }
            }
            return 0;
        }

        if (dst.bits != 64 || src_mem.bits != 32) {
            return -1;
        }

        if (src_mem.is_label) {
            uint32_t displacement = 0;
            uint32_t next_offset = *offset +
                                   asm_rex_size(1, dst.code, 0) +
                                   1u + 1u + 4u;

            if (emit && asm_rip_relative_disp32(ctx, next_offset, labels, label_count, src_mem.label, &displacement) != 0) {
                return -1;
            }

            if (asm_emit_rex(ctx, offset, emit, 1, dst.code, 0) != 0 ||
                asm_emit_byte(ctx, offset, emit, 0x63) != 0 ||
                asm_emit_modrm_rip_relative(ctx, offset, emit, dst.code, displacement) != 0) {
                return -1;
            }
        } else {
            if (asm_emit_rex(ctx, offset, emit, 1, dst.code, src_mem.base.code) != 0 ||
                asm_emit_byte(ctx, offset, emit, 0x63) != 0 ||
                asm_emit_modrm_base_disp32(ctx, offset, emit, dst.code, src_mem.base, src_mem.displacement) != 0) {
                return -1;
            }
        }

        return 0;
    }

    if (streq(line, "imul")) {
        char *dst_text;
        char *src_text;
        char *imm_text;
        asm_reg_t dst;
        asm_reg_t src;
        uint64_t value = 0;

        if (asm_split_three_args(args, &dst_text, &src_text, &imm_text) == 0) {
            if (asm_register(dst_text, &dst) != 0 ||
                asm_register(src_text, &src) != 0 ||
                dst.bits != src.bits) {
                return -1;
            }

            if (emit && asm_value(ctx, imm_text, labels, label_count, &value) != 0) {
                return -1;
            }

            if (value > 0xFFFFFFFFull) {
                return -1;
            }

            if (asm_emit_rex(ctx, offset, emit, dst.bits == 64, dst.code, src.code) != 0 ||
                asm_emit_byte(ctx, offset, emit, 0x69) != 0 ||
                asm_emit_modrm_reg(ctx, offset, emit, dst.code, src.code) != 0 ||
                asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
                return -1;
            }

            return 0;
        }

        if (asm_split_two_args(args, &dst_text, &src_text) != 0 ||
            asm_register(dst_text, &dst) != 0) {
            return -1;
        }

        if (asm_register(src_text, &src) == 0) {
            return asm_emit_reg_reg_two_byte_op(ctx, offset, emit, 0xAF, dst.bits == 64, dst, src);
        }

        if (emit && asm_value(ctx, src_text, labels, label_count, &value) != 0) {
            return -1;
        }

        if (value > 0xFFFFFFFFull) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, dst.bits == 64, dst.code, dst.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0x69) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, dst.code, dst.code) != 0 ||
            asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "add") || streq(line, "sub") || streq(line, "cmp") ||
        streq(line, "and") || streq(line, "or") || streq(line, "xor") ||
        streq(line, "test")) {
        char *dst_text;
        char *src_text;
        asm_reg_t dst;
        asm_reg_t src;
        char src_label[ASM_LABEL_NAME_SIZE];
        uint64_t value = 0;
        uint8_t opcode = 0x01;
        uint8_t mem_opcode = 0x03;
        int group = 0;

        if (asm_split_two_args(args, &dst_text, &src_text) != 0 ||
            asm_register(dst_text, &dst) != 0) {
            return -1;
        }

        if (streq(line, "add")) { opcode = 0x01; mem_opcode = 0x03; group = 0; }
        if (streq(line, "or"))  { opcode = 0x09; mem_opcode = 0x0B; group = 1; }
        if (streq(line, "and")) { opcode = 0x21; mem_opcode = 0x23; group = 4; }
        if (streq(line, "sub")) { opcode = 0x29; mem_opcode = 0x2B; group = 5; }
        if (streq(line, "xor")) { opcode = 0x31; mem_opcode = 0x33; group = 6; }
        if (streq(line, "cmp")) { opcode = 0x39; mem_opcode = 0x3B; group = 7; }
        if (streq(line, "test")) { opcode = 0x85; mem_opcode = 0x85; group = 0; }

        if (asm_register(src_text, &src) == 0) {
            return asm_emit_reg_reg_op(ctx, offset, emit, opcode, dst.bits == 64, dst, src);
        }

        {
            int src_mem_bits = 0;
            if (asm_parse_label_memory(src_text, src_label, sizeof(src_label), &src_mem_bits) == 0) {
                return asm_emit_reg_mem_label_op(ctx, offset, emit, labels, label_count, mem_opcode, dst, src_label);
            }
        }

        if (emit && asm_value(ctx, src_text, labels, label_count, &value) != 0) {
            return -1;
        }

        if (streq(line, "test")) {
            if (value > 0xFFFFFFFFull) {
                return -1;
            }

            if (asm_emit_rex(ctx, offset, emit, dst.bits == 64, 0, dst.code) != 0 ||
                asm_emit_byte(ctx, offset, emit, 0xF7) != 0 ||
                asm_emit_modrm_reg(ctx, offset, emit, 0, dst.code) != 0 ||
                asm_emit_u32(ctx, offset, emit, (uint32_t)value) != 0) {
                return -1;
            }

            return 0;
        }

        return asm_emit_group1_imm32(ctx, offset, emit, group, dst.bits == 64, dst, value);
    }

    if (streq(line, "inc") || streq(line, "dec")) {
        asm_reg_t reg;
        int group = streq(line, "inc") ? 0 : 1;

        if (asm_register(args, &reg) != 0) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, reg.bits == 64, 0, reg.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xFF) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, group, reg.code) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "idiv")) {
        asm_reg_t reg;

        if (asm_register(args, &reg) != 0 || reg.bits != 64) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, 1, 0, reg.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xF7) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, 7, reg.code) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "not")) {
        asm_reg_t reg;

        if (asm_register(args, &reg) != 0) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, reg.bits == 64, 0, reg.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xF7) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, 2, reg.code) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "shl") || streq(line, "shr") || streq(line, "sar")) {
        char *dst_text;
        char *src_text;
        asm_reg_t dst;
        asm_reg_t src;
        uint64_t value = 0;
        int group = 4;

        if (asm_split_two_args(args, &dst_text, &src_text) != 0 ||
            asm_register(dst_text, &dst) != 0) {
            return -1;
        }

        if (streq(line, "shl")) group = 4;
        if (streq(line, "shr")) group = 5;
        if (streq(line, "sar")) group = 7;

        if (asm_register(src_text, &src) == 0) {
            if (src.bits != 8 || src.code != 1) {
                return -1;
            }
            if (asm_emit_rex(ctx, offset, emit, dst.bits == 64, 0, dst.code) != 0 ||
                asm_emit_byte(ctx, offset, emit, 0xD3) != 0 ||
                asm_emit_modrm_reg(ctx, offset, emit, group, dst.code) != 0) {
                return -1;
            }
            return 0;
        }

        if (emit && asm_value(ctx, src_text, labels, label_count, &value) != 0) {
            return -1;
        }

        if (value > 255ull) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, dst.bits == 64, 0, dst.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, 0xC1) != 0 ||
            asm_emit_modrm_reg(ctx, offset, emit, group, dst.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, (uint8_t)value) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "push") || streq(line, "pop")) {
        asm_reg_t reg;
        uint8_t opcode = streq(line, "push") ? 0x50 : 0x58;

        if (asm_register(args, &reg) != 0 || reg.bits != 64) {
            return -1;
        }

        if (asm_emit_rex(ctx, offset, emit, 0, 0, reg.code) != 0 ||
            asm_emit_byte(ctx, offset, emit, (uint8_t)(opcode + (reg.code & 7))) != 0) {
            return -1;
        }

        return 0;
    }

    if (streq(line, "call") || streq(line, "jmp")) {
        return asm_emit_call_jmp_target(ctx,
                                        offset,
                                        emit,
                                        labels,
                                        label_count,
                                        args,
                                        streq(line, "call") ? 0xE8 : 0xE9,
                                        streq(line, "call") ? 2 : 4);
    }

    {
        uint8_t jcc_opcode = 0;
        if (asm_jcc_opcode(line, &jcc_opcode) == 0) {
            if (emit) {
                uint32_t rel = 0;
                if (asm_rel32(ctx, *offset + 6u, labels, label_count, args, &rel) != 0) {
                    return -1;
                }

                if (asm_emit_byte(ctx, offset, emit, 0x0F) != 0 ||
                    asm_emit_byte(ctx, offset, emit, jcc_opcode) != 0 ||
                    asm_emit_u32(ctx, offset, emit, rel) != 0) {
                    return -1;
                }

                return 0;
            }

            *offset += 6u;
            return *offset <= ctx->out_capacity ? 0 : -1;
        }
    }

    return -1;
}

static int asm_assemble_line(char *line,
                             asm_label_t *labels,
                             uint32_t *label_count,
                             const assembler_context_t *ctx,
                             uint32_t *offset,
                             int emit) {
    char *colon;
    char *space;

    strip_asm_comment(line);
    line = trim_mutable(line);
    if (*line == '\0') {
        return 0;
    }

    colon = line;
    while (*colon && !char_is_space(*colon) && *colon != ':') {
        ++colon;
    }

    if (*colon == ':') {
        *colon = '\0';
        if (!emit && asm_add_label(labels, label_count, line, *offset) != 0) {
            return -1;
        }

        line = trim_mutable(colon + 1);
        if (*line == '\0') {
            return 0;
        }
    }

    space = line;
    while (*space && !char_is_space(*space)) {
        ++space;
    }

    if (*space) {
        char *directive = trim_mutable(space + 1);
        char *directive_end = directive;

        while (*directive_end && !char_is_space(*directive_end)) {
            ++directive_end;
        }

        if ((directive_end - directive == 2) &&
            directive[0] == 'd' &&
            (directive[1] == 'b' || directive[1] == 'q')) {
            *space = '\0';
            if (!emit && asm_add_label(labels, label_count, line, *offset) != 0) {
                return -1;
            }

            return asm_assemble_instruction(directive, labels, *label_count, ctx, offset, emit);
        }
    }

    return asm_assemble_instruction(line, labels, *label_count, ctx, offset, emit);
}

static int asm_next_line(const char *source, uint32_t size, uint32_t *pos, char *line) {
    uint32_t len = 0;

    if (*pos >= size) {
        return 0;
    }

    while (*pos < size) {
        char ch = source[*pos];
        ++(*pos);

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            break;
        }

        if (len + 1 < ASM_LINE_SIZE) {
            line[len++] = ch;
        }
    }

    line[len] = '\0';
    return 1;
}

int assembler_assemble_source(const char *source,
                              uint32_t size,
                              unsigned char *out,
                              uint32_t out_capacity,
                              uint64_t base_address,
                              uint32_t *out_size) {
    return assembler_assemble_source_ex(source,
                                        size,
                                        out,
                                        out_capacity,
                                        base_address,
                                        out_size,
                                        0);
}

int assembler_assemble_source_ex(const char *source,
                                 uint32_t size,
                                 unsigned char *out,
                                 uint32_t out_capacity,
                                 uint64_t base_address,
                                 uint32_t *out_size,
                                 uint32_t *error_line) {
    return assembler_assemble_source_ex_symbols(source,
                                                size,
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
                                         uint32_t export_count) {
    return assembler_assemble_source_ex_relocs(source,
                                               size,
                                               out,
                                               out_capacity,
                                               base_address,
                                               out_size,
                                               error_line,
                                               external_symbols,
                                               external_symbol_count,
                                               export_names,
                                               export_values,
                                               export_count,
                                               0,
                                               0,
                                               0);
}

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
                                        uint32_t *relocation_count) {
    assembler_context_t ctx = {
        out,
        out_capacity,
        base_address,
        external_symbols,
        external_symbol_count,
        relocations,
        relocation_capacity,
        relocation_count,
    };
    asm_label_t *labels = asm_work_labels;
    uint32_t label_count = 0;
    uint32_t offset = 0;
    uint32_t pos = 0;
    uint32_t line_number = 0;
    char line[ASM_LINE_SIZE];

    if (source == 0 || out == 0 || out_size == 0 || out_capacity == 0) {
        return -1;
    }

    if (error_line) {
        *error_line = 0;
    }
    if (relocation_count) {
        *relocation_count = 0;
    }

    while (asm_next_line(source, size, &pos, line)) {
        ++line_number;
        if (asm_assemble_line(line, labels, &label_count, &ctx, &offset, 0) != 0) {
            if (error_line) {
                *error_line = line_number;
            }
            return -1;
        }
    }

    if ((export_count != 0 && (export_names == 0 || export_values == 0)) ||
        (external_symbol_count != 0 && external_symbols == 0)) {
        return -1;
    }

    for (uint32_t i = 0; i < export_count; ++i) {
        if (export_names[i] == 0 ||
            asm_value(&ctx, export_names[i], labels, label_count, &export_values[i]) != 0) {
            return -1;
        }
    }

    offset = 0;
    pos = 0;
    line_number = 0;
    while (asm_next_line(source, size, &pos, line)) {
        ++line_number;
        if (asm_assemble_line(line, labels, &label_count, &ctx, &offset, 1) != 0) {
            if (error_line) {
                *error_line = line_number;
            }
            return -1;
        }
    }

    *out_size = offset;
    return 0;
}

int assembler_measure_source(const char *source,
                             uint32_t size,
                             uint32_t *out_size,
                             uint32_t *error_line) {
    return assembler_measure_source_ex_symbols(source,
                                               size,
                                               out_size,
                                               error_line,
                                               0,
                                               0);
}

int assembler_measure_source_ex_symbols(const char *source,
                                        uint32_t size,
                                        uint32_t *out_size,
                                        uint32_t *error_line,
                                        const assembler_symbol_t *external_symbols,
                                        uint32_t external_symbol_count) {
    assembler_context_t ctx = {
        0,
        0xFFFFFFFFu,
        0,
        external_symbols,
        external_symbol_count,
        0,
        0,
        0,
    };
    asm_label_t *labels = asm_work_labels;
    uint32_t label_count = 0;
    uint32_t offset = 0;
    uint32_t pos = 0;
    uint32_t line_number = 0;
    char line[ASM_LINE_SIZE];

    if (source == 0 || out_size == 0) {
        return -1;
    }

    if (error_line) {
        *error_line = 0;
    }

    while (asm_next_line(source, size, &pos, line)) {
        ++line_number;
        if (asm_assemble_line(line, labels, &label_count, &ctx, &offset, 0) != 0) {
            if (error_line) {
                *error_line = line_number;
            }
            return -1;
        }
    }

    *out_size = offset;
    return 0;
}
