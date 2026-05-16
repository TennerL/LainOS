
#include "version.h"
#include "kernel.h"
#include "ahci.h"
#include "assembler.h"
#include "browser.h"
#include "desktop.h"
#include "editor.h"
#include "keyboard.h"
#include "kmem.h"
#include "lainfs.h"
#include "net.h"
#include "shell.h"
#include "storage.h"
#include "usb.h"
#include "zobject.h"
#include "zscript.h"
#include "ramdisk_seed.h"

#define MAX_DRIVES 26
#define SCRIPT_BUFFER_SIZE 65536u
#define SCRIPT_LINE_SIZE 128u
#define SCRIPT_MAX_DEPTH 4
#define EXEC_BUFFER_SIZE 524288u
#define EXEC_API_MAGIC 0x4C41494E45584543ull
#define ASM_SOURCE_SIZE 524288u
#define Z_INCLUDE_BUFFER_SIZE 65536u
#define ZMODULE_IMAGE_SIZE 131072u
#define SHELL_PATH_SIZE 128u
#define SHELL_MAX_SESSIONS 2u
#define Z_INCLUDE_MAX_DEPTH 4u
#define Z_INCLUDE_MAX_DIRS 4u
#define Z_INCLUDE_ONCE_MAX 16u
#define ZLINK_MAX_OBJECTS 16u
#define ZMODULE_MAX_MODULES 6u
#define ZMODULE_MAX_EXPORTS ZOBJECT_MAX_RESOLVED_SYMBOLS
#define ZMODULE_NAME_SIZE 32u
#define ZMODULE_TICK_HZ 20u

static int script_depth;
static char (*script_buffers)[SCRIPT_BUFFER_SIZE + 1];
static unsigned char *exec_buffer;
static unsigned char *asm_output;
static char *zscript_output;
static char (*zinclude_buffers)[Z_INCLUDE_BUFFER_SIZE + 1];
static char *shell_manifest_buffer;
static char *shell_source_buffer;
static char *shell_wget_buffer;
static char zbuild_report[512];
static char ztest_report[256];
static int shell_work_buffers_ready;

typedef struct {
    int loaded;
    char name[ZMODULE_NAME_SIZE];
    unsigned char *image;
    uint32_t image_size;
    uint32_t object_count;
    uint32_t export_count;
    zobject_resolved_symbol_t exports[ZMODULE_MAX_EXPORTS];
} zmodule_slot_t;

static zmodule_slot_t zmodule_slots[ZMODULE_MAX_MODULES];
static unsigned long long zmodule_last_tick;

typedef void (*command_handler_t)(const char *args, const boot_info_t *info);

typedef struct {
    uint64_t magic;
    uint64_t version;
    void (*puts)(const char *s);
    void (*put_hex64)(unsigned long long value);
    void (*put_dec64)(unsigned long long value);
    unsigned long long (*ticks)(void);
} exec_api_t;

typedef void (*exec_program_t)(const exec_api_t *api);
typedef uint64_t (*exec_program_ret_t)(const exec_api_t *api);
typedef void (*zmodule_tick_t)(void);
typedef void (*zmodule_unload_t)(void);
typedef void (*zmodule_void_hook_t)(void);

typedef struct {
    const char *name;
    const char *help;
    command_handler_t handler;
} command_t;

typedef struct {
    char drive_letter;
    uint32_t base_dir;
    uint32_t include_dirs[Z_INCLUDE_MAX_DIRS];
    uint32_t include_dir_count;
    uint32_t once_dirs[Z_INCLUDE_ONCE_MAX];
    char once_names[Z_INCLUDE_ONCE_MAX][32];
    uint32_t once_count;
} z_source_context_t;

typedef struct {
    int present;
    char label[12];
} drive_t;

typedef struct {
    int initialized;
    int drive;
    uint32_t dir_ids[MAX_DRIVES];
    char paths[MAX_DRIVES][SHELL_PATH_SIZE];
} shell_session_t;

static drive_t drives[MAX_DRIVES];
static shell_session_t shell_sessions[SHELL_MAX_SESSIONS];
static unsigned int active_session_index;

static shell_session_t *active_session(void) {
    return &shell_sessions[active_session_index];
}

#define current_drive (active_session()->drive)
#define cwd_dirs (active_session()->dir_ids)
#define cwd_paths (active_session()->paths)

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
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

static void zero_memory(void *ptr, uint32_t size) {
    unsigned char *p = (unsigned char *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static int shell_alloc_work_buffers(void) {
    if (shell_work_buffers_ready) {
        return 1;
    }

    if (script_buffers == 0) {
        script_buffers = (char (*)[SCRIPT_BUFFER_SIZE + 1])kmalloc(sizeof(*script_buffers) * SCRIPT_MAX_DEPTH);
    }
    if (exec_buffer == 0) {
        exec_buffer = (unsigned char *)kmalloc(EXEC_BUFFER_SIZE);
    }
    if (asm_output == 0) {
        asm_output = (unsigned char *)kmalloc(EXEC_BUFFER_SIZE);
    }
    if (zscript_output == 0) {
        zscript_output = (char *)kmalloc(ASM_SOURCE_SIZE + 1u);
    }
    if (zinclude_buffers == 0) {
        zinclude_buffers = (char (*)[Z_INCLUDE_BUFFER_SIZE + 1])kmalloc(sizeof(*zinclude_buffers) * Z_INCLUDE_MAX_DEPTH);
    }
    if (shell_manifest_buffer == 0) {
        shell_manifest_buffer = (char *)kmalloc(ASM_SOURCE_SIZE + 1u);
    }
    if (shell_source_buffer == 0) {
        shell_source_buffer = (char *)kmalloc(ASM_SOURCE_SIZE + 1u);
    }
    if (shell_wget_buffer == 0) {
        shell_wget_buffer = (char *)kmalloc(LAINFS_FILE_CAPACITY + 1u);
    }

    shell_work_buffers_ready = script_buffers != 0 &&
                               exec_buffer != 0 &&
                               asm_output != 0 &&
                               zscript_output != 0 &&
                               zinclude_buffers != 0 &&
                               shell_manifest_buffer != 0 &&
                               shell_source_buffer != 0 &&
                               shell_wget_buffer != 0;
    return shell_work_buffers_ready;
}

static int parse_color_arg(const char *args, unsigned int *color) {
    int base = 10;
    unsigned int value = 0;

    if (args == 0 || *args == '\0') {
        return -1;
    }

    if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
        base = 16;
        args += 2;
    }

    if (*args == '\0') {
        return -1;
    }

    while (*args) {
        char c = *args;
        unsigned int digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = 10u + (unsigned int)(c - 'a');
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = 10u + (unsigned int)(c - 'A');
        } else {
            return -1;
        }

        if (digit >= (unsigned int)base) {
            return -1;
        }

        value = value * (unsigned int)base + digit;
        ++args;
    }

    *color = value;
    return 0;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static char hex_digit(unsigned int value) {
    value &= 0xFu;
    if (value < 10u) {
        return (char)('0' + value);
    }
    return (char)('a' + (value - 10u));
}

static void make_zobject_prefix(const char *name, char *out, uint32_t out_capacity) {
    uint32_t hash = 0x811u;

    while (*name) {
        hash = ((hash << 5) ^ (hash >> 2) ^ (unsigned char)*name) & 0xFFFu;
        ++name;
    }

    if (out_capacity < 6u) {
        if (out_capacity != 0) {
            out[0] = '\0';
        }
        return;
    }

    out[0] = 'o';
    out[1] = hex_digit(hash >> 8);
    out[2] = hex_digit(hash >> 4);
    out[3] = hex_digit(hash);
    out[4] = '_';
    out[5] = '\0';
}

static int parse_drive_spec(const char *s) {
    s = skip_const_spaces(s);

    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        return -1;
    }

    char drive_letter = to_upper(s[0]);

    if (s[1] != ':') {
        return -1;
    }

    s = skip_const_spaces(s + 2);
    if (*s != '\0') {
        return -1;
    }

    return drive_letter - 'A';
}

static int parse_drive_arg(const char *s) {
    s = skip_const_spaces(s);

    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        return -1;
    }

    char drive_letter = to_upper(s[0]);
    ++s;

    if (*s == ':') {
        ++s;
    }

    s = skip_const_spaces(s);
    if (*s != '\0') {
        return -1;
    }

    return drive_letter - 'A';
}

static void copy_label(char *dst, const char *src) {
    unsigned int i = 0;

    while (src[i] && i < 11) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static void copy_text_limited(char *dst, uint32_t dst_size, const char *src) {
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

static int make_suffixed_name(const char *base, const char *suffix, char *out, uint32_t out_size) {
    uint32_t i = 0;
    uint32_t j = 0;

    if (base == 0 || suffix == 0 || out == 0 || out_size == 0 || *base == '\0') {
        return -1;
    }

    while (base[i]) {
        if (i + 1u >= out_size) {
            out[0] = '\0';
            return -1;
        }
        out[i] = base[i];
        ++i;
    }

    while (suffix[j]) {
        if (i + 1u >= out_size) {
            out[0] = '\0';
            return -1;
        }
        out[i++] = suffix[j++];
    }

    out[i] = '\0';
    return 0;
}

static int make_object_name_from_source(const char *source_name, char *out, uint32_t out_size) {
    uint32_t i = 0;
    uint32_t dot = 0xFFFFFFFFu;

    if (source_name == 0 || out == 0 || out_size == 0 || *source_name == '\0') {
        return -1;
    }

    while (source_name[i]) {
        if (source_name[i] == '.') {
            dot = i;
        }
        ++i;
    }

    if (dot != 0xFFFFFFFFu) {
        i = dot;
    }

    if (i == 0 || i + 3u >= out_size) {
        out[0] = '\0';
        return -1;
    }

    for (uint32_t j = 0; j < i; ++j) {
        out[j] = source_name[j];
    }
    out[i++] = '.';
    out[i++] = 'z';
    out[i++] = 'o';
    out[i] = '\0';
    return 0;
}

static void copy_bytes(unsigned char *dst, const unsigned char *src, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        dst[i] = src[i];
    }
}

static int append_text_limited(char *out, uint32_t out_size, uint32_t *pos, const char *text) {
    if (out == 0 || pos == 0 || text == 0 || out_size == 0) {
        return -1;
    }

    while (*text) {
        if (*pos + 1u >= out_size) {
            out[out_size - 1u] = '\0';
            return -1;
        }
        out[*pos] = *text++;
        ++(*pos);
    }
    out[*pos] = '\0';
    return 0;
}

static int append_dec_limited(char *out, uint32_t out_size, uint32_t *pos, uint64_t value) {
    char digits[21];
    uint32_t count = 0;

    if (value == 0) {
        return append_text_limited(out, out_size, pos, "0");
    }

    while (value != 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (count > 0) {
        char text[2];
        text[0] = digits[--count];
        text[1] = '\0';
        if (append_text_limited(out, out_size, pos, text) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_u64_arg(const char *s, uint64_t *out) {
    uint64_t value = 0;

    if (s == 0 || out == 0) {
        return -1;
    }

    s = skip_const_spaces(s);
    if (*s == '\0') {
        return -1;
    }

    while (*s) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        value = value * 10u + (uint64_t)(*s - '0');
        ++s;
    }

    *out = value;
    return 0;
}

static int starts_with_text(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) {
            return 0;
        }
        ++s;
        ++prefix;
    }

    return 1;
}

static int contains_text(const char *s, const char *needle) {
    if (s == 0 || needle == 0 || *needle == '\0') {
        return 0;
    }

    while (*s) {
        const char *a = s;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return 1;
        }
        ++s;
    }

    return 0;
}

static int path_is_separator(char ch) {
    return ch == '/' || ch == '\\';
}

static int copy_path_part_limited(char *dst, uint32_t dst_size, const char *start, uint32_t len) {
    if (dst == 0 || dst_size == 0 || len == 0 || len >= dst_size) {
        return -1;
    }

    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = start[i];
    }
    dst[len] = '\0';
    return 0;
}

static int resolve_file_path(char drive_letter,
                             uint32_t base_dir,
                             const char *path,
                             uint32_t *out_parent,
                             char *out_name,
                             uint32_t out_name_size) {
    uint32_t dir = base_dir;
    const char *s = path;
    char part[32];

    if (path == 0 || out_parent == 0 || out_name == 0 || out_name_size == 0) {
        return -1;
    }

    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    if (*s == '\0') {
        return -1;
    }

    if (path_is_separator(*s)) {
        dir = LAINFS_ROOT_DIR;
        while (path_is_separator(*s)) {
            ++s;
        }
    }

    for (;;) {
        const char *start = s;
        uint32_t len = 0;
        int is_last = 0;

        while (*s && !path_is_separator(*s)) {
            ++s;
            ++len;
        }
        while (path_is_separator(*s)) {
            ++s;
        }
        is_last = *s == '\0';

        if (copy_path_part_limited(part, sizeof(part), start, len) != 0) {
            return -1;
        }

        if (is_last) {
            copy_text_limited(out_name, out_name_size, part);
            *out_parent = dir;
            return 0;
        }

        if (streq(part, ".")) {
            continue;
        }
        if (streq(part, "..")) {
            if (lainfs_parent_dir(drive_letter, dir, &dir) != 0) {
                return -5;
            }
            continue;
        }
        if (lainfs_find_dir(drive_letter, dir, part, &dir) != 0) {
            return -5;
        }
    }
}

static int resolve_dir_path(char drive_letter,
                            uint32_t base_dir,
                            const char *path,
                            uint32_t *out_dir) {
    uint32_t dir = base_dir;
    const char *s = path;
    char part[32];

    if (path == 0 || out_dir == 0) {
        return -1;
    }

    s = skip_const_spaces(s);
    if (*s == '\0' || streq(s, ".")) {
        *out_dir = base_dir;
        return 0;
    }

    if (path_is_separator(*s)) {
        dir = LAINFS_ROOT_DIR;
        while (path_is_separator(*s)) {
            ++s;
        }
    }

    if (*s == '\0') {
        *out_dir = dir;
        return 0;
    }

    for (;;) {
        const char *start = s;
        uint32_t len = 0;

        while (*s && !path_is_separator(*s)) {
            ++s;
            ++len;
        }
        while (path_is_separator(*s)) {
            ++s;
        }

        if (copy_path_part_limited(part, sizeof(part), start, len) != 0) {
            return -1;
        }

        if (streq(part, ".")) {
        } else if (streq(part, "..")) {
            if (lainfs_parent_dir(drive_letter, dir, &dir) != 0) {
                return -5;
            }
        } else if (lainfs_find_dir(drive_letter, dir, part, &dir) != 0) {
            return -5;
        }

        if (*s == '\0') {
            *out_dir = dir;
            return 0;
        }
    }
}

static int path_drive_prefix(const char **path, int fallback_drive) {
    const char *s;
    int drive;

    if (path == 0 || *path == 0) {
        return -1;
    }

    s = skip_const_spaces(*path);
    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':') {
        drive = to_upper(s[0]) - 'A';
        s += 2;
        *path = s;
    } else {
        drive = fallback_drive;
        *path = s;
    }

    if (drive < 0 || drive >= MAX_DRIVES || !drives[drive].present) {
        return -1;
    }

    return drive;
}

static int resolve_file_path_with_drive(const char *path,
                                        int fallback_drive,
                                        int *out_drive,
                                        uint32_t *out_parent,
                                        char *out_name,
                                        uint32_t out_name_size) {
    int drive = path_drive_prefix(&path, fallback_drive);

    if (drive < 0 || out_drive == 0) {
        return -1;
    }

    if (resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          out_parent,
                          out_name,
                          out_name_size) != 0) {
        return -1;
    }

    *out_drive = drive;
    return 0;
}

static int append_source_bytes(char *out,
                               uint32_t out_capacity,
                               uint32_t *out_size,
                               const char *text,
                               uint32_t size) {
    if (out == 0 || out_size == 0 || text == 0 ||
        size > out_capacity ||
        *out_size > out_capacity - size) {
        return -1;
    }

    for (uint32_t i = 0; i < size; ++i) {
        out[*out_size + i] = text[i];
    }
    *out_size += size;
    if (*out_size < out_capacity) {
        out[*out_size] = '\0';
    }
    return 0;
}

static int parse_z_include_line(char *line, char **include_name) {
    char *s = skip_spaces(line);
    char *start;
    char *end;

    if (*s == '#') {
        ++s;
        s = skip_spaces(s);
    }

    if (!starts_with_text(s, "include")) {
        return 0;
    }
    s += 7;
    if (*s != ' ' && *s != '\t') {
        return 0;
    }
    s = skip_spaces(s);
    if (*s != '"') {
        return -1;
    }

    start = ++s;
    while (*s && *s != '"') {
        ++s;
    }
    if (*s != '"') {
        return -1;
    }
    end = s;
    *end = '\0';
    s = skip_spaces(end + 1);
    if (*start == '\0' || (*s != '\0' && *s != '\n' && *s != '\r')) {
        return -1;
    }

    *include_name = start;
    return 1;
}

static int parse_z_pragma_once_line(char *line) {
    char *s = skip_spaces(line);

    if (*s != '#') {
        return 0;
    }
    ++s;
    s = skip_spaces(s);
    if (!starts_with_text(s, "pragma")) {
        return 0;
    }
    s += 6;
    if (*s != ' ' && *s != '\t') {
        return 0;
    }
    s = skip_spaces(s);
    if (!starts_with_text(s, "once")) {
        return 0;
    }
    s += 4;
    s = skip_spaces(s);
    return *s == '\0' || *s == '\n' || *s == '\r';
}

static int z_source_once_index(const z_source_context_t *ctx, uint32_t parent_id, const char *name) {
    for (uint32_t i = 0; i < ctx->once_count; ++i) {
        if (ctx->once_dirs[i] == parent_id && streq(ctx->once_names[i], name)) {
            return (int)i;
        }
    }

    return -1;
}

static int z_source_mark_once(z_source_context_t *ctx, uint32_t parent_id, const char *name) {
    if (z_source_once_index(ctx, parent_id, name) >= 0) {
        return 0;
    }
    if (ctx->once_count >= Z_INCLUDE_ONCE_MAX) {
        return -1;
    }

    ctx->once_dirs[ctx->once_count] = parent_id;
    copy_text_limited(ctx->once_names[ctx->once_count],
                      sizeof(ctx->once_names[ctx->once_count]),
                      name);
    ++ctx->once_count;
    return 0;
}

static int load_z_source_file(z_source_context_t *ctx,
                              uint32_t parent_id,
                              const char *path,
                              char *buffer,
                              uint32_t buffer_capacity,
                              uint32_t *size_out,
                              uint32_t *resolved_parent,
                              char *resolved_name,
                              uint32_t resolved_name_size) {
    int status;

    status = resolve_file_path(ctx->drive_letter,
                               parent_id,
                               path,
                               resolved_parent,
                               resolved_name,
                               resolved_name_size);
    if (status != 0) {
        return status;
    }

    if (z_source_once_index(ctx, *resolved_parent, resolved_name) >= 0) {
        *size_out = 0;
        buffer[0] = '\0';
        return 1;
    }

    return lainfs_load_file_in_dir(ctx->drive_letter,
                                   *resolved_parent,
                                   resolved_name,
                                   buffer,
                                   buffer_capacity,
                                   size_out);
}

static int load_z_include_file(z_source_context_t *ctx,
                               uint32_t current_parent_id,
                               const char *include_name,
                               char *buffer,
                               uint32_t buffer_capacity,
                               uint32_t *size_out,
                               uint32_t *resolved_parent,
                               char *resolved_name,
                               uint32_t resolved_name_size) {
    int status = load_z_source_file(ctx,
                                    current_parent_id,
                                    include_name,
                                    buffer,
                                    buffer_capacity,
                                    size_out,
                                    resolved_parent,
                                    resolved_name,
                                    resolved_name_size);
    if (status != -5) {
        return status;
    }

    for (uint32_t i = 0; i < ctx->include_dir_count; ++i) {
        status = load_z_source_file(ctx,
                                    ctx->include_dirs[i],
                                    include_name,
                                    buffer,
                                    buffer_capacity,
                                    size_out,
                                    resolved_parent,
                                    resolved_name,
                                    resolved_name_size);
        if (status != -5) {
            return status;
        }
    }

    return -5;
}

static int load_z_source_expanded_recursive(z_source_context_t *ctx,
                                            uint32_t parent_id,
                                            const char *name,
                                            char *out,
                                            uint32_t out_capacity,
                                            uint32_t *out_size,
                                            uint32_t depth) {
    char *buffer;
    uint32_t size = 0;
    uint32_t resolved_parent = parent_id;
    char resolved_name[32];
    int status;

    if (depth >= Z_INCLUDE_MAX_DEPTH) {
        return -30;
    }

    buffer = zinclude_buffers[depth];
    if (depth == 0) {
        status = load_z_source_file(ctx,
                                    parent_id,
                                    name,
                                    buffer,
                                    Z_INCLUDE_BUFFER_SIZE,
                                    &size,
                                    &resolved_parent,
                                    resolved_name,
                                    sizeof(resolved_name));
    } else {
        status = load_z_include_file(ctx,
                                     parent_id,
                                     name,
                                     buffer,
                                     Z_INCLUDE_BUFFER_SIZE,
                                     &size,
                                     &resolved_parent,
                                     resolved_name,
                                     sizeof(resolved_name));
    }
    if (status == 1) {
        return 0;
    }
    if (status != 0) {
        return status;
    }
    buffer[size] = '\0';

    for (uint32_t pos = 0; pos < size;) {
        char *line_start = buffer + pos;
        char *include_name = 0;
        uint32_t line_len;
        int include_status;

        while (pos < size && buffer[pos] != '\n' && buffer[pos] != '\r') {
            ++pos;
        }
        line_len = (uint32_t)((buffer + pos) - line_start);
        if (pos < size) {
            buffer[pos++] = '\0';
            if (pos < size && buffer[pos - 1u] == '\r' && buffer[pos] == '\n') {
                ++pos;
            }
        }

        include_status = parse_z_include_line(line_start, &include_name);
        if (include_status < 0) {
            return -31;
        }
        if (include_status > 0) {
            status = load_z_source_expanded_recursive(ctx,
                                                      resolved_parent,
                                                      include_name,
                                                      out,
                                                      out_capacity,
                                                      out_size,
                                                      depth + 1u);
            if (status != 0) {
                return status;
            }
            if (append_source_bytes(out, out_capacity, out_size, "\n", 1u) != 0) {
                return -32;
            }
        } else if (parse_z_pragma_once_line(line_start)) {
            if (z_source_mark_once(ctx, resolved_parent, resolved_name) != 0) {
                return -33;
            }
        } else {
            if (append_source_bytes(out, out_capacity, out_size, line_start, line_len) != 0 ||
                append_source_bytes(out, out_capacity, out_size, "\n", 1u) != 0) {
                return -32;
            }
        }
    }

    return 0;
}

static int load_z_source_expanded(char drive_letter,
                                  uint32_t parent_id,
                                  const uint32_t *include_dirs,
                                  uint32_t include_dir_count,
                                  const char *name,
                                  char *out,
                                  uint32_t out_capacity,
                                  uint32_t *out_size) {
    z_source_context_t ctx;

    if (out == 0 || out_size == 0 || out_capacity == 0) {
        return -1;
    }

    ctx.drive_letter = drive_letter;
    ctx.base_dir = parent_id;
    ctx.include_dir_count = include_dir_count > Z_INCLUDE_MAX_DIRS ? Z_INCLUDE_MAX_DIRS : include_dir_count;
    ctx.once_count = 0;
    for (uint32_t i = 0; i < Z_INCLUDE_MAX_DIRS; ++i) {
        ctx.include_dirs[i] = (i < ctx.include_dir_count && include_dirs != 0) ? include_dirs[i] : 0;
    }

    *out_size = 0;
    out[0] = '\0';
    return load_z_source_expanded_recursive(&ctx,
                                            parent_id,
                                            name,
                                            out,
                                            out_capacity,
                                            out_size,
                                            0);
}

static void print_drive_name(int index) {
    char name[3];

    name[0] = (char)('A' + index);
    name[1] = ':';
    name[2] = '\0';
    console_puts(name);
}

static int active_drive(void);

static void reset_cwd_in_session(shell_session_t *session, int drive) {
    if (drive < 0 || drive >= MAX_DRIVES) {
        return;
    }

    session->dir_ids[drive] = LAINFS_ROOT_DIR;
    session->paths[drive][0] = '\\';
    session->paths[drive][1] = '\0';
}

static void init_session_blank(shell_session_t *session) {
    session->initialized = 1;
    session->drive = -1;

    for (int i = 0; i < MAX_DRIVES; ++i) {
        reset_cwd_in_session(session, i);
    }
}

static void ensure_session_initialized(unsigned int index) {
    if (index >= SHELL_MAX_SESSIONS || shell_sessions[index].initialized) {
        return;
    }

    if (shell_sessions[active_session_index].initialized) {
        shell_sessions[index] = shell_sessions[active_session_index];
        shell_sessions[index].initialized = 1;
    } else {
        init_session_blank(&shell_sessions[index]);
    }
}

static void reset_cwd(int drive) {
    reset_cwd_in_session(active_session(), drive);
}

static uint32_t active_dir(void) {
    int drive = active_drive();
    if (drive < 0) {
        return LAINFS_ROOT_DIR;
    }

    return cwd_dirs[drive];
}

static void append_path_part(int drive, const char *name) {
    unsigned int len = 0;
    unsigned int i = 0;

    if (drive < 0 || drive >= MAX_DRIVES) {
        return;
    }

    while (cwd_paths[drive][len]) {
        ++len;
    }

    if (len > 1 && len + 1 < SHELL_PATH_SIZE) {
        cwd_paths[drive][len++] = '\\';
        cwd_paths[drive][len] = '\0';
    }

    while (name[i] && len + 1 < SHELL_PATH_SIZE) {
        cwd_paths[drive][len++] = name[i++];
    }

    cwd_paths[drive][len] = '\0';
}

static void pop_path_part(int drive) {
    unsigned int len = 0;

    if (drive < 0 || drive >= MAX_DRIVES) {
        return;
    }

    while (cwd_paths[drive][len]) {
        ++len;
    }

    if (len <= 1) {
        cwd_paths[drive][0] = '\\';
        cwd_paths[drive][1] = '\0';
        return;
    }

    while (len > 1 && cwd_paths[drive][len - 1] != '\\') {
        --len;
    }

    if (len <= 1) {
        cwd_paths[drive][1] = '\0';
    } else {
        cwd_paths[drive][len - 1] = '\0';
    }
}

static int resolve_dir_arg(int drive, const char *arg, uint32_t *out_dir) {
    arg = skip_const_spaces(arg);

    if (*arg == '\0' || streq(arg, ".")) {
        *out_dir = cwd_dirs[drive];
        return 0;
    }

    if (streq(arg, "\\") || streq(arg, "/")) {
        *out_dir = LAINFS_ROOT_DIR;
        return 0;
    }

    if (streq(arg, "..")) {
        return lainfs_parent_dir((char)('A' + drive), cwd_dirs[drive], out_dir);
    }

    return lainfs_find_dir((char)('A' + drive), cwd_dirs[drive], arg, out_dir);
}

static int is_script_comment_or_blank(const char *line) {
    line = skip_const_spaces(line);
    return *line == '\0' || *line == '#';
}

static void run_script_text(char *script, uint32_t size, const boot_info_t *info) {
    char line[SCRIPT_LINE_SIZE];
    uint32_t line_len = 0;

    for (uint32_t i = 0; i <= size; ++i) {
        char ch = i < size ? script[i] : '\n';

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            line[line_len] = '\0';

            if (!is_script_comment_or_blank(line)) {
                console_puts("> ");
                console_puts(line);
                console_puts("\n");
                shell_run_command(line, info);
            }

            line_len = 0;
            continue;
        }

        if (line_len + 1 < SCRIPT_LINE_SIZE) {
            line[line_len++] = ch;
        }
    }
}

static void cmd_help(const char *args, const boot_info_t *info);
static void cmd_bgcolor(const char *args, const boot_info_t *info);
static void cmd_fgcolor(const char *args, const boot_info_t *info);
static void cmd_resolution(const char *args, const boot_info_t *info);
static void cmd_clear(const char *args, const boot_info_t *info);
static void cmd_echo(const char *args, const boot_info_t *info);
static void cmd_info(const char *args, const boot_info_t *info);
static void cmd_heap(const char *args, const boot_info_t *info);
static void cmd_cpus(const char *args, const boot_info_t *info);
static void cmd_smp(const char *args, const boot_info_t *info);
static void cmd_gfx(const char *args, const boot_info_t *info);
static void cmd_reboot(const char *args, const boot_info_t *info);
static void cmd_poweroff(const char *args, const boot_info_t *info);
static void cmd_mkdrive(const char *args, const boot_info_t *info);
static void cmd_drives(const char *args, const boot_info_t *info);
static void cmd_blk(const char *args, const boot_info_t *info);
static void cmd_part(const char *args, const boot_info_t *info);
static void cmd_mount(const char *args, const boot_info_t *info);
static void cmd_mounts(const char *args, const boot_info_t *info);
static void cmd_format(const char *args, const boot_info_t *info);
static void cmd_ls(const char *args, const boot_info_t *info);
static void cmd_cd(const char *args, const boot_info_t *info);
static void cmd_pwd(const char *args, const boot_info_t *info);
static void cmd_mkdir(const char *args, const boot_info_t *info);
static void cmd_rm(const char *args, const boot_info_t *info);
static void cmd_rename(const char *args, const boot_info_t *info);
static void cmd_cp(const char *args, const boot_info_t *info);
static void cmd_write(const char *args, const boot_info_t *info);
static void cmd_cat(const char *args, const boot_info_t *info);
static void cmd_edit(const char *args, const boot_info_t *info);
static void cmd_browse(const char *args, const boot_info_t *info);
static void cmd_desktop(const char *args, const boot_info_t *info);
static void cmd_ahci(const char *args, const boot_info_t *info);
static void cmd_net(const char *args, const boot_info_t *info);
static void cmd_wget(const char *args, const boot_info_t *info);
static void cmd_mouse(const char *args, const boot_info_t *info);
static void cmd_usb(const char *args, const boot_info_t *info);
static void cmd_ticks(const char *args, const boot_info_t *info);
static void cmd_run(const char *args, const boot_info_t *info);
static void cmd_exec(const char *args, const boot_info_t *info);
static void cmd_asm(const char *args, const boot_info_t *info);
static void cmd_zc(const char *args, const boot_info_t *info);
static void cmd_zco(const char *args, const boot_info_t *info);
static void cmd_zlink(const char *args, const boot_info_t *info);
static void cmd_zbuild(const char *args, const boot_info_t *info);
static void cmd_zclean(const char *args, const boot_info_t *info);
static void cmd_ztest(const char *args, const boot_info_t *info);
static void cmd_zinstall(const char *args, const boot_info_t *info);
static int zbuild_read_layout(int drive,
                              const char *target_name,
                              char *manifest,
                              uint32_t manifest_capacity,
                              uint32_t *build_dir,
                              char *output_name,
                              uint32_t output_name_size,
                              uint32_t *install_dir,
                              char *install_name,
                              uint32_t install_name_size,
                              uint64_t *expected_return,
                              int *has_expected_return,
                              int *objects_only,
                              uint32_t *object_count);
static void cmd_zmod(const char *args, const boot_info_t *info);
static void cmd_zunload(const char *args, const boot_info_t *info);
static void cmd_zreload(const char *args, const boot_info_t *info);
static void cmd_zmods(const char *args, const boot_info_t *info);
static int zmodule_find_slot_by_name(const char *name);
static void cmd_zrun(const char *args, const boot_info_t *info);
static void cmd_zasm(const char *args, const boot_info_t *info);
static void cmd_keymap(const char *args, const boot_info_t *info);
static void split_first_arg(char *s, char **first, char **rest);

static const command_t commands[] = {
    { "help",    "show commands",             cmd_help },
    { "bgcolor", "set background color",      cmd_bgcolor },
    { "fgcolor", "set text color",            cmd_fgcolor },
    { "resolution", "set next-boot resolution", cmd_resolution },
    { "clear",   "clear screen",              cmd_clear },
    { "echo",    "print text",                cmd_echo },
    { "info",    "show kernel info",          cmd_info },
    { "heap",    "show heap diagnostics",     cmd_heap },
    { "cpus",    "show CPU topology",         cmd_cpus },
    { "smp",     "run a multicore work test", cmd_smp },
    { "gfx",     "show graphics SMP stats",   cmd_gfx },
    { "reboot",  "restart the machine",       cmd_reboot },
    { "poweroff", "power off the machine",     cmd_poweroff },
    { "shutdown", "power off the machine",     cmd_poweroff },
    { "mkdrive", "create a virtual drive",    cmd_mkdrive },
    { "drives",  "list virtual drives",       cmd_drives },
    { "blk",     "list block devices",        cmd_blk },
    { "part",    "list partitions",           cmd_part },
    { "mount",   "mount partition to drive",  cmd_mount },
    { "mounts",  "list mounted filesystems",  cmd_mounts },
    { "format",  "format drive, partition, or disk as lainfs", cmd_format },
    { "ls",      "list directory entries",    cmd_ls },
    { "cd",      "change directory",          cmd_cd },
    { "pwd",     "show current directory",    cmd_pwd },
    { "mkdir",   "create a directory",         cmd_mkdir },
    { "rm",      "delete a file or directory", cmd_rm },
    { "del",     "delete a file or directory", cmd_rm },
    { "rename",  "rename a file or directory", cmd_rename },
    { "mv",      "rename a file or directory", cmd_rename },
    { "cp",      "copy a file",                cmd_cp },
    { "write",   "write a text file",         cmd_write },
    { "cat",     "print a text file",         cmd_cat },
    { "edit",    "edit a text file",          cmd_edit },
    { "browse",  "browse and move lainfs entries", cmd_browse },
    { "desktop", "enter framebuffer desktop", cmd_desktop },
    { "keymap",  "set keyboard layout",       cmd_keymap },
    { "ahci",    "show AHCI status",          cmd_ahci },
    { "net",     "show network devices",      cmd_net },
    { "wget",    "fetch http://IP/path to a file", cmd_wget },
    { "mouse",   "show mouse diagnostics",    cmd_mouse },
    { "usb",     "show USB controllers",      cmd_usb },
    { "ticks",   "show timer ticks",          cmd_ticks },
    { "run",     "run a script file",         cmd_run },
    { "exec",    "run a flat binary file",     cmd_exec },
    { "asm",     "assemble a tiny asm file",   cmd_asm },
    { "zc",      "compile a tiny .Z file",     cmd_zc },
    { "zco",     "compile .Z to a .zo object", cmd_zco },
    { "zlink",   "link a .zo object",          cmd_zlink },
    { "zbuild",  "build many .Z files",         cmd_zbuild },
    { "zclean",  "remove zbuild artifacts",     cmd_zclean },
    { "ztest",   "build and run a zbuild target", cmd_ztest },
    { "zinstall", "build and install a zbuild target", cmd_zinstall },
    { "zmod",    "load and run .zo module(s)",  cmd_zmod },
    { "zunload", "unload a resident .zo module", cmd_zunload },
    { "zreload", "unload then load a .zo module", cmd_zreload },
    { "zmods",   "list loaded .zo modules",     cmd_zmods },
    { "zrun",    "compile and run a .Z file",  cmd_zrun },
    { "zasm",    "dump generated asm for a .Z file", cmd_zasm },
};

static const unsigned int command_count = sizeof(commands) / sizeof(commands[0]);

static void cmd_help(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    for (unsigned int i = 0; i < command_count; ++i) {
        console_puts(commands[i].name);
        console_puts(" - ");
        console_puts(commands[i].help);
        console_puts("\n");
    }
}

static void cmd_clear(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;
    console_clear();
}

static void cmd_resolution(const char *args, const boot_info_t *info) {
    (void)info;

    char *width_text = 0;
    char *height_text = 0;
    char *extra = 0;
    char config[32];
    uint32_t pos = 0;
    uint32_t size = 0;
    uint64_t width = 0;
    uint64_t height = 0;
    int drive = active_drive();
    int status;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg((char *)args, &width_text, &height_text);
    split_first_arg(height_text, &height_text, &extra);
    if (*width_text == '\0') {
        console_puts("current resolution: ");
        console_put_dec64(graphics_width());
        console_puts("x");
        console_put_dec64(graphics_height());
        console_puts(" format=");
        console_put_dec64(graphics_format());
        console_puts("\n");

        status = lainfs_load_file_in_dir((char)('A' + drive),
                                         LAINFS_ROOT_DIR,
                                         "bootres.cfg",
                                         config,
                                         sizeof(config) - 1u,
                                         &size);
        if (status == 0) {
            config[size] = '\0';
            console_puts("next boot request: ");
            console_puts(config);
            if (size == 0 || config[size - 1u] != '\n') {
                console_puts("\n");
            }
        } else {
            console_puts("next boot request: firmware default\n");
        }
        return;
    }

    if (*height_text == '\0' || *extra != '\0' ||
        parse_u64_arg(width_text, &width) != 0 ||
        parse_u64_arg(height_text, &height) != 0 ||
        width == 0 || height == 0 ||
        width > 16384u || height > 16384u) {
        console_puts("usage: resolution width height\n");
        return;
    }

    config[0] = '\0';
    if (append_dec_limited(config, sizeof(config), &pos, width) != 0 ||
        append_text_limited(config, sizeof(config), &pos, " ") != 0 ||
        append_dec_limited(config, sizeof(config), &pos, height) != 0 ||
        append_text_limited(config, sizeof(config), &pos, "\n") != 0) {
        console_puts("resolution failed: config is too large\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     LAINFS_ROOT_DIR,
                                     "bootres.cfg",
                                     config,
                                     pos);
    if (status != 0) {
        console_puts("resolution failed: could not save bootres.cfg\n");
        return;
    }

    console_puts("next boot resolution set to ");
    console_put_dec64(width);
    console_puts("x");
    console_put_dec64(height);
    console_puts("\nreboot to apply it\n");
}

static void cmd_echo(const char *args, const boot_info_t *info) {
    (void)info;
    console_puts(args);
    console_puts("\n");
}

static void cmd_info(const char *args, const boot_info_t *info) {
    (void)args;

    console_puts("Kernel version: ");
    console_puts(BUILD_VERSION_STRING);
    console_puts("\n");
    console_kprintf2("Kernel base: 0x%x, framebuffer: 0x%x\n",
                     info->kernel_base,
                     info->framebuffer_base);
    console_kprintf2("Resolution: %u x %u\n",
                     info->framebuffer_width,
                     info->framebuffer_height);
    console_kprintf1("Memory map bytes: %u\n", info->memory_map_size);
    console_kprintf1("RSDP: 0x%x\n", info->rsdp);
    console_kprintf2("CPU cores online/detected: %u/%u\n",
                     cpu_online_core_count(),
                     cpu_core_count());
}

static void cmd_heap(const char *args, const boot_info_t *info) {
    kmem_stats_t stats;

    (void)args;
    (void)info;

    kmem_get_stats(&stats);
    console_puts("heap used bytes: ");
    console_put_dec64(stats.heap_used_bytes);
    console_puts("\nfree pages: ");
    console_put_dec64(stats.free_pages);
    console_puts("/");
    console_put_dec64(stats.total_pages);
    console_puts(" (");
    console_put_dec64(stats.free_pages * KMEM_PAGE_SIZE);
    console_puts(" bytes)\nfree ranges: ");
    console_put_dec64(stats.free_ranges);
    console_puts(" largest=");
    console_put_dec64(stats.largest_free_range_pages);
    console_puts(" pages\nsmall free blocks: ");
    console_put_dec64(stats.small_free_blocks);
    console_puts("\nalloc/free/live/peak: ");
    console_put_dec64(stats.allocation_count);
    console_puts("/");
    console_put_dec64(stats.free_count);
    console_puts("/");
    console_put_dec64(stats.live_allocations);
    console_puts("/");
    console_put_dec64(stats.peak_live_allocations);
    console_puts("\nheap faults invalid/double/guard: ");
    console_put_dec64(stats.invalid_frees);
    console_puts("/");
    console_put_dec64(stats.double_frees);
    console_puts("/");
    console_put_dec64(stats.guard_failures);
    console_puts("\nshell work buffers: ");
    console_puts(shell_work_buffers_ready ? "heap\n" : "not allocated\n");
}

static void cmd_cpus(const char *args, const boot_info_t *info) {
    unsigned int count = cpu_core_count();

    (void)args;
    (void)info;

    console_puts("CPU topology from ACPI MADT\n");
    console_puts("local APIC base: 0x");
    console_put_hex64(cpu_lapic_base());
    console_puts("\nonline: ");
    console_put_dec64(cpu_online_core_count());
    console_puts("\ncores: ");
    console_put_dec64(count);
    console_puts("\n");

    for (unsigned int i = 0; i < count; ++i) {
        console_puts("  cpu ");
        console_put_dec64(i);
        console_puts(": local APIC id ");
        console_put_dec64(cpu_lapic_id(i));
        if (i == 0) {
            console_puts(" (bootstrap)");
        }
        console_puts("\n");
    }
}

typedef struct {
    volatile uint64_t value;
    uint64_t iterations;
} smp_test_job_t;

static void smp_test_worker(void *arg) {
    smp_test_job_t *job = (smp_test_job_t *)arg;
    uint64_t value = 0;

    for (uint64_t i = 0; i < job->iterations; ++i) {
        value += (i ^ (i >> 3)) + 1u;
    }

    job->value = value;
}

static void cmd_smp(const char *args, const boot_info_t *info) {
    enum { SMP_TEST_MAX_JOBS = 8 };
    smp_test_job_t jobs[SMP_TEST_MAX_JOBS];
    unsigned int ids[SMP_TEST_MAX_JOBS];
    unsigned int online = cpu_online_core_count();
    unsigned int job_count;
    unsigned long long start;
    unsigned long long end;
    uint64_t checksum = 0;

    (void)args;
    (void)info;

    console_kprintf2("SMP online/detected: %u/%u\n", online, cpu_core_count());
    if (online <= 1u) {
        console_puts("no secondary CPUs are online\n");
        return;
    }

    job_count = online - 1u;
    if (job_count > SMP_TEST_MAX_JOBS) {
        job_count = SMP_TEST_MAX_JOBS;
    }

    start = timer_ticks();
    for (unsigned int i = 0; i < job_count; ++i) {
        jobs[i].value = 0;
        jobs[i].iterations = 12000000u + (uint64_t)i * 2000000u;
        ids[i] = smp_submit_work(smp_test_worker, &jobs[i]);
        if (ids[i] == 0u) {
            console_puts("smp queue full while submitting job\n");
            job_count = i;
            break;
        }
    }

    for (unsigned int i = 0; i < job_count; ++i) {
        smp_wait_work(ids[i]);
        checksum ^= jobs[i].value + (uint64_t)ids[i];
    }
    end = timer_ticks();

    console_puts("jobs completed: ");
    console_put_dec64(job_count);
    console_puts("\npending queue items: ");
    console_put_dec64(smp_pending_work_count());
    console_puts("\nelapsed ticks: ");
    console_put_dec64(end - start);
    console_puts("\nchecksum: 0x");
    console_put_hex64(checksum);
    console_puts("\n");
}

static void cmd_gfx(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    console_puts("Graphics\n");
    console_puts("resolution: ");
    console_put_dec64(graphics_width());
    console_puts(" x ");
    console_put_dec64(graphics_height());
    console_puts("\npitch: ");
    console_put_dec64(graphics_pitch());
    console_puts("\nformat: ");
    console_put_dec64(graphics_format());
    console_puts("\nbackbuffer: ");
    console_puts(graphics_backbuffer_active() ? "active" : "inactive");
    console_puts("\nSMP last workers: ");
    console_put_dec64(graphics_smp_last_workers());
    console_puts("\nSMP operations: ");
    console_put_dec64(graphics_smp_ops());
    console_puts("\nSMP jobs submitted: ");
    console_put_dec64(graphics_smp_jobs());
    console_puts("\nSMP pixels: ");
    console_put_dec64(graphics_smp_pixels());
    console_puts("\n");
}

static void cmd_reboot(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    console_puts("rebooting...\n");
    power_reboot();
}

static void cmd_poweroff(const char *args, const boot_info_t *info) {
    (void)args;

    console_puts("powering off...\n");
    if (power_poweroff(info) != 0) {
        console_puts("poweroff: ACPI S5 shutdown is not available\n");
    }
}

static void cmd_mkdrive(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = parse_drive_spec(args);
    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: mkdrive C:\n");
        return;
    }

    if (drive == 0 || drive == 1) {
        console_puts("A: and B: are reserved.\n");
        return;
    }

    drives[drive].present = 1;
    copy_label(drives[drive].label, "VIRTUAL");
    current_drive = drive;
    reset_cwd(drive);

    console_puts("created ");
    print_drive_name(drive);
    console_puts(" as a virtual drive\n");
}

static void cmd_drives(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int found = 0;

    for (int i = 0; i < MAX_DRIVES; ++i) {
        if (!drives[i].present) {
            continue;
        }

        found = 1;
        if (i == current_drive) {
            console_puts("* ");
        } else {
            console_puts("  ");
        }

        print_drive_name(i);
        console_puts(" ");
        console_puts(drives[i].label);
        console_puts("\n");
    }

    if (!found) {
        console_puts("no virtual drives. try: mkdrive C:\n");
    }
}

static void print_size_mib(uint64_t blocks, uint32_t block_size) {
    uint64_t bytes = blocks * block_size;
    console_put_dec64(bytes / (1024ull * 1024ull));
    console_puts(" MiB");
}

static void cmd_blk(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    uint32_t count = storage_block_device_count();
    if (count == 0) {
        console_puts("no block devices\n");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const block_device_t *dev = storage_get_block_device(i);
        if (!dev) {
            continue;
        }

        console_puts(dev->name);
        console_puts(dev->write ? " rw" : " ro");
        console_puts(" blocks=");
        console_put_dec64(dev->block_count);
        console_puts(" block_size=");
        console_put_dec64(dev->block_size);
        console_puts(" size=");
        print_size_mib(dev->block_count, dev->block_size);
        console_puts("\n");
    }
}

static void cmd_part(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    uint32_t count = storage_partition_count();
    if (count == 0) {
        console_puts("no partitions discovered\n");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const partition_t *part = storage_get_partition(i);
        if (!part) {
            continue;
        }

        console_puts(part->name);
        console_puts(" start=");
        console_put_dec64(part->start_lba);
        console_puts(" blocks=");
        console_put_dec64(part->block_count);
        console_puts(" type=0x");
        console_put_hex32(part->mbr_type);
        console_puts(" fs=");
        console_puts(part->fs_hint);
        console_puts("\n");
    }
}

static void split_first_arg(char *s, char **first, char **rest) {
    s = skip_spaces(s);
    *first = s;

    while (*s && *s != ' ' && *s != '\t') {
        ++s;
    }

    if (*s) {
        *s++ = '\0';
        s = skip_spaces(s);
    }

    *rest = s;
}

static void cmd_mount(const char *args, const boot_info_t *info) {
    (void)info;

    char *mutable_args = (char *)args;
    char *drive_arg = 0;
    char *part_arg = 0;
    int drive = -1;
    int status = 0;

    split_first_arg(mutable_args, &drive_arg, &part_arg);
    drive = parse_drive_arg(drive_arg);

    if (drive < 0 || drive >= MAX_DRIVES || *part_arg == '\0') {
        console_puts("usage: mount C: rd0p1\n");
        return;
    }

    status = storage_mount((char)('A' + drive), part_arg);
    if (status == -1) {
        console_puts("partition not found: ");
        console_puts(part_arg);
        console_puts("\n");
        return;
    }

    if (status == -2) {
        console_puts("A: and B: are reserved.\n");
        return;
    }

    drives[drive].present = 1;
    copy_label(drives[drive].label, "MOUNTED");
    current_drive = drive;
    reset_cwd(drive);

    console_puts("mounted ");
    console_puts(part_arg);
    console_puts(" at ");
    print_drive_name(drive);
    console_puts("\n");
}

static void cmd_mounts(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int found = 0;

    for (uint32_t i = 0; i < STORAGE_MAX_MOUNTS; ++i) {
        const mount_t *mount = storage_get_mount(i);
        if (!mount) {
            continue;
        }

        const partition_t *part = storage_get_partition(mount->partition_index);
        found = 1;
        console_puts("  ");
        print_drive_name((int)(mount->drive_letter - 'A'));
        console_puts(" ");
        console_puts(mount->partition_name[0] ? mount->partition_name : (part ? part->name : "?"));
        console_puts(" ");
        console_puts(mount->fs_name);
        console_puts("\n");
    }

    if (!found) {
        console_puts("no mounts. try: mount C: rd0p1\n");
    }
}

static int active_drive(void) {
    if (current_drive >= 0 && current_drive < MAX_DRIVES) {
        return current_drive;
    }

    return -1;
}

static int drive_from_args_or_current(const char *args) {
    int drive = parse_drive_arg(args);
    if (drive >= 0) {
        return drive;
    }

    if (*skip_const_spaces(args) != '\0') {
        return -1;
    }

    return active_drive();
}

static int set_mounted_drive(char drive_letter, const char *label) {
    int drive = to_upper(drive_letter) - 'A';

    if (drive < 0 || drive >= MAX_DRIVES) {
        return -1;
    }

    drives[drive].present = 1;
    copy_label(drives[drive].label, label);
    current_drive = drive;
    reset_cwd(drive);
    return 0;
}

static int has_real_writable_block_device(void) {
    for (uint32_t i = 0; i < storage_block_device_count(); ++i) {
        const block_device_t *dev = storage_get_block_device(i);

        if (!dev || !dev->write) {
            continue;
        }

        if (streq(dev->name, "rd0")) {
            continue;
        }

        return 1;
    }

    return 0;
}

static int live_seed_parent_for_path(char drive_letter,
                                     const char *path,
                                     uint32_t *out_parent,
                                     char *out_name,
                                     uint32_t out_name_size) {
    uint32_t parent = LAINFS_ROOT_DIR;
    const char *p = path;

    if (path == 0 || out_parent == 0 || out_name == 0 || out_name_size == 0) {
        return -1;
    }

    while (*p == '/' || *p == '\\') {
        ++p;
    }

    while (*p) {
        const char *start = p;
        uint32_t len;
        char part[32];

        while (*p && *p != '/' && *p != '\\') {
            ++p;
        }

        len = (uint32_t)(p - start);
        if (copy_path_part_limited(part, sizeof(part), start, len) != 0) {
            return -1;
        }

        if (*p == '\0') {
            copy_text_limited(out_name, out_name_size, part);
            *out_parent = parent;
            return 0;
        }

        {
            uint32_t next_parent = LAINFS_ROOT_DIR;
            int status = lainfs_find_dir(drive_letter, parent, part, &next_parent);

            if (status != 0) {
                status = lainfs_make_dir_in_dir(drive_letter, parent, part);
                if (status != 0) {
                    return -1;
                }
                if (lainfs_find_dir(drive_letter, parent, part, &next_parent) != 0) {
                    return -1;
                }
            }

            parent = next_parent;
        }

        while (*p == '/' || *p == '\\') {
            ++p;
        }
    }

    return -1;
}

static void seed_live_ramdisk(char drive_letter) {
    uint32_t copied = 0;
    uint32_t failed = 0;

    for (uint32_t i = 0; i < RAMDISK_SEED_ENTRY_COUNT; ++i) {
        const ramdisk_seed_entry_t *seed = &ramdisk_seed_entries[i];
        uint32_t parent = LAINFS_ROOT_DIR;
        char name[32];

        if (live_seed_parent_for_path(drive_letter,
                                      seed->path,
                                      &parent,
                                      name,
                                      sizeof(name)) != 0 ||
            lainfs_save_file_in_dir(drive_letter,
                                    parent,
                                    name,
                                    (const char *)seed->data,
                                    seed->size) != 0) {
            ++failed;
            continue;
        }

        ++copied;
    }

    console_puts("seeded live ramdisk files=");
    console_put_dec64(copied);
    if (failed != 0) {
        console_puts(" failed=");
        console_put_dec64(failed);
    }
    console_puts("\n");
}

static int create_seeded_live_ramdisk(char drive_letter, int make_active) {
    int saved_drive = current_drive;

    drive_letter = to_upper(drive_letter);
    if (!storage_find_partition("rd0p1", 0)) {
        return -1;
    }

    if (lainfs_format_partition("rd0p1") != 0 || storage_mount(drive_letter, "rd0p1") != 0) {
        return -1;
    }

    set_mounted_drive(drive_letter, "LIVE");
    console_puts("created live ramdisk rd0p1 at ");
    print_drive_name((int)(drive_letter - 'A'));
    console_puts("\n");
    seed_live_ramdisk(drive_letter);

    if (!make_active && saved_drive >= 0 && saved_drive < MAX_DRIVES) {
        current_drive = saved_drive;
    }

    return 0;
}

int shell_mount_first_lainfs(char drive_letter) {
    drive_letter = to_upper(drive_letter);

    for (uint32_t i = 0; i < storage_partition_count(); ++i) {
        const partition_t *part = storage_get_partition(i);
        if (!part || !streq(part->fs_hint, "lainfs")) {
            continue;
        }

        if (storage_mount(drive_letter, part->name) == 0) {
            set_mounted_drive(drive_letter, "SYSTEM");
            console_puts("mounted ");
            console_puts(part->name);
            console_puts(" at ");
            print_drive_name((int)(drive_letter - 'A'));
            console_puts("\n");
            if (drive_letter != 'R') {
                create_seeded_live_ramdisk('R', 0);
            }
            return 0;
        }
    }

    if (!has_real_writable_block_device() && create_seeded_live_ramdisk(drive_letter, 1) == 0) {
        return 0;
    }

    return -1;
}

static void cmd_format(const char *args, const boot_info_t *info) {
    (void)info;

    const char *target = skip_const_spaces(args);
    int drive = drive_from_args_or_current(args);

    if (drive >= 0 && drive < MAX_DRIVES) {
        int status = lainfs_format((char)('A' + drive));
        if (status == -1) {
            console_puts("drive is not mounted\n");
            return;
        }
        if (status == -4) {
            console_puts("format failed: mounted partition is read-only\n");
            return;
        }
        if (status == -5) {
            console_puts("format failed: disk write failed\n");
            return;
        }
        if (status != 0) {
            console_puts("format failed\n");
            return;
        }

        console_puts("formatted ");
        print_drive_name(drive);
        console_puts(" as lainfs\n");
        reset_cwd(drive);
        return;
    }

    if (*target == '\0') {
        console_puts("usage: format C: | format hd1p1 | format hd1\n");
        return;
    }

    if (storage_find_partition(target, 0)) {
        int status = lainfs_format_partition(target);
        if (status == -4) {
            console_puts("format failed: partition is read-only\n");
            return;
        }
        if (status == -5) {
            console_puts("format failed: disk write failed\n");
            return;
        }
        if (status != 0) {
            console_puts("format failed\n");
            return;
        }

        console_puts("formatted ");
        console_puts(target);
        console_puts(" as lainfs\n");
        return;
    }

    if (storage_find_block_device(target, 0)) {
        char partition_name[12];
        int status = lainfs_format_block_device(target, partition_name, sizeof(partition_name));
        if (status == -2) {
            console_puts("format failed: disk is not writable, mounted, or too small\n");
            return;
        }
        if (status == -3 || status == -5) {
            console_puts("format failed: disk write failed\n");
            return;
        }
        if (status != 0) {
            console_puts("format failed\n");
            return;
        }

        console_puts("formatted ");
        console_puts(target);
        console_puts(" as ");
        console_puts(partition_name);
        console_puts(" (lainfs)\n");
        return;
    }

    console_puts("unknown drive, partition, or block device\n");
}

static void cmd_ls(const char *args, const boot_info_t *info) {
    (void)info;

    uint32_t dir_id = LAINFS_ROOT_DIR;
    const char *target = skip_const_spaces(args);
    int drive = active_drive();

    if (parse_drive_arg(target) >= 0) {
        drive = parse_drive_arg(target);
        dir_id = LAINFS_ROOT_DIR;
    } else if (drive >= 0) {
        int status = resolve_dir_arg(drive, target, &dir_id);
        if (status != 0) {
            console_puts("ls failed: directory not found\n");
            return;
        }
    }

    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: ls [directory|C:]\n");
        return;
    }

    int status = lainfs_list_dir((char)('A' + drive), dir_id);
    if (status == -1) {
        console_puts("drive is not mounted\n");
    } else if (status == -2) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("ls failed: directory not found\n");
    } else if (status != 0) {
        console_puts("ls failed\n");
    }
}

static void cmd_cd(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *target = skip_const_spaces(args);
    uint32_t dir_id = LAINFS_ROOT_DIR;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*target == '\0') {
        reset_cwd(drive);
        return;
    }

    if (streq(target, "\\") || streq(target, "/")) {
        reset_cwd(drive);
        return;
    }

    if (streq(target, ".")) {
        return;
    }

    if (streq(target, "..")) {
        int status = lainfs_parent_dir((char)('A' + drive), cwd_dirs[drive], &dir_id);
        if (status != 0) {
            console_puts("cd failed\n");
            return;
        }

        cwd_dirs[drive] = dir_id;
        pop_path_part(drive);
        return;
    }

    int status = lainfs_find_dir((char)('A' + drive), cwd_dirs[drive], target, &dir_id);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("cd failed: directory not found\n");
    } else if (status != 0) {
        console_puts("cd failed\n");
    } else {
        cwd_dirs[drive] = dir_id;
        append_path_part(drive, target);
    }
}

static void cmd_pwd(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int drive = active_drive();
    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    print_drive_name(drive);
    console_puts(cwd_paths[drive]);
    console_puts("\n");
}

static void cmd_mkdir(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: mkdir name\n");
        return;
    }

    int status = lainfs_make_dir_in_dir((char)('A' + drive), cwd_dirs[drive], name);
    if (status == -2) {
        console_puts("mkdir failed: invalid name\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("directory is full\n");
    } else if (status == -6) {
        console_puts("mkdir failed: name already exists\n");
    } else if (status != 0) {
        console_puts("mkdir failed\n");
    }
}

static void cmd_rm(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: rm name\n");
        return;
    }

    int status = lainfs_delete_in_dir((char)('A' + drive), cwd_dirs[drive], name);
    if (status == -2) {
        console_puts("delete failed: invalid name\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("delete failed: not found\n");
    } else if (status == -9) {
        console_puts("delete failed: directory is not empty\n");
    } else if (status != 0) {
        console_puts("delete failed\n");
    }
}

static void cmd_rename(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    char *old_name = 0;
    char *new_name = 0;
    uint32_t target_parent = LAINFS_ROOT_DIR;
    const char *target_name = 0;
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    split_first_arg((char *)args, &old_name, &new_name);
    if (*old_name == '\0' || *new_name == '\0') {
        console_puts("usage: rename old new\n");
        return;
    }

    if (streq(new_name, ".")) {
        target_parent = cwd_dirs[drive];
        target_name = old_name;
    } else if (streq(new_name, "..")) {
        status = lainfs_parent_dir((char)('A' + drive), cwd_dirs[drive], &target_parent);
        if (status != 0) {
            console_puts("rename failed\n");
            return;
        }
        target_name = old_name;
    } else if (lainfs_find_dir((char)('A' + drive), cwd_dirs[drive], new_name, &target_parent) == 0) {
        target_name = old_name;
    } else {
        target_parent = cwd_dirs[drive];
        target_name = new_name;
    }

    status = lainfs_rename_in_dir((char)('A' + drive),
                                  cwd_dirs[drive],
                                  old_name,
                                  target_parent,
                                  target_name);
    if (status == -2) {
        console_puts("rename failed: invalid name\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("rename failed: not found\n");
    } else if (status == -6) {
        console_puts("rename failed: target exists\n");
    } else if (status == -8) {
        console_puts("rename failed: cannot move a directory into itself\n");
    } else if (status != 0) {
        console_puts("rename failed\n");
    }
}

static void cmd_cp(const char *args, const boot_info_t *info) {
    (void)info;

    char *src_name = 0;
    char *dst_name = 0;
    char *extra = 0;

    split_first_arg((char *)args, &src_name, &dst_name);
    split_first_arg(dst_name, &dst_name, &extra);

    if (*src_name == '\0' || *dst_name == '\0' || *extra != '\0') {
        console_puts("usage: cp source dest\n");
        return;
    }

    if (shell_api_copy_file(src_name, dst_name) != 0) {
        console_puts("cp failed\n");
        return;
    }

    console_puts("copied ");
    console_puts(src_name);
    console_puts(" to ");
    console_puts(dst_name);
    console_puts("\n");
}

static void cmd_write(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    char *name = 0;
    char *text = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    split_first_arg((char *)args, &name, &text);
    if (*name == '\0' || *text == '\0') {
        console_puts("usage: write name text\n");
        return;
    }

    int status = lainfs_write_file_in_dir((char)('A' + drive), cwd_dirs[drive], name, text);
    if (status == -2) {
        console_puts("write failed: invalid filename\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("directory is full\n");
    } else if (status == -8) {
        console_puts("write failed: name is a directory\n");
    } else if (status == -9) {
        console_puts("write failed: disk is full\n");
    } else if (status != 0) {
        console_puts("write failed\n");
    }
}

static void cmd_cat(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: cat name\n");
        return;
    }

    int status = lainfs_read_file_in_dir((char)('A' + drive), cwd_dirs[drive], name);
    if (status == -2) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("file not found\n");
    } else if (status != 0) {
        console_puts("cat failed\n");
    }
}

static void cmd_edit(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: edit name\n");
        return;
    }

    int status = editor_run_in_dir((char)('A' + drive), cwd_dirs[drive], name);
    if (status == -1) {
        console_puts("edit failed: invalid filename\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -6) {
        console_puts("edit failed: file is too large\n");
    } else if (status != 0) {
        console_puts("edit failed\n");
    }
}

static void cmd_browse(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    int right_drive = drive;
    uint32_t right_dir = LAINFS_ROOT_DIR;
    const char *target = skip_const_spaces(args);
    const char *right_path = "\\";
    char right_path_buffer[SHELL_PATH_SIZE];
    int status;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*target != '\0') {
        right_drive = path_drive_prefix(&target, drive);
        if (right_drive < 0) {
            console_puts("browse failed: drive not mounted\n");
            return;
        }
        if (*target == '\0') {
            right_dir = cwd_dirs[right_drive];
        }
    }

    if (*target != '\0' && resolve_dir_arg(right_drive, target, &right_dir) != 0) {
        console_puts("browse failed: directory not found\n");
        return;
    }

    if (*target == '\0' || streq(target, ".")) {
        right_path = cwd_paths[right_drive];
    } else if (*target != '\0') {
        uint32_t len = 0;
        uint32_t i = 0;

        right_path_buffer[0] = '\0';
        if (target[0] == '\\' || target[0] == '/') {
            while (target[i] && i + 1u < sizeof(right_path_buffer)) {
                right_path_buffer[i] = target[i];
                ++i;
            }
            right_path_buffer[i] = '\0';
        } else {
            while (cwd_paths[right_drive][len] && len + 1u < sizeof(right_path_buffer)) {
                right_path_buffer[len] = cwd_paths[right_drive][len];
                ++len;
            }
            if (len > 1u && len + 1u < sizeof(right_path_buffer)) {
                right_path_buffer[len++] = '\\';
            }
            while (target[i] && len + 1u < sizeof(right_path_buffer)) {
                right_path_buffer[len++] = target[i++];
            }
            right_path_buffer[len] = '\0';
        }
        right_path = right_path_buffer;
    }

    status = browser_run((char)('A' + drive),
                         cwd_dirs[drive],
                         cwd_paths[drive],
                         (char)('A' + right_drive),
                         right_dir,
                         right_path);
    if (status != 0) {
        console_puts("browse failed\n");
    }
}

int shell_api_mkdir(const char *path) {
    int drive = active_drive();
    uint32_t parent = LAINFS_ROOT_DIR;
    char name[32];

    if (drive < 0 || path == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          &parent,
                          name,
                          sizeof(name)) != 0) {
        return -1;
    }

    return lainfs_make_dir_in_dir((char)('A' + drive), parent, name);
}

int shell_api_delete(const char *path) {
    int drive = active_drive();
    uint32_t parent = LAINFS_ROOT_DIR;
    char name[32];

    if (drive < 0 || path == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          &parent,
                          name,
                          sizeof(name)) != 0) {
        return -1;
    }

    return lainfs_delete_in_dir((char)('A' + drive), parent, name);
}

int shell_api_write_file(const char *path, const char *text) {
    int drive = active_drive();
    uint32_t parent = LAINFS_ROOT_DIR;
    char name[32];

    if (drive < 0 || path == 0 || text == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          &parent,
                          name,
                          sizeof(name)) != 0) {
        return -1;
    }

    return lainfs_write_file_in_dir((char)('A' + drive), parent, name, text);
}

int shell_api_cat_file(const char *path) {
    int drive = active_drive();
    uint32_t parent = LAINFS_ROOT_DIR;
    char name[32];

    if (drive < 0 || path == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          &parent,
                          name,
                          sizeof(name)) != 0) {
        return -1;
    }

    return lainfs_read_file_in_dir((char)('A' + drive), parent, name);
}

int shell_api_file_size(const char *path) {
    int drive = active_drive();
    uint32_t parent = LAINFS_ROOT_DIR;
    uint32_t size = 0;
    char name[32];

    if (drive < 0 || path == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          &parent,
                          name,
                          sizeof(name)) != 0) {
        return -1;
    }

    if (lainfs_load_file_in_dir((char)('A' + drive),
                                parent,
                                name,
                                zinclude_buffers[0],
                                ASM_SOURCE_SIZE,
                                &size) != 0) {
        return -1;
    }

    return (int)size;
}

int shell_api_read_file(const char *path, char *buffer, uint32_t capacity) {
    int drive = active_drive();
    uint32_t parent = LAINFS_ROOT_DIR;
    uint32_t size = 0;
    char name[32];

    if (drive < 0 || path == 0 || buffer == 0 || capacity == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          path,
                          &parent,
                          name,
                          sizeof(name)) != 0) {
        return -1;
    }

    if (lainfs_load_file_in_dir((char)('A' + drive),
                                parent,
                                name,
                                buffer,
                                capacity,
                                &size) != 0) {
        return -1;
    }

    if (size < capacity) {
        buffer[size] = '\0';
    }
    return (int)size;
}

int shell_api_load_file_shared(const char *path) {
    return shell_api_read_file(path, shell_wget_buffer, LAINFS_FILE_CAPACITY);
}

uint8_t *shell_api_file_buffer(void) {
    return (uint8_t *)shell_wget_buffer;
}

int shell_api_http_get(const char *url, char *buffer, uint32_t capacity) {
    uint32_t size = 0;
    int status;

    if (url == 0 || buffer == 0 || capacity == 0) {
        return -1;
    }
    if (net_device_count() == 0) {
        return -2;
    }

    status = net_http_get(0, url, buffer, capacity, &size);
    if (status != 0) {
        return status;
    }
    if (size < capacity) {
        buffer[size] = '\0';
    }
    return (int)size;
}

int shell_api_rename(const char *old_path, const char *new_path) {
    int drive = active_drive();
    uint32_t old_parent = LAINFS_ROOT_DIR;
    uint32_t new_parent = LAINFS_ROOT_DIR;
    char old_name[32];
    char new_name[32];

    if (drive < 0 || old_path == 0 || new_path == 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          old_path,
                          &old_parent,
                          old_name,
                          sizeof(old_name)) != 0 ||
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          new_path,
                          &new_parent,
                          new_name,
                          sizeof(new_name)) != 0) {
        return -1;
    }

    return lainfs_rename_in_dir((char)('A' + drive),
                                old_parent,
                                old_name,
                                new_parent,
                                new_name);
}

int shell_api_copy_file(const char *src_path, const char *dst_path) {
    int drive = active_drive();
    int src_drive = -1;
    int dst_drive = -1;
    uint32_t src_parent = LAINFS_ROOT_DIR;
    uint32_t dst_parent = LAINFS_ROOT_DIR;
    uint32_t size = 0;
    char src_name[32];
    char dst_name[32];

    if (drive < 0 || src_path == 0 || dst_path == 0 ||
        resolve_file_path_with_drive(src_path,
                                     drive,
                                     &src_drive,
                                     &src_parent,
                                     src_name,
                                     sizeof(src_name)) != 0 ||
        resolve_file_path_with_drive(dst_path,
                                     drive,
                                     &dst_drive,
                                     &dst_parent,
                                     dst_name,
                                     sizeof(dst_name)) != 0) {
        return -1;
    }

    if (lainfs_load_file_in_dir((char)('A' + src_drive),
                                src_parent,
                                src_name,
                                zinclude_buffers[0],
                                ASM_SOURCE_SIZE,
                                &size) != 0) {
        return -1;
    }

    {
        uint32_t dst_dir = LAINFS_ROOT_DIR;
        if (lainfs_find_dir((char)('A' + dst_drive), dst_parent, dst_name, &dst_dir) == 0) {
            dst_parent = dst_dir;
            copy_text_limited(dst_name, sizeof(dst_name), src_name);
        }
    }

    return lainfs_save_file_in_dir((char)('A' + dst_drive),
                                   dst_parent,
                                   dst_name,
                                   zinclude_buffers[0],
                                   size);
}

int shell_api_strlen(const char *text) {
    uint32_t len = 0;

    if (text == 0) {
        return -1;
    }

    while (text[len]) {
        ++len;
    }

    return (int)len;
}

int shell_api_strcmp(const char *a, const char *b) {
    if (a == 0 || b == 0) {
        return -1;
    }

    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }

    if (*a == *b) {
        return 0;
    }

    return ((unsigned char)*a < (unsigned char)*b) ? -1 : 1;
}

int shell_api_starts_with(const char *text, const char *prefix) {
    if (text == 0 || prefix == 0) {
        return 0;
    }

    while (*prefix) {
        if (*text != *prefix) {
            return 0;
        }
        ++text;
        ++prefix;
    }

    return 1;
}

int shell_api_atoi(const char *text) {
    int sign = 1;
    int value = 0;

    if (text == 0) {
        return 0;
    }

    text = skip_const_spaces(text);
    if (*text == '-') {
        sign = -1;
        ++text;
    } else if (*text == '+') {
        ++text;
    }

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        ++text;
    }

    return value * sign;
}

int shell_api_list_dir(const char *path) {
    int drive = active_drive();
    uint32_t dir = LAINFS_ROOT_DIR;

    if (drive < 0 || path == 0 ||
        resolve_dir_path((char)('A' + drive), cwd_dirs[drive], path, &dir) != 0) {
        return -1;
    }

    return lainfs_list_dir((char)('A' + drive), dir);
}

int shell_api_chdir(const char *path) {
    int drive = active_drive();
    uint32_t dir = LAINFS_ROOT_DIR;

    if (drive < 0 || path == 0 ||
        resolve_dir_path((char)('A' + drive), cwd_dirs[drive], path, &dir) != 0) {
        return -1;
    }

    cwd_dirs[drive] = dir;
    return 0;
}

int shell_api_dir_count(const char *path) {
    int drive = active_drive();
    uint32_t dir = LAINFS_ROOT_DIR;
    uint32_t count = 0;

    if (drive < 0 || path == 0 ||
        resolve_dir_path((char)('A' + drive), cwd_dirs[drive], path, &dir) != 0 ||
        lainfs_child_count((char)('A' + drive), dir, &count) != 0) {
        return -1;
    }

    return (int)count;
}

int shell_api_dir_name(const char *path, uint32_t index, char *buffer, uint32_t capacity) {
    int drive = active_drive();
    uint32_t dir = LAINFS_ROOT_DIR;
    uint32_t type = 0;
    uint32_t size = 0;

    if (drive < 0 || path == 0 || buffer == 0 || capacity == 0 ||
        resolve_dir_path((char)('A' + drive), cwd_dirs[drive], path, &dir) != 0 ||
        lainfs_child_info((char)('A' + drive), dir, index, buffer, capacity, &type, &size) != 0) {
        return -1;
    }

    return 0;
}

int shell_api_dir_type(const char *path, uint32_t index) {
    int drive = active_drive();
    uint32_t dir = LAINFS_ROOT_DIR;
    uint32_t type = 0;
    uint32_t size = 0;
    char name[32];

    if (drive < 0 || path == 0 ||
        resolve_dir_path((char)('A' + drive), cwd_dirs[drive], path, &dir) != 0 ||
        lainfs_child_info((char)('A' + drive), dir, index, name, sizeof(name), &type, &size) != 0) {
        return -1;
    }

    return (int)type;
}

int shell_api_dir_size(const char *path, uint32_t index) {
    int drive = active_drive();
    uint32_t dir = LAINFS_ROOT_DIR;
    uint32_t type = 0;
    uint32_t size = 0;
    char name[32];

    if (drive < 0 || path == 0 ||
        resolve_dir_path((char)('A' + drive), cwd_dirs[drive], path, &dir) != 0 ||
        lainfs_child_info((char)('A' + drive), dir, index, name, sizeof(name), &type, &size) != 0) {
        return -1;
    }

    return (int)size;
}

static int copy_command_arg(const char *text, char *command, uint32_t command_size) {
    uint32_t i = 0;

    if (text == 0 || command == 0 || command_size == 0 || *text == '\0') {
        return -1;
    }

    while (text[i] && i + 1u < command_size) {
        command[i] = text[i];
        ++i;
    }
    if (text[i] != '\0') {
        return -1;
    }
    command[i] = '\0';
    return 0;
}

int shell_api_zbuild(const char *target) {
    char *manifest = shell_manifest_buffer;
    char command[64];
    char output_name[32];
    uint32_t build_dir = 0;
    uint32_t output_size = 0;
    int drive = active_drive();

    if (drive < 0 || copy_command_arg(target, command, sizeof(command)) != 0 ||
        zbuild_read_layout(drive,
                           command,
                           manifest,
                           ASM_SOURCE_SIZE,
                           &build_dir,
                           output_name,
                           sizeof(output_name),
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0) != 0) {
        return -1;
    }

    cmd_zclean(command, 0);
    cmd_zbuild(command, 0);
    if (lainfs_load_file_in_dir((char)('A' + drive),
                                build_dir,
                                output_name,
                                zinclude_buffers[0],
                                ASM_SOURCE_SIZE,
                                &output_size) != 0 ||
        output_size == 0) {
        return -1;
    }

    return 0;
}

int shell_api_ztest(const char *target) {
    char *manifest = shell_manifest_buffer;
    char command[64];
    char output_name[32];
    char testlog_name[32];
    uint32_t build_dir = 0;
    uint32_t testlog_size = 0;
    int drive = active_drive();

    if (drive < 0 || copy_command_arg(target, command, sizeof(command)) != 0 ||
        make_suffixed_name(command, ".testlog", testlog_name, sizeof(testlog_name)) != 0 ||
        zbuild_read_layout(drive,
                           command,
                           manifest,
                           ASM_SOURCE_SIZE,
                           &build_dir,
                           output_name,
                           sizeof(output_name),
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0) != 0) {
        return -1;
    }

    cmd_ztest(command, 0);
    if (lainfs_load_file_in_dir((char)('A' + drive),
                                build_dir,
                                testlog_name,
                                zinclude_buffers[0],
                                ASM_SOURCE_SIZE,
                                &testlog_size) != 0 ||
        testlog_size >= ASM_SOURCE_SIZE) {
        return -1;
    }
    zinclude_buffers[0][testlog_size] = '\0';

    return contains_text(zinclude_buffers[0], "\nstatus ok\n") ? 0 : -1;
}

int shell_api_zinstall(const char *target) {
    char *manifest = shell_manifest_buffer;
    char command[64];
    char output_name[32];
    char install_name[32];
    uint32_t build_dir = 0;
    uint32_t install_dir = 0;
    uint32_t installed_size = 0;
    int drive = active_drive();

    if (drive < 0 || copy_command_arg(target, command, sizeof(command)) != 0 ||
        zbuild_read_layout(drive,
                           command,
                           manifest,
                           ASM_SOURCE_SIZE,
                           &build_dir,
                           output_name,
                           sizeof(output_name),
                           &install_dir,
                           install_name,
                           sizeof(install_name),
                           0,
                           0,
                           0,
                           0) != 0) {
        return -1;
    }
    (void)build_dir;
    (void)output_name;

    cmd_zinstall(command, 0);
    if (lainfs_load_file_in_dir((char)('A' + drive),
                                install_dir,
                                install_name,
                                zinclude_buffers[0],
                                ASM_SOURCE_SIZE,
                                &installed_size) != 0 ||
        installed_size == 0) {
        return -1;
    }

    return 0;
}

int shell_api_zmod(const char *target) {
    char command[64];
    uint32_t before = shell_module_count();

    if (active_drive() < 0 || copy_command_arg(target, command, sizeof(command)) != 0) {
        return -1;
    }

    cmd_zmod(command, 0);
    return shell_module_count() > before ? 0 : -1;
}

int shell_api_zunload(const char *target) {
    char command[64];
    uint32_t before = shell_module_count();

    if (copy_command_arg(target, command, sizeof(command)) != 0) {
        return -1;
    }

    cmd_zunload(command, 0);
    return shell_module_count() < before ? 0 : -1;
}

int shell_api_zreload(const char *target) {
    char command[64];
    uint32_t before;
    int had_old;

    if (active_drive() < 0 || copy_command_arg(target, command, sizeof(command)) != 0) {
        return -1;
    }

    before = shell_module_count();
    had_old = zmodule_find_slot_by_name(command) >= 0;
    cmd_zreload(command, 0);
    if (had_old) {
        return shell_module_count() == before ? 0 : -1;
    }
    return shell_module_count() > before ? 0 : -1;
}

static void cmd_keymap(const char *args, const boot_info_t *info) {
    const char *layout = skip_const_spaces(args);

    (void)info;

    if (*layout == '\0') {
        console_puts("keyboard layout: ");
        console_puts(keyboard_layout_name(keyboard_get_layout()));
        console_puts("\n");
        return;
    }

    if (streq(layout, "us")) {
        keyboard_set_layout(KEYBOARD_LAYOUT_US);
    } else if (streq(layout, "de")) {
        keyboard_set_layout(KEYBOARD_LAYOUT_DE);
    } else {
        console_puts("usage: keymap us|de\n");
        return;
    }

    console_puts("keyboard layout set to ");
    console_puts(keyboard_layout_name(keyboard_get_layout()));
    console_puts("\n");
}

static void cmd_ahci(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    console_puts("AHCI controllers=");
    console_put_dec64(ahci_controller_count());
    console_puts(" disks=");
    console_put_dec64(ahci_disk_count());
    console_puts("\n");
}

static void put_hex_byte(uint8_t value) {
    static const char digits[] = "0123456789abcdef";
    char text[3];

    text[0] = digits[(value >> 4) & 0x0Fu];
    text[1] = digits[value & 0x0Fu];
    text[2] = '\0';
    console_puts(text);
}

static void cmd_net_print_mac(const uint8_t mac[NET_MAC_SIZE]) {
    for (uint32_t i = 0; i < NET_MAC_SIZE; ++i) {
        if (i != 0) {
            console_puts(":");
        }
        put_hex_byte(mac[i]);
    }
}

static void cmd_net_print_ipv4(uint32_t ip) {
    console_put_dec64((ip >> 24) & 0xFFu);
    console_puts(".");
    console_put_dec64((ip >> 16) & 0xFFu);
    console_puts(".");
    console_put_dec64((ip >> 8) & 0xFFu);
    console_puts(".");
    console_put_dec64(ip & 0xFFu);
}

static void cmd_net_print_bytes(const uint8_t *bytes, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) {
            console_puts(" ");
        }
        put_hex_byte(bytes[i]);
    }
}

static void build_test_frame(const net_device_t *dev, uint8_t frame[NET_MIN_FRAME_SIZE]) {
    static const char payload[] = "lainos e1000 test";

    for (uint32_t i = 0; i < 6u; ++i) {
        frame[i] = 0xFFu;
        frame[6u + i] = dev->mac[i];
    }

    frame[12] = 0x88u;
    frame[13] = 0xB5u;
    for (uint32_t i = 14u; i < NET_MIN_FRAME_SIZE; ++i) {
        frame[i] = 0;
    }
    for (uint32_t i = 0; payload[i] && 14u + i < NET_MIN_FRAME_SIZE; ++i) {
        frame[14u + i] = (uint8_t)payload[i];
    }
}

static void cmd_net_print_debug(void) {
    net_debug_info_t debug;
    e1000_debug_info_t hw;

    net_debug_info(&debug);
    console_puts("netdbg arp_tx=");
    console_put_dec64(debug.arp_requests);
    console_puts(" arp_txerr=");
    console_put_dec64(debug.arp_tx_errors);
    console_puts(" arp_rx=");
    console_put_dec64(debug.arp_replies);
    console_puts(" arp_bad=");
    console_put_dec64(debug.arp_mismatches);
    console_puts(" rx_arp=");
    console_put_dec64(debug.rx_arp);
    console_puts(" rx_ip=");
    console_put_dec64(debug.rx_ipv4);
    console_puts(" rx_other=");
    console_put_dec64(debug.rx_other);
    console_puts(" last_type=0x");
    console_put_hex32(debug.last_eth_type);
    console_puts(" inner=0x");
    console_put_hex32(debug.last_inner_eth_type);
    console_puts(" last_arp_op=");
    console_put_dec64(debug.last_arp_op);
    console_puts(" request=");
    cmd_net_print_ipv4(debug.last_arp_requested_ip);
    console_puts(" sender=");
    cmd_net_print_ipv4(debug.last_arp_sender_ip);
    console_puts(" target=");
    cmd_net_print_ipv4(debug.last_arp_target_ip);
    console_puts("\n");

    if (e1000_debug_info(0, &hw) == 0) {
        console_puts("e1000 status=0x");
        console_put_hex32(hw.status);
        console_puts(" ctrl=0x");
        console_put_hex32(hw.ctrl);
        console_puts(" ext=0x");
        console_put_hex32(hw.ctrl_ext);
        console_puts(" pcicmd=0x");
        console_put_hex32(hw.pci_command);
        console_puts(" pm=0x");
        console_put_hex32(hw.pmcsr);
        console_puts(" rctl=0x");
        console_put_hex32(hw.rctl);
            console_puts(" tctl=0x");
            console_put_hex32(hw.tctl);
            console_puts(" rxdctl=0x");
            console_put_hex32(hw.rxdctl);
            console_puts(" txdctl=0x");
            console_put_hex32(hw.txdctl);
            console_puts(" tarc0=0x");
            console_put_hex32(hw.tarc0);
            console_puts(" tarc1=0x");
            console_put_hex32(hw.tarc1);
            console_puts(" iosfpc=0x");
            console_put_hex32(hw.iosfpc);
            console_puts(" pba=0x");
            console_put_hex32(hw.pba);
            console_puts(" gcr=0x");
            console_put_hex32(hw.gcr);
            console_puts(" fwsm=0x");
            console_put_hex32(hw.fwsm);
            console_puts(" h2me=0x");
            console_put_hex32(hw.h2me);
            console_puts(" phyres=0x");
            console_put_hex32(hw.phy_result);
            console_puts(" phypm=0x");
            console_put_hex32(hw.phy_pm_ctrl);
            console_puts(" phyulp=0x");
            console_put_hex32(hw.phy_ulp_cfg);
            console_puts(" rst=0x");
            console_put_hex32(hw.reset_result);
            console_puts(" rdh=");
        console_put_dec64(hw.rdh);
        console_puts(" rdt=");
        console_put_dec64(hw.rdt);
        console_puts(" tdh=");
        console_put_dec64(hw.tdh);
        console_puts(" tdt=");
        console_put_dec64(hw.tdt);
        console_puts(" txbase=0x");
        console_put_hex64(hw.tx_reg_base);
        console_puts(" txreglen=");
        console_put_dec64(hw.tx_reg_len);
        console_puts(" txs=0x");
        console_put_hex32(hw.last_tx_status);
        console_puts(" txc=0x");
        console_put_hex32(hw.last_tx_command);
        console_puts(" txlen=");
        console_put_dec64(hw.last_tx_length);
        console_puts(" txaddr=0x");
        console_put_hex64(hw.last_tx_desc_addr);
        console_puts(" txdesc=0x");
        console_put_hex64(hw.last_tx_desc_phys);
        console_puts(" txdescbytes=");
        cmd_net_print_bytes(hw.last_tx_desc_bytes, sizeof(hw.last_tx_desc_bytes));
        console_puts(" rxst=0x");
            console_put_hex32(hw.first_rx_status);
            console_puts(" rxerr=0x");
            console_put_hex32(hw.first_rx_errors);
            console_puts(" rxlen=");
            console_put_dec64(hw.first_rx_length);
            console_puts(" rxaddr=0x");
            console_put_hex64(hw.first_rx_desc_addr);
            console_puts(" rxbytes=");
            cmd_net_print_bytes(hw.first_rx_bytes, sizeof(hw.first_rx_bytes));
            console_puts(" rxdd=");
            console_put_dec64(hw.rx_scan_dd_count);
            console_puts(" rxdi=");
            console_put_dec64(hw.rx_scan_index);
            console_puts(" rxds=0x");
            console_put_hex32(hw.rx_scan_status);
            console_puts("/e0x");
            console_put_hex32(hw.rx_scan_errors);
            console_puts("/");
            console_put_dec64(hw.rx_scan_length);
            console_puts(" ");
            cmd_net_print_bytes(hw.rx_scan_bytes, sizeof(hw.rx_scan_bytes));
            console_puts(" lastrx=0x");
            console_put_hex32(hw.last_rx_status);
            console_puts("/e0x");
            console_put_hex32(hw.last_rx_errors);
            console_puts("/");
            console_put_dec64(hw.last_rx_length);
            console_puts("/a0x");
            console_put_hex64(hw.last_rx_desc_addr);
            console_puts(" ");
            cmd_net_print_bytes(hw.last_rx_bytes, sizeof(hw.last_rx_bytes));
            console_puts(" rxring=0x");
            console_put_hex64(hw.rx_ring_phys);
            console_puts(" txring=0x");
            console_put_hex64(hw.tx_ring_phys);
            console_puts(" rxbuf=0x");
            console_put_hex64(hw.first_rx_phys);
            console_puts("\n");
        }
    }

static void cmd_net_print_device(uint32_t index) {
    const net_device_t *dev = net_get_device_const(index);

    if (!dev) {
        return;
    }

    console_puts(dev->name);
    console_puts(" ");
    console_puts(dev->driver);
    console_puts(" pci=");
    console_put_hex32(dev->vendor_id);
    console_puts(":");
    console_put_hex32(dev->device_id);
    console_puts(" mac=");
    cmd_net_print_mac(dev->mac);
    console_puts(dev->link_up ? " link=up" : " link=down");
    console_puts(" rx=");
    console_put_dec64(dev->rx_packets);
    console_puts(" tx=");
    console_put_dec64(dev->tx_packets);
    console_puts(" drop=");
    console_put_dec64(dev->rx_dropped);
    console_puts(" txerr=");
    console_put_dec64(dev->tx_errors);
    console_puts("\n");
}

static void cmd_net(const char *args, const boot_info_t *info) {
    char *mutable_args = (char *)args;
    char *command = 0;
    char *index_text = 0;
    char *extra = 0;
    uint64_t index = 0;

    (void)info;

    split_first_arg(mutable_args, &command, &index_text);
    if (*command != '\0') {
        split_first_arg(index_text, &index_text, &extra);
        if (streq(command, "arp")) {
            uint32_t ip = 0;
            uint8_t mac[NET_MAC_SIZE];

            if (*index_text == '\0' ||
                *extra != '\0' ||
                net_parse_ipv4_addr(index_text, &ip) != 0) {
                console_puts("usage: net arp ip\n");
                return;
            }

            if (net_device_count() == 0) {
                console_puts("net: no supported network device\n");
                return;
            }

            console_puts("net: arp ");
            cmd_net_print_ipv4(ip);
            console_puts("\n");
            if (net_arp_probe(0, ip, mac) != 0) {
                console_puts("net: arp timed out\n");
                cmd_net_print_device(0);
                cmd_net_print_debug();
            } else {
                console_puts("net: arp reply mac=");
                cmd_net_print_mac(mac);
                console_puts("\n");
                cmd_net_print_device(0);
                cmd_net_print_debug();
            }
            return;
        }

        if (streq(command, "reset")) {
            if (*index_text != '\0' || *extra != '\0') {
                console_puts("usage: net reset\n");
                return;
            }

            if (net_device_count() == 0) {
                console_puts("net: no supported network device\n");
                return;
            }

            console_puts("net: reset eth0\n");
            if (e1000_reset_controller(0) != 0) {
                console_puts("net: reset completed with warnings\n");
            } else {
                console_puts("net: reset complete\n");
            }
            cmd_net_print_device(0);
            cmd_net_print_debug();
            return;
        }

        if (streq(command, "ip")) {
            char *mask_text = extra;
            char *gateway_text = 0;
            char *too_much = 0;
            uint32_t address = 0;
            uint32_t mask = 0;
            uint32_t gateway = 0;

            if (*index_text == '\0') {
                console_puts("ip=");
                cmd_net_print_ipv4(net_ipv4_address());
                console_puts(" mask=");
                cmd_net_print_ipv4(net_ipv4_netmask());
                console_puts(" gateway=");
                cmd_net_print_ipv4(net_ipv4_gateway());
                console_puts("\n");
                return;
            }

            split_first_arg(mask_text, &mask_text, &gateway_text);
            split_first_arg(gateway_text, &gateway_text, &too_much);
            if (*mask_text == '\0' ||
                *too_much != '\0' ||
                net_parse_ipv4_addr(index_text, &address) != 0 ||
                net_parse_ipv4_addr(mask_text, &mask) != 0 ||
                (*gateway_text != '\0' && net_parse_ipv4_addr(gateway_text, &gateway) != 0)) {
                console_puts("usage: net ip [address mask [gateway]]\n");
                return;
            }

            net_set_ipv4_config(address, mask, gateway);
            console_puts("net: ip=");
            cmd_net_print_ipv4(net_ipv4_address());
            console_puts(" mask=");
            cmd_net_print_ipv4(net_ipv4_netmask());
            console_puts(" gateway=");
            cmd_net_print_ipv4(net_ipv4_gateway());
            console_puts("\n");
            return;
        }

        if ((!streq(command, "poll") && !streq(command, "send")) ||
            *index_text == '\0' ||
            *extra != '\0' ||
            parse_u64_arg(index_text, &index) != 0) {
            console_puts("usage: net [poll|send] index | net arp ip | net reset | net ip [address mask [gateway]]\n");
            return;
        }

        if (index >= net_device_count()) {
            console_puts("net: device not found\n");
            return;
        }

        if (streq(command, "poll")) {
            int handled = net_poll_device((uint32_t)index);
            console_puts("net: poll handled ");
            console_put_dec64(handled < 0 ? 0u : (uint32_t)handled);
            console_puts(" packet(s)\n");
        } else {
            const net_device_t *dev = net_get_device_const((uint32_t)index);
            uint8_t frame[NET_MIN_FRAME_SIZE];
            if (!dev) {
                console_puts("net: device not found\n");
                return;
            }

            build_test_frame(dev, frame);
            if (net_send_frame((uint32_t)index, frame, sizeof(frame)) != 0) {
                console_puts("net: send failed\n");
            } else {
                console_puts("net: sent raw Ethernet test frame\n");
            }
        }
    }

    if (net_device_count() == 0) {
        console_puts("no supported network devices. In QEMU try: -netdev user,id=net0 -device e1000,netdev=net0\n");
    }

    console_puts("ip=");
    cmd_net_print_ipv4(net_ipv4_address());
    console_puts(" mask=");
    cmd_net_print_ipv4(net_ipv4_netmask());
    console_puts(" gateway=");
    cmd_net_print_ipv4(net_ipv4_gateway());
    console_puts("\n");

    cmd_net_print_debug();

    for (uint32_t i = 0; i < net_device_count(); ++i) {
        cmd_net_print_device(i);
    }

    for (uint32_t i = 0; i < intel_net_unsupported_count(); ++i) {
        uint16_t vendor_id = 0;
        uint16_t device_id = 0;
        const char *name = "?";
        const char *needed_driver = "?";

        if (intel_net_unsupported_info(i, &vendor_id, &device_id, &name, &needed_driver) != 0) {
            continue;
        }

        console_puts("unsupported Intel Ethernet ");
        console_puts(name);
        console_puts(" pci=");
        console_put_hex32(vendor_id);
        console_puts(":");
        console_put_hex32(device_id);
        console_puts(" needs ");
        console_puts(needed_driver);
        console_puts("\n");
    }
}

static void wget_default_name(const char *url, char *out, uint32_t out_size) {
    const char *last = url;
    const char *s = url;

    while (*s) {
        if (*s == '/') {
            last = s + 1;
        }
        ++s;
    }

    if (*last == '\0') {
        copy_text_limited(out, out_size, "index.html");
    } else {
        copy_text_limited(out, out_size, last);
    }
}

static void cmd_wget(const char *args, const boot_info_t *info) {
    char *url = 0;
    char *output = 0;
    char *extra = 0;
    char output_name[32];
    uint32_t size = 0;
    uint32_t parent = LAINFS_ROOT_DIR;
    int drive = active_drive();
    int status = 0;

    (void)info;

    split_first_arg((char *)args, &url, &output);
    split_first_arg(output, &output, &extra);
    if (*url == '\0' || *extra != '\0') {
        console_puts("usage: wget http://IP[:port]/path [output]\n");
        return;
    }

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (net_device_count() == 0) {
        console_puts("wget failed: no network device\n");
        return;
    }

    if (*output == '\0') {
        wget_default_name(url, output_name, sizeof(output_name));
        output = output_name;
    }

    if (resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          output,
                          &parent,
                          output_name,
                          sizeof(output_name)) != 0) {
        console_puts("wget failed: bad output path\n");
        return;
    }

    console_puts("wget: fetching ");
    console_puts(url);
    console_puts("\n");

    status = net_http_get(0, url, shell_wget_buffer, LAINFS_FILE_CAPACITY + 1u, &size);
    if (status == -2) {
        console_puts("wget failed: use plain http://numeric.ip[:port]/path\n");
        return;
    }
    if (status == -3) {
        console_puts("wget failed: arp lookup timed out\n");
        return;
    }
    if (status == -7) {
        console_puts("wget failed: no route; set net ip address mask gateway\n");
        return;
    }
    if (status == -5) {
        console_puts("wget failed: tcp connect timed out\n");
        return;
    }
    if (status == -6) {
        console_puts("wget failed: http receive timed out\n");
        return;
    }
    if (status != 0) {
        console_puts("wget failed\n");
        return;
    }

    if (lainfs_save_file_in_dir((char)('A' + drive), parent, output_name, shell_wget_buffer, size) != 0) {
        console_puts("wget failed: could not save output\n");
        return;
    }

    console_puts("wget: saved ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(size);
    console_puts("\n");
}

static void cmd_desktop(const char *args, const boot_info_t *info) {
    (void)args;
    desktop_run(info);
}

static void console_put_signed_dec(int value) {
    if (value < 0) {
        console_puts("-");
        console_put_dec64((uint64_t)(-(int64_t)value));
    } else {
        console_put_dec64((uint64_t)value);
    }
}

static void cmd_mouse(const char *args, const boot_info_t *info) {
    mouse_debug_info_t debug;

    (void)args;
    (void)info;
    mouse_debug_info(&debug);

    console_puts("mouse: enabled=");
    console_put_dec64((uint64_t)debug.enabled);
    console_puts(" source=");
    if (debug.last_source == 1) {
        console_puts("ps2");
    } else if (debug.last_source == 2) {
        console_puts("usb");
    } else {
        console_puts("none");
    }
    console_puts(" ps2=");
    console_put_dec64((uint64_t)debug.ps2_enabled);
    console_puts(" ps2wheel=");
    console_put_dec64((uint64_t)debug.ps2_has_wheel);
    console_puts(" ps2size=");
    console_put_dec64((uint64_t)debug.ps2_packet_size);
    console_puts("\n");

    console_puts("mouse: ps2pkts=");
    console_put_dec64((uint64_t)debug.ps2_packets);
    console_puts(" usbreports=");
    console_put_dec64((uint64_t)debug.usb_reports);
    console_puts(" rejected=");
    console_put_dec64((uint64_t)debug.rejected_packets);
    console_puts("\n");

    console_puts("mouse: x=");
    console_put_signed_dec(debug.x);
    console_puts(" y=");
    console_put_signed_dec(debug.y);
    console_puts(" buttons=");
    console_put_dec64((uint64_t)debug.buttons);
    console_puts(" dx=");
    console_put_signed_dec(debug.dx);
    console_puts(" dy=");
    console_put_signed_dec(debug.dy);
    console_puts(" wheel=");
    console_put_signed_dec(debug.wheel);
    console_puts(" pendingwheel=");
    console_put_signed_dec(debug.pending_wheel);
    console_puts("\n");
}

static void cmd_usb(const char *args, const boot_info_t *info) {
    char *mutable_args = (char *)args;
    char *command = 0;
    char *index_text = 0;
    char *extra = 0;
    uint64_t index = 0;

    (void)info;

    split_first_arg(mutable_args, &command, &index_text);
    if (*command != '\0') {
        split_first_arg(index_text, &index_text, &extra);
        if ((!streq(command, "init") &&
             !streq(command, "handoff") &&
             !streq(command, "halt") &&
             !streq(command, "reset") &&
             !streq(command, "rings") &&
             !streq(command, "bm") &&
             !streq(command, "nobm") &&
             !streq(command, "run") &&
             !streq(command, "poke") &&
             !streq(command, "pokenodma") &&
             !streq(command, "status") &&
             !streq(command, "start") &&
             !streq(command, "scan") &&
             !streq(command, "enum")) ||
            *index_text == '\0' ||
            *extra != '\0' ||
            parse_u64_arg(index_text, &index) != 0) {
            console_puts("usage: usb [scan|init|handoff|halt|reset|rings|bm|nobm|poke|pokenodma|run|status|start|enum] index\n");
            return;
        }

        if (streq(command, "scan")) {
            console_puts("usb: scanning xHCI ports on controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_scan_ports((uint32_t)index) != 0) {
                console_puts("usb: xHCI port scan failed\n");
            }
        } else if (streq(command, "init")) {
            console_puts("usb: probing xHCI controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_init_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI probe failed\n");
            } else {
                console_puts("usb: xHCI probe ok\n");
            }
        } else if (streq(command, "handoff")) {
            console_puts("usb: requesting xHCI BIOS handoff for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_handoff_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI handoff failed\n");
            } else {
                console_puts("usb: xHCI handoff ok\n");
            }
        } else if (streq(command, "halt")) {
            console_puts("usb: halting xHCI controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_halt_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI halt failed or timed out\n");
            } else {
                console_puts("usb: xHCI halt ok\n");
            }
        } else if (streq(command, "reset")) {
            console_puts("usb: resetting xHCI controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_reset_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI reset failed or timed out\n");
            } else {
                console_puts("usb: xHCI reset ok\n");
            }
        } else if (streq(command, "rings")) {
            console_puts("usb: setting up xHCI rings for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_setup_rings((uint32_t)index) != 0) {
                console_puts("usb: xHCI ring setup failed\n");
            } else {
                console_puts("usb: xHCI ring setup ok\n");
            }
        } else if (streq(command, "bm")) {
            console_puts("usb: enabling xHCI bus mastering for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_busmaster_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI bus mastering failed\n");
            } else {
                console_puts("usb: xHCI bus mastering ok\n");
            }
        } else if (streq(command, "nobm")) {
            console_puts("usb: disabling xHCI bus mastering for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_no_busmaster_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI bus mastering disable failed\n");
            } else {
                console_puts("usb: xHCI bus mastering disabled\n");
            }
        } else if (streq(command, "run")) {
            console_puts("usb: setting xHCI run bit for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_run_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI run failed or timed out\n");
            } else {
                console_puts("usb: xHCI run ok\n");
            }
        } else if (streq(command, "poke")) {
            console_puts("usb: poking xHCI run bit for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_poke_run_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI run poke failed\n");
            } else {
                console_puts("usb: xHCI run poke returned\n");
            }
        } else if (streq(command, "pokenodma")) {
            console_puts("usb: poking xHCI run bit without bus mastering for controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_poke_no_dma_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI no-DMA run poke failed\n");
            } else {
                console_puts("usb: xHCI no-DMA run poke returned\n");
            }
        } else if (streq(command, "status")) {
            uint32_t usbcmd = 0;
            uint32_t usbsts = 0;
            if (usb_xhci_status_controller((uint32_t)index, &usbcmd, &usbsts) != 0) {
                console_puts("usb: xHCI status failed\n");
            } else {
                console_puts("usb: USBCMD=0x");
                console_put_hex32(usbcmd);
                console_puts(" USBSTS=0x");
                console_put_hex32(usbsts);
                console_puts("\n");
            }
        } else if (streq(command, "start")) {
            console_puts("usb: starting xHCI controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_start_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI start failed or timed out\n");
            } else {
                console_puts("usb: xHCI start ok\n");
            }
        } else {
            console_puts("usb: enumerating xHCI ports on controller ");
            console_put_dec64(index);
            console_puts("\n");
            if (usb_xhci_enumerate_controller((uint32_t)index) != 0) {
                console_puts("usb: xHCI enumeration failed or timed out\n");
            } else {
                console_puts("usb: xHCI enumeration returned\n");
            }
        }
    }

    {
        uint32_t before_reports = 0;

        for (uint32_t i = 0; i < usb_controller_count(); ++i) {
            const usb_controller_info_t *ctrl = usb_controller_info(i);
            if (ctrl != 0) {
                before_reports += ctrl->mouse_report_count;
            }
        }

        for (uint32_t poll = 0; poll < 100000u; ++poll) {
            uint32_t after_reports = 0;

            usb_poll();
            for (uint32_t i = 0; i < usb_controller_count(); ++i) {
                const usb_controller_info_t *ctrl = usb_controller_info(i);
                if (ctrl != 0) {
                    after_reports += ctrl->mouse_report_count;
                }
            }
            if (after_reports != before_reports) {
                break;
            }
            __asm__ __volatile__("pause");
        }
    }

    console_puts("USB host controllers=");
    console_put_dec64(usb_controller_count());
    console_puts(" xhci=");
    console_put_dec64(usb_xhci_controller_count());
    console_puts("\n");

    for (uint32_t i = 0; i < usb_controller_count(); ++i) {
        const usb_controller_info_t *ctrl = usb_controller_info(i);
        if (ctrl == 0) {
            continue;
        }

        console_puts("  ");
        console_puts(usb_controller_type_name(ctrl->type));
        console_puts(" pci=");
        console_put_dec64(ctrl->bus);
        console_puts(":");
        console_put_dec64(ctrl->device);
        console_puts(".");
        console_put_dec64(ctrl->function);
        console_puts(" vendor=0x");
        console_put_hex64(ctrl->vendor_id);
        console_puts(" device=0x");
        console_put_hex64(ctrl->device_id);
        console_puts(" bar0=0x");
        console_put_hex64(ctrl->bar0);
        if (ctrl->bar0_is_io) {
            console_puts(" io");
        }
        if (ctrl->type == USB_CONTROLLER_XHCI) {
            console_puts(" hci=0x");
            console_put_hex64(ctrl->hci_version);
            console_puts(" slots=");
            console_put_dec64(ctrl->max_slots);
            console_puts(" intrs=");
            console_put_dec64(ctrl->interrupter_count);
            console_puts(" ports=");
            console_put_dec64(ctrl->port_count);
            console_puts(" scratch=");
            console_put_dec64(ctrl->scratchpad_count);
            console_puts(" page=");
            console_put_dec64(ctrl->page_size);
            console_puts(" hcs2=0x");
            console_put_hex32(ctrl->hcsparams2);
            console_puts(" hcc1=0x");
            console_put_hex32(ctrl->hccparams1);
            console_puts(" init=");
            console_put_dec64(ctrl->initialized);
            console_puts(" run=");
            console_put_dec64(ctrl->running);
            console_puts(" connected=");
            console_put_dec64(ctrl->connected_port_count);
            console_puts(" reset=");
            console_put_dec64(ctrl->reset_port_count);
            console_puts(" slots-enabled=");
            console_put_dec64(ctrl->enabled_slot_count);
            console_puts(" addressed=");
            console_put_dec64(ctrl->addressed_device_count);
            console_puts(" desc=");
            console_put_dec64(ctrl->descriptor_count);
            console_puts(" cc=");
            console_put_dec64(ctrl->last_completion_code);
            console_puts(" mouse=");
            console_put_dec64(ctrl->mouse_configured);
            console_puts(" mslot=");
            console_put_dec64(ctrl->mouse_slot);
            console_puts(" mdci=");
            console_put_dec64(ctrl->mouse_dci);
            console_puts(" msize=");
            console_put_dec64(ctrl->mouse_report_size);
            console_puts(" mpending=");
            console_put_dec64(ctrl->mouse_pending);
            console_puts(" mreps=");
            console_put_dec64(ctrl->mouse_report_count);
            console_puts(" mcc=");
            console_put_dec64(ctrl->mouse_last_completion_code);
            console_puts(" mstage=");
            console_put_dec64(ctrl->mouse_stage);
            console_puts(" mif=");
            console_put_dec64(ctrl->mouse_interface);
            console_puts(" mep=0x");
            console_put_hex32(ctrl->mouse_endpoint);
            console_puts(" mint=");
            console_put_dec64(ctrl->mouse_interval);
            console_puts(" mproto=");
            console_put_dec64(ctrl->mouse_protocol);
            console_puts(" mhid=");
            console_put_dec64(ctrl->mouse_hid_report_size);
            console_puts(" mparsed=");
            console_put_dec64(ctrl->mouse_report_parsed);
            console_puts(" mrid=");
            console_put_dec64(ctrl->mouse_report_id);
            console_puts(" mbits=");
            console_put_dec64(ctrl->mouse_buttons_bit);
            console_puts("/");
            console_put_dec64(ctrl->mouse_x_bit);
            console_puts("/");
            console_put_dec64(ctrl->mouse_y_bit);
            console_puts("/");
            console_put_dec64(ctrl->mouse_wheel_bit);
            console_puts(" msz=");
            console_put_dec64(ctrl->mouse_axis_size);
            console_puts("/");
            console_put_dec64(ctrl->mouse_wheel_size);
            console_puts(" mxfer=");
            console_put_dec64(ctrl->mouse_last_transferred);
            console_puts(" mwheel=");
            if (ctrl->mouse_last_wheel < 0) {
                console_puts("-");
                console_put_dec64((uint64_t)(-ctrl->mouse_last_wheel));
            } else {
                console_put_dec64((uint64_t)ctrl->mouse_last_wheel);
            }
            console_puts(" mrep=");
            console_put_hex32(ctrl->mouse_last_report0);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_report1);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_report2);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_report3);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_report4);
            console_puts(" mnzxfer=");
            console_put_dec64(ctrl->mouse_last_nonzero_transferred);
            console_puts(" mnzwheel=");
            if (ctrl->mouse_last_nonzero_wheel < 0) {
                console_puts("-");
                console_put_dec64((uint64_t)(-ctrl->mouse_last_nonzero_wheel));
            } else {
                console_put_dec64((uint64_t)ctrl->mouse_last_nonzero_wheel);
            }
            console_puts(" mnzrep=");
            console_put_hex32(ctrl->mouse_last_nonzero_report0);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_nonzero_report1);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_nonzero_report2);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_nonzero_report3);
            console_puts(",");
            console_put_hex32(ctrl->mouse_last_nonzero_report4);
            console_puts(" estage=");
            console_put_dec64(ctrl->enum_stage);
            console_puts(" eport=");
            console_put_dec64(ctrl->enum_port);
            console_puts(" eportsc=0x");
            console_put_hex32(ctrl->enum_portsc);
            console_puts(" espd=");
            console_put_dec64(ctrl->enum_speed);
            console_puts(" eslot=");
            console_put_dec64(ctrl->enum_slot);
            console_puts(" ecc=");
            console_put_dec64(ctrl->enum_completion_code);
            console_puts("\n    dma dcbaa=0x");
            console_put_hex64(ctrl->dcbaa_phys);
            console_puts(" cr=0x");
            console_put_hex64(ctrl->command_ring_phys);
            console_puts(" er=0x");
            console_put_hex64(ctrl->event_ring_phys);
            console_puts(" erst=0x");
            console_put_hex64(ctrl->erst_phys);
        }
        console_puts("\n");
    }
}

static void cmd_ticks(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    console_puts("ticks=");
    console_put_dec64(timer_ticks());
    console_puts(" hz=");
    console_put_dec64(timer_frequency());
    console_puts("\n");
}

static int run_script_file(const char *name, const boot_info_t *info, int quiet_missing) {
    char *script = 0;
    uint32_t size = 0;
    int drive = active_drive();

    if (drive < 0) {
        return -1;
    }

    if (script_depth >= SCRIPT_MAX_DEPTH) {
        console_puts("run failed: script nesting too deep\n");
        return -2;
    }

    script = script_buffers[script_depth];

    int status = lainfs_load_file_in_dir((char)('A' + drive),
                                         active_dir(),
                                         name,
                                         script,
                                         SCRIPT_BUFFER_SIZE,
                                         &size);
    if (status == -3) {
        if (!quiet_missing) {
            console_puts("drive is not formatted as lainfs\n");
        }

        return status;
    }

    if (status == -5) {
        if (!quiet_missing) {
            console_puts("script not found\n");
        }

        return status;
    }

    if (status != 0) {
        console_puts("run failed\n");
        return status;
    }

    script[size] = '\0';

    ++script_depth;
    run_script_text(script, size, info);
    --script_depth;

    return 0;
}

static void cmd_run(const char *args, const boot_info_t *info) {
    const char *name = skip_const_spaces(args);

    if (active_drive() < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: run scriptname\n");
        return;
    }

    run_script_file(name, info, 0);
}

static void cmd_exec(const char *args, const boot_info_t *info) {
    (void)info;

    const char *name = skip_const_spaces(args);
    uint32_t size = 0;
    int drive = active_drive();

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: exec file.bin\n");
        return;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);

    int status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         name,
                                         (char *)exec_buffer,
                                         EXEC_BUFFER_SIZE,
                                         &size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }

    if (status == -5) {
        console_puts("binary not found\n");
        return;
    }

    if (status != 0) {
        console_puts("exec failed\n");
        return;
    }

    if (size == 0) {
        console_puts("exec failed: empty binary\n");
        return;
    }

    console_puts("running ");
    console_puts(name);
    console_puts("\n");

    exec_program_t program = (exec_program_t)(uintptr_t)exec_buffer;
    program(&api);

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);

    console_puts("\nprogram returned\n");
}

static void cmd_asm(const char *args, const boot_info_t *info) {
    (void)info;

    char *source = shell_source_buffer;
    uint32_t source_size = 0;
    uint32_t output_size = 0;
    uint32_t error_line = 0;
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: asm source.asm output.bin\n");
        return;
    }

    int status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         source_name,
                                         source,
                                         ASM_SOURCE_SIZE,
                                         &source_size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }

    if (status == -5) {
        console_puts("source not found\n");
        return;
    }

    if (status != 0) {
        console_puts("asm failed: could not load source\n");
        return;
    }

    source[source_size] = '\0';
    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (assembler_assemble_source_ex(source,
                                     source_size,
                                     asm_output,
                                     EXEC_BUFFER_SIZE,
                                     (uint64_t)(uintptr_t)exec_buffer,
                                     &output_size,
                                     &error_line) != 0) {
        console_puts("asm failed: unsupported syntax");
        if (error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)asm_output,
                                     output_size);
    if (status == -9) {
        console_puts("asm failed: disk is full\n");
        return;
    }

    if (status != 0) {
        console_puts("asm failed: could not save output\n");
        return;
    }

    console_puts("assembled ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static int compile_z_source_file(const char *name,
                                 char *source,
                                 uint32_t *source_size,
                                 uint32_t *compile_error_line,
                                 uint32_t *asm_size,
                                 int *drive_out) {
    int drive = active_drive();
    int status;

    if (drive_out) {
        *drive_out = drive;
    }

    if (drive < 0) {
        return -10;
    }

    status = load_z_source_expanded((char)('A' + drive),
                                    cwd_dirs[drive],
                                    0,
                                    0,
                                    name,
                                    source,
                                    ASM_SOURCE_SIZE,
                                    source_size);
    if (status != 0) {
        return status;
    }

    zero_memory(zscript_output, ASM_SOURCE_SIZE + 1u);

    if (zscript_compile_source(source,
                               *source_size,
                               zscript_output,
                               ASM_SOURCE_SIZE,
                               asm_size,
                               compile_error_line) != 0) {
        return -20;
    }

    zscript_output[*asm_size] = '\0';
    return 0;
}

static void cmd_zc(const char *args, const boot_info_t *info) {
    (void)info;

    char *source = shell_source_buffer;
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t output_size = 0;
    uint32_t compile_error_line = 0;
    uint32_t assemble_error_line = 0;
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: zc source.Z output.bin\n");
        return;
    }

    status = compile_z_source_file(source_name,
                                   source,
                                   &source_size,
                                   &compile_error_line,
                                   &asm_size,
                                   &drive);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -20) {
        console_puts("zc failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    if (status == -30) {
        console_puts("zc failed: include nesting is too deep\n");
        return;
    }
    if (status == -31) {
        console_puts("zc failed: malformed include line\n");
        return;
    }
    if (status == -32) {
        console_puts("zc failed: expanded source is too large\n");
        return;
    }
    if (status == -33) {
        console_puts("zc failed: too many pragma once headers\n");
        return;
    }
    if (status != 0) {
        console_puts("zc failed: could not load source\n");
        return;
    }

    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (assembler_assemble_source_ex(zscript_output,
                                     asm_size,
                                     asm_output,
                                     EXEC_BUFFER_SIZE,
                                     (uint64_t)(uintptr_t)exec_buffer,
                                     &output_size,
                                     &assemble_error_line) != 0) {
        console_puts("zc failed: compiler emitted unsupported asm");
        if (assemble_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(assemble_error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)asm_output,
                                     output_size);
    if (status == -9) {
        console_puts("zc failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zc failed: could not save output\n");
        return;
    }

    console_puts("compiled ");
    console_puts(source_name);
    console_puts(" to ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static void cmd_zco(const char *args, const boot_info_t *info) {
    (void)info;

    char *source = shell_source_buffer;
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t object_size = 0;
    uint32_t compile_error_line = 0;
    int zobject_status = 0;
    char entry_label[32];
    char label_prefix[8];
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: zco source.Z output.zo\n");
        return;
    }

    status = load_z_source_expanded((char)('A' + drive),
                                    cwd_dirs[drive],
                                    0,
                                    0,
                                    source_name,
                                    source,
                                    ASM_SOURCE_SIZE,
                                    &source_size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -30) {
        console_puts("zco failed: include nesting is too deep\n");
        return;
    }
    if (status == -31) {
        console_puts("zco failed: malformed include line\n");
        return;
    }
    if (status == -32) {
        console_puts("zco failed: expanded source is too large\n");
        return;
    }
    if (status == -33) {
        console_puts("zco failed: too many pragma once headers\n");
        return;
    }
    if (status != 0) {
        console_puts("zco failed: could not load source\n");
        return;
    }

    zero_memory(zscript_output, ASM_SOURCE_SIZE + 1u);
    make_zobject_prefix(output_name, label_prefix, sizeof(label_prefix));
    if (zscript_compile_source_object(source,
                                      source_size,
                                      label_prefix,
                                      zscript_output,
                                      ASM_SOURCE_SIZE,
                                      &asm_size,
                                      &compile_error_line,
                                      entry_label,
                                      sizeof(entry_label)) != 0) {
        console_puts("zco failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    zscript_output[asm_size] = '\0';

    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    zobject_status = zobject_from_asm(zscript_output,
                                      asm_size,
                                      entry_label,
                                      asm_output,
                                      EXEC_BUFFER_SIZE,
                                      &object_size);
    if (zobject_status != 0) {
        if (zobject_status <= -3000) {
            console_puts("zco failed: generated asm failed on line ");
            console_put_dec64((uint32_t)(-zobject_status - 3000));
            console_puts("\n");
        } else if (zobject_status == -60 || zobject_status == -30) {
            console_puts("zco failed: object is too large\n");
        } else if (zobject_status == -21) {
            console_puts("zco failed: generated asm exceeded assembler limits\n");
        } else {
            console_puts("zco failed: could not create object\n");
        }
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)asm_output,
                                     object_size);
    if (status == -9) {
        console_puts("zco failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zco failed: could not save output\n");
        return;
    }

    console_puts("compiled object ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(object_size);
    console_puts("\n");
}

static void cmd_zlink(const char *args, const boot_info_t *info) {
    (void)info;

    const unsigned char *objects[ZLINK_MAX_OBJECTS];
    uint32_t object_sizes[ZLINK_MAX_OBJECTS];
    uint32_t object_count = 0;
    uint32_t object_offset = 0;
    uint32_t output_size = 0;
    uint32_t error_line = 0;
    char *mutable_args = (char *)args;
    char *tokens[ZLINK_MAX_OBJECTS + 1u];
    uint32_t token_count = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    mutable_args = skip_spaces(mutable_args);
    while (*mutable_args != '\0') {
        if (token_count >= ZLINK_MAX_OBJECTS + 1u) {
            console_puts("usage: zlink input.zo [more.zo ...] output.bin\n");
            return;
        }

        tokens[token_count++] = mutable_args;
        while (*mutable_args && *mutable_args != ' ' && *mutable_args != '\t') {
            ++mutable_args;
        }
        if (*mutable_args != '\0') {
            *mutable_args++ = '\0';
            mutable_args = skip_spaces(mutable_args);
        }
    }

    if (token_count < 2u) {
        console_puts("usage: zlink input.zo [more.zo ...] output.bin\n");
        return;
    }

    output_name = tokens[token_count - 1u];
    zero_memory(asm_output, EXEC_BUFFER_SIZE);

    for (uint32_t i = 0; i + 1u < token_count; ++i) {
        uint32_t remaining = EXEC_BUFFER_SIZE - object_offset;

        objects[object_count] = asm_output + object_offset;
        status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         tokens[i],
                                         (char *)(asm_output + object_offset),
                                         remaining,
                                         &object_sizes[object_count]);
        if (status == -3) {
            console_puts("drive is not formatted as lainfs\n");
            return;
        }
        if (status == -5) {
            console_puts(".zo object not found: ");
            console_puts(tokens[i]);
            console_puts("\n");
            return;
        }
        if (status != 0) {
            console_puts("zlink failed: could not load object\n");
            return;
        }

        object_offset += object_sizes[object_count];
        ++object_count;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (zobject_link_flat_many(objects,
                               object_sizes,
                               object_count,
                               exec_buffer,
                               EXEC_BUFFER_SIZE,
                               (uint64_t)(uintptr_t)exec_buffer,
                               &output_size,
                               &error_line) != 0) {
        console_puts("zlink failed: unsupported object");
        if (error_line != 0) {
            console_puts(" asm line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)exec_buffer,
                                     output_size);
    if (status == -9) {
        console_puts("zlink failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zlink failed: could not save output\n");
        return;
    }

    console_puts("linked ");
    console_put_dec64(object_count);
    console_puts(" object(s)");
    console_puts(" to ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static void cmd_zbuild(const char *args, const boot_info_t *info) {
    (void)info;

    char *manifest = shell_manifest_buffer;
    char *source = shell_source_buffer;
    const unsigned char *objects[ZLINK_MAX_OBJECTS];
    uint32_t object_sizes[ZLINK_MAX_OBJECTS];
    uint32_t object_count = 0;
    uint32_t object_offset = 0;
    uint32_t manifest_size = 0;
    uint32_t output_size = 0;
    uint32_t link_error_line = 0;
    int zobject_status = 0;
    uint32_t source_dir = 0;
    uint32_t build_dir = 0;
    uint32_t include_dirs[Z_INCLUDE_MAX_DIRS];
    uint32_t include_dir_count = 0;
    char *mutable_args = (char *)args;
    char *target_name = 0;
    char *extra = 0;
    char manifest_name[32];
    char output_name[32];
    char report_name[32];
    uint32_t report_size = 0;
    int link_output = 1;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }
    source_dir = cwd_dirs[drive];
    build_dir = cwd_dirs[drive];

    split_first_arg(mutable_args, &target_name, &extra);
    if (*target_name == '\0' || *extra != '\0') {
        console_puts("usage: zbuild target\n");
        console_puts("reads target.zbuild; lines are: source.Z [output.zo]\n");
        return;
    }

    if (make_suffixed_name(target_name, ".zbuild", manifest_name, sizeof(manifest_name)) != 0 ||
        make_suffixed_name(target_name, ".bin", output_name, sizeof(output_name)) != 0 ||
        make_suffixed_name(target_name, ".buildlog", report_name, sizeof(report_name)) != 0) {
        console_puts("zbuild failed: target name is too long\n");
        return;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     manifest_name,
                                     manifest,
                                     ASM_SOURCE_SIZE,
                                     &manifest_size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts("zbuild failed: manifest not found: ");
        console_puts(manifest_name);
        console_puts("\n");
        return;
    }
    if (status != 0) {
        console_puts("zbuild failed: could not load manifest\n");
        return;
    }

    manifest[manifest_size] = '\0';
    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    include_dirs[include_dir_count++] = cwd_dirs[drive];

    for (uint32_t pos = 0, line = 1; pos < manifest_size;) {
        char *line_start = manifest + pos;
        char *line_text;
        char *source_name;
        char *object_name;
        char object_name_buffer[32];
        uint32_t source_size = 0;
        uint32_t asm_size = 0;
        uint32_t object_size = 0;
        uint32_t compile_error_line = 0;
        char entry_label[32];
        char label_prefix[8];

        while (pos < manifest_size && manifest[pos] != '\n' && manifest[pos] != '\r') {
            ++pos;
        }
        if (pos < manifest_size) {
            manifest[pos++] = '\0';
            if (pos < manifest_size && manifest[pos - 1u] == '\r' && manifest[pos] == '\n') {
                manifest[pos++] = '\0';
            }
        }

        line_text = skip_spaces(line_start);
        if (*line_text == '\0' ||
            *line_text == '#' ||
            *line_text == ';' ||
            (line_text[0] == '/' && line_text[1] == '/')) {
            ++line;
            continue;
        }

        split_first_arg(line_text, &source_name, &object_name);
        if (streq(source_name, "src") || streq(source_name, "source")) {
            char *dir_name = 0;
            char *unused = 0;
            split_first_arg(object_name, &dir_name, &unused);
            if (*dir_name == '\0' || *unused != '\0' ||
                resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_name, &source_dir) != 0) {
                console_puts("zbuild failed: bad src directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            ++line;
            continue;
        }
        if (streq(source_name, "build")) {
            char *dir_name = 0;
            char *unused = 0;
            split_first_arg(object_name, &dir_name, &unused);
            if (*dir_name == '\0' || *unused != '\0' ||
                resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_name, &build_dir) != 0) {
                console_puts("zbuild failed: bad build directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            ++line;
            continue;
        }
        if (streq(source_name, "include")) {
            char *dir_name = 0;
            char *unused = 0;
            uint32_t include_dir = 0;
            split_first_arg(object_name, &dir_name, &unused);
            if (*dir_name == '\0' || *unused != '\0' ||
                include_dir_count >= Z_INCLUDE_MAX_DIRS ||
                resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_name, &include_dir) != 0) {
                console_puts("zbuild failed: bad include directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            include_dirs[include_dir_count++] = include_dir;
            ++line;
            continue;
        }
        if (streq(source_name, "output")) {
            char *directive_output = 0;
            char *unused = 0;
            split_first_arg(object_name, &directive_output, &unused);
            if (*directive_output == '\0' || *unused != '\0') {
                console_puts("zbuild failed: bad output directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            copy_text_limited(output_name, sizeof(output_name), directive_output);
            ++line;
            continue;
        }
        if (streq(source_name, "module") || streq(source_name, "objects-only")) {
            char *unused = 0;
            char *first = 0;
            split_first_arg(object_name, &first, &unused);
            if (*first != '\0' || *unused != '\0') {
                console_puts("zbuild failed: bad module directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            link_output = 0;
            ++line;
            continue;
        }
        if (streq(source_name, "test-return") ||
            streq(source_name, "install") ||
            streq(source_name, "install-name")) {
            ++line;
            continue;
        }

        if (*object_name == '\0') {
            if (make_object_name_from_source(source_name,
                                             object_name_buffer,
                                             sizeof(object_name_buffer)) != 0) {
                console_puts("zbuild failed: bad source name on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            object_name = object_name_buffer;
        } else {
            char *unused = 0;
            char *first_object_name = object_name;
            split_first_arg(object_name, &first_object_name, &unused);
            if (*unused != '\0') {
                console_puts("zbuild failed: too many fields on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            object_name = first_object_name;
        }

        if (object_count >= ZLINK_MAX_OBJECTS) {
            console_puts("zbuild failed: too many objects; max=");
            console_put_dec64(ZLINK_MAX_OBJECTS);
            console_puts("\n");
            return;
        }

        status = load_z_source_expanded((char)('A' + drive),
                                        source_dir,
                                        include_dirs,
                                        include_dir_count,
                                        source_name,
                                        source,
                                        ASM_SOURCE_SIZE,
                                        &source_size);
        if (status == -5) {
            console_puts("zbuild failed: source not found on line ");
            console_put_dec64(line);
            console_puts(": ");
            console_puts(source_name);
            console_puts("\n");
            return;
        }
        if (status == -30) {
            console_puts("zbuild failed: include nesting is too deep on line ");
            console_put_dec64(line);
            console_puts("\n");
            return;
        }
        if (status == -31) {
            console_puts("zbuild failed: malformed include while loading line ");
            console_put_dec64(line);
            console_puts("\n");
            return;
        }
        if (status == -32) {
            console_puts("zbuild failed: expanded source is too large on line ");
            console_put_dec64(line);
            console_puts("\n");
            return;
        }
        if (status == -33) {
            console_puts("zbuild failed: too many pragma once headers on line ");
            console_put_dec64(line);
            console_puts("\n");
            return;
        }
        if (status != 0) {
            console_puts("zbuild failed: could not load source on line ");
            console_put_dec64(line);
            console_puts("\n");
            return;
        }

        zero_memory(zscript_output, ASM_SOURCE_SIZE + 1u);
        make_zobject_prefix(object_name, label_prefix, sizeof(label_prefix));
        if (zscript_compile_source_object(source,
                                          source_size,
                                          label_prefix,
                                          zscript_output,
                                          ASM_SOURCE_SIZE,
                                          &asm_size,
                                          &compile_error_line,
                                          entry_label,
                                          sizeof(entry_label)) != 0) {
            console_puts("zbuild failed: ");
            console_puts(source_name);
            console_puts(" unsupported .Z syntax");
            if (compile_error_line != 0) {
                console_puts(" on line ");
                console_put_dec64(compile_error_line);
            }
            console_puts("\n");
            return;
        }
        zscript_output[asm_size] = '\0';

        zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
        zobject_status = zobject_from_asm(zscript_output,
                                          asm_size,
                                          entry_label,
                                          exec_buffer,
                                          EXEC_BUFFER_SIZE,
                                          &object_size);
        if (zobject_status != 0) {
            if (zobject_status <= -3000) {
                console_puts("zbuild failed: generated asm failed for ");
                console_puts(source_name);
                console_puts(" on asm line ");
                console_put_dec64((uint32_t)(-zobject_status - 3000));
                console_puts("\n");
            } else if (zobject_status == -60 || zobject_status == -30) {
                console_puts("zbuild failed: object too large for ");
                console_puts(source_name);
                console_puts("\n");
            } else if (zobject_status == -21) {
                console_puts("zbuild failed: generated asm exceeded assembler limits for ");
                console_puts(source_name);
                console_puts("\n");
            } else {
                console_puts("zbuild failed: could not create object for ");
                console_puts(source_name);
                console_puts("\n");
            }
            return;
        }

        status = lainfs_save_file_in_dir((char)('A' + drive),
                                         build_dir,
                                         object_name,
                                         (const char *)exec_buffer,
                                         object_size);
        if (status == -9) {
            console_puts("zbuild failed: disk is full while saving ");
            console_puts(object_name);
            console_puts("\n");
            return;
        }
        if (status != 0) {
            console_puts("zbuild failed: could not save ");
            console_puts(object_name);
            console_puts("\n");
            return;
        }

        if (object_size > EXEC_BUFFER_SIZE ||
            object_offset > EXEC_BUFFER_SIZE - object_size) {
            console_puts("zbuild failed: object set is too large\n");
            return;
        }

        objects[object_count] = asm_output + object_offset;
        object_sizes[object_count] = object_size;
        copy_bytes(asm_output + object_offset, exec_buffer, object_size);
        object_offset += object_size;
        ++object_count;

        console_puts("zbuild: ");
        console_puts(source_name);
        console_puts(" -> ");
        console_puts(object_name);
        console_puts(" bytes=");
        console_put_dec64(object_size);
        console_puts("\n");
        ++line;
    }

    if (object_count == 0) {
        console_puts("zbuild failed: manifest has no sources\n");
        return;
    }

    if (!link_output) {
        zbuild_report[0] = '\0';
        if (append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "target ") == 0 &&
            append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, target_name) == 0 &&
            append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "\nobjects ") == 0 &&
            append_dec_limited(zbuild_report, sizeof(zbuild_report), &report_size, object_count) == 0 &&
            append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "\nstatus module\n") == 0) {
            status = lainfs_save_file_in_dir((char)('A' + drive),
                                             build_dir,
                                             report_name,
                                             zbuild_report,
                                             report_size);
            if (status != 0) {
                console_puts("zbuild warning: could not save build log\n");
            }
        }

        console_puts("zbuild: built ");
        console_put_dec64(object_count);
        console_puts(" module object(s)\n");
        return;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (zobject_link_flat_many(objects,
                               object_sizes,
                               object_count,
                               exec_buffer,
                               EXEC_BUFFER_SIZE,
                               (uint64_t)(uintptr_t)exec_buffer,
                               &output_size,
                               &link_error_line) != 0) {
        console_puts("zbuild failed: link failed");
        if (link_error_line != 0) {
            console_puts(" asm line ");
            console_put_dec64(link_error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     build_dir,
                                     output_name,
                                     (const char *)exec_buffer,
                                     output_size);
    if (status == -9) {
        console_puts("zbuild failed: disk is full while saving output\n");
        return;
    }
    if (status != 0) {
        console_puts("zbuild failed: could not save output\n");
        return;
    }

    zbuild_report[0] = '\0';
    if (append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "target ") != 0 ||
        append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, target_name) != 0 ||
        append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "\nobjects ") != 0 ||
        append_dec_limited(zbuild_report, sizeof(zbuild_report), &report_size, object_count) != 0 ||
        append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "\noutput ") != 0 ||
        append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, output_name) != 0 ||
        append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "\nbytes ") != 0 ||
        append_dec_limited(zbuild_report, sizeof(zbuild_report), &report_size, output_size) != 0 ||
        append_text_limited(zbuild_report, sizeof(zbuild_report), &report_size, "\nstatus ok\n") != 0) {
        report_size = 0;
    }
    if (report_size != 0) {
        status = lainfs_save_file_in_dir((char)('A' + drive),
                                         build_dir,
                                         report_name,
                                         zbuild_report,
                                         report_size);
        if (status != 0) {
            console_puts("zbuild warning: could not save build log\n");
        }
    }

    console_puts("zbuild: linked ");
    console_put_dec64(object_count);
    console_puts(" object(s) to ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static void cmd_zclean(const char *args, const boot_info_t *info) {
    (void)info;

    char *manifest = shell_manifest_buffer;
    uint32_t manifest_size = 0;
    uint32_t build_dir = 0;
    uint32_t removed_count = 0;
    char *mutable_args = (char *)args;
    char *target_name = 0;
    char *extra = 0;
    char manifest_name[32];
    char output_name[32];
    char report_name[32];
    char testlog_name[32];
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }
    build_dir = cwd_dirs[drive];

    split_first_arg(mutable_args, &target_name, &extra);
    if (*target_name == '\0' || *extra != '\0') {
        console_puts("usage: zclean target\n");
        return;
    }

    if (make_suffixed_name(target_name, ".zbuild", manifest_name, sizeof(manifest_name)) != 0 ||
        make_suffixed_name(target_name, ".bin", output_name, sizeof(output_name)) != 0 ||
        make_suffixed_name(target_name, ".buildlog", report_name, sizeof(report_name)) != 0 ||
        make_suffixed_name(target_name, ".testlog", testlog_name, sizeof(testlog_name)) != 0) {
        console_puts("zclean failed: target name is too long\n");
        return;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     manifest_name,
                                     manifest,
                                     ASM_SOURCE_SIZE,
                                     &manifest_size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts("zclean failed: manifest not found: ");
        console_puts(manifest_name);
        console_puts("\n");
        return;
    }
    if (status != 0) {
        console_puts("zclean failed: could not load manifest\n");
        return;
    }

    manifest[manifest_size] = '\0';

    for (uint32_t pos = 0, line = 1; pos < manifest_size;) {
        char *line_start = manifest + pos;
        char *line_text;
        char *source_name;
        char *object_name;
        char object_name_buffer[32];

        while (pos < manifest_size && manifest[pos] != '\n' && manifest[pos] != '\r') {
            ++pos;
        }
        if (pos < manifest_size) {
            manifest[pos++] = '\0';
            if (pos < manifest_size && manifest[pos - 1u] == '\r' && manifest[pos] == '\n') {
                manifest[pos++] = '\0';
            }
        }

        line_text = skip_spaces(line_start);
        if (*line_text == '\0' ||
            *line_text == '#' ||
            *line_text == ';' ||
            (line_text[0] == '/' && line_text[1] == '/')) {
            ++line;
            continue;
        }

        split_first_arg(line_text, &source_name, &object_name);
        if (streq(source_name, "build")) {
            char *dir_name = 0;
            char *unused = 0;
            split_first_arg(object_name, &dir_name, &unused);
            if (*dir_name == '\0' || *unused != '\0' ||
                resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_name, &build_dir) != 0) {
                console_puts("zclean failed: bad build directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            ++line;
            continue;
        }
        if (streq(source_name, "output")) {
            char *directive_output = 0;
            char *unused = 0;
            split_first_arg(object_name, &directive_output, &unused);
            if (*directive_output == '\0' || *unused != '\0') {
                console_puts("zclean failed: bad output directive on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            copy_text_limited(output_name, sizeof(output_name), directive_output);
            ++line;
            continue;
        }
        if (streq(source_name, "src") ||
            streq(source_name, "source") ||
            streq(source_name, "include") ||
            streq(source_name, "module") ||
            streq(source_name, "objects-only") ||
            streq(source_name, "test-return") ||
            streq(source_name, "install") ||
            streq(source_name, "install-name")) {
            ++line;
            continue;
        }

        if (*object_name == '\0') {
            if (make_object_name_from_source(source_name,
                                             object_name_buffer,
                                             sizeof(object_name_buffer)) != 0) {
                console_puts("zclean failed: bad source name on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            object_name = object_name_buffer;
        } else {
            char *unused = 0;
            char *first_object_name = object_name;
            split_first_arg(object_name, &first_object_name, &unused);
            if (*unused != '\0') {
                console_puts("zclean failed: too many fields on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            object_name = first_object_name;
        }

        status = lainfs_delete_in_dir((char)('A' + drive), build_dir, object_name);
        if (status == 0) {
            ++removed_count;
        } else if (status != -5) {
            console_puts("zclean failed: could not remove ");
            console_puts(object_name);
            console_puts("\n");
            return;
        }
        ++line;
    }

    status = lainfs_delete_in_dir((char)('A' + drive), build_dir, output_name);
    if (status == 0) {
        ++removed_count;
    } else if (status != -5) {
        console_puts("zclean failed: could not remove output\n");
        return;
    }

    status = lainfs_delete_in_dir((char)('A' + drive), build_dir, report_name);
    if (status == 0) {
        ++removed_count;
    } else if (status != -5) {
        console_puts("zclean failed: could not remove build log\n");
        return;
    }

    status = lainfs_delete_in_dir((char)('A' + drive), build_dir, testlog_name);
    if (status == 0) {
        ++removed_count;
    } else if (status != -5) {
        console_puts("zclean failed: could not remove test log\n");
        return;
    }

    console_puts("zclean: removed ");
    console_put_dec64(removed_count);
    console_puts(" artifact(s)\n");
}

static int zbuild_read_layout(int drive,
                              const char *target_name,
                              char *manifest,
                              uint32_t manifest_capacity,
                              uint32_t *build_dir,
                              char *output_name,
                              uint32_t output_name_size,
                              uint32_t *install_dir,
                              char *install_name,
                              uint32_t install_name_size,
                              uint64_t *expected_return,
                              int *has_expected_return,
                              int *objects_only,
                              uint32_t *object_count) {
    uint32_t manifest_size = 0;
    uint32_t source_count = 0;
    char manifest_name[32];
    char first_object_name[32];
    int link_output = 1;
    int status;

    if (make_suffixed_name(target_name, ".zbuild", manifest_name, sizeof(manifest_name)) != 0 ||
        make_suffixed_name(target_name, ".bin", output_name, output_name_size) != 0) {
        return -20;
    }

    *build_dir = cwd_dirs[drive];
    if (install_dir) {
        *install_dir = cwd_dirs[drive];
    }
    if (install_name && install_name_size != 0) {
        install_name[0] = '\0';
    }
    if (has_expected_return) {
        *has_expected_return = 0;
    }
    if (objects_only) {
        *objects_only = 0;
    }
    if (object_count) {
        *object_count = 0;
    }
    first_object_name[0] = '\0';
    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     manifest_name,
                                     manifest,
                                     manifest_capacity,
                                     &manifest_size);
    if (status != 0) {
        return status;
    }
    manifest[manifest_size] = '\0';

    for (uint32_t pos = 0; pos < manifest_size;) {
        char *line_start = manifest + pos;
        char *line_text;
        char *first;
        char *rest;

        while (pos < manifest_size && manifest[pos] != '\n' && manifest[pos] != '\r') {
            ++pos;
        }
        if (pos < manifest_size) {
            manifest[pos++] = '\0';
            if (pos < manifest_size && manifest[pos - 1u] == '\r' && manifest[pos] == '\n') {
                manifest[pos++] = '\0';
            }
        }

        line_text = skip_spaces(line_start);
        if (*line_text == '\0' ||
            *line_text == '#' ||
            *line_text == ';' ||
            (line_text[0] == '/' && line_text[1] == '/')) {
            continue;
        }

        split_first_arg(line_text, &first, &rest);
        if (streq(first, "src") ||
            streq(first, "source") ||
            streq(first, "include")) {
            continue;
        } else if (streq(first, "build")) {
            char *dir_name = 0;
            char *unused = 0;
            split_first_arg(rest, &dir_name, &unused);
            if (*dir_name == '\0' || *unused != '\0' ||
                resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_name, build_dir) != 0) {
                return -21;
            }
        } else if (streq(first, "output")) {
            char *name = 0;
            char *unused = 0;
            split_first_arg(rest, &name, &unused);
            if (*name == '\0' || *unused != '\0') {
                return -22;
            }
            copy_text_limited(output_name, output_name_size, name);
        } else if (streq(first, "install")) {
            char *dir_name = 0;
            char *unused = 0;
            split_first_arg(rest, &dir_name, &unused);
            if (*dir_name == '\0' || *unused != '\0' ||
                (install_dir &&
                 resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_name, install_dir) != 0)) {
                return -24;
            }
        } else if (streq(first, "install-name")) {
            char *name = 0;
            char *unused = 0;
            split_first_arg(rest, &name, &unused);
            if (*name == '\0' || *unused != '\0') {
                return -25;
            }
            if (install_name && install_name_size != 0) {
                copy_text_limited(install_name, install_name_size, name);
            }
        } else if (streq(first, "test-return")) {
            char *value_text = 0;
            char *unused = 0;
            uint64_t value = 0;
            split_first_arg(rest, &value_text, &unused);
            if (*value_text == '\0' || *unused != '\0' ||
                parse_u64_arg(value_text, &value) != 0) {
                return -23;
            }
            if (expected_return) {
                *expected_return = value;
            }
            if (has_expected_return) {
                *has_expected_return = 1;
            }
        } else if (streq(first, "module") || streq(first, "objects-only")) {
            char *unused = 0;
            char *directive_arg = 0;
            split_first_arg(rest, &directive_arg, &unused);
            if (*directive_arg != '\0' || *unused != '\0') {
                return -26;
            }
            link_output = 0;
        } else {
            char *object_name = rest;
            char object_name_buffer[32];

            if (*object_name == '\0') {
                if (make_object_name_from_source(first,
                                                 object_name_buffer,
                                                 sizeof(object_name_buffer)) != 0) {
                    return -27;
                }
                object_name = object_name_buffer;
            } else {
                char *unused = 0;
                char *first_object = object_name;
                split_first_arg(object_name, &first_object, &unused);
                if (*unused != '\0') {
                    return -28;
                }
                object_name = first_object;
            }

            ++source_count;
            if (source_count == 1) {
                copy_text_limited(first_object_name, sizeof(first_object_name), object_name);
            }
        }
    }

    if (objects_only) {
        *objects_only = !link_output;
    }
    if (object_count) {
        *object_count = source_count;
    }
    if (!link_output && source_count == 1) {
        copy_text_limited(output_name, output_name_size, first_object_name);
    }

    if (install_name && install_name_size != 0 && install_name[0] == '\0') {
        copy_text_limited(install_name, install_name_size, output_name);
    }

    return 0;
}

static void cmd_ztest(const char *args, const boot_info_t *info) {
    (void)info;

    char *manifest = shell_manifest_buffer;
    char *mutable_args = (char *)args;
    char *target_name = 0;
    char *extra = 0;
    char output_name[32];
    char testlog_name[32];
    uint32_t build_dir = 0;
    uint32_t output_size = 0;
    uint32_t report_size = 0;
    uint64_t result = 0;
    uint64_t expected_return = 0;
    int has_expected_return = 0;
    int objects_only = 0;
    int passed = 1;
    int drive = active_drive();
    int status;

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &target_name, &extra);
    if (*target_name == '\0' || *extra != '\0') {
        console_puts("usage: ztest target\n");
        return;
    }

    if (make_suffixed_name(target_name, ".testlog", testlog_name, sizeof(testlog_name)) != 0) {
        console_puts("ztest failed: target name is too long\n");
        return;
    }

    status = zbuild_read_layout(drive,
                                target_name,
                                manifest,
                                ASM_SOURCE_SIZE,
                                &build_dir,
                                output_name,
                                sizeof(output_name),
                                0,
                                0,
                                0,
                                &expected_return,
                                &has_expected_return,
                                &objects_only,
                                0);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts("ztest failed: manifest not found\n");
        return;
    }
    if (status != 0) {
        console_puts("ztest failed: bad manifest layout\n");
        return;
    }
    if (objects_only) {
        console_puts("ztest failed: module targets do not produce executables\n");
        return;
    }

    cmd_zclean(target_name, 0);
    cmd_zbuild(target_name, 0);

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     build_dir,
                                     output_name,
                                     (char *)exec_buffer,
                                     EXEC_BUFFER_SIZE,
                                     &output_size);
    if (status == -5) {
        console_puts("ztest failed: build did not produce output\n");
        return;
    }
    if (status != 0 || output_size == 0) {
        console_puts("ztest failed: could not load output\n");
        return;
    }

    console_puts("ztest: running ");
    console_puts(output_name);
    console_puts("\n");
    result = ((exec_program_ret_t)(uintptr_t)exec_buffer)(&api);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (has_expected_return && result != expected_return) {
        passed = 0;
    }

    ztest_report[0] = '\0';
    if (append_text_limited(ztest_report, sizeof(ztest_report), &report_size, "target ") == 0 &&
        append_text_limited(ztest_report, sizeof(ztest_report), &report_size, target_name) == 0 &&
        append_text_limited(ztest_report, sizeof(ztest_report), &report_size, "\noutput ") == 0 &&
        append_text_limited(ztest_report, sizeof(ztest_report), &report_size, output_name) == 0 &&
        append_text_limited(ztest_report, sizeof(ztest_report), &report_size, "\nresult ") == 0 &&
        append_dec_limited(ztest_report, sizeof(ztest_report), &report_size, result) == 0 &&
        (!has_expected_return ||
         (append_text_limited(ztest_report, sizeof(ztest_report), &report_size, "\nexpected ") == 0 &&
          append_dec_limited(ztest_report, sizeof(ztest_report), &report_size, expected_return) == 0)) &&
        append_text_limited(ztest_report, sizeof(ztest_report), &report_size, passed ? "\nstatus ok\n" : "\nstatus failed\n") == 0) {
        status = lainfs_save_file_in_dir((char)('A' + drive),
                                         build_dir,
                                         testlog_name,
                                         ztest_report,
                                         report_size);
        if (status != 0) {
            console_puts("ztest warning: could not save test log\n");
        }
    }

    console_puts("ztest: result=");
    console_put_dec64(result);
    if (has_expected_return) {
        console_puts(" expected=");
        console_put_dec64(expected_return);
    }
    console_puts(passed ? " ok\n" : " failed\n");
}

static void cmd_zinstall(const char *args, const boot_info_t *info) {
    (void)info;

    char *manifest = shell_manifest_buffer;
    char *mutable_args = (char *)args;
    char *target_name = 0;
    char *dest_path = 0;
    char *extra = 0;
    char output_name[32];
    char install_name[32];
    uint32_t build_dir = 0;
    uint32_t install_dir = 0;
    uint32_t output_size = 0;
    uint32_t object_count = 0;
    int objects_only = 0;
    int drive = active_drive();
    int status;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &target_name, &dest_path);
    split_first_arg(dest_path, &dest_path, &extra);
    if (*target_name == '\0' || *extra != '\0') {
        console_puts("usage: zinstall target [dest.bin]\n");
        return;
    }

    status = zbuild_read_layout(drive,
                                target_name,
                                manifest,
                                ASM_SOURCE_SIZE,
                                &build_dir,
                                output_name,
                                sizeof(output_name),
                                &install_dir,
                                install_name,
                                sizeof(install_name),
                                0,
                                0,
                                &objects_only,
                                &object_count);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts("zinstall failed: manifest not found\n");
        return;
    }
    if (status != 0) {
        console_puts("zinstall failed: bad manifest layout\n");
        return;
    }
    if (objects_only && object_count != 1) {
        console_puts("zinstall failed: module target has multiple objects\n");
        return;
    }

    if (*dest_path != '\0' &&
        resolve_file_path((char)('A' + drive),
                          cwd_dirs[drive],
                          dest_path,
                          &install_dir,
                          install_name,
                          sizeof(install_name)) != 0) {
        console_puts("zinstall failed: bad install path\n");
        return;
    }

    cmd_zclean(target_name, 0);
    cmd_zbuild(target_name, 0);

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     build_dir,
                                     output_name,
                                     (char *)exec_buffer,
                                     EXEC_BUFFER_SIZE,
                                     &output_size);
    if (status == -5) {
        console_puts("zinstall failed: build did not produce output\n");
        return;
    }
    if (status != 0 || output_size == 0) {
        console_puts("zinstall failed: could not load output\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     install_dir,
                                     install_name,
                                     (const char *)exec_buffer,
                                     output_size);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (status == -9) {
        console_puts("zinstall failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zinstall failed: could not save installed output\n");
        return;
    }

    console_puts("zinstall: installed ");
    console_puts(output_name);
    console_puts(" as ");
    console_puts(install_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static int zmodule_find_free_slot(void) {
    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            return (int)i;
        }
    }

    return -1;
}

static int zmodule_collect_exports(zobject_resolved_symbol_t *symbols,
                                   uint32_t capacity,
                                   uint32_t *out_count) {
    uint32_t count = 0;

    if (symbols == 0 || out_count == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            if (streq(zmodule_slots[i].exports[j].name, "zmodule_tick") ||
                streq(zmodule_slots[i].exports[j].name, "zmodule_unload") ||
                streq(zmodule_slots[i].exports[j].name, "zmodule_redraw")) {
                continue;
            }
            if (count >= capacity) {
                return -1;
            }

            symbols[count++] = zmodule_slots[i].exports[j];
        }
    }

    *out_count = count;
    return 0;
}

void shell_modules_tick(void) {
    unsigned long long now = timer_ticks();
    unsigned int hz = timer_frequency();
    unsigned long long interval;

    if (hz == 0u) {
        hz = 100u;
    }
    interval = hz / ZMODULE_TICK_HZ;
    if (interval == 0ull) {
        interval = 1ull;
    }

    if (now - zmodule_last_tick < interval) {
        return;
    }
    zmodule_last_tick = now;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            if (streq(zmodule_slots[i].exports[j].name, "zmodule_tick")) {
                ((zmodule_tick_t)(uintptr_t)zmodule_slots[i].exports[j].value)();
                break;
            }
        }
    }
}

uint32_t shell_module_count(void) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (zmodule_slots[i].loaded) {
            ++count;
        }
    }

    return count;
}

const char *shell_module_name(uint32_t index) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }
        if (seen == index) {
            return zmodule_slots[i].name;
        }
        ++seen;
    }

    return 0;
}

int shell_module_tick(uint32_t index) {
    return shell_module_call(index, "zmodule_tick");
}

int shell_module_call(uint32_t index, const char *export_name) {
    uint32_t seen = 0;

    if (export_name == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }
        if (seen == index) {
            for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
                if (streq(zmodule_slots[i].exports[j].name, export_name)) {
                    ((zmodule_void_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value)();
                    return 0;
                }
            }
            return -1;
        }
        ++seen;
    }

    return -1;
}

static uint32_t zmodule_name_len(const char *name) {
    uint32_t len = 0;

    while (name != 0 && name[len] != '\0') {
        ++len;
    }

    return len;
}

static int zmodule_name_has_zo_suffix(const char *name) {
    uint32_t len = zmodule_name_len(name);

    return len > 3u &&
           name[len - 3u] == '.' &&
           name[len - 2u] == 'z' &&
           name[len - 1u] == 'o';
}

static int zmodule_name_matches(const char *loaded_name, const char *query) {
    uint32_t loaded_len;
    uint32_t query_len;

    if (loaded_name == 0 || query == 0 || *query == '\0') {
        return 0;
    }
    if (streq(loaded_name, query)) {
        return 1;
    }

    loaded_len = zmodule_name_len(loaded_name);
    query_len = zmodule_name_len(query);

    if (zmodule_name_has_zo_suffix(loaded_name) &&
        loaded_len == query_len + 3u) {
        for (uint32_t i = 0; i < query_len; ++i) {
            if (loaded_name[i] != query[i]) {
                return 0;
            }
        }
        return 1;
    }

    if (zmodule_name_has_zo_suffix(query) &&
        query_len == loaded_len + 3u) {
        for (uint32_t i = 0; i < loaded_len; ++i) {
            if (loaded_name[i] != query[i]) {
                return 0;
            }
        }
        return 1;
    }

    return 0;
}

static int zmodule_find_slot_by_name(const char *name) {
    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (zmodule_slots[i].loaded && zmodule_name_matches(zmodule_slots[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static void zmodule_call_unload_hook(zmodule_slot_t *slot) {
    if (slot == 0 || !slot->loaded) {
        return;
    }

    for (uint32_t i = 0; i < slot->export_count; ++i) {
        if (streq(slot->exports[i].name, "zmodule_unload")) {
            ((zmodule_unload_t)(uintptr_t)slot->exports[i].value)();
            return;
        }
    }
}

static void zmodule_clear_slot(uint32_t slot_index) {
    if (slot_index >= ZMODULE_MAX_MODULES || !zmodule_slots[slot_index].loaded) {
        return;
    }

    zmodule_call_unload_hook(&zmodule_slots[slot_index]);
    kfree(zmodule_slots[slot_index].image);
    zmodule_slots[slot_index].image = 0;
    zero_memory(zmodule_slots[slot_index].exports, sizeof(zmodule_slots[slot_index].exports));
    zmodule_slots[slot_index].loaded = 0;
    zmodule_slots[slot_index].name[0] = '\0';
    zmodule_slots[slot_index].image_size = 0;
    zmodule_slots[slot_index].object_count = 0;
    zmodule_slots[slot_index].export_count = 0;
}

static void cmd_zmod(const char *args, const boot_info_t *info) {
    (void)info;

    const unsigned char *objects[ZLINK_MAX_OBJECTS];
    uint32_t object_sizes[ZLINK_MAX_OBJECTS];
    uint32_t object_count = 0;
    uint32_t object_offset = 0;
    uint32_t output_size = 0;
    uint32_t error_line = 0;
    uint32_t resident_symbol_count = 0;
    uint32_t export_symbol_count = 0;
    char *mutable_args = (char *)args;
    char *tokens[ZLINK_MAX_OBJECTS];
    uint32_t token_count = 0;
    int slot_index = -1;
    int drive = active_drive();
    int status = 0;
    zobject_resolved_symbol_t resident_symbols[ZOBJECT_MAX_RESOLVED_SYMBOLS];
    zobject_resolved_symbol_t export_symbols[ZMODULE_MAX_EXPORTS];

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    mutable_args = skip_spaces(mutable_args);
    while (*mutable_args != '\0') {
        if (token_count >= ZLINK_MAX_OBJECTS) {
            console_puts("usage: zmod input.zo [more.zo ...]\n");
            return;
        }

        tokens[token_count++] = mutable_args;
        while (*mutable_args && *mutable_args != ' ' && *mutable_args != '\t') {
            ++mutable_args;
        }
        if (*mutable_args != '\0') {
            *mutable_args++ = '\0';
            mutable_args = skip_spaces(mutable_args);
        }
    }

    if (token_count == 0) {
        console_puts("usage: zmod input.zo [more.zo ...]\n");
        return;
    }

    slot_index = zmodule_find_free_slot();
    if (slot_index < 0) {
        console_puts("zmod failed: no free resident module slots\n");
        return;
    }

    if (zmodule_collect_exports(resident_symbols,
                                ZOBJECT_MAX_RESOLVED_SYMBOLS,
                                &resident_symbol_count) != 0) {
        console_puts("zmod failed: resident symbol table is full\n");
        return;
    }

    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    for (uint32_t i = 0; i < token_count; ++i) {
        uint32_t remaining = EXEC_BUFFER_SIZE - object_offset;
        const char *load_name = tokens[i];
        char suffixed_name[32];
        int has_dot = 0;

        for (uint32_t j = 0; tokens[i][j]; ++j) {
            if (tokens[i][j] == '.') {
                has_dot = 1;
                break;
            }
        }

        objects[object_count] = asm_output + object_offset;
        {
            int object_drive = drive;
            uint32_t object_parent = cwd_dirs[drive];
            char object_name[32];

            status = resolve_file_path_with_drive(load_name,
                                                  drive,
                                                  &object_drive,
                                                  &object_parent,
                                                  object_name,
                                                  sizeof(object_name));
            if (status == 0) {
                status = lainfs_load_file_in_dir((char)('A' + object_drive),
                                                 object_parent,
                                                 object_name,
                                                 (char *)(asm_output + object_offset),
                                                 remaining,
                                                 &object_sizes[object_count]);
            }
        }
        if (status == -5 && !has_dot &&
            make_suffixed_name(tokens[i], ".zo", suffixed_name, sizeof(suffixed_name)) == 0) {
            int object_drive = drive;
            uint32_t object_parent = cwd_dirs[drive];
            char object_name[32];

            load_name = suffixed_name;
            status = resolve_file_path_with_drive(load_name,
                                                  drive,
                                                  &object_drive,
                                                  &object_parent,
                                                  object_name,
                                                  sizeof(object_name));
            if (status == 0) {
                status = lainfs_load_file_in_dir((char)('A' + object_drive),
                                                 object_parent,
                                                 object_name,
                                                 (char *)(asm_output + object_offset),
                                                 remaining,
                                                 &object_sizes[object_count]);
            }
        }
        if (status == -3) {
            console_puts("drive is not formatted as lainfs\n");
            return;
        }
        if (status == -5) {
            console_puts(".zo module not found: ");
            console_puts(tokens[i]);
            console_puts("\n");
            return;
        }
        if (status != 0) {
            console_puts("zmod failed: could not load module\n");
            return;
        }

        object_offset += object_sizes[object_count];
        ++object_count;
    }

    kfree(zmodule_slots[slot_index].image);
    zmodule_slots[slot_index].image = (unsigned char *)kmalloc(ZMODULE_IMAGE_SIZE);
    if (zmodule_slots[slot_index].image == 0) {
        console_puts("zmod failed: out of heap for resident module\n");
        return;
    }

    zero_memory(zmodule_slots[slot_index].image, ZMODULE_IMAGE_SIZE);
    if (zobject_link_flat_many_ex(objects,
                                  object_sizes,
                                  object_count,
                                  zmodule_slots[slot_index].image,
                                  ZMODULE_IMAGE_SIZE,
                                  (uint64_t)(uintptr_t)zmodule_slots[slot_index].image,
                                  &output_size,
                                  &error_line,
                                  resident_symbols,
                                  resident_symbol_count,
                                  export_symbols,
                                  ZMODULE_MAX_EXPORTS,
                                  &export_symbol_count) != 0) {
        console_puts("zmod failed: unresolved or unsupported module");
        if (error_line != 0) {
            console_puts(" asm line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        kfree(zmodule_slots[slot_index].image);
        zmodule_slots[slot_index].image = 0;
        return;
    }

    zmodule_slots[slot_index].loaded = 1;
    zmodule_slots[slot_index].image_size = output_size;
    zmodule_slots[slot_index].object_count = object_count;
    zmodule_slots[slot_index].export_count = export_symbol_count;
    copy_text_limited(zmodule_slots[slot_index].name,
                      sizeof(zmodule_slots[slot_index].name),
                      tokens[0]);
    for (uint32_t i = 0; i < export_symbol_count; ++i) {
        zmodule_slots[slot_index].exports[i] = export_symbols[i];
    }

    console_puts("loading ");
    console_put_dec64(object_count);
    console_puts(" module object(s) in slot ");
    console_put_dec64((uint32_t)slot_index);
    console_puts(" at 0x");
    console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[slot_index].image);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts(" exports=");
    console_put_dec64(export_symbol_count);
    console_puts("\n");

    ((exec_program_t)(uintptr_t)zmodule_slots[slot_index].image)(&api);

    console_puts("\nmodule resident\n");
}

static void cmd_zunload(const char *args, const boot_info_t *info) {
    char *mutable_args = (char *)args;
    char *name = 0;
    char *extra = 0;
    int slot_index;

    (void)info;

    split_first_arg(mutable_args, &name, &extra);
    if (*name == '\0' || *extra != '\0') {
        console_puts("usage: zunload module\n");
        return;
    }

    slot_index = zmodule_find_slot_by_name(name);
    if (slot_index < 0) {
        console_puts("zunload failed: module not resident: ");
        console_puts(name);
        console_puts("\n");
        return;
    }

    zmodule_clear_slot((uint32_t)slot_index);
    console_puts("zunload: unloaded ");
    console_puts(name);
    console_puts("\n");
}

static void cmd_zreload(const char *args, const boot_info_t *info) {
    char command[64];
    char *mutable_command = command;
    char *name = 0;
    char *extra = 0;

    (void)info;

    if (copy_command_arg(args, command, sizeof(command)) != 0) {
        console_puts("usage: zreload module\n");
        return;
    }

    split_first_arg(mutable_command, &name, &extra);
    if (*name == '\0' || *extra != '\0') {
        console_puts("usage: zreload module\n");
        return;
    }

    {
        int slot_index = zmodule_find_slot_by_name(name);
        if (slot_index >= 0) {
            zmodule_clear_slot((uint32_t)slot_index);
            console_puts("zreload: unloaded old ");
            console_puts(name);
            console_puts("\n");
        }
    }

    cmd_zmod(name, 0);
}

static void cmd_zmods(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int any = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }

        any = 1;
        console_puts("#");
        console_put_dec64(i);
        console_puts(" ");
        console_puts(zmodule_slots[i].name);
        console_puts(" base=0x");
        console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[i].image);
        console_puts(" bytes=");
        console_put_dec64(zmodule_slots[i].image_size);
        console_puts(" objects=");
        console_put_dec64(zmodule_slots[i].object_count);
        console_puts(" exports=");
        console_put_dec64(zmodule_slots[i].export_count);
        console_puts("\n");

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            console_puts("  ");
            console_puts(zmodule_slots[i].exports[j].name);
            console_puts(" = 0x");
            console_put_hex64(zmodule_slots[i].exports[j].value);
            console_puts("\n");
        }
    }

    if (!any) {
        console_puts("no resident modules\n");
    }
}

static void cmd_zrun(const char *args, const boot_info_t *info) {
    (void)info;

    char *source = shell_source_buffer;
    const char *name = skip_const_spaces(args);
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t output_size = 0;
    uint32_t compile_error_line = 0;
    uint32_t assemble_error_line = 0;
    int drive = active_drive();
    int status = 0;

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: zrun source.Z\n");
        return;
    }

    status = compile_z_source_file(name,
                                   source,
                                   &source_size,
                                   &compile_error_line,
                                   &asm_size,
                                   &drive);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -20) {
        console_puts("zrun failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    if (status == -30) {
        console_puts("zrun failed: include nesting is too deep\n");
        return;
    }
    if (status == -31) {
        console_puts("zrun failed: malformed include line\n");
        return;
    }
    if (status == -32) {
        console_puts("zrun failed: expanded source is too large\n");
        return;
    }
    if (status == -33) {
        console_puts("zrun failed: too many pragma once headers\n");
        return;
    }
    if (status != 0) {
        console_puts("zrun failed: could not load source\n");
        return;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (assembler_assemble_source_ex(zscript_output,
                                     asm_size,
                                     exec_buffer,
                                     EXEC_BUFFER_SIZE,
                                     (uint64_t)(uintptr_t)exec_buffer,
                                     &output_size,
                                     &assemble_error_line) != 0) {
        console_puts("zrun failed: compiler emitted unsupported asm");
        if (assemble_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(assemble_error_line);
        }
        console_puts("\n");
        return;
    }

    console_puts("running ");
    console_puts(name);
    console_puts(" at 0x");
    console_put_hex64((uint64_t)(uintptr_t)exec_buffer);
    console_puts("\n");

    ((exec_program_t)(uintptr_t)exec_buffer)(&api);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);

    console_puts("\nprogram returned\n");
}

static void cmd_zasm(const char *args, const boot_info_t *info) {
    (void)info;

    char *source = shell_source_buffer;
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t compile_error_line = 0;
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0') {
        console_puts("usage: zasm source.Z [output.asm]\n");
        return;
    }

    status = compile_z_source_file(source_name,
                                   source,
                                   &source_size,
                                   &compile_error_line,
                                   &asm_size,
                                   &drive);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -20) {
        console_puts("zasm failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    if (status == -30) {
        console_puts("zasm failed: include nesting is too deep\n");
        return;
    }
    if (status == -31) {
        console_puts("zasm failed: malformed include line\n");
        return;
    }
    if (status == -32) {
        console_puts("zasm failed: expanded source is too large\n");
        return;
    }
    if (status == -33) {
        console_puts("zasm failed: too many pragma once headers\n");
        return;
    }
    if (status != 0) {
        console_puts("zasm failed: could not load source\n");
        return;
    }

    if (*output_name != '\0') {
        status = lainfs_save_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         output_name,
                                         zscript_output,
                                         asm_size);
        if (status == -9) {
            console_puts("zasm failed: disk is full\n");
            return;
        }
        if (status != 0) {
            console_puts("zasm failed: could not save output\n");
            return;
        }

        console_puts("wrote generated asm to ");
        console_puts(output_name);
        console_puts("\n");
        return;
    }

    console_puts("generated asm for ");
    console_puts(source_name);
    console_puts(":\n");
    console_puts(zscript_output);
    if (asm_size == 0 || zscript_output[asm_size - 1] != '\n') {
        console_puts("\n");
    }
}

static void cmd_bgcolor(const char *args, const boot_info_t *info) {
    (void)info;

    unsigned int color = 0;

    if (parse_color_arg(args, &color) != 0) {
        console_puts("Invalid color, use 6 digit hex code!\n");
        return;
    }

    console_set_bg_color(color);
}

static void cmd_fgcolor(const char *args, const boot_info_t *info) {
    (void)info;

    unsigned int color = 0;

    if (parse_color_arg(args, &color) != 0) {
        console_puts("Invalid color, use 6 digit hex code!\n");
        return;
    }

    console_set_fg_color(color);
}

void shell_set_session(unsigned int session) {
    if (session >= SHELL_MAX_SESSIONS) {
        return;
    }

    ensure_session_initialized(session);
    active_session_index = session;
}

void shell_init(void) {
    zero_memory(shell_sessions, sizeof(shell_sessions));

    for (int i = 0; i < MAX_DRIVES; ++i) {
        drives[i].present = 0;
        drives[i].label[0] = '\0';
    }

    active_session_index = 0;
    init_session_blank(&shell_sessions[0]);
    if (!shell_alloc_work_buffers()) {
        console_puts("shell: out of heap for work buffers\n");
    }
}

void shell_run_autoexec(const char *name, const boot_info_t *info) {
    int previous_drive = current_drive;

    if (active_drive() < 0) {
        return;
    }
    if (!shell_work_buffers_ready) {
        return;
    }

    run_script_file(name, info, 1);
    current_drive = previous_drive;
}

void shell_print_prompt(void) {
    if (current_drive >= 0 && drives[current_drive].present) {
        print_drive_name(current_drive);
        console_puts(cwd_paths[current_drive]);
        console_puts("> ");
        return;
    }

    console_puts("> ");
}

void shell_run_command(char *line, const boot_info_t *info) {
    char *name = skip_spaces(line);
    char *args = name;

    while (*args && *args != ' ' && *args != '\t') {
        ++args;
    }

    if (*args) {
        *args++ = '\0';
        args = skip_spaces(args);
    }

    if (*name == '\0') {
        return;
    }

    int requested_drive = parse_drive_spec(name);
    if (requested_drive >= 0 && requested_drive < MAX_DRIVES) {
        if (drives[requested_drive].present || storage_drive_is_mounted((char)('A' + requested_drive))) {
            current_drive = requested_drive;
        } else {
            print_drive_name(requested_drive);
            console_puts(" does not exist. create it with mkdrive ");
            print_drive_name(requested_drive);
            console_puts("\n");
        }
        return;
    }

    for (unsigned int i = 0; i < command_count; ++i) {
        if (streq(name, commands[i].name)) {
            commands[i].handler(args, info);
            return;
        }
    }

    console_puts("unknown command: ");
    console_puts(name);
    console_puts("\n");
}
