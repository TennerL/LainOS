#include "assembler.h"
#include "kernel.h"

#define ASM_LINE_SIZE 128u
#define ASM_MAX_LABELS 32u
#define ASM_LABEL_NAME_SIZE 24u

typedef struct {
    char name[ASM_LABEL_NAME_SIZE];
    uint32_t offset;
} asm_label_t;

typedef struct {
    unsigned char *out;
    uint32_t out_capacity;
    uint64_t base_address;
} assembler_context_t;

typedef struct {
    int code;
    int bits;
} asm_reg_t;

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
    }

    return -1;
}

static int asm_symbol_value(const char *name, uint64_t *out) {
    if (streq(name, "puts")) {
        *out = (uint64_t)(uintptr_t)console_puts;
        return 0;
    }

    if (streq(name, "put_hex64")) {
        *out = (uint64_t)(uintptr_t)console_put_hex64;
        return 0;
    }

    if (streq(name, "put_dec64")) {
        *out = (uint64_t)(uintptr_t)console_put_dec64;
        return 0;
    }

    if (streq(name, "ticks")) {
        *out = (uint64_t)(uintptr_t)timer_ticks;
        return 0;
    }

    return -1;
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

    if (asm_symbol_value(s, out) == 0) {
        return 0;
    }

    if (asm_find_label(labels, label_count, s, &label_offset) == 0) {
        *out = ctx->base_address + label_offset;
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

static int asm_emit_modrm_reg(const assembler_context_t *ctx,
                              uint32_t *offset,
                              int emit,
                              int reg,
                              int rm) {
    return asm_emit_byte(ctx, offset, emit, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
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
        asm_reg_t dst;
        asm_reg_t src;
        uint64_t value = 0;

        if (asm_split_two_args(args, &dst_text, &src_text) != 0 ||
            asm_register(dst_text, &dst) != 0) {
            return -1;
        }

        if (asm_register(src_text, &src) == 0) {
            return asm_emit_reg_reg_op(ctx, offset, emit, 0x89, dst.bits == 64, dst, src);
        }

        if (emit && asm_value(ctx, src_text, labels, label_count, &value) != 0) {
            return -1;
        }

        if (dst.bits == 64) {
            if (asm_emit_rex(ctx, offset, emit, 1, 0, dst.code) != 0 ||
                asm_emit_byte(ctx, offset, emit, (uint8_t)(0xB8 + (dst.code & 7))) != 0 ||
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

    if (streq(line, "add") || streq(line, "sub") || streq(line, "cmp") ||
        streq(line, "and") || streq(line, "or") || streq(line, "xor") ||
        streq(line, "test")) {
        char *dst_text;
        char *src_text;
        asm_reg_t dst;
        asm_reg_t src;
        uint64_t value = 0;
        uint8_t opcode = 0x01;
        int group = 0;

        if (asm_split_two_args(args, &dst_text, &src_text) != 0 ||
            asm_register(dst_text, &dst) != 0) {
            return -1;
        }

        if (streq(line, "add")) { opcode = 0x01; group = 0; }
        if (streq(line, "or"))  { opcode = 0x09; group = 1; }
        if (streq(line, "and")) { opcode = 0x21; group = 4; }
        if (streq(line, "sub")) { opcode = 0x29; group = 5; }
        if (streq(line, "xor")) { opcode = 0x31; group = 6; }
        if (streq(line, "cmp")) { opcode = 0x39; group = 7; }
        if (streq(line, "test")) { opcode = 0x85; group = 0; }

        if (asm_register(src_text, &src) == 0) {
            return asm_emit_reg_reg_op(ctx, offset, emit, opcode, dst.bits == 64, dst, src);
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
    assembler_context_t ctx = {
        out,
        out_capacity,
        base_address,
    };
    asm_label_t labels[ASM_MAX_LABELS];
    uint32_t label_count = 0;
    uint32_t offset = 0;
    uint32_t pos = 0;
    char line[ASM_LINE_SIZE];

    if (source == 0 || out == 0 || out_size == 0 || out_capacity == 0) {
        return -1;
    }

    while (asm_next_line(source, size, &pos, line)) {
        if (asm_assemble_line(line, labels, &label_count, &ctx, &offset, 0) != 0) {
            return -1;
        }
    }

    offset = 0;
    pos = 0;
    while (asm_next_line(source, size, &pos, line)) {
        if (asm_assemble_line(line, labels, &label_count, &ctx, &offset, 1) != 0) {
            return -1;
        }
    }

    *out_size = offset;
    return 0;
}
