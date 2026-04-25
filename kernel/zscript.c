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
    Z_TOKEN_PLUS,
    Z_TOKEN_MINUS,
    Z_TOKEN_STAR,
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

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    int32_t stack_offset;
    uint32_t array_length;
    uint32_t dim_count;
    uint32_t dims[Z_MAX_ARRAY_DIMS];
    int32_t struct_index;
    uint32_t total_size_bytes;
} z_local_t;

typedef struct {
    char name[Z_MAX_TOKEN_TEXT];
    uint32_t offset;
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
    z_string_t strings[Z_MAX_STRINGS];
    z_function_t functions[Z_MAX_FUNCTIONS];
    z_struct_t structs[Z_MAX_STRUCTS];
    char string_pool[Z_STRING_POOL_SIZE];
    int in_function;
    char current_function_label[20];
    char current_exit_label[16];
    uint32_t local_count;
    z_local_t locals[Z_MAX_LOCALS];
    int32_t next_stack_offset;
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

static int z_emit_store_rax_to_offset(z_compiler_t *c, int32_t offset) {
    return z_emit_text(c, "    mov [rbp") ||
           z_emit_i32(c, offset) ||
           z_emit_text(c, "], rax\n");
}

static int z_emit_store_rax_to_reg_ptr(z_compiler_t *c, const char *reg) {
    return z_emit_text(c, "    mov [") ||
           z_emit_text(c, reg) ||
           z_emit_text(c, "], rax\n");
}

static int z_emit_load_rax_from_offset(z_compiler_t *c, int32_t offset) {
    return z_emit_text(c, "    mov rax, [rbp") ||
           z_emit_i32(c, offset) ||
           z_emit_text(c, "]\n");
}

static int z_emit_load_rax_from_reg_ptr(z_compiler_t *c, const char *reg) {
    return z_emit_text(c, "    mov rax, [") ||
           z_emit_text(c, reg) ||
           z_emit_text(c, "]\n");
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
        else if (ch == '+') token.type = Z_TOKEN_PLUS;
        else if (ch == '-') token.type = Z_TOKEN_MINUS;
        else if (ch == '*') token.type = Z_TOKEN_STAR;
        else if (ch == '=') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_EQ;
            } else {
                token.type = Z_TOKEN_ASSIGN;
            }
        } else if (ch == '&') {
            token.type = Z_TOKEN_AMP;
        } else if (ch == '!') {
            if (c->pos < c->size && c->source[c->pos] == '=') {
                ++c->pos;
                token.type = Z_TOKEN_NE;
            } else {
                z_set_error(c, token.line);
                return -1;
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
    c->locals[c->local_count].struct_index = struct_index;
    c->locals[c->local_count].total_size_bytes = total_size_bytes;
    for (i = 0; i < Z_MAX_ARRAY_DIMS; ++i) {
        c->locals[c->local_count].dims[i] = (i < dim_count) ? dims[i] : 0;
    }
    ++c->local_count;
    return (int)(c->local_count - 1u);
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

static int z_parse_expr(z_compiler_t *c);
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

static int z_parse_typed_name(z_compiler_t *c, char *out_name) {
    if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "int"))) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (z_next_token(c) != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_STAR) {
        if (z_next_token(c) != 0) {
            return -1;
        }
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

static int z_current_starts_function_definition(z_compiler_t *c) {
    uint32_t saved_pos = c->pos;
    uint32_t saved_line = c->line;
    z_token_t saved_current = c->current;
    char name[Z_MAX_TOKEN_TEXT];
    int result = 0;

    if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "int"))) {
        return 0;
    }

    if (z_parse_typed_name(c, name) == 0 &&
        c->current.type == Z_TOKEN_LPAREN) {
        result = 1;
    }

    c->pos = saved_pos;
    c->line = saved_line;
    c->current = saved_current;
    return result;
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
        uint32_t field_index;

        if (!(c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "int"))) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (z_next_token(c) != 0 ||
            z_expect_ident(c, field_name) != 0) {
            return -1;
        }

        if (c->structs[struct_index].field_count >= Z_MAX_STRUCT_FIELDS ||
            z_find_struct_field(&c->structs[struct_index], field_name) >= 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        field_index = c->structs[struct_index].field_count++;
        z_copy_text(c->structs[struct_index].fields[field_index].name, field_name);
        c->structs[struct_index].fields[field_index].offset = c->structs[struct_index].size_bytes;
        c->structs[struct_index].size_bytes += 8u;

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

    if (z_streq(name, "ticks")) {
        if (z_expect(c, Z_TOKEN_LPAREN) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0 ||
            z_emit_instr0(c, "call ticks") != 0) {
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

            if (z_parse_expr(c) != 0 ||
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

    return z_emit_load_rax_from_offset(c, c->locals[local_index].stack_offset);
}

static int z_emit_local_field_address(z_compiler_t *c, int local_index, uint32_t field_offset) {
    return z_emit_address_of_offset(c, c->locals[local_index].stack_offset + (int32_t)field_offset);
}

static int z_parse_local_field(z_compiler_t *c, int local_index, uint32_t *out_field_offset) {
    char field_name[Z_MAX_TOKEN_TEXT];
    int struct_index;
    int field_index;

    if (c->locals[local_index].struct_index < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (z_expect(c, Z_TOKEN_DOT) != 0 ||
        z_expect_ident(c, field_name) != 0) {
        return -1;
    }

    struct_index = c->locals[local_index].struct_index;
    field_index = z_find_struct_field(&c->structs[struct_index], field_name);
    if (field_index < 0) {
        z_set_error(c, c->current.line);
        return -1;
    }

    *out_field_offset = c->structs[struct_index].fields[field_index].offset;
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
            z_parse_expr(c) != 0 ||
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

static int z_parse_primary(z_compiler_t *c) {
    if (c->current.type == Z_TOKEN_NUMBER) {
        uint64_t value = c->current.number;

        if (z_next_token(c) != 0) {
            return -1;
        }

        return z_emit_instr2_u64(c, "mov", "rax", value);
    }

    if (c->current.type == Z_TOKEN_IDENT) {
        char name[Z_MAX_TOKEN_TEXT];
        int local_index;
        uint32_t remaining_dims = 0;
        uint32_t field_offset = 0;

        z_copy_text(name, c->current.text);
        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_LPAREN) {
            return z_parse_call_expression(c, name);
        }

        local_index = z_find_local(c, name);
        if (local_index < 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (c->current.type == Z_TOKEN_LBRACKET) {
            if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0) {
                return -1;
            }

            if (remaining_dims == 0) {
                if (z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
                    z_emit_load_rax_from_reg_ptr(c, "rcx") != 0) {
                    return -1;
                }
            }

            return 0;
        }

        if (c->current.type == Z_TOKEN_DOT) {
            if (z_parse_local_field(c, local_index, &field_offset) != 0) {
                return -1;
            }

            return z_emit_load_rax_from_offset(c, c->locals[local_index].stack_offset + (int32_t)field_offset);
        }

        if (c->locals[local_index].array_length != 0) {
            return z_emit_address_of_offset(c, c->locals[local_index].stack_offset);
        }

        return z_emit_load_rax_from_offset(c, c->locals[local_index].stack_offset);
    }

    if (c->current.type == Z_TOKEN_LPAREN) {
        if (z_next_token(c) != 0 ||
            z_parse_expr(c) != 0 ||
            z_expect(c, Z_TOKEN_RPAREN) != 0) {
            return -1;
        }

        return 0;
    }

    z_set_error(c, c->current.line);
    return -1;
}

static int z_parse_unary(z_compiler_t *c) {
    if (c->current.type == Z_TOKEN_AMP) {
        char name[Z_MAX_TOKEN_TEXT];
        int local_index;
        uint32_t remaining_dims = 0;
        uint32_t field_offset = 0;

        if (z_next_token(c) != 0 ||
            z_expect_ident(c, name) != 0) {
            return -1;
        }

        local_index = z_find_local(c, name);
        if (local_index < 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        if (c->current.type == Z_TOKEN_LBRACKET) {
            if (z_parse_indexed_address(c, local_index, &remaining_dims) != 0) {
                return -1;
            }
            return 0;
        }

        if (c->current.type == Z_TOKEN_DOT) {
            if (z_parse_local_field(c, local_index, &field_offset) != 0) {
                return -1;
            }

            return z_emit_local_field_address(c, local_index, field_offset);
        }

        return z_emit_address_of_offset(c, c->locals[local_index].stack_offset);
    }

    if (c->current.type == Z_TOKEN_STAR) {
        if (z_next_token(c) != 0 ||
            z_parse_unary(c) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_load_rax_from_reg_ptr(c, "rcx") != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_MINUS) {
        if (z_next_token(c) != 0 ||
            z_parse_unary(c) != 0 ||
            z_emit_instr2_u64(c, "mov", "rcx", 0) != 0 ||
            z_emit_instr2_text(c, "sub", "rcx", "rax") != 0 ||
            z_emit_instr2_text(c, "mov", "rax", "rcx") != 0) {
            return -1;
        }

        return 0;
    }

    return z_parse_primary(c);
}

static int z_parse_mul(z_compiler_t *c) {
    if (z_parse_unary(c) != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_STAR) {
        if (z_next_token(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_parse_unary(c) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_pop_reg(c, "rax") != 0 ||
            z_emit_instr2_text(c, "imul", "rax", "rcx") != 0) {
            return -1;
        }
    }

    return 0;
}

static int z_parse_expr(z_compiler_t *c) {
    if (z_parse_mul(c) != 0) {
        return -1;
    }

    while (c->current.type == Z_TOKEN_PLUS || c->current.type == Z_TOKEN_MINUS) {
        z_token_type_t op = c->current.type;

        if (z_next_token(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_parse_mul(c) != 0 ||
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
    }

    return 0;
}

static int z_emit_false_jump(z_compiler_t *c, const char *false_label) {
    z_token_type_t op;

    if (z_parse_expr(c) != 0) {
        return -1;
    }

    op = c->current.type;
    if (op == Z_TOKEN_EQ || op == Z_TOKEN_NE ||
        op == Z_TOKEN_LT || op == Z_TOKEN_LE ||
        op == Z_TOKEN_GT || op == Z_TOKEN_GE) {
        if (z_next_token(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_parse_expr(c) != 0 ||
            z_emit_instr2_text(c, "mov", "rcx", "rax") != 0 ||
            z_emit_pop_reg(c, "rax") != 0 ||
            z_emit_instr2_text(c, "cmp", "rax", "rcx") != 0) {
            return -1;
        }

        if (op == Z_TOKEN_EQ) return z_emit_instr1_text(c, "jne", false_label);
        if (op == Z_TOKEN_NE) return z_emit_instr1_text(c, "je", false_label);
        if (op == Z_TOKEN_LT) return z_emit_instr1_text(c, "jge", false_label);
        if (op == Z_TOKEN_LE) return z_emit_instr1_text(c, "jg", false_label);
        if (op == Z_TOKEN_GT) return z_emit_instr1_text(c, "jle", false_label);
        return z_emit_instr1_text(c, "jl", false_label);
    }

    if (z_emit_instr2_u64(c, "cmp", "rax", 0) != 0) {
        return -1;
    }

    return z_emit_instr1_text(c, "je", false_label);
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
                            uint32_t array_length,
                            const uint32_t *dims,
                            uint32_t dim_count,
                            int32_t struct_index,
                            uint32_t total_size_bytes) {
    int local_index;
    uint64_t alloc_size = (uint64_t)total_size_bytes;

    local_index = z_add_local(c, name, array_length, dims, dim_count, struct_index, total_size_bytes);

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

                if (z_parse_expr(c) != 0 ||
                    z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset + (int32_t)(i * 8u)) != 0) {
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
                z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset + (int32_t)(i * 8u)) != 0) {
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
            z_parse_expr(c) != 0 ||
            z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset) != 0) {
            return -1;
        }
    } else {
        if (z_emit_instr2_u64(c, "mov", "rax", 0) != 0 ||
            z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset) != 0) {
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

        if (z_parse_expr(c) != 0 ||
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
            z_parse_expr(c) != 0 ||
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
            z_parse_expr(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c) != 0 ||
            z_emit_push_rax(c) != 0 ||
            z_expect(c, Z_TOKEN_COMMA) != 0 ||
            z_parse_expr(c) != 0 ||
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

    if (z_parse_call_expression(c, name) != 0 ||
        z_expect(c, Z_TOKEN_SEMI) != 0) {
        return -1;
    }

    return 0;
}

static int z_parse_statement(z_compiler_t *c) {
    if (!c->in_function) {
        z_set_error(c, c->current.line);
        return -1;
    }

    if (c->current.type == Z_TOKEN_LBRACE) {
        return z_parse_block(c);
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "int")) {
        char name[Z_MAX_TOKEN_TEXT];
        uint32_t array_length = 0;
        uint32_t dims[Z_MAX_ARRAY_DIMS];
        uint32_t dim_count = 0;

        if (z_parse_typed_name(c, name) != 0 ||
            z_parse_array_decl_suffixes(c, &array_length, dims, &dim_count) != 0) {
            return -1;
        }

        return z_parse_var_decl(c,
                                name,
                                array_length,
                                dims,
                                dim_count,
                                -1,
                                array_length == 0 ? 8u : array_length * 8u);
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "struct")) {
        char struct_name[Z_MAX_TOKEN_TEXT];
        char var_name[Z_MAX_TOKEN_TEXT];
        uint32_t dims[Z_MAX_ARRAY_DIMS] = {0, 0, 0};
        int struct_index;

        if (z_parse_struct_type_name(c, struct_name) != 0 ||
            z_expect_ident(c, var_name) != 0) {
            return -1;
        }

        struct_index = z_find_struct(c, struct_name);
        if (struct_index < 0) {
            z_set_error(c, c->current.line);
            return -1;
        }

        return z_parse_var_decl(c,
                                var_name,
                                0,
                                dims,
                                0,
                                struct_index,
                                c->structs[struct_index].size_bytes);
    }

    if (c->current.type == Z_TOKEN_STAR) {
        if (z_next_token(c) != 0 ||
            z_parse_unary(c) != 0 ||
            z_emit_instr2_text(c, "mov", "rdx", "rax") != 0 ||
            z_expect(c, Z_TOKEN_ASSIGN) != 0 ||
            z_parse_expr(c) != 0 ||
            z_emit_store_rax_to_reg_ptr(c, "rdx") != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT && z_streq(c->current.text, "return")) {
        if (z_next_token(c) != 0 ||
            z_parse_expr(c) != 0 ||
            z_expect(c, Z_TOKEN_SEMI) != 0 ||
            z_emit_instr1_text(c, "jmp", c->current_exit_label) != 0) {
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
            z_parse_statement(c) != 0 ||
            z_emit_instr1_text(c, "jmp", start_label) != 0 ||
            z_emit_label(c, end_label) != 0) {
            return -1;
        }

        return 0;
    }

    if (c->current.type == Z_TOKEN_IDENT) {
        char name[Z_MAX_TOKEN_TEXT];
        int local_index;
        uint32_t field_offset = 0;

        z_copy_text(name, c->current.text);
        if (z_next_token(c) != 0) {
            return -1;
        }

        if (c->current.type == Z_TOKEN_ASSIGN) {
            local_index = z_find_local(c, name);
            if (local_index < 0 ||
                c->locals[local_index].struct_index >= 0 ||
                c->locals[local_index].array_length != 0 ||
                z_next_token(c) != 0 ||
                z_parse_expr(c) != 0 ||
                z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset) != 0 ||
                z_expect(c, Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->current.type == Z_TOKEN_DOT) {
            local_index = z_find_local(c, name);
            if (local_index < 0 ||
                z_parse_local_field(c, local_index, &field_offset) != 0 ||
                z_expect(c, Z_TOKEN_ASSIGN) != 0 ||
                z_parse_expr(c) != 0 ||
                z_emit_store_rax_to_offset(c, c->locals[local_index].stack_offset + (int32_t)field_offset) != 0 ||
                z_expect(c, Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
        }

        if (c->current.type == Z_TOKEN_LBRACKET) {
            uint32_t remaining_dims = 0;
            local_index = z_find_local(c, name);
            if (local_index < 0 ||
                z_parse_indexed_address(c, local_index, &remaining_dims) != 0 ||
                remaining_dims != 0 ||
                z_emit_instr2_text(c, "mov", "rdx", "rax") != 0 ||
                z_expect(c, Z_TOKEN_ASSIGN) != 0 ||
                z_parse_expr(c) != 0 ||
                z_emit_store_rax_to_reg_ptr(c, "rdx") != 0 ||
                z_expect(c, Z_TOKEN_SEMI) != 0) {
                z_set_error(c, c->current.line);
                return -1;
            }

            return 0;
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
    uint32_t param_count = 0;
    uint32_t i;

    if (z_begin_function(c, name) != 0 ||
        z_expect(c, Z_TOKEN_LPAREN) != 0) {
        return -1;
    }

    if (c->current.type != Z_TOKEN_RPAREN) {
        for (;;) {
            if (param_count >= Z_MAX_PARAMS) {
                z_set_error(c, c->current.line);
                return -1;
            }

            if (z_parse_typed_name(c, param_names[param_count]) != 0) {
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
        int local_index = z_add_local(c, param_names[i], 0, 0, 0, -1, 8u);

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
    c.in_function = 0;
    c.current_function_label[0] = '\0';
    c.current_exit_label[0] = '\0';
    c.local_count = 0;
    c.next_stack_offset = 0;

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
           c.current.type == Z_TOKEN_IDENT &&
           z_streq(c.current.text, "struct")) {
        if (z_parse_struct_definition(&c) != 0) {
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

        if (z_parse_typed_name(&c, function_name) != 0 ||
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

    if (c.string_count != 0) {
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
