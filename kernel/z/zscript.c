#include "zscript.h"

#define Z_MAX_TOKEN_TEXT 32u
#define Z_MAX_FUNCTIONS 64u
#define Z_MAX_LABEL_TEXT 32u
#define Z_MAX_LABEL_PREFIX 8u
#define Z_MAX_LOCALS 64u
#define Z_MAX_PARAMS 6u
#define Z_MAX_STRINGS 64u
#define Z_STRING_POOL_SIZE 4096u
#define Z_MAX_ARRAY_DIMS 3u
#define Z_MAX_STRUCTS 32u
#define Z_MAX_STRUCT_FIELDS 16u
#define Z_MAX_GLOBALS 32u
#define Z_MAX_LOOP_DEPTH 16u
#define Z_MAX_CONSTANTS 64u
#define Z_MAX_TYPEDEFS 32u
#define Z_MAX_FUNCTION_SIGNATURES 64u

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
    Z_TOKEN_COLON,
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
    int32_t function_sig_index;
} z_type_t;

typedef struct {
    z_type_t return_type;
    uint32_t param_count;
    z_type_t param_types[Z_MAX_PARAMS];
} z_function_signature_t;

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
    int is_defined;
    z_struct_field_t fields[Z_MAX_STRUCT_FIELDS];
} z_struct_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    char label[Z_MAX_LABEL_TEXT];
    int is_extern;
    int is_export;
    int is_defined;
    int32_t signature_index;
} z_function_t;

typedef struct {
    char label[Z_MAX_LABEL_TEXT];
    uint32_t offset;
    uint32_t length;
} z_string_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    char label[Z_MAX_LABEL_TEXT];
    z_type_t type;
    uint32_t array_length;
    uint32_t dim_count;
    uint32_t dims[Z_MAX_ARRAY_DIMS];
    int32_t struct_index;
    uint32_t total_size_bytes;
    uint64_t init_value;
    int has_init;
    int is_export;
} z_global_t;

typedef struct {
    char break_label[Z_MAX_LABEL_TEXT];
    char continue_label[Z_MAX_LABEL_TEXT];
} z_loop_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    uint64_t value;
} z_constant_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    z_type_t type;
} z_typedef_t;

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
    char label_prefix[Z_MAX_LABEL_PREFIX];
    int object_mode;
    uint32_t string_pool_used;
    uint32_t string_count;
    uint32_t function_count;
    uint32_t struct_count;
    uint32_t global_count;
    uint32_t constant_count;
    uint32_t typedef_count;
    uint32_t function_signature_count;
    z_string_t strings[Z_MAX_STRINGS];
    z_function_t functions[Z_MAX_FUNCTIONS];
    z_struct_t structs[Z_MAX_STRUCTS];
    z_global_t globals[Z_MAX_GLOBALS];
    z_constant_t constants[Z_MAX_CONSTANTS];
    z_typedef_t typedefs[Z_MAX_TYPEDEFS];
    z_function_signature_t function_signatures[Z_MAX_FUNCTION_SIGNATURES];
    char string_pool[Z_STRING_POOL_SIZE];
    int in_function;
    char current_function_label[Z_MAX_LABEL_TEXT];
    char current_exit_label[Z_MAX_LABEL_TEXT];
    z_type_t current_return_type;
    int32_t current_struct_return_offset;
    uint32_t local_count;
    z_local_t locals[Z_MAX_LOCALS];
    int32_t next_stack_offset;
    uint32_t loop_depth;
    z_loop_t loops[Z_MAX_LOOP_DEPTH];
    uint32_t pending_array_length;
    uint32_t pending_dim_count;
    uint32_t pending_dims[Z_MAX_ARRAY_DIMS];
} z_compiler_t;

static const char *const z_arg_registers[Z_MAX_PARAMS] = {
    "rdi", "rsi", "rdx", "rcx", "r8", "r9",
};

static int z_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int z_char_is_line_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
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

static const char *z_skip_line_spaces_ptr(const char *p, const char *end);
static const char *z_line_end_ptr(const char *p, const char *end);
static int z_parse_define_number_text(const char *p, const char *end, uint64_t *out_value);
static int z_find_constant(const z_compiler_t *c, const char *name);
static int z_add_constant(z_compiler_t *c, const char *name, uint64_t value);

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
    type.function_sig_index = -1;
    return type;
}

static int z_type_same(const z_type_t *a, const z_type_t *b) {
    return a->kind == b->kind &&
           a->pointer_depth == b->pointer_depth &&
           a->struct_index == b->struct_index &&
           a->function_sig_index == b->function_sig_index;
}

static int z_type_assignable(const z_type_t *dst, const z_type_t *src) {
    if (dst->pointer_depth != 0 || src->pointer_depth != 0) {
        if (dst->kind != src->kind ||
            dst->pointer_depth != src->pointer_depth ||
            dst->struct_index != src->struct_index) {
            return 0;
        }

        if (dst->function_sig_index >= 0 &&
            src->function_sig_index >= 0 &&
            dst->function_sig_index != src->function_sig_index) {
            return 0;
        }

        return 1;
    }

    if ((dst->kind == Z_TYPE_STRUCT && dst->pointer_depth == 0) ||
        (src->kind == Z_TYPE_STRUCT && src->pointer_depth == 0)) {
        return z_type_same(dst, src);
    }

    return dst->kind != Z_TYPE_VOID && src->kind != Z_TYPE_VOID;
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

static int z_type_is_qualifier_name(const char *name) {
    return z_streq(name, "const") || z_streq(name, "volatile");
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

static uint32_t z_array_element_size_bytes(const z_compiler_t *c, z_type_t type) {
    uint32_t size = z_type_storage_size_bytes(c, type);

    if (size == 0u) {
        return 8u;
    }

    return size;
}

static uint32_t z_pointer_step_size_bytes(const z_compiler_t *c, z_type_t pointer_type) {
    if (pointer_type.pointer_depth == 0) {
        return 1u;
    }

    return z_array_element_size_bytes(c, z_type_pointee(pointer_type));
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

static int z_emit_load_from_base_offset(z_compiler_t *c, const char *dst, const char *base, uint32_t offset, uint32_t bits) {
    return z_emit_text(c, "    mov ") ||
           z_emit_text(c, dst) ||
           z_emit_text(c, ", ") ||
           z_emit_text(c, z_mem_prefix_for_bits(bits)) ||
           z_emit_text(c, "[") ||
           z_emit_text(c, base) ||
           (offset != 0u ? z_emit_text(c, "+") || z_emit_u64(c, offset) : 0) ||
           z_emit_text(c, "]\n");
}

static int z_emit_store_to_base_offset(z_compiler_t *c, const char *base, uint32_t offset, const char *src, uint32_t bits) {
    return z_emit_text(c, "    mov ") ||
           z_emit_text(c, z_mem_prefix_for_bits(bits)) ||
           z_emit_text(c, "[") ||
           z_emit_text(c, base) ||
           (offset != 0u ? z_emit_text(c, "+") || z_emit_u64(c, offset) : 0) ||
           z_emit_text(c, "], ") ||
           z_emit_text(c, src) ||
           z_emit_char(c, '\n');
}

static int z_emit_copy_memory(z_compiler_t *c, const char *dst_reg, const char *src_reg, uint32_t size) {
    uint32_t offset = 0;

    while (offset + 8u <= size) {
        if (z_emit_load_from_base_offset(c, "rax", src_reg, offset, 64u) != 0 ||
            z_emit_store_to_base_offset(c, dst_reg, offset, "rax", 64u) != 0) {
            return -1;
        }
        offset += 8u;
    }

    while (offset + 4u <= size) {
        if (z_emit_load_from_base_offset(c, "eax", src_reg, offset, 32u) != 0 ||
            z_emit_store_to_base_offset(c, dst_reg, offset, "eax", 32u) != 0) {
            return -1;
        }
        offset += 4u;
    }

    while (offset + 2u <= size) {
        if (z_emit_load_from_base_offset(c, "ax", src_reg, offset, 16u) != 0 ||
            z_emit_store_to_base_offset(c, dst_reg, offset, "ax", 16u) != 0) {
            return -1;
        }
        offset += 2u;
    }

    if (offset < size) {
        if (z_emit_load_from_base_offset(c, "al", src_reg, offset, 8u) != 0 ||
            z_emit_store_to_base_offset(c, dst_reg, offset, "al", 8u) != 0) {
            return -1;
        }
    }

    return 0;
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
    char prefix[Z_MAX_LABEL_PREFIX + 3u];
    uint32_t i = 0;

    while (c->label_prefix[i] && i + 1u < sizeof(prefix)) {
        prefix[i] = c->label_prefix[i];
        ++i;
    }
    prefix[i++] = 'z';
    prefix[i++] = 'l';
    prefix[i] = '\0';

    z_make_prefixed_label(out, prefix, c->label_counter++);
}

static void z_make_kind_label(z_compiler_t *c, char *out, const char *kind, uint32_t value) {
    char prefix[Z_MAX_LABEL_PREFIX + 3u];
    uint32_t i = 0;
    uint32_t j = 0;

    while (c->label_prefix[i] && i + 1u < sizeof(prefix)) {
        prefix[i] = c->label_prefix[i];
        ++i;
    }

    while (kind[j] && i + 1u < sizeof(prefix)) {
        prefix[i++] = kind[j++];
    }

    prefix[i] = '\0';
    z_make_prefixed_label(out, prefix, value);
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

static int z_read_directive_word(const char **p, const char *end, char *out) {
    uint32_t len = 0;

    *p = z_skip_line_spaces_ptr(*p, end);
    if (*p >= end || !z_char_is_ident_start(**p)) {
        out[0] = '\0';
        return -1;
    }

    while (*p < end && z_char_is_ident_continue(**p)) {
        if (len + 1u >= Z_MAX_TOKEN_TEXT) {
            return -1;
        }
        out[len++] = **p;
        ++*p;
    }

    out[len] = '\0';
    return 0;
}

static int z_skip_preprocessor_block(z_compiler_t *c, int stop_at_else) {
    int depth = 1;

    while (c->pos < c->size) {
        const char *line_start = c->source + c->pos;
        const char *end = c->source + c->size;
        const char *line_end = z_line_end_ptr(line_start, end);
        const char *s = z_skip_line_spaces_ptr(line_start, line_end);

        if (s < line_end && *s == '#') {
            char directive[Z_MAX_TOKEN_TEXT];

            ++s;
            if (z_read_directive_word(&s, line_end, directive) != 0) {
                directive[0] = '\0';
            }

            if (z_streq(directive, "ifndef") ||
                z_streq(directive, "ifdef") ||
                z_streq(directive, "if")) {
                ++depth;
            } else if (z_streq(directive, "else") && depth == 1 && stop_at_else) {
                c->pos = (uint32_t)(line_end - c->source);
                if (c->pos < c->size && c->source[c->pos] == '\n') {
                    ++c->pos;
                    ++c->line;
                }
                return 0;
            } else if (z_streq(directive, "endif")) {
                --depth;
                if (depth == 0) {
                    c->pos = (uint32_t)(line_end - c->source);
                    if (c->pos < c->size && c->source[c->pos] == '\n') {
                        ++c->pos;
                        ++c->line;
                    }
                    return 0;
                }
            }
        }

        c->pos = (uint32_t)(line_end - c->source);
        if (c->pos < c->size && c->source[c->pos] == '\n') {
            ++c->pos;
            ++c->line;
        }
    }

    z_set_error(c, c->line);
    return -1;
}

static int z_parse_if_number_text(const char *p, const char *end, uint64_t *out_value) {
    p = z_skip_line_spaces_ptr(p, end);

    if (p < end && z_char_is_ident_start(*p)) {
        char name[Z_MAX_TOKEN_TEXT];
        if (z_read_directive_word(&p, end, name) != 0) {
            return -1;
        }
        p = z_skip_line_spaces_ptr(p, end);
        if (p != end) {
            return -1;
        }
        *out_value = 0;
        return 0;
    }

    return z_parse_define_number_text(p, end, out_value);
}

static int z_handle_preprocessor_directive(z_compiler_t *c) {
    const char *end = c->source + c->size;
    const char *line_start = c->source + c->pos;
    const char *line_end = z_line_end_ptr(line_start, end);
    const char *s = line_start;
    char directive[Z_MAX_TOKEN_TEXT];

    if (s >= line_end || *s != '#') {
        return 0;
    }

    ++s;
    if (z_read_directive_word(&s, line_end, directive) != 0) {
        directive[0] = '\0';
    }

    if (z_streq(directive, "define")) {
        char name[Z_MAX_TOKEN_TEXT];
        uint64_t value = 1;

        if (z_read_directive_word(&s, line_end, name) != 0) {
            z_set_error(c, c->line);
            return -1;
        }

        s = z_skip_line_spaces_ptr(s, line_end);
        if (s < line_end && *s == '(') {
            const char *after_paren = s + 1;
            if (after_paren < line_end && z_char_is_ident_start(*after_paren)) {
                c->pos = (uint32_t)(line_end - c->source);
                return 0;
            }
        }

        if (s < line_end &&
            !(s + 1 < line_end && s[0] == '/' && s[1] == '/') &&
            z_parse_define_number_text(s, line_end, &value) != 0) {
            c->pos = (uint32_t)(line_end - c->source);
            return 0;
        }

        if (z_add_constant(c, name, value) != 0) {
            z_set_error(c, c->line);
            return -1;
        }
    } else if (z_streq(directive, "ifndef") || z_streq(directive, "ifdef")) {
        char name[Z_MAX_TOKEN_TEXT];
        int defined;
        int should_skip;

        if (z_read_directive_word(&s, line_end, name) != 0) {
            z_set_error(c, c->line);
            return -1;
        }

        defined = z_find_constant(c, name) >= 0;
        should_skip = z_streq(directive, "ifndef") ? defined : !defined;
        c->pos = (uint32_t)(line_end - c->source);
        if (c->pos < c->size && c->source[c->pos] == '\n') {
            ++c->pos;
            ++c->line;
        }
        if (should_skip) {
            return z_skip_preprocessor_block(c, 1);
        }
        return 0;
    } else if (z_streq(directive, "if")) {
        uint64_t value;
        int should_skip;

        if (z_parse_if_number_text(s, line_end, &value) != 0) {
            c->pos = (uint32_t)(line_end - c->source);
            return 0;
        }

        should_skip = value == 0;
        c->pos = (uint32_t)(line_end - c->source);
        if (c->pos < c->size && c->source[c->pos] == '\n') {
            ++c->pos;
            ++c->line;
        }
        if (should_skip) {
            return z_skip_preprocessor_block(c, 1);
        }
        return 0;
    } else if (z_streq(directive, "else")) {
        c->pos = (uint32_t)(line_end - c->source);
        if (c->pos < c->size && c->source[c->pos] == '\n') {
            ++c->pos;
            ++c->line;
        }
        return z_skip_preprocessor_block(c, 0);
    } else if (z_streq(directive, "endif") ||
               z_streq(directive, "pragma") ||
               z_streq(directive, "include")) {
        c->pos = (uint32_t)(line_end - c->source);
        return 0;
    }

    c->pos = (uint32_t)(line_end - c->source);
    return 0;
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

        if (c->pos < c->size && c->source[c->pos] == '#') {
            if (z_handle_preprocessor_directive(c) != 0) {
                return;
            }
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
        else if (ch == ':') token.type = Z_TOKEN_COLON;
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

static int z_find_constant(const z_compiler_t *c, const char *name) {
    uint32_t i;

    for (i = 0; i < c->constant_count; ++i) {
        if (z_streq(c->constants[i].name, name)) {
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

static int z_find_typedef(const z_compiler_t *c, const char *name) {
    uint32_t i;

    for (i = 0; i < c->typedef_count; ++i) {
        if (z_streq(c->typedefs[i].name, name)) {
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

static int z_add_typedef(z_compiler_t *c, const char *name, z_type_t type) {
    if (c->typedef_count >= Z_MAX_TYPEDEFS ||
        z_find_typedef(c, name) >= 0 ||
        z_find_constant(c, name) >= 0 ||
        z_find_global(c, name) >= 0 ||
        z_find_function(c, name) >= 0) {
        return -1;
    }

    z_copy_text(c->typedefs[c->typedef_count].name, name);
    c->typedefs[c->typedef_count].type = type;
    ++c->typedef_count;
    return 0;
}

static int z_ensure_struct(z_compiler_t *c, const char *name, int *out_index) {
    int index = z_find_struct(c, name);

    if (index >= 0) {
        *out_index = index;
        return 0;
    }

    if (c->struct_count >= Z_MAX_STRUCTS) {
        return -1;
    }

    index = (int)c->struct_count;
    z_copy_text(c->structs[index].name, name);
    c->structs[index].size_bytes = 0;
    c->structs[index].field_count = 0;
    c->structs[index].is_defined = 0;
    ++c->struct_count;
    *out_index = index;
    return 0;
}

static int z_add_constant(z_compiler_t *c, const char *name, uint64_t value) {
    int existing = z_find_constant(c, name);

    if (existing >= 0) {
        return c->constants[existing].value == value ? 0 : -1;
    }

    if (c->constant_count >= Z_MAX_CONSTANTS ||
        z_find_global(c, name) >= 0 ||
        z_find_function(c, name) >= 0) {
        return -1;
    }

    z_copy_text(c->constants[c->constant_count].name, name);
    c->constants[c->constant_count].value = value;
    ++c->constant_count;
    return 0;
}

static int z_function_signature_same(const z_function_signature_t *a,
                                     z_type_t return_type,
                                     const z_type_t *param_types,
                                     uint32_t param_count) {
    uint32_t i;

    if (!z_type_same(&a->return_type, &return_type) ||
        a->param_count != param_count) {
        return 0;
    }

    for (i = 0; i < param_count; ++i) {
        if (!z_type_same(&a->param_types[i], &param_types[i])) {
            return 0;
        }
    }

    return 1;
}

static int z_add_function_signature(z_compiler_t *c,
                                    z_type_t return_type,
                                    const z_type_t *param_types,
                                    uint32_t param_count) {
    uint32_t i;
    uint32_t param_index;

    for (i = 0; i < c->function_signature_count; ++i) {
        if (z_function_signature_same(&c->function_signatures[i],
                                      return_type,
                                      param_types,
                                      param_count)) {
            return (int)i;
        }
    }

    if (c->function_signature_count >= Z_MAX_FUNCTION_SIGNATURES ||
        param_count > Z_MAX_PARAMS) {
        return -1;
    }

    i = c->function_signature_count;
    c->function_signatures[i].return_type = return_type;
    c->function_signatures[i].param_count = param_count;
    for (param_index = 0; param_index < param_count; ++param_index) {
        c->function_signatures[i].param_types[param_index] = param_types[param_index];
    }

    ++c->function_signature_count;
    return (int)i;
}

static int z_set_function_signature(z_compiler_t *c,
                                    const char *name,
                                    z_type_t return_type,
                                    const z_type_t *param_types,
                                    uint32_t param_count) {
    int function_index = z_find_function(c, name);
    int signature_index = z_add_function_signature(c, return_type, param_types, param_count);

    if (function_index < 0 || signature_index < 0) {
        return -1;
    }

    if (c->functions[function_index].signature_index >= 0 &&
        c->functions[function_index].signature_index != signature_index) {
        return -1;
    }

    c->functions[function_index].signature_index = signature_index;
    return 0;
}

static int z_check_call_argument_type(z_compiler_t *c,
                                      int32_t signature_index,
                                      uint32_t arg_index,
                                      const z_type_t *arg_type) {
    const z_function_signature_t *signature;

    if (signature_index < 0) {
        return 0;
    }

    signature = &c->function_signatures[signature_index];
    if (arg_index >= signature->param_count ||
        !z_type_assignable(&signature->param_types[arg_index], arg_type)) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static int z_check_call_argument_count(z_compiler_t *c,
                                       int32_t signature_index,
                                       uint32_t arg_count) {
    if (signature_index >= 0 &&
        c->function_signatures[signature_index].param_count != arg_count) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static z_type_t z_function_pointer_return_type(z_compiler_t *c, z_type_t type) {
    if (type.function_sig_index >= 0) {
        return c->function_signatures[type.function_sig_index].return_type;
    }

    return z_make_type(Z_TYPE_INT, 0, -1);
}

static int z_function_known_return_assignable(z_compiler_t *c, const char *name, const z_type_t *expected_type) {
    int function_index = z_find_function(c, name);
    int32_t signature_index;

    if (function_index < 0) {
        return 0;
    }

    signature_index = c->functions[function_index].signature_index;
    if (signature_index < 0) {
        return 0;
    }

    return z_type_assignable(expected_type, &c->function_signatures[signature_index].return_type);
}

static int z_current_starts_known_struct_return_call(z_compiler_t *c, const z_type_t *expected_type) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    uint32_t saved_error_line = c->error_line;
    z_token_t saved_current = c->current;
    int result;

    if (!z_type_is_struct_value(expected_type) ||
        c->current.type != Z_TOKEN_IDENT ||
        !z_function_known_return_assignable(c, c->current.text, expected_type)) {
        return 0;
    }

    result = z_next_token(c) == 0 && c->current.type == Z_TOKEN_LPAREN;
    c->pos = saved_pos;
    c->line = saved_line;
    c->error_line = saved_error_line;
    c->current = saved_current;
    return result;
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
    z_make_kind_label(c, c->functions[c->function_count].label, "zf", c->function_count);
    c->functions[c->function_count].is_extern = 0;
    c->functions[c->function_count].is_export = 0;
    c->functions[c->function_count].is_defined = 0;
    c->functions[c->function_count].signature_index = -1;
    z_copy_text(out_label, c->functions[c->function_count].label);
    ++c->function_count;
    return 0;
}

static int z_declare_extern_function(z_compiler_t *c, const char *name) {
    int index = z_find_function(c, name);

    if (index < 0) {
        if (c->function_count >= Z_MAX_FUNCTIONS) {
            return -1;
        }

        index = (int)c->function_count;
        z_copy_text(c->functions[index].name, name);
        z_copy_text(c->functions[index].label, name);
        c->functions[index].is_export = 0;
        c->functions[index].is_defined = 0;
        c->functions[index].signature_index = -1;
        ++c->function_count;
    } else if (c->functions[index].is_defined) {
        return -1;
    }

    c->functions[index].is_extern = 1;
    z_copy_text(c->functions[index].label, name);
    return 0;
}

static int z_mark_export_function(z_compiler_t *c, const char *name) {
    char label[Z_MAX_LABEL_TEXT];
    int index;

    if (z_ensure_function(c, name, label) != 0) {
        return -1;
    }

    index = z_find_function(c, name);
    if (index < 0 || c->functions[index].is_extern) {
        return -1;
    }

    c->functions[index].is_export = 1;
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
                        int has_init,
                        int is_export) {
    uint32_t i;

    if (c->global_count >= Z_MAX_GLOBALS ||
        z_find_global(c, name) >= 0 ||
        z_find_constant(c, name) >= 0 ||
        z_find_function(c, name) >= 0) {
        return -1;
    }

    z_copy_text(c->globals[c->global_count].name, name);
    if (is_export && c->object_mode) {
        z_copy_text(c->globals[c->global_count].label, name);
    } else {
        z_make_kind_label(c, c->globals[c->global_count].label, "zg", c->global_count);
    }
    c->globals[c->global_count].type = type;
    c->globals[c->global_count].array_length = array_length;
    c->globals[c->global_count].dim_count = dim_count;
    c->globals[c->global_count].struct_index = struct_index;
    c->globals[c->global_count].total_size_bytes = total_size_bytes;
    c->globals[c->global_count].init_value = init_value;
    c->globals[c->global_count].has_init = has_init;
    c->globals[c->global_count].is_export = is_export && c->object_mode;
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
    z_make_kind_label(c, c->strings[c->string_count].label, "zs", c->string_count);
    ++c->string_count;
    return (int)(c->string_count - 1u);
}

static int z_parse_expr(z_compiler_t *c, z_type_t *out_type);
static int z_parse_statement(z_compiler_t *c);
static int z_parse_struct_source_address(z_compiler_t *c, z_type_t *out_type);
static int z_parse_struct_return_call_to_rax_destination(z_compiler_t *c,
                                                        z_type_t expected_type,
                                                        z_token_type_t terminator);
static int z_parse_assignment_to_offset(z_compiler_t *c, int32_t offset, z_type_t type, z_token_type_t terminator);
static int z_parse_array_decl_suffixes(z_compiler_t *c,
                                       uint32_t *out_length,
                                       uint32_t *out_dims,
                                       uint32_t *out_dim_count);

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
    z_compiler_t probe;

    if (c->current.type != Z_TOKEN_IDENT) {
        return 0;
    }

    if (z_type_is_qualifier_name(c->current.text)) {
        probe.source = c->source;
        probe.size = c->size;
        probe.pos = c->pos;
        probe.line = c->line;
        probe.current.type = c->current.type;
        probe.current.number = c->current.number;
        probe.current.line = c->current.line;
        z_copy_text(probe.current.text, c->current.text);
        probe.error_line = 0;
        probe.string_pool_used = 0;
        do {
            if (z_next_token(&probe) != 0 || probe.current.type != Z_TOKEN_IDENT) {
                return 0;
            }
        } while (z_type_is_qualifier_name(probe.current.text));

        if (z_streq(probe.current.text, "struct")) {
            return 1;
        }

        return z_ident_type_kind(probe.current.text, &kind) == 0 ||
               z_find_typedef(c, probe.current.text) >= 0;
    }

    if (z_streq(c->current.text, "struct")) {
        return 1;
    }

    return z_ident_type_kind(c->current.text, &kind) == 0 ||
           z_find_typedef(c, c->current.text) >= 0;
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

    while (c->current.type == Z_TOKEN_IDENT && z_type_is_qualifier_name(c->current.text)) {
        if (z_next_token(c) != 0) {
            return -1;
        }
    }

    if (z_streq(c->current.text, "struct")) {
        if (z_parse_struct_type_name(c, struct_name) != 0) {
            return -1;
        }

        if (z_ensure_struct(c, struct_name, &struct_index) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        type = z_make_type(Z_TYPE_STRUCT, 0, struct_index);
    } else if (z_find_typedef(c, c->current.text) >= 0) {
        type = c->typedefs[z_find_typedef(c, c->current.text)].type;
        if (z_next_token(c) != 0) {
            return -1;
        }
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

static int z_parse_function_pointer_parameter_tail(z_compiler_t *c,
                                                   z_type_t *out_param_types,
                                                   uint32_t *out_param_count) {
    z_type_t param_type;
    uint32_t param_count = 0;

    if (z_expect(c, Z_TOKEN_LPAREN) != 0) {
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
            char ignored_name[Z_MAX_TOKEN_TEXT];

            if (z_parse_type(c, &param_type) != 0) {
                return -1;
            }

            if (c->current.type == Z_TOKEN_IDENT) {
                if (z_next_token(c) != 0) {
                    return -1;
                }
            } else if (c->current.type == Z_TOKEN_LPAREN) {
                if (z_next_token(c) != 0 ||
                    z_expect(c, Z_TOKEN_STAR) != 0 ||
                    z_expect_ident(c, ignored_name) != 0 ||
                    z_expect(c, Z_TOKEN_RPAREN) != 0 ||
                    z_parse_function_pointer_parameter_tail(c, 0, 0) != 0) {
                    return -1;
                }
                ++param_type.pointer_depth;
            }

            if (param_count >= Z_MAX_PARAMS) {
                z_set_error(c, c->current.line);
                return -1;
            }
            if (out_param_types) {
                out_param_types[param_count] = param_type;
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

    if (out_param_count) {
        *out_param_count = param_count;
    }

    return z_expect(c, Z_TOKEN_RPAREN);
}

static int z_parse_typed_name(z_compiler_t *c, z_type_t *out_type, char *out_name) {
    c->pending_array_length = 0;
    c->pending_dim_count = 0;

    if (z_parse_type(c, out_type) != 0) {
        return -1;
    }

    if (c->current.type == Z_TOKEN_LPAREN) {
        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_STAR) != 0 ||
            z_expect_ident(c, out_name) != 0 ||
            z_parse_array_decl_suffixes(c,
                                        &c->pending_array_length,
                                        c->pending_dims,
                                        &c->pending_dim_count) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0) {
            return -1;
        }

        {
            z_type_t return_type = *out_type;
            z_type_t param_types[Z_MAX_PARAMS];
            uint32_t param_count = 0;
            int signature_index;

            if (z_parse_function_pointer_parameter_tail(c, param_types, &param_count) != 0) {
                return -1;
            }

            ++out_type->pointer_depth;
            signature_index = z_add_function_signature(c, return_type, param_types, param_count);
            if (signature_index < 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            out_type->function_sig_index = signature_index;
        }
        return 0;
    }

    return z_expect_ident(c, out_name);
}

static int z_parse_array_decl_suffixes(z_compiler_t *c,
                                       uint32_t *out_length,
                                       uint32_t *out_dims,
                                       uint32_t *out_dim_count) {
    uint32_t total = 0;
    uint32_t dim_count = 0;

    if (c->pending_dim_count != 0) {
        uint32_t i;

        *out_length = c->pending_array_length;
        *out_dim_count = c->pending_dim_count;
        for (i = 0; i < c->pending_dim_count; ++i) {
            out_dims[i] = c->pending_dims[i];
        }

        c->pending_array_length = 0;
        c->pending_dim_count = 0;
        return 0;
    }

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
    uint32_t saved_error_line = c->error_line;
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
    c->error_line = saved_error_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_function_definition(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    uint32_t saved_error_line = c->error_line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    z_type_t type;
    int result = 0;

    if (!z_current_is_type_name(c)) {
        if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "export"))) {
            return 0;
        }

        if (z_next_token(c) != 0 || !z_current_is_type_name(c)) {
            c->pos = saved_pos;
            c->line = saved_line;
            c->error_line = saved_error_line;
            c->current = saved_current;
            return 0;
        }
    }

    if (z_parse_typed_name(c, &type, name) == 0 &&
        c->current.type == Z_TOKEN_LPAREN) {
        uint32_t paren_depth = 1;

        if (z_next_token(c) != 0) {
            result = 0;
        } else {
            while (c->current.type != Z_TOKEN_EOF && paren_depth != 0) {
                if (c->current.type == Z_TOKEN_LPAREN) {
                    ++paren_depth;
                } else if (c->current.type == Z_TOKEN_RPAREN) {
                    --paren_depth;
                }

                if (paren_depth != 0 && z_next_token(c) != 0) {
                    break;
                }
            }

            if (paren_depth == 0 &&
                z_next_token(c) == 0 &&
                c->current.type == Z_TOKEN_LBRACE) {
                result = 1;
            }
        }
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->error_line = saved_error_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_extern_function(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    uint32_t saved_error_line = c->error_line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    z_type_t type;
    int result = 0;

    if (c->current.type != Z_TOKEN_IDENT || !z_streq(c->current.text, "extern")) {
        return 0;
    }

    if (z_next_token(c) == 0 &&
        z_current_is_type_name(c) &&
        z_parse_typed_name(c, &type, name) == 0 &&
        c->current.type == Z_TOKEN_LPAREN) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->error_line = saved_error_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_struct_definition(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    uint32_t saved_error_line = c->error_line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    int result = 0;

    if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "struct"))) {
        return 0;
    }

    if (z_parse_struct_type_name(c, name) == 0 &&
        (c->current.type == Z_TOKEN_LBRACE ||
         c->current.type == Z_TOKEN_SEMI)) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->error_line = saved_error_line;
    c->current = saved_current;
    return result;
}

static int z_current_starts_enum_definition(const z_compiler_t *c) {
    return c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "enum");
}

static int z_current_starts_typedef_definition(const z_compiler_t *c) {
    return c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "typedef");
}

static int z_current_starts_global_definition(const z_compiler_t *c) {
    if (c->current.type != Z_TOKEN_IDENT) {
        return 0;
    }

    return z_streq(c->current.text, "global") ||
           z_streq(c->current.text, "static");
}

static int z_parse_constant_literal(z_compiler_t *c, uint64_t *out_value) {
    int negate = 0;

    if (c->current.type == Z_TOKEN_MINUS) {
        negate = 1;
        if (z_next_token(c) != 0) {
            return -1;
        }
    }

    if (c->current.type != Z_TOKEN_NUMBER) {
        z_set_error(c, c->current.line);
        return -1;
    }

    *out_value = negate ? (uint64_t)(0ull - c->current.number) : c->current.number;
    return z_next_token(c);
}

static const char *z_skip_line_spaces_ptr(const char *p, const char *end) {
    while (p < end && z_char_is_line_space(*p)) {
        ++p;
    }
    return p;
}

static const char *z_line_end_ptr(const char *p, const char *end) {
    while (p < end && *p != '\n') {
        ++p;
    }
    return p;
}

static int z_parse_define_number_text(const char *p, const char *end, uint64_t *out_value) {
    uint64_t value = 0;
    int base = 10;
    int negate = 0;
    int saw_digit = 0;

    p = z_skip_line_spaces_ptr(p, end);

    if (p < end && *p == '(') {
        ++p;
        p = z_skip_line_spaces_ptr(p, end);
    }

    if (p < end && *p == '-') {
        negate = 1;
        ++p;
    } else if (p < end && *p == '+') {
        ++p;
    }

    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }

    while (p < end) {
        int digit;

        if (base == 16) {
            if (!z_char_is_hex_digit(*p)) {
                break;
            }
            digit = z_hex_value(*p);
        } else {
            if (!z_char_is_digit(*p)) {
                break;
            }
            digit = *p - '0';
        }

        saw_digit = 1;
        value = value * (uint64_t)base + (uint64_t)digit;
        ++p;
    }

    if (!saw_digit) {
        return -1;
    }

    while (p < end && (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L')) {
        ++p;
    }

    p = z_skip_line_spaces_ptr(p, end);
    if (p < end && *p == ')') {
        ++p;
        p = z_skip_line_spaces_ptr(p, end);
    }

    if (p + 1 < end && p[0] == '/' && p[1] == '/') {
        p = end;
    }

    if (p != end) {
        return -1;
    }

    *out_value = negate ? (uint64_t)(0ull - value) : value;
    return 0;
}

static int z_parse_enum_definition(z_compiler_t *c) {
    uint64_t next_value = 0;

    if (!z_current_starts_enum_definition(c) ||
        z_next_token(c) != 0) {
        return -1;
    }

    if (c->current.type == Z_TOKEN_IDENT) {
        if (z_next_token(c) != 0) {
            return -1;
        }
    }

    if (z_expect(c, Z_TOKEN_LBRACE) != 0) {
        return -1;
    }

    while (c->current.type != Z_TOKEN_RBRACE) {
        char name[Z_MAX_TOKEN_TEXT];
        uint64_t value = next_value;

        if (c->current.type == Z_TOKEN_EOF ||
            z_expect_ident(c, name) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_ASSIGN) {
            if (z_next_token(c) != 0 ||
                z_parse_constant_literal(c, &value) != 0) {
                return -1;
            }
        }

        if (z_add_constant(c, name, value) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }
        next_value = value + 1u;

        if (c->current.type == Z_TOKEN_COMMA) {
            if (z_next_token(c) != 0) {
                return -1;
            }
            if (c->current.type == Z_TOKEN_RBRACE) {
                break;
            }
        } else if (c->current.type != Z_TOKEN_RBRACE) {
            z_set_error(c, c->current.line);
            return -1;
        }
    }

    return z_expect(c, Z_TOKEN_RBRACE) == 0 &&
           z_expect(c, Z_TOKEN_SEMI) == 0 ? 0 : -1;
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
    int is_export = c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "global");

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
        total_size = array_length * z_array_element_size_bytes(c, type);
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

        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_LBRACE) {
            if (z_next_token(c) != 0) {
                return -1;
            }
        }

        if (c->current.type == Z_TOKEN_IDENT) {
            int constant_index = z_find_constant(c, c->current.text);
            if (constant_index < 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            init_value = c->constants[constant_index].value;
            if (z_next_token(c) != 0) {
                return -1;
            }
        } else if (z_parse_constant_literal(c, &init_value) != 0) {
            return -1;
        }
        has_init = 1;

        if (c->current.type == Z_TOKEN_COMMA) {
            if (z_next_token(c) != 0) {
                return -1;
            }
        }

        if (c->current.type == Z_TOKEN_RBRACE &&
            z_next_token(c) != 0) {
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
                     has_init,
                     is_export) < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static int z_parse_struct_body(z_compiler_t *c, int struct_index) {
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

    if (z_expect(c, Z_TOKEN_RBRACE) != 0) {
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

    if (z_ensure_struct(c, struct_name, &struct_index) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (c->current.type == Z_TOKEN_SEMI) {
        return z_next_token(c);
    }

    if (c->current.type != Z_TOKEN_LBRACE ||
        c->structs[struct_index].is_defined) {
        z_set_error(c, c->current.line);
        return -1;
    }

    c->structs[struct_index].size_bytes = 0;
    c->structs[struct_index].field_count = 0;
    c->structs[struct_index].is_defined = 1;

    if (z_parse_struct_body(c, struct_index) != 0 ||
        z_expect(c, Z_TOKEN_SEMI) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_typedef_definition(z_compiler_t *c) {
    char alias_name[Z_MAX_TOKEN_TEXT];
    z_type_t type;

    if (!z_current_starts_typedef_definition(c) ||
        z_next_token(c) != 0) {
        return -1;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "struct")) {
        char struct_name[Z_MAX_TOKEN_TEXT];
        int struct_index;

        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_IDENT) {
            z_copy_text(struct_name, c->current.text);
            if (z_next_token(c) != 0 ||
                z_ensure_struct(c, struct_name, &struct_index) != 0) {
                return -1;
            }
        } else {
            z_make_kind_label(c, struct_name, "zs", c->struct_count);
            if (z_ensure_struct(c, struct_name, &struct_index) != 0) {
                return -1;
            }
        }

        if (c->current.type == Z_TOKEN_LBRACE) {
            if (c->structs[struct_index].is_defined) {
                z_set_error(c, c->current.line);
                return -1;
            }

            c->structs[struct_index].size_bytes = 0;
            c->structs[struct_index].field_count = 0;
            c->structs[struct_index].is_defined = 1;
            if (z_parse_struct_body(c, struct_index) != 0) {
                return -1;
            }
        }

        if (z_expect_ident(c, alias_name) != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        type = z_make_type(Z_TYPE_STRUCT, 0, struct_index);
        if (z_add_typedef(c, alias_name, type) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (z_parse_typed_name(c, &type, alias_name) != 0 ||
        z_expect(c, Z_TOKEN_SEMI) != 0 ||
        z_add_typedef(c, alias_name, type) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static int z_parse_call_expression(z_compiler_t *c, const char *name, z_type_t *out_type) {
    uint32_t arg_count = 0;
    char function_label[Z_MAX_LABEL_TEXT];
    int indirect_target_pushed = 0;
    int32_t signature_index = -1;
    int local_index;
    int global_index;

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

        if (out_type) {
            *out_type = z_make_type(Z_TYPE_INT, 0, -1);
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

        if (out_type) {
            *out_type = z_make_type(Z_TYPE_INT, 0, -1);
        }
        return 0;
    }

    local_index = z_find_local(c, name);
    global_index = z_find_global(c, name);
    if (local_index >= 0) {
        if (c->locals[local_index].type.pointer_depth == 0 ||
            c->locals[local_index].array_length != 0 ||
            z_emit_load_rax_from_offset_typed(c,
                                              c->locals[local_index].stack_offset,
                                              c->locals[local_index].type) != 0 ||
            z_emit_push_rax(c) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }
        signature_index = c->locals[local_index].type.function_sig_index;
        indirect_target_pushed = 1;
    } else if (global_index >= 0) {
        if (c->globals[global_index].type.pointer_depth == 0 ||
            c->globals[global_index].array_length != 0 ||
            z_emit_load_rax_from_label_typed(c,
                                             c->globals[global_index].label,
                                             c->globals[global_index].type) != 0 ||
            z_emit_push_rax(c) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }
        signature_index = c->globals[global_index].type.function_sig_index;
        indirect_target_pushed = 1;
    } else {
        int function_index = z_find_function(c, name);
        if (function_index >= 0) {
            signature_index = c->functions[function_index].signature_index;
        }
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

            z_type_t arg_type;

            if (signature_index >= 0 &&
                arg_count < c->function_signatures[signature_index].param_count &&
                z_type_is_struct_value(&c->function_signatures[signature_index].param_types[arg_count])) {
                if (z_parse_struct_source_address(c, &arg_type) != 0 ||
                    z_check_call_argument_type(c, signature_index, arg_count, &arg_type) != 0 ||
                    z_emit_push_rax(c) != 0) {
                    return -1;
                }
            } else {
                if (z_parse_expr(c, &arg_type) != 0 ||
                    z_check_call_argument_type(c, signature_index, arg_count, &arg_type) != 0 ||
                    z_emit_push_rax(c) != 0) {
                    return -1;
                }
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

    if (z_expect(c, Z_TOKEN_RPAREN) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (!indirect_target_pushed) {
        int function_index;

        if (z_ensure_function(c, name, function_label) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        function_index = z_find_function(c, name);
        if (function_index >= 0) {
            signature_index = c->functions[function_index].signature_index;
        }
    }

    if (z_check_call_argument_count(c, signature_index, arg_count) != 0) {
        return -1;
    }

    if (signature_index >= 0 &&
        z_type_is_struct_value(&c->function_signatures[signature_index].return_type)) {
        z_set_error(c, c->current.line);
        return -1;
    }

    while (arg_count > 0) {
        --arg_count;
        if (z_emit_pop_reg(c, z_arg_registers[arg_count]) != 0) {
            return -1;
        }
    }

    if (indirect_target_pushed) {
        if (z_emit_pop_reg(c, "rax") != 0) {
            return -1;
        }
        if (out_type) {
            *out_type = z_function_pointer_return_type(c,
                                                       local_index >= 0 ? c->locals[local_index].type
                                                                        : c->globals[global_index].type);
        }
        return z_emit_instr1_text(c, "call", "rax");
    }

    if (out_type) {
        *out_type = signature_index >= 0
                    ? c->function_signatures[signature_index].return_type
                    : z_make_type(Z_TYPE_INT, 0, -1);
    }
    return z_emit_instr1_text(c, "call", function_label);
}

static int z_parse_indirect_call_from_rax(z_compiler_t *c, z_type_t callee_type, z_type_t *out_type) {
    uint32_t arg_count = 0;
    int32_t signature_index = callee_type.function_sig_index;

    if (callee_type.pointer_depth == 0 ||
        z_emit_push_rax(c) != 0 ||
        z_expect(c, Z_TOKEN_LPAREN) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (c->current.type != Z_TOKEN_RPAREN) {
        for (;;) {
            if (arg_count >= Z_MAX_PARAMS) {
                z_set_error(c, c->current.line);
                return -1;
            }

            z_type_t arg_type;

            if (signature_index >= 0 &&
                arg_count < c->function_signatures[signature_index].param_count &&
                z_type_is_struct_value(&c->function_signatures[signature_index].param_types[arg_count])) {
                if (z_parse_struct_source_address(c, &arg_type) != 0 ||
                    z_check_call_argument_type(c, signature_index, arg_count, &arg_type) != 0 ||
                    z_emit_push_rax(c) != 0) {
                    return -1;
                }
            } else {
                if (z_parse_expr(c, &arg_type) != 0 ||
                    z_check_call_argument_type(c, signature_index, arg_count, &arg_type) != 0 ||
                    z_emit_push_rax(c) != 0) {
                    return -1;
                }
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

    if (z_expect(c, Z_TOKEN_RPAREN) != 0) {
        return -1;
    }

    if (z_check_call_argument_count(c, signature_index, arg_count) != 0) {
        return -1;
    }

    if (signature_index >= 0 &&
        z_type_is_struct_value(&c->function_signatures[signature_index].return_type)) {
        z_set_error(c, c->current.line);
        return -1;
    }

    while (arg_count > 0) {
        --arg_count;
        if (z_emit_pop_reg(c, z_arg_registers[arg_count]) != 0) {
            return -1;
        }
    }

    if (z_emit_pop_reg(c, "rax") != 0 ||
        z_emit_instr1_text(c, "call", "rax") != 0) {
        return -1;
    }

    if (out_type) {
        *out_type = z_function_pointer_return_type(c, callee_type);
    }
    return 0;
}

static z_type_t z_indexed_value_type(const z_local_t *local) {
    if (local->array_length != 0) {
        return local->type;
    }

    if (local->type.pointer_depth != 0) {
        return z_type_pointee(local->type);
    }

    return local->type;
}

static z_type_t z_indexed_global_value_type(const z_global_t *global) {
    if (global->array_length != 0) {
        return global->type;
    }

    if (global->type.pointer_depth != 0) {
        return z_type_pointee(global->type);
    }

    return global->type;
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

static int z_parse_struct_source_address(z_compiler_t *c, z_type_t *out_type) {
    char name[Z_MAX_TOKEN_TEXT];
    int local_index;
    int global_index;
    z_type_t type;

    if (c->current.type != Z_TOKEN_IDENT) {
        z_set_error(c, c->current.line);
        return -1;
    }

    z_copy_text(name, c->current.text);
    if (z_next_token(c) != 0) {
        return -1;
    }

    local_index = z_find_local(c, name);
    if (local_index >= 0) {
        if (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
            if (z_parse_field_address_from_local(c, local_index, &type) != 0) {
                return -1;
            }
        } else {
            type = c->locals[local_index].type;
            if (z_emit_address_of_offset(c, c->locals[local_index].stack_offset) != 0) {
                return -1;
            }
        }

        if (!z_type_is_struct_value(&type)) {
            z_set_error(c, c->current.line);
            return -1;
        }
        if (out_type) {
            *out_type = type;
        }
        return 0;
    }

    global_index = z_find_global(c, name);
    if (global_index >= 0) {
        if (c->current.type == Z_TOKEN_DOT || c->current.type == Z_TOKEN_ARROW) {
            if (z_parse_field_address_from_global(c, global_index, &type) != 0) {
                return -1;
            }
        } else {
            type = c->globals[global_index].type;
            if (z_emit_instr2_text(c, "mov", "rax", c->globals[global_index].label) != 0) {
                return -1;
            }
        }

        if (!z_type_is_struct_value(&type)) {
            z_set_error(c, c->current.line);
            return -1;
        }
        if (out_type) {
            *out_type = type;
        }
        return 0;
    }

    z_set_error(c, c->current.line);
    return -1;
}

static int z_parse_struct_return_call_to_rax_destination(z_compiler_t *c,
                                                        z_type_t expected_type,
                                                        z_token_type_t terminator) {
    char name[Z_MAX_TOKEN_TEXT];
    char function_label[Z_MAX_LABEL_TEXT];
    int function_index;
    int32_t signature_index;
    const z_function_signature_t *signature;
    uint32_t arg_count = 0;

    if (!z_type_is_struct_value(&expected_type) ||
        c->current.type != Z_TOKEN_IDENT) {
        z_set_error(c, c->current.line);
        return -1;
    }

    z_copy_text(name, c->current.text);
    function_index = z_find_function(c, name);
    if (function_index < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    signature_index = c->functions[function_index].signature_index;
    if (signature_index < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    signature = &c->function_signatures[signature_index];
    if (!z_type_assignable(&expected_type, &signature->return_type)) {
        z_set_error(c, c->current.line);
        return -1;
    }
    z_copy_text(function_label, c->functions[function_index].label);

    if (z_next_token(c) != 0 ||
        z_expect(c, Z_TOKEN_LPAREN) != 0 ||
        z_emit_push_rax(c) != 0) {
        return -1;
    }

    if (c->current.type != Z_TOKEN_RPAREN) {
        for (;;) {
            z_type_t arg_type;

            if (arg_count + 1u >= Z_MAX_PARAMS) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (arg_count < signature->param_count &&
                z_type_is_struct_value(&signature->param_types[arg_count])) {
                if (z_parse_struct_source_address(c, &arg_type) != 0 ||
                    z_check_call_argument_type(c, signature_index, arg_count, &arg_type) != 0 ||
                    z_emit_push_rax(c) != 0) {
                    return -1;
                }
            } else {
                if (z_parse_expr(c, &arg_type) != 0 ||
                    z_check_call_argument_type(c, signature_index, arg_count, &arg_type) != 0 ||
                    z_emit_push_rax(c) != 0) {
                    return -1;
                }
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
        z_check_call_argument_count(c, signature_index, arg_count) != 0) {
        return -1;
    }

    while (arg_count > 0) {
        --arg_count;
        if (z_emit_pop_reg(c, z_arg_registers[arg_count + 1u]) != 0) {
            return -1;
        }
    }

    if (z_emit_pop_reg(c, "rdi") != 0 ||
        z_emit_instr1_text(c, "call", function_label) != 0 ||
        z_expect(c, terminator) != 0) {
        return -1;
    }

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
        uint64_t scale = z_pointer_step_size_bytes(c, c->locals[local_index].type);

        if (c->locals[local_index].dim_count != 0) {
            if (index_depth >= c->locals[local_index].dim_count) {
                z_set_error(c, c->current.line);
                return -1;
            }

            scale = (uint64_t)z_local_index_stride(&c->locals[local_index], index_depth) *
                    z_array_element_size_bytes(c, c->locals[local_index].type);
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
        uint64_t scale = z_pointer_step_size_bytes(c, c->globals[global_index].type);

        if (c->globals[global_index].dim_count != 0) {
            if (index_depth >= c->globals[global_index].dim_count) {
                z_set_error(c, c->current.line);
                return -1;
            }

            scale = (uint64_t)z_global_index_stride(&c->globals[global_index], index_depth) *
                    z_array_element_size_bytes(c, c->globals[global_index].type);
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
    if (c->current.type == Z_TOKEN_STRING) {
        int string_index = z_add_string(c, (uint32_t)c->current.number);

        if (string_index < 0 ||
            z_next_token(c) != 0) {
            return -1;
        }

        if (out_type) {
            *out_type = z_make_type(Z_TYPE_U8, 1, -1);
        }

        return z_emit_instr2_text(c, "mov", "rax", c->strings[string_index].label);
    }

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
            if (z_parse_call_expression(c, name, out_type) != 0) {
                return -1;
            }
            return 0;
        }

        local_index = z_find_local(c, name);
        if (local_index < 0) {
            int constant_index = z_find_constant(c, name);
            int global_index = z_find_global(c, name);

            if (constant_index >= 0) {
                if (out_type) {
                    *out_type = z_make_type(Z_TYPE_INT, 0, -1);
                }
                return z_emit_instr2_u64(c, "mov", "rax", c->constants[constant_index].value);
            }

            if (global_index < 0) {
                int function_index = z_find_function(c, name);

                if (function_index < 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                if (out_type) {
                    *out_type = c->functions[function_index].signature_index >= 0
                                ? c->function_signatures[c->functions[function_index].signature_index].return_type
                                : z_make_type(Z_TYPE_INT, 0, -1);
                    ++out_type->pointer_depth;
                    out_type->function_sig_index = c->functions[function_index].signature_index;
                }
                return z_emit_instr2_text(c, "mov", "rax", c->functions[function_index].label);
            }

            if (c->current.type == Z_TOKEN_LBRACKET) {
                if (z_parse_indexed_global_address(c, global_index, &remaining_dims) != 0) {
                    return -1;
                }

                if (remaining_dims == 0) {
                    z_type_t element_type = z_indexed_global_value_type(&c->globals[global_index]);
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
                z_type_t element_type = z_indexed_value_type(&c->locals[local_index]);
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
    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "sizeof")) {
        uint64_t size_value = 0;

        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_LPAREN) != 0) {
            return -1;
        }

        if (z_current_is_type_name(c)) {
            z_type_t type;

            if (z_parse_type(c, &type) != 0 ||
                z_expect(c, Z_TOKEN_RPAREN) != 0) {
                return -1;
            }

            size_value = z_type_storage_size_bytes(c, type);
        } else if (c->current.type == Z_TOKEN_IDENT) {
            char name[Z_MAX_TOKEN_TEXT];
            int local_index;

            z_copy_text(name, c->current.text);
            if (z_next_token(c) != 0 ||
                z_expect(c, Z_TOKEN_RPAREN) != 0) {
                return -1;
            }

            local_index = z_find_local(c, name);
            if (local_index >= 0) {
                size_value = c->locals[local_index].total_size_bytes;
            } else {
                int global_index = z_find_global(c, name);
                if (global_index < 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }
                size_value = c->globals[global_index].total_size_bytes;
            }
        } else {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (out_type) {
            *out_type = z_make_type(Z_TYPE_U64, 0, -1);
        }
        return z_emit_instr2_u64(c, "mov", "rax", size_value);
    }

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
                int function_index = z_find_function(c, name);

                if (function_index < 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                if (out_type) {
                    *out_type = c->functions[function_index].signature_index >= 0
                                ? c->function_signatures[c->functions[function_index].signature_index].return_type
                                : z_make_type(Z_TYPE_INT, 0, -1);
                    ++out_type->pointer_depth;
                    out_type->function_sig_index = c->functions[function_index].signature_index;
                }
                return z_emit_instr2_text(c, "mov", "rax", c->functions[function_index].label);
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

    {
        z_type_t value_type;

        if (z_parse_primary(c, &value_type) != 0) {
            return -1;
        }

        while (c->current.type == Z_TOKEN_LPAREN) {
            if (z_parse_indirect_call_from_rax(c, value_type, &value_type) != 0) {
                return -1;
            }
        }

        if (out_type) {
            *out_type = value_type;
        }
        return 0;
    }
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
            if (current_type.pointer_depth != 0 && right_type.pointer_depth == 0) {
                if (z_emit_text(c, "    imul rcx, rcx, ") != 0 ||
                    z_emit_u64(c, z_pointer_step_size_bytes(c, current_type)) != 0 ||
                    z_emit_char(c, '\n') != 0) {
                    return -1;
                }
            } else if (right_type.pointer_depth != 0 && current_type.pointer_depth == 0) {
                if (z_emit_imul_rax_imm(c, z_pointer_step_size_bytes(c, right_type)) != 0) {
                    return -1;
                }
                current_type = right_type;
            }

            if (z_emit_instr2_text(c, "add", "rax", "rcx") != 0) {
                return -1;
            }
        } else {
            if (current_type.pointer_depth != 0 && right_type.pointer_depth == 0) {
                if (z_emit_text(c, "    imul rcx, rcx, ") != 0 ||
                    z_emit_u64(c, z_pointer_step_size_bytes(c, current_type)) != 0 ||
                    z_emit_char(c, '\n') != 0) {
                    return -1;
                }
            }

            if (z_emit_instr2_text(c, "sub", "rax", "rcx") != 0) {
                return -1;
            }
        }

        if (!(current_type.pointer_depth != 0 && right_type.pointer_depth == 0)) {
            current_type = z_type_promote_binary(current_type, right_type);
        }
    }

    if (out_type) {
        *out_type = current_type;
    }
    return 0;
}

static int z_parse_condition_or(z_compiler_t *c);

static int z_emit_bool_from_rax_truthy(z_compiler_t *c) {
    char true_label[Z_MAX_LABEL_TEXT];
    char end_label[Z_MAX_LABEL_TEXT];

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
    char true_label[Z_MAX_LABEL_TEXT];
    char end_label[Z_MAX_LABEL_TEXT];
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
        char true_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];

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
        char false_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];

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
        char true_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];

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
            if (z_next_token(c) != 0) {
                return -1;
            }

            if (c->current.type == Z_TOKEN_STRING) {
                uint32_t offset = (uint32_t)c->current.number;
                uint32_t element_size = z_array_element_size_bytes(c, type);

                if (element_size != 1u) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                for (i = 0; i < array_length; ++i) {
                    uint8_t byte_value = (uint8_t)c->string_pool[offset + i];

                    if (z_emit_instr2_u64(c, "mov", "rax", byte_value) != 0 ||
                        z_emit_store_rax_to_offset_typed(c,
                                                         c->locals[local_index].stack_offset +
                                                             (int32_t)i,
                                                         type) != 0) {
                        return -1;
                    }

                    if (byte_value == 0) {
                        ++i;
                        break;
                    }
                }

                if (z_next_token(c) != 0) {
                    return -1;
                }
            } else {
                if (z_expect(c, Z_TOKEN_LBRACE) != 0) {
                    return -1;
                }

                for (i = 0; i < array_length; ++i) {
                    if (c->current.type == Z_TOKEN_RBRACE) {
                        break;
                    }

                    if (z_parse_expr(c, 0) != 0 ||
                        z_emit_store_rax_to_offset_typed(c,
                                                         c->locals[local_index].stack_offset +
                                                             (int32_t)(i * z_array_element_size_bytes(c, type)),
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
            }
        } else {
            i = 0;
        }

        for (; i < array_length; ++i) {
            if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
                z_emit_store_rax_to_offset_typed(c,
                                                 c->locals[local_index].stack_offset +
                                                     (int32_t)(i * z_array_element_size_bytes(c, type)),
                                                 type) != 0) {
                return -1;
            }
        }

        return z_expect(c, Z_TOKEN_SEMI);
    }

    if (struct_index >= 0) {
        uint32_t offset = 0;

        if (c->current.type == Z_TOKEN_ASSIGN) {
            if (z_parse_assignment_to_offset(c,
                                             c->locals[local_index].stack_offset,
                                             type,
                                             Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
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
        int braced = 0;

        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_LBRACE) {
            braced = 1;
            if (z_next_token(c) != 0) {
                return -1;
            }
        }

        if (z_parse_expr(c, 0) != 0 ||
            z_emit_store_rax_to_offset_typed(c, c->locals[local_index].stack_offset, type) != 0) {
            return -1;
        }

        if (braced) {
            if (c->current.type == Z_TOKEN_COMMA &&
                z_next_token(c) != 0) {
                return -1;
            }

            if (z_expect(c, Z_TOKEN_RBRACE) != 0) {
                return -1;
            }
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

    if (z_parse_call_expression(c, name, 0) != 0 ||
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
    z_type_t value_type;

    if (z_next_token(c) != 0) {
        return -1;
    }

    if (z_type_is_struct_value(&type)) {
        uint32_t size = z_type_storage_size_bytes(c, type);

        if (z_current_starts_known_struct_return_call(c, &type)) {
            if (z_emit_address_of_offset(c, offset) != 0 ||
                z_parse_struct_return_call_to_rax_destination(c, type, terminator) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            return 0;
        }

        if (z_parse_struct_source_address(c, &value_type) != 0 ||
            !z_type_assignable(&type, &value_type) ||
            z_emit_instr1_text(c, "push", "rax") != 0 ||
            z_emit_address_of_offset(c, offset) != 0 ||
            z_emit_pop_reg(c, "rsi") != 0 ||
            z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
            z_emit_copy_memory(c, "rdi", "rsi", size) != 0 ||
            z_expect(c, terminator) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (
        z_parse_expr(c, &value_type) != 0 ||
        !z_type_assignable(&type, &value_type) ||
        z_emit_store_rax_to_offset_typed(c, offset, type) != 0 ||
        z_expect(c, terminator) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static int z_parse_assignment_to_reg_ptr(z_compiler_t *c,
                                         const char *address_reg,
                                         z_type_t type,
                                         z_token_type_t terminator) {
    z_type_t value_type;

    if (z_next_token(c) != 0) {
        return -1;
    }

    if (z_type_is_struct_value(&type)) {
        uint32_t size = z_type_storage_size_bytes(c, type);

        if (z_current_starts_known_struct_return_call(c, &type)) {
            if (z_emit_instr2_text(c, "mov", "rax", address_reg) != 0 ||
                z_parse_struct_return_call_to_rax_destination(c, type, terminator) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            return 0;
        }

        if (z_emit_instr1_text(c, "push", address_reg) != 0 ||
            z_parse_struct_source_address(c, &value_type) != 0 ||
            !z_type_assignable(&type, &value_type) ||
            z_emit_instr2_text(c, "mov", "rsi", "rax") != 0 ||
            z_emit_pop_reg(c, "rdi") != 0 ||
            z_emit_copy_memory(c, "rdi", "rsi", size) != 0 ||
            z_expect(c, terminator) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (z_emit_instr1_text(c, "push", address_reg) != 0 ||
        z_parse_expr(c, &value_type) != 0 ||
        !z_type_assignable(&type, &value_type) ||
        z_emit_pop_reg(c, "rdx") != 0 ||
        z_emit_store_rax_to_reg_ptr_typed(c, "rdx", type) != 0 ||
        z_expect(c, terminator) != 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    return 0;
}

static int z_parse_assignment_to_label(z_compiler_t *c,
                                       const char *label,
                                       z_type_t type,
                                       z_token_type_t terminator) {
    z_type_t value_type;

    if (z_next_token(c) != 0) {
        return -1;
    }

    if (z_type_is_struct_value(&type)) {
        uint32_t size = z_type_storage_size_bytes(c, type);

        if (z_current_starts_known_struct_return_call(c, &type)) {
            if (z_emit_instr2_text(c, "mov", "rax", label) != 0 ||
                z_parse_struct_return_call_to_rax_destination(c, type, terminator) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            return 0;
        }

        if (z_parse_struct_source_address(c, &value_type) != 0 ||
            !z_type_assignable(&type, &value_type) ||
            z_emit_instr1_text(c, "push", "rax") != 0 ||
            z_emit_instr2_text(c, "mov", "rax", label) != 0 ||
            z_emit_pop_reg(c, "rsi") != 0 ||
            z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
            z_emit_copy_memory(c, "rdi", "rsi", size) != 0 ||
            z_expect(c, terminator) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return 0;
    }

    if (
        z_parse_expr(c, &value_type) != 0 ||
        !z_type_assignable(&type, &value_type) ||
        z_emit_store_rax_to_label_typed(c, label, type) != 0 ||
        z_expect(c, terminator) != 0) {
        z_set_error(c, c->current.line);
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
            element_type = z_indexed_global_value_type(&c->globals[global_index]);
        } else {
            if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0 ||
                remaining_dims != 0 ||
                z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
            element_type = z_indexed_value_type(&c->locals[local_index]);
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
            total_size = array_length * z_array_element_size_bytes(c, type);
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

        if (z_type_is_struct_value(&c->current_return_type)) {
            z_type_t value_type;

            if (z_current_starts_known_struct_return_call(c, &c->current_return_type)) {
                if (z_emit_load_rax_from_offset_typed(c,
                                                      c->current_struct_return_offset,
                                                      z_make_type(Z_TYPE_I64, 0, -1)) != 0 ||
                    z_parse_struct_return_call_to_rax_destination(c, c->current_return_type, Z_TOKEN_SEMI) != 0 ||
                    z_emit_instr1_text(c, "jmp", c->current_exit_label) != 0) {
                    return -1;
                }
            } else if (z_parse_struct_source_address(c, &value_type) != 0 ||
                       !z_type_assignable(&c->current_return_type, &value_type) ||
                       z_emit_instr2_text(c, "mov", "rsi", "rax") != 0 ||
                       z_emit_load_rax_from_offset_typed(c,
                                                        c->current_struct_return_offset,
                                                        z_make_type(Z_TYPE_I64, 0, -1)) != 0 ||
                       z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
                       z_emit_copy_memory(c,
                                          "rdi",
                                          "rsi",
                                          z_type_storage_size_bytes(c, c->current_return_type)) != 0 ||
                       z_expect(c, Z_TOKEN_SEMI) != 0 ||
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
        char false_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];

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

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "switch")) {
        char dispatch_label[Z_MAX_LABEL_TEXT];
        char default_label[Z_MAX_LABEL_TEXT];
        char cleanup_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];
        char dispatch_out[4096];
        char *saved_out = c->out;
        uint32_t saved_capacity = c->out_capacity;
        uint32_t saved_size = c->out_size;
        uint32_t dispatch_size = 0;
        int have_default = 0;

        z_make_label(c, dispatch_label);
        z_make_label(c, default_label);
        z_make_label(c, cleanup_label);
        z_make_label(c, end_label);

        if (z_next_token(c) != 0 ||
            z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_parse_expr(c, 0) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_emit_instr1_text(c, "jmp", dispatch_label) != 0 ||
            z_expect(c, Z_TOKEN_LBRACE) != 0 ||
            z_push_loop(c, cleanup_label, cleanup_label) != 0) {
            return -1;
        }

        while (c->current.type != Z_TOKEN_RBRACE) {
            if (c->current.type == Z_TOKEN_EOF) {
                z_set_error(c, c->current.line);
                z_pop_loop(c);
                return -1;
            }

            if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "case")) {
                uint64_t case_value;
                char case_label[Z_MAX_LABEL_TEXT];
                uint32_t body_size;

                z_make_label(c, case_label);
                if (z_next_token(c) != 0 ||
                    c->current.type != Z_TOKEN_NUMBER) {
                    z_set_error(c, c->current.line);
                    z_pop_loop(c);
                    return -1;
                }

                case_value = c->current.number;
                body_size = c->out_size;
                c->out = dispatch_out;
                c->out_capacity = sizeof(dispatch_out);
                c->out_size = dispatch_size;
                if (z_emit_instr2_text(c, "mov", "rcx", "[rsp]") != 0 ||
                    z_emit_instr2_u64(c, "cmp", "rcx", case_value) != 0 ||
                    z_emit_instr1_text(c, "je", case_label) != 0) {
                    c->out = saved_out;
                    c->out_capacity = saved_capacity;
                    c->out_size = saved_size;
                    z_pop_loop(c);
                    return -1;
                }
                dispatch_size = c->out_size;
                c->out = saved_out;
                c->out_capacity = saved_capacity;
                c->out_size = body_size;

                if (z_next_token(c) != 0 ||
                    z_expect(c, Z_TOKEN_COLON) != 0 ||
                    z_emit_label(c, case_label) != 0) {
                    z_pop_loop(c);
                    return -1;
                }
                continue;
            }

            if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "default")) {
                if (have_default ||
                    z_next_token(c) != 0 ||
                    z_expect(c, Z_TOKEN_COLON) != 0 ||
                    z_emit_label(c, default_label) != 0) {
                    z_set_error(c, c->current.line);
                    z_pop_loop(c);
                    return -1;
                }
                have_default = 1;
                continue;
            }

            if (z_parse_statement(c) != 0) {
                z_pop_loop(c);
                return -1;
            }
        }

        z_pop_loop(c);
        if (z_emit_instr1_text(c, "jmp", cleanup_label) != 0 ||
            z_emit_label(c, dispatch_label) != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < dispatch_size; ++i) {
            if (z_emit_char(c, dispatch_out[i]) != 0) {
                return -1;
            }
        }

        if (z_emit_instr1_text(c, "jmp", have_default ? default_label : cleanup_label) != 0 ||
            z_emit_label(c, cleanup_label) != 0 ||
            z_emit_instr2_u64(c, "add", "rsp", 8) != 0 ||
            z_emit_label(c, end_label) != 0 ||
            z_expect(c, Z_TOKEN_RBRACE) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "while")) {
        char start_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];

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
        char start_label[Z_MAX_LABEL_TEXT];
        char post_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];
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
        char start_label[Z_MAX_LABEL_TEXT];
        char cond_label[Z_MAX_LABEL_TEXT];
        char end_label[Z_MAX_LABEL_TEXT];

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

            if (c->locals[local_index].array_length != 0 ||
                z_parse_assignment_to_offset(c,
                                             c->locals[local_index].stack_offset,
                                             c->locals[local_index].type,
                                             Z_TOKEN_SEMI) != 0) {
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
                    z_type_is_struct_value(&c->globals[global_index].type) ||
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

            if (z_type_is_struct_value(&c->locals[local_index].type) ||
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
                    z_type_is_struct_value(&c->globals[global_index].type) ||
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

            if (z_type_is_struct_value(&c->locals[local_index].type) ||
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
                if (z_parse_assignment_to_reg_ptr(c, "rdx", field_type, Z_TOKEN_SEMI) != 0) {
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

            if (c->current.type == Z_TOKEN_LPAREN) {
                if (z_type_is_struct_value(&field_type) ||
                    z_emit_instr2_text(c, "mov", "rcx", "rdx") != 0 ||
                    z_emit_load_rax_from_reg_ptr_typed(c, "rcx", field_type) != 0 ||
                    z_parse_indirect_call_from_rax(c, field_type, 0) != 0 ||
                    z_expect(c, Z_TOKEN_SEMI) != 0) {
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

                element_type = z_indexed_global_value_type(&c->globals[global_index]);
            } else {
                if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0 ||
                    remaining_dims != 0 ||
                    z_emit_instr2_text(c, "mov", "rdx", "rax") != 0) {
                    z_set_error(c, c->current.line);
                    return -1;
                }

                element_type = z_indexed_value_type(&c->locals[local_index]);
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

            if (c->current.type == Z_TOKEN_LPAREN) {
                if (z_emit_instr2_text(c, "mov", "rcx", "rdx") != 0 ||
                    z_emit_load_rax_from_reg_ptr_typed(c, "rcx", element_type) != 0 ||
                    z_parse_indirect_call_from_rax(c, element_type, 0) != 0 ||
                    z_expect(c, Z_TOKEN_SEMI) != 0) {
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

static int z_begin_function(z_compiler_t *c, const char *name, int is_export) {
    int function_index;

    c->local_count = 0;
    c->next_stack_offset = 0;
    c->in_function = 1;
    c->current_struct_return_offset = 0;
    if (is_export && z_mark_export_function(c, name) != 0) {
        return -1;
    }
    if (z_ensure_function(c, name, c->current_function_label) != 0) {
        return -1;
    }
    z_make_label(c, c->current_exit_label);

    function_index = z_find_function(c, name);
    if (function_index < 0 ||
        c->functions[function_index].is_extern ||
        c->functions[function_index].is_defined) {
        return -1;
    }

    c->functions[function_index].is_defined = 1;
    if ((is_export &&
         (z_emit_text(c, "; zo_export ") != 0 ||
          z_emit_text(c, name) != 0 ||
          z_emit_char(c, ' ') != 0 ||
          z_emit_text(c, c->current_function_label) != 0 ||
          z_emit_char(c, '\n') != 0 ||
          z_emit_label(c, name) != 0)) ||
        z_emit_label(c, c->current_function_label) != 0 ||
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

static int z_parse_function_definition(z_compiler_t *c,
                                       const char *name,
                                       z_type_t return_type,
                                       int is_export) {
    char param_names[Z_MAX_PARAMS][Z_MAX_TOKEN_TEXT];
    z_type_t param_types[Z_MAX_PARAMS];
    uint32_t param_count = 0;
    uint32_t i;

    if (z_expect(c, Z_TOKEN_LPAREN) != 0) {
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

    if (z_expect(c, Z_TOKEN_RPAREN) != 0 ||
        z_set_function_signature(c, name, return_type, param_types, param_count) != 0 ||
        z_begin_function(c, name, is_export) != 0) {
        return -1;
    }

    c->current_return_type = return_type;
    if (z_type_is_struct_value(&return_type)) {
        if (param_count + 1u > Z_MAX_PARAMS) {
            z_set_error(c, c->current.line);
            return -1;
        }

        c->next_stack_offset -= 8;
        c->current_struct_return_offset = c->next_stack_offset;
        if (z_emit_instr2_u64(c, "sub", "rsp", 8) != 0 ||
            z_emit_text(c, "    mov [rbp") != 0 ||
            z_emit_i32(c, c->current_struct_return_offset) != 0 ||
            z_emit_text(c, "], rdi\n") != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }
    }

    for (i = 0; i < param_count; ++i) {
        const char *param_reg = z_arg_registers[i + (z_type_is_struct_value(&return_type) ? 1u : 0u)];
        int32_t param_struct_index = z_type_is_struct_value(&param_types[i]) ? param_types[i].struct_index : -1;
        uint32_t param_size = param_struct_index >= 0
                              ? z_type_storage_size_bytes(c, param_types[i])
                              : 8u;
        int local_index = z_add_local(c, param_names[i], param_types[i], 0, 0, 0, param_struct_index, param_size);

        if (local_index < 0 ||
            z_emit_instr2_u64(c, "sub", "rsp", param_size) != 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (param_struct_index >= 0) {
            if (z_emit_instr2_text(c, "mov", "rsi", param_reg) != 0 ||
                z_emit_address_of_offset(c, c->locals[local_index].stack_offset) != 0 ||
                z_emit_instr2_text(c, "mov", "rdi", "rax") != 0 ||
                z_emit_copy_memory(c, "rdi", "rsi", param_size) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }
        } else if (z_emit_text(c, "    mov [rbp") != 0 ||
                   z_emit_i32(c, c->locals[local_index].stack_offset) != 0 ||
                   z_emit_text(c, "], ") != 0 ||
                   z_emit_text(c, param_reg) != 0 ||
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

static int z_parse_function_prototype_tail(z_compiler_t *c,
                                           z_type_t *out_param_types,
                                           uint32_t *out_param_count) {
    if (z_parse_function_pointer_parameter_tail(c, out_param_types, out_param_count) != 0 ||
        z_expect(c, Z_TOKEN_SEMI) != 0) {
        return -1;
    }

    return 0;
}

static int z_current_starts_function_prototype(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    uint32_t saved_error_line = c->error_line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    z_type_t type;
    int result = 0;

    if (z_current_is_type_name(c) &&
        z_parse_typed_name(c, &type, name) == 0 &&
        c->current.type == Z_TOKEN_LPAREN &&
        z_parse_function_prototype_tail(c, 0, 0) == 0) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->error_line = saved_error_line;
    c->current = saved_current;
    return result;
}

static int z_parse_function_prototype(z_compiler_t *c) {
    z_type_t function_type;
    z_type_t param_types[Z_MAX_PARAMS];
    uint32_t param_count = 0;
    char function_name[Z_MAX_TOKEN_TEXT];
    char function_label[Z_MAX_LABEL_TEXT];

    if (!z_current_starts_function_prototype(c) ||
        z_parse_typed_name(c, &function_type, function_name) != 0 ||
        z_ensure_function(c, function_name, function_label) != 0 ||
        z_parse_function_prototype_tail(c, param_types, &param_count) != 0 ||
        z_set_function_signature(c, function_name, function_type, param_types, param_count) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_extern_function(z_compiler_t *c) {
    z_type_t function_type;
    z_type_t param_types[Z_MAX_PARAMS];
    uint32_t param_count = 0;
    char function_name[Z_MAX_TOKEN_TEXT];

    if (!z_current_starts_extern_function(c) ||
        z_next_token(c) != 0 ||
        z_parse_typed_name(c, &function_type, function_name) != 0 ||
        z_declare_extern_function(c, function_name) != 0 ||
        z_emit_text(c, "; zo_extern ") != 0 ||
        z_emit_text(c, function_name) != 0 ||
        z_emit_char(c, '\n') != 0 ||
        z_parse_function_prototype_tail(c, param_types, &param_count) != 0 ||
        z_set_function_signature(c, function_name, function_type, param_types, param_count) != 0) {
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

    if (global->is_export &&
        (z_emit_text(c, "; zo_export ") != 0 ||
         z_emit_text(c, global->name) != 0 ||
         z_emit_char(c, ' ') != 0 ||
         z_emit_text(c, global->label) != 0 ||
         z_emit_char(c, '\n') != 0)) {
        return -1;
    }

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

static int zscript_compile_source_internal(const char *source,
                                           uint32_t size,
                                           const char *label_prefix,
                                           int emit_start_stub,
                                           char *out,
                                           uint32_t out_capacity,
                                           uint32_t *out_size,
                                           uint32_t *error_line,
                                           char *entry_label_out,
                                           uint32_t entry_label_capacity) {
    z_compiler_t c;
    char entry_label[Z_MAX_LABEL_TEXT];
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
    c.label_prefix[0] = '\0';
    c.object_mode = !emit_start_stub;
    if (label_prefix != 0) {
        i = 0;
        while (label_prefix[i] && i + 1u < Z_MAX_LABEL_PREFIX) {
            c.label_prefix[i] = label_prefix[i];
            ++i;
        }
        c.label_prefix[i] = '\0';
    }
    c.string_pool_used = 0;
    c.string_count = 0;
    c.function_count = 0;
    c.struct_count = 0;
    c.global_count = 0;
    c.constant_count = 0;
    c.typedef_count = 0;
    c.function_signature_count = 0;
    c.in_function = 0;
    c.current_function_label[0] = '\0';
    c.current_exit_label[0] = '\0';
    c.current_return_type = z_make_type(Z_TYPE_INT, 0, -1);
    c.current_struct_return_offset = 0;
    c.local_count = 0;
    c.next_stack_offset = 0;
    c.loop_depth = 0;
    c.pending_array_length = 0;
    c.pending_dim_count = 0;

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

    while (c.current.type != Z_TOKEN_EOF) {
        int parsed_declaration = 0;

        if (z_current_starts_typedef_definition(&c)) {
            parsed_declaration = 1;
            if (z_parse_typedef_definition(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        } else if (z_current_starts_struct_definition(&c)) {
            parsed_declaration = 1;
            if (z_parse_struct_definition(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        } else if (z_current_starts_enum_definition(&c)) {
            parsed_declaration = 1;
            if (z_parse_enum_definition(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        } else if (z_current_starts_global_definition(&c)) {
            parsed_declaration = 1;
            if (z_parse_global_definition(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        } else if (z_current_starts_extern_function(&c)) {
            parsed_declaration = 1;
            if (z_parse_extern_function(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        } else if (z_current_starts_function_prototype(&c)) {
            parsed_declaration = 1;
            if (z_parse_function_prototype(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        }

        if (!parsed_declaration) {
            break;
        }
    }

    while (c.current.type != Z_TOKEN_EOF && z_current_starts_extern_function(&c)) {
        if (z_parse_extern_function(&c) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    if (entry_label_out != 0 && entry_label_capacity != 0) {
        i = 0;
        while (entry_label[i] && i + 1u < entry_label_capacity) {
            entry_label_out[i] = entry_label[i];
            ++i;
        }
        entry_label_out[i] = '\0';
    }

    if (c.object_mode &&
        (z_emit_line(&c, "; zo_extern puts") != 0 ||
         z_emit_line(&c, "; zo_extern put_dec64") != 0 ||
         z_emit_line(&c, "; zo_extern put_hex64") != 0)) {
        if (error_line) {
            *error_line = c.error_line;
        }
        return -1;
    }

    if (z_emit_line(&c, "bits 64") != 0 ||
        z_emit_line(&c, "default rel") != 0 ||
        z_emit_line(&c, "section .text") != 0) {
        if (error_line) {
            *error_line = c.error_line;
        }
        return -1;
    }

    if (emit_start_stub) {
        if (z_emit_line(&c, "start:") != 0 ||
            z_emit_instr1_text(&c, "call", entry_label) != 0 ||
            z_emit_instr0(&c, "ret") != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    while (c.current.type != Z_TOKEN_EOF && z_current_starts_function_definition(&c)) {
        char function_name[Z_MAX_TOKEN_TEXT];
        char function_label[Z_MAX_LABEL_TEXT];
        z_type_t function_type;
        int is_export = 0;

        if (c.current.type == Z_TOKEN_IDENT && z_streq(c.current.text, "export")) {
            is_export = 1;
            if (z_next_token(&c) != 0) {
                if (error_line) {
                    *error_line = c.error_line;
                }
                return -1;
            }
        }

        if (z_parse_typed_name(&c, &function_type, function_name) != 0 ||
            c.current.type != Z_TOKEN_LPAREN ||
            z_ensure_function(&c, function_name, function_label) != 0 ||
            z_parse_function_definition(&c, function_name, function_type, is_export) != 0) {
            if (error_line) {
                *error_line = c.error_line;
            }
            return -1;
        }
    }

    c.local_count = 0;
    c.next_stack_offset = 0;
    c.in_function = 1;
    c.current_return_type = z_make_type(Z_TYPE_INT, 0, -1);
    c.current_struct_return_offset = 0;
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

int zscript_compile_source(const char *source,
                           uint32_t size,
                           char *out,
                           uint32_t out_capacity,
                           uint32_t *out_size,
                           uint32_t *error_line) {
    return zscript_compile_source_internal(source,
                                           size,
                                           "",
                                           1,
                                           out,
                                           out_capacity,
                                           out_size,
                                           error_line,
                                           0,
                                           0);
}

int zscript_compile_source_object(const char *source,
                                  uint32_t size,
                                  const char *label_prefix,
                                  char *out,
                                  uint32_t out_capacity,
                                  uint32_t *out_size,
                                  uint32_t *error_line,
                                  char *entry_label,
                                  uint32_t entry_label_capacity) {
    return zscript_compile_source_internal(source,
                                           size,
                                           label_prefix,
                                           0,
                                           out,
                                           out_capacity,
                                           out_size,
                                           error_line,
                                           entry_label,
                                           entry_label_capacity);
}
