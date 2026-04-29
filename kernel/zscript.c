#include "zscript.h"

#define Z_MAX_TOKEN_TEXT 32u
#define Z_MAX_FUNCTIONS 64u
#define Z_MAX_LOCALS 64u
#define Z_MAX_PARAMS 6u
#define Z_MAX_STRINGS 64u
#define Z_STRING_POOL_SIZE 4096u
#define Z_MAX_ARRAY_DIMS 3u
#define Z_MAX_STRUCTS 32u
#define Z_MAX_STRUCT_FIELDS 16u
#define Z_MAX_GLOBALS 32u
#define Z_MAX_LOOP_DEPTH 16u

typedef enum {
    Z_TOKEN_EOF = 0,
    Z_TOKEN_IDENT,
    Z_TOKEN_NUMBER,
    Z_TOKEN_STRING,
    Z_TOKEN_LPAREN,
    Z_TOKEN_RPAREN,
    Z_TOKEN_LBRACE,
    Z_TOKEN_RBRACE,
    Z_TOKEN_LBRACKET,
    Z_TOKEN_RBRACKET,
    Z_TOKEN_DOT,
    Z_TOKEN_SEMI,
    Z_TOKEN_COMMA,
    Z_TOKEN_ASSIGN,
    Z_TOKEN_AMP,
    Z_TOKEN_AMP_AMP,
    Z_TOKEN_PIPE_PIPE,
    Z_TOKEN_BANG,
    Z_TOKEN_PLUS,
    Z_TOKEN_PLUS_PLUS,
    Z_TOKEN_PLUS_ASSIGN,
    Z_TOKEN_MINUS,
    Z_TOKEN_ARROW,
    Z_TOKEN_MINUS_MINUS,
    Z_TOKEN_MINUS_ASSIGN,
    Z_TOKEN_STAR,
    Z_TOKEN_STAR_ASSIGN,
    Z_TOKEN_SLASH,
    Z_TOKEN_SLASH_ASSIGN,
    Z_TOKEN_PERCENT,
    Z_TOKEN_PERCENT_ASSIGN,
    Z_TOKEN_EQ,
    Z_TOKEN_NE,
    Z_TOKEN_LT,
    Z_TOKEN_LE,
    Z_TOKEN_GT,
    Z_TOKEN_GE,
} z_token_type_t;

typedef struct {
    z_token_type_t type;
    uint64_t number;
    char text[Z_MAX_TOKEN_TEXT];
    uint32_t line;
} z_token_t;

typedef enum {
    Z_TYPE_VOID = 0,
    Z_TYPE_INT,
    Z_TYPE_I8,
    Z_TYPE_I16,
    Z_TYPE_I32,
    Z_TYPE_I64,
    Z_TYPE_U8,
    Z_TYPE_U16,
    Z_TYPE_U32,
    Z_TYPE_U64,
    Z_TYPE_STRUCT,
} z_type_kind_t;

typedef struct {
    z_type_kind_t kind;
    uint32_t pointer_depth;
    int32_t struct_index;
} z_type_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    int32_t stack_offset;
    uint32_t array_length;
    uint32_t dim_count;
    uint32_t dims[Z_MAX_ARRAY_DIMS];
    z_type_t type;
    int32_t struct_index;
    uint32_t total_size_bytes;
} z_local_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    uint32_t offset;
    z_type_t type;
} z_struct_field_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    uint32_t size_bytes;
    uint32_t field_count;
    z_struct_field_t fields[Z_MAX_STRUCT_FIELDS];
} z_struct_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    char label[20];
} z_function_t;

typedef struct {
    char label[16];
    uint32_t offset;
    uint32_t length;
} z_string_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    char label[20];
    z_type_t type;
    uint32_t array_length;
    uint32_t dim_count;
    uint32_t dims[Z_MAX_ARRAY_DIMS];
    int32_t struct_index;
    uint32_t total_size_bytes;
    uint64_t init_value;
    int has_init;
} z_global_t;

typedef struct {
    char break_label[16];
    char continue_label[16];
} z_loop_t;

typedef struct {
    const char *source;
    uint32_t size;
    uint32_t pos;
    uint32_t line;
    z_token_t current;
    char *out;
    uint32_t out_capacity;
    uint32_t out_size;
    uint32_t error_line;
    uint32_t label_counter;
    uint32_t string_pool_used;
    uint32_t string_count;
    uint32_t function_count;
    uint32_t struct_count;
    uint32_t global_count;
    z_string_t strings[Z_MAX_STRINGS];
    z_function_t functions[Z_MAX_FUNCTIONS];
    z_struct_t structs[Z_MAX_STRUCTS];
    z_global_t globals[Z_MAX_GLOBALS];
    char string_pool[Z_STRING_POOL_SIZE];
    int in_function;
    char current_function_label[20];
    char current_exit_label[16];
    uint32_t local_count;
    z_local_t locals[Z_MAX_LOCALS];
    int32_t next_stack_offset;
    uint32_t loop_depth;
    z_loop_t loops[Z_MAX_LOOP_DEPTH];
} z_compiler_t;

static const char *const z_arg_registers[Z_MAX_PARAMS] = {
    "rdi", "rsi", "rdx", "rcx", "r8", "r9",
};

static int z_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int z_char_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static int z_char_is_hex_digit(char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int z_char_is_ident_start(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           ch == '_';
}

static int z_char_is_ident_continue(char ch) {
    return z_char_is_ident_start(ch) || z_char_is_digit(ch);
}

static int z_hex_value(char ch) {
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

static int z_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void z_copy_text(char *dst, const char *src) {
    uint32_t i = 0;

    while (src[i]) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static z_type_t z_make_type(z_type_kind_t kind, uint32_t pointer_depth, int32_t struct_index) {
    z_type_t type;
    type.kind = kind;
    type.pointer_depth = pointer_depth;
    type.struct_index = struct_index;
    return type;
}

static int z_type_is_scalar(const z_type_t *type) {
    return type->pointer_depth == 0 &&
           type->kind != Z_TYPE_VOID &&
           type->kind != Z_TYPE_STRUCT;
}

static int z_type_is_signed(const z_type_t *type) {
    return type->pointer_depth == 0 &&
           (type->kind == Z_TYPE_INT ||
            type->kind == Z_TYPE_I8 ||
            type->kind == Z_TYPE_I16 ||
            type->kind == Z_TYPE_I32 ||
            type->kind == Z_TYPE_I64);
}

static uint32_t z_type_bit_width(const z_type_t *type) {
    if (type->pointer_depth != 0) {
        return 64u;
    }

    switch (type->kind) {
        case Z_TYPE_I8:
        case Z_TYPE_U8:
            return 8u;
        case Z_TYPE_I16:
        case Z_TYPE_U16:
            return 16u;
        case Z_TYPE_I32:
        case Z_TYPE_U32:
            return 32u;
        case Z_TYPE_VOID:
            return 0u;
        case Z_TYPE_INT:
        case Z_TYPE_I64:
        case Z_TYPE_U64:
        case Z_TYPE_STRUCT:
        default:
            return 64u;
    }
}

static z_type_t z_type_pointee(z_type_t type) {
    if (type.pointer_depth != 0) {
        --type.pointer_depth;
    }
    return type;
}

static int z_type_is_unsigned(const z_type_t *type) {
    return type->pointer_depth == 0 &&
           (type->kind == Z_TYPE_U8 ||
            type->kind == Z_TYPE_U16 ||
            type->kind == Z_TYPE_U32 ||
            type->kind == Z_TYPE_U64);
}

static int z_type_is_integer_like(const z_type_t *type) {
    return type->pointer_depth != 0 ||
           type->kind == Z_TYPE_INT ||
           type->kind == Z_TYPE_I8 ||
           type->kind == Z_TYPE_I16 ||
           type->kind == Z_TYPE_I32 ||
           type->kind == Z_TYPE_I64 ||
           type->kind == Z_TYPE_U8 ||
           type->kind == Z_TYPE_U16 ||
           type->kind == Z_TYPE_U32 ||
           type->kind == Z_TYPE_U64;
}

static int z_type_is_struct_value(const z_type_t *type) {
    return type->kind == Z_TYPE_STRUCT &&
           type->pointer_depth == 0 &&
           type->struct_index >= 0;
}

static int z_type_is_struct_pointer(const z_type_t *type) {
    return type->kind == Z_TYPE_STRUCT &&
           type->pointer_depth != 0 &&
           type->struct_index >= 0;
}

static z_type_t z_type_for_bits(uint32_t bits, int is_unsigned) {
    if (bits <= 8u) {
        return z_make_type(is_unsigned ? Z_TYPE_U8 : Z_TYPE_I8, 0, -1);
    }
    if (bits <= 16u) {
        return z_make_type(is_unsigned ? Z_TYPE_U16 : Z_TYPE_I16, 0, -1);
    }
    if (bits <= 32u) {
        return z_make_type(is_unsigned ? Z_TYPE_U32 : Z_TYPE_I32, 0, -1);
    }
    return z_make_type(is_unsigned ? Z_TYPE_U64 : Z_TYPE_I64, 0, -1);
}

static z_type_t z_type_promote_binary(z_type_t left, z_type_t right) {
    uint32_t left_bits;
    uint32_t right_bits;
    uint32_t bits;
    int is_unsigned;

    if (left.pointer_depth != 0 || right.pointer_depth != 0) {
        return z_make_type(Z_TYPE_I64, 0, -1);
    }

    left_bits = z_type_bit_width(&left);
    right_bits = z_type_bit_width(&right);
    bits = (left_bits > right_bits) ? left_bits : right_bits;
    if (bits < 32u) {
        bits = 32u;
    }

    is_unsigned = z_type_is_unsigned(&left) || z_type_is_unsigned(&right);
    if (left.kind == Z_TYPE_INT || right.kind == Z_TYPE_INT) {
        if (!is_unsigned && bits < 64u) {
            bits = 64u;
        }
    }

    return z_type_for_bits(bits, is_unsigned);
}

static uint32_t z_type_storage_size_bytes(const z_compiler_t *c, z_type_t type) {
    if (type.pointer_depth != 0) {
        return 8u;
    }

    switch (type.kind) {
        case Z_TYPE_I8:
        case Z_TYPE_U8:
            return 1u;
        case Z_TYPE_I16:
        case Z_TYPE_U16:
            return 2u;
        case Z_TYPE_I32:
        case Z_TYPE_U32:
            return 4u;
        case Z_TYPE_VOID:
            return 0u;
        case Z_TYPE_STRUCT:
            if (type.struct_index >= 0) {
                return c->structs[type.struct_index].size_bytes;
            }
            return 0u;
        case Z_TYPE_INT:
        case Z_TYPE_I64:
        case Z_TYPE_U64:
        default:
            return 8u;
    }
}

static uint32_t z_align_up_u32(uint32_t value, uint32_t alignment) {
    if (alignment == 0u) {
        return value;
    }

    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void z_set_error(z_compiler_t *c, uint32_t line) {
    if (c->error_line == 0) {
        c->error_line = line;
    }
}

static int z_emit_char(z_compiler_t *c, char ch) {
    if (c->out_size >= c->out_capacity) {
        z_set_error(c, c->current.line ? c->current.line : c->line);
        return -1;
    }

    c->out[c->out_size++] = ch;
    return 0;
}

static int z_emit_text(z_compiler_t *c, const char *text) {
    while (*text) {
        if (z_emit_char(c, *text++) != 0) {
            return -1;
        }
    }

    return 0;
}

static int z_emit_u64(z_compiler_t *c, uint64_t value) {
    char digits[32];
    uint32_t count = 0;

    if (value == 0) {
        return z_emit_char(c, '0');
    }

    while (value != 0) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (count > 0) {
        if (z_emit_char(c, digits[--count]) != 0) {
            return -1;
        }
    }

    return 0;
}

static int z_emit_i32(z_compiler_t *c, int32_t value) {
    if (value < 0) {
        if (z_emit_char(c, '-') != 0) {
            return -1;
        }
        return z_emit_u64(c, (uint64_t)(-(int64_t)value));
    }

    return z_emit_u64(c, (uint64_t)value);
}

static int z_emit_line(z_compiler_t *c, const char *text) {
    return z_emit_text(c, text) || z_emit_char(c, '\n');
}

static int z_emit_label(z_compiler_t *c, const char *label) {
    return z_emit_text(c, label) || z_emit_text(c, ":\n");
}

static int z_emit_instr0(z_compiler_t *c, const char *op) {
    return z_emit_text(c, "    ") ||
           z_emit_text(c, op) ||
           z_emit_char(c, '\n');
}

static int z_emit_instr1_text(z_compiler_t *c, const char *op, const char *arg) {
    return z_emit_text(c, "    ") ||
           z_emit_text(c, op) ||
           z_emit_char(c, ' ') ||
           z_emit_text(c, arg) ||
           z_emit_char(c, '\n');
}

static int z_emit_instr2_text(z_compiler_t *c, const char *op, const char *a, const char *b) {
    return z_emit_text(c, "    ") ||
           z_emit_text(c, op) ||
           z_emit_char(c, ' ') ||
           z_emit_text(c, a) ||
           z_emit_text(c, ", ") ||
           z_emit_text(c, b) ||
           z_emit_char(c, '\n');
}

static int z_emit_instr2_u64(z_compiler_t *c, const char *op, const char *a, uint64_t value) {
    return z_emit_text(c, "    ") ||
           z_emit_text(c, op) ||
           z_emit_char(c, ' ') ||
           z_emit_text(c, a) ||
           z_emit_text(c, ", ") ||
           z_emit_u64(c, value) ||
           z_emit_char(c, '\n');
}

static int z_emit_imul_rax_imm(z_compiler_t *c, uint64_t value) {
    return z_emit_text(c, "    imul rax, rax, ") ||
           z_emit_u64(c, value) ||
           z_emit_char(c, '\n');
}

static const char *z_rax_alias_for_bits(uint32_t bits) {
    if (bits == 8u) {
        return "al";
    }
    if (bits == 16u) {
        return "ax";
    }
    if (bits == 32u) {
        return "eax";
    }
    return "rax";
}

static const char *z_mem_prefix_for_bits(uint32_t bits) {
    if (bits == 8u) {
        return "byte ";
    }
    if (bits == 16u) {
        return "word ";
    }
    if (bits == 32u) {
        return "dword ";
    }
    return "qword ";
}

static int z_emit_sized_offset_memory(z_compiler_t *c, uint32_t bits, int32_t offset) {
    return z_emit_text(c, z_mem_prefix_for_bits(bits)) ||
           z_emit_text(c, "[rbp") ||
           z_emit_i32(c, offset) ||
           z_emit_char(c, ']');
}

static int z_emit_sized_reg_memory(z_compiler_t *c, uint32_t bits, const char *reg) {
    return z_emit_text(c, z_mem_prefix_for_bits(bits)) ||
           z_emit_char(c, '[') ||
           z_emit_text(c, reg) ||
           z_emit_char(c, ']');
}

static int z_emit_sized_label_memory(z_compiler_t *c, uint32_t bits, const char *label) {
    return z_emit_text(c, z_mem_prefix_for_bits(bits)) ||
           z_emit_char(c, '[') ||
           z_emit_text(c, label) ||
           z_emit_char(c, ']');
}

static int z_emit_normalize_rax_for_type(z_compiler_t *c, z_type_t type) {
    uint32_t bits;

    if (!z_type_is_integer_like(&type) || type.pointer_depth != 0) {
        return 0;
    }

    bits = z_type_bit_width(&type);
    if (bits >= 64u) {
        return 0;
    }

    if (z_type_is_unsigned(&type)) {
        uint64_t mask = (bits == 32u) ? 0xFFFFFFFFull : ((1ull << bits) - 1ull);
        return z_emit_instr2_u64(c, "and", "rax", mask);
    }

    return z_emit_text(c, "    shl rax, ") ||
           z_emit_u64(c, 64u - bits) ||
           z_emit_char(c, '\n') ||
           z_emit_text(c, "    sar rax, ") ||
           z_emit_u64(c, 64u - bits) ||
           z_emit_char(c, '\n');
}

static int z_emit_store_rax_to_offset(z_compiler_t *c, int32_t offset) {
    return z_emit_text(c, "    mov [rbp") ||
           z_emit_i32(c, offset) ||
           z_emit_text(c, "], rax\n");
}

static int z_emit_store_rax_to_offset_typed(z_compiler_t *c, int32_t offset, z_type_t type) {
    uint32_t bits = z_type_bit_width(&type);

    if (!z_type_is_scalar(&type) || bits == 64u) {
        return z_emit_text(c, "    mov ") ||
               z_emit_sized_offset_memory(c, 64u, offset) ||
               z_emit_text(c, ", rax\n");
    }

    return z_emit_text(c, "    mov ") ||
           z_emit_sized_offset_memory(c, bits, offset) ||
           z_emit_text(c, ", ") ||
           z_emit_text(c, z_rax_alias_for_bits(bits)) ||
           z_emit_char(c, '\n');
}

static int z_emit_store_rax_to_reg_ptr_typed(z_compiler_t *c, const char *reg, z_type_t type) {
    uint32_t bits = z_type_bit_width(&type);

    if (!z_type_is_scalar(&type) || bits == 64u) {
        return z_emit_text(c, "    mov ") ||
               z_emit_sized_reg_memory(c, 64u, reg) ||
               z_emit_text(c, ", rax\n");
    }

    return z_emit_text(c, "    mov ") ||
           z_emit_sized_reg_memory(c, bits, reg) ||
           z_emit_text(c, ", ") ||
           z_emit_text(c, z_rax_alias_for_bits(bits)) ||
           z_emit_char(c, '\n');
}

static int z_emit_store_rax_to_label_typed(z_compiler_t *c, const char *label, z_type_t type) {
    uint32_t bits = z_type_bit_width(&type);

    if (!z_type_is_scalar(&type) || bits == 64u) {
        return z_emit_text(c, "    mov ") ||
               z_emit_sized_label_memory(c, 64u, label) ||
               z_emit_text(c, ", rax\n");
    }

    return z_emit_text(c, "    mov ") ||
           z_emit_sized_label_memory(c, bits, label) ||
           z_emit_text(c, ", ") ||
           z_emit_text(c, z_rax_alias_for_bits(bits)) ||
           z_emit_char(c, '\n');
}

static int z_emit_load_rax_from_offset_typed(z_compiler_t *c, int32_t offset, z_type_t type) {
    uint32_t bits = z_type_bit_width(&type);

    if (!z_type_is_scalar(&type) || bits == 64u) {
        return z_emit_text(c, "    mov rax, ") ||
               z_emit_sized_offset_memory(c, 64u, offset) ||
               z_emit_char(c, '\n');
    }

    if (bits == 32u) {
        if (z_type_is_signed(&type)) {
            return z_emit_text(c, "    movsxd rax, ") ||
                   z_emit_sized_offset_memory(c, 32u, offset) ||
                   z_emit_char(c, '\n');
        }

        return z_emit_text(c, "    mov eax, ") ||
               z_emit_sized_offset_memory(c, 32u, offset) ||
               z_emit_char(c, '\n');
    }

    if (z_type_is_signed(&type)) {
        return z_emit_text(c, "    movsx rax, ") ||
               z_emit_sized_offset_memory(c, bits, offset) ||
               z_emit_char(c, '\n');
    }

    return z_emit_text(c, "    movzx eax, ") ||
           z_emit_sized_offset_memory(c, bits, offset) ||
           z_emit_char(c, '\n');
}

static int z_emit_load_rax_from_reg_ptr_typed(z_compiler_t *c, const char *reg, z_type_t type) {
    uint32_t bits = z_type_bit_width(&type);

    if (!z_type_is_scalar(&type) || bits == 64u) {
        return z_emit_text(c, "    mov rax, ") ||
               z_emit_sized_reg_memory(c, 64u, reg) ||
               z_emit_char(c, '\n');
    }

    if (bits == 32u) {
        if (z_type_is_signed(&type)) {
            return z_emit_text(c, "    movsxd rax, ") ||
                   z_emit_sized_reg_memory(c, 32u, reg) ||
                   z_emit_char(c, '\n');
        }

        return z_emit_text(c, "    mov eax, ") ||
               z_emit_sized_reg_memory(c, 32u, reg) ||
               z_emit_char(c, '\n');
    }

    if (z_type_is_signed(&type)) {
        return z_emit_text(c, "    movsx rax, ") ||
               z_emit_sized_reg_memory(c, bits, reg) ||
               z_emit_char(c, '\n');
    }

    return z_emit_text(c, "    movzx eax, ") ||
           z_emit_sized_reg_memory(c, bits, reg) ||
           z_emit_char(c, '\n');
}

static int z_emit_load_rax_from_label_typed(z_compiler_t *c, const char *label, z_type_t type) {
    uint32_t bits = z_type_bit_width(&type);

    if (!z_type_is_scalar(&type) || bits == 64u) {
        return z_emit_text(c, "    mov rax, ") ||
               z_emit_sized_label_memory(c, 64u, label) ||
               z_emit_char(c, '\n');
    }

    if (bits == 32u) {
        if (z_type_is_signed(&type)) {
            return z_emit_text(c, "    movsxd rax, ") ||
                   z_emit_sized_label_memory(c, 32u, label) ||
                   z_emit_char(c, '\n');
        }

        return z_emit_text(c, "    mov eax, ") ||
               z_emit_sized_label_memory(c, 32u, label) ||
               z_emit_char(c, '\n');
    }

    if (z_type_is_signed(&type)) {
        return z_emit_text(c, "    movsx rax, ") ||
               z_emit_sized_label_memory(c, bits, label) ||
               z_emit_char(c, '\n');
    }

    return z_emit_text(c, "    movzx eax, ") ||
           z_emit_sized_label_memory(c, bits, label) ||
           z_emit_char(c, '\n');
}

static int z_emit_address_of_offset(z_compiler_t *c, int32_t offset) {
    if (z_emit_instr2_text(c, "mov", "rax", "rbp") != 0) {
        return -1;
    }

    if (offset < 0) {
        return z_emit_instr2_u64(c, "sub", "rax", (uint64_t)(-(int64_t)offset));
    }

    return z_emit_instr2_u64(c, "add", "rax", (uint64_t)offset);
}

static int z_emit_push_rax(z_compiler_t *c) {
    return z_emit_instr1_text(c, "push", "rax");
}

static int z_emit_pop_reg(z_compiler_t *c, const char *reg) {
    return z_emit_instr1_text(c, "pop", reg);
}

static void z_make_prefixed_label(char *out, const char *prefix, uint32_t value) {
    char digits[16];
    uint32_t count = 0;
    uint32_t i = 0;

    while (prefix[i]) {
        out[i] = prefix[i];
        ++i;
    }

    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);

    while (count > 0) {
        out[i++] = digits[--count];
    }

    out[i] = '\0';
}

static void z_make_label(z_compiler_t *c, char *out) {
    z_make_prefixed_label(out, "zl", c->label_counter++);
}

static int z_push_loop(z_compiler_t *c, const char *break_label, const char *continue_label) {
    if (c->loop_depth >= Z_MAX_LOOP_DEPTH) {
        z_set_error(c, c->current.line);
        return -1;
    }

    z_copy_text(c->loops[c->loop_depth].break_label, break_label);
    z_copy_text(c->loops[c->loop_depth].continue_label, continue_label);
    ++c->loop_depth;
    return 0;
}

static void z_pop_loop(z_compiler_t *c) {
    if (c->loop_depth != 0) {
        --c->loop_depth;
    }
}

static const z_loop_t *z_current_loop(z_compiler_t *c) {
    if (c->loop_depth == 0) {
        return 0;
    }

    return &c->loops[c->loop_depth - 1u];
}

static void z_skip_ws_and_comments(z_compiler_t *c) {
    for (;;) {
        while (c->pos < c->size && z_char_is_space(c->source[c->pos])) {
            if (c->source[c->pos] == '\n') {
                ++c->line;
            }
            ++c->pos;
        }

        if (c->pos + 1 < c->size &&
            c->source[c->pos] == '/' &&
            c->source[c->pos + 1] == '/') {
            c->pos += 2;
            while (c->pos < c->size && c->source[c->pos] != '\n') {
                ++c->pos;
            }
            continue;
        }

        if (c->pos + 1 < c->size &&
            c->source[c->pos] == '/' &&
            c->source[c->pos + 1] == '*') {
            c->pos += 2;
            while (c->pos + 1 < c->size &&
                   !(c->source[c->pos] == '*' && c->source[c->pos + 1] == '/')) {
                if (c->source[c->pos] == '\n') {
                    ++c->line;
                }
                ++c->pos;
            }

            if (c->pos + 1 >= c->size) {
                z_set_error(c, c->line);
                return;
            }

            c->pos += 2;
            continue;
        }

        break;
    }
}

static int z_read_number(z_compiler_t *c, z_token_t *token) {
    uint64_t value = 0;
    int base = 10;

    if (c->source[c->pos] == '0' &&
        c->pos + 1 < c->size &&
        (c->source[c->pos + 1] == 'x' || c->source[c->pos + 1] == 'X')) {
        base = 16;
        c->pos += 2;
        if (c->pos >= c->size || !z_char_is_hex_digit(c->source[c->pos])) {
            return -1;
        }
    }

    while (c->pos < c->size) {
        char ch = c->source[c->pos];
        int digit = -1;

        if (base == 16) {
            if (!z_char_is_hex_digit(ch)) {
                break;
            }
            digit = z_hex_value(ch);
        } else {
            if (!z_char_is_digit(ch)) {
                break;
            }
            digit = ch - '0';
        }

        value = value * (uint64_t)base + (uint64_t)digit;
        ++c->pos;
    }

    token->type = Z_TOKEN_NUMBER;
    token->number = value;
    return 0;
}

static int z_read_ident(z_compiler_t *c, z_token_t *token) {
    uint32_t len = 0;

    while (c->pos < c->size && z_char_is_ident_continue(c->source[c->pos])) {
        if (len + 1u >= Z_MAX_TOKEN_TEXT) {
            return -1;
        }

        token->text[len++] = c->source[c->pos++];
    }

    token->text[len] = '\0';
    token->type = Z_TOKEN_IDENT;
    return 0;
}

static int z_read_string(z_compiler_t *c, z_token_t *token) {
    uint32_t used = c->string_pool_used;
    uint32_t start;

    ++c->pos;
    start = used;

    while (c->pos < c->size) {
        char ch = c->source[c->pos++];

        if (ch == '"') {
            if (used >= Z_STRING_POOL_SIZE) {
                return -1;
            }

            c->string_pool[used++] = '\0';
            token->type = Z_TOKEN_STRING;
            token->number = start;
            c->string_pool_used = used;
            return 0;
        }

        if (ch == '\\') {
            if (c->pos >= c->size) {
                return -1;
            }

            ch = c->source[c->pos++];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == '0') {
                ch = '\0';
            } else if (ch != '"' && ch != '\\') {
                return -1;
            }
        }

        if (ch == '\n') {
            ++c->line;
        }

        if (used + 1u >= Z_STRING_POOL_SIZE) {
            return -1;
        }

        c->string_pool[used++] = ch;
    }

    return -1;
}

static int z_next_token(z_compiler_t *c) {
    z_token_t token;

    z_skip_ws_and_comments(c);
    if (c->error_line != 0) {
        return -1;
    }

    token.type = Z_TOKEN_EOF;
    token.number = 0;
    token.text[0] = '\0';
    token.line = c->line;

    if (c->pos >= c->size) {
        c->current = token;
        return 0;
    }

    {
        char ch = c->source[c->pos++];

        if (z_char_is_ident_start(ch)) {
            --c->pos;
            if (z_read_ident(c, &token) != 0) {
                z_set_error(c, token.line);
                return -1;
            }

            c->current = token;
            return 0;
        }

        if (z_char_is_digit(ch)) {
            --c->pos;
            if (z_read_number(c, &token) != 0) {
                z_set_error(c, token.line);
                return -1;
            }

            c->current = token;
            return 0;
        }

        if (ch == '"') {
            --c->pos;
            if (z_read_string(c, &token) != 0) {
                z_set_error(c, token.line);
                return -1;
            }

            c->current = token;
            return 0;
        }

        if (ch == '(') token.type = Z_TOKEN_LPAREN;
        else if (ch == ')') token.type = Z_TOKEN_RPAREN;
        else if (ch == '{') token.type = Z_TOKEN_LBRACE;
        else if (ch == '}') token.type = Z_TOKEN_RBRACE;
        else if (ch == '[') token.type = Z_TOKEN_LBRACKET;
        else if (ch == ']') token.type = Z_TOKEN_RBRACKET;
        else if (ch == '.') token.type = Z_TOKEN_DOT;
        else if (ch == ';') token.type = Z_TOKEN_SEMI;
        else if (ch == ',') token.type = Z_TOKEN_COMMA;
        else if (ch == '+') {
            if (c->pos < c->size && c->source[c->pos] == '+') {
                ++c->pos;
                token.type = Z_TOKEN_PLUS_PLUS;
            } else if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_PLUS_ASSIGN;
            } else {
                token.type = Z_TOKEN_PLUS;
            }
        }
        else if (ch == '-') {
            if (c->pos < c->size && c->source[c->pos] == '>') {
                ++c->pos;
                token.type = Z_TOKEN_ARROW;
            } else if (c->pos < c->size && c->source[c->pos] == '-') {
                ++c->pos;
                token.type = Z_TOKEN_MINUS_MINUS;
            } else if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_MINUS_ASSIGN;
            } else {
                token.type = Z_TOKEN_MINUS;
            }
        }
        else if (ch == '*') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_STAR_ASSIGN;
            } else {
                token.type = Z_TOKEN_STAR;
            }
        }
        else if (ch == '/') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_SLASH_ASSIGN;
            } else {
                token.type = Z_TOKEN_SLASH;
            }
        }
        else if (ch == '%') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_PERCENT_ASSIGN;
            } else {
                token.type = Z_TOKEN_PERCENT;
            }
        }
        else if (ch == '=') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_EQ;
            } else {
                token.type = Z_TOKEN_ASSIGN;
            }
        } else if (ch == '&') {
            if (c->pos < c->size && c->source[c->pos] == '&') {
                ++c->pos;
                token.type = Z_TOKEN_AMP_AMP;
            } else {
                token.type = Z_TOKEN_AMP;
            }
        } else if (ch == '|') {
            if (c->pos < c->size && c->source[c->pos] == '|') {
                ++c->pos;
                token.type = Z_TOKEN_PIPE_PIPE;
            } else {
                z_set_error(c, token.line);
                return -1;
            }
        } else if (ch == '!') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_NE;
            } else {
                token.type = Z_TOKEN_BANG;
            }
        } else if (ch == '<') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_LE;
            } else {
                token.type = Z_TOKEN_LT;
            }
        } else if (ch == '>') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_GE;
            } else {
                token.type = Z_TOKEN_GT;
            }
        } else {
            z_set_error(c, token.line);
            return -1;
        }
    }

    c->current = token;
    return 0;
}

static int z_expect(z_compiler_t *c, z_token_type_t type) {
    if (c->current.type != type) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return z_next_token(c);
}

static int z_expect_ident(z_compiler_t *c, char *out_name) {
    if (c->current.type != Z_TOKEN_IDENT) {
        z_set_error(c, c->current.line);
        return -1;
    }

    z_copy_text(out_name, c->current.text);
    return z_next_token(c);
}

static int z_find_local(const z_compiler_t *c, const char *name) {
    uint32_t i;

    for (i = 0; i < c->local_count; ++i) {
        if (z_streq(c->locals[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int z_find_global(const z_compiler_t *c, const char *name) {
    uint32_t i;

    for (i = 0; i < c->global_count; ++i) {
        if (z_streq(c->globals[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int z_find_function(const z_compiler_t *c, const char *name) {
    uint32_t i;

    for (i = 0; i < c->function_count; ++i) {
        if (z_streq(c->functions[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int z_find_struct(const z_compiler_t *c, const char *name) {
    uint32_t i;

    for (i = 0; i < c->struct_count; ++i) {
        if (z_streq(c->structs[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int z_find_struct_field(const z_struct_t *s, const char *name) {
    uint32_t i;

    for (i = 0; i < s->field_count; ++i) {
        if (z_streq(s->fields[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int z_ensure_function(z_compiler_t *c, const char *name, char *out_label) {
    int index = z_find_function(c, name);

    if (index >= 0) {
        z_copy_text(out_label, c->functions[index].label);
        return 0;
    }

    if (c->function_count >= Z_MAX_FUNCTIONS) {
        return -1;
    }

    z_copy_text(c->functions[c->function_count].name, name);
    z_make_prefixed_label(c->functions[c->function_count].label, "zf", c->function_count);
    z_copy_text(out_label, c->functions[c->function_count].label);
    ++c->function_count;
    return 0;
}

static int z_add_local(z_compiler_t *c,
                       const char *name,
                       z_type_t type,
                       uint32_t array_length,
                       const uint32_t *dims,
                       uint32_t dim_count,
                       int32_t struct_index,
                       uint32_t total_size_bytes) {
    int32_t slot_bytes;
    uint32_t i;

    if (!c->in_function || c->local_count >= Z_MAX_LOCALS || z_find_local(c, name) >= 0) {
        return -1;
    }

    slot_bytes = (int32_t)total_size_bytes;
    c->next_stack_offset -= slot_bytes;
    z_copy_text(c->locals[c->local_count].name, name);
    c->locals[c->local_count].stack_offset = c->next_stack_offset;
    c->locals[c->local_count].array_length = array_length;
    c->locals[c->local_count].dim_count = dim_count;
    c->locals[c->local_count].type = type;
    c->locals[c->local_count].struct_index = struct_index;
    c->locals[c->local_count].total_size_bytes = total_size_bytes;
    for (i = 0; i < Z_MAX_ARRAY_DIMS; ++i) {
        c->locals[c->local_count].dims[i] = (i < dim_count) ? dims[i] : 0;
    }
    ++c->local_count;
    return (int)(c->local_count - 1u);
}

static int z_add_global(z_compiler_t *c,
                        const char *name,
                        z_type_t type,
                        uint32_t array_length,
                        const uint32_t *dims,
                        uint32_t dim_count,
                        int32_t struct_index,
                        uint32_t total_size_bytes,
                        uint64_t init_value,
                        int has_init) {
    uint32_t i;

    if (c->global_count >= Z_MAX_GLOBALS ||
        z_find_global(c, name) >= 0 ||
        z_find_function(c, name) >= 0) {
        return -1;
    }

    z_copy_text(c->globals[c->global_count].name, name);
    z_make_prefixed_label(c->globals[c->global_count].label, "zg", c->global_count);
    c->globals[c->global_count].type = type;
    c->globals[c->global_count].array_length = array_length;
    c->globals[c->global_count].dim_count = dim_count;
    c->globals[c->global_count].struct_index = struct_index;
    c->globals[c->global_count].total_size_bytes = total_size_bytes;
    c->globals[c->global_count].init_value = init_value;
    c->globals[c->global_count].has_init = has_init;
    for (i = 0; i < Z_MAX_ARRAY_DIMS; ++i) {
        c->globals[c->global_count].dims[i] = (i < dim_count) ? dims[i] : 0;
    }

    ++c->global_count;
    return (int)(c->global_count - 1u);
}

static int z_add_string(z_compiler_t *c, uint32_t offset) {
    uint32_t length = 0;

    if (c->string_count >= Z_MAX_STRINGS) {
        return -1;
    }

    while (c->string_pool[offset + length] != '\0') {
        ++length;
    }

    c->strings[c->string_count].offset = offset;
    c->strings[c->string_count].length = length;
    z_make_prefixed_label(c->strings[c->string_count].label, "zs", c->string_count);
    ++c->string_count;
    return (int)(c->string_count - 1u);
}

static int z_parse_expr(z_compiler_t *c, z_type_t *out_type);
static int z_parse_statement(z_compiler_t *c);

static int z_parse_struct_type_name(z_compiler_t *c, char *out_name) {
    if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "struct"))) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (z_next_token(c) != 0) {
        return -1;
    }

    return z_expect_ident(c, out_name);
}

static int z_ident_type_kind(const char *name, z_type_kind_t *out_kind) {
    if (z_streq(name, "void")) {
        *out_kind = Z_TYPE_VOID;
        return 0;
    }
    if (z_streq(name, "int")) {
        *out_kind = Z_TYPE_INT;
        return 0;
    }
    if (z_streq(name, "int8_t")) {
        *out_kind = Z_TYPE_I8;
        return 0;
    }
    if (z_streq(name, "int16_t")) {
        *out_kind = Z_TYPE_I16;
        return 0;
    }
    if (z_streq(name, "int32_t")) {
        *out_kind = Z_TYPE_I32;
        return 0;
    }
    if (z_streq(name, "int64_t")) {
        *out_kind = Z_TYPE_I64;
        return 0;
    }
    if (z_streq(name, "uint8_t")) {
        *out_kind = Z_TYPE_U8;
        return 0;
    }
    if (z_streq(name, "uint16_t")) {
        *out_kind = Z_TYPE_U16;
        return 0;
    }
    if (z_streq(name, "uint32_t")) {
        *out_kind = Z_TYPE_U32;
        return 0;
    }
    if (z_streq(name, "uint64_t")) {
        *out_kind = Z_TYPE_U64;
        return 0;
    }

    return -1;
}

static int z_current_is_type_name(const z_compiler_t *c) {
    z_type_kind_t kind;

    if (c->current.type != Z_TOKEN_IDENT) {
        return 0;
    }

    if (z_streq(c->current.text, "struct")) {
        return 1;
    }

    return z_ident_type_kind(c->current.text, &kind) == 0;
}

static int z_parse_type(z_compiler_t *c, z_type_t *out_type) {
    z_type_kind_t kind;
    z_type_t type;
    char struct_name[Z_MAX_TOKEN_TEXT];
    int struct_index = -1;

    if (!z_current_is_type_name(c)) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (z_streq(c->current.text, "struct")) {
        if (z_parse_struct_type_name(c, struct_name) != 0) {
            return -1;
        }

        struct_index = z_find_struct(c, struct_name);
        if (struct_index < 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        type = z_make_type(Z_TYPE_STRUCT, 0, struct_index);
    } else {
        if (z_ident_type_kind(c->current.text, &kind) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        type = z_make_type(kind, 0, -1);
        if (z_next_token(c) != 0) {
            return -1;
        }
    }

    while (c->current.type == Z_TOKEN_STAR) {
        ++type.pointer_depth;
        if (z_next_token(c) != 0) {
            return -1;
        }
    }

    *out_type = type;
    return 0;
}

static int z_parse_typed_name(z_compiler_t *c, z_type_t *out_type, char *out_name) {
    if (z_parse_type(c, out_type) != 0) {
        return -1;
    }

    return z_expect_ident(c, out_name);
}

static int z_parse_array_decl_suffixes(z_compiler_t *c,
                                       uint32_t *out_length,
                                       uint32_t *out_dims,
                                       uint32_t *out_dim_count) {
    uint32_t total = 0;
    uint32_t dim_count = 0;

    *out_length = 0;
    *out_dim_count = 0;

    while (c->current.type == Z_TOKEN_LBRACKET) {
        if (dim_count >= Z_MAX_ARRAY_DIMS) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type != Z_TOKEN_NUMBER || c->current.number == 0 || c->current.number > 1024u) {
            z_set_error(c, c->current.line);
            return -1;
        }

        out_dims[dim_count++] = (uint32_t)c->current.number;
        total = (total == 0) ? (uint32_t)c->current.number : total * (uint32_t)c->current.number;
        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_RBRACKET) != 0) {
            return -1;
        }
    }

    *out_length = total;
    *out_dim_count = dim_count;
    return 0;
}

static int z_current_starts_cast(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    z_token_t saved_current = c->current;
    z_type_t ignored_type;
    int result = 0;

    if (c->current.type != Z_TOKEN_LPAREN) {
        return 0;
    }

    if (z_next_token(c) == 0 &&
        z_parse_type(c, &ignored_type) == 0 &&
        c->current.type == Z_TOKEN_RPAREN) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_function_definition(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    z_type_t type;
    int result = 0;

    if (!z_current_is_type_name(c)) {
        return 0;
    }

    if (z_parse_typed_name(c, &type, name) == 0 &&
        c->current.type == Z_TOKEN_LPAREN) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_struct_definition(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    int result = 0;

    if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "struct"))) {
        return 0;
    }

    if (z_parse_struct_type_name(c, name) == 0 &&
        c->current.type == Z_TOKEN_LBRACE) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_global_definition(const z_compiler_t *c) {
    return c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "global");
}

static int z_parse_global_definition(z_compiler_t *c) {
    char name[Z_MAX_TOKEN_TEXT];
    z_type_t type;
    uint32_t array_length = 0;
    uint32_t dims[Z_MAX_ARRAY_DIMS];
    uint32_t dim_count = 0;
    uint32_t type_size = 0;
    uint32_t total_size = 0;
    uint64_t init_value = 0;
    int has_init = 0;

    if (!z_current_starts_global_definition(c) ||
        z_next_token(c) != 0 ||
        z_parse_typed_name(c, &type, name) != 0 ||
        z_parse_array_decl_suffixes(c, &array_length, dims, &dim_count) != 0) {
        return -1;
    }

    if (type.kind == Z_TYPE_VOID && type.pointer_depth == 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    type_size = z_type_storage_size_bytes(c, type);
    if (type_size == 0u) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (array_length != 0) {
        total_size = array_length * ((type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0) ? type_size : 8u);
    } else if (type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0) {
        total_size = z_align_up_u32(type_size, 8u);
    } else {
        total_size = type_size;
    }

    if (c->current.type == Z_TOKEN_ASSIGN) {
        if (array_length != 0 || (type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0)) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_next_token(c) != 0 || c->current.type != Z_TOKEN_NUMBER) {
            z_set_error(c, c->current.line);
            return -1;
        }

        init_value = c->current.number;
        has_init = 1;
        if (z_next_token(c) != 0) {
            return -1;
        }
    }

    if (z_expect(c, Z_TOKEN_SEMI) != 0) {
        return -1;
    }

    if (z_add_global(c,
                     name,
                     type,
                     array_length,
                     dims,
                     dim_count,
                     type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0 ? type.struct_index : -1,
                     total_size,
                     init_value,
                     has_init) < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static int z_parse_struct_definition(z_compiler_t *c) {
    char struct_name[Z_MAX_TOKEN_TEXT];
    int struct_index;

    if (z_parse_struct_type_name(c, struct_name) != 0) {
        return -1;
    }

    if (c->current.type != Z_TOKEN_LBRACE || c->struct_count >= Z_MAX_STRUCTS || z_find_struct(c, struct_name) >= 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    struct_index = (int)c->struct_count;
    z_copy_text(c->structs[struct_index].name, struct_name);
    c->structs[struct_index].size_bytes = 0;
    c->structs[struct_index].field_count = 0;
    ++c->struct_count;

    if (z_expect(c, Z_TOKEN_LBRACE) != 0) {
        return -1;
    }

    while (c->current.type != Z_TOKEN_RBRACE) {
        char field_name[Z_MAX_TOKEN_TEXT];
        z_type_t field_type;
        uint32_t field_index;
        uint32_t field_size = 0;

        if (z_parse_typed_name(c, &field_type, field_name) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (field_type.kind == Z_TYPE_VOID && field_type.pointer_depth == 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (c->structs[struct_index].field_count >= Z_MAX_STRUCT_FIELDS ||
            z_find_struct_field(&c->structs[struct_index], field_name) >= 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        field_size = z_type_storage_size_bytes(c, field_type);
        if (field_size == 0u) {
            z_set_error(c, c->current.line);
            return -1;
        }

        field_index = c->structs[struct_index].field_count++;
        z_copy_text(c->structs[struct_index].fields[field_index].name, field_name);
        c->structs[struct_index].fields[field_index].offset = c->structs[struct_index].size_bytes;
        c->structs[struct_index].fields[field_index].type = field_type;
        c->structs[struct_index].size_bytes += field_size;

        if (z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }
    }

    if (z_expect(c, Z_TOKEN_RBRACE) != 0 ||
        z_expect(c, Z_TOKEN_SEMI) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_call_expression(z_compiler_t *c, const char *name) {
    uint32_t arg_count = 0;
    char function_label[20];

    if (z_streq(name, "ticks") ||
        z_streq(name, "status_memory_total_kb") ||
        z_streq(name, "status_memory_free_kb") ||
        z_streq(name, "status_memory_used_kb") ||
        z_streq(name, "status_cpu_core_count") ||
        z_streq(name, "mem_total_kb") ||
        z_streq(name, "mem_free_kb") ||
        z_streq(name, "mem_used_kb") ||
        z_streq(name, "cpu_count")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr1_text(c, "call", name) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "cpu_usage")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
            z_emit_instr0(c, "call cpu_usage") != 0) {
            return -1;
        }

        return 0;
    }

    if (z_expect(c, Z_TOKEN_LPAREN) != 0) {
        return -1;
    }

    if (c->current.type != Z_TOKEN_RPAREN) {
        for (;;) {
            if (arg_count >= Z_MAX_PARAMS) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_parse_expr(c, 0) != 0 ||
                z_emit_push_rax(c) != 0) {
                return -1;
            }

            ++arg_count;
            if (c->current.type != Z_TOKEN_COMMA) {
                break;
            }

            if (z_next_token(c) != 0) {
                return -1;
            }
        }
    }

    if (z_expect(c, Z_TOKEN_RPAREN) != 0 ||
        z_ensure_function(c, name, function_label) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    while (arg_count > 0) {
        --arg_count;
        if (z_emit_pop_reg(c, z_arg_registers[arg_count]) != 0) {
            return -1;
        }
    }

    return z_emit_instr1_text(c, "call", function_label);
}

static int z_emit_local_base_address(z_compiler_t *c, int local_index) {
    if (c->locals[local_index].array_length != 0) {
        return z_emit_address_of_offset(c, c->locals[local_index].stack_offset);
    }

    return z_emit_load_rax_from_offset_typed(c,
                                             c->locals[local_index].stack_offset,
                                             c->locals[local_index].type);
}

static int z_find_field_for_struct_type(z_compiler_t *c,
                                        z_type_t type,
                                        const char *field_name,
                                        uint32_t *out_field_offset,
                                        z_type_t *out_field_type) {
    int field_index;

    if (!z_type_is_struct_value(&type)) {
        z_set_error(c, c->current.line);
        return -1;
    }

    field_index = z_find_struct_field(&c->structs[type.struct_index], field_name);
    if (field_index < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    *out_field_offset = c->structs[type.struct_index].fields[field_index].offset;
    *out_field_type = c->structs[type.struct_index].fields[field_index].type;
    return 0;
}

static int z_emit_add_rax_u32(z_compiler_t *c, uint32_t value) {
    if (value == 0u) {
        return 0;
    }

    return z_emit_instr2_u64(c, "add", "rax", value);
}

static int z_parse_field_address_from_local(z_compiler_t *c,
                                            int local_index,
                                            z_type_t *out_type) {
    z_type_t current_type = c->locals[local_index].type;
    int have_address = 0;

    while (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
        z_token_type_t op = c->current.type;
        char field_name[Z_MAX_TOKEN_TEXT];
        uint32_t field_offset = 0;
        z_type_t field_type = z_make_type(Z_TYPE_INT, 0, -1);

        if (op == Z_TOKEN_DOT) {
            if (!z_type_is_struct_value(&current_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (!have_address) {
                if (z_emit_address_of_offset(c, c->locals[local_index].stack_offset) != 0) {
                    return -1;
                }
                have_address = 1;
            }

            if (z_next_token(c) != 0 ||
                z_expect_ident(c, field_name) != 0 ||
                z_find_field_for_struct_type(c, current_type, field_name, &field_offset, &field_type) != 0 ||
                z_emit_add_rax_u32(c, field_offset) != 0) {
                return -1;
            }
        } else {
            z_type_t pointee_type;

            if (!z_type_is_struct_pointer(&current_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (!have_address) {
                if (z_emit_load_rax_from_offset_typed(c,
                                                      c->locals[local_index].stack_offset,
                                                      current_type) != 0) {
                    return -1;
                }
                have_address = 1;
            } else if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                       z_emit_load_rax_from_reg_ptr_typed(c, "rcx", current_type) != 0) {
                return -1;
            }

            pointee_type = z_type_pointee(current_type);
            if (z_next_token(c) != 0 ||
                z_expect_ident(c, field_name) != 0 ||
                z_find_field_for_struct_type(c, pointee_type, field_name, &field_offset, &field_type) != 0 ||
                z_emit_add_rax_u32(c, field_offset) != 0) {
                return -1;
            }
        }

        current_type = field_type;
    }

    if (!have_address) {
        z_set_error(c, c->current.line);
        return -1;
    }

    *out_type = current_type;
    return 0;
}

static int z_parse_field_address_from_global(z_compiler_t *c,
                                             int global_index,
                                             z_type_t *out_type) {
    z_type_t current_type = c->globals[global_index].type;
    int have_address = 0;

    while (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
        z_token_type_t op = c->current.type;
        char field_name[Z_MAX_TOKEN_TEXT];
        uint32_t field_offset = 0;
        z_type_t field_type = z_make_type(Z_TYPE_INT, 0, -1);

        if (op == Z_TOKEN_DOT) {
            if (!z_type_is_struct_value(&current_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (!have_address) {
                if (z_emit_instr2_text(c, "mov", "rax", c->globals[global_index].label) != 0) {
                    return -1;
                }
                have_address = 1;
            }

            if (z_next_token(c) != 0 ||
                z_expect_ident(c, field_name) != 0 ||
                z_find_field_for_struct_type(c, current_type, field_name, &field_offset, &field_type) != 0 ||
                z_emit_add_rax_u32(c, field_offset) != 0) {
                return -1;
            }
        } else {
            z_type_t pointee_type;

            if (!z_type_is_struct_pointer(&current_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (!have_address) {
                if (z_emit_load_rax_from_label_typed(c, c->globals[global_index].label, current_type) != 0) {
                    return -1;
                }
                have_address = 1;
            } else if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                       z_emit_load_rax_from_reg_ptr_typed(c, "rcx", current_type) != 0) {
                return -1;
            }

            pointee_type = z_type_pointee(current_type);
            if (z_next_token(c) != 0 ||
                z_expect_ident(c, field_name) != 0 ||
                z_find_field_for_struct_type(c, pointee_type, field_name, &field_offset, &field_type) != 0 ||
                z_emit_add_rax_u32(c, field_offset) != 0) {
                return -1;
            }
        }

        current_type = field_type;
    }

    if (!have_address) {
        z_set_error(c, c->current.line);
        return -1;
    }

    *out_type = current_type;
    return 0;
}

static uint32_t z_local_index_stride(const z_local_t *local, uint32_t index_depth) {
    uint32_t stride = 1;
    uint32_t i;

    for (i = index_depth + 1; i < local->dim_count; ++i) {
        stride *= local->dims[i];
    }

    return stride;
}

static uint32_t z_global_index_stride(const z_global_t *global, uint32_t index_depth) {
    uint32_t stride = 1;
    uint32_t i;

    for (i = index_depth + 1; i < global->dim_count; ++i) {
        stride *= global->dims[i];
    }

    return stride;
}

static int z_parse_indexed_address(z_compiler_t *c, int local_index, uint32_t *out_remaining_dims) {
    uint32_t index_depth = 0;

    if (z_emit_local_base_address(c, local_index) != 0 ||
        z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_LBRACKET) {
        uint64_t scale = 8u;

        if (c->locals[local_index].dim_count != 0) {
            if (index_depth >= c->locals[local_index].dim_count) {
                z_set_error(c, c->current.line);
                return -1;
            }

            scale = (uint64_t)z_local_index_stride(&c->locals[local_index], index_depth) * 8u;
        } else if (index_depth != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_expect(c, Z_TOKEN_LBRACKET) != 0 ||
            z_emit_instr1_text(c, "push", "rdx") != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_imul_rax_imm(c, scale) != 0 ||
            z_emit_pop_reg(c, "rdx") != 0 ||
            z_emit_instr2_text(c, "add", "rdx", "rax") != 0) {
            return -1;
        }

        if (z_expect(c, Z_TOKEN_RBRACKET) != 0) {
            return -1;
        }

        ++index_depth;
    }

    if (z_emit_instr2_text(c, "mov", "rax", "rdx") != 0) {
        return -1;
    }

    *out_remaining_dims = (c->locals[local_index].dim_count > index_depth)
        ? (c->locals[local_index].dim_count - index_depth)
        : 0;
    return 0;
}

static int z_parse_indexed_global_address(z_compiler_t *c, int global_index, uint32_t *out_remaining_dims) {
    uint32_t index_depth = 0;

    if (z_emit_instr2_text(c, "mov", "rax", c->globals[global_index].label) != 0 ||
        z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_LBRACKET) {
        uint64_t scale = 8u;

        if (c->globals[global_index].dim_count != 0) {
            if (index_depth >= c->globals[global_index].dim_count) {
                z_set_error(c, c->current.line);
                return -1;
            }

            scale = (uint64_t)z_global_index_stride(&c->globals[global_index], index_depth) * 8u;
        } else if (index_depth != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_expect(c, Z_TOKEN_LBRACKET) != 0 ||
            z_emit_instr1_text(c, "push", "rdx") != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_imul_rax_imm(c, scale) != 0 ||
            z_emit_pop_reg(c, "rdx") != 0 ||
            z_emit_instr2_text(c, "add", "rdx", "rax") != 0) {
            return -1;
        }

        if (z_expect(c, Z_TOKEN_RBRACKET) != 0) {
            return -1;
        }

        ++index_depth;
    }

    if (z_emit_instr2_text(c, "mov", "rax", "rdx") != 0) {
        return -1;
    }

    *out_remaining_dims = (c->globals[global_index].dim_count > index_depth)
        ? (c->globals[global_index].dim_count - index_depth)
        : 0;
    return 0;
}

static int z_parse_primary(z_compiler_t *c, z_type_t *out_type) {
    if (c->current.type == Z_TOKEN_NUMBER) {
        uint64_t value = c->current.number;

        if (z_next_token(c) != 0) {
            return -1;
        }

        if (out_type) {
            *out_type = z_make_type(Z_TYPE_INT, 0, -1);
        }

        return z_emit_instr2_u64(c, "mov", "rax", value);
    }

    if (c->current.type == Z_TOKEN_IDENT) {
        char name[Z_MAX_TOKEN_TEXT];
        int local_index;
        uint32_t remaining_dims = 0;

        z_copy_text(name, c->current.text);
        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_LPAREN) {
            if (z_parse_call_expression(c, name) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = z_make_type(Z_TYPE_INT, 0, -1);
            }
            return 0;
        }

        local_index = z_find_local(c, name);
        if (local_index < 0) {
            int global_index = z_find_global(c, name);

            if (global_index < 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (c->current.type == Z_TOKEN_LBRACKET) {
                if (z_parse_indexed_global_address(c, global_index, &remaining_dims) != 0) {
                    return -1;
                }

                if (remaining_dims == 0) {
                    z_type_t element_type = c->globals[global_index].type;
                    if (c->globals[global_index].type.pointer_depth != 0) {
                        element_type = z_type_pointee(c->globals[global_index].type);
                    }
                    if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                        z_emit_load_rax_from_reg_ptr_typed(c, "rcx", element_type) != 0) {
                        return -1;
                    }
                    if (out_type) {
                        *out_type = element_type;
                    }
                } else if (out_type) {
                    *out_type = z_make_type(Z_TYPE_I64, 1, -1);
                }

                return 0;
            }

            if (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
                z_type_t field_type;

                if (z_parse_field_address_from_global(c, global_index, &field_type) != 0) {
                    return -1;
                }

                if (z_type_is_struct_value(&field_type)) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                    z_emit_load_rax_from_reg_ptr_typed(c, "rcx", field_type) != 0) {
                    return -1;
                }
                if (out_type) {
                    *out_type = field_type;
                }
                return 0;
            }

            if (c->globals[global_index].array_length != 0) {
                if (z_emit_instr2_text(c, "mov", "rax", c->globals[global_index].label) != 0) {
                    return -1;
                }
                if (out_type) {
                    *out_type = z_make_type(c->globals[global_index].type.kind,
                                            c->globals[global_index].type.pointer_depth + 1u,
                                            c->globals[global_index].type.struct_index);
                }
                return 0;
            }

            if (z_type_is_struct_value(&c->globals[global_index].type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_emit_load_rax_from_label_typed(c,
                                                 c->globals[global_index].label,
                                                 c->globals[global_index].type) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = c->globals[global_index].type;
            }
            return 0;
        }

        if (c->current.type == Z_TOKEN_LBRACKET) {
            if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0) {
                return -1;
            }

            if (remaining_dims == 0) {
                z_type_t element_type = c->locals[local_index].type;
                if (c->locals[local_index].type.pointer_depth != 0) {
                    element_type = z_type_pointee(c->locals[local_index].type);
                }
                if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                    z_emit_load_rax_from_reg_ptr_typed(c, "rcx", element_type) != 0) {
                    return -1;
                }
                if (out_type) {
                    *out_type = element_type;
                }
            } else if (out_type) {
                *out_type = z_make_type(Z_TYPE_I64, 1, -1);
            }

            return 0;
        }

        if (c->current.type == Z_TOKEN_DOT) {
            z_type_t field_type;

            if (z_parse_field_address_from_local(c, local_index, &field_type) != 0) {
                return -1;
            }

            if (z_type_is_struct_value(&field_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                z_emit_load_rax_from_reg_ptr_typed(c, "rcx", field_type) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = field_type;
            }
            return 0;
        }

        if (c->current.type == Z_TOKEN_ARROW) {
            z_type_t field_type;

            if (z_parse_field_address_from_local(c, local_index, &field_type) != 0) {
                return -1;
            }

            if (z_type_is_struct_value(&field_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                z_emit_load_rax_from_reg_ptr_typed(c, "rcx", field_type) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = field_type;
            }
            return 0;
        }

        if (c->locals[local_index].array_length != 0) {
            if (z_emit_address_of_offset(c, c->locals[local_index].stack_offset) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = z_make_type(c->locals[local_index].type.kind,
                                        c->locals[local_index].type.pointer_depth + 1u,
                                        c->locals[local_index].type.struct_index);
            }
            return 0;
        }

        if (z_emit_load_rax_from_offset_typed(c,
                                              c->locals[local_index].stack_offset,
                                              c->locals[local_index].type) != 0) {
            return -1;
        }
        if (out_type) {
            *out_type = c->locals[local_index].type;
        }
        return 0;
    }

    if (c->current.type == Z_TOKEN_LPAREN) {
        if (z_next_token(c) != 0 ||
            z_parse_expr(c, out_type) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0) {
            return -1;
        }

        return 0;
    }

    z_set_error(c, c->current.line);
    return -1;
}

static int z_parse_unary(z_compiler_t *c, z_type_t *out_type) {
    if (c->current.type == Z_TOKEN_AMP) {
        char name[Z_MAX_TOKEN_TEXT];
        int local_index;
        uint32_t remaining_dims = 0;

        if (z_next_token(c) != 0 ||
            z_expect_ident(c, name) != 0) {
            return -1;
        }

        local_index = z_find_local(c, name);
        if (local_index < 0) {
            int global_index = z_find_global(c, name);

            if (global_index < 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (c->current.type == Z_TOKEN_LBRACKET) {
                if (z_parse_indexed_global_address(c, global_index, &remaining_dims) != 0) {
                    return -1;
                }
                if (out_type) {
                    z_type_t base_type = c->globals[global_index].type;
                    *out_type = z_make_type(base_type.kind,
                                            base_type.pointer_depth + 1u,
                                            base_type.struct_index);
                }
                return 0;
            }

            if (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
                z_type_t field_type;

                if (z_parse_field_address_from_global(c, global_index, &field_type) != 0) {
                    return -1;
                }
                if (out_type) {
                    *out_type = z_make_type(field_type.kind,
                                            field_type.pointer_depth + 1u,
                                            field_type.struct_index);
                }
                return 0;
            }

            if (z_emit_instr2_text(c, "mov", "rax", c->globals[global_index].label) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = z_make_type(c->globals[global_index].type.kind,
                                        c->globals[global_index].type.pointer_depth + 1u,
                                        c->globals[global_index].type.struct_index);
            }
            return 0;
        }

        if (c->current.type == Z_TOKEN_LBRACKET) {
            if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0) {
                return -1;
            }
            if (out_type) {
                z_type_t base_type = c->locals[local_index].type;
                *out_type = z_make_type(base_type.kind,
                                        base_type.pointer_depth + 1u,
                                        base_type.struct_index);
            }
            return 0;
        }

        if (c->current.type == Z_TOKEN_DOT) {
            z_type_t field_type;

            if (z_parse_field_address_from_local(c, local_index, &field_type) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = z_make_type(field_type.kind,
                                        field_type.pointer_depth + 1u,
                                        field_type.struct_index);
            }
            return 0;
        }

        if (c->current.type == Z_TOKEN_ARROW) {
            z_type_t field_type;

            if (z_parse_field_address_from_local(c, local_index, &field_type) != 0) {
                return -1;
            }
            if (out_type) {
                *out_type = z_make_type(field_type.kind,
                                        field_type.pointer_depth + 1u,
                                        field_type.struct_index);
            }
            return 0;
        }

        if (z_emit_address_of_offset(c, c->locals[local_index].stack_offset) != 0) {
            return -1;
        }
        if (out_type) {
            *out_type = z_make_type(c->locals[local_index].type.kind,
                                    c->locals[local_index].type.pointer_depth + 1u,
                                    c->locals[local_index].type.struct_index);
        }
        return 0;
    }

    if (c->current.type == Z_TOKEN_LPAREN && z_current_starts_cast(c)) {
        z_type_t cast_type;

        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_type(c, &cast_type) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_parse_unary(c, 0) != 0) {
            return -1;
        }

        if (z_emit_normalize_rax_for_type(c, cast_type) != 0) {
            return -1;
        }

        if (out_type) {
            *out_type = cast_type;
        }
        return 0;
    }

    if (c->current.type == Z_TOKEN_STAR) {
        z_type_t value_type;
        if (z_next_token(c) != 0 ||
            z_parse_unary(c, &value_type) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_load_rax_from_reg_ptr_typed(c,
                                               "rcx",
                                               value_type.pointer_depth != 0 ? z_type_pointee(value_type)
                                                                             : z_make_type(Z_TYPE_INT, 0, -1)) != 0) {
            return -1;
        }

        if (out_type) {
            *out_type = value_type.pointer_depth != 0 ? z_type_pointee(value_type)
                                                      : z_make_type(Z_TYPE_INT, 0, -1);
        }
        return 0;
    }

    if (c->current.type == Z_TOKEN_MINUS) {
        z_type_t value_type;
        if (z_next_token(c) != 0 ||
            z_parse_unary(c, &value_type) != 0 ||
            z_emit_instr2_u64(c, "mov", "rcx", 0) != 0 ||
            z_emit_instr2_text(c, "sub", "rcx", "rax") != 0 ||
            z_emit_instr2_text(c, "mov", "rax", "rcx") != 0) {
            return -1;
        }

        if (out_type) {
            *out_type = value_type;
        }
        return 0;
    }

    return z_parse_primary(c, out_type);
}

static int z_parse_mul(z_compiler_t *c, z_type_t *out_type) {
    z_type_t current_type;

    if (z_parse_unary(c, &current_type) != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_STAR ||
           c->current.type == Z_TOKEN_SLASH ||
           c->current.type == Z_TOKEN_PERCENT) {
        z_token_type_t op = c->current.type;
        z_type_t right_type;

        if (z_next_token(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_parse_unary(c, &right_type) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_pop_reg(c, "rax") != 0) {
            return -1;
        }

        if (op == Z_TOKEN_STAR) {
            if (z_emit_instr2_text(c, "imul", "rax", "rcx") != 0) {
                return -1;
            }
        } else {
            if (z_emit_instr0(c, "cqo") != 0 ||
                z_emit_instr1_text(c, "idiv", "rcx") != 0) {
                return -1;
            }

            if (op == Z_TOKEN_PERCENT &&
                z_emit_instr2_text(c, "mov", "rax", "rdx") != 0) {
                return -1;
            }
        }

        current_type = z_type_promote_binary(current_type, right_type);
    }

    if (out_type) {
        *out_type = current_type;
    }
    return 0;
}

static int z_parse_expr(z_compiler_t *c, z_type_t *out_type) {
    z_type_t current_type;

    if (z_parse_mul(c, &current_type) != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_PLUS || c->current.type == Z_TOKEN_MINUS) {
        z_token_type_t op = c->current.type;
        z_type_t right_type;

        if (z_next_token(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_parse_mul(c, &right_type) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_pop_reg(c, "rax") != 0) {
            return -1;
        }

        if (op == Z_TOKEN_PLUS) {
            if (z_emit_instr2_text(c, "add", "rax", "rcx") != 0) {
                return -1;
            }
        } else {
            if (z_emit_instr2_text(c, "sub", "rax", "rcx") != 0) {
                return -1;
            }
        }

        current_type = z_type_promote_binary(current_type, right_type);
    }

    if (out_type) {
        *out_type = current_type;
    }
    return 0;
}

static int z_parse_condition_or(z_compiler_t *c);

static int z_emit_bool_from_rax_truthy(z_compiler_t *c) {
    char true_label[16];
    char end_label[16];

    z_make_label(c, true_label);
    z_make_label(c, end_label);

    if (z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
        z_emit_instr1_text(c, "jne", true_label) != 0 ||
        z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
        z_emit_instr1_text(c, "jmp", end_label) != 0 ||
        z_emit_label(c, true_label) != 0 ||
        z_emit_instr2_u64(c, "mov", "rax", 1) != 0 ||
        z_emit_label(c, end_label) != 0) {
        return -1;
    }

    return 0;
}

static int z_emit_bool_from_comparison(z_compiler_t *c, z_token_type_t op, int is_unsigned) {
    char true_label[16];
    char end_label[16];
    const char *jump = "je";

    z_make_label(c, true_label);
    z_make_label(c, end_label);

    if (op == Z_TOKEN_EQ) jump = "je";
    else if (op == Z_TOKEN_NE) jump = "jne";
    else if (op == Z_TOKEN_LT) jump = is_unsigned ? "jb" : "jl";
    else if (op == Z_TOKEN_LE) jump = is_unsigned ? "jbe" : "jle";
    else if (op == Z_TOKEN_GT) jump = is_unsigned ? "ja" : "jg";
    else if (op == Z_TOKEN_GE) jump = is_unsigned ? "jae" : "jge";
    else {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (z_emit_instr1_text(c, jump, true_label) != 0 ||
        z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
        z_emit_instr1_text(c, "jmp", end_label) != 0 ||
        z_emit_label(c, true_label) != 0 ||
        z_emit_instr2_u64(c, "mov", "rax", 1) != 0 ||
        z_emit_label(c, end_label) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_condition_primary(z_compiler_t *c) {
    z_token_type_t op;
    z_type_t left_type;

    if (c->current.type == Z_TOKEN_LPAREN && !z_current_starts_cast(c)) {
        if (z_next_token(c) != 0 ||
            z_parse_condition_or(c) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_parse_expr(c, &left_type) != 0) {
        return -1;
    }

    op = c->current.type;
    if (op == Z_TOKEN_EQ || op == Z_TOKEN_NE ||
        op == Z_TOKEN_LT || op == Z_TOKEN_LE ||
        op == Z_TOKEN_GT || op == Z_TOKEN_GE) {
        z_type_t right_type;
        z_type_t compare_type;
        if (z_next_token(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_parse_expr(c, &right_type) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_pop_reg(c, "rax") != 0 ||
            z_emit_instr2_text(c, "cmp", "rax", "rcx") != 0) {
            return -1;
        }

        compare_type = z_type_promote_binary(left_type, right_type);
        return z_emit_bool_from_comparison(c, op, z_type_is_unsigned(&compare_type));
    }

    return z_emit_bool_from_rax_truthy(c);
}

static int z_parse_condition_not(z_compiler_t *c) {
    if (c->current.type == Z_TOKEN_BANG) {
        char true_label[16];
        char end_label[16];

        z_make_label(c, true_label);
        z_make_label(c, end_label);

        if (z_next_token(c) != 0 ||
            z_parse_condition_not(c) != 0 ||
            z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
            z_emit_instr1_text(c, "je", true_label) != 0 ||
            z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
            z_emit_instr1_text(c, "jmp", end_label) != 0 ||
            z_emit_label(c, true_label) != 0 ||
            z_emit_instr2_u64(c, "mov", "rax", 1) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }

        return 0;
    }

    return z_parse_condition_primary(c);
}

static int z_parse_condition_and(z_compiler_t *c) {
    if (z_parse_condition_not(c) != 0) {
        return -1;
    }

    if (c->current.type == Z_TOKEN_AMP_AMP) {
        char false_label[16];
        char end_label[16];

        z_make_label(c, false_label);
        z_make_label(c, end_label);

        do {
            if (z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
                z_emit_instr1_text(c, "je", false_label) != 0 ||
                z_next_token(c) != 0 ||
                z_parse_condition_not(c) != 0) {
                return -1;
            }
        } while (c->current.type == Z_TOKEN_AMP_AMP);

        if (z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
            z_emit_instr1_text(c, "je", false_label) != 0 ||
            z_emit_instr2_u64(c, "mov", "rax", 1) != 0 ||
            z_emit_instr1_text(c, "jmp", end_label) != 0 ||
            z_emit_label(c, false_label) != 0 ||
            z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int z_parse_condition_or(z_compiler_t *c) {
    if (z_parse_condition_and(c) != 0) {
        return -1;
    }

    if (c->current.type == Z_TOKEN_PIPE_PIPE) {
        char true_label[16];
        char end_label[16];

        z_make_label(c, true_label);
        z_make_label(c, end_label);

        do {
            if (z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
                z_emit_instr1_text(c, "jne", true_label) != 0 ||
                z_next_token(c) != 0 ||
                z_parse_condition_and(c) != 0) {
                return -1;
            }
        } while (c->current.type == Z_TOKEN_PIPE_PIPE);

        if (z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
            z_emit_instr1_text(c, "jne", true_label) != 0 ||
            z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
            z_emit_instr1_text(c, "jmp", end_label) != 0 ||
            z_emit_label(c, true_label) != 0 ||
            z_emit_instr2_u64(c, "mov", "rax", 1) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int z_emit_false_jump(z_compiler_t *c, const char *false_label) {
    if (z_parse_condition_or(c) != 0 ||
        z_emit_instr2_u64(c, "cmp", "rax", 0) != 0 ||
        z_emit_instr1_text(c, "je", false_label) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_block(z_compiler_t *c) {
    if (z_expect(c, Z_TOKEN_LBRACE) != 0) {
        return -1;
    }

    while (c->current.type != Z_TOKEN_RBRACE) {
        if (c->current.type == Z_TOKEN_EOF) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_parse_statement(c) != 0) {
            return -1;
        }
    }

    return z_expect(c, Z_TOKEN_RBRACE);
}

static int z_parse_var_decl(z_compiler_t *c,
                            const char *name,
                            z_type_t type,
                            uint32_t array_length,
                            const uint32_t *dims,
                            uint32_t dim_count,
                            int32_t struct_index,
                            uint32_t total_size_bytes) {
    int local_index;
    uint64_t alloc_size = (uint64_t)total_size_bytes;

    local_index = z_add_local(c, name, type, array_length, dims, dim_count, struct_index, total_size_bytes);

    if (local_index < 0 ||
        z_emit_instr2_u64(c, "sub", "rsp", alloc_size) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (array_length != 0) {
        uint32_t i;

        if (c->current.type == Z_TOKEN_ASSIGN) {
            if (z_next_token(c) != 0 ||
                z_expect(c, Z_TOKEN_LBRACE) != 0) {
                return -1;
            }

            for (i = 0; i < array_length; ++i) {
                if (c->current.type == Z_TOKEN_RBRACE) {
                    break;
                }

                if (z_parse_expr(c, 0) != 0 ||
                    z_emit_store_rax_to_offset_typed(c,
                                                     c->locals[local_index].stack_offset + (int32_t)(i * 8u),
                                                     type) != 0) {
                    return -1;
                }

                if (c->current.type == Z_TOKEN_COMMA) {
                    if (z_next_token(c) != 0) {
                        return -1;
                    }
                } else if (c->current.type != Z_TOKEN_RBRACE) {
                    z_set_error(c, c->current.line);
                    return -1;
                }
            }

            if (c->current.type != Z_TOKEN_RBRACE) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_next_token(c) != 0) {
                return -1;
            }
        } else {
            i = 0;
        }

        for (; i < array_length; ++i) {
            if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
                z_emit_store_rax_to_offset_typed(c,
                                                 c->locals[local_index].stack_offset + (int32_t)(i * 8u),
                                                 type) != 0) {
                return -1;
            }
        }

        return z_expect(c, Z_TOKEN_SEMI);
    }

    if (struct_index >= 0) {
        uint32_t offset = 0;

        if (c->current.type == Z_TOKEN_ASSIGN) {
            z_set_error(c, c->current.line);
            return -1;
        }

        while (offset < total_size_bytes) {
            if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
                z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset + (int32_t)offset) != 0) {
                return -1;
            }
            offset += 8u;
        }

        return z_expect(c, Z_TOKEN_SEMI);
    }

    if (c->current.type == Z_TOKEN_ASSIGN) {
        if (z_next_token(c) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_store_rax_to_offset_typed(c, c->locals[local_index].stack_offset, type) != 0) {
            return -1;
        }
    } else {
        if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
            z_emit_store_rax_to_offset_typed(c, c->locals[local_index].stack_offset, type) != 0) {
            return -1;
        }
    }

    return z_expect(c, Z_TOKEN_SEMI);
}

static int z_parse_call_stmt(z_compiler_t *c, const char *name) {
    if (z_streq(name, "print")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_STRING) {
            int string_index = z_add_string(c, (uint32_t)c->current.number);

            if (string_index < 0 ||
                z_next_token(c) != 0 ||
                z_expect(c, Z_TOKEN_RPAREN) != 0 ||
                z_emit_instr2_text(c, "mov", "rdi", c->strings[string_index].label) != 0 ||
                z_emit_instr0(c, "call puts") != 0 ||
                z_expect(c, Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
            z_emit_instr0(c, "call put_dec64") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "print_hex")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
            z_emit_instr0(c, "call put_hex64") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "put_pixel")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rdx", "rax") != 0 ||
            z_emit_pop_reg(c, "rsi") != 0 ||
            z_emit_pop_reg(c, "rdi") != 0 ||
            z_emit_instr0(c, "call put_pixel") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "put_char_at")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rdx", "rax") != 0 ||
            z_emit_pop_reg(c, "rsi") != 0 ||
            z_emit_pop_reg(c, "rdi") != 0 ||
            z_emit_instr0(c, "call put_char_at") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "put_dec_at")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rdx", "rax") != 0 ||
            z_emit_pop_reg(c, "rsi") != 0 ||
            z_emit_pop_reg(c, "rdi") != 0 ||
            z_emit_instr0(c, "call put_dec_at") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "set_margin")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr2_text(c, "mov", "rsi", "rax") != 0 ||
            z_emit_pop_reg(c, "rdi") != 0 ||
            z_emit_instr0(c, "call set_margin") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_streq(name, "statusbar_enable")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr0(c, "call statusbar_enable") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (z_parse_call_expression(c, name) != 0 ||
        z_expect(c, Z_TOKEN_SEMI) != 0) {
        return -1;
    }

    return 0;
}

static int z_emit_compound_op(z_compiler_t *c, z_token_type_t op) {
    if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
        z_emit_pop_reg(c, "rax") != 0) {
        return -1;
    }

    if (op == Z_TOKEN_PLUS_ASSIGN) {
        return z_emit_instr2_text(c, "add", "rax", "rcx");
    }

    if (op == Z_TOKEN_MINUS_ASSIGN) {
        return z_emit_instr2_text(c, "sub", "rax", "rcx");
    }

    if (op == Z_TOKEN_STAR_ASSIGN) {
        return z_emit_instr2_text(c, "imul", "rax", "rcx");
    }

    if (op == Z_TOKEN_SLASH_ASSIGN || op == Z_TOKEN_PERCENT_ASSIGN) {
        if (z_emit_instr0(c, "cqo") != 0 ||
            z_emit_instr1_text(c, "idiv", "rcx") != 0) {
            return -1;
        }

        if (op == Z_TOKEN_PERCENT_ASSIGN) {
            return z_emit_instr2_text(c, "mov", "rax", "rdx");
        }

        return 0;
    }

    z_set_error(c, c->current.line);
    return -1;
}

static int z_token_is_compound_assign(z_token_type_t type) {
    return type == Z_TOKEN_PLUS_ASSIGN ||
           type == Z_TOKEN_MINUS_ASSIGN ||
           type == Z_TOKEN_STAR_ASSIGN ||
           type == Z_TOKEN_SLASH_ASSIGN ||
           type == Z_TOKEN_PERCENT_ASSIGN;
}

static int z_parse_compound_to_offset(z_compiler_t *c,
                                      int32_t offset,
                                      z_type_t type,
                                      z_token_type_t op,
                                      z_token_type_t terminator) {
    if (z_next_token(c) != 0 ||
        z_emit_load_rax_from_offset_typed(c, offset, type) != 0 ||
        z_emit_push_rax(c) != 0 ||
        z_parse_expr(c, 0) != 0 ||
        z_emit_compound_op(c, op) != 0 ||
        z_emit_store_rax_to_offset_typed(c, offset, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_compound_to_reg_ptr(z_compiler_t *c,
                                       const char *address_reg,
                                       z_type_t type,
                                       z_token_type_t op,
                                       z_token_type_t terminator) {
    if (z_next_token(c) != 0 ||
        z_emit_instr1_text(c, "push", address_reg) != 0 ||
        z_emit_load_rax_from_reg_ptr_typed(c, address_reg, type) != 0 ||
        z_emit_push_rax(c) != 0 ||
        z_parse_expr(c, 0) != 0 ||
        z_emit_compound_op(c, op) != 0 ||
        z_emit_pop_reg(c, "rdx") != 0 ||
        z_emit_store_rax_to_reg_ptr_typed(c, "rdx", type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_compound_to_label(z_compiler_t *c,
                                     const char *label,
                                     z_type_t type,
                                     z_token_type_t op,
                                     z_token_type_t terminator) {
    if (z_next_token(c) != 0 ||
        z_emit_load_rax_from_label_typed(c, label, type) != 0 ||
        z_emit_push_rax(c) != 0 ||
        z_parse_expr(c, 0) != 0 ||
        z_emit_compound_op(c, op) != 0 ||
        z_emit_store_rax_to_label_typed(c, label, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_incdec_offset(z_compiler_t *c,
                                 int32_t offset,
                                 z_type_t type,
                                 z_token_type_t op,
                                 z_token_type_t terminator) {
    const char *instr = (op == Z_TOKEN_PLUS_PLUS) ? "add" : "sub";

    if (z_next_token(c) != 0 ||
        z_emit_load_rax_from_offset_typed(c, offset, type) != 0 ||
        z_emit_instr2_u64(c, instr, "rax", 1) != 0 ||
        z_emit_store_rax_to_offset_typed(c, offset, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_incdec_reg_ptr(z_compiler_t *c,
                                  const char *address_reg,
                                  z_type_t type,
                                  z_token_type_t op,
                                  z_token_type_t terminator) {
    const char *instr = (op == Z_TOKEN_PLUS_PLUS) ? "add" : "sub";

    if (z_next_token(c) != 0 ||
        z_emit_load_rax_from_reg_ptr_typed(c, address_reg, type) != 0 ||
        z_emit_instr2_u64(c, instr, "rax", 1) != 0 ||
        z_emit_store_rax_to_reg_ptr_typed(c, address_reg, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_incdec_label(z_compiler_t *c,
                                const char *label,
                                z_type_t type,
                                z_token_type_t op,
                                z_token_type_t terminator) {
    const char *instr = (op == Z_TOKEN_PLUS_PLUS) ? "add" : "sub";

    if (z_next_token(c) != 0 ||
        z_emit_load_rax_from_label_typed(c, label, type) != 0 ||
        z_emit_instr2_u64(c, instr, "rax", 1) != 0 ||
        z_emit_store_rax_to_label_typed(c, label, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_assignment_to_offset(z_compiler_t *c, int32_t offset, z_type_t type, z_token_type_t terminator) {
    if (z_next_token(c) != 0 ||
        z_parse_expr(c, 0) != 0 ||
        z_emit_store_rax_to_offset_typed(c, offset, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_assignment_to_reg_ptr(z_compiler_t *c,
                                         const char *address_reg,
                                         z_type_t type,
                                         z_token_type_t terminator) {
    if (z_next_token(c) != 0 ||
        z_emit_instr1_text(c, "push", address_reg) != 0 ||
        z_parse_expr(c, 0) != 0 ||
        z_emit_pop_reg(c, "rdx") != 0 ||
        z_emit_store_rax_to_reg_ptr_typed(c, "rdx", type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_assignment_to_label(z_compiler_t *c,
                                       const char *label,
                                       z_type_t type,
                                       z_token_type_t terminator) {
    if (z_next_token(c) != 0 ||
        z_parse_expr(c, 0) != 0 ||
        z_emit_store_rax_to_label_typed(c, label, type) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_for_post(z_compiler_t *c) {
    char name[Z_MAX_TOKEN_TEXT];
    int local_index;

    if (c->current.type != Z_TOKEN_IDENT) {
        z_set_error(c, c->current.line);
        return -1;
    }

    z_copy_text(name, c->current.text);
    if (z_next_token(c) != 0) {
        return -1;
    }

    local_index = z_find_local(c, name);

    if (c->current.type == Z_TOKEN_ASSIGN) {
        if (local_index < 0) {
            int global_index = z_find_global(c, name);
            if (global_index < 0 ||
                c->globals[global_index].struct_index >= 0 ||
                c->globals[global_index].array_length != 0 ||
                z_parse_assignment_to_label(c,
                                            c->globals[global_index].label,
                                            c->globals[global_index].type,
                                            Z_TOKEN_RPAREN) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->locals[local_index].struct_index >= 0 ||
            c->locals[local_index].array_length != 0 ||
            z_parse_assignment_to_offset(c,
                                         c->locals[local_index].stack_offset,
                                         c->locals[local_index].type,
                                         Z_TOKEN_RPAREN) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (z_token_is_compound_assign(c->current.type)) {
        z_token_type_t op = c->current.type;

        if (local_index < 0) {
            int global_index = z_find_global(c, name);
            if (global_index < 0 ||
                c->globals[global_index].struct_index >= 0 ||
                c->globals[global_index].array_length != 0 ||
                z_parse_compound_to_label(c,
                                          c->globals[global_index].label,
                                          c->globals[global_index].type,
                                          op,
                                          Z_TOKEN_RPAREN) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->locals[local_index].struct_index >= 0 ||
            c->locals[local_index].array_length != 0 ||
            z_parse_compound_to_offset(c,
                                       c->locals[local_index].stack_offset,
                                       c->locals[local_index].type,
                                       op,
                                       Z_TOKEN_RPAREN) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_PLUS_PLUS || c->current.type == Z_TOKEN_MINUS_MINUS) {
        z_token_type_t op = c->current.type;

        if (local_index < 0) {
            int global_index = z_find_global(c, name);
            if (global_index < 0 ||
                c->globals[global_index].struct_index >= 0 ||
                c->globals[global_index].array_length != 0 ||
                z_parse_incdec_label(c,
                                     c->globals[global_index].label,
                                     c->globals[global_index].type,
                                     op,
                                     Z_TOKEN_RPAREN) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->locals[local_index].struct_index >= 0 ||
            c->locals[local_index].array_length != 0 ||
            z_parse_incdec_offset(c,
                                  c->locals[local_index].stack_offset,
                                  c->locals[local_index].type,
                                  op,
                                  Z_TOKEN_RPAREN) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
        z_type_t field_type;

        if (local_index < 0) {
            int global_index = z_find_global(c, name);
            if (global_index < 0 ||
                z_parse_field_address_from_global(c, global_index, &field_type) != 0 ||
                z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
        } else {
            if (z_parse_field_address_from_local(c, local_index, &field_type) != 0 ||
                z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
        }

        if (c->current.type == Z_TOKEN_ASSIGN) {
            if (z_type_is_struct_value(&field_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }
            return z_parse_assignment_to_reg_ptr(c, "rdx", field_type, Z_TOKEN_RPAREN);
        }

        if (z_token_is_compound_assign(c->current.type)) {
            z_token_type_t op = c->current.type;
            if (z_type_is_struct_value(&field_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }
            return z_parse_compound_to_reg_ptr(c, "rdx", field_type, op, Z_TOKEN_RPAREN);
        }

        if (c->current.type == Z_TOKEN_PLUS_PLUS || c->current.type == Z_TOKEN_MINUS_MINUS) {
            z_token_type_t op = c->current.type;
            if (z_type_is_struct_value(&field_type)) {
                z_set_error(c, c->current.line);
                return -1;
            }
            return z_parse_incdec_reg_ptr(c, "rdx", field_type, op, Z_TOKEN_RPAREN);
        }
    }

    if (c->current.type == Z_TOKEN_LBRACKET) {
        uint32_t remaining_dims = 0;
        z_type_t element_type;

        if (local_index < 0) {
            int global_index = z_find_global(c, name);
            if (global_index < 0 ||
                z_parse_indexed_global_address(c, global_index, &remaining_dims) != 0 ||
                remaining_dims != 0 ||
                z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            element_type = c->globals[global_index].type;
        } else {
            if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0 ||
                remaining_dims != 0 ||
                z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            element_type = c->locals[local_index].type;
        }

        if (element_type.pointer_depth != 0) {
            element_type = z_type_pointee(element_type);
        }

        if (c->current.type == Z_TOKEN_ASSIGN) {
            return z_parse_assignment_to_reg_ptr(c, "rdx", element_type, Z_TOKEN_RPAREN);
        }

        if (z_token_is_compound_assign(c->current.type)) {
            z_token_type_t op = c->current.type;
            return z_parse_compound_to_reg_ptr(c, "rdx", element_type, op, Z_TOKEN_RPAREN);
        }

        if (c->current.type == Z_TOKEN_PLUS_PLUS || c->current.type == Z_TOKEN_MINUS_MINUS) {
            z_token_type_t op = c->current.type;
            return z_parse_incdec_reg_ptr(c, "rdx", element_type, op, Z_TOKEN_RPAREN);
        }
    }

    z_set_error(c, c->current.line);
    return -1;
}

static int z_parse_statement(z_compiler_t *c) {
    if (!c->in_function) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (c->current.type == Z_TOKEN_LBRACE) {
        return z_parse_block(c);
    }

    if (z_current_is_type_name(c) && !(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "void"))) {
        char name[Z_MAX_TOKEN_TEXT];
        z_type_t type;
        uint32_t array_length = 0;
        uint32_t dims[Z_MAX_ARRAY_DIMS];
        uint32_t dim_count = 0;
        uint32_t type_size = 0;
        uint32_t total_size = 0;

        if (z_parse_typed_name(c, &type, name) != 0 ||
            z_parse_array_decl_suffixes(c, &array_length, dims, &dim_count) != 0) {
            return -1;
        }

        if (type.kind == Z_TYPE_VOID && type.pointer_depth == 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        type_size = z_type_storage_size_bytes(c, type);
        if (type_size == 0u) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (array_length != 0) {
            total_size = array_length * ((type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0) ? type_size : 8u);
        } else if (type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0) {
            total_size = z_align_up_u32(type_size, 8u);
        } else {
            total_size = 8u;
        }

        return z_parse_var_decl(c,
                                name,
                                type,
                                array_length,
                                dims,
                                dim_count,
                                type.kind == Z_TYPE_STRUCT && type.pointer_depth == 0 ? type.struct_index : -1,
                                total_size);
    }

    if (c->current.type == Z_TOKEN_STAR) {
        z_token_type_t op;
        z_type_t value_type = z_make_type(Z_TYPE_INT, 0, -1);
        z_type_t deref_type = z_make_type(Z_TYPE_INT, 0, -1);

        if (z_next_token(c) != 0 ||
            z_parse_unary(c, &value_type) != 0 ||
            z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
            return -1;
        }

        if (value_type.pointer_depth != 0) {
            deref_type = z_type_pointee(value_type);
        }

        op = c->current.type;
        if (op == Z_TOKEN_ASSIGN) {
            if (z_next_token(c) != 0 ||
                z_emit_instr1_text(c, "push", "rdx") != 0 ||
                z_parse_expr(c, 0) != 0 ||
                z_emit_pop_reg(c, "rdx") != 0 ||
                z_emit_store_rax_to_reg_ptr_typed(c, "rdx", deref_type) != 0 ||
                z_expect(c, Z_TOKEN_SEMI) != 0) {
                return -1;
            }

            return 0;
        }

        if (z_token_is_compound_assign(op)) {
            return z_parse_compound_to_reg_ptr(c, "rdx", deref_type, op, Z_TOKEN_SEMI);
        }

        if (op == Z_TOKEN_PLUS_PLUS || op == Z_TOKEN_MINUS_MINUS) {
            return z_parse_incdec_reg_ptr(c, "rdx", deref_type, op, Z_TOKEN_SEMI);
        }

        z_set_error(c, c->current.line);
        return -1;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "return")) {
        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_SEMI) {
            if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
                z_next_token(c) != 0 ||
                z_emit_instr1_text(c, "jmp", c->current_exit_label) != 0) {
                return -1;
            }

            return 0;
        }

        if (z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0 ||
            z_emit_instr1_text(c, "jmp", c->current_exit_label) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "break")) {
        const z_loop_t *loop = z_current_loop(c);

        if (loop == 0 ||
            z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0 ||
            z_emit_instr1_text(c, "jmp", loop->break_label) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "continue")) {
        const z_loop_t *loop = z_current_loop(c);

        if (loop == 0 ||
            z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0 ||
            z_emit_instr1_text(c, "jmp", loop->continue_label) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "if")) {
        char false_label[16];
        char end_label[16];

        z_make_label(c, false_label);
        z_make_label(c, end_label);

        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_emit_false_jump(c, false_label) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_parse_statement(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "else")) {
            if (z_emit_instr1_text(c, "jmp", end_label) != 0 ||
                z_emit_label(c, false_label) != 0 ||
                z_next_token(c) != 0 ||
                z_parse_statement(c) != 0 ||
                z_emit_label(c, end_label) != 0) {
                return -1;
            }
        } else {
            if (z_emit_label(c, false_label) != 0) {
                return -1;
            }
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "while")) {
        char start_label[16];
        char end_label[16];

        z_make_label(c, start_label);
        z_make_label(c, end_label);

        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_emit_label(c, start_label) != 0 ||
            z_emit_false_jump(c, end_label) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_push_loop(c, end_label, start_label) != 0) {
            return -1;
        }

        if (z_parse_statement(c) != 0) {
            z_pop_loop(c);
            return -1;
        }
        z_pop_loop(c);

        if (z_emit_instr1_text(c, "jmp", start_label) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "for")) {
        char start_label[16];
        char post_label[16];
        char end_label[16];
        char post_out[2048];
        char *saved_out;
        uint32_t saved_capacity;
        uint32_t saved_size;
        uint32_t post_size = 0;

        z_make_label(c, start_label);
        z_make_label(c, post_label);
        z_make_label(c, end_label);

        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_LPAREN) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_SEMI) {
            if (z_next_token(c) != 0) {
                return -1;
            }
        } else if (z_parse_statement(c) != 0) {
            return -1;
        }

        if (z_emit_label(c, start_label) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_SEMI) {
            if (z_next_token(c) != 0) {
                return -1;
            }
        } else if (z_emit_false_jump(c, end_label) != 0 ||
                   z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_RPAREN) {
            if (z_next_token(c) != 0) {
                return -1;
            }
        } else {
            saved_out = c->out;
            saved_capacity = c->out_capacity;
            saved_size = c->out_size;
            c->out = post_out;
            c->out_capacity = sizeof(post_out);
            c->out_size = 0;

            if (z_parse_for_post(c) != 0) {
                c->out = saved_out;
                c->out_capacity = saved_capacity;
                c->out_size = saved_size;
                return -1;
            }

            post_size = c->out_size;
            c->out = saved_out;
            c->out_capacity = saved_capacity;
            c->out_size = saved_size;
        }

        if (z_push_loop(c, end_label, post_label) != 0) {
            return -1;
        }

        if (z_parse_statement(c) != 0) {
            z_pop_loop(c);
            return -1;
        }
        z_pop_loop(c);

        if (z_emit_label(c, post_label) != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < post_size; ++i) {
            if (z_emit_char(c, post_out[i]) != 0) {
                return -1;
            }
        }

        if (z_emit_instr1_text(c, "jmp", start_label) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "do")) {
        char start_label[16];
        char cond_label[16];
        char end_label[16];

        z_make_label(c, start_label);
        z_make_label(c, cond_label);
        z_make_label(c, end_label);

        if (z_next_token(c) != 0 ||
            z_emit_label(c, start_label) != 0 ||
            z_push_loop(c, end_label, cond_label) != 0) {
            return -1;
        }

        if (z_parse_statement(c) != 0) {
            z_pop_loop(c);
            return -1;
        }
        z_pop_loop(c);

        if (z_emit_label(c, cond_label) != 0) {
            return -1;
        }

        if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "while"))) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_emit_false_jump(c, end_label) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0 ||
            z_emit_instr1_text(c, "jmp", start_label) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT) {
        char name[Z_MAX_TOKEN_TEXT];
        int local_index;

        z_copy_text(name, c->current.text);
        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_ASSIGN) {
            local_index = z_find_local(c, name);
            if (local_index < 0) {
                int global_index = z_find_global(c, name);
                if (global_index < 0 ||
                    c->globals[global_index].struct_index >= 0 ||
                    c->globals[global_index].array_length != 0 ||
                    z_parse_assignment_to_label(c,
                                                c->globals[global_index].label,
                                                c->globals[global_index].type,
                                                Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (c->locals[local_index].struct_index >= 0 ||
                c->locals[local_index].array_length != 0 ||
                z_next_token(c) != 0 ||
                z_parse_expr(c, 0) != 0 ||
                z_emit_store_rax_to_offset_typed(c,
                                                 c->locals[local_index].stack_offset,
                                                 c->locals[local_index].type) != 0 ||
                z_expect(c, Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (z_token_is_compound_assign(c->current.type)) {
            z_token_type_t op = c->current.type;

            local_index = z_find_local(c, name);
            if (local_index < 0) {
                int global_index = z_find_global(c, name);
                if (global_index < 0 ||
                    c->globals[global_index].struct_index >= 0 ||
                    c->globals[global_index].array_length != 0 ||
                    z_parse_compound_to_label(c,
                                              c->globals[global_index].label,
                                              c->globals[global_index].type,
                                              op,
                                              Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (c->locals[local_index].struct_index >= 0 ||
                c->locals[local_index].array_length != 0 ||
                z_parse_compound_to_offset(c,
                                           c->locals[local_index].stack_offset,
                                           c->locals[local_index].type,
                                           op,
                                           Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->current.type == Z_TOKEN_PLUS_PLUS || c->current.type == Z_TOKEN_MINUS_MINUS) {
            z_token_type_t op = c->current.type;

            local_index = z_find_local(c, name);
            if (local_index < 0) {
                int global_index = z_find_global(c, name);
                if (global_index < 0 ||
                    c->globals[global_index].struct_index >= 0 ||
                    c->globals[global_index].array_length != 0 ||
                    z_parse_incdec_label(c,
                                         c->globals[global_index].label,
                                         c->globals[global_index].type,
                                         op,
                                         Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (c->locals[local_index].struct_index >= 0 ||
                c->locals[local_index].array_length != 0 ||
                z_parse_incdec_offset(c,
                                      c->locals[local_index].stack_offset,
                                      c->locals[local_index].type,
                                      op,
                                      Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
            z_type_t field_type;

            local_index = z_find_local(c, name);
            if (local_index < 0) {
                int global_index = z_find_global(c, name);
                if (global_index < 0 ||
                    z_parse_field_address_from_global(c, global_index, &field_type) != 0 ||
                    z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }
            } else {
                if (z_parse_field_address_from_local(c, local_index, &field_type) != 0 ||
                    z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }
            }

            if (c->current.type == Z_TOKEN_ASSIGN) {
                if (z_type_is_struct_value(&field_type) ||
                    z_parse_assignment_to_reg_ptr(c, "rdx", field_type, Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (z_token_is_compound_assign(c->current.type)) {
                z_token_type_t op = c->current.type;
                if (z_type_is_struct_value(&field_type)) {
                    z_set_error(c, c->current.line);
                    return -1;
                }
                if (z_parse_compound_to_reg_ptr(c, "rdx", field_type, op, Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (c->current.type == Z_TOKEN_PLUS_PLUS || c->current.type == Z_TOKEN_MINUS_MINUS) {
                z_token_type_t op = c->current.type;
                if (z_type_is_struct_value(&field_type)) {
                    z_set_error(c, c->current.line);
                    return -1;
                }
                if (z_parse_incdec_reg_ptr(c, "rdx", field_type, op, Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            z_set_error(c, c->current.line);
            return -1;
        }

        if (c->current.type == Z_TOKEN_LBRACKET) {
            uint32_t remaining_dims = 0;
            z_type_t element_type;
            local_index = z_find_local(c, name);
            if (local_index < 0) {
                int global_index = z_find_global(c, name);
                if (global_index < 0 ||
                    z_parse_indexed_global_address(c, global_index, &remaining_dims) != 0 ||
                    remaining_dims != 0 ||
                    z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                element_type = c->globals[global_index].type;
                if (element_type.pointer_depth != 0) {
                    element_type = z_type_pointee(element_type);
                }
            } else {
                if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0 ||
                    remaining_dims != 0 ||
                    z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                element_type = c->locals[local_index].type;
                if (element_type.pointer_depth != 0) {
                    element_type = z_type_pointee(element_type);
                }
            }

            if (c->current.type == Z_TOKEN_ASSIGN) {
                if (z_next_token(c) != 0 ||
                    z_emit_instr1_text(c, "push", "rdx") != 0 ||
                    z_parse_expr(c, 0) != 0 ||
                    z_emit_pop_reg(c, "rdx") != 0 ||
                    z_emit_store_rax_to_reg_ptr_typed(c, "rdx", element_type) != 0 ||
                    z_expect(c, Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (z_token_is_compound_assign(c->current.type)) {
                z_token_type_t op = c->current.type;
                if (z_parse_compound_to_reg_ptr(c, "rdx", element_type, op, Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            if (c->current.type == Z_TOKEN_PLUS_PLUS || c->current.type == Z_TOKEN_MINUS_MINUS) {
                z_token_type_t op = c->current.type;
                if (z_parse_incdec_reg_ptr(c, "rdx", element_type, op, Z_TOKEN_SEMI) != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                return 0;
            }

            z_set_error(c, c->current.line);
            return -1;
        }

        if (c->current.type == Z_TOKEN_LPAREN) {
            return z_parse_call_stmt(c, name);
        }

        z_set_error(c, c->current.line);
        return -1;
    }

    z_set_error(c, c->current.line);
    return -1;
}

static int z_begin_function(z_compiler_t *c, const char *name) {
    c->local_count = 0;
    c->next_stack_offset = 0;
    c->in_function = 1;
    if (z_ensure_function(c, name, c->current_function_label) != 0) {
        return -1;
    }
    z_make_label(c, c->current_exit_label);

    if (z_emit_label(c, c->current_function_label) != 0 ||
        z_emit_instr1_text(c, "push", "rbp") != 0 ||
        z_emit_instr2_text(c, "mov", "rbp", "rsp") != 0) {
        return -1;
    }

    return 0;
}

static int z_end_function(z_compiler_t *c) {
    c->in_function = 0;

    if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
        z_emit_label(c, c->current_exit_label) != 0 ||
        z_emit_instr0(c, "leave") != 0 ||
        z_emit_instr0(c, "ret") != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_function_definition(z_compiler_t *c, const char *name) {
    char param_names[Z_MAX_PARAMS][Z_MAX_TOKEN_TEXT];
    z_type_t param_types[Z_MAX_PARAMS];
    uint32_t param_count = 0;
    uint32_t i;

    if (z_begin_function(c, name) != 0 ||
        z_expect(c, Z_TOKEN_LPAREN) != 0) {
        return -1;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "void")) {
        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type != Z_TOKEN_RPAREN) {
            z_set_error(c, c->current.line);
            return -1;
        }
    } else if (c->current.type != Z_TOKEN_RPAREN) {
        for (;;) {
            if (param_count >= Z_MAX_PARAMS) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_parse_typed_name(c, &param_types[param_count], param_names[param_count]) != 0) {
                return -1;
            }

            ++param_count;
            if (c->current.type != Z_TOKEN_COMMA) {
                break;
            }

            if (z_next_token(c) != 0) {
                return -1;
            }
        }
    }

    if (z_expect(c, Z_TOKEN_RPAREN) != 0) {
        return -1;
    }

    for (i = 0; i < param_count; ++i) {
        int local_index = z_add_local(c, param_names[i], param_types[i], 0, 0, 0, -1, 8u);

        if (local_index < 0 ||
            z_emit_instr2_u64(c, "sub", "rsp", 8) != 0 ||
            z_emit_text(c, "    mov [rbp") != 0 ||
            z_emit_i32(c, c->locals[local_index].stack_offset) != 0 ||
            z_emit_text(c, "], ") != 0 ||
            z_emit_text(c, z_arg_registers[i]) != 0 ||
            z_emit_char(c, '\n') != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }
    }

    if (z_parse_block(c) != 0 ||
        z_end_function(c) != 0) {
        return -1;
    }

    return 0;
}

static int z_emit_string_data(z_compiler_t *c, const z_string_t *string_info) {
    const char *text = c->string_pool + string_info->offset;
    uint32_t i;

    if (z_emit_text(c, string_info->label) != 0 ||
        z_emit_text(c, " db \"") != 0) {
        return -1;
    }

    for (i = 0; i < string_info->length; ++i) {
        char ch = text[i];

        if (ch == '\n') {
            if (z_emit_text(c, "\\n") != 0) {
                return -1;
            }
        } else if (ch == '\r') {
            if (z_emit_text(c, "\\r") != 0) {
                return -1;
            }
        } else if (ch == '\t') {
            if (z_emit_text(c, "\\t") != 0) {
                return -1;
            }
        } else if (ch == '\0') {
            if (z_emit_text(c, "\\0") != 0) {
                return -1;
            }
        } else if (ch == '"' || ch == '\\') {
            if (z_emit_char(c, '\\') != 0 ||
                z_emit_char(c, ch) != 0) {
                return -1;
            }
        } else {
            if (z_emit_char(c, ch) != 0) {
                return -1;
            }
        }
    }

    return z_emit_text(c, "\", 0\n");
}

static int z_emit_db_byte_value(z_compiler_t *c, uint8_t value) {
    return z_emit_u64(c, value);
}

static int z_emit_global_data(z_compiler_t *c, const z_global_t *global) {
    uint32_t i;
    uint64_t value = global->has_init ? global->init_value : 0;

    if (z_emit_text(c, global->label) != 0 ||
        z_emit_text(c, " db ") != 0) {
        return -1;
    }

    for (i = 0; i < global->total_size_bytes; ++i) {
        uint8_t byte_value = 0;

        if (global->has_init && i < 8u) {
            byte_value = (uint8_t)((value >> (i * 8u)) & 0xFFu);
        }

        if (i != 0 &&
            z_emit_text(c, ", ") != 0) {
            return -1;
        }

        if (z_emit_db_byte_value(c, byte_value) != 0) {
            return -1;
        }
    }

    return z_emit_char(c, '\n');
}

int zscript_compile_source(const char *source,
                           uint32_t size,
                           char *out,
                           uint32_t out_capacity,
                           uint32_t *out_size,
                           uint32_t *error_line) {
    z_compiler_t c;
    char entry_label[20];
    uint32_t i;

    if (source == 0 || out == 0 || out_size == 0 || out_capacity == 0) {
        return -1;
    }

    c.source = source;
    c.size = size;
    c.pos = 0;
    c.line = 1;
    c.current.type = Z_TOKEN_EOF;
    c.current.number = 0;
    c.current.text[0] = '\0';
    c.current.line = 1;
    c.out = out;
    c.out_capacity = out_capacity;
    c.out_size = 0;
    c.error_line = 0;
    c.label_counter = 0;
    c.string_pool_used = 0;
    c.string_count = 0;
    c.function_count = 0;
    c.struct_count = 0;
    c.global_count = 0;
    c.in_function = 0;
    c.current_function_label[0] = '\0';
    c.current_exit_label[0] = '\0';
    c.local_count = 0;
    c.next_stack_offset = 0;
    c.loop_depth = 0;

    if (error_line) {
        *error_line = 0;
    }

    if (z_ensure_function(&c, "entry", entry_label) != 0 ||
        z_next_token(&c) != 0) {
        if (error_line) {
            *error_line = c.error_line;
        }
        return -1;
    }

    while (c.current.type != Z_TOKEN_EOF &&
           z_current_starts_struct_definition(&c)) {
        if (z_parse_struct_definition(&c) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    while (c.current.type != Z_TOKEN_EOF &&
           z_current_starts_global_definition(&c)) {
        if (z_parse_global_definition(&c) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    if (
        z_emit_line(&c, "bits 64") != 0 ||
        z_emit_line(&c, "default rel") != 0 ||
        z_emit_line(&c, "section .text") != 0 ||
        z_emit_line(&c, "start:") != 0 ||
        z_emit_instr1_text(&c, "call", entry_label) != 0 ||
        z_emit_instr0(&c, "ret") != 0) {
        if (error_line) {
            *error_line = c.error_line;
        }
        return -1;
    }

    while (c.current.type != Z_TOKEN_EOF && z_current_starts_function_definition(&c)) {
        char function_name[Z_MAX_TOKEN_TEXT];
        z_type_t function_type;

        if (z_parse_typed_name(&c, &function_type, function_name) != 0 ||
            c.current.type != Z_TOKEN_LPAREN ||
            z_parse_function_definition(&c, function_name) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    c.local_count = 0;
    c.next_stack_offset = 0;
    c.in_function = 1;
    z_copy_text(c.current_function_label, entry_label);
    z_make_label(&c, c.current_exit_label);
    if (z_emit_label(&c, c.current_function_label) != 0 ||
        z_emit_instr1_text(&c, "push", "rbp") != 0 ||
        z_emit_instr2_text(&c, "mov", "rbp", "rsp") != 0) {
        if (error_line) {
            *error_line = c.error_line;
        }
        return -1;
    }

    while (c.current.type != Z_TOKEN_EOF) {
        if (z_parse_statement(&c) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    if (z_end_function(&c) != 0) {
        if (error_line) {
            *error_line = c.error_line;
        }
        return -1;
    }

    if (c.string_count != 0 || c.global_count != 0) {
        if (z_emit_line(&c, "section .data") != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    for (i = 0; i < c.string_count; ++i) {
        if (z_emit_string_data(&c, &c.strings[i]) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    for (i = 0; i < c.global_count; ++i) {
        if (z_emit_global_data(&c, &c.globals[i]) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    if (c.out_size >= c.out_capacity) {
        if (error_line) {
            *error_line = c.error_line ? c.error_line : c.line;
        }
        return -1;
    }

    c.out[c.out_size] = '\0';
    *out_size = c.out_size;
    if (error_line) {
        *error_line = 0;
    }
    return 0;
}
