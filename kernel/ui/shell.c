
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
#include "registry.h"
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
#define EXEC_BUFFER_SIZE (32u * 1024u * 1024u)
#define EXEC_API_MAGIC 0x4C41494E45584543ull
#define ASM_SOURCE_SIZE (3u * 1024u * 1024u)
#define Z_INCLUDE_BUFFER_SIZE (512u * 1024u)
#define ZMODULE_IMAGE_SIZE (16u * 1024u * 1024u)
#define SHELL_PATH_SIZE 128u
#define SHELL_MAX_SESSIONS 2u
#define Z_INCLUDE_MAX_DEPTH 4u
#define Z_INCLUDE_MAX_DIRS 4u
#define Z_INCLUDE_ONCE_MAX 32u
#define ZLINK_MAX_OBJECTS 1024u
#define ZMODULE_MAX_MODULES 8u
#define ZMODULE_MAX_EXPORTS ZOBJECT_MAX_RESOLVED_SYMBOLS
#define ZMODULE_NAME_SIZE 32u
#define ZMODULE_TICK_HZ 20u
#define ZMODULE_CALL_STACK_SIZE (4u * 1024u * 1024u)
#define SHELL_REGISTRY_FILE "registry.cfg"
#define SHELL_BOOTMODE_FILE "bootmode.cfg"
#define SHELL_BG_JOBS 8u
#define SHELL_BG_LINE_SIZE SCRIPT_LINE_SIZE
#define SHELL_TASK_SNAPSHOT_MAX 32u

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
static char zmodule_manifest_object_names[ZLINK_MAX_OBJECTS][32];
static int shell_work_buffers_ready;
static unsigned int shell_bg_next_id = 1u;
static int shell_boot_safe_mode;
static int shell_boot_debug_mode;
static int shell_boot_usb_safe_mode;

typedef enum {
    SHELL_BG_FREE = 0,
    SHELL_BG_RUNNING,
    SHELL_BG_DONE
} shell_bg_state_t;

typedef struct {
    shell_bg_state_t state;
    unsigned int job_id;
    unsigned int task_id;
    int status;
    char line[SHELL_BG_LINE_SIZE];
} shell_bg_job_t;

typedef struct {
    unsigned int slot;
    boot_info_t info;
    char line[SHELL_BG_LINE_SIZE];
} shell_bg_context_t;

static shell_bg_job_t shell_bg_jobs[SHELL_BG_JOBS];

typedef struct {
    int loaded;
    int unloading;
    uint32_t active_calls;
    int unload_called;
    char name[ZMODULE_NAME_SIZE];
    unsigned char *image_alloc;
    unsigned char *image;
    unsigned char *call_stack;
    uint32_t call_stack_size;
    uint32_t image_size;
    uint32_t object_count;
    uint32_t export_count;
    zobject_resolved_symbol_t exports[ZMODULE_MAX_EXPORTS];
} zmodule_slot_t;

static zmodule_slot_t zmodule_slots[ZMODULE_MAX_MODULES];
static zobject_resolved_symbol_t zmodule_resident_symbol_work[ZOBJECT_MAX_RESOLVED_SYMBOLS];
static zobject_resolved_symbol_t zmodule_export_symbol_work[ZMODULE_MAX_EXPORTS];
static unsigned long long zmodule_last_tick;

#define ZMODULE_IMAGE_ALIGNMENT 16u

static unsigned char *zmodule_align_image_allocation(unsigned char *ptr) {
    uintptr_t value;

    if (ptr == 0) {
        return 0;
    }
    value = (uintptr_t)ptr;
    value = (value + (uintptr_t)(ZMODULE_IMAGE_ALIGNMENT - 1u)) &
            ~(uintptr_t)(ZMODULE_IMAGE_ALIGNMENT - 1u);
    return (unsigned char *)value;
}

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
typedef void (*zmodule_key_hook_t)(uint32_t key_type, uint32_t ch);
typedef void (*zmodule_mouse_hook_t)(uint32_t x, uint32_t y, uint32_t buttons, int32_t wheel);

static void zmodule_call_program_on_stack(exec_program_t fn,
                                          const exec_api_t *api,
                                          void *stack_top) {
    __asm__ __volatile__(
        "mov %[fn], %%r10\n\t"
        "mov %[stack_top], %%r11\n\t"
        "mov %%rsp, %%rax\n\t"
        "mov %%r11, %%rsp\n\t"
        "and $-16, %%rsp\n\t"
        "push %%rax\n\t"
        "sub $8, %%rsp\n\t"
        "mov %[api], %%rdi\n\t"
        "call *%%r10\n\t"
        "add $8, %%rsp\n\t"
        "pop %%rsp\n\t"
        :
        : [fn] "r"(fn), [api] "r"(api), [stack_top] "r"(stack_top)
        : "rax", "rdi", "r10", "r11", "memory", "cc");
}

static void zmodule_call_void_on_stack(zmodule_void_hook_t fn,
                                       void *stack_top) {
    __asm__ __volatile__(
        "mov %[fn], %%r10\n\t"
        "mov %[stack_top], %%r11\n\t"
        "mov %%rsp, %%rax\n\t"
        "mov %%r11, %%rsp\n\t"
        "and $-16, %%rsp\n\t"
        "push %%rax\n\t"
        "sub $8, %%rsp\n\t"
        "call *%%r10\n\t"
        "add $8, %%rsp\n\t"
        "pop %%rsp\n\t"
        :
        : [fn] "r"(fn), [stack_top] "r"(stack_top)
        : "rax", "r10", "r11", "memory", "cc");
}

static void zmodule_call_key_on_stack(zmodule_key_hook_t fn,
                                      uint32_t key_type,
                                      uint32_t ch,
                                      void *stack_top) {
    __asm__ __volatile__(
        "mov %[fn], %%r10\n\t"
        "mov %[stack_top], %%r11\n\t"
        "mov %%rsp, %%rax\n\t"
        "mov %%r11, %%rsp\n\t"
        "and $-16, %%rsp\n\t"
        "push %%rax\n\t"
        "sub $8, %%rsp\n\t"
        "mov %[key_type], %%edi\n\t"
        "mov %[ch], %%esi\n\t"
        "call *%%r10\n\t"
        "add $8, %%rsp\n\t"
        "pop %%rsp\n\t"
        :
        : [fn] "r"(fn),
          [key_type] "r"(key_type),
          [ch] "r"(ch),
          [stack_top] "r"(stack_top)
        : "rax", "rdi", "rsi", "r10", "r11", "memory", "cc");
}

static void zmodule_call_mouse_on_stack(zmodule_mouse_hook_t fn,
                                        uint32_t x,
                                        uint32_t y,
                                        uint32_t buttons,
                                        int32_t wheel,
                                        void *stack_top) {
    __asm__ __volatile__(
        "mov %[fn], %%r10\n\t"
        "mov %[stack_top], %%r11\n\t"
        "mov %%rsp, %%rax\n\t"
        "mov %%r11, %%rsp\n\t"
        "and $-16, %%rsp\n\t"
        "push %%rax\n\t"
        "sub $8, %%rsp\n\t"
        "mov %[x], %%edi\n\t"
        "mov %[y], %%esi\n\t"
        "mov %[buttons], %%edx\n\t"
        "mov %[wheel], %%ecx\n\t"
        "call *%%r10\n\t"
        "add $8, %%rsp\n\t"
        "pop %%rsp\n\t"
        :
        : [fn] "r"(fn),
          [x] "r"(x),
          [y] "r"(y),
          [buttons] "r"(buttons),
          [wheel] "r"(wheel),
          [stack_top] "r"(stack_top)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r10", "r11", "memory", "cc");
}

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

static int str_ends_with(const char *s, const char *suffix) {
    uint32_t s_len = 0;
    uint32_t suffix_len = 0;

    while (s[s_len]) {
        ++s_len;
    }
    while (suffix[suffix_len]) {
        ++suffix_len;
    }
    if (suffix_len > s_len) {
        return 0;
    }
    return streq(s + s_len - suffix_len, suffix);
}

static int active_drive(void);
static int append_text_limited(char *out, uint32_t out_size, uint32_t *pos, const char *text);
static int shell_run_command_foreground(char *line, const boot_info_t *info, int background);
static const char *kernel_task_state_text(unsigned int state);
static const char *lainfs_check_reason_text(uint32_t reason);
static void print_lainfs_entry_detail(char drive_letter, uint32_t entry_id);
static void print_lainfs_mount_check(char drive_letter);
static void reset_cwd(int drive);

static void zero_memory(void *ptr, uint32_t size) {
    unsigned char *p = (unsigned char *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void copy_string_limited(char *dst, uint32_t dst_size, const char *src) {
    uint32_t i = 0;

    if (dst_size == 0) {
        return;
    }

    while (src && src[i] && i + 1u < dst_size) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
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

static char *trim_spaces_mutable(char *s) {
    char *end;

    s = skip_spaces(s);
    end = s;
    while (*end != '\0') {
        ++end;
    }
    while (end > s &&
           (end[-1] == ' ' ||
            end[-1] == '\t' ||
            end[-1] == '\r' ||
            end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return s;
}

void shell_registry_save(void) {
    int drive = active_drive();
    uint32_t pos = 0;
    uint32_t count;

    if (drive < 0 || !shell_work_buffers_ready || shell_manifest_buffer == 0) {
        return;
    }

    count = registry_count();
    shell_manifest_buffer[0] = '\0';
    for (uint32_t i = 0; i < count; ++i) {
        const char *key = registry_key_at(i);
        const char *value = registry_value_at(i);
        if (key == 0 || value == 0) {
            continue;
        }
        if (append_text_limited(shell_manifest_buffer, ASM_SOURCE_SIZE, &pos, key) != 0 ||
            append_text_limited(shell_manifest_buffer, ASM_SOURCE_SIZE, &pos, "=") != 0 ||
            append_text_limited(shell_manifest_buffer, ASM_SOURCE_SIZE, &pos, value) != 0 ||
            append_text_limited(shell_manifest_buffer, ASM_SOURCE_SIZE, &pos, "\n") != 0) {
            return;
        }
    }

    if (lainfs_save_file_in_dir((char)('A' + drive),
                                LAINFS_ROOT_DIR,
                                SHELL_REGISTRY_FILE,
                                shell_manifest_buffer,
                                pos) == 0) {
        (void)lainfs_flush((char)('A' + drive));
    }
}

void shell_registry_load(void) {
    int drive = active_drive();
    uint32_t size = 0;

    if (drive < 0 || !shell_work_buffers_ready || shell_manifest_buffer == 0) {
        return;
    }
    if (lainfs_load_file_in_dir((char)('A' + drive),
                                LAINFS_ROOT_DIR,
                                SHELL_REGISTRY_FILE,
                                shell_manifest_buffer,
                                ASM_SOURCE_SIZE,
                                &size) != 0 ||
        size >= ASM_SOURCE_SIZE) {
        return;
    }

    shell_manifest_buffer[size] = '\0';
    registry_suspend_save(1);
    for (uint32_t pos = 0; pos < size;) {
        char *line = shell_manifest_buffer + pos;
        char *eq;
        char *key;
        char *value;

        while (pos < size && shell_manifest_buffer[pos] != '\n') {
            ++pos;
        }
        if (pos < size) {
            shell_manifest_buffer[pos++] = '\0';
        }

        key = trim_spaces_mutable(line);
        if (*key == '\0' || *key == '#' || *key == ';') {
            continue;
        }
        eq = key;
        while (*eq != '\0' && *eq != '=') {
            ++eq;
        }
        if (*eq != '=') {
            continue;
        }
        *eq++ = '\0';
        key = trim_spaces_mutable(key);
        value = trim_spaces_mutable(eq);
        if (*key != '\0') {
            (void)registry_set(key, value);
        }
    }
    registry_suspend_save(0);
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

    if (drive < 0 || drive >= MAX_DRIVES ||
        (!drives[drive].present && !storage_drive_is_mounted((char)('A' + drive)))) {
        return -1;
    }
    if (!drives[drive].present && storage_drive_is_mounted((char)('A' + drive))) {
        drives[drive].present = 1;
        copy_label(drives[drive].label, "MOUNT");
        reset_cwd(drive);
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
    return *line == '\0' || *line == '#' || (line[0] == '/' && line[1] == '/');
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

static void shell_bg_worker(void *arg) {
    shell_bg_context_t *ctx = (shell_bg_context_t *)arg;

    if (ctx == 0 || ctx->slot >= SHELL_BG_JOBS) {
        if (ctx != 0) {
            kfree(ctx);
        }
        return;
    }

    shell_bg_jobs[ctx->slot].status = shell_run_command_foreground(ctx->line, &ctx->info, 1);
    __sync_synchronize();
    kfree(ctx);
}

static void shell_bg_poll(void) {
    for (uint32_t i = 0; i < SHELL_BG_JOBS; ++i) {
        if (shell_bg_jobs[i].state == SHELL_BG_RUNNING &&
            kernel_task_done(shell_bg_jobs[i].task_id)) {
            kernel_task_release(shell_bg_jobs[i].task_id);
            shell_bg_jobs[i].state = SHELL_BG_DONE;
        }
    }
}

static int shell_bg_start(char *line, const boot_info_t *info) {
    shell_bg_context_t *ctx;
    unsigned int task_id;
    uint32_t slot = SHELL_BG_JOBS;

    shell_bg_poll();
    for (uint32_t i = 0; i < SHELL_BG_JOBS; ++i) {
        if (shell_bg_jobs[i].state == SHELL_BG_FREE || shell_bg_jobs[i].state == SHELL_BG_DONE) {
            slot = i;
            break;
        }
    }

    if (slot == SHELL_BG_JOBS) {
        return -1;
    }

    ctx = (shell_bg_context_t *)kmalloc(sizeof(*ctx));
    if (ctx == 0) {
        return -2;
    }

    zero_memory(ctx, sizeof(*ctx));
    ctx->slot = slot;
    if (info) {
        ctx->info = *info;
    }
    copy_string_limited(ctx->line, sizeof(ctx->line), line);

    shell_bg_jobs[slot].state = SHELL_BG_RUNNING;
    shell_bg_jobs[slot].job_id = shell_bg_next_id++;
    if (shell_bg_jobs[slot].job_id == 0u) {
        shell_bg_jobs[slot].job_id = shell_bg_next_id++;
    }
    copy_string_limited(shell_bg_jobs[slot].line, sizeof(shell_bg_jobs[slot].line), ctx->line);
    shell_bg_jobs[slot].status = -1;

    task_id = kernel_task_submit_named(shell_bg_worker, ctx, shell_bg_jobs[slot].line);
    if (task_id == 0u) {
        shell_bg_jobs[slot].state = SHELL_BG_FREE;
        shell_bg_jobs[slot].job_id = 0u;
        shell_bg_jobs[slot].task_id = 0u;
        shell_bg_jobs[slot].status = 0;
        shell_bg_jobs[slot].line[0] = '\0';
        kfree(ctx);
        return -3;
    }

    shell_bg_jobs[slot].task_id = task_id;
    console_puts("[");
    console_put_dec64(shell_bg_jobs[slot].job_id);
    console_puts("] ");
    console_puts(shell_bg_jobs[slot].line);
    console_puts(" &\n");
    return 0;
}

static void shell_bg_list(void) {
    int found = 0;

    shell_bg_poll();
    for (uint32_t i = 0; i < SHELL_BG_JOBS; ++i) {
        if (shell_bg_jobs[i].state == SHELL_BG_FREE) {
            continue;
        }

        found = 1;
        console_puts("[");
        console_put_dec64(shell_bg_jobs[i].job_id);
        console_puts("] ");
        console_puts(shell_bg_jobs[i].state == SHELL_BG_DONE ? "done    " : "running ");
        if (shell_bg_jobs[i].state == SHELL_BG_DONE) {
            console_puts("status=");
            console_put_dec64((uint32_t)shell_bg_jobs[i].status);
            console_puts(" ");
        }
        console_puts(shell_bg_jobs[i].line);
        console_puts("\n");
    }

    if (!found) {
        console_puts("no background jobs\n");
    }
}

static void cmd_help(const char *args, const boot_info_t *info);
static void cmd_bgcolor(const char *args, const boot_info_t *info);
static void cmd_fgcolor(const char *args, const boot_info_t *info);
static void cmd_resolution(const char *args, const boot_info_t *info);
static void cmd_reg(const char *args, const boot_info_t *info);
static void cmd_theme(const char *args, const boot_info_t *info);
static void cmd_clear(const char *args, const boot_info_t *info);
static void cmd_echo(const char *args, const boot_info_t *info);
static void cmd_info(const char *args, const boot_info_t *info);
static void cmd_heap(const char *args, const boot_info_t *info);
static void cmd_heaptest(const char *args, const boot_info_t *info);
static void cmd_cpus(const char *args, const boot_info_t *info);
static void cmd_smp(const char *args, const boot_info_t *info);
static void cmd_tasks(const char *args, const boot_info_t *info);
static void cmd_tasktest(const char *args, const boot_info_t *info);
static void cmd_jobs(const char *args, const boot_info_t *info);
static void cmd_wait(const char *args, const boot_info_t *info);
static void cmd_bootmode(const char *args, const boot_info_t *info);
static void cmd_gfx(const char *args, const boot_info_t *info);
static void cmd_fscheck(const char *args, const boot_info_t *info);
static void cmd_fsrepair(const char *args, const boot_info_t *info);
static void cmd_fsflush(const char *args, const boot_info_t *info);
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
static void cmd_date(const char *args, const boot_info_t *info);
static void cmd_run(const char *args, const boot_info_t *info);
static void cmd_exec(const char *args, const boot_info_t *info);
static void cmd_asm(const char *args, const boot_info_t *info);
static void refresh_system_autoexec(char drive_letter);
static void cmd_zc(const char *args, const boot_info_t *info);
static void cmd_zcc(const char *args, const boot_info_t *info);
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
static void cmd_zmodtest(const char *args, const boot_info_t *info);
static void cmd_zmods(const char *args, const boot_info_t *info);
static int zmodule_find_slot_by_name(const char *name);
static void zmodule_clear_slot(uint32_t slot_index);
static void cmd_zrun(const char *args, const boot_info_t *info);
static void cmd_zasm(const char *args, const boot_info_t *info);
static void cmd_keymap(const char *args, const boot_info_t *info);
static void split_first_arg(char *s, char **first, char **rest);

static const command_t commands[] = {
    { "help",    "show commands",             cmd_help },
    { "bgcolor", "set background color",      cmd_bgcolor },
    { "fgcolor", "set text color",            cmd_fgcolor },
    { "resolution", "set next-boot resolution", cmd_resolution },
    { "reg",     "read or write system registry", cmd_reg },
    { "theme",   "personalize desktop theme", cmd_theme },
    { "clear",   "clear screen",              cmd_clear },
    { "echo",    "print text",                cmd_echo },
    { "info",    "show kernel info",          cmd_info },
    { "heap",    "show heap diagnostics",     cmd_heap },
    { "heaptest", "run heap stress diagnostics", cmd_heaptest },
    { "cpus",    "show CPU topology",         cmd_cpus },
    { "smp",     "run a multicore work test", cmd_smp },
    { "tasks",   "show cooperative tasks",     cmd_tasks },
    { "tasktest", "run cooperative task test", cmd_tasktest },
    { "jobs",    "show background jobs",       cmd_jobs },
    { "wait",    "wait for background jobs",   cmd_wait },
    { "bootmode", "set next boot mode",        cmd_bootmode },
    { "gfx",     "show graphics SMP stats",   cmd_gfx },
    { "fscheck", "check lainfs metadata",      cmd_fscheck },
    { "fsrepair", "repair safe lainfs metadata", cmd_fsrepair },
    { "fsflush", "flush filesystem cache",     cmd_fsflush },
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
    { "wget",    "fetch http(s)://host/path to a file", cmd_wget },
    { "mouse",   "show mouse diagnostics",    cmd_mouse },
    { "usb",     "show USB controllers",      cmd_usb },
    { "ticks",   "show timer ticks",          cmd_ticks },
    { "date",    "show CMOS RTC time",        cmd_date },
    { "run",     "run a script file",         cmd_run },
    { "exec",    "run a flat binary file",     cmd_exec },
    { "asm",     "assemble a tiny asm file",   cmd_asm },
    { "zc",      "compile a tiny .Z file",     cmd_zc },
    { "zcc",     "compile one staged C unit",   cmd_zcc },
    { "zco",     "compile .Z to a .zo object", cmd_zco },
    { "zlink",   "link a .zo object",          cmd_zlink },
    { "zbuild",  "build many .Z files",         cmd_zbuild },
    { "zclean",  "remove zbuild artifacts",     cmd_zclean },
    { "ztest",   "build and run a zbuild target", cmd_ztest },
    { "zinstall", "build and install a zbuild target", cmd_zinstall },
    { "zmod",    "load and run .zo module(s)",  cmd_zmod },
    { "zunload", "unload a resident .zo module", cmd_zunload },
    { "zreload", "unload then load a .zo module", cmd_zreload },
    { "zmodtest", "stress reload a .zo module", cmd_zmodtest },
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
    int config_drive = drive;
    int status;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    {
        const mount_t *active_mount = storage_get_mount_by_drive((char)('A' + drive));
        if (active_mount &&
            streq(active_mount->partition_name, "rd0p1")) {
            const mount_t *system_mount = storage_get_mount_by_drive('S');
            if (system_mount &&
                !streq(system_mount->partition_name, "rd0p1") &&
                storage_partition_is_writable(system_mount->partition_index)) {
                config_drive = 'S' - 'A';
            }
        }
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

        status = lainfs_load_file_in_dir((char)('A' + config_drive),
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

    status = lainfs_save_file_in_dir((char)('A' + config_drive),
                                     LAINFS_ROOT_DIR,
                                     "bootres.cfg",
                                     config,
                                     pos);
    if (status != 0) {
        console_puts("resolution failed: could not save bootres.cfg\n");
        return;
    }
    if (lainfs_flush((char)('A' + config_drive)) != 0) {
        console_puts("resolution warning: saved request but flush failed\n");
    }

    console_puts("next boot resolution set to ");
    console_put_dec64(width);
    console_puts("x");
    console_put_dec64(height);
    console_puts(" on ");
    print_drive_name(config_drive);
    console_puts("\nreboot to apply it\n");
}

static void registry_print_all(void) {
    uint32_t count = registry_count();

    for (uint32_t i = 0; i < count; ++i) {
        const char *key = registry_key_at(i);
        const char *value = registry_value_at(i);
        if (key != 0 && value != 0) {
            console_puts(key);
            console_puts(" = ");
            console_puts(value);
            console_puts("\n");
        }
    }
}

static void cmd_reg(const char *args, const boot_info_t *info) {
    char *command = 0;
    char *key = 0;
    char *value = 0;
    const char *current;
    int status;

    (void)info;

    split_first_arg((char *)args, &command, &key);
    split_first_arg(key, &key, &value);

    if (*command == '\0' || streq(command, "list")) {
        registry_print_all();
        return;
    }
    if (streq(command, "get")) {
        if (*key == '\0') {
            console_puts("usage: reg get key\n");
            return;
        }
        current = registry_get(key);
        if (current == 0) {
            console_puts("registry key not found\n");
            return;
        }
        console_puts(current);
        console_puts("\n");
        return;
    }
    if (streq(command, "set")) {
        if (*key == '\0' || *value == '\0') {
            console_puts("usage: reg set key value\n");
            return;
        }
        status = registry_set(key, value);
        if (status == REGISTRY_OK) {
            console_puts("registry updated\n");
        } else if (status == REGISTRY_ERR_TOO_LONG) {
            console_puts("registry set failed: key or value too long\n");
        } else if (status == REGISTRY_ERR_FULL) {
            console_puts("registry set failed: registry full\n");
        } else {
            console_puts("registry set failed\n");
        }
        return;
    }

    console_puts("usage: reg [list|get key|set key value]\n");
}

static void theme_set_lain(void) {
    (void)registry_set("desktop.theme", "lain");
    (void)registry_set("desktop.bg.top", "0x35063e");
    (void)registry_set("desktop.bg.bottom", "0x2b1d3d");
    (void)registry_set("desktop.topbar", "0x100b18");
    (void)registry_set("desktop.taskbar", "0x171020");
    (void)registry_set("desktop.panel", "0x221a2d");
    (void)registry_set("desktop.panel.inner", "0x3a2f49");
    (void)registry_set("desktop.accent", "0xe05f4f");
    (void)registry_set("desktop.accent.soft", "0xb98556");
    (void)registry_set("desktop.text", "0xf6eadb");
    (void)registry_set("desktop.button", "0x8f3f62");
    (void)registry_set("desktop.task.active", "0x4b2347");
    (void)registry_set("desktop.task.inactive", "0x2d2038");
    (void)registry_set("desktop.title.left", "0x7c3a78");
    (void)registry_set("desktop.title.right", "0xe05f4f");
}

static void theme_set_midnight(void) {
    (void)registry_set("desktop.theme", "midnight");
    (void)registry_set("desktop.bg.top", "0x101820");
    (void)registry_set("desktop.bg.bottom", "0x05080d");
    (void)registry_set("desktop.topbar", "0x090d12");
    (void)registry_set("desktop.taskbar", "0x0c1118");
    (void)registry_set("desktop.panel", "0x151d28");
    (void)registry_set("desktop.panel.inner", "0x263241");
    (void)registry_set("desktop.accent", "0x33aaff");
    (void)registry_set("desktop.accent.soft", "0x4d728f");
    (void)registry_set("desktop.text", "0xe8f1f7");
    (void)registry_set("desktop.button", "0x2b5f8a");
    (void)registry_set("desktop.task.active", "0x1f415c");
    (void)registry_set("desktop.task.inactive", "0x182330");
    (void)registry_set("desktop.title.left", "0x1d5274");
    (void)registry_set("desktop.title.right", "0x33aaff");
}

static void theme_set_olive(void) {
    (void)registry_set("desktop.theme", "olive");
    (void)registry_set("desktop.bg.top", "0x1f2a20");
    (void)registry_set("desktop.bg.bottom", "0x11170f");
    (void)registry_set("desktop.topbar", "0x121810");
    (void)registry_set("desktop.taskbar", "0x151b13");
    (void)registry_set("desktop.panel", "0x20291c");
    (void)registry_set("desktop.panel.inner", "0x384331");
    (void)registry_set("desktop.accent", "0xb6c46a");
    (void)registry_set("desktop.accent.soft", "0x71804a");
    (void)registry_set("desktop.text", "0xf0efd8");
    (void)registry_set("desktop.button", "0x546832");
    (void)registry_set("desktop.task.active", "0x45552e");
    (void)registry_set("desktop.task.inactive", "0x26301f");
    (void)registry_set("desktop.title.left", "0x4f612f");
    (void)registry_set("desktop.title.right", "0xb6c46a");
}

static void theme_set_plum(void) {
    (void)registry_set("desktop.theme", "plum");
    (void)registry_set("desktop.bg.top", "0x2b1838");
    (void)registry_set("desktop.bg.bottom", "0x140f22");
    (void)registry_set("desktop.topbar", "0x120b1a");
    (void)registry_set("desktop.taskbar", "0x171020");
    (void)registry_set("desktop.panel", "0x271a32");
    (void)registry_set("desktop.panel.inner", "0x4a315e");
    (void)registry_set("desktop.accent", "0xd16b9a");
    (void)registry_set("desktop.accent.soft", "0x98618a");
    (void)registry_set("desktop.text", "0xf6eadb");
    (void)registry_set("desktop.button", "0x7b4c8f");
    (void)registry_set("desktop.task.active", "0x4b2347");
    (void)registry_set("desktop.task.inactive", "0x2d2038");
    (void)registry_set("desktop.title.left", "0x613273");
    (void)registry_set("desktop.title.right", "0xd16b9a");
}

static void cmd_theme(const char *args, const boot_info_t *info) {
    const char *name = skip_const_spaces(args);

    (void)info;

    if (*name == '\0' || streq(name, "list")) {
        console_puts("themes: lain midnight olive plum\n");
        console_puts("current: ");
        console_puts(registry_get("desktop.theme") ? registry_get("desktop.theme") : "custom");
        console_puts("\n");
        return;
    }

    if (streq(name, "lain")) {
        theme_set_lain();
    } else if (streq(name, "midnight")) {
        theme_set_midnight();
    } else if (streq(name, "olive")) {
        theme_set_olive();
    } else if (streq(name, "plum")) {
        theme_set_plum();
    } else {
        console_puts("usage: theme [list|lain|midnight|olive|plum]\n");
        return;
    }

    console_puts("desktop theme set to ");
    console_puts(name);
    console_puts("\n");
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
    console_puts(" smallest=");
    console_put_dec64(stats.smallest_free_range_pages);
    console_puts(" frag=");
    console_put_dec64(stats.fragmentation_percent);
    console_puts("%");
    console_puts(" pages\nsmall free blocks: ");
    console_put_dec64(stats.small_free_blocks);
    console_puts("\nallocation failures: ");
    console_put_dec64(stats.allocation_failures);
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

static void cmd_heaptest(const char *args, const boot_info_t *info) {
    kmem_test_result_t result;
    kmem_soak_result_t soak;
    char *command = 0;
    char *cycles_text = 0;
    char *extra = 0;
    uint64_t cycles = 64u;

    (void)info;

    split_first_arg((char *)args, &command, &cycles_text);
    split_first_arg(cycles_text, &cycles_text, &extra);

    if (*command != '\0') {
        if (streq(command, "soak")) {
            if (*cycles_text != '\0' &&
                (parse_u64_arg(cycles_text, &cycles) != 0 ||
                 cycles == 0u ||
                 cycles > 1000u ||
                 *skip_const_spaces(extra) != '\0')) {
                console_puts("usage: heaptest [soak cycles]\n");
                return;
            }
        } else if (parse_u64_arg(command, &cycles) == 0 &&
                   cycles > 0u &&
                   cycles <= 1000u &&
                   *skip_const_spaces(cycles_text) == '\0') {
            command = "soak";
        } else {
            console_puts("usage: heaptest [soak cycles]\n");
            return;
        }
    }

    if (*command != '\0') {
        console_puts("running heap soak diagnostics...\n");
        kmem_run_soaktest((uint32_t)cycles, &soak);
        console_puts(soak.passed ? "heap soak: PASS\n" : "heap soak: FAIL\n");
        console_puts("cycles: ");
        console_put_dec64(soak.cycles);
        console_puts("\nalloc attempts/successes: ");
        console_put_dec64(soak.alloc_attempts);
        console_puts("/");
        console_put_dec64(soak.alloc_successes);
        console_puts("\nunexpected allocation failures: ");
        console_put_dec64(soak.unexpected_failures);
        console_puts("\nlive allocations before/after: ");
        console_put_dec64(soak.live_allocations_before);
        console_puts("/");
        console_put_dec64(soak.live_allocations_after);
        console_puts("\nheap used before/after: ");
        console_put_dec64(soak.heap_used_before);
        console_puts("/");
        console_put_dec64(soak.heap_used_after);
        console_puts("\nfree pages before/after: ");
        console_put_dec64(soak.free_pages_before);
        console_puts("/");
        console_put_dec64(soak.free_pages_after);
        console_puts("\nlargest free range before/after: ");
        console_put_dec64(soak.largest_free_range_before);
        console_puts("/");
        console_put_dec64(soak.largest_free_range_after);
        console_puts("\nsmall free blocks before/after: ");
        console_put_dec64(soak.small_free_blocks_before);
        console_puts("/");
        console_put_dec64(soak.small_free_blocks_after);
        console_puts("\nfragmentation before/after/worst: ");
        console_put_dec64(soak.fragmentation_before);
        console_puts("%/");
        console_put_dec64(soak.fragmentation_after);
        console_puts("%/");
        console_put_dec64(soak.worst_fragmentation);
        console_puts("%");
        console_puts("\nallocation failures before/after: ");
        console_put_dec64(soak.allocation_failures_before);
        console_puts("/");
        console_put_dec64(soak.allocation_failures_after);
        console_puts("\nheap fault counters before/after: ");
        console_put_dec64(soak.fault_count_before);
        console_puts("/");
        console_put_dec64(soak.fault_count_after);
        console_puts("\n");
        return;
    }

    console_puts("running heap diagnostics...\n");
    kmem_run_selftest(&result);
    console_puts(result.passed ? "heaptest: PASS\n" : "heaptest: FAIL\n");
    console_puts("alloc attempts/successes: ");
    console_put_dec64(result.alloc_attempts);
    console_puts("/");
    console_put_dec64(result.alloc_successes);
    console_puts("\nexpected allocation failures: ");
    console_put_dec64(result.expected_failures);
    console_puts(" unexpected successes: ");
    console_put_dec64(result.unexpected_successes);
    console_puts("\nlive allocations before/after: ");
    console_put_dec64(result.live_allocations_before);
    console_puts("/");
    console_put_dec64(result.live_allocations_after);
    console_puts("\nheap used before/after: ");
    console_put_dec64(result.heap_used_before);
    console_puts("/");
    console_put_dec64(result.heap_used_after);
    console_puts("\nfree pages before/after: ");
    console_put_dec64(result.free_pages_before);
    console_puts("/");
    console_put_dec64(result.free_pages_after);
    console_puts("\nlargest free range before/after: ");
    console_put_dec64(result.largest_free_range_before);
    console_puts("/");
    console_put_dec64(result.largest_free_range_after);
    console_puts("\nsmall free blocks before/after: ");
    console_put_dec64(result.small_free_blocks_before);
    console_puts("/");
    console_put_dec64(result.small_free_blocks_after);
    console_puts("\nheap fault counters before/after: ");
    console_put_dec64(result.fault_count_before);
    console_puts("/");
    console_put_dec64(result.fault_count_after);
    console_puts("\n");
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
    console_puts("\nlocal APIC timer hz/count: ");
    console_put_dec64(cpu_lapic_timer_frequency());
    console_puts("/");
    console_put_dec64(cpu_lapic_timer_init_count());
    console_puts("\n");

    for (unsigned int i = 0; i < count; ++i) {
        console_puts("  cpu ");
        console_put_dec64(i);
        console_puts(": local APIC id ");
        console_put_dec64(cpu_lapic_id(i));
        if (i == 0) {
            console_puts(" (bootstrap)");
        }
        console_puts(" timer=");
        console_puts(cpu_core_local_timer_configured(i) ? "on" : "off");
        console_puts(" ticks=");
        console_put_dec64(cpu_core_local_timer_ticks(i));
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

static const char *kernel_task_state_text(unsigned int state) {
    switch (state) {
        case KERNEL_TASK_STATE_QUEUED:
            return "queued ";
        case KERNEL_TASK_STATE_RUNNING:
            return "running";
        case KERNEL_TASK_STATE_DONE:
            return "done   ";
        case KERNEL_TASK_STATE_FREE:
            return "free   ";
        default:
            return "unknown";
    }
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

static void cmd_tasks(const char *args, const boot_info_t *info) {
    kernel_task_info_t task_info[SHELL_TASK_SNAPSHOT_MAX];
    unsigned int ran;
    unsigned int task_count;

    (void)args;
    (void)info;

    ran = kernel_task_poll();
    shell_bg_poll();
    task_count = kernel_task_snapshot(task_info, SHELL_TASK_SNAPSHOT_MAX);

    console_puts("cooperative tasks pending: ");
    console_put_dec64(kernel_task_pending_count());
    console_puts("\npolled locally: ");
    console_put_dec64(ran);
    console_puts("\nsmp queue pending: ");
    console_put_dec64(smp_pending_work_count());
    console_puts("\n");
    if (task_count == 0u) {
        console_puts("kernel tasks: none\n");
    } else {
        console_puts("kernel tasks:\n");
        for (unsigned int i = 0; i < task_count; ++i) {
            console_puts("  #");
            console_put_dec64(task_info[i].id);
            console_puts(" ");
            console_puts(kernel_task_state_text(task_info[i].state));
            if (task_info[i].smp_id != 0u) {
                console_puts(" smp=");
                console_put_dec64(task_info[i].smp_id);
            }
            console_puts(" ");
            console_puts(task_info[i].name[0] ? task_info[i].name : "task");
            console_puts("\n");
        }
    }
    console_puts("shell background jobs:\n");
    shell_bg_list();
}

static void cmd_tasktest(const char *args, const boot_info_t *info) {
    enum { TASK_TEST_MAX_JOBS = 8 };
    smp_test_job_t jobs[TASK_TEST_MAX_JOBS];
    unsigned int ids[TASK_TEST_MAX_JOBS];
    unsigned int job_count = cpu_online_core_count();
    unsigned long long start;
    unsigned long long end;
    uint64_t checksum = 0;

    (void)args;
    (void)info;

    if (job_count == 0u) {
        job_count = 1u;
    }
    if (job_count > TASK_TEST_MAX_JOBS) {
        job_count = TASK_TEST_MAX_JOBS;
    }

    start = timer_ticks();
    for (unsigned int i = 0; i < job_count; ++i) {
        jobs[i].value = 0;
        jobs[i].iterations = 7000000u + (uint64_t)i * 1500000u;
        ids[i] = kernel_task_submit_named(smp_test_worker, &jobs[i], "tasktest");
        if (ids[i] == 0u) {
            console_puts("task queue full while submitting job\n");
            job_count = i;
            break;
        }
    }

    for (unsigned int i = 0; i < job_count; ++i) {
        kernel_task_wait(ids[i]);
        checksum ^= jobs[i].value + (uint64_t)ids[i];
    }
    end = timer_ticks();

    console_puts("tasks completed: ");
    console_put_dec64(job_count);
    console_puts("\npending tasks: ");
    console_put_dec64(kernel_task_pending_count());
    console_puts("\nsmp queue pending: ");
    console_put_dec64(smp_pending_work_count());
    console_puts("\nelapsed ticks: ");
    console_put_dec64(end - start);
    console_puts("\nchecksum: 0x");
    console_put_hex64(checksum);
    console_puts("\n");
}

static void run_background_tasktest(void) {
    smp_test_job_t job;

    job.value = 0;
    job.iterations = 45000000u;
    smp_test_worker(&job);
}

static void cmd_jobs(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    shell_bg_list();
}

static void cmd_wait(const char *args, const boot_info_t *info) {
    uint64_t requested = 0;
    int wait_all;
    int matched = 0;

    (void)info;

    args = skip_const_spaces(args);
    wait_all = *args == '\0';
    if (!wait_all && parse_u64_arg(args, &requested) != 0) {
        console_puts("usage: wait [job]\n");
        return;
    }

    shell_bg_poll();
    for (uint32_t i = 0; i < SHELL_BG_JOBS; ++i) {
        if (shell_bg_jobs[i].state == SHELL_BG_FREE) {
            continue;
        }
        if (!wait_all && shell_bg_jobs[i].job_id != (unsigned int)requested) {
            continue;
        }

        matched = 1;
        if (shell_bg_jobs[i].state == SHELL_BG_RUNNING) {
            kernel_task_wait(shell_bg_jobs[i].task_id);
            shell_bg_jobs[i].state = SHELL_BG_DONE;
        }

            console_puts("[");
            console_put_dec64(shell_bg_jobs[i].job_id);
        console_puts("] done    status=");
        console_put_dec64((uint32_t)shell_bg_jobs[i].status);
        console_puts(" ");
        console_puts(shell_bg_jobs[i].line);
        console_puts("\n");
        shell_bg_jobs[i].state = SHELL_BG_FREE;
        shell_bg_jobs[i].job_id = 0u;
        shell_bg_jobs[i].task_id = 0u;
        shell_bg_jobs[i].status = 0;
        shell_bg_jobs[i].line[0] = '\0';
    }

    if (!matched) {
        console_puts("wait: no matching background job\n");
    }
}

static int text_contains_word(const char *text, const char *word) {
    uint32_t word_len = 0;

    while (word[word_len] != '\0') {
        ++word_len;
    }
    if (word_len == 0u) {
        return 0;
    }

    for (uint32_t i = 0; text && text[i] != '\0'; ++i) {
        uint32_t j = 0;
        while (j < word_len && text[i + j] == word[j]) {
            ++j;
        }
        if (j == word_len) {
            return 1;
        }
    }
    return 0;
}

void shell_boot_mode_load(void) {
    uint32_t size = 0;
    int drive = active_drive();
    int status;

    shell_boot_safe_mode = 0;
    shell_boot_debug_mode = 0;
    shell_boot_usb_safe_mode = 0;

    if (drive < 0 || !shell_work_buffers_ready) {
        return;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     LAINFS_ROOT_DIR,
                                     SHELL_BOOTMODE_FILE,
                                     shell_source_buffer,
                                     ASM_SOURCE_SIZE,
                                     &size);
    if (status != 0 || size == 0u) {
        return;
    }

    shell_source_buffer[size] = '\0';
    shell_boot_safe_mode = text_contains_word(shell_source_buffer, "safe");
    shell_boot_debug_mode = text_contains_word(shell_source_buffer, "debug");
    shell_boot_usb_safe_mode = text_contains_word(shell_source_buffer, "usb-safe") ||
                               text_contains_word(shell_source_buffer, "usbsafe") ||
                               text_contains_word(shell_source_buffer, "nousb");

    if (shell_boot_safe_mode) {
        console_puts("bootmode: safe mode active, autoexec will be skipped\n");
    }
    if (shell_boot_debug_mode) {
        console_puts("bootmode: debug diagnostics active\n");
    }
    if (shell_boot_usb_safe_mode) {
        console_puts("bootmode: USB safe mode active, automatic USB init will be skipped\n");
    }
}

int shell_boot_safe_mode_enabled(void) {
    return shell_boot_safe_mode;
}

int shell_boot_debug_mode_enabled(void) {
    return shell_boot_debug_mode;
}

int shell_boot_usb_safe_mode_enabled(void) {
    return shell_boot_usb_safe_mode;
}

static int shell_boot_mode_save_flags(void) {
    char content[64];
    uint32_t pos = 0;
    int drive = active_drive();

    if (drive < 0) {
        return -1;
    }

    if (!shell_boot_safe_mode && !shell_boot_debug_mode && !shell_boot_usb_safe_mode) {
        if (append_text_limited(content, sizeof(content), &pos, "normal\n") != 0) {
            return -1;
        }
    } else {
        if (shell_boot_safe_mode &&
            append_text_limited(content, sizeof(content), &pos, "safe\n") != 0) {
            return -1;
        }
        if (shell_boot_debug_mode &&
            append_text_limited(content, sizeof(content), &pos, "debug\n") != 0) {
            return -1;
        }
        if (shell_boot_usb_safe_mode &&
            append_text_limited(content, sizeof(content), &pos, "usb-safe\n") != 0) {
            return -1;
        }
    }

    return lainfs_save_file_in_dir((char)('A' + drive),
                                   LAINFS_ROOT_DIR,
                                   SHELL_BOOTMODE_FILE,
                                   content,
                                   pos);
}

static void cmd_bootmode(const char *args, const boot_info_t *info) {
    const char *mode = skip_const_spaces(args);
    int old_safe = shell_boot_safe_mode;
    int old_debug = shell_boot_debug_mode;
    int old_usb_safe = shell_boot_usb_safe_mode;

    (void)info;

    if (*mode == '\0') {
        console_puts("bootmode:");
        if (shell_boot_safe_mode) {
            console_puts(" safe");
        }
        if (shell_boot_debug_mode) {
            console_puts(" debug");
        }
        if (shell_boot_usb_safe_mode) {
            console_puts(" usb-safe");
        }
        if (!shell_boot_safe_mode && !shell_boot_debug_mode && !shell_boot_usb_safe_mode) {
            console_puts(" normal");
        }
        console_puts("\nusage: bootmode [normal|safe|debug]\n");
        return;
    }

    if (active_drive() < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (streq(mode, "normal")) {
        shell_boot_safe_mode = 0;
        shell_boot_debug_mode = 0;
        shell_boot_usb_safe_mode = 0;
    } else if (streq(mode, "safe")) {
        shell_boot_safe_mode = 1;
        shell_boot_debug_mode = 0;
    } else if (streq(mode, "debug")) {
        shell_boot_safe_mode = 0;
        shell_boot_debug_mode = 1;
    } else {
        console_puts("usage: bootmode [normal|safe|debug]\n");
        return;
    }

    if (shell_boot_mode_save_flags() != 0) {
        shell_boot_safe_mode = old_safe;
        shell_boot_debug_mode = old_debug;
        shell_boot_usb_safe_mode = old_usb_safe;
        console_puts("bootmode failed: could not save bootmode.cfg\n");
        return;
    }

    console_puts("next boot mode: ");
    console_puts(mode);
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

static void print_lainfs_cache_stats(void) {
    lainfs_cache_stats_t stats;

    lainfs_cache_stats(&stats);
    console_puts("dir valid/dirty: ");
    console_put_dec64(stats.directory_valid);
    console_puts("/");
    console_put_dec64(stats.directory_dirty);
    console_puts("\ndata valid/dirty: ");
    console_put_dec64(stats.data_valid);
    console_puts("/");
    console_put_dec64(stats.data_dirty);
    console_puts("\ndir reads/writes/hits: ");
    console_put_dec64(stats.directory_reads);
    console_puts("/");
    console_put_dec64(stats.directory_writes);
    console_puts("/");
    console_put_dec64(stats.directory_cache_hits);
    console_puts("\ndata reads/writes/hits: ");
    console_put_dec64(stats.data_reads);
    console_puts("/");
    console_put_dec64(stats.data_writes);
    console_puts("/");
    console_put_dec64(stats.data_cache_hits);
    console_puts("\nflushes: ");
    console_put_dec64(stats.flushes);
    console_puts("\n");
}

static const char *lainfs_check_reason_text(uint32_t reason) {
    switch (reason) {
        case LAINFS_CHECK_OK:
            return "ok";
        case LAINFS_CHECK_NO_PARTITION:
            return "drive is not mounted";
        case LAINFS_CHECK_BAD_SUPERBLOCK:
            return "bad or missing lainfs superblock";
        case LAINFS_CHECK_BAD_LAYOUT:
            return "invalid filesystem layout";
        case LAINFS_CHECK_BAD_ENTRY_TYPE:
            return "invalid directory entry type";
        case LAINFS_CHECK_BAD_NAME:
            return "invalid directory entry name";
        case LAINFS_CHECK_BAD_PARENT:
            return "invalid parent directory link";
        case LAINFS_CHECK_BAD_DIRECTORY:
            return "invalid directory entry metadata";
        case LAINFS_CHECK_BAD_FILE_SIZE:
            return "invalid file size";
        case LAINFS_CHECK_BAD_FILE_EXTENT:
            return "invalid file extent";
        case LAINFS_CHECK_OVERLAPPING_EXTENTS:
            return "overlapping file extents";
        default:
            return "unknown validation failure";
    }
}

static const char *lainfs_repair_status_text(int status, uint32_t repairs) {
    if (status == 0) {
        return "repaired";
    }
    if (status == -1) {
        return repairs == 0 ? "metadata write/read failed" : "metadata flush failed after repairs";
    }
    if (status == -2) {
        return "no safe automatic repair";
    }
    if (status == -3) {
        return "partially repaired";
    }
    if (status == -4) {
        return "repair limit reached";
    }
    return "repair failed";
}

static void print_lainfs_entry_detail(char drive_letter, uint32_t entry_id) {
    lainfs_entry_detail_t detail;

    if (entry_id == 0 || lainfs_entry_detail(drive_letter, entry_id, &detail) != 0) {
        return;
    }

    console_puts("\nentry ");
    console_put_dec64(detail.entry_id);
    console_puts(": name=");
    console_puts(detail.name[0] ? detail.name : "?");
    console_puts(" type=");
    if (detail.type == LAINFS_ENTRY_TYPE_DIR) {
        console_puts("dir");
    } else if (detail.type == LAINFS_ENTRY_TYPE_FILE) {
        console_puts("file");
    } else {
        console_put_dec64(detail.type);
    }
    console_puts(" parent=");
    console_put_dec64(detail.parent_id);
    console_puts(" size=");
    console_put_dec64(detail.size);

    if (detail.type == LAINFS_ENTRY_TYPE_FILE) {
        if (detail.extent_valid) {
            console_puts(" extents=");
            console_put_dec64(detail.extent_count);
            for (uint32_t i = 0; i < detail.extent_count; ++i) {
                console_puts(" [");
                console_put_dec64(detail.extent_start_lba[i]);
                console_puts("+");
                console_put_dec64(detail.extent_blocks[i]);
                console_puts("]");
            }
        } else {
            console_puts(" extents=invalid legacy=[");
            console_put_dec64(detail.legacy_start_lba);
            console_puts("+");
            console_put_dec64(detail.legacy_blocks);
            console_puts("]");
        }
    }
    console_puts("\n");
}

static void print_lainfs_mount_check(char drive_letter) {
    const mount_t *mount = storage_get_mount_by_drive(drive_letter);
    uint32_t reason = LAINFS_CHECK_OK;
    uint32_t entry_id = 0;
    int status;
    int drive = to_upper(drive_letter) - 'A';

    if (drive < 0 || drive >= MAX_DRIVES || mount == 0 || !streq(mount->fs_name, "lainfs")) {
        return;
    }

    if (!storage_partition_is_writable(mount->partition_index)) {
        return;
    }

    status = lainfs_check(drive_letter, &reason, &entry_id);
    console_puts("fscheck ");
    print_drive_name(drive);
    console_puts(": ");
    if (status == 0) {
        console_puts("ok\n");
        return;
    }

    console_puts("warning: ");
    console_puts(lainfs_check_reason_text(reason));
    if (entry_id != 0) {
        console_puts(" at entry ");
        console_put_dec64(entry_id);
    }
    console_puts("; run fsrepair ");
    print_drive_name(drive);
    console_puts("\n");
    print_lainfs_entry_detail(drive_letter, entry_id);
}

static void cmd_fscheck(const char *args, const boot_info_t *info) {
    const char *target = skip_const_spaces(args);
    uint32_t reason = LAINFS_CHECK_OK;
    uint32_t entry_id = 0;
    int drive;
    int status;

    (void)info;

    if (*target == '\0') {
        drive = active_drive();
    } else {
        drive = parse_drive_spec(target);
    }

    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: fscheck [drive:]\n");
        return;
    }

    status = lainfs_check((char)('A' + drive), &reason, &entry_id);
    console_puts("fscheck ");
    print_drive_name(drive);
    console_puts(": ");
    if (status == 0) {
        console_puts("ok\n");
        return;
    }

    console_puts(lainfs_check_reason_text(reason));
    if (entry_id != 0) {
        console_puts(" at entry ");
        console_put_dec64(entry_id);
    }
    console_puts("\n");
    print_lainfs_entry_detail((char)('A' + drive), entry_id);
}

static void cmd_fsrepair(const char *args, const boot_info_t *info) {
    const char *target = skip_const_spaces(args);
    uint32_t reason = LAINFS_CHECK_OK;
    uint32_t entry_id = 0;
    uint32_t repairs = 0;
    int drive;
    int status;

    (void)info;

    if (*target == '\0') {
        drive = active_drive();
    } else {
        drive = parse_drive_spec(target);
    }

    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: fsrepair [drive:]\n");
        return;
    }

    status = lainfs_repair((char)('A' + drive), &reason, &entry_id, &repairs);
    console_puts("fsrepair ");
    print_drive_name(drive);
    console_puts(": ");
    if (status == 0) {
        console_puts("ok, repairs=");
        console_put_dec64(repairs);
        console_puts("\n");
        return;
    }

    if (repairs != 0) {
        console_puts(lainfs_repair_status_text(status, repairs));
        console_puts(", repairs=");
        console_put_dec64(repairs);
        console_puts(", remaining ");
    } else {
        console_puts(lainfs_repair_status_text(status, repairs));
        console_puts(": ");
    }
    console_puts(lainfs_check_reason_text(reason));
    if (entry_id != 0) {
        console_puts(" at entry ");
        console_put_dec64(entry_id);
    }
    console_puts("\n");
    print_lainfs_entry_detail((char)('A' + drive), entry_id);
}

static void cmd_fsflush(const char *args, const boot_info_t *info) {
    const char *target = skip_const_spaces(args);
    int status;

    (void)info;

    if (*target == '\0' || streq(target, "all")) {
        status = lainfs_flush_all();
    } else {
        int drive = parse_drive_spec(target);
        if (drive < 0 || drive >= MAX_DRIVES) {
            console_puts("usage: fsflush [drive:|all]\n");
            return;
        }
        status = lainfs_flush((char)('A' + drive));
    }

    if (status != 0) {
        console_puts("fsflush failed\n");
        print_lainfs_cache_stats();
        return;
    }

    console_puts("filesystem cache flushed\n");
    print_lainfs_cache_stats();
}

static void cmd_reboot(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    if (lainfs_flush_all() != 0) {
        console_puts("reboot warning: filesystem cache flush failed\n");
    }
    console_puts("rebooting...\n");
    power_reboot();
}

static void cmd_poweroff(const char *args, const boot_info_t *info) {
    (void)args;

    if (lainfs_flush_all() != 0) {
        console_puts("poweroff warning: filesystem cache flush failed\n");
    }
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
    print_lainfs_mount_check((char)('A' + drive));
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

static int mounted_drive_is_live_ramdisk(char drive_letter) {
    const mount_t *mount = storage_get_mount_by_drive(drive_letter);

    return mount && streq(mount->partition_name, "rd0p1");
}

static int mount_formatted_system_partition(const char *partition_name) {
    const mount_t *system_mount;

    if (!partition_name || streq(partition_name, "rd0p1")) {
        return 0;
    }

    system_mount = storage_get_mount_by_drive('S');
    if (system_mount && !mounted_drive_is_live_ramdisk('S')) {
        return 0;
    }

    if (storage_mount('S', partition_name) != 0) {
        return -1;
    }

    set_mounted_drive('S', "SYSTEM");
    console_puts("mounted ");
    console_puts(partition_name);
    console_puts(" at ");
    print_drive_name('S' - 'A');
    console_puts("\n");
    print_lainfs_mount_check('S');
    refresh_system_autoexec('S');
    return 0;
}

static void clear_drives_for_block_device(uint32_t device_index) {
    for (uint32_t i = 0; i < MAX_DRIVES; ++i) {
        const mount_t *mount = storage_get_mount(i);
        const partition_t *part;

        if (!mount) {
            continue;
        }

        part = storage_get_partition(mount->partition_index);
        if (part && part->device_index == device_index) {
            drives[i].present = 0;
        }
    }
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
    const char *first_failed_path = 0;

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
            if (!first_failed_path) {
                first_failed_path = seed->path;
            }
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
        if (first_failed_path) {
            console_puts(" first=");
            console_puts(first_failed_path);
        }
    }
    console_puts("\n");
}

static const ramdisk_seed_entry_t *find_ramdisk_seed(const char *path) {
    for (uint32_t i = 0; i < RAMDISK_SEED_ENTRY_COUNT; ++i) {
        if (streq(ramdisk_seed_entries[i].path, path)) {
            return &ramdisk_seed_entries[i];
        }
    }

    return 0;
}

static void refresh_system_autoexec(char drive_letter) {
    const ramdisk_seed_entry_t *autoexec = find_ramdisk_seed("examples/autoexec");

    if (!autoexec) {
        return;
    }

    if (lainfs_save_file(drive_letter,
                         "autoexec",
                         (const char *)autoexec->data,
                         autoexec->size) == 0) {
        console_puts("updated autoexec on ");
        print_drive_name((int)(drive_letter - 'A'));
        console_puts("\n");
    } else {
        console_puts("autoexec update failed on ");
        print_drive_name((int)(drive_letter - 'A'));
        console_puts("\n");
    }
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
    print_lainfs_mount_check(drive_letter);
    seed_live_ramdisk(drive_letter);
    refresh_system_autoexec(drive_letter);

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
            print_lainfs_mount_check(drive_letter);
            refresh_system_autoexec(drive_letter);
            if (drive_letter != 'R') {
                create_seeded_live_ramdisk('R', 0);
            }
            return 0;
        }
    }

    if (create_seeded_live_ramdisk('R', 1) == 0) {
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
        mount_formatted_system_partition(target);
        return;
    }

    if (storage_find_block_device(target, 0)) {
        char partition_name[12];
        uint32_t device_index = 0;
        storage_find_block_device(target, &device_index);
        clear_drives_for_block_device(device_index);
        int status = lainfs_format_block_device(target, partition_name, sizeof(partition_name));
        if (status == -2) {
            console_puts("format failed: could not create partition\n");
            return;
        }
        if (status == -4) {
            console_puts("format failed: disk is read-only\n");
            return;
        }
        if (status == -6) {
            console_puts("format failed: disk is too small\n");
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
        mount_formatted_system_partition(partition_name);
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
    const char *path = skip_const_spaces(args);
    int status;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*path == '\0') {
        console_puts("usage: rm path\n");
        return;
    }

    status = shell_api_delete(path);
    if (status == -1 || status == -2) {
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
        resolve_file_path_with_drive(path,
                                     drive,
                                     &drive,
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
        resolve_file_path_with_drive(path,
                                     drive,
                                     &drive,
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

static int shell_http_url_starts_with(const char *text, const char *prefix) {
    uint32_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static int shell_http_make_redirect_url(const char *current_url,
                                        const char *location,
                                        char *out,
                                        uint32_t out_size) {
    uint32_t pos = 0;
    uint32_t scheme_len = 0;
    const char *host_start;
    const char *path_start;
    const char *base_end;

    if (current_url == 0 || location == 0 || out == 0 || out_size == 0 || location[0] == '\0') {
        return -1;
    }
    out[0] = '\0';

    if (shell_http_url_starts_with(location, "http://") ||
        shell_http_url_starts_with(location, "https://")) {
        copy_text_limited(out, out_size, location);
        return out[0] != '\0' ? 0 : -1;
    }

    if (shell_http_url_starts_with(current_url, "https://")) {
        scheme_len = 8;
    } else if (shell_http_url_starts_with(current_url, "http://")) {
        scheme_len = 7;
    } else {
        return -1;
    }

    if (shell_http_url_starts_with(location, "//")) {
        (void)scheme_len;
        if (shell_http_url_starts_with(current_url, "https://")) {
            if (append_text_limited(out, out_size, &pos, "https:") != 0) {
                return -1;
            }
        } else if (append_text_limited(out, out_size, &pos, "http:") != 0) {
            return -1;
        }
        for (uint32_t i = 0; location[i] != '\0' && pos + 1u < out_size; ++i) {
            out[pos++] = location[i];
        }
        out[pos] = '\0';
        return location[0] != '\0' && pos + 1u < out_size ? 0 : -1;
    }

    host_start = current_url + scheme_len;
    path_start = host_start;
    while (*path_start != '\0' && *path_start != '/') {
        ++path_start;
    }

    if (location[0] == '/') {
        for (const char *s = current_url; s < path_start && pos + 1u < out_size; ++s) {
            out[pos++] = *s;
        }
        for (uint32_t i = 0; location[i] != '\0' && pos + 1u < out_size; ++i) {
            out[pos++] = location[i];
        }
        out[pos] = '\0';
        return pos + 1u < out_size ? 0 : -1;
    }

    base_end = path_start;
    if (*path_start == '/') {
        const char *s = path_start;
        base_end = path_start + 1;
        while (*s != '\0') {
            if (*s == '/') {
                base_end = s + 1;
            }
            ++s;
        }
    }

    for (const char *s = current_url; s < base_end && pos + 1u < out_size; ++s) {
        out[pos++] = *s;
    }
    if (*path_start == '\0' && pos + 1u < out_size) {
        out[pos++] = '/';
    }
    for (uint32_t i = 0; location[i] != '\0' && pos + 1u < out_size; ++i) {
        out[pos++] = location[i];
    }
    out[pos] = '\0';
    return pos + 1u < out_size ? 0 : -1;
}

int shell_api_http_get(const char *url, char *buffer, uint32_t capacity) {
    return shell_api_http_get_ex(url, buffer, capacity, 0);
}

int shell_api_http_get_ex(const char *url, char *buffer, uint32_t capacity, net_http_info_t *info) {
    uint32_t size = 0;
    char *current_url = 0;
    char *next_url = 0;
    net_http_info_t local_info;
    net_http_info_t *fetch_info = info != 0 ? info : &local_info;
    int status;
    int result;

    if (info != 0) {
        for (uint32_t i = 0; i < sizeof(*info); ++i) {
            ((uint8_t *)info)[i] = 0;
        }
    }
    if (url == 0 || buffer == 0 || capacity == 0) {
        if (info != 0) {
            info->error = -1;
        }
        return -1;
    }
    if (net_device_count() == 0) {
        if (info != 0) {
            info->error = -2;
        }
        return -2;
    }

    current_url = (char *)kmalloc(NET_HTTP_URL_SIZE);
    next_url = (char *)kmalloc(NET_HTTP_URL_SIZE);
    if (current_url == 0 || next_url == 0) {
        if (info != 0) {
            info->error = -11;
        }
        kfree(current_url);
        kfree(next_url);
        return -11;
    }

    copy_text_limited(current_url, NET_HTTP_URL_SIZE, url);
    for (uint32_t redirects = 0; redirects < 5u; ++redirects) {
        for (uint32_t i = 0; i < sizeof(*fetch_info); ++i) {
            ((uint8_t *)fetch_info)[i] = 0;
        }
        status = net_http_get_ex(0, current_url, buffer, capacity, &size, fetch_info);
        if (status != 0) {
            kfree(current_url);
            kfree(next_url);
            return status;
        }
        if (fetch_info->status_code >= 300u &&
            fetch_info->status_code < 400u &&
            fetch_info->location[0] != '\0') {
            if (shell_http_make_redirect_url(current_url,
                                             fetch_info->location,
                                             next_url,
                                             NET_HTTP_URL_SIZE) != 0) {
                kfree(current_url);
                kfree(next_url);
                return -13;
            }
            copy_text_limited(current_url, NET_HTTP_URL_SIZE, next_url);
            continue;
        }
        break;
    }
    if (fetch_info->status_code >= 300u &&
        fetch_info->status_code < 400u &&
        fetch_info->location[0] != '\0') {
        kfree(current_url);
        kfree(next_url);
        return -13;
    }
    if (size < capacity) {
        buffer[size] = '\0';
    }
    result = (int)size;
    kfree(current_url);
    kfree(next_url);
    return result;
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

static int shell_match_one_star(const char *name, const char *pattern) {
    const char *star = 0;
    const char *suffix;
    uint32_t prefix_len = 0;
    uint32_t suffix_len;
    uint32_t name_len;

    if (name == 0 || pattern == 0) {
        return 0;
    }
    while (pattern[prefix_len] != '\0') {
        if (pattern[prefix_len] == '*') {
            star = pattern + prefix_len;
            break;
        }
        if (name[prefix_len] == '\0' || name[prefix_len] != pattern[prefix_len]) {
            return 0;
        }
        ++prefix_len;
    }
    if (star == 0) {
        return streq(name, pattern);
    }
    name_len = 0;
    while (name[name_len] != '\0') {
        ++name_len;
    }
    suffix = star + 1;
    suffix_len = 0;
    while (suffix[suffix_len] != '\0') {
        ++suffix_len;
    }
    if (name_len < prefix_len + suffix_len) {
        return 0;
    }
    for (uint32_t i = 0; i < suffix_len; ++i) {
        if (name[name_len - suffix_len + i] != suffix[i]) {
            return 0;
        }
    }
    return 1;
}

static int shell_split_glob_source(const char *src_path,
                                   int fallback_drive,
                                   int *out_drive,
                                   uint32_t *out_dir,
                                   char *pattern,
                                   uint32_t pattern_size) {
    const char *path = src_path;
    const char *last_sep = 0;
    const char *s;
    int drive;
    char dir_path[128];

    if (src_path == 0 || out_drive == 0 || out_dir == 0 ||
        pattern == 0 || pattern_size == 0) {
        return -1;
    }
    drive = path_drive_prefix(&path, fallback_drive);
    if (drive < 0) {
        return -1;
    }

    s = path;
    while (*s != '\0') {
        if (path_is_separator(*s)) {
            last_sep = s;
        }
        ++s;
    }

    if (last_sep == 0) {
        copy_text_limited(pattern, pattern_size, path);
        if (resolve_dir_path((char)('A' + drive), cwd_dirs[drive], ".", out_dir) != 0) {
            return -1;
        }
    } else {
        uint32_t dir_len = (uint32_t)(last_sep - path);
        if (dir_len == 0) {
            copy_text_limited(dir_path, sizeof(dir_path), "/");
        } else if (copy_path_part_limited(dir_path, sizeof(dir_path), path, dir_len) != 0) {
            return -1;
        }
        copy_text_limited(pattern, pattern_size, last_sep + 1);
        if (resolve_dir_path((char)('A' + drive), cwd_dirs[drive], dir_path, out_dir) != 0) {
            return -1;
        }
    }

    *out_drive = drive;
    return 0;
}

static int shell_api_copy_file_glob(const char *src_path, const char *dst_path) {
    int active = active_drive();
    int src_drive = -1;
    int dst_drive = -1;
    uint32_t src_dir = LAINFS_ROOT_DIR;
    uint32_t dst_parent = LAINFS_ROOT_DIR;
    uint32_t dst_dir = LAINFS_ROOT_DIR;
    uint32_t child_count = 0;
    uint32_t copied = 0;
    char pattern[32];
    char dst_name[32];
    char child_name[32];

    if (active < 0 ||
        shell_split_glob_source(src_path, active, &src_drive, &src_dir,
                                pattern, sizeof(pattern)) != 0 ||
        resolve_file_path_with_drive(dst_path,
                                     active,
                                     &dst_drive,
                                     &dst_parent,
                                     dst_name,
                                     sizeof(dst_name)) != 0) {
        return -1;
    }
    if (lainfs_find_dir((char)('A' + dst_drive), dst_parent, dst_name, &dst_dir) != 0) {
        if (!streq(dst_name, ".")) {
            return -1;
        }
        dst_dir = dst_parent;
    }
    if (lainfs_child_count((char)('A' + src_drive), src_dir, &child_count) != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < child_count; ++i) {
        uint32_t type = 0;
        uint32_t size = 0;

        if (lainfs_child_info((char)('A' + src_drive),
                              src_dir,
                              i,
                              child_name,
                              sizeof(child_name),
                              &type,
                              &size) != 0 ||
            type != LAINFS_ENTRY_TYPE_FILE ||
            !shell_match_one_star(child_name, pattern)) {
            continue;
        }
        if (lainfs_load_file_in_dir((char)('A' + src_drive),
                                    src_dir,
                                    child_name,
                                    zinclude_buffers[0],
                                    ASM_SOURCE_SIZE,
                                    &size) != 0 ||
            lainfs_save_file_in_dir((char)('A' + dst_drive),
                                    dst_dir,
                                    child_name,
                                    zinclude_buffers[0],
                                    size) != 0) {
            return -1;
        }
        ++copied;
    }

    return copied == 0 ? -1 : 0;
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

    if (src_path != 0 && contains_text(src_path, "*")) {
        return shell_api_copy_file_glob(src_path, dst_path);
    }

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

    console_suppress_current_cpu_push();
    cmd_zclean(command, 0);
    cmd_zbuild(command, 0);
    console_suppress_current_cpu_pop();
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

static int zinstall_manifest_objects_present(int drive,
                                             const char *target_name,
                                             char *manifest,
                                             uint32_t manifest_capacity,
                                             uint32_t install_dir) {
    char manifest_name[32];
    uint32_t manifest_size = 0;
    uint32_t found_count = 0;
    int status;

    if (make_suffixed_name(target_name, ".zbuild", manifest_name, sizeof(manifest_name)) != 0) {
        return -1;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     manifest_name,
                                     manifest,
                                     manifest_capacity,
                                     &manifest_size);
    if (status != 0 || manifest_size >= manifest_capacity) {
        return -1;
    }
    manifest[manifest_size] = '\0';

    for (uint32_t pos = 0; pos < manifest_size;) {
        char *line_start = manifest + pos;
        char *line_text;
        char *source_name;
        char *object_name;
        char object_name_buffer[32];
        uint32_t object_size = 0;

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

        split_first_arg(line_text, &source_name, &object_name);
        if (streq(source_name, "src") ||
            streq(source_name, "source") ||
            streq(source_name, "include") ||
            streq(source_name, "build") ||
            streq(source_name, "output") ||
            streq(source_name, "module") ||
            streq(source_name, "objects-only") ||
            streq(source_name, "test-return") ||
            streq(source_name, "install") ||
            streq(source_name, "install-name")) {
            continue;
        }

        if (*object_name == '\0') {
            if (make_object_name_from_source(source_name,
                                             object_name_buffer,
                                             sizeof(object_name_buffer)) != 0) {
                return -1;
            }
            object_name = object_name_buffer;
        } else {
            char *unused = 0;
            char *first_object_name = object_name;
            split_first_arg(object_name, &first_object_name, &unused);
            if (*unused != '\0') {
                return -1;
            }
            object_name = first_object_name;
        }

        status = lainfs_load_file_in_dir((char)('A' + drive),
                                         install_dir,
                                         object_name,
                                         (char *)exec_buffer,
                                         EXEC_BUFFER_SIZE,
                                         &object_size);
        if (status != 0 || object_size == 0) {
            return -1;
        }

        ++found_count;
    }

    return found_count != 0 ? 0 : -1;
}

int shell_api_zinstall(const char *target) {
    char *manifest = shell_manifest_buffer;
    char command[64];
    char output_name[32];
    char install_name[32];
    char report_name[32];
    uint32_t build_dir = 0;
    uint32_t install_dir = 0;
    uint32_t installed_size = 0;
    int objects_only = 0;
    uint32_t object_count = 0;
    int drive = active_drive();
    unsigned char *saved_exec_buffer = exec_buffer;
    unsigned char *api_exec_buffer = 0;
    int result = -1;

    if (drive < 0 || copy_command_arg(target, command, sizeof(command)) != 0 ||
        make_suffixed_name(command, ".buildlog", report_name, sizeof(report_name)) != 0 ||
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
                           &objects_only,
                           &object_count) != 0) {
        return -1;
    }

    api_exec_buffer = (unsigned char *)kzalloc(EXEC_BUFFER_SIZE);
    if (!api_exec_buffer) {
        return -1;
    }
    exec_buffer = api_exec_buffer;

    console_suppress_current_cpu_push();
    cmd_zinstall(command, 0);
    console_suppress_current_cpu_pop();
    if (objects_only && object_count != 1) {
        (void)build_dir;
        (void)report_name;
        result = zinstall_manifest_objects_present(drive,
                                                   command,
                                                   manifest,
                                                   ASM_SOURCE_SIZE,
                                                   install_dir);
        goto done;
    }

    (void)output_name;
    if (lainfs_load_file_in_dir((char)('A' + drive),
                                install_dir,
                                install_name,
                                zinclude_buffers[0],
                                ASM_SOURCE_SIZE,
                                &installed_size) != 0 ||
        installed_size == 0) {
        goto done;
    }

    result = 0;

done:
    exec_buffer = saved_exec_buffer;
    zero_memory(api_exec_buffer, EXEC_BUFFER_SIZE);
    kfree(api_exec_buffer);
    return result;
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
    console_puts(" udp_txerr=");
    console_put_dec64(debug.udp_tx_errors);
    console_puts(" arp_rx=");
    console_put_dec64(debug.arp_replies);
    console_puts(" arp_bad=");
    console_put_dec64(debug.arp_mismatches);
    console_puts(" rx_arp=");
    console_put_dec64(debug.rx_arp);
    console_puts(" rx_ip=");
    console_put_dec64(debug.rx_ipv4);
    console_puts(" rx_udp=");
    console_put_dec64(debug.rx_udp);
    console_puts(" rx_other=");
    console_put_dec64(debug.rx_other);
    console_puts(" dhcp=");
    console_put_dec64(debug.dhcp_tx);
    console_puts("/");
    console_put_dec64(debug.dhcp_rx);
    console_puts(" dns=");
    console_put_dec64(debug.dns_tx);
    console_puts("/");
    console_put_dec64(debug.dns_rx);
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
    console_puts(" tcpstream tx=");
    console_put_dec64(debug.tcp_stream_tx);
    console_puts(" rx=");
    console_put_dec64(debug.tcp_stream_rx);
    console_puts(" retx=");
    console_put_dec64(debug.tcp_stream_retx);
    console_puts(" txerr=");
    console_put_dec64(debug.tcp_stream_last_error);
    console_puts(" tls_state=0x");
    console_put_hex32(debug.tls_last_state);
    console_puts(" tls_err=");
    console_put_dec64(debug.tls_last_error);
    console_puts(" tls_got=");
    console_put_dec64(debug.tls_last_got);
    console_puts(" tls_body=");
    console_put_dec64(debug.tls_last_body_size);
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

        if (streq(command, "dns")) {
            uint32_t dns = 0;

            if (*index_text == '\0') {
                console_puts("dns=");
                cmd_net_print_ipv4(net_dns_server());
                console_puts("\n");
                return;
            }

            if (*extra != '\0' || net_parse_ipv4_addr(index_text, &dns) != 0) {
                console_puts("usage: net dns [server]\n");
                return;
            }

            net_set_dns_server(dns);
            console_puts("net: dns=");
            cmd_net_print_ipv4(net_dns_server());
            console_puts("\n");
            return;
        }

        if (streq(command, "dhcp")) {
            if (*index_text != '\0' || *extra != '\0') {
                console_puts("usage: net dhcp\n");
                return;
            }

            if (net_device_count() == 0) {
                console_puts("net: no supported network device\n");
                return;
            }

            console_puts("net: dhcp on eth0\n");
            if (net_dhcp_configure(0) != 0) {
                console_puts("net: dhcp failed\n");
                cmd_net_print_debug();
                return;
            }
            console_puts("net: ip=");
            cmd_net_print_ipv4(net_ipv4_address());
            console_puts(" mask=");
            cmd_net_print_ipv4(net_ipv4_netmask());
            console_puts(" gateway=");
            cmd_net_print_ipv4(net_ipv4_gateway());
            console_puts(" dns=");
            cmd_net_print_ipv4(net_dns_server());
            console_puts("\n");
            return;
        }

        if (streq(command, "resolve")) {
            uint32_t ip = 0;

            if (*index_text == '\0' || *extra != '\0') {
                console_puts("usage: net resolve host\n");
                return;
            }

            if (net_device_count() == 0) {
                console_puts("net: no supported network device\n");
                return;
            }

            if (net_dns_resolve(0, index_text, &ip) != 0) {
                console_puts("net: resolve failed\n");
                return;
            }
            console_puts("net: ");
            console_puts(index_text);
            console_puts("=");
            cmd_net_print_ipv4(ip);
            console_puts("\n");
            return;
        }

        if ((!streq(command, "poll") && !streq(command, "send")) ||
            *index_text == '\0' ||
            *extra != '\0' ||
            parse_u64_arg(index_text, &index) != 0) {
            console_puts("usage: net [poll|send] index | net arp ip | net dhcp | net dns [server] | net resolve host | net reset | net ip [address mask [gateway]]\n");
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
    console_puts(" dns=");
    cmd_net_print_ipv4(net_dns_server());
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
        console_puts("usage: wget URL [output]\n");
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
        console_puts("wget failed: use http://host[:port]/path or https://host[:port]/path\n");
        return;
    }
    if (status == -9) {
        console_puts("wget failed: https unavailable\n");
        return;
    }
    if (status == -10) {
        net_debug_info_t debug;
        net_debug_info(&debug);
        console_puts("wget failed: tls handshake failed state=0x");
        console_put_hex32(debug.tls_last_state);
        console_puts(" err=");
        console_put_dec64(debug.tls_last_error);
        console_puts("\n");
        return;
    }
    if (status == -11) {
        console_puts("wget failed: out of memory\n");
        return;
    }
    if (status == -12) {
        console_puts("wget failed: RTC time unavailable for certificate validation\n");
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
    if (status == -8) {
        console_puts("wget failed: dns lookup failed; try net dhcp or net dns server\n");
        return;
    }
    if (status == -5) {
        console_puts("wget failed: tcp connect timed out\n");
        return;
    }
    if (status == -6) {
        net_debug_info_t debug;
        net_debug_info(&debug);
        console_puts("wget failed: http receive timed out state=0x");
        console_put_hex32(debug.tls_last_state);
        console_puts(" err=");
        console_put_dec64(debug.tls_last_error);
        console_puts(" got=");
        console_put_dec64(debug.tls_last_got);
        console_puts(" body=");
        console_put_dec64(debug.tls_last_body_size);
        console_puts("\n");
        return;
    }
    if (status == -4) {
        net_debug_info_t debug;
        net_debug_info(&debug);
        console_puts("wget failed: tls send failed state=0x");
        console_put_hex32(debug.tls_last_state);
        console_puts(" err=");
        console_put_dec64(debug.tls_last_error);
        console_puts(" txerr=");
        console_put_dec64(debug.tcp_stream_last_error);
        console_puts(" retx=");
        console_put_dec64(debug.tcp_stream_retx);
        console_puts("\n");
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

static void cmd_usb_health(void) {
    uint32_t count = usb_controller_count();

    console_puts("USB health: ");
    if (shell_boot_usb_safe_mode) {
        console_puts("usb-safe next boot");
    } else {
        console_puts("normal next boot");
    }
    console_puts("\ncontrollers=");
    console_put_dec64(count);
    console_puts(" xhci=");
    console_put_dec64(usb_xhci_controller_count());
    console_puts("\n");

    if (count == 0u) {
        console_puts(shell_boot_usb_safe_mode ? "  automatic USB init skipped\n" : "  no USB controllers detected\n");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const usb_controller_info_t *ctrl = usb_controller_info(i);

        if (ctrl == 0) {
            continue;
        }

        console_puts("  #");
        console_put_dec64(i);
        console_puts(" ");
        console_puts(usb_controller_type_name(ctrl->type));
        console_puts(" pci=");
        console_put_dec64(ctrl->bus);
        console_puts(":");
        console_put_dec64(ctrl->device);
        console_puts(".");
        console_put_dec64(ctrl->function);
        console_puts(" ");
        if (ctrl->type != USB_CONTROLLER_XHCI) {
            console_puts("detected");
        } else if (ctrl->running) {
            console_puts("running");
        } else if (ctrl->initialized) {
            console_puts("initialized-stopped");
        } else {
            console_puts("detected-not-started");
        }
        if (ctrl->connected_port_count != 0u) {
            console_puts(" ports-connected=");
            console_put_dec64(ctrl->connected_port_count);
        }
        if (ctrl->mouse_configured) {
            console_puts(" mouse=ok reports=");
            console_put_dec64(ctrl->mouse_report_count);
        } else if (ctrl->type == USB_CONTROLLER_XHCI && ctrl->enum_stage != 0u) {
            console_puts(" enum-stage=");
            console_put_dec64(ctrl->enum_stage);
            console_puts(" cc=");
            console_put_dec64(ctrl->enum_completion_code);
        }
        if (ctrl->last_completion_code != 0u) {
            console_puts(" last-cc=");
            console_put_dec64(ctrl->last_completion_code);
        }
        console_puts("\n");
    }
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
        if (streq(command, "health")) {
            if (*index_text != '\0' || *extra != '\0') {
                console_puts("usage: usb health\n");
                return;
            }
            cmd_usb_health();
            return;
        }
        if (streq(command, "safe")) {
            if (streq(index_text, "on") && *extra == '\0') {
                int old_usb_safe = shell_boot_usb_safe_mode;
                shell_boot_usb_safe_mode = 1;
                if (shell_boot_mode_save_flags() != 0) {
                    shell_boot_usb_safe_mode = old_usb_safe;
                    console_puts("usb safe failed: could not save bootmode.cfg\n");
                    return;
                }
                console_puts("usb safe: on for next boot\n");
                return;
            }
            if (streq(index_text, "off") && *extra == '\0') {
                int old_usb_safe = shell_boot_usb_safe_mode;
                shell_boot_usb_safe_mode = 0;
                if (shell_boot_mode_save_flags() != 0) {
                    shell_boot_usb_safe_mode = old_usb_safe;
                    console_puts("usb safe failed: could not save bootmode.cfg\n");
                    return;
                }
                console_puts("usb safe: off for next boot\n");
                return;
            }
            if ((streq(index_text, "status") || *index_text == '\0') && *extra == '\0') {
                console_puts("usb safe: ");
                console_puts(shell_boot_usb_safe_mode ? "on\n" : "off\n");
                return;
            }
            console_puts("usage: usb safe [on|off|status]\n");
            return;
        }
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
            console_puts("usage: usb health | usb safe [on|off|status] | usb [scan|init|handoff|halt|reset|rings|bm|nobm|poke|pokenodma|run|status|start|enum] index\n");
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

static void shell_put_2digits(unsigned int value) {
    if (value < 10u) {
        console_puts("0");
    }
    console_put_dec64(value);
}

static void cmd_date(const char *args, const boot_info_t *info) {
    rtc_time_t now;

    (void)args;
    (void)info;

    if (clock_get_rtc_time(&now) != 0) {
        console_puts("date failed: RTC time unavailable\n");
        return;
    }

    console_put_dec64(now.year);
    console_puts("-");
    shell_put_2digits(now.month);
    console_puts("-");
    shell_put_2digits(now.day);
    console_puts(" ");
    shell_put_2digits(now.hour);
    console_puts(":");
    shell_put_2digits(now.minute);
    console_puts(":");
    shell_put_2digits(now.second);
    console_puts(" UTC unix=");
    console_put_dec64(clock_unix_time_from_rtc(&now));
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

static void print_zscript_compile_failure(const char *command, const char *source_name, uint32_t error_line) {
    console_puts(command);
    console_puts(" failed: ");
    if (source_name != 0 && source_name[0] != '\0') {
        console_puts(source_name);
        console_puts(" ");
    }
    if (zscript_last_error() == ZSCRIPT_ERROR_OUTPUT_FULL) {
        console_puts("generated asm exceeded Z build buffer");
    } else {
        console_puts("unsupported .Z syntax");
    }
    if (error_line != 0) {
        console_puts(" on line ");
        console_put_dec64(error_line);
    }
    console_puts("\n");
}

static void print_zscript_source_line(const char *source, uint32_t source_size, uint32_t line_number) {
    uint32_t pos = 0;
    uint32_t current_line = 1;
    uint32_t target_line;

    if (source == 0 || source_size == 0u || line_number == 0u) {
        return;
    }
    target_line = line_number > 2u ? line_number - 2u : 1u;

    while (pos < source_size && current_line < target_line) {
        if (source[pos] == '\n') {
            ++current_line;
        }
        ++pos;
    }
    if (current_line != target_line || pos >= source_size) {
        return;
    }

    while (pos < source_size && current_line <= line_number + 2u) {
        console_puts(current_line == line_number ? "generated line: " : "generated context: ");
        console_put_dec64(current_line);
        console_puts(": ");
        while (pos < source_size && source[pos] != '\n') {
            char ch = source[pos++];
            if (ch == '\t') {
                console_puts("    ");
            } else {
                char text[2];
                text[0] = ch;
                text[1] = '\0';
                console_puts(text);
            }
        }
        console_puts("\n");
        if (pos < source_size && source[pos] == '\n') {
            ++pos;
        }
        ++current_line;
    }
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
        print_zscript_compile_failure("zc", 0, compile_error_line);
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

static int zcc_emit_hubbub_errors_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "export uint8_t *hubbub_error_to_string(int error) {\n"
        "    if (error == 0) {\n"
        "        return \"No error\";\n"
        "    }\n"
        "    if (error == 1) {\n"
        "        return \"Internal (reprocess token)\";\n"
        "    }\n"
        "    if (error == 2) {\n"
        "        return \"Encoding of document has changed\";\n"
        "    }\n"
        "    if (error == 3) {\n"
        "        return \"Parser is paused\";\n"
        "    }\n"
        "    if (error == 5) {\n"
        "        return \"Insufficient memory\";\n"
        "    }\n"
        "    if (error == 6) {\n"
        "        return \"Bad parameter\";\n"
        "    }\n"
        "    if (error == 7) {\n"
        "        return \"Invalid input\";\n"
        "    }\n"
        "    if (error == 8) {\n"
        "        return \"File not found\";\n"
        "    }\n"
        "    if (error == 9) {\n"
        "        return \"Insufficient data\";\n"
        "    }\n"
        "    if (error == 10) {\n"
        "        return \"Unsupported charset\";\n"
        "    }\n"
        "    if (error == 11) {\n"
        "        return \"Unknown error\";\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_hubbub_string_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "uint8_t hubbub_ascii_upper(uint8_t c) {\n"
        "    if (c >= 97 && c <= 122) {\n"
        "        return c - 32;\n"
        "    }\n"
        "    return c;\n"
        "}\n"
        "\n"
        "export int hubbub_string_match(const uint8_t *a,\n"
        "                               uint64_t a_len,\n"
        "                               const uint8_t *b,\n"
        "                               uint64_t b_len) {\n"
        "    uint64_t i;\n"
        "\n"
        "    if (a_len != b_len) {\n"
        "        return 0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < b_len) {\n"
        "        if (a[i] != b[i]) {\n"
        "            return 0;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "export int hubbub_string_match_ci(const uint8_t *a,\n"
        "                                  uint64_t a_len,\n"
        "                                  const uint8_t *b,\n"
        "                                  uint64_t b_len) {\n"
        "    uint64_t i;\n"
        "    uint8_t aa;\n"
        "    uint8_t bb;\n"
        "\n"
        "    if (a_len != b_len) {\n"
        "        return 0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < b_len) {\n"
        "        aa = hubbub_ascii_upper(a[i]);\n"
        "        bb = hubbub_ascii_upper(b[i]);\n"
        "        if (aa != bb) {\n"
        "            return 0;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_hubbub_detect_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "enum {\n"
        "    PARSERUTILS_OK = 0,\n"
        "    PARSERUTILS_BADPARM = 2,\n"
        "    HUBBUB_CHARSET_UNKNOWN = 0,\n"
        "    HUBBUB_CHARSET_TENTATIVE = 1,\n"
        "    HUBBUB_CHARSET_CONFIDENT = 2,\n"
        "    MIB_ISO_8859_1 = 4,\n"
        "    MIB_UTF_8 = 106,\n"
        "    MIB_UTF_16 = 1015,\n"
        "    MIB_UTF_16BE = 1013,\n"
        "    MIB_UTF_16LE = 1014,\n"
        "    MIB_UTF_32 = 1017,\n"
        "    MIB_UTF_32BE = 1018,\n"
        "    MIB_UTF_32LE = 1019,\n"
        "    MIB_WINDOWS_1252 = 2252\n"
        "};\n"
        "\n"
        "uint8_t hubbub_detect_upper(uint8_t c) {\n"
        "    if (c >= 97 && c <= 122) {\n"
        "        return c - 32;\n"
        "    }\n"
        "    return c;\n"
        "}\n"
        "\n"
        "int hubbub_detect_space(uint8_t c) {\n"
        "    if (c == 9 || c == 10 || c == 12 || c == 13 || c == 32 || c == 47) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int hubbub_detect_match_ci(const uint8_t *s,\n"
        "                           uint64_t off,\n"
        "                           uint64_t len,\n"
        "                           const uint8_t *pat,\n"
        "                           uint64_t pat_len) {\n"
        "    uint64_t i;\n"
        "    if (off + pat_len > len) {\n"
        "        return 0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < pat_len) {\n"
        "        if (hubbub_detect_upper(s[off + i]) != hubbub_detect_upper(pat[i])) {\n"
        "            return 0;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "uint16_t hubbub_detect_mibenum_from_name(const uint8_t *name, uint64_t len) {\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-8\", 5) != 0 && len == 5) {\n"
        "        return MIB_UTF_8;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-16BE\", 8) != 0 && len == 8) {\n"
        "        return MIB_UTF_16BE;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-16LE\", 8) != 0 && len == 8) {\n"
        "        return MIB_UTF_16LE;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-16\", 6) != 0 && len == 6) {\n"
        "        return MIB_UTF_16;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-32BE\", 8) != 0 && len == 8) {\n"
        "        return MIB_UTF_32BE;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-32LE\", 8) != 0 && len == 8) {\n"
        "        return MIB_UTF_32LE;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"UTF-32\", 6) != 0 && len == 6) {\n"
        "        return MIB_UTF_32;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"ISO-8859-1\", 10) != 0 && len == 10) {\n"
        "        return MIB_ISO_8859_1;\n"
        "    }\n"
        "    if (hubbub_detect_match_ci(name, 0, len, \"Windows-1252\", 12) != 0 && len == 12) {\n"
        "        return MIB_WINDOWS_1252;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint16_t hubbub_charset_read_bom(const uint8_t *data, uint64_t len) {\n"
        "    if (data == 0 || len < 3) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (data[0] == 254 && data[1] == 255) {\n"
        "        return MIB_UTF_16BE;\n"
        "    }\n"
        "    if (data[0] == 255 && data[1] == 254) {\n"
        "        return MIB_UTF_16LE;\n"
        "    }\n"
        "    if (data[0] == 239 && data[1] == 187 && data[2] == 191) {\n"
        "        return MIB_UTF_8;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export uint16_t hubbub_charset_parse_content(const uint8_t *value,\n"
        "                                             uint32_t valuelen) {\n"
        "    uint32_t i;\n"
        "    uint32_t start;\n"
        "    uint32_t end;\n"
        "    uint8_t quote;\n"
        "\n"
        "    if (value == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i + 7 < valuelen) {\n"
        "        if (hubbub_detect_match_ci(value, i, valuelen, \"charset\", 7) != 0) {\n"
        "            i = i + 7;\n"
        "            while (i < valuelen && hubbub_detect_space(value[i]) != 0) {\n"
        "                i = i + 1;\n"
        "            }\n"
        "            if (i >= valuelen || value[i] != 61) {\n"
        "                return 0;\n"
        "            }\n"
        "            i = i + 1;\n"
        "            while (i < valuelen && hubbub_detect_space(value[i]) != 0) {\n"
        "                i = i + 1;\n"
        "            }\n"
        "            if (i >= valuelen) {\n"
        "                return 0;\n"
        "            }\n"
        "            quote = 0;\n"
        "            if (value[i] == 34 || value[i] == 39) {\n"
        "                quote = value[i];\n"
        "                i = i + 1;\n"
        "            }\n"
        "            start = i;\n"
        "            while (i < valuelen) {\n"
        "                if (quote != 0) {\n"
        "                    if (value[i] == quote) {\n"
        "                        break;\n"
        "                    }\n"
        "                } else if (hubbub_detect_space(value[i]) != 0 || value[i] == 59) {\n"
        "                    break;\n"
        "                }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            end = i;\n"
        "            return hubbub_detect_mibenum_from_name(value + start, end - start);\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint16_t hubbub_charset_scan_meta(const uint8_t *data, uint64_t len) {\n"
        "    uint64_t i;\n"
        "    uint64_t max;\n"
        "    uint64_t j;\n"
        "    if (data == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    max = len;\n"
        "    if (max > 512) {\n"
        "        max = 512;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i + 7 < max) {\n"
        "        if (hubbub_detect_match_ci(data, i, max, \"charset\", 7) != 0) {\n"
        "            j = i;\n"
        "            while (j < max && data[j] != 62) {\n"
        "                j = j + 1;\n"
        "            }\n"
        "            return hubbub_charset_parse_content(data + i, (uint32_t)(j - i));\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export void hubbub_charset_fix_charset(uint16_t *charset) {\n"
        "    if (charset == 0) {\n"
        "        return;\n"
        "    }\n"
        "    if (*charset == MIB_ISO_8859_1) {\n"
        "        *charset = MIB_WINDOWS_1252;\n"
        "    }\n"
        "}\n"
        "\n"
        "export int hubbub_charset_extract(const uint8_t *data,\n"
        "                                  uint64_t len,\n"
        "                                  uint16_t *mibenum,\n"
        "                                  uint32_t *source) {\n"
        "    uint16_t charset;\n"
        "\n"
        "    if (data == 0 || mibenum == 0 || source == 0) {\n"
        "        return PARSERUTILS_BADPARM;\n"
        "    }\n"
        "    if (*source == HUBBUB_CHARSET_CONFIDENT || *source == HUBBUB_CHARSET_TENTATIVE) {\n"
        "        return PARSERUTILS_OK;\n"
        "    }\n"
        "    charset = hubbub_charset_read_bom(data, len);\n"
        "    if (charset != 0) {\n"
        "        *mibenum = charset;\n"
        "        *source = HUBBUB_CHARSET_CONFIDENT;\n"
        "        return PARSERUTILS_OK;\n"
        "    }\n"
        "    charset = hubbub_charset_scan_meta(data, len);\n"
        "    if (charset != 0) {\n"
        "        hubbub_charset_fix_charset(&charset);\n"
        "        if (charset != MIB_UTF_32 && charset != MIB_UTF_32BE && charset != MIB_UTF_32LE) {\n"
        "            *mibenum = charset;\n"
        "            *source = HUBBUB_CHARSET_TENTATIVE;\n"
        "            return PARSERUTILS_OK;\n"
        "        }\n"
        "    }\n"
        "    *mibenum = MIB_WINDOWS_1252;\n"
        "    *source = HUBBUB_CHARSET_TENTATIVE;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_string_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct lwc_string_s {\n"
        "    struct lwc_string_s *next;\n"
        "    uint64_t len;\n"
        "    uint32_t hash;\n"
        "    uint32_t refcnt;\n"
        "};\n"
        "\n"
        "typedef struct lwc_string_s lwc_string;\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "extern int lwc_intern_string(const uint8_t *s, uint64_t slen, lwc_string **ret);\n"
        "extern lwc_string *lwc_string_ref(lwc_string *str);\n"
        "extern void lwc_string_unref(lwc_string *str);\n"
        "extern uint8_t *lwc_string_data(lwc_string *str);\n"
        "extern uint64_t lwc_string_length(lwc_string *str);\n"
        "extern uint32_t lwc_string_hash_value(lwc_string *str);\n"
        "extern int lwc_string_isequal(lwc_string *a, lwc_string *b, int *ret);\n"
        "extern int lwc_string_caseless_isequal(lwc_string *a, lwc_string *b, int *ret);\n"
        "extern int parserutils_charset_utf8_length(const uint8_t *s, uint64_t max, uint64_t *len);\n"
        "extern int parserutils_charset_utf8_next(const uint8_t *s, uint32_t len, uint32_t off, uint32_t *nextoff);\n"
        "extern int parserutils_charset_utf8_to_ucs4(const uint8_t *s, uint64_t len, uint32_t *ucs4, uint64_t *clen);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_INDEX_SIZE_ERR = 1,\n"
        "    DOM_NOT_SUPPORTED_ERR = 9,\n"
        "    DOM_NO_MEM_ERR = 131072,\n"
        "    DOM_STRING_CDATA = 0,\n"
        "    DOM_STRING_INTERNED = 1\n"
        "};\n"
        "\n"
        "uint8_t dom_lower(uint8_t c) {\n"
        "    if (c >= 65 && c <= 90) {\n"
        "        return c + 32;\n"
        "    }\n"
        "    return c;\n"
        "}\n"
        "\n"
        "int dom_lwc_error_to_exception(int err) {\n"
        "    if (err == 0) {\n"
        "        return DOM_NO_ERR;\n"
        "    }\n"
        "    if (err == 2) {\n"
        "        return DOM_INDEX_SIZE_ERR;\n"
        "    }\n"
        "    return DOM_NO_MEM_ERR;\n"
        "}\n"
        "\n"
        "export uint8_t *dom_string_data(const dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return \"\";\n"
        "    }\n"
        "    if (str->type == DOM_STRING_INTERNED) {\n"
        "        return lwc_string_data((lwc_string *)str->ptr);\n"
        "    }\n"
        "    return str->ptr;\n"
        "}\n"
        "\n"
        "export uint64_t dom_string_byte_length(const dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (str->type == DOM_STRING_INTERNED) {\n"
        "        return lwc_string_length((lwc_string *)str->ptr);\n"
        "    }\n"
        "    return str->len;\n"
        "}\n"
        "\n"
        "void dom_string_destroy(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return;\n"
        "    }\n"
        "    if (str->type == DOM_STRING_INTERNED) {\n"
        "        lwc_string_unref((lwc_string *)str->ptr);\n"
        "    } else if (str->ptr != 0) {\n"
        "        free(str->ptr);\n"
        "    }\n"
        "    free((uint8_t *)str);\n"
        "}\n"
        "\n"
        "export dom_string *dom_string_ref(dom_string *str) {\n"
        "    if (str != 0) {\n"
        "        str->refcnt = str->refcnt + 1;\n"
        "    }\n"
        "    return str;\n"
        "}\n"
        "\n"
        "export void dom_string_unref(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return;\n"
        "    }\n"
        "    if (str->refcnt > 0) {\n"
        "        str->refcnt = str->refcnt - 1;\n"
        "    }\n"
        "    if (str->refcnt == 0) {\n"
        "        dom_string_destroy(str);\n"
        "    }\n"
        "}\n"
        "\n"
        "int dom_string_alloc_cdata(const uint8_t *ptr, uint64_t len, dom_string **out) {\n"
        "    dom_string *ret;\n"
        "    uint8_t *buf;\n"
        "    uint64_t i;\n"
        "    if (out == 0 || (ptr == 0 && len != 0)) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    ret = (dom_string *)malloc(24);\n"
        "    if (ret == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    buf = malloc(len + 1);\n"
        "    if (buf == 0) {\n"
        "        free((uint8_t *)ret);\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        buf[i] = ptr[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    buf[len] = 0;\n"
        "    ret->refcnt = 1;\n"
        "    ret->type = DOM_STRING_CDATA;\n"
        "    ret->len = len;\n"
        "    ret->ptr = buf;\n"
        "    *out = ret;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str) {\n"
        "    if (ptr == 0) {\n"
        "        len = 0;\n"
        "        ptr = \"\";\n"
        "    }\n"
        "    return dom_string_alloc_cdata(ptr, len, str);\n"
        "}\n"
        "\n"
        "export int dom_string_create_interned(const uint8_t *ptr, uint64_t len, dom_string **str) {\n"
        "    dom_string *ret;\n"
        "    lwc_string *intern;\n"
        "    int rc;\n"
        "    if (str == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    if (ptr == 0) {\n"
        "        ptr = \"\";\n"
        "        len = 0;\n"
        "    }\n"
        "    rc = lwc_intern_string(ptr, len, &intern);\n"
        "    if (rc != 0) {\n"
        "        return dom_lwc_error_to_exception(rc);\n"
        "    }\n"
        "    ret = (dom_string *)malloc(24);\n"
        "    if (ret == 0) {\n"
        "        lwc_string_unref(intern);\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    ret->refcnt = 1;\n"
        "    ret->type = DOM_STRING_INTERNED;\n"
        "    ret->len = len;\n"
        "    ret->ptr = (uint8_t *)intern;\n"
        "    *str = ret;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_string_intern(dom_string *str, lwc_string **lwcstr) {\n"
        "    lwc_string *intern;\n"
        "    int rc;\n"
        "    if (str == 0 || lwcstr == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    if (str->type != DOM_STRING_INTERNED) {\n"
        "        rc = lwc_intern_string(str->ptr, str->len, &intern);\n"
        "        if (rc != 0) {\n"
        "            return dom_lwc_error_to_exception(rc);\n"
        "        }\n"
        "        free(str->ptr);\n"
        "        str->ptr = (uint8_t *)intern;\n"
        "        str->type = DOM_STRING_INTERNED;\n"
        "    }\n"
        "    *lwcstr = lwc_string_ref((lwc_string *)str->ptr);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int dom_bytes_equal(const uint8_t *a, const uint8_t *b, uint64_t len, int fold) {\n"
        "    uint64_t i;\n"
        "    uint8_t aa;\n"
        "    uint8_t bb;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        aa = a[i];\n"
        "        bb = b[i];\n"
        "        if (fold != 0) {\n"
        "            aa = dom_lower(aa);\n"
        "            bb = dom_lower(bb);\n"
        "        }\n"
        "        if (aa != bb) {\n"
        "            return 0;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "export int dom_string_isequal(const dom_string *s1, const dom_string *s2) {\n"
        "    uint64_t len;\n"
        "    int match;\n"
        "    if (s1 == 0 || s2 == 0) {\n"
        "        if (dom_string_byte_length(s1) == dom_string_byte_length(s2)) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    if (s1->type == DOM_STRING_INTERNED && s2->type == DOM_STRING_INTERNED) {\n"
        "        match = 0;\n"
        "        lwc_string_isequal((lwc_string *)s1->ptr, (lwc_string *)s2->ptr, &match);\n"
        "        return match;\n"
        "    }\n"
        "    len = dom_string_byte_length(s1);\n"
        "    if (len != dom_string_byte_length(s2)) {\n"
        "        return 0;\n"
        "    }\n"
        "    return dom_bytes_equal(dom_string_data(s1), dom_string_data(s2), len, 0);\n"
        "}\n"
        "\n"
        "export int dom_string_caseless_isequal(const dom_string *s1, const dom_string *s2) {\n"
        "    uint64_t len;\n"
        "    int match;\n"
        "    if (s1 == 0 || s2 == 0) {\n"
        "        if (dom_string_byte_length(s1) == dom_string_byte_length(s2)) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    if (s1->type == DOM_STRING_INTERNED && s2->type == DOM_STRING_INTERNED) {\n"
        "        match = 0;\n"
        "        lwc_string_caseless_isequal((lwc_string *)s1->ptr, (lwc_string *)s2->ptr, &match);\n"
        "        return match;\n"
        "    }\n"
        "    len = dom_string_byte_length(s1);\n"
        "    if (len != dom_string_byte_length(s2)) {\n"
        "        return 0;\n"
        "    }\n"
        "    return dom_bytes_equal(dom_string_data(s1), dom_string_data(s2), len, 1);\n"
        "}\n"
        "\n"
        "export int dom_string_lwc_isequal(const dom_string *s1, lwc_string *s2) {\n"
        "    dom_string temp;\n"
        "    temp.refcnt = 1;\n"
        "    temp.type = DOM_STRING_INTERNED;\n"
        "    temp.len = lwc_string_length(s2);\n"
        "    temp.ptr = (uint8_t *)s2;\n"
        "    return dom_string_isequal(s1, &temp);\n"
        "}\n"
        "\n"
        "export int dom_string_caseless_lwc_isequal(const dom_string *s1, lwc_string *s2) {\n"
        "    dom_string temp;\n"
        "    temp.refcnt = 1;\n"
        "    temp.type = DOM_STRING_INTERNED;\n"
        "    temp.len = lwc_string_length(s2);\n"
        "    temp.ptr = (uint8_t *)s2;\n"
        "    return dom_string_caseless_isequal(s1, &temp);\n"
        "}\n"
        "\n"
        "export uint32_t dom_string_length(dom_string *str) {\n"
        "    uint64_t len;\n"
        "    if (parserutils_charset_utf8_length(dom_string_data(str), dom_string_byte_length(str), &len) != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return (uint32_t)len;\n"
        "}\n"
        "\n"
        "export uint32_t dom_string_index(dom_string *str, uint32_t chr) {\n"
        "    const uint8_t *data;\n"
        "    uint64_t bytes;\n"
        "    uint32_t off;\n"
        "    uint32_t next;\n"
        "    uint32_t idx;\n"
        "    uint32_t ucs4;\n"
        "    uint64_t clen;\n"
        "    data = dom_string_data(str);\n"
        "    bytes = dom_string_byte_length(str);\n"
        "    off = 0;\n"
        "    idx = 0;\n"
        "    while (off < bytes) {\n"
        "        clen = bytes - off;\n"
        "        if (parserutils_charset_utf8_to_ucs4(data + off, clen, &ucs4, &clen) != 0) {\n"
        "            return 4294967295;\n"
        "        }\n"
        "        if (ucs4 == chr) {\n"
        "            return idx;\n"
        "        }\n"
        "        if (parserutils_charset_utf8_next(data, (uint32_t)bytes, off, &next) != 0) {\n"
        "            return 4294967295;\n"
        "        }\n"
        "        off = next;\n"
        "        idx = idx + 1;\n"
        "    }\n"
        "    return 4294967295;\n"
        "}\n"
        "\n"
        "export int dom_string_at(dom_string *str, uint32_t index, uint32_t *ch) {\n"
        "    const uint8_t *data;\n"
        "    uint64_t bytes;\n"
        "    uint32_t off;\n"
        "    uint32_t next;\n"
        "    uint32_t idx;\n"
        "    uint64_t clen;\n"
        "    data = dom_string_data(str);\n"
        "    bytes = dom_string_byte_length(str);\n"
        "    off = 0;\n"
        "    idx = 0;\n"
        "    while (off < bytes) {\n"
        "        if (idx == index) {\n"
        "            clen = bytes - off;\n"
        "            return parserutils_charset_utf8_to_ucs4(data + off, clen, ch, &clen);\n"
        "        }\n"
        "        if (parserutils_charset_utf8_next(data, (uint32_t)bytes, off, &next) != 0) {\n"
        "            return DOM_INDEX_SIZE_ERR;\n"
        "        }\n"
        "        off = next;\n"
        "        idx = idx + 1;\n"
        "    }\n"
        "    return DOM_INDEX_SIZE_ERR;\n"
        "}\n"
        "\n"
        "export int dom_string_concat(dom_string *s1, dom_string *s2, dom_string **result) {\n"
        "    uint64_t len1;\n"
        "    uint64_t len2;\n"
        "    uint8_t *buf;\n"
        "    uint8_t *data1;\n"
        "    uint8_t *data2;\n"
        "    uint64_t i;\n"
        "    int rc;\n"
        "    len1 = dom_string_byte_length(s1);\n"
        "    len2 = dom_string_byte_length(s2);\n"
        "    data1 = dom_string_data(s1);\n"
        "    data2 = dom_string_data(s2);\n"
        "    buf = malloc(len1 + len2 + 1);\n"
        "    if (buf == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len1) {\n"
        "        buf[i] = data1[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len2) {\n"
        "        buf[len1 + i] = data2[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    buf[len1 + len2] = 0;\n"
        "    rc = dom_string_alloc_cdata(buf, len1 + len2, result);\n"
        "    free(buf);\n"
        "    return rc;\n"
        "}\n"
        "\n"
        "uint32_t dom_byte_offset_for_char(dom_string *str, uint32_t index) {\n"
        "    const uint8_t *data;\n"
        "    uint32_t bytes;\n"
        "    uint32_t off;\n"
        "    uint32_t next;\n"
        "    uint32_t idx;\n"
        "    data = dom_string_data(str);\n"
        "    bytes = (uint32_t)dom_string_byte_length(str);\n"
        "    off = 0;\n"
        "    idx = 0;\n"
        "    while (idx < index && off < bytes) {\n"
        "        if (parserutils_charset_utf8_next(data, bytes, off, &next) != 0) {\n"
        "            return bytes;\n"
        "        }\n"
        "        off = next;\n"
        "        idx = idx + 1;\n"
        "    }\n"
        "    return off;\n"
        "}\n"
        "\n"
        "export int dom_string_substr(dom_string *str, uint32_t i1, uint32_t i2, dom_string **result) {\n"
        "    uint32_t start;\n"
        "    uint32_t end;\n"
        "    uint32_t bytes;\n"
        "    uint8_t *data;\n"
        "    bytes = (uint32_t)dom_string_byte_length(str);\n"
        "    start = dom_byte_offset_for_char(str, i1);\n"
        "    end = dom_byte_offset_for_char(str, i2);\n"
        "    if (start > bytes) {\n"
        "        start = bytes;\n"
        "    }\n"
        "    if (end > bytes) {\n"
        "        end = bytes;\n"
        "    }\n"
        "    if (end < start) {\n"
        "        end = start;\n"
        "    }\n"
        "    data = dom_string_data(str);\n"
        "    return dom_string_alloc_cdata(data + start, end - start, result);\n"
        "}\n"
        "\n"
        "export uint32_t dom_string_hash(dom_string *str) {\n"
        "    const uint8_t *data;\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint32_t h;\n"
        "    if (str != 0 && str->type == DOM_STRING_INTERNED) {\n"
        "        return lwc_string_hash_value((lwc_string *)str->ptr);\n"
        "    }\n"
        "    data = dom_string_data(str);\n"
        "    len = dom_string_byte_length(str);\n"
        "    h = 2166136261;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        h = h * 16777619;\n"
        "        h = h ^ data[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return h;\n"
        "}\n"
        "\n"
        "int dom_string_ascii_case_map(dom_string *source, dom_string **result, int lower) {\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint8_t *buf;\n"
        "    uint8_t *data;\n"
        "    uint8_t c;\n"
        "    int rc;\n"
        "    len = dom_string_byte_length(source);\n"
        "    data = dom_string_data(source);\n"
        "    buf = malloc(len + 1);\n"
        "    if (buf == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        c = data[i];\n"
        "        if (lower != 0) {\n"
        "            c = dom_lower(c);\n"
        "        } else if (c >= 97 && c <= 122) {\n"
        "            c = c - 32;\n"
        "        }\n"
        "        buf[i] = c;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    buf[len] = 0;\n"
        "    rc = dom_string_alloc_cdata(buf, len, result);\n"
        "    free(buf);\n"
        "    return rc;\n"
        "}\n"
        "\n"
        "export int dom_string_tolower(dom_string *source, int ascii_only, dom_string **result) {\n"
        "    if (ascii_only == 0) {\n"
        "        return DOM_NOT_SUPPORTED_ERR;\n"
        "    }\n"
        "    return dom_string_ascii_case_map(source, result, 1);\n"
        "}\n"
        "\n"
        "export int dom_string_toupper(dom_string *source, int ascii_only, dom_string **result) {\n"
        "    if (ascii_only == 0) {\n"
        "        return DOM_NOT_SUPPORTED_ERR;\n"
        "    }\n"
        "    return dom_string_ascii_case_map(source, result, 0);\n"
        "}\n"
        "\n"
        "export int dom_string_insert(dom_string *target, dom_string *source, uint32_t offset, dom_string **result) {\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
        "\n"
        "export int dom_string_replace(dom_string *target, dom_string *source, uint32_t i1, uint32_t i2, dom_string **result) {\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
        "\n"
        "export int dom_string_whitespace_op(dom_string *s, uint32_t op, dom_string **ret) {\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_namespace_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "extern uint8_t *dom_string_data(const dom_string *str);\n"
        "extern uint64_t dom_string_byte_length(const dom_string *str);\n"
        "extern uint32_t dom_string_length(dom_string *str);\n"
        "extern uint32_t dom_string_index(dom_string *str, uint32_t chr);\n"
        "extern int dom_string_substr(dom_string *str, uint32_t i1, uint32_t i2, dom_string **result);\n"
        "extern int dom_string_isequal(const dom_string *s1, const dom_string *s2);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_INVALID_CHARACTER_ERR = 5,\n"
        "    DOM_NAMESPACE_ERR = 14,\n"
        "    DOM_NO_MEM_ERR = 131072,\n"
        "    DOM_NAMESPACE_NULL = 0,\n"
        "    DOM_NAMESPACE_HTML = 1,\n"
        "    DOM_NAMESPACE_MATHML = 2,\n"
        "    DOM_NAMESPACE_SVG = 3,\n"
        "    DOM_NAMESPACE_XLINK = 4,\n"
        "    DOM_NAMESPACE_XML = 5,\n"
        "    DOM_NAMESPACE_XMLNS = 6,\n"
        "    DOM_NAMESPACE_COUNT = 7\n"
        "};\n"
        "\n"
        "global dom_string *dom_namespace_xml;\n"
        "global dom_string *dom_namespace_xmlns;\n"
        "global dom_string *dom_namespaces[7];\n"
        "\n"
        "int dom_namespace_ascii_letter(uint32_t ch) {\n"
        "    if (ch >= 65 && ch <= 90) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (ch >= 97 && ch <= 122) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int dom_namespace_ascii_digit(uint32_t ch) {\n"
        "    if (ch >= 48 && ch <= 57) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int dom_namespace_first_char(uint32_t ch, int allow_colon) {\n"
        "    if (dom_namespace_ascii_letter(ch) != 0 || ch == 95) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (allow_colon != 0 && ch == 58) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (ch >= 192) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int dom_namespace_name_char(uint32_t ch, int allow_colon) {\n"
        "    if (dom_namespace_first_char(ch, allow_colon) != 0) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (dom_namespace_ascii_digit(ch) != 0 || ch == 45 || ch == 46) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int dom_namespace_validate_ascii(dom_string *name, int allow_colon, int first_letter_only) {\n"
        "    uint8_t *data;\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint32_t ch;\n"
        "    if (name == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    len = dom_string_byte_length(name);\n"
        "    if (len == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    data = dom_string_data(name);\n"
        "    ch = data[0];\n"
        "    if (first_letter_only != 0) {\n"
        "        if (dom_namespace_ascii_letter(ch) == 0 && ch != 95) {\n"
        "            return 0;\n"
        "        }\n"
        "    } else if (dom_namespace_first_char(ch, allow_colon) == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    i = 1;\n"
        "    while (i < len) {\n"
        "        ch = data[i];\n"
        "        if (dom_namespace_name_char(ch, allow_colon) == 0) {\n"
        "            return 0;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "export int _dom_validate_name(dom_string *name) {\n"
        "    return dom_namespace_validate_ascii(name, 1, 0);\n"
        "}\n"
        "\n"
        "export int _dom_validate_ncname(dom_string *name) {\n"
        "    return dom_namespace_validate_ascii(name, 0, 1);\n"
        "}\n"
        "\n"
        "int dom_namespace_initialise(void) {\n"
        "    int err;\n"
        "    if (dom_namespace_xml != 0) {\n"
        "        return DOM_NO_ERR;\n"
        "    }\n"
        "    err = dom_string_create(\"xml\", 3, &dom_namespace_xml);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"xmlns\", 5, &dom_namespace_xmlns);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        dom_string_unref(dom_namespace_xml);\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"http://www.w3.org/1999/xhtml\", 28, &dom_namespaces[1]);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"http://www.w3.org/1998/Math/MathML\", 34, &dom_namespaces[2]);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"http://www.w3.org/2000/svg\", 26, &dom_namespaces[3]);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"http://www.w3.org/1999/xlink\", 28, &dom_namespaces[4]);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"http://www.w3.org/XML/1998/namespace\", 36, &dom_namespaces[5]);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"http://www.w3.org/2000/xmlns/\", 29, &dom_namespaces[6]);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_namespace_finalise(void) {\n"
        "    uint32_t i;\n"
        "    if (dom_namespace_xmlns != 0) {\n"
        "        dom_string_unref(dom_namespace_xmlns);\n"
        "    }\n"
        "    if (dom_namespace_xml != 0) {\n"
        "        dom_string_unref(dom_namespace_xml);\n"
        "    }\n"
        "    i = 1;\n"
        "    while (i < DOM_NAMESPACE_COUNT) {\n"
        "        if (dom_namespaces[i] != 0) {\n"
        "            dom_string_unref(dom_namespaces[i]);\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export dom_string *_dom_namespace_get_xml_prefix(void) {\n"
        "    if (dom_namespace_xml == 0) {\n"
        "        if (dom_namespace_initialise() != DOM_NO_ERR) {\n"
        "            return 0;\n"
        "        }\n"
        "    }\n"
        "    return dom_namespace_xml;\n"
        "}\n"
        "\n"
        "export dom_string *_dom_namespace_get_xmlns_prefix(void) {\n"
        "    if (dom_namespace_xml == 0) {\n"
        "        if (dom_namespace_initialise() != DOM_NO_ERR) {\n"
        "            return 0;\n"
        "        }\n"
        "    }\n"
        "    return dom_namespace_xmlns;\n"
        "}\n"
        "\n"
        "export dom_string *dom_namespace_get_by_index(uint32_t index) {\n"
        "    if (dom_namespace_initialise() != DOM_NO_ERR) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (index >= DOM_NAMESPACE_COUNT) {\n"
        "        return 0;\n"
        "    }\n"
        "    return dom_namespaces[index];\n"
        "}\n"
        "\n"
        "export int _dom_namespace_split_qname(dom_string *qname,\n"
        "                                      dom_string **prefix,\n"
        "                                      dom_string **localname) {\n"
        "    uint32_t colon;\n"
        "    int err;\n"
        "    if (dom_namespace_initialise() != DOM_NO_ERR) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    colon = dom_string_index(qname, 58);\n"
        "    if (colon == 4294967295) {\n"
        "        *prefix = 0;\n"
        "        *localname = dom_string_ref(qname);\n"
        "        return DOM_NO_ERR;\n"
        "    }\n"
        "    err = dom_string_substr(qname, 0, colon, prefix);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_substr(qname, colon + 1, dom_string_length(qname), localname);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        dom_string_unref(*prefix);\n"
        "        *prefix = 0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export uint32_t dom_namespace_split_prefix_length(dom_string *qname) {\n"
        "    dom_string *prefix;\n"
        "    dom_string *localname;\n"
        "    uint32_t len;\n"
        "    int err;\n"
        "    err = _dom_namespace_split_qname(qname, &prefix, &localname);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return 4294967295;\n"
        "    }\n"
        "    if (prefix == 0) {\n"
        "        if (localname != 0) {\n"
        "            dom_string_unref(localname);\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    len = dom_string_byte_length(prefix);\n"
        "    dom_string_unref(prefix);\n"
        "    if (localname != 0) {\n"
        "        dom_string_unref(localname);\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export int _dom_namespace_validate_qname(dom_string *qname,\n"
        "                                        dom_string *ns) {\n"
        "    uint32_t colon;\n"
        "    uint32_t len;\n"
        "    dom_string *prefix;\n"
        "    dom_string *lname;\n"
        "    int err;\n"
        "    if (dom_namespace_initialise() != DOM_NO_ERR) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    if (qname == 0) {\n"
        "        if (ns != 0) {\n"
        "            return DOM_NAMESPACE_ERR;\n"
        "        }\n"
        "        return DOM_NO_ERR;\n"
        "    }\n"
        "    if (_dom_validate_name(qname) == 0) {\n"
        "        return DOM_NAMESPACE_ERR;\n"
        "    }\n"
        "    len = dom_string_length(qname);\n"
        "    colon = dom_string_index(qname, 58);\n"
        "    if (colon == 4294967295) {\n"
        "        if (ns != 0 && dom_string_isequal(ns, dom_namespaces[DOM_NAMESPACE_XMLNS]) != 0) {\n"
        "            if (dom_string_isequal(qname, dom_namespace_xmlns) == 0) {\n"
        "                return DOM_NAMESPACE_ERR;\n"
        "            }\n"
        "        }\n"
        "        if (ns != 0 && dom_string_isequal(qname, dom_namespace_xmlns) != 0) {\n"
        "            if (dom_string_isequal(ns, dom_namespaces[DOM_NAMESPACE_XMLNS]) == 0) {\n"
        "                return DOM_NAMESPACE_ERR;\n"
        "            }\n"
        "        }\n"
        "        return DOM_NO_ERR;\n"
        "    }\n"
        "    if (colon == 0) {\n"
        "        return DOM_NAMESPACE_ERR;\n"
        "    }\n"
        "    if (ns == 0) {\n"
        "        return DOM_NAMESPACE_ERR;\n"
        "    }\n"
        "    err = dom_string_substr(qname, 0, colon, &prefix);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_substr(qname, colon + 1, len, &lname);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        dom_string_unref(prefix);\n"
        "        return err;\n"
        "    }\n"
        "    if (_dom_validate_ncname(prefix) == 0 || _dom_validate_ncname(lname) == 0) {\n"
        "        dom_string_unref(prefix);\n"
        "        dom_string_unref(lname);\n"
        "        return DOM_NAMESPACE_ERR;\n"
        "    }\n"
        "    dom_string_unref(lname);\n"
        "    if (dom_string_isequal(prefix, dom_namespace_xml) != 0) {\n"
        "        if (dom_string_isequal(ns, dom_namespaces[DOM_NAMESPACE_XML]) == 0) {\n"
        "            dom_string_unref(prefix);\n"
        "            return DOM_NAMESPACE_ERR;\n"
        "        }\n"
        "    }\n"
        "    if (dom_string_isequal(prefix, dom_namespace_xmlns) != 0) {\n"
        "        if (dom_string_isequal(ns, dom_namespaces[DOM_NAMESPACE_XMLNS]) == 0) {\n"
        "            dom_string_unref(prefix);\n"
        "            return DOM_NAMESPACE_ERR;\n"
        "        }\n"
        "    }\n"
        "    if (dom_string_isequal(ns, dom_namespaces[DOM_NAMESPACE_XMLNS]) != 0) {\n"
        "        if (dom_string_isequal(prefix, dom_namespace_xmlns) == 0) {\n"
        "            dom_string_unref(prefix);\n"
        "            return DOM_NAMESPACE_ERR;\n"
        "        }\n"
        "    }\n"
        "    dom_string_unref(prefix);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_nodelist_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_document;\n"
        "\n"
        "struct dom_node_internal {\n"
        "    dom_string *name;\n"
        "    uint32_t type;\n"
        "    struct dom_node_internal *parent;\n"
        "    struct dom_node_internal *first_child;\n"
        "    struct dom_node_internal *last_child;\n"
        "    struct dom_node_internal *previous;\n"
        "    struct dom_node_internal *next;\n"
        "    struct dom_document *owner;\n"
        "    dom_string *namespace;\n"
        "};\n"
        "\n"
        "typedef struct dom_node_internal dom_node_internal;\n"
        "typedef struct dom_node_internal dom_node;\n"
        "typedef struct dom_document dom_document;\n"
        "\n"
        "struct dom_nodelist {\n"
        "    dom_document *owner;\n"
        "    dom_node_internal *root;\n"
        "    uint32_t type;\n"
        "    dom_string *name;\n"
        "    int any_name;\n"
        "    int any_namespace;\n"
        "    int any_localname;\n"
        "    dom_string *namespace;\n"
        "    dom_string *localname;\n"
        "    uint32_t refcnt;\n"
        "};\n"
        "\n"
        "typedef struct dom_nodelist dom_nodelist;\n"
        "\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "extern uint8_t *dom_string_data(const dom_string *str);\n"
        "extern uint64_t dom_string_byte_length(const dom_string *str);\n"
        "extern int dom_string_isequal(const dom_string *s1, const dom_string *s2);\n"
        "extern int dom_string_caseless_isequal(const dom_string *s1, const dom_string *s2);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_NO_MEM_ERR = 131072,\n"
        "    DOM_ELEMENT_NODE = 1,\n"
        "    DOM_NODELIST_CHILDREN = 0,\n"
        "    DOM_NODELIST_BY_NAME = 1,\n"
        "    DOM_NODELIST_BY_NAMESPACE = 2,\n"
        "    DOM_NODELIST_BY_NAME_CASELESS = 3,\n"
        "    DOM_NODELIST_BY_NAMESPACE_CASELESS = 4\n"
        "};\n"
        "\n"
        "dom_node *dom_node_ref(dom_node *node) {\n"
        "    return node;\n"
        "}\n"
        "\n"
        "void dom_node_unref(dom_node *node) {\n"
        "}\n"
        "\n"
        "void _dom_document_remove_nodelist(dom_document *doc, dom_nodelist *nl) {\n"
        "}\n"
        "\n"
        "int dom_nodelist_string_is_star(dom_string *str) {\n"
        "    uint8_t *data;\n"
        "    if (str == 0 || dom_string_byte_length(str) != 1) {\n"
        "        return 0;\n"
        "    }\n"
        "    data = dom_string_data(str);\n"
        "    if (data[0] == 42) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export uint8_t *_dom_nodelist_create(dom_document *doc,\n"
        "                                uint32_t type,\n"
        "                                dom_node_internal *root,\n"
        "                                dom_string *tagname,\n"
        "                                dom_string *namespace,\n"
        "                                dom_string *localname) {\n"
        "    dom_nodelist *l;\n"
        "    l = (dom_nodelist *)malloc(80);\n"
        "    if (l == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    dom_node_ref((dom_node *)doc);\n"
        "    dom_node_ref(root);\n"
        "    l->owner = doc;\n"
        "    l->root = root;\n"
        "    l->type = type;\n"
        "    if (type == DOM_NODELIST_BY_NAME || type == DOM_NODELIST_BY_NAME_CASELESS) {\n"
        "        l->any_name = dom_nodelist_string_is_star(tagname);\n"
        "        l->name = dom_string_ref(tagname);\n"
        "    } else if (type == DOM_NODELIST_BY_NAMESPACE || type == DOM_NODELIST_BY_NAMESPACE_CASELESS) {\n"
        "        l->any_namespace = dom_nodelist_string_is_star(namespace);\n"
        "        l->any_localname = dom_nodelist_string_is_star(localname);\n"
        "        if (namespace != 0) {\n"
        "            dom_string_ref(namespace);\n"
        "        }\n"
        "        if (localname != 0) {\n"
        "            dom_string_ref(localname);\n"
        "        }\n"
        "        l->namespace = namespace;\n"
        "        l->localname = localname;\n"
        "    }\n"
        "    l->refcnt = 1;\n"
        "    return (uint8_t *)l;\n"
        "}\n"
        "\n"
        "export void dom_nodelist_ref(dom_nodelist *nl) {\n"
        "    if (nl != 0) {\n"
        "        nl->refcnt = nl->refcnt + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "export void dom_nodelist_unref(dom_nodelist *nl) {\n"
        "    if (nl == 0) {\n"
        "        return;\n"
        "    }\n"
        "    if (nl->refcnt > 0) {\n"
        "        nl->refcnt = nl->refcnt - 1;\n"
        "    }\n"
        "    if (nl->refcnt != 0) {\n"
        "        return;\n"
        "    }\n"
        "    if (nl->type == DOM_NODELIST_BY_NAME || nl->type == DOM_NODELIST_BY_NAME_CASELESS) {\n"
        "        if (nl->name != 0) {\n"
        "            dom_string_unref(nl->name);\n"
        "        }\n"
        "    } else if (nl->type == DOM_NODELIST_BY_NAMESPACE || nl->type == DOM_NODELIST_BY_NAMESPACE_CASELESS) {\n"
        "        if (nl->namespace != 0) {\n"
        "            dom_string_unref(nl->namespace);\n"
        "        }\n"
        "        if (nl->localname != 0) {\n"
        "            dom_string_unref(nl->localname);\n"
        "        }\n"
        "    }\n"
        "    dom_node_unref(nl->root);\n"
        "    _dom_document_remove_nodelist(nl->owner, nl);\n"
        "    dom_node_unref((dom_node *)nl->owner);\n"
        "    free((uint8_t *)nl);\n"
        "}\n"
        "\n"
        "int dom_nodelist_node_matches(dom_nodelist *nl, dom_node_internal *cur) {\n"
        "    if (nl->type == DOM_NODELIST_CHILDREN) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (cur->type != DOM_ELEMENT_NODE) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (nl->type == DOM_NODELIST_BY_NAME) {\n"
        "        if (nl->any_name != 0) {\n"
        "            return 1;\n"
        "        }\n"
        "        return dom_string_isequal(cur->name, nl->name);\n"
        "    }\n"
        "    if (nl->type == DOM_NODELIST_BY_NAME_CASELESS) {\n"
        "        if (nl->any_name != 0) {\n"
        "            return 1;\n"
        "        }\n"
        "        return dom_string_caseless_isequal(cur->name, nl->name);\n"
        "    }\n"
        "    if (nl->type == DOM_NODELIST_BY_NAMESPACE) {\n"
        "        if (nl->any_namespace == 0 && dom_string_isequal(cur->namespace, nl->namespace) == 0) {\n"
        "            return 0;\n"
        "        }\n"
        "        if (nl->any_localname != 0) {\n"
        "            return 1;\n"
        "        }\n"
        "        return dom_string_isequal(cur->name, nl->localname);\n"
        "    }\n"
        "    if (nl->type == DOM_NODELIST_BY_NAMESPACE_CASELESS) {\n"
        "        if (nl->any_namespace == 0 && dom_string_caseless_isequal(cur->namespace, nl->namespace) == 0) {\n"
        "            return 0;\n"
        "        }\n"
        "        if (nl->any_localname != 0) {\n"
        "            return 1;\n"
        "        }\n"
        "        return dom_string_caseless_isequal(cur->name, nl->localname);\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "dom_node_internal *dom_nodelist_next(dom_nodelist *nl, dom_node_internal *cur) {\n"
        "    dom_node_internal *node;\n"
        "    if (cur == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (nl->type == DOM_NODELIST_CHILDREN) {\n"
        "        return cur->next;\n"
        "    }\n"
        "    if (cur->first_child != 0) {\n"
        "        return cur->first_child;\n"
        "    }\n"
        "    node = cur;\n"
        "    while (node != 0 && node != nl->root) {\n"
        "        if (node->next != 0) {\n"
        "            return node->next;\n"
        "        }\n"
        "        node = node->parent;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export int dom_nodelist_get_length(dom_nodelist *nl, uint32_t *length) {\n"
        "    dom_node_internal *cur;\n"
        "    uint32_t len;\n"
        "    len = 0;\n"
        "    cur = nl->root->first_child;\n"
        "    while (cur != 0) {\n"
        "        if (dom_nodelist_node_matches(nl, cur) != 0) {\n"
        "            len = len + 1;\n"
        "        }\n"
        "        cur = dom_nodelist_next(nl, cur);\n"
        "    }\n"
        "    *length = len;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export uint8_t *_dom_nodelist_item(dom_nodelist *nl, uint32_t index) {\n"
        "    dom_node_internal *cur;\n"
        "    uint32_t count;\n"
        "    count = 0;\n"
        "    cur = nl->root->first_child;\n"
        "    while (cur != 0) {\n"
        "        if (dom_nodelist_node_matches(nl, cur) != 0) {\n"
        "            if (count == index) {\n"
        "                dom_node_ref(cur);\n"
        "                return (uint8_t *)cur;\n"
        "            }\n"
        "            count = count + 1;\n"
        "        }\n"
        "        cur = dom_nodelist_next(nl, cur);\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export int _dom_nodelist_match(dom_nodelist *nl,\n"
        "                               uint32_t type,\n"
        "                               dom_node_internal *root,\n"
        "                               dom_string *tagname,\n"
        "                               dom_string *namespace,\n"
        "                               dom_string *localname) {\n"
        "    if (nl->root != root || nl->type != type) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (type == DOM_NODELIST_CHILDREN) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (type == DOM_NODELIST_BY_NAME) {\n"
        "        return dom_string_isequal(nl->name, tagname);\n"
        "    }\n"
        "    if (type == DOM_NODELIST_BY_NAME_CASELESS) {\n"
        "        return dom_string_caseless_isequal(nl->name, tagname);\n"
        "    }\n"
        "    if (type == DOM_NODELIST_BY_NAMESPACE) {\n"
        "        if (dom_string_isequal(nl->namespace, namespace) == 0) {\n"
        "            return 0;\n"
        "        }\n"
        "        return dom_string_isequal(nl->localname, localname);\n"
        "    }\n"
        "    if (type == DOM_NODELIST_BY_NAMESPACE_CASELESS) {\n"
        "        if (dom_string_caseless_isequal(nl->namespace, namespace) == 0) {\n"
        "            return 0;\n"
        "        }\n"
        "        return dom_string_caseless_isequal(nl->localname, localname);\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export int _dom_nodelist_equal(dom_nodelist *l1, dom_nodelist *l2) {\n"
        "    return _dom_nodelist_match(l1, l1->type, l2->root, l2->name,\n"
        "                               l2->namespace, l2->localname);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_implementation_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "extern int _dom_validate_name(dom_string *name);\n"
        "extern int _dom_namespace_validate_qname(dom_string *qname, dom_string *ns);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_INVALID_CHARACTER_ERR = 5,\n"
        "    DOM_NOT_SUPPORTED_ERR = 9,\n"
        "    DOM_NAMESPACE_ERR = 14\n"
        "};\n"
        "\n"
        "uint64_t dom_implementation_strlen(const uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export int dom_implementation_has_feature(const uint8_t *feature,\n"
        "                                          const uint8_t *version,\n"
        "                                          uint8_t *result) {\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
        "\n"
        "export int dom_implementation_create_document_type(const uint8_t *qname,\n"
        "                                                   const uint8_t *public_id,\n"
        "                                                   const uint8_t *system_id,\n"
        "                                                   uint8_t *doctype) {\n"
        "    dom_string *qname_s;\n"
        "    int err;\n"
        "    if (qname == 0) {\n"
        "        return DOM_INVALID_CHARACTER_ERR;\n"
        "    }\n"
        "    err = dom_string_create(qname, dom_implementation_strlen(qname), &qname_s);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    if (_dom_validate_name(qname_s) == 0) {\n"
        "        dom_string_unref(qname_s);\n"
        "        return DOM_INVALID_CHARACTER_ERR;\n"
        "    }\n"
        "    err = _dom_namespace_validate_qname(qname_s, (dom_string *)0);\n"
        "    dom_string_unref(qname_s);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
        "\n"
        "export int dom_implementation_create_document(uint32_t impl_type,\n"
        "                                              const uint8_t *namespace_uri,\n"
        "                                              const uint8_t *qname,\n"
        "                                              uint8_t *doctype,\n"
        "                                              uint8_t *daf,\n"
        "                                              uint8_t *daf_ctx) {\n"
        "    dom_string *namespace_s;\n"
        "    dom_string *qname_s;\n"
        "    int err;\n"
        "    namespace_s = (dom_string *)0;\n"
        "    qname_s = (dom_string *)0;\n"
        "    if (namespace_uri != 0) {\n"
        "        err = dom_string_create(namespace_uri, dom_implementation_strlen(namespace_uri), &namespace_s);\n"
        "        if (err != DOM_NO_ERR) {\n"
        "            return err;\n"
        "        }\n"
        "    }\n"
        "    if (qname != 0) {\n"
        "        err = dom_string_create(qname, dom_implementation_strlen(qname), &qname_s);\n"
        "        if (err != DOM_NO_ERR) {\n"
        "            if (namespace_s != 0) {\n"
        "                dom_string_unref(namespace_s);\n"
        "            }\n"
        "            return err;\n"
        "        }\n"
        "    }\n"
        "    if (qname_s != 0 && _dom_validate_name(qname_s) == 0) {\n"
        "        dom_string_unref(qname_s);\n"
        "        if (namespace_s != 0) {\n"
        "            dom_string_unref(namespace_s);\n"
        "        }\n"
        "        return DOM_INVALID_CHARACTER_ERR;\n"
        "    }\n"
        "    err = _dom_namespace_validate_qname(qname_s, namespace_s);\n"
        "    if (qname_s != 0) {\n"
        "        dom_string_unref(qname_s);\n"
        "    }\n"
        "    if (namespace_s != 0) {\n"
        "        dom_string_unref(namespace_s);\n"
        "    }\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return DOM_NAMESPACE_ERR;\n"
        "    }\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
        "\n"
        "export int dom_implementation_get_feature(const uint8_t *feature,\n"
        "                                         const uint8_t *version,\n"
        "                                         uint8_t *object) {\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_document_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_node_internal {\n"
        "    dom_string *name;\n"
        "    uint32_t type;\n"
        "    struct dom_node_internal *parent;\n"
        "    struct dom_node_internal *first_child;\n"
        "    struct dom_node_internal *last_child;\n"
        "    struct dom_node_internal *previous;\n"
        "    struct dom_node_internal *next;\n"
        "    struct dom_node_internal *owner;\n"
        "    dom_string *namespace;\n"
        "};\n"
        "\n"
        "typedef struct dom_node_internal dom_node_internal;\n"
        "\n"
        "struct dom_document {\n"
        "    dom_string *name;\n"
        "    uint32_t type;\n"
        "    dom_node_internal *parent;\n"
        "    dom_node_internal *first_child;\n"
        "    dom_node_internal *last_child;\n"
        "    dom_node_internal *previous;\n"
        "    dom_node_internal *next;\n"
        "    dom_node_internal *owner;\n"
        "    dom_string *namespace;\n"
        "    dom_string *uri;\n"
        "    uint32_t quirks;\n"
        "};\n"
        "\n"
        "typedef struct dom_document dom_document;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "extern int _dom_validate_name(dom_string *name);\n"
        "extern int _dom_namespace_validate_qname(dom_string *qname, dom_string *ns);\n"
        "extern uint8_t *_dom_nodelist_create(dom_node_internal *doc,\n"
        "                                     uint32_t type,\n"
        "                                     dom_node_internal *root,\n"
        "                                     dom_string *tagname,\n"
        "                                     dom_string *namespace,\n"
        "                                     dom_string *localname);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_INVALID_CHARACTER_ERR = 5,\n"
        "    DOM_NOT_SUPPORTED_ERR = 9,\n"
        "    DOM_NAMESPACE_ERR = 14,\n"
        "    DOM_DOCUMENT_NODE = 9,\n"
        "    DOM_NODELIST_BY_NAME = 1,\n"
        "    DOM_NODELIST_BY_NAMESPACE = 2\n"
        "};\n"
        "\n"
        "export uint8_t *_dom_document_create(uint8_t *daf, uint8_t *daf_ctx) {\n"
        "    dom_document *doc;\n"
        "    dom_string *uri;\n"
        "    int err;\n"
        "    doc = (dom_document *)malloc(96);\n"
        "    if (doc == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    err = dom_string_create(\"about:blank\", 11, &uri);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        free((uint8_t *)doc);\n"
        "        return 0;\n"
        "    }\n"
        "    doc->name = (dom_string *)0;\n"
        "    doc->type = DOM_DOCUMENT_NODE;\n"
        "    doc->parent = (dom_node_internal *)0;\n"
        "    doc->first_child = (dom_node_internal *)0;\n"
        "    doc->last_child = (dom_node_internal *)0;\n"
        "    doc->previous = (dom_node_internal *)0;\n"
        "    doc->next = (dom_node_internal *)0;\n"
        "    doc->owner = (dom_node_internal *)doc;\n"
        "    doc->namespace = (dom_string *)0;\n"
        "    doc->uri = uri;\n"
        "    doc->quirks = 0;\n"
        "    return (uint8_t *)doc;\n"
        "}\n"
        "\n"
        "export void _dom_document_destroy(dom_node_internal *node) {\n"
        "    dom_document *doc;\n"
        "    if (node == 0) {\n"
        "        return;\n"
        "    }\n"
        "    doc = (dom_document *)node;\n"
        "    if (doc->uri != 0) {\n"
        "        dom_string_unref(doc->uri);\n"
        "    }\n"
        "    free((uint8_t *)doc);\n"
        "}\n"
        "\n"
        "export uint8_t *_dom_document_get_implementation(dom_document *doc) {\n"
        "    return \"libdom\";\n"
        "}\n"
        "\n"
        "export uint8_t *_dom_document_get_uri(dom_document *doc) {\n"
        "    if (doc == 0 || doc->uri == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return (uint8_t *)dom_string_ref(doc->uri);\n"
        "}\n"
        "\n"
        "export int _dom_document_set_uri(dom_document *doc, dom_string *uri) {\n"
        "    if (doc->uri != 0) {\n"
        "        dom_string_unref(doc->uri);\n"
        "    }\n"
        "    doc->uri = dom_string_ref(uri);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_document_get_quirks_mode(dom_document *doc, uint32_t *result) {\n"
        "    *result = doc->quirks;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_document_set_quirks_mode(dom_document *doc, uint32_t result) {\n"
        "    doc->quirks = result;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export uint8_t *_dom_document_get_elements_by_tag_name(dom_document *doc,\n"
        "                                                       dom_string *tagname) {\n"
        "    return _dom_nodelist_create((dom_node_internal *)doc,\n"
        "                                DOM_NODELIST_BY_NAME,\n"
        "                                (dom_node_internal *)doc,\n"
        "                                tagname,\n"
        "                                (dom_string *)0,\n"
        "                                (dom_string *)0);\n"
        "}\n"
        "\n"
        "export uint8_t *_dom_document_get_elements_by_tag_name_ns(dom_document *doc,\n"
        "                                                          dom_string *namespace,\n"
        "                                                          dom_string *localname) {\n"
        "    return _dom_nodelist_create((dom_node_internal *)doc,\n"
        "                                DOM_NODELIST_BY_NAMESPACE,\n"
        "                                (dom_node_internal *)doc,\n"
        "                                (dom_string *)0,\n"
        "                                namespace,\n"
        "                                localname);\n"
        "}\n"
        "\n"
        "export int _dom_document_create_element(dom_document *doc,\n"
        "                                       dom_string *tag_name,\n"
        "                                       uint8_t *result) {\n"
        "    if (_dom_validate_name(tag_name) == 0) {\n"
        "        return DOM_INVALID_CHARACTER_ERR;\n"
        "    }\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_document_create_element_ns(dom_document *doc,\n"
        "                                          dom_string *namespace,\n"
        "                                          dom_string *qname,\n"
        "                                          uint8_t *result) {\n"
        "    int err;\n"
        "    if (_dom_validate_name(qname) == 0) {\n"
        "        return DOM_INVALID_CHARACTER_ERR;\n"
        "    }\n"
        "    err = _dom_namespace_validate_qname(qname, namespace);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NOT_SUPPORTED_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_html_button_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_html_button_element {\n"
        "    uint8_t disabled;\n"
        "    uint32_t tab_index;\n"
        "    uint8_t *form;\n"
        "    dom_string *access_key;\n"
        "    dom_string *name;\n"
        "    dom_string *type;\n"
        "    dom_string *value;\n"
        "};\n"
        "\n"
        "typedef struct dom_html_button_element dom_html_button_element;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_NO_MEM_ERR = 1\n"
        "};\n"
        "\n"
        "void dom_html_button_unref_string(dom_string *str) {\n"
        "    if (str != 0) {\n"
        "        dom_string_unref(str);\n"
        "    }\n"
        "}\n"
        "\n"
        "dom_string *dom_html_button_ref_string(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return (dom_string *)0;\n"
        "    }\n"
        "    return dom_string_ref(str);\n"
        "}\n"
        "\n"
        "int dom_html_button_set_string(dom_string **slot, dom_string *value) {\n"
        "    if (*slot != 0) {\n"
        "        dom_string_unref(*slot);\n"
        "    }\n"
        "    *slot = dom_html_button_ref_string(value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int _dom_html_button_element_initialise(uint8_t *params, dom_html_button_element *ele) {\n"
        "    ele->disabled = 0;\n"
        "    ele->tab_index = 0;\n"
        "    ele->form = (uint8_t *)0;\n"
        "    ele->access_key = (dom_string *)0;\n"
        "    ele->name = (dom_string *)0;\n"
        "    ele->value = (dom_string *)0;\n"
        "    return dom_string_create(\"submit\", 6, &ele->type);\n"
        "}\n"
        "\n"
        "export int _dom_html_button_element_create(uint8_t *params, dom_html_button_element **ele) {\n"
        "    int err;\n"
        "    *ele = (dom_html_button_element *)malloc(96);\n"
        "    if (*ele == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    err = _dom_html_button_element_initialise(params, *ele);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        free((uint8_t *)*ele);\n"
        "        *ele = (dom_html_button_element *)0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_html_button_element_finalise(dom_html_button_element *ele) {\n"
        "    dom_html_button_unref_string(ele->access_key);\n"
        "    dom_html_button_unref_string(ele->name);\n"
        "    dom_html_button_unref_string(ele->type);\n"
        "    dom_html_button_unref_string(ele->value);\n"
        "}\n"
        "\n"
        "export void _dom_html_button_element_destroy(dom_html_button_element *ele) {\n"
        "    if (ele == 0) {\n"
        "        return;\n"
        "    }\n"
        "    _dom_html_button_element_finalise(ele);\n"
        "    free((uint8_t *)ele);\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_disabled(dom_html_button_element *ele, uint8_t *disabled) {\n"
        "    *disabled = ele->disabled;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_set_disabled(dom_html_button_element *ele, uint8_t disabled) {\n"
        "    ele->disabled = disabled;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_tab_index(dom_html_button_element *button, int32_t *tab_index) {\n"
        "    *tab_index = (int32_t)button->tab_index;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_set_tab_index(dom_html_button_element *button, uint32_t tab_index) {\n"
        "    button->tab_index = tab_index;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_access_key(dom_html_button_element *element, dom_string **attr) {\n"
        "    *attr = dom_html_button_ref_string(element->access_key);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_set_access_key(dom_html_button_element *element, dom_string *attr) {\n"
        "    return dom_html_button_set_string(&element->access_key, attr);\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_name(dom_html_button_element *element, dom_string **attr) {\n"
        "    *attr = dom_html_button_ref_string(element->name);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_set_name(dom_html_button_element *element, dom_string *attr) {\n"
        "    return dom_html_button_set_string(&element->name, attr);\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_type(dom_html_button_element *element, dom_string **attr) {\n"
        "    *attr = dom_html_button_ref_string(element->type);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_value(dom_html_button_element *element, dom_string **attr) {\n"
        "    *attr = dom_html_button_ref_string(element->value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_set_value(dom_html_button_element *element, dom_string *attr) {\n"
        "    return dom_html_button_set_string(&element->value, attr);\n"
        "}\n"
        "\n"
        "export int dom_html_button_element_get_form(dom_html_button_element *button, uint8_t **form) {\n"
        "    *form = button->form;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_html_button_element_set_form(dom_html_button_element *button, uint8_t *form) {\n"
        "    button->form = form;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_html_button_element_parse_attribute(uint8_t *ele,\n"
        "                                                    dom_string *name,\n"
        "                                                    dom_string *value,\n"
        "                                                    dom_string **parsed) {\n"
        "    *parsed = dom_html_button_ref_string(value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_html_button_element_copy_internal(dom_html_button_element *old,\n"
        "                                                 dom_html_button_element *new) {\n"
        "    new->disabled = old->disabled;\n"
        "    new->tab_index = old->tab_index;\n"
        "    new->form = old->form;\n"
        "    new->access_key = dom_html_button_ref_string(old->access_key);\n"
        "    new->name = dom_html_button_ref_string(old->name);\n"
        "    new->type = dom_html_button_ref_string(old->type);\n"
        "    new->value = dom_html_button_ref_string(old->value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_html_input_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_html_input_element {\n"
        "    uint8_t disabled;\n"
        "    uint8_t read_only;\n"
        "    uint8_t checked;\n"
        "    uint8_t checked_set;\n"
        "    uint8_t default_checked;\n"
        "    uint8_t default_checked_set;\n"
        "    uint32_t size;\n"
        "    uint32_t tab_index;\n"
        "    uint32_t max_length;\n"
        "    uint8_t *form;\n"
        "    dom_string *accept;\n"
        "    dom_string *access_key;\n"
        "    dom_string *align;\n"
        "    dom_string *alt;\n"
        "    dom_string *name;\n"
        "    dom_string *src;\n"
        "    dom_string *type;\n"
        "    dom_string *use_map;\n"
        "    dom_string *value;\n"
        "    dom_string *default_value;\n"
        "};\n"
        "\n"
        "typedef struct dom_html_input_element dom_html_input_element;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_NO_MEM_ERR = 1\n"
        "};\n"
        "\n"
        "dom_string *dom_html_input_ref_string(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return (dom_string *)0;\n"
        "    }\n"
        "    return dom_string_ref(str);\n"
        "}\n"
        "\n"
        "void dom_html_input_unref_string(dom_string *str) {\n"
        "    if (str != 0) {\n"
        "        dom_string_unref(str);\n"
        "    }\n"
        "}\n"
        "\n"
        "int dom_html_input_set_string(dom_string **slot, dom_string *value) {\n"
        "    if (*slot != 0) {\n"
        "        dom_string_unref(*slot);\n"
        "    }\n"
        "    *slot = dom_html_input_ref_string(value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int _dom_html_input_element_initialise(uint8_t *params, dom_html_input_element *ele) {\n"
        "    ele->disabled = 0;\n"
        "    ele->read_only = 0;\n"
        "    ele->checked = 0;\n"
        "    ele->checked_set = 0;\n"
        "    ele->default_checked = 0;\n"
        "    ele->default_checked_set = 0;\n"
        "    ele->size = 20;\n"
        "    ele->tab_index = 0;\n"
        "    ele->max_length = 0;\n"
        "    ele->form = (uint8_t *)0;\n"
        "    ele->accept = (dom_string *)0;\n"
        "    ele->access_key = (dom_string *)0;\n"
        "    ele->align = (dom_string *)0;\n"
        "    ele->alt = (dom_string *)0;\n"
        "    ele->name = (dom_string *)0;\n"
        "    ele->src = (dom_string *)0;\n"
        "    ele->use_map = (dom_string *)0;\n"
        "    ele->value = (dom_string *)0;\n"
        "    ele->default_value = (dom_string *)0;\n"
        "    return dom_string_create(\"text\", 4, &ele->type);\n"
        "}\n"
        "\n"
        "export int _dom_html_input_element_create(uint8_t *params, dom_html_input_element **ele) {\n"
        "    int err;\n"
        "    *ele = (dom_html_input_element *)malloc(192);\n"
        "    if (*ele == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    err = _dom_html_input_element_initialise(params, *ele);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        free((uint8_t *)*ele);\n"
        "        *ele = (dom_html_input_element *)0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_html_input_element_finalise(dom_html_input_element *ele) {\n"
        "    dom_html_input_unref_string(ele->accept);\n"
        "    dom_html_input_unref_string(ele->access_key);\n"
        "    dom_html_input_unref_string(ele->align);\n"
        "    dom_html_input_unref_string(ele->alt);\n"
        "    dom_html_input_unref_string(ele->name);\n"
        "    dom_html_input_unref_string(ele->src);\n"
        "    dom_html_input_unref_string(ele->type);\n"
        "    dom_html_input_unref_string(ele->use_map);\n"
        "    dom_html_input_unref_string(ele->value);\n"
        "    dom_html_input_unref_string(ele->default_value);\n"
        "}\n"
        "\n"
        "export void _dom_html_input_element_destroy(dom_html_input_element *ele) {\n"
        "    if (ele == 0) {\n"
        "        return;\n"
        "    }\n"
        "    _dom_html_input_element_finalise(ele);\n"
        "    free((uint8_t *)ele);\n"
        "}\n"
        "\n"
        "export int dom_html_input_element_get_disabled(dom_html_input_element *ele, uint8_t *disabled) { *disabled = ele->disabled; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_disabled(dom_html_input_element *ele, uint8_t disabled) { ele->disabled = disabled; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_read_only(dom_html_input_element *ele, uint8_t *read_only) { *read_only = ele->read_only; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_read_only(dom_html_input_element *ele, uint8_t read_only) { ele->read_only = read_only; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_checked(dom_html_input_element *ele, uint8_t *checked) { *checked = ele->checked; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_checked(dom_html_input_element *ele, uint8_t checked) { ele->checked = checked; ele->checked_set = 1; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_default_checked(dom_html_input_element *ele, uint8_t *default_checked) { *default_checked = ele->default_checked; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_default_checked(dom_html_input_element *ele, uint8_t default_checked) { ele->default_checked = default_checked; ele->default_checked_set = 1; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_size(dom_html_input_element *input, uint32_t *size) { *size = input->size; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_size(dom_html_input_element *input, uint32_t size) { input->size = size; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_tab_index(dom_html_input_element *input, int32_t *tab_index) { *tab_index = (int32_t)input->tab_index; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_tab_index(dom_html_input_element *input, uint32_t tab_index) { input->tab_index = tab_index; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_max_length(dom_html_input_element *input, int32_t *max_length) { *max_length = (int32_t)input->max_length; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_max_length(dom_html_input_element *input, uint32_t max_length) { input->max_length = max_length; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_default_value(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->default_value); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_default_value(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->default_value, value); }\n"
        "export int dom_html_input_element_get_accept(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->accept); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_accept(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->accept, value); }\n"
        "export int dom_html_input_element_get_access_key(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->access_key); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_access_key(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->access_key, value); }\n"
        "export int dom_html_input_element_get_align(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->align); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_align(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->align, value); }\n"
        "export int dom_html_input_element_get_alt(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->alt); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_alt(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->alt, value); }\n"
        "export int dom_html_input_element_get_name(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->name); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_name(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->name, value); }\n"
        "export int dom_html_input_element_get_src(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->src); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_src(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->src, value); }\n"
        "export int dom_html_input_element_get_type(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->type); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_get_use_map(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->use_map); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_use_map(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->use_map, value); }\n"
        "export int dom_html_input_element_get_value(dom_html_input_element *input, dom_string **value) { *value = dom_html_input_ref_string(input->value); return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_set_value(dom_html_input_element *input, dom_string *value) { return dom_html_input_set_string(&input->value, value); }\n"
        "export int dom_html_input_element_get_form(dom_html_input_element *input, uint8_t **form) { *form = input->form; return DOM_NO_ERR; }\n"
        "export int _dom_html_input_element_set_form(dom_html_input_element *input, uint8_t *form) { input->form = form; return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_blur(dom_html_input_element *ele) { return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_focus(dom_html_input_element *ele) { return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_select(dom_html_input_element *ele) { return DOM_NO_ERR; }\n"
        "export int dom_html_input_element_click(dom_html_input_element *ele) { ele->checked = 1; ele->checked_set = 1; return DOM_NO_ERR; }\n"
        "export int _dom_html_input_element_parse_attribute(uint8_t *ele, dom_string *name, dom_string *value, dom_string **parsed) { *parsed = dom_html_input_ref_string(value); return DOM_NO_ERR; }\n"
        "export int _dom_html_input_element_copy_internal(dom_html_input_element *old, dom_html_input_element *new) {\n"
        "    new->disabled = old->disabled; new->read_only = old->read_only; new->checked = old->checked; new->checked_set = old->checked_set;\n"
        "    new->default_checked = old->default_checked; new->default_checked_set = old->default_checked_set; new->size = old->size; new->tab_index = old->tab_index;\n"
        "    new->max_length = old->max_length; new->form = old->form; new->accept = dom_html_input_ref_string(old->accept); new->access_key = dom_html_input_ref_string(old->access_key);\n"
        "    new->align = dom_html_input_ref_string(old->align); new->alt = dom_html_input_ref_string(old->alt); new->name = dom_html_input_ref_string(old->name); new->src = dom_html_input_ref_string(old->src);\n"
        "    new->type = dom_html_input_ref_string(old->type); new->use_map = dom_html_input_ref_string(old->use_map); new->value = dom_html_input_ref_string(old->value); new->default_value = dom_html_input_ref_string(old->default_value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_html_textarea_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_html_text_area_element {\n"
        "    uint8_t disabled;\n"
        "    uint8_t read_only;\n"
        "    uint32_t cols;\n"
        "    uint32_t rows;\n"
        "    uint32_t tab_index;\n"
        "    uint8_t *form;\n"
        "    dom_string *access_key;\n"
        "    dom_string *name;\n"
        "    dom_string *type;\n"
        "    dom_string *value;\n"
        "    dom_string *default_value;\n"
        "};\n"
        "\n"
        "typedef struct dom_html_text_area_element dom_html_text_area_element;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_NO_MEM_ERR = 1\n"
        "};\n"
        "\n"
        "dom_string *dom_html_textarea_ref_string(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return (dom_string *)0;\n"
        "    }\n"
        "    return dom_string_ref(str);\n"
        "}\n"
        "\n"
        "void dom_html_textarea_unref_string(dom_string *str) {\n"
        "    if (str != 0) {\n"
        "        dom_string_unref(str);\n"
        "    }\n"
        "}\n"
        "\n"
        "int dom_html_textarea_set_string(dom_string **slot, dom_string *value) {\n"
        "    if (*slot != 0) {\n"
        "        dom_string_unref(*slot);\n"
        "    }\n"
        "    *slot = dom_html_textarea_ref_string(value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int _dom_html_text_area_element_initialise(uint8_t *params, dom_html_text_area_element *ele) {\n"
        "    ele->disabled = 0;\n"
        "    ele->read_only = 0;\n"
        "    ele->cols = 20;\n"
        "    ele->rows = 2;\n"
        "    ele->tab_index = 0;\n"
        "    ele->form = (uint8_t *)0;\n"
        "    ele->access_key = (dom_string *)0;\n"
        "    ele->name = (dom_string *)0;\n"
        "    ele->value = (dom_string *)0;\n"
        "    ele->default_value = (dom_string *)0;\n"
        "    return dom_string_create(\"textarea\", 8, &ele->type);\n"
        "}\n"
        "\n"
        "export int _dom_html_text_area_element_create(uint8_t *params, dom_html_text_area_element **ele) {\n"
        "    int err;\n"
        "    *ele = (dom_html_text_area_element *)malloc(128);\n"
        "    if (*ele == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    err = _dom_html_text_area_element_initialise(params, *ele);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        free((uint8_t *)*ele);\n"
        "        *ele = (dom_html_text_area_element *)0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_html_text_area_element_finalise(dom_html_text_area_element *ele) {\n"
        "    dom_html_textarea_unref_string(ele->access_key);\n"
        "    dom_html_textarea_unref_string(ele->name);\n"
        "    dom_html_textarea_unref_string(ele->type);\n"
        "    dom_html_textarea_unref_string(ele->value);\n"
        "    dom_html_textarea_unref_string(ele->default_value);\n"
        "}\n"
        "\n"
        "export void _dom_html_text_area_element_destroy(dom_html_text_area_element *ele) {\n"
        "    if (ele == 0) {\n"
        "        return;\n"
        "    }\n"
        "    _dom_html_text_area_element_finalise(ele);\n"
        "    free((uint8_t *)ele);\n"
        "}\n"
        "\n"
        "export int dom_html_text_area_element_get_disabled(dom_html_text_area_element *ele, uint8_t *disabled) { *disabled = ele->disabled; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_disabled(dom_html_text_area_element *ele, uint8_t disabled) { ele->disabled = disabled; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_get_read_only(dom_html_text_area_element *ele, uint8_t *read_only) { *read_only = ele->read_only; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_read_only(dom_html_text_area_element *ele, uint8_t read_only) { ele->read_only = read_only; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_get_default_value(dom_html_text_area_element *ele, dom_string **value) { *value = dom_html_textarea_ref_string(ele->default_value); return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_default_value(dom_html_text_area_element *ele, dom_string *value) { return dom_html_textarea_set_string(&ele->default_value, value); }\n"
        "export int dom_html_text_area_element_get_value(dom_html_text_area_element *ele, dom_string **value) { *value = dom_html_textarea_ref_string(ele->value); return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_value(dom_html_text_area_element *ele, dom_string *value) { return dom_html_textarea_set_string(&ele->value, value); }\n"
        "export int dom_html_text_area_element_get_access_key(dom_html_text_area_element *ele, dom_string **value) { *value = dom_html_textarea_ref_string(ele->access_key); return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_access_key(dom_html_text_area_element *ele, dom_string *value) { return dom_html_textarea_set_string(&ele->access_key, value); }\n"
        "export int dom_html_text_area_element_get_name(dom_html_text_area_element *ele, dom_string **value) { *value = dom_html_textarea_ref_string(ele->name); return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_name(dom_html_text_area_element *ele, dom_string *value) { return dom_html_textarea_set_string(&ele->name, value); }\n"
        "export int dom_html_text_area_element_get_type(dom_html_text_area_element *ele, dom_string **value) { *value = dom_html_textarea_ref_string(ele->type); return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_get_tab_index(dom_html_text_area_element *ele, int32_t *tab_index) { *tab_index = (int32_t)ele->tab_index; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_tab_index(dom_html_text_area_element *ele, uint32_t tab_index) { ele->tab_index = tab_index; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_get_cols(dom_html_text_area_element *ele, int32_t *cols) { *cols = (int32_t)ele->cols; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_cols(dom_html_text_area_element *ele, uint32_t cols) { ele->cols = cols; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_get_rows(dom_html_text_area_element *ele, int32_t *rows) { *rows = (int32_t)ele->rows; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_set_rows(dom_html_text_area_element *ele, uint32_t rows) { ele->rows = rows; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_get_form(dom_html_text_area_element *ele, uint8_t **form) { *form = ele->form; return DOM_NO_ERR; }\n"
        "export int _dom_html_text_area_element_set_form(dom_html_text_area_element *ele, uint8_t *form) { ele->form = form; return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_blur(dom_html_text_area_element *ele) { return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_focus(dom_html_text_area_element *ele) { return DOM_NO_ERR; }\n"
        "export int dom_html_text_area_element_select(dom_html_text_area_element *ele) { return DOM_NO_ERR; }\n"
        "export int _dom_html_text_area_element_parse_attribute(uint8_t *ele, dom_string *name, dom_string *value, dom_string **parsed) { *parsed = dom_html_textarea_ref_string(value); return DOM_NO_ERR; }\n"
        "export int _dom_html_text_area_element_copy_internal(dom_html_text_area_element *old, dom_html_text_area_element *new) {\n"
        "    new->disabled = old->disabled; new->read_only = old->read_only; new->cols = old->cols; new->rows = old->rows;\n"
        "    new->tab_index = old->tab_index; new->form = old->form; new->access_key = dom_html_textarea_ref_string(old->access_key);\n"
        "    new->name = dom_html_textarea_ref_string(old->name); new->type = dom_html_textarea_ref_string(old->type);\n"
        "    new->value = dom_html_textarea_ref_string(old->value); new->default_value = dom_html_textarea_ref_string(old->default_value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_html_select_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_html_select_element {\n"
        "    uint8_t disabled;\n"
        "    uint8_t multiple;\n"
        "    uint32_t size;\n"
        "    int32_t tab_index;\n"
        "    int32_t selected_index;\n"
        "    uint32_t length;\n"
        "    uint8_t *form;\n"
        "    dom_string *name;\n"
        "    dom_string *value;\n"
        "    dom_string *type_one;\n"
        "    dom_string *type_multiple;\n"
        "};\n"
        "\n"
        "typedef struct dom_html_select_element dom_html_select_element;\n"
        "\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, dom_string **str);\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_NO_MEM_ERR = 1,\n"
        "    DOM_NOT_SUPPORTED_ERR = 9\n"
        "};\n"
        "\n"
        "dom_string *dom_html_select_ref_string(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return (dom_string *)0;\n"
        "    }\n"
        "    return dom_string_ref(str);\n"
        "}\n"
        "\n"
        "void dom_html_select_unref_string(dom_string *str) {\n"
        "    if (str != 0) {\n"
        "        dom_string_unref(str);\n"
        "    }\n"
        "}\n"
        "\n"
        "int dom_html_select_set_string(dom_string **slot, dom_string *value) {\n"
        "    if (*slot != 0) {\n"
        "        dom_string_unref(*slot);\n"
        "    }\n"
        "    *slot = dom_html_select_ref_string(value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int _dom_html_select_element_initialise(uint8_t *params, dom_html_select_element *ele) {\n"
        "    int err;\n"
        "    ele->disabled = 0;\n"
        "    ele->multiple = 0;\n"
        "    ele->size = 0;\n"
        "    ele->tab_index = 0;\n"
        "    ele->selected_index = -1;\n"
        "    ele->length = 0;\n"
        "    ele->form = (uint8_t *)0;\n"
        "    ele->name = (dom_string *)0;\n"
        "    ele->value = (dom_string *)0;\n"
        "    ele->type_one = (dom_string *)0;\n"
        "    ele->type_multiple = (dom_string *)0;\n"
        "    err = dom_string_create(\"select-one\", 10, &ele->type_one);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        return err;\n"
        "    }\n"
        "    err = dom_string_create(\"select-multiple\", 15, &ele->type_multiple);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        dom_string_unref(ele->type_one);\n"
        "        ele->type_one = (dom_string *)0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_html_select_element_create(uint8_t *params, dom_html_select_element **ele) {\n"
        "    int err;\n"
        "    *ele = (dom_html_select_element *)malloc(128);\n"
        "    if (*ele == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    err = _dom_html_select_element_initialise(params, *ele);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        free((uint8_t *)*ele);\n"
        "        *ele = (dom_html_select_element *)0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_html_select_element_finalise(dom_html_select_element *ele) {\n"
        "    dom_html_select_unref_string(ele->name);\n"
        "    dom_html_select_unref_string(ele->value);\n"
        "    dom_html_select_unref_string(ele->type_one);\n"
        "    dom_html_select_unref_string(ele->type_multiple);\n"
        "}\n"
        "\n"
        "export void _dom_html_select_element_destroy(dom_html_select_element *ele) {\n"
        "    if (ele == 0) {\n"
        "        return;\n"
        "    }\n"
        "    _dom_html_select_element_finalise(ele);\n"
        "    free((uint8_t *)ele);\n"
        "}\n"
        "\n"
        "export int dom_html_select_element_get_type(dom_html_select_element *ele, dom_string **type) {\n"
        "    if (ele->multiple != 0) {\n"
        "        *type = dom_html_select_ref_string(ele->type_multiple);\n"
        "    } else {\n"
        "        *type = dom_html_select_ref_string(ele->type_one);\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int dom_html_select_element_get_selected_index(dom_html_select_element *ele, int32_t *index) { *index = ele->selected_index; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_selected_index(dom_html_select_element *ele, int32_t index) { ele->selected_index = index; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_get_value(dom_html_select_element *ele, dom_string **value) { *value = dom_html_select_ref_string(ele->value); return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_value(dom_html_select_element *ele, dom_string *value) { return dom_html_select_set_string(&ele->value, value); }\n"
        "export int dom_html_select_element_get_length(dom_html_select_element *ele, uint32_t *len) { *len = ele->length; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_length(dom_html_select_element *ele, uint32_t len) { return DOM_NOT_SUPPORTED_ERR; }\n"
        "export int dom_html_select_element_get_form(dom_html_select_element *ele, uint8_t **form) { *form = ele->form; return DOM_NO_ERR; }\n"
        "export int _dom_html_select_element_set_form(dom_html_select_element *ele, uint8_t *form) { ele->form = form; return DOM_NO_ERR; }\n"
        "export int dom__html_select_element_get_options(dom_html_select_element *ele, uint8_t **col) { *col = (uint8_t *)0; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_get_disabled(dom_html_select_element *ele, uint8_t *disabled) { *disabled = ele->disabled; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_disabled(dom_html_select_element *ele, uint8_t disabled) { ele->disabled = disabled; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_get_multiple(dom_html_select_element *ele, uint8_t *multiple) { *multiple = ele->multiple; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_multiple(dom_html_select_element *ele, uint8_t multiple) { ele->multiple = multiple; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_get_name(dom_html_select_element *ele, dom_string **name) { *name = dom_html_select_ref_string(ele->name); return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_name(dom_html_select_element *ele, dom_string *name) { return dom_html_select_set_string(&ele->name, name); }\n"
        "export int dom_html_select_element_get_size(dom_html_select_element *ele, int32_t *size) { *size = (int32_t)ele->size; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_size(dom_html_select_element *ele, int32_t size) { ele->size = (uint32_t)size; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_get_tab_index(dom_html_select_element *ele, int32_t *tab_index) { *tab_index = ele->tab_index; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_set_tab_index(dom_html_select_element *ele, int32_t tab_index) { ele->tab_index = tab_index; return DOM_NO_ERR; }\n"
        "export int dom__html_select_element_add(dom_html_select_element *select, uint8_t *ele, uint8_t *before) { select->length = select->length + 1; return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_remove(dom_html_select_element *ele, int32_t index) { if (index >= 0 && ele->length > 0) { ele->length = ele->length - 1; } return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_blur(dom_html_select_element *ele) { return DOM_NO_ERR; }\n"
        "export int dom_html_select_element_focus(dom_html_select_element *ele) { return DOM_NO_ERR; }\n"
        "export int _dom_html_select_element_parse_attribute(uint8_t *ele, dom_string *name, dom_string *value, dom_string **parsed) { *parsed = dom_html_select_ref_string(value); return DOM_NO_ERR; }\n"
        "export int _dom_html_select_element_copy_internal(dom_html_select_element *old, dom_html_select_element *new) {\n"
        "    new->disabled = old->disabled; new->multiple = old->multiple; new->size = old->size; new->tab_index = old->tab_index;\n"
        "    new->selected_index = old->selected_index; new->length = old->length; new->form = old->form;\n"
        "    new->name = dom_html_select_ref_string(old->name); new->value = dom_html_select_ref_string(old->value);\n"
        "    new->type_one = dom_html_select_ref_string(old->type_one); new->type_multiple = dom_html_select_ref_string(old->type_multiple);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_dom_html_script_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct dom_string {\n"
        "    uint32_t refcnt;\n"
        "    uint32_t type;\n"
        "    uint64_t len;\n"
        "    uint8_t *ptr;\n"
        "};\n"
        "\n"
        "typedef struct dom_string dom_string;\n"
        "\n"
        "struct dom_html_script_element {\n"
        "    uint32_t flags;\n"
        "    uint8_t defer;\n"
        "    uint8_t async;\n"
        "    dom_string *html_for;\n"
        "    dom_string *event;\n"
        "    dom_string *charset;\n"
        "    dom_string *src;\n"
        "    dom_string *type;\n"
        "    dom_string *text;\n"
        "};\n"
        "\n"
        "typedef struct dom_html_script_element dom_html_script_element;\n"
        "\n"
        "extern dom_string *dom_string_ref(dom_string *str);\n"
        "extern void dom_string_unref(dom_string *str);\n"
        "\n"
        "enum {\n"
        "    DOM_NO_ERR = 0,\n"
        "    DOM_NO_MEM_ERR = 1,\n"
        "    DOM_HTML_SCRIPT_ELEMENT_FLAG_NON_BLOCKING = 4\n"
        "};\n"
        "\n"
        "dom_string *dom_html_script_ref_string(dom_string *str) {\n"
        "    if (str == 0) {\n"
        "        return (dom_string *)0;\n"
        "    }\n"
        "    return dom_string_ref(str);\n"
        "}\n"
        "\n"
        "void dom_html_script_unref_string(dom_string *str) {\n"
        "    if (str != 0) {\n"
        "        dom_string_unref(str);\n"
        "    }\n"
        "}\n"
        "\n"
        "int dom_html_script_set_string(dom_string **slot, dom_string *value) {\n"
        "    if (*slot != 0) {\n"
        "        dom_string_unref(*slot);\n"
        "    }\n"
        "    *slot = dom_html_script_ref_string(value);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int _dom_html_script_element_initialise(uint8_t *params, dom_html_script_element *ele) {\n"
        "    ele->flags = DOM_HTML_SCRIPT_ELEMENT_FLAG_NON_BLOCKING;\n"
        "    ele->defer = 0;\n"
        "    ele->async = 0;\n"
        "    ele->html_for = (dom_string *)0;\n"
        "    ele->event = (dom_string *)0;\n"
        "    ele->charset = (dom_string *)0;\n"
        "    ele->src = (dom_string *)0;\n"
        "    ele->type = (dom_string *)0;\n"
        "    ele->text = (dom_string *)0;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_html_script_element_create(uint8_t *params, dom_html_script_element **ele) {\n"
        "    int err;\n"
        "    *ele = (dom_html_script_element *)malloc(128);\n"
        "    if (*ele == 0) {\n"
        "        return DOM_NO_MEM_ERR;\n"
        "    }\n"
        "    err = _dom_html_script_element_initialise(params, *ele);\n"
        "    if (err != DOM_NO_ERR) {\n"
        "        free((uint8_t *)*ele);\n"
        "        *ele = (dom_html_script_element *)0;\n"
        "        return err;\n"
        "    }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_html_script_element_finalise(dom_html_script_element *ele) {\n"
        "    dom_html_script_unref_string(ele->html_for);\n"
        "    dom_html_script_unref_string(ele->event);\n"
        "    dom_html_script_unref_string(ele->charset);\n"
        "    dom_html_script_unref_string(ele->src);\n"
        "    dom_html_script_unref_string(ele->type);\n"
        "    dom_html_script_unref_string(ele->text);\n"
        "}\n"
        "\n"
        "export void _dom_html_script_element_destroy(dom_html_script_element *ele) {\n"
        "    if (ele == 0) {\n"
        "        return;\n"
        "    }\n"
        "    _dom_html_script_element_finalise(ele);\n"
        "    free((uint8_t *)ele);\n"
        "}\n"
        "\n"
        "export int dom_html_script_element_get_html_for(dom_html_script_element *ele, dom_string **value) { *value = dom_html_script_ref_string(ele->html_for); return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_html_for(dom_html_script_element *ele, dom_string *value) { return dom_html_script_set_string(&ele->html_for, value); }\n"
        "export int dom_html_script_element_get_event(dom_html_script_element *ele, dom_string **value) { *value = dom_html_script_ref_string(ele->event); return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_event(dom_html_script_element *ele, dom_string *value) { return dom_html_script_set_string(&ele->event, value); }\n"
        "export int dom_html_script_element_get_charset(dom_html_script_element *ele, dom_string **value) { *value = dom_html_script_ref_string(ele->charset); return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_charset(dom_html_script_element *ele, dom_string *value) { return dom_html_script_set_string(&ele->charset, value); }\n"
        "export int dom_html_script_element_get_src(dom_html_script_element *ele, dom_string **value) { *value = dom_html_script_ref_string(ele->src); return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_src(dom_html_script_element *ele, dom_string *value) { return dom_html_script_set_string(&ele->src, value); }\n"
        "export int dom_html_script_element_get_type(dom_html_script_element *ele, dom_string **value) { *value = dom_html_script_ref_string(ele->type); return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_type(dom_html_script_element *ele, dom_string *value) { return dom_html_script_set_string(&ele->type, value); }\n"
        "export int dom_html_script_element_get_defer(dom_html_script_element *ele, uint8_t *defer) { *defer = ele->defer; return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_defer(dom_html_script_element *ele, uint8_t defer) { ele->defer = defer; return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_get_async(dom_html_script_element *ele, uint8_t *async) { *async = ele->async; return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_async(dom_html_script_element *ele, uint8_t async) { ele->async = async; return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_get_text(dom_html_script_element *ele, dom_string **text) { *text = dom_html_script_ref_string(ele->text); return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_text(dom_html_script_element *ele, dom_string *text) { return dom_html_script_set_string(&ele->text, text); }\n"
        "export int dom_html_script_element_get_flags(dom_html_script_element *ele, uint32_t *flags) { *flags = ele->flags; return DOM_NO_ERR; }\n"
        "export int dom_html_script_element_set_flags(dom_html_script_element *ele, uint32_t flags) { ele->flags = flags; return DOM_NO_ERR; }\n"
        "export int _dom_html_script_element_parse_attribute(uint8_t *ele, dom_string *name, dom_string *value, dom_string **parsed) { *parsed = dom_html_script_ref_string(value); return DOM_NO_ERR; }\n"
        "export int _dom_html_script_element_copy_internal(dom_html_script_element *old, dom_html_script_element *new) {\n"
        "    new->flags = old->flags; new->defer = old->defer; new->async = old->async;\n"
        "    new->html_for = dom_html_script_ref_string(old->html_for); new->event = dom_html_script_ref_string(old->event);\n"
        "    new->charset = dom_html_script_ref_string(old->charset); new->src = dom_html_script_ref_string(old->src);\n"
        "    new->type = dom_html_script_ref_string(old->type); new->text = dom_html_script_ref_string(old->text);\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_bloom_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct bloom_filter {\n"
        "    uint64_t size;\n"
        "    uint32_t items;\n"
        "    uint8_t *filter;\n"
        "};\n"
        "\n"
        "typedef struct bloom_filter bloom_filter;\n"
        "\n"
        "uint32_t bloom_fnv(const uint8_t *datum, uint64_t len) {\n"
        "    uint32_t z;\n"
        "    uint64_t i;\n"
        "    if (datum == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    z = 2166136261;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        z = z * 16777619;\n"
        "        z = z ^ (uint32_t)datum[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return z;\n"
        "}\n"
        "\n"
        "export bloom_filter *bloom_create(uint64_t size) {\n"
        "    bloom_filter *b;\n"
        "    uint8_t *filter;\n"
        "    uint64_t i;\n"
        "    b = (bloom_filter *)malloc(32);\n"
        "    if (b == 0) {\n"
        "        return (bloom_filter *)0;\n"
        "    }\n"
        "    b->filter = malloc(size);\n"
        "    if (b->filter == 0) {\n"
        "        free((uint8_t *)b);\n"
        "        return (bloom_filter *)0;\n"
        "    }\n"
        "    b->size = size;\n"
        "    b->items = 0;\n"
        "    filter = b->filter;\n"
        "    i = 0;\n"
        "    while (i < size) {\n"
        "        filter[i] = 0;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return b;\n"
        "}\n"
        "\n"
        "export void bloom_destroy(bloom_filter *b) {\n"
        "    if (b == 0) {\n"
        "        return;\n"
        "    }\n"
        "    if (b->filter != 0) {\n"
        "        free(b->filter);\n"
        "    }\n"
        "    free((uint8_t *)b);\n"
        "}\n"
        "\n"
        "export void bloom_insert_hash(bloom_filter *b, uint32_t hash) {\n"
        "    uint32_t index;\n"
        "    uint32_t byte_index;\n"
        "    uint32_t bit_index;\n"
        "    uint32_t mask;\n"
        "    uint8_t *filter;\n"
        "    if (b == 0 || b->size == 0 || b->filter == 0) {\n"
        "        return;\n"
        "    }\n"
        "    filter = b->filter;\n"
        "    index = hash % (uint32_t)(b->size << 3);\n"
        "    byte_index = index >> 3;\n"
        "    bit_index = index & 7;\n"
        "    mask = 1 << bit_index;\n"
        "    filter[byte_index] = (uint8_t)((uint32_t)filter[byte_index] | mask);\n"
        "    b->items = b->items + 1;\n"
        "}\n"
        "\n"
        "export void bloom_insert_str(bloom_filter *b, const uint8_t *s, uint64_t z) {\n"
        "    bloom_insert_hash(b, bloom_fnv(s, z));\n"
        "}\n"
        "\n"
        "export uint8_t bloom_search_hash(bloom_filter *b, uint32_t hash) {\n"
        "    uint32_t index;\n"
        "    uint32_t byte_index;\n"
        "    uint32_t bit_index;\n"
        "    uint32_t mask;\n"
        "    uint32_t value;\n"
        "    uint32_t matched;\n"
        "    uint8_t *filter;\n"
        "    if (b == 0 || b->size == 0 || b->filter == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    filter = b->filter;\n"
        "    index = hash % (uint32_t)(b->size << 3);\n"
        "    byte_index = index >> 3;\n"
        "    bit_index = index & 7;\n"
        "    mask = 1 << bit_index;\n"
        "    value = (uint32_t)filter[byte_index];\n"
        "    matched = value & mask;\n"
        "    if (matched != 0) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export uint8_t bloom_search_str(bloom_filter *b, const uint8_t *s, uint64_t z) {\n"
        "    return bloom_search_hash(b, bloom_fnv(s, z));\n"
        "}\n"
        "\n"
        "export uint32_t bloom_items(bloom_filter *b) {\n"
        "    if (b == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return b->items;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_url_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "\n"
        "enum {\n"
        "    NSERROR_OK = 0,\n"
        "    NSERROR_NOMEM = 2,\n"
        "    NSERROR_BAD_PARAMETER = 18\n"
        "};\n"
        "\n"
        "uint64_t url_z_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "uint8_t url_is_hex(uint8_t c) {\n"
        "    if (c >= 48) {\n"
        "        if (c <= 57) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    if (c >= 65) {\n"
        "        if (c <= 70) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    if (c >= 97) {\n"
        "        if (c <= 102) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint8_t url_xdigit_to_hex(uint8_t c) {\n"
        "    if (c >= 48) {\n"
        "        if (c <= 57) {\n"
        "            return c - 48;\n"
        "        }\n"
        "    }\n"
        "    if (c >= 65) {\n"
        "        if (c <= 70) {\n"
        "            return c - 65 + 10;\n"
        "        }\n"
        "    }\n"
        "    return c - 97 + 10;\n"
        "}\n"
        "\n"
        "uint8_t url_hex_digit(uint32_t value) {\n"
        "    if (value < 10) {\n"
        "        return (uint8_t)(48 + value);\n"
        "    }\n"
        "    return (uint8_t)(65 + value - 10);\n"
        "}\n"
        "\n"
        "uint8_t url_contains_char(uint8_t *s, uint8_t c) {\n"
        "    uint64_t i;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (s[i] != 0) {\n"
        "        if (s[i] == c) {\n"
        "            return 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint8_t url_is_unreserved(uint8_t c) {\n"
        "    if (c >= 97) {\n"
        "        if (c <= 122) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    if (c >= 65) {\n"
        "        if (c <= 90) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    if (c >= 48) {\n"
        "        if (c <= 57) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "    if (c == 45) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c == 46) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c == 95) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint8_t url_should_escape(uint8_t c, uint8_t *exceptions) {\n"
        "    if (url_is_unreserved(c) != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (exceptions != 0) {\n"
        "        if (url_contains_char(exceptions, c) != 0) {\n"
        "            return 0;\n"
        "        }\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "export int url_unescape(uint8_t *str, uint64_t length, uint64_t *length_out, uint8_t **result_out) {\n"
        "    uint64_t i;\n"
        "    uint64_t out_i;\n"
        "    uint8_t c;\n"
        "    uint8_t c1;\n"
        "    uint8_t c2;\n"
        "    uint8_t hi;\n"
        "    uint8_t lo;\n"
        "    uint8_t *result;\n"
        "    if (str == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (result_out == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (length == 0) {\n"
        "        length = url_z_strlen(str);\n"
        "    }\n"
        "    result = malloc(length + 1);\n"
        "    if (result == 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    i = 0;\n"
        "    out_i = 0;\n"
        "    while (i < length) {\n"
        "        c = str[i];\n"
        "        if (c == 37) {\n"
        "            if (i + 2 < length) {\n"
        "                c1 = str[i + 1];\n"
        "                c2 = str[i + 2];\n"
        "                if (url_is_hex(c1) != 0) {\n"
        "                    if (url_is_hex(c2) != 0) {\n"
        "                        hi = url_xdigit_to_hex(c1);\n"
        "                        lo = url_xdigit_to_hex(c2);\n"
        "                        c = (uint8_t)(((uint32_t)hi << 4) | (uint32_t)lo);\n"
        "                        i = i + 2;\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        result[out_i] = c;\n"
        "        out_i = out_i + 1;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    result[out_i] = 0;\n"
        "    if (length_out != 0) {\n"
        "        *length_out = out_i;\n"
        "    }\n"
        "    *result_out = result;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int url_escape(uint8_t *unescaped, uint8_t sptoplus, uint8_t *escexceptions, uint8_t **result) {\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint64_t out_i;\n"
        "    uint8_t c;\n"
        "    uint32_t high;\n"
        "    uint32_t low;\n"
        "    uint8_t *escaped;\n"
        "    if (unescaped == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (result == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    len = url_z_strlen(unescaped);\n"
        "    escaped = malloc(len * 3 + 1);\n"
        "    if (escaped == 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    i = 0;\n"
        "    out_i = 0;\n"
        "    while (i < len) {\n"
        "        c = unescaped[i];\n"
        "        if (c == 32) {\n"
        "            if (sptoplus != 0) {\n"
        "                escaped[out_i] = 43;\n"
        "                out_i = out_i + 1;\n"
        "            } else if (url_should_escape(c, escexceptions) != 0) {\n"
        "                escaped[out_i] = 37;\n"
        "                out_i = out_i + 1;\n"
        "                high = ((uint32_t)c >> 4) & 15;\n"
        "                low = (uint32_t)c & 15;\n"
        "                escaped[out_i] = url_hex_digit(high);\n"
        "                out_i = out_i + 1;\n"
        "                escaped[out_i] = url_hex_digit(low);\n"
        "                out_i = out_i + 1;\n"
        "            } else {\n"
        "                escaped[out_i] = c;\n"
        "                out_i = out_i + 1;\n"
        "            }\n"
        "        } else if (url_should_escape(c, escexceptions) != 0) {\n"
        "            escaped[out_i] = 37;\n"
        "            out_i = out_i + 1;\n"
        "            high = ((uint32_t)c >> 4) & 15;\n"
        "            low = (uint32_t)c & 15;\n"
        "            escaped[out_i] = url_hex_digit(high);\n"
        "            out_i = out_i + 1;\n"
        "            escaped[out_i] = url_hex_digit(low);\n"
        "            out_i = out_i + 1;\n"
        "        } else {\n"
        "            escaped[out_i] = c;\n"
        "            out_i = out_i + 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    escaped[out_i] = 0;\n"
        "    *result = escaped;\n"
        "    return NSERROR_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_utils_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "\n"
        "uint64_t nsutils_z_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "uint8_t nsutils_is_space(uint8_t c) {\n"
        "    if (c == 32) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c == 9) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c == 10) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c == 13) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export uint8_t *squash_whitespace(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint64_t j;\n"
        "    uint8_t *c;\n"
        "    if (s == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    len = nsutils_z_strlen(s);\n"
        "    c = malloc(len + 1);\n"
        "    if (c == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    i = 0;\n"
        "    j = 0;\n"
        "    while (s[i] != 0) {\n"
        "        if (nsutils_is_space(s[i]) != 0) {\n"
        "            c[j] = 32;\n"
        "            j = j + 1;\n"
        "            while (nsutils_is_space(s[i]) != 0) {\n"
        "                i = i + 1;\n"
        "            }\n"
        "        } else {\n"
        "            c[j] = s[i];\n"
        "            j = j + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "    c[j] = 0;\n"
        "    return c;\n"
        "}\n"
        "\n"
        "export uint8_t *cnv_space2nbsp(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint64_t j;\n"
        "    uint64_t numNBS;\n"
        "    uint8_t *d;\n"
        "    if (s == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    len = nsutils_z_strlen(s);\n"
        "    i = 0;\n"
        "    numNBS = 0;\n"
        "    while (i < len) {\n"
        "        if (s[i] == 32) {\n"
        "            numNBS = numNBS + 1;\n"
        "        } else if (s[i] == 9) {\n"
        "            numNBS = numNBS + 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    d = malloc(len + numNBS + 1);\n"
        "    if (d == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    i = 0;\n"
        "    j = 0;\n"
        "    while (i < len) {\n"
        "        if (s[i] == 32) {\n"
        "            d[j] = 194;\n"
        "            j = j + 1;\n"
        "            d[j] = 160;\n"
        "            j = j + 1;\n"
        "        } else if (s[i] == 9) {\n"
        "            d[j] = 194;\n"
        "            j = j + 1;\n"
        "            d[j] = 160;\n"
        "            j = j + 1;\n"
        "        } else {\n"
        "            d[j] = s[i];\n"
        "            j = j + 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    d[j] = 0;\n"
        "    return d;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_useragent_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "static uint8_t *core_user_agent_string;\n"
        "\n"
        "uint64_t ua_z_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "uint8_t *ua_z_strdup(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint8_t *copy;\n"
        "    len = ua_z_strlen(s);\n"
        "    copy = malloc(len + 1);\n"
        "    if (copy == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        copy[i] = s[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    copy[len] = 0;\n"
        "    return copy;\n"
        "}\n"
        "\n"
        "uint8_t *user_agent_build_string(void) {\n"
        "    uint8_t format[35] = \"Mozilla/5.0 (LainOS) NetSurf/3.12\";\n"
        "    return ua_z_strdup(format);\n"
        "}\n"
        "\n"
        "export uint8_t *user_agent_string(void) {\n"
        "    if (core_user_agent_string == 0) {\n"
        "        core_user_agent_string = user_agent_build_string();\n"
        "    }\n"
        "    return core_user_agent_string;\n"
        "}\n"
        "\n"
        "export void free_user_agent_string(void) {\n"
        "    if (core_user_agent_string != 0) {\n"
        "        free(core_user_agent_string);\n"
        "        core_user_agent_string = (uint8_t *)0;\n"
        "    }\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_mouse_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "export void browser_mouse_state_dump(uint32_t mouse) {\n"
        "    return;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_searchweb_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct nsurl_s { uint32_t opaque; };\n"
        "struct bitmap_s { uint32_t opaque; };\n"
        "\n"
        "extern int nsurl_create(uint8_t *url, struct nsurl_s **url_out);\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2, NSERROR_INIT_FAILED = 8, NSERROR_BAD_PARAMETER = 18, NSERROR_BAD_URL = 25 };\n"
        "enum { SEARCH_WEB_OMNI_NONE = 0, SEARCH_WEB_OMNI_SEARCHONLY = 1 };\n"
        "\n"
        "global uint8_t *searchweb_provider_name = \"DuckDuckGo\";\n"
        "global uint8_t *searchweb_search_prefix = \"https://www.duckduckgo.com/html/?q=\";\n"
        "global uint8_t searchweb_initialized = 0;\n"
        "\n"
        "uint64_t searchweb_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) { return 0; }\n"
        "    while (s[len] != 0) { len = len + 1; }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "int searchweb_make_default_url(uint8_t *term, struct nsurl_s **url_out) {\n"
        "    uint8_t url[512];\n"
        "    uint64_t i;\n"
        "    uint64_t j;\n"
        "    if (term == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (url_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    i = 0;\n"
        "    while (searchweb_search_prefix[i] != 0 && i < 511) {\n"
        "        url[i] = searchweb_search_prefix[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    j = 0;\n"
        "    while (term[j] != 0 && i < 511) {\n"
        "        if (term[j] == 32) { url[i] = 43; } else { url[i] = term[j]; }\n"
        "        i = i + 1;\n"
        "        j = j + 1;\n"
        "    }\n"
        "    url[i] = 0;\n"
        "    return nsurl_create(url, url_out);\n"
        "}\n"
        "\n"
        "export int search_web_init(uint8_t *provider_fname) {\n"
        "    searchweb_initialized = 1;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int search_web_finalise(void) {\n"
        "    if (searchweb_initialized == 0) { return NSERROR_INIT_FAILED; }\n"
        "    searchweb_initialized = 0;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int64_t search_web_iterate_providers(int64_t iter, uint8_t **name) {\n"
        "    if (searchweb_initialized == 0) { searchweb_initialized = 1; }\n"
        "    if (name == 0) { return -1; }\n"
        "    if (iter < 0) {\n"
        "        *name = searchweb_provider_name;\n"
        "        return 0;\n"
        "    }\n"
        "    return -1;\n"
        "}\n"
        "\n"
        "export int search_web_select_provider(uint8_t *selection) {\n"
        "    if (searchweb_initialized == 0) { return NSERROR_INIT_FAILED; }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int search_web_get_provider_bitmap(struct bitmap_s **bitmap_out) {\n"
        "    if (searchweb_initialized == 0) { return NSERROR_INIT_FAILED; }\n"
        "    if (bitmap_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    *bitmap_out = 0;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int search_web_omni(uint8_t *term, int flags, struct nsurl_s **url_out) {\n"
        "    int ret;\n"
        "    if (term == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (url_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (flags == SEARCH_WEB_OMNI_NONE) {\n"
        "        ret = nsurl_create(term, url_out);\n"
        "        if (ret == NSERROR_OK) { return NSERROR_OK; }\n"
        "    }\n"
        "    if (searchweb_initialized == 0) { searchweb_initialized = 1; }\n"
        "    return searchweb_make_default_url(term, url_out);\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_scrollbar_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *messages_get(uint8_t *key);\n"
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct rect_s { int x0; int y0; int x1; int y1; };\n"
        "struct redraw_context_s { uint32_t opaque; };\n"
        "struct scrollbar_msg_data_s {\n"
        "    struct scrollbar_s *scrollbar;\n"
        "    int msg;\n"
        "    int scroll_offset;\n"
        "    int x0;\n"
        "    int y0;\n"
        "    int x1;\n"
        "    int y1;\n"
        "};\n"
        "struct scrollbar_s {\n"
        "    uint8_t horizontal;\n"
        "    int length;\n"
        "    int full_size;\n"
        "    int visible_size;\n"
        "    int offset;\n"
        "    int bar_pos;\n"
        "    int bar_len;\n"
        "    uint8_t *client_callback;\n"
        "    uint8_t *client_data;\n"
        "    uint8_t dragging;\n"
        "    int drag_start_coord;\n"
        "    int drag_start_pos;\n"
        "    uint8_t drag_content;\n"
        "    struct scrollbar_s *pair;\n"
        "    uint8_t pair_drag;\n"
        "};\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2 };\n"
        "enum { SCROLLBAR_WIDTH = 16, SCROLLBAR_MSG_MOVED = 0, SCROLLBAR_MSG_SCROLL_START = 1, SCROLLBAR_MSG_SCROLL_FINISHED = 2 };\n"
        "enum { SCROLLBAR_MOUSE_NONE = 0, SCROLLBAR_MOUSE_USED = 1, SCROLLBAR_MOUSE_BOTH = 2, SCROLLBAR_MOUSE_UP = 4, SCROLLBAR_MOUSE_PUP = 8, SCROLLBAR_MOUSE_VRT = 16, SCROLLBAR_MOUSE_PDWN = 32, SCROLLBAR_MOUSE_DWN = 64, SCROLLBAR_MOUSE_LFT = 128, SCROLLBAR_MOUSE_PLFT = 256, SCROLLBAR_MOUSE_HRZ = 512, SCROLLBAR_MOUSE_PRGT = 1024, SCROLLBAR_MOUSE_RGT = 2048 };\n"
        "\n"
        "int scrollbar_clamp_offset(struct scrollbar_s *s, int value) {\n"
        "    int max_offset;\n"
        "    if (s == 0) { return 0; }\n"
        "    max_offset = s->full_size - s->visible_size;\n"
        "    if (max_offset < 0) { max_offset = 0; }\n"
        "    if (value < 0) { return 0; }\n"
        "    if (value > max_offset) { return max_offset; }\n"
        "    return value;\n"
        "}\n"
        "\n"
        "void scrollbar_recompute_bar(struct scrollbar_s *s) {\n"
        "    int well_length;\n"
        "    int max_offset;\n"
        "    if (s == 0) { return; }\n"
        "    well_length = s->length - 32;\n"
        "    if (well_length < 0) { well_length = 0; }\n"
        "    if (s->full_size <= 0) { s->bar_len = 0; s->bar_pos = 0; return; }\n"
        "    s->bar_len = (well_length * s->visible_size) / s->full_size;\n"
        "    if (s->bar_len < 1 && s->visible_size > 0) { s->bar_len = 1; }\n"
        "    if (s->bar_len > well_length) { s->bar_len = well_length; }\n"
        "    max_offset = s->full_size - s->visible_size;\n"
        "    if (max_offset <= 0) { s->bar_pos = 0; return; }\n"
        "    s->bar_pos = ((well_length - s->bar_len) * s->offset) / max_offset;\n"
        "}\n"
        "\n"
        "void scrollbar_emit_moved(struct scrollbar_s *s) {\n"
        "    struct scrollbar_msg_data_s data;\n"
        "    if (s == 0) { return; }\n"
        "    return;\n"
        "}\n"
        "\n"
        "export int scrollbar_create(uint8_t horizontal, int length, int full_size, int visible_size, uint8_t *client_data, uint8_t *client_callback, struct scrollbar_s **out) {\n"
        "    struct scrollbar_s *s;\n"
        "    if (out == 0) { return NSERROR_NOMEM; }\n"
        "    s = (struct scrollbar_s *)malloc(128);\n"
        "    if (s == 0) { *out = 0; return NSERROR_NOMEM; }\n"
        "    s->horizontal = horizontal;\n"
        "    s->length = length;\n"
        "    s->full_size = full_size;\n"
        "    s->visible_size = visible_size;\n"
        "    s->offset = 0;\n"
        "    s->bar_pos = 0;\n"
        "    s->bar_len = 0;\n"
        "    s->client_callback = client_callback;\n"
        "    s->client_data = client_data;\n"
        "    s->dragging = 0;\n"
        "    s->drag_start_coord = 0;\n"
        "    s->drag_start_pos = 0;\n"
        "    s->drag_content = 0;\n"
        "    s->pair = (struct scrollbar_s *)0;\n"
        "    s->pair_drag = 0;\n"
        "    scrollbar_recompute_bar(s);\n"
        "    *out = s;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void scrollbar_destroy(struct scrollbar_s *s) {\n"
        "    struct scrollbar_s *pair;\n"
        "    if (s == 0) { return; }\n"
        "    pair = s->pair;\n"
        "    if (pair != 0) { pair->pair = (struct scrollbar_s *)0; }\n"
        "    free((uint8_t *)s);\n"
        "}\n"
        "\n"
        "export int scrollbar_redraw(struct scrollbar_s *s, int x, int y, struct rect_s *clip, int scale, struct redraw_context_s *ctx) { return NSERROR_OK; }\n"
        "\n"
        "export void scrollbar_set(struct scrollbar_s *s, int value, uint8_t bar_pos) {\n"
        "    int well_length;\n"
        "    int max_offset;\n"
        "    if (s == 0) { return; }\n"
        "    if (bar_pos != 0) {\n"
        "        well_length = s->length - 32;\n"
        "        max_offset = s->full_size - s->visible_size;\n"
        "        if (well_length <= s->bar_len || max_offset <= 0) { s->offset = 0; }\n"
        "        else { s->offset = (value * max_offset) / (well_length - s->bar_len); }\n"
        "    } else {\n"
        "        s->offset = value;\n"
        "    }\n"
        "    s->offset = scrollbar_clamp_offset(s, s->offset);\n"
        "    scrollbar_recompute_bar(s);\n"
        "    scrollbar_emit_moved(s);\n"
        "}\n"
        "\n"
        "export uint8_t scrollbar_scroll(struct scrollbar_s *s, int change) {\n"
        "    int old_offset;\n"
        "    if (s == 0) { return 0; }\n"
        "    old_offset = s->offset;\n"
        "    s->offset = scrollbar_clamp_offset(s, s->offset + change);\n"
        "    scrollbar_recompute_bar(s);\n"
        "    if (s->offset != old_offset) { scrollbar_emit_moved(s); return 1; }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export int scrollbar_get_offset(struct scrollbar_s *s) { if (s == 0) { return 0; } return s->offset; }\n"
        "\n"
        "export void scrollbar_set_extents(struct scrollbar_s *s, int length, int visible_size, int full_size) {\n"
        "    if (s == 0) { return; }\n"
        "    if (length >= 0) { s->length = length; }\n"
        "    if (visible_size >= 0) { s->visible_size = visible_size; }\n"
        "    if (full_size >= 0) { s->full_size = full_size; }\n"
        "    s->offset = scrollbar_clamp_offset(s, s->offset);\n"
        "    scrollbar_recompute_bar(s);\n"
        "}\n"
        "\n"
        "export uint8_t scrollbar_is_horizontal(struct scrollbar_s *s) { if (s == 0) { return 0; } return s->horizontal; }\n"
        "\n"
        "export int scrollbar_mouse_action(struct scrollbar_s *s, uint32_t mouse, int x, int y) {\n"
        "    if (s == 0) { return SCROLLBAR_MOUSE_NONE; }\n"
        "    if (mouse == 0) { return SCROLLBAR_MOUSE_NONE; }\n"
        "    if (s->horizontal != 0) { return SCROLLBAR_MOUSE_USED | SCROLLBAR_MOUSE_HRZ; }\n"
        "    return SCROLLBAR_MOUSE_USED | SCROLLBAR_MOUSE_VRT;\n"
        "}\n"
        "\n"
        "export uint8_t *scrollbar_mouse_status_to_message(int status) {\n"
        "    if (status == SCROLLBAR_MOUSE_UP) { return messages_get(\"ScrollUp\"); }\n"
        "    if (status == SCROLLBAR_MOUSE_DWN) { return messages_get(\"ScrollDown\"); }\n"
        "    if (status == SCROLLBAR_MOUSE_LFT) { return messages_get(\"ScrollLeft\"); }\n"
        "    if (status == SCROLLBAR_MOUSE_RGT) { return messages_get(\"ScrollRight\"); }\n"
        "    if (status == SCROLLBAR_MOUSE_HRZ) { return messages_get(\"ScrollHorizontal\"); }\n"
        "    if (status == SCROLLBAR_MOUSE_VRT) { return messages_get(\"ScrollVertical\"); }\n"
        "    if (status == 513) { return messages_get(\"ScrollHorizontal\"); }\n"
        "    if (status == 17) { return messages_get(\"ScrollVertical\"); }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export void scrollbar_mouse_drag_end(struct scrollbar_s *s, uint32_t mouse, int x, int y) { if (s != 0) { s->dragging = 0; s->pair_drag = 0; } }\n"
        "\n"
        "export void scrollbar_start_content_drag(struct scrollbar_s *s, int x, int y) { if (s != 0) { s->dragging = 1; s->drag_content = 1; s->drag_start_coord = x; s->drag_start_pos = s->offset; } }\n"
        "\n"
        "export void scrollbar_make_pair(struct scrollbar_s *horizontal, struct scrollbar_s *vertical) { if (horizontal != 0) { horizontal->pair = vertical; } if (vertical != 0) { vertical->pair = horizontal; } }\n"
        "\n"
        "export uint8_t *scrollbar_get_data(struct scrollbar_s *s) { if (s == 0) { return 0; } return s->client_data; }\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_file_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern uint8_t *strdup(uint8_t *s);\n"
        "extern uint64_t strlen(uint8_t *s);\n"
        "extern uint8_t *strrchr(uint8_t *s, int ch);\n"
        "extern int nsurl_create(uint8_t *url, struct nsurl_s **url_out);\n"
        "extern int url_escape(uint8_t *str, uint8_t sptoplus, uint8_t *escexceptions, uint8_t **result);\n"
        "extern int url_unescape(uint8_t *str, uint64_t length, uint8_t *skip, uint8_t **result);\n"
        "extern uint8_t *nsurl_get_component(struct nsurl_s *url, int component);\n"
        "extern uint8_t *lwc_string_data(uint8_t *s);\n"
        "extern uint64_t lwc_string_length(uint8_t *s);\n"
        "extern void lwc_string_unref(uint8_t *s);\n"
        "extern int rmdir(uint8_t *path);\n"
        "\n"
        "struct nsurl_s { uint32_t opaque; };\n"
        "struct gui_file_table_s {\n"
        "    uint8_t *mkpath;\n"
        "    uint8_t *basename;\n"
        "    uint8_t *nsurl_to_path;\n"
        "    uint8_t *path_to_nsurl;\n"
        "    uint8_t *mkdir_all;\n"
        "};\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2, NSERROR_NOT_FOUND = 3, NSERROR_BAD_PARAMETER = 18, NSERROR_NOT_DIRECTORY = 24, NSERROR_UNKNOWN = 32 };\n"
        "enum { NSURL_PATH = 2 };\n"
        "\n"
        "global struct gui_file_table_s file_table;\n"
        "global struct gui_file_table_s *default_file_table = &file_table;\n"
        "\n"
        "uint64_t file_copy_component(uint8_t *out, uint64_t pos, uint8_t *component) {\n"
        "    uint64_t i;\n"
        "    if (component == 0) { return pos; }\n"
        "    i = 0;\n"
        "    while (component[i] != 0) { out[pos] = component[i]; pos = pos + 1; i = i + 1; }\n"
        "    return pos;\n"
        "}\n"
        "\n"
        "int posix_basename(uint8_t *path, uint8_t **str, uint64_t *size) {\n"
        "    uint8_t *leafname;\n"
        "    uint8_t *fname;\n"
        "    if (path == 0 || str == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    leafname = strrchr(path, 47);\n"
        "    if (leafname == 0) { leafname = path; } else { leafname = leafname + 1; }\n"
        "    fname = strdup(leafname);\n"
        "    if (fname == 0) { return NSERROR_NOMEM; }\n"
        "    *str = fname;\n"
        "    if (size != 0) { *size = strlen(fname); }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "int posix_nsurl_to_path(struct nsurl_s *url, uint8_t **path_out) {\n"
        "    uint8_t *urlpath;\n"
        "    uint8_t *data;\n"
        "    uint64_t length;\n"
        "    int ret;\n"
        "    if (url == 0 || path_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    urlpath = nsurl_get_component(url, NSURL_PATH);\n"
        "    if (urlpath == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    data = lwc_string_data(urlpath);\n"
        "    length = lwc_string_length(urlpath);\n"
        "    ret = url_unescape(data, length, (uint8_t *)0, path_out);\n"
        "    lwc_string_unref(urlpath);\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "int posix_path_to_nsurl(uint8_t *path, struct nsurl_s **url_out) {\n"
        "    uint8_t *escpath;\n"
        "    uint8_t *urlstr;\n"
        "    uint64_t prefix_len;\n"
        "    uint64_t path_len;\n"
        "    uint64_t pos;\n"
        "    int ret;\n"
        "    if (path == 0 || url_out == 0 || path[0] == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    ret = url_escape(path, 0, \"/\", &escpath);\n"
        "    if (ret != NSERROR_OK) { return ret; }\n"
        "    while (escpath[0] == 47) { escpath = escpath + 1; }\n"
        "    prefix_len = 8;\n"
        "    path_len = strlen(escpath);\n"
        "    urlstr = malloc(prefix_len + path_len + 1);\n"
        "    if (urlstr == 0) { return NSERROR_NOMEM; }\n"
        "    urlstr[0] = 102; urlstr[1] = 105; urlstr[2] = 108; urlstr[3] = 101;\n"
        "    urlstr[4] = 58; urlstr[5] = 47; urlstr[6] = 47; urlstr[7] = 47;\n"
        "    pos = file_copy_component(urlstr, prefix_len, escpath);\n"
        "    urlstr[pos] = 0;\n"
        "    ret = nsurl_create(urlstr, url_out);\n"
        "    free(urlstr);\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "int posix_mkdir_all(uint8_t *fname) {\n"
        "    if (fname == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "int posix_vmkpath(uint8_t **str, uint64_t *size, uint64_t nelm, uint8_t *ap) {\n"
        "    return NSERROR_BAD_PARAMETER;\n"
        "}\n"
        "\n"
        "export int netsurf_mkpath(uint8_t **str, uint64_t *size, uint64_t nelm, uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d, uint8_t *e) {\n"
        "    uint64_t total;\n"
        "    uint64_t pos;\n"
        "    uint8_t *out;\n"
        "    if (str == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    total = 0;\n"
        "    if (nelm > 0 && a != 0) { total = total + strlen(a); }\n"
        "    if (nelm > 1 && b != 0) { total = total + 1 + strlen(b); }\n"
        "    if (nelm > 2 && c != 0) { total = total + 1 + strlen(c); }\n"
        "    if (nelm > 3 && d != 0) { total = total + 1 + strlen(d); }\n"
        "    if (nelm > 4 && e != 0) { total = total + 1 + strlen(e); }\n"
        "    if (*str == 0) { out = malloc(total + 1); if (out == 0) { return NSERROR_NOMEM; } *str = out; } else { out = *str; }\n"
        "    pos = 0;\n"
        "    if (nelm > 0 && a != 0) { pos = file_copy_component(out, pos, a); }\n"
        "    if (nelm > 1 && b != 0) { if (pos > 0) { out[pos] = 47; pos = pos + 1; } pos = file_copy_component(out, pos, b); }\n"
        "    if (nelm > 2 && c != 0) { if (pos > 0) { out[pos] = 47; pos = pos + 1; } pos = file_copy_component(out, pos, c); }\n"
        "    if (nelm > 3 && d != 0) { if (pos > 0) { out[pos] = 47; pos = pos + 1; } pos = file_copy_component(out, pos, d); }\n"
        "    if (nelm > 4 && e != 0) { if (pos > 0) { out[pos] = 47; pos = pos + 1; } pos = file_copy_component(out, pos, e); }\n"
        "    out[pos] = 0;\n"
        "    if (size != 0) { *size = pos; }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int netsurf_nsurl_to_path(struct nsurl_s *url, uint8_t **path_out) { return posix_nsurl_to_path(url, path_out); }\n"
        "export int netsurf_path_to_nsurl(uint8_t *path, struct nsurl_s **url) { return posix_path_to_nsurl(path, url); }\n"
        "export int netsurf_mkdir_all(uint8_t *fname) { return posix_mkdir_all(fname); }\n"
        "export int netsurf_recursive_rm(uint8_t *path) { if (path == 0) { return NSERROR_BAD_PARAMETER; } if (rmdir(path) != 0) { return NSERROR_UNKNOWN; } return NSERROR_OK; }\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_filepath_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern uint8_t **calloc(uint64_t count, uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern uint8_t *strdup(uint8_t *s);\n"
        "extern uint64_t strlen(uint8_t *s);\n"
        "extern uint8_t *getenv(uint8_t *name);\n"
        "extern uint8_t *realpath(uint8_t *path, uint8_t *resolved_path);\n"
        "extern int access(uint8_t *path, int amode);\n"
        "\n"
        "enum { PATH_MAX = 4096, MAX_RESPATH = 128, R_OK = 4 };\n"
        "\n"
        "uint64_t filepath_copy(uint8_t *out, uint64_t pos, uint8_t *src, uint64_t cap) {\n"
        "    uint64_t i;\n"
        "    if (out == 0 || src == 0 || cap == 0) { return pos; }\n"
        "    i = 0;\n"
        "    while (src[i] != 0 && pos + 1 < cap) { out[pos] = src[i]; pos = pos + 1; i = i + 1; }\n"
        "    out[pos] = 0;\n"
        "    return pos;\n"
        "}\n"
        "\n"
        "uint64_t filepath_copy_n(uint8_t *out, uint64_t pos, uint8_t *src, uint64_t len, uint64_t cap) {\n"
        "    uint64_t i;\n"
        "    if (out == 0 || src == 0 || cap == 0) { return pos; }\n"
        "    i = 0;\n"
        "    while (i < len && pos + 1 < cap) { out[pos] = src[i]; pos = pos + 1; i = i + 1; }\n"
        "    out[pos] = 0;\n"
        "    return pos;\n"
        "}\n"
        "\n"
        "uint8_t *filepath_join(uint8_t *out, uint64_t cap, uint8_t *a, uint8_t *b) {\n"
        "    uint64_t pos;\n"
        "    if (out == 0 || cap == 0) { return (uint8_t *)0; }\n"
        "    out[0] = 0;\n"
        "    pos = filepath_copy(out, 0, a, cap);\n"
        "    if (pos > 0 && out[pos - 1] != 47 && b != 0 && b[0] != 0 && pos + 1 < cap) { out[pos] = 47; pos = pos + 1; out[pos] = 0; }\n"
        "    pos = filepath_copy(out, pos, b, cap);\n"
        "    return out;\n"
        "}\n"
        "\n"
        "uint8_t *filepath_join3(uint8_t *out, uint64_t cap, uint8_t *a, uint8_t *b, uint8_t *c) {\n"
        "    uint64_t pos;\n"
        "    if (out == 0 || cap == 0) { return (uint8_t *)0; }\n"
        "    filepath_join(out, cap, a, b);\n"
        "    pos = strlen(out);\n"
        "    if (pos > 0 && out[pos - 1] != 47 && c != 0 && c[0] != 0 && pos + 1 < cap) { out[pos] = 47; pos = pos + 1; out[pos] = 0; }\n"
        "    pos = filepath_copy(out, pos, c, cap);\n"
        "    return out;\n"
        "}\n"
        "\n"
        "uint8_t *filepath_strdup_range(uint8_t *start, uint64_t len) {\n"
        "    uint8_t *out;\n"
        "    if (start == 0) { return (uint8_t *)0; }\n"
        "    out = malloc(len + 1);\n"
        "    if (out == 0) { return (uint8_t *)0; }\n"
        "    filepath_copy_n(out, 0, start, len, len + 1);\n"
        "    return out;\n"
        "}\n"
        "\n"
        "uint8_t *filepath_expand_path(uint8_t *path, int pathlen) {\n"
        "    uint8_t *out;\n"
        "    uint8_t *home;\n"
        "    uint64_t home_len;\n"
        "    uint64_t pos;\n"
        "    if (path == 0 || pathlen <= 0) { return (uint8_t *)0; }\n"
        "    if (path[0] == 126) {\n"
        "        home = getenv(\"HOME\");\n"
        "        if (home == 0) { home = \"\"; }\n"
        "        home_len = strlen(home);\n"
        "        out = malloc(home_len + pathlen + 1);\n"
        "        if (out == 0) { return (uint8_t *)0; }\n"
        "        pos = filepath_copy(out, 0, home, home_len + pathlen + 1);\n"
        "        filepath_copy_n(out, pos, path + 1, pathlen - 1, home_len + pathlen + 1);\n"
        "        return out;\n"
        "    }\n"
        "    return filepath_strdup_range(path, pathlen);\n"
        "}\n"
        "\n"
        "uint8_t *filepath_try_real(uint8_t *path, uint8_t *str) {\n"
        "    uint8_t *ret;\n"
        "    uint8_t *out;\n"
        "    if (path == 0) { return (uint8_t *)0; }\n"
        "    if (str == 0) { out = malloc(PATH_MAX); } else { out = str; }\n"
        "    if (out == 0) { return (uint8_t *)0; }\n"
        "    ret = realpath(path, out);\n"
        "    if (ret == 0) { filepath_copy(out, 0, path, PATH_MAX); ret = out; }\n"
        "    if (access(ret, R_OK) != 0) { if (str == 0) { free(out); } return (uint8_t *)0; }\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *filepath_vsfindfile(uint8_t *str, uint8_t *format, uint8_t *ap) {\n"
        "    uint8_t *tmp;\n"
        "    uint8_t *ret;\n"
        "    if (format == 0) { return (uint8_t *)0; }\n"
        "    tmp = malloc(PATH_MAX);\n"
        "    if (tmp == 0) { return (uint8_t *)0; }\n"
        "    filepath_copy(tmp, 0, format, PATH_MAX);\n"
        "    ret = filepath_try_real(tmp, str);\n"
        "    free(tmp);\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *filepath_sfindfile(uint8_t *str, uint8_t *format, uint8_t *a, uint8_t *b, uint8_t *c) {\n"
        "    uint8_t *tmp;\n"
        "    uint8_t *ret;\n"
        "    if (format == 0) { return (uint8_t *)0; }\n"
        "    tmp = malloc(PATH_MAX);\n"
        "    if (tmp == 0) { return (uint8_t *)0; }\n"
        "    if (a != 0 && b != 0) { filepath_join(tmp, PATH_MAX, a, b); } else { filepath_copy(tmp, 0, format, PATH_MAX); }\n"
        "    ret = filepath_try_real(tmp, str);\n"
        "    free(tmp);\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *filepath_findfile(uint8_t *format, uint8_t *a, uint8_t *b, uint8_t *c) {\n"
        "    return filepath_sfindfile((uint8_t *)0, format, a, b, c);\n"
        "}\n"
        "\n"
        "export uint8_t *filepath_sfind(uint8_t **respathv, uint8_t *filepath, uint8_t *filename) {\n"
        "    int respathc;\n"
        "    uint8_t *candidate;\n"
        "    if (respathv == 0 || filepath == 0 || filename == 0) { return (uint8_t *)0; }\n"
        "    if (respathv[0] == 0) { return (uint8_t *)0; }\n"
        "    respathc = 0;\n"
        "    while (respathv[respathc] != 0) {\n"
        "        candidate = respathv[respathc];\n"
        "        filepath_join(filepath, PATH_MAX, candidate, filename);\n"
        "        if (access(filepath, R_OK) == 0) { realpath(filepath, filepath); return filepath; }\n"
        "        respathc = respathc + 1;\n"
        "    }\n"
        "    return (uint8_t *)0;\n"
        "}\n"
        "\n"
        "export uint8_t *filepath_find(uint8_t **respathv, uint8_t *filename) {\n"
        "    uint8_t *filepath;\n"
        "    uint8_t *ret;\n"
        "    if (respathv == 0 || respathv[0] == 0) { return (uint8_t *)0; }\n"
        "    filepath = malloc(PATH_MAX);\n"
        "    if (filepath == 0) { return (uint8_t *)0; }\n"
        "    ret = filepath_sfind(respathv, filepath, filename);\n"
        "    if (ret == 0) { free(filepath); }\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *filepath_sfinddef(uint8_t **respathv, uint8_t *filepath, uint8_t *filename, uint8_t *def) {\n"
        "    uint8_t *ret;\n"
        "    uint8_t *home;\n"
        "    if (respathv == 0 || respathv[0] == 0 || filepath == 0) { return (uint8_t *)0; }\n"
        "    ret = filepath_sfind(respathv, filepath, filename);\n"
        "    if (ret != 0) { return ret; }\n"
        "    if (def == 0) { return (uint8_t *)0; }\n"
        "    if (def[0] == 126) { home = getenv(\"HOME\"); if (home == 0) { home = \"\"; } filepath_join3(filepath, PATH_MAX, home, def + 1, filename); }\n"
        "    else { filepath_join(filepath, PATH_MAX, def, filename); }\n"
        "    realpath(filepath, filepath);\n"
        "    return filepath;\n"
        "}\n"
        "\n"
        "export uint8_t **filepath_generate(uint8_t **pathv, uint8_t **langv) {\n"
        "    uint8_t **respath;\n"
        "    int pathc;\n"
        "    int langc;\n"
        "    int respathc;\n"
        "    uint8_t *tmp;\n"
        "    respath = calloc(MAX_RESPATH, 8);\n"
        "    if (respath == 0) { return 0; }\n"
        "    if (pathv == 0) { return respath; }\n"
        "    pathc = 0;\n"
        "    respathc = 0;\n"
        "    while (pathv[pathc] != 0 && respathc + 1 < MAX_RESPATH) {\n"
        "        if (langv != 0) {\n"
        "            langc = 0;\n"
        "            while (langv[langc] != 0 && respathc + 1 < MAX_RESPATH) {\n"
        "                tmp = malloc(PATH_MAX);\n"
        "                if (tmp != 0) { filepath_join(tmp, PATH_MAX, pathv[pathc], langv[langc]); respath[respathc] = tmp; respathc = respathc + 1; }\n"
        "                langc = langc + 1;\n"
        "            }\n"
        "        }\n"
        "        respath[respathc] = strdup(pathv[pathc]);\n"
        "        if (respath[respathc] != 0) { respathc = respathc + 1; }\n"
        "        pathc = pathc + 1;\n"
        "    }\n"
        "    return respath;\n"
        "}\n"
        "\n"
        "export uint8_t **filepath_path_to_strvec(uint8_t *path) {\n"
        "    uint8_t **vec;\n"
        "    uint64_t start;\n"
        "    uint64_t i;\n"
        "    int outc;\n"
        "    int len;\n"
        "    if (path == 0) { return 0; }\n"
        "    vec = calloc(MAX_RESPATH, 8);\n"
        "    if (vec == 0) { return 0; }\n"
        "    start = 0;\n"
        "    i = 0;\n"
        "    outc = 0;\n"
        "    while (path[i] != 0 && outc + 1 < MAX_RESPATH) {\n"
        "        if (path[i] == 58) { len = i - start; vec[outc] = filepath_expand_path(path + start, len); if (vec[outc] != 0) { outc = outc + 1; } start = i + 1; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (outc + 1 < MAX_RESPATH && i > start) { len = i - start; vec[outc] = filepath_expand_path(path + start, len); }\n"
        "    return vec;\n"
        "}\n"
        "\n"
        "export void filepath_free_strvec(uint8_t **pathv) {\n"
        "    int pathc;\n"
        "    if (pathv == 0) { return; }\n"
        "    pathc = 0;\n"
        "    while (pathv[pathc] != 0) { free(pathv[pathc]); pathc = pathc + 1; }\n"
        "    free(pathv);\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_messages_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern uint8_t *strdup(uint8_t *s);\n"
        "extern struct hash_table *hash_create(uint32_t chains);\n"
        "extern void hash_destroy(struct hash_table *ht);\n"
        "extern int hash_add(struct hash_table *ht, const uint8_t *key, const uint8_t *value);\n"
        "extern uint8_t *hash_get(struct hash_table *ht, const uint8_t *key);\n"
        "extern int hash_add_file(struct hash_table *ht, const uint8_t *path);\n"
        "extern int hash_add_inline(struct hash_table *ht, const uint8_t *data, uint64_t size);\n"
        "\n"
        "struct hash_table { uint32_t opaque; };\n"
        "\n"
        "enum { HASH_SIZE = 101 };\n"
        "enum { NSERROR_OK = 0, NSERROR_UNKNOWN = 1, NSERROR_NOMEM = 2, NSERROR_NO_FETCH_HANDLER = 3, NSERROR_NOT_FOUND = 4, NSERROR_NOT_DIRECTORY = 5, NSERROR_SAVE_FAILED = 6, NSERROR_CLONE_FAILED = 7, NSERROR_INIT_FAILED = 8, NSERROR_BMP_ERROR = 9, NSERROR_GIF_ERROR = 10, NSERROR_ICO_ERROR = 11, NSERROR_PNG_ERROR = 12, NSERROR_SPRITE_ERROR = 13, NSERROR_SVG_ERROR = 14, NSERROR_BAD_ENCODING = 15, NSERROR_NEED_DATA = 16, NSERROR_ENCODING_CHANGE = 17, NSERROR_BAD_PARAMETER = 18, NSERROR_INVALID = 19, NSERROR_BOX_CONVERT = 20, NSERROR_STOPPED = 21, NSERROR_DOM = 22, NSERROR_CSS = 23, NSERROR_CSS_BASE = 24, NSERROR_BAD_URL = 25, NSERROR_BAD_CONTENT = 26, NSERROR_FRAME_DEPTH = 27, NSERROR_PERMISSION = 28, NSERROR_NOSPACE = 29, NSERROR_BAD_SIZE = 30, NSERROR_NOT_IMPLEMENTED = 31, NSERROR_BAD_REDIRECT = 32, NSERROR_CYCLIC_REDIRECT = 33, NSERROR_UNSAFE_REDIRECT = 34, NSERROR_BAD_AUTH = 35, NSERROR_BAD_CERTS = 36, NSERROR_TIMEOUT = 37 };\n"
        "enum { SSL_CERT_ERR_OK = 0, SSL_CERT_ERR_UNKNOWN = 1, SSL_CERT_ERR_BAD_ISSUER = 2, SSL_CERT_ERR_BAD_SIG = 3, SSL_CERT_ERR_TOO_YOUNG = 4, SSL_CERT_ERR_TOO_OLD = 5, SSL_CERT_ERR_SELF_SIGNED = 6, SSL_CERT_ERR_CHAIN_SELF_SIGNED = 7, SSL_CERT_ERR_REVOKED = 8, SSL_CERT_ERR_HOSTNAME_MISMATCH = 9, SSL_CERT_ERR_CERT_MISSING = 10 };\n"
        "\n"
        "global struct hash_table *messages_hash;\n"
        "\n"
        "struct hash_table *messages_create_ctx(int hash_size) {\n"
        "    struct hash_table *ctx;\n"
        "    ctx = hash_create(hash_size);\n"
        "    if (ctx == 0) { return (struct hash_table *)0; }\n"
        "    hash_add(ctx, \"LoginDescription\", \"The site is requesting your username and password.\");\n"
        "    hash_add(ctx, \"PrivacyDescription\", \"A privacy error occurred while communicating with this site.\");\n"
        "    hash_add(ctx, \"TimeoutDescription\", \"A connection to this site could not be established.\");\n"
        "    hash_add(ctx, \"FetchErrorDescription\", \"An error occurred when connecting to this site.\");\n"
        "    return ctx;\n"
        "}\n"
        "\n"
        "void messages_destroy_ctx(struct hash_table *ctx) {\n"
        "    if (ctx != 0) { hash_destroy(ctx); }\n"
        "}\n"
        "\n"
        "int messages_ensure_hash(void) {\n"
        "    if (messages_hash == 0) { messages_hash = messages_create_ctx(HASH_SIZE); }\n"
        "    if (messages_hash == 0) { return NSERROR_NOMEM; }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "uint8_t *messages_get_ctx(uint8_t *key, struct hash_table *ctx) {\n"
        "    uint8_t *value;\n"
        "    if (key == 0) { return \"\"; }\n"
        "    value = (uint8_t *)0;\n"
        "    if (ctx != 0) { value = hash_get(ctx, key); }\n"
        "    if (value == 0) { value = key; }\n"
        "    return value;\n"
        "}\n"
        "\n"
        "int messages_load_ctx(uint8_t *path, struct hash_table **ctx) {\n"
        "    struct hash_table *nctx;\n"
        "    int res;\n"
        "    if (path == 0 || ctx == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (*ctx != 0) { return hash_add_file(*ctx, path); }\n"
        "    nctx = messages_create_ctx(HASH_SIZE);\n"
        "    if (nctx == 0) { return NSERROR_NOMEM; }\n"
        "    res = hash_add_file(nctx, path);\n"
        "    if (res == NSERROR_OK) { *ctx = nctx; } else { hash_destroy(nctx); }\n"
        "    return res;\n"
        "}\n"
        "\n"
        "export int messages_add_from_file(uint8_t *path) {\n"
        "    if (path == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    return messages_load_ctx(path, &messages_hash);\n"
        "}\n"
        "\n"
        "export int messages_add_from_inline(uint8_t *data, uint64_t size) {\n"
        "    int res;\n"
        "    if (data == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    res = messages_ensure_hash();\n"
        "    if (res != NSERROR_OK) { return res; }\n"
        "    return hash_add_inline(messages_hash, data, size);\n"
        "}\n"
        "\n"
        "export int messages_add_key_value(uint8_t *key, uint8_t *value) {\n"
        "    int res;\n"
        "    if (key == 0 || value == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    res = messages_ensure_hash();\n"
        "    if (res != NSERROR_OK) { return res; }\n"
        "    return hash_add(messages_hash, key, value);\n"
        "}\n"
        "\n"
        "export uint8_t *messages_get_buff(uint8_t *key, uint8_t *a, uint8_t *b, uint8_t *c) {\n"
        "    uint8_t *msg;\n"
        "    if (messages_hash == 0 || key == 0) { return (uint8_t *)0; }\n"
        "    msg = hash_get(messages_hash, key);\n"
        "    if (msg == 0) { return (uint8_t *)0; }\n"
        "    return strdup(msg);\n"
        "}\n"
        "\n"
        "export uint8_t *messages_get(uint8_t *key) {\n"
        "    return messages_get_ctx(key, messages_hash);\n"
        "}\n"
        "\n"
        "export uint8_t *messages_get_errorcode(int code) {\n"
        "    if (code == NSERROR_OK) { return messages_get_ctx(\"OK\", messages_hash); }\n"
        "    if (code == NSERROR_NOMEM) { return messages_get_ctx(\"NoMemory\", messages_hash); }\n"
        "    if (code == NSERROR_NO_FETCH_HANDLER) { return messages_get_ctx(\"NoHandler\", messages_hash); }\n"
        "    if (code == NSERROR_NOT_FOUND) { return messages_get_ctx(\"NotFound\", messages_hash); }\n"
        "    if (code == NSERROR_NOT_DIRECTORY) { return messages_get_ctx(\"NotDirectory\", messages_hash); }\n"
        "    if (code == NSERROR_SAVE_FAILED) { return messages_get_ctx(\"SaveFailed\", messages_hash); }\n"
        "    if (code == NSERROR_CLONE_FAILED) { return messages_get_ctx(\"CloneFailed\", messages_hash); }\n"
        "    if (code == NSERROR_INIT_FAILED) { return messages_get_ctx(\"InitFailed\", messages_hash); }\n"
        "    if (code == NSERROR_BMP_ERROR) { return messages_get_ctx(\"BMPError\", messages_hash); }\n"
        "    if (code == NSERROR_GIF_ERROR) { return messages_get_ctx(\"GIFError\", messages_hash); }\n"
        "    if (code == NSERROR_ICO_ERROR) { return messages_get_ctx(\"ICOError\", messages_hash); }\n"
        "    if (code == NSERROR_PNG_ERROR) { return messages_get_ctx(\"PNGError\", messages_hash); }\n"
        "    if (code == NSERROR_SPRITE_ERROR) { return messages_get_ctx(\"SpriteError\", messages_hash); }\n"
        "    if (code == NSERROR_SVG_ERROR) { return messages_get_ctx(\"SVGError\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_ENCODING) { return messages_get_ctx(\"BadEncoding\", messages_hash); }\n"
        "    if (code == NSERROR_NEED_DATA) { return messages_get_ctx(\"NeedData\", messages_hash); }\n"
        "    if (code == NSERROR_ENCODING_CHANGE) { return messages_get_ctx(\"EncodingChanged\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_PARAMETER) { return messages_get_ctx(\"BadParameter\", messages_hash); }\n"
        "    if (code == NSERROR_INVALID) { return messages_get_ctx(\"Invalid\", messages_hash); }\n"
        "    if (code == NSERROR_BOX_CONVERT) { return messages_get_ctx(\"BoxConvert\", messages_hash); }\n"
        "    if (code == NSERROR_STOPPED) { return messages_get_ctx(\"Stopped\", messages_hash); }\n"
        "    if (code == NSERROR_DOM) { return messages_get_ctx(\"ParsingFail\", messages_hash); }\n"
        "    if (code == NSERROR_CSS) { return messages_get_ctx(\"CSSGeneric\", messages_hash); }\n"
        "    if (code == NSERROR_CSS_BASE) { return messages_get_ctx(\"CSSBase\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_URL) { return messages_get_ctx(\"BadURL\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_CONTENT) { return messages_get_ctx(\"BadContent\", messages_hash); }\n"
        "    if (code == NSERROR_FRAME_DEPTH) { return messages_get_ctx(\"FrameDepth\", messages_hash); }\n"
        "    if (code == NSERROR_PERMISSION) { return messages_get_ctx(\"PermissionError\", messages_hash); }\n"
        "    if (code == NSERROR_NOSPACE) { return messages_get_ctx(\"NoSpace\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_SIZE) { return messages_get_ctx(\"BadSize\", messages_hash); }\n"
        "    if (code == NSERROR_NOT_IMPLEMENTED) { return messages_get_ctx(\"NotImplemented\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_REDIRECT) { return messages_get_ctx(\"UnsupportedRedirect\", messages_hash); }\n"
        "    if (code == NSERROR_CYCLIC_REDIRECT) { return messages_get_ctx(\"CyclicRedirect\", messages_hash); }\n"
        "    if (code == NSERROR_UNSAFE_REDIRECT) { return messages_get_ctx(\"UnsafeRedirect\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_AUTH) { return messages_get_ctx(\"BadAuth\", messages_hash); }\n"
        "    if (code == NSERROR_BAD_CERTS) { return messages_get_ctx(\"CertificateVerificationNeeded\", messages_hash); }\n"
        "    if (code == NSERROR_TIMEOUT) { return messages_get_ctx(\"Timeout\", messages_hash); }\n"
        "    return messages_get_ctx(\"Unknown\", messages_hash);\n"
        "}\n"
        "\n"
        "export uint8_t *messages_get_sslcode(int code) {\n"
        "    if (code == SSL_CERT_ERR_OK) { return messages_get_ctx(\"SSLCertErrOk\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_UNKNOWN) { return messages_get_ctx(\"SSLCertErrUnknown\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_BAD_ISSUER) { return messages_get_ctx(\"SSLCertErrBadIssuer\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_BAD_SIG) { return messages_get_ctx(\"SSLCertErrBadSig\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_TOO_YOUNG) { return messages_get_ctx(\"SSLCertErrTooYoung\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_TOO_OLD) { return messages_get_ctx(\"SSLCertErrTooOld\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_SELF_SIGNED) { return messages_get_ctx(\"SSLCertErrSelfSigned\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_CHAIN_SELF_SIGNED) { return messages_get_ctx(\"SSLCertErrChainSelfSigned\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_REVOKED) { return messages_get_ctx(\"SSLCertErrRevoked\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_HOSTNAME_MISMATCH) { return messages_get_ctx(\"SSLCertErrHostnameMismatch\", messages_hash); }\n"
        "    if (code == SSL_CERT_ERR_CERT_MISSING) { return messages_get_ctx(\"SSLCertErrCertMissing\", messages_hash); }\n"
        "    return messages_get_ctx(\"Unknown\", messages_hash);\n"
        "}\n"
        "\n"
        "export void messages_destroy(void) {\n"
        "    messages_destroy_ctx(messages_hash);\n"
        "    messages_hash = (struct hash_table *)0;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_nsoption_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *strdup(uint8_t *s);\n"
        "extern void free(void *ptr);\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2, NSERROR_BAD_PARAMETER = 18, NSERROR_NOT_FOUND = 4 };\n"
        "enum { OPTION_BOOL = 0, OPTION_INTEGER = 1, OPTION_UINT = 2, OPTION_STRING = 3, OPTION_COLOUR = 4 };\n"
        "enum { NSOPTION_http_proxy = 0, NSOPTION_http_proxy_host = 1, NSOPTION_http_proxy_port = 2, NSOPTION_http_proxy_auth = 3, NSOPTION_http_proxy_auth_user = 4, NSOPTION_http_proxy_auth_pass = 5, NSOPTION_http_proxy_noproxy = 6, NSOPTION_treeview_font_size = 7, NSOPTION_font_size = 8, NSOPTION_font_min_size = 9, NSOPTION_font_sans = 10, NSOPTION_font_serif = 11, NSOPTION_font_mono = 12, NSOPTION_font_cursive = 13, NSOPTION_font_fantasy = 14, NSOPTION_accept_language = 15, NSOPTION_accept_charset = 16, NSOPTION_memory_cache_size = 17, NSOPTION_disc_cache_path = 18, NSOPTION_disc_cache_size = 19, NSOPTION_disc_cache_age = 20, NSOPTION_block_advertisements = 21, NSOPTION_disable_popups = 22, NSOPTION_do_not_track = 23, NSOPTION_send_referer = 24, NSOPTION_foreground_images = 25, NSOPTION_background_images = 26, NSOPTION_animate_images = 27, NSOPTION_enable_javascript = 28, NSOPTION_author_level_css = 29, NSOPTION_script_timeout = 30, NSOPTION_expire_url = 31, NSOPTION_font_default = 32, NSOPTION_ca_bundle = 33, NSOPTION_ca_path = 34, NSOPTION_cookie_file = 35, NSOPTION_cookie_jar = 36, NSOPTION_homepage_url = 37, NSOPTION_search_url_bar = 38, NSOPTION_search_web_provider = 39, NSOPTION_url_suggestion = 40, NSOPTION_window_x = 41, NSOPTION_window_y = 42, NSOPTION_window_width = 43, NSOPTION_window_height = 44, NSOPTION_toolbar_status_size = 45, NSOPTION_scale = 46, NSOPTION_incremental_reflow = 47, NSOPTION_min_reflow_period = 48, NSOPTION_core_select_menu = 49, NSOPTION_display_decoded_idn = 50, NSOPTION_max_fetchers = 51, NSOPTION_max_fetchers_per_host = 52, NSOPTION_max_cached_fetch_handles = 53, NSOPTION_max_retried_fetches = 54, NSOPTION_curl_fetch_timeout = 55, NSOPTION_fetch_redirect_limit = 56, NSOPTION_suppress_curl_debug = 57, NSOPTION_target_blank = 58, NSOPTION_button_2_tab = 59, NSOPTION_foreground_new = 60, NSOPTION_margin_top = 61, NSOPTION_margin_bottom = 62, NSOPTION_margin_left = 63, NSOPTION_margin_right = 64, NSOPTION_export_scale = 65, NSOPTION_suppress_images = 66, NSOPTION_remove_backgrounds = 67, NSOPTION_enable_loosening = 68, NSOPTION_enable_PDF_compression = 69, NSOPTION_enable_PDF_password = 70, NSOPTION_prefer_dark_mode = 71, NSOPTION_sys_colour_AccentColor = 72, NSOPTION_sys_colour_VisitedText = 90, NSOPTION_log_filter = 91, NSOPTION_verbose_filter = 92, NSOPTION_LISTEND = 93 };\n"
        "enum { NSOPTION_TABLE_COUNT = 94, NSOPTION_GENERATE_CHANGED = 1 };\n"
        "\n"
        "struct nsoption_s { uint8_t *key; int key_len; int type; uint64_t value; };\n"
        "\n"
        "global struct nsoption_s nsoption_active[94];\n"
        "global struct nsoption_s nsoption_defaults[94];\n"
        "global struct nsoption_s *nsoptions;\n"
        "global struct nsoption_s *nsoptions_default;\n"
        "\n"
        "uint32_t nsoption_strlen(uint8_t *s) { uint32_t n; n = 0; if (s == 0) { return 0; } while (s[n] != 0) { n = n + 1; } return n; }\n"
        "void nsoption_set_entry(struct nsoption_s *table, int idx, uint8_t *key, int type, uint64_t value) {\n"
        "    struct nsoption_s *entry;\n"
        "    entry = table + idx;\n"
        "    entry->key = key;\n"
        "    entry->key_len = nsoption_strlen(key);\n"
        "    entry->type = type;\n"
        "    entry->value = value;\n"
        "}\n"
        "void nsoption_copy_table(struct nsoption_s *dst, struct nsoption_s *src) {\n"
        "    int i;\n"
        "    struct nsoption_s *d;\n"
        "    struct nsoption_s *s;\n"
        "    i = 0;\n"
        "    while (i < NSOPTION_TABLE_COUNT) { d = dst + i; s = src + i; d->key = s->key; d->key_len = s->key_len; d->type = s->type; d->value = s->value; i = i + 1; }\n"
        "}\n"
        "void nsoption_init_defaults(void) {\n"
        "    int i;\n"
        "    i = 0;\n"
        "    while (i < NSOPTION_TABLE_COUNT) { nsoption_set_entry(nsoption_defaults, i, \"\", OPTION_INTEGER, 0); i = i + 1; }\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_http_proxy, \"http_proxy\", OPTION_BOOL, 0);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_http_proxy_host, \"http_proxy_host\", OPTION_STRING, (uint64_t)\"localhost\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_http_proxy_port, \"http_proxy_port\", OPTION_INTEGER, 8080);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_treeview_font_size, \"treeview_font_size\", OPTION_INTEGER, 100);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_font_size, \"font_size\", OPTION_INTEGER, 100);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_font_min_size, \"font_min_size\", OPTION_INTEGER, 85);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_font_sans, \"font_sans\", OPTION_STRING, (uint64_t)\"sans-serif\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_font_serif, \"font_serif\", OPTION_STRING, (uint64_t)\"serif\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_font_mono, \"font_mono\", OPTION_STRING, (uint64_t)\"monospace\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_accept_language, \"accept_language\", OPTION_STRING, (uint64_t)\"en\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_memory_cache_size, \"memory_cache_size\", OPTION_INTEGER, 8388608);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_disc_cache_size, \"disc_cache_size\", OPTION_UINT, 0);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_block_advertisements, \"block_advertisements\", OPTION_BOOL, 0);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_foreground_images, \"foreground_images\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_background_images, \"background_images\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_animate_images, \"animate_images\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_enable_javascript, \"enable_javascript\", OPTION_BOOL, 0);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_author_level_css, \"author_level_css\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_script_timeout, \"script_timeout\", OPTION_INTEGER, 10);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_font_default, \"font_default\", OPTION_INTEGER, 0);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_search_url_bar, \"search_url_bar\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_search_web_provider, \"search_web_provider\", OPTION_INTEGER, 0);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_window_width, \"window_width\", OPTION_INTEGER, 1024);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_window_height, \"window_height\", OPTION_INTEGER, 768);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_scale, \"scale\", OPTION_INTEGER, 100);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_incremental_reflow, \"incremental_reflow\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_min_reflow_period, \"min_reflow_period\", OPTION_INTEGER, 25);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_core_select_menu, \"core_select_menu\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_display_decoded_idn, \"display_decoded_idn\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_max_fetchers, \"max_fetchers\", OPTION_INTEGER, 8);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_max_fetchers_per_host, \"max_fetchers_per_host\", OPTION_INTEGER, 4);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_max_cached_fetch_handles, \"max_cached_fetch_handles\", OPTION_INTEGER, 6);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_max_retried_fetches, \"max_retried_fetches\", OPTION_UINT, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_curl_fetch_timeout, \"curl_fetch_timeout\", OPTION_UINT, 30);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_fetch_redirect_limit, \"fetch_redirect_limit\", OPTION_INTEGER, 10);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_target_blank, \"target_blank\", OPTION_BOOL, 1);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_margin_top, \"margin_top\", OPTION_INTEGER, 10);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_margin_bottom, \"margin_bottom\", OPTION_INTEGER, 10);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_margin_left, \"margin_left\", OPTION_INTEGER, 10);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_margin_right, \"margin_right\", OPTION_INTEGER, 10);\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_prefer_dark_mode, \"prefer_dark_mode\", OPTION_BOOL, 0);\n"
        "    i = NSOPTION_sys_colour_AccentColor;\n"
        "    while (i <= NSOPTION_sys_colour_VisitedText) { nsoption_set_entry(nsoption_defaults, i, \"sys_colour\", OPTION_COLOUR, 16777215); i = i + 1; }\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_log_filter, \"log_filter\", OPTION_STRING, (uint64_t)\"(level:WARNING||cat:jserrors)\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_verbose_filter, \"verbose_filter\", OPTION_STRING, (uint64_t)\"(level:VERBOSE||cat:jserrors)\");\n"
        "    nsoption_set_entry(nsoption_defaults, NSOPTION_LISTEND, \"\", OPTION_INTEGER, 0);\n"
        "}\n"
        "\n"
        "export int nsoption_init(int (*set_defaults)(struct nsoption_s *defaults), struct nsoption_s **popts, struct nsoption_s **pdefs) {\n"
        "    struct nsoption_s *saved;\n"
        "    int res;\n"
        "    nsoption_init_defaults();\n"
        "    nsoptions_default = nsoption_defaults;\n"
        "    saved = nsoptions;\n"
        "    nsoptions = nsoptions_default;\n"
        "    if (set_defaults != 0) { res = set_defaults(nsoptions_default); if (res != NSERROR_OK) { nsoptions = saved; return res; } }\n"
        "    nsoption_copy_table(nsoption_active, nsoptions_default);\n"
        "    nsoptions = nsoption_active;\n"
        "    if (popts != 0) { *popts = nsoptions; }\n"
        "    if (pdefs != 0) { *pdefs = nsoptions_default; }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int nsoption_finalise(struct nsoption_s *opts, struct nsoption_s *defs) { nsoptions = (struct nsoption_s *)0; nsoptions_default = (struct nsoption_s *)0; return NSERROR_OK; }\n"
        "export int nsoption_read(uint8_t *path, struct nsoption_s *opts) { return NSERROR_OK; }\n"
        "\n"
        "export int nsoption_generate(uint8_t *generate_cb, uint8_t *ctx, int flags, struct nsoption_s *opts, struct nsoption_s *defs) { return NSERROR_OK; }\n"
        "\n"
        "export int nsoption_write(uint8_t *path, struct nsoption_s *opts, struct nsoption_s *defs) { return NSERROR_OK; }\n"
        "export int nsoption_dump(uint8_t *outf, struct nsoption_s *opts) { return NSERROR_OK; }\n"
        "export int nsoption_commandline(int *pargc, uint8_t **argv, struct nsoption_s *opts) { return NSERROR_OK; }\n"
        "export int nsoption_snoptionf(uint8_t *string, uint64_t size, int option_idx, uint8_t *fmt) {\n"
        "    uint64_t i;\n"
        "    if (string == 0 || size == 0) { return -1; }\n"
        "    if (fmt == 0) { string[0] = 0; return 0; }\n"
        "    i = 0;\n"
        "    while (i + 1 < size && fmt[i] != 0) { string[i] = fmt[i]; i = i + 1; }\n"
        "    string[i] = 0;\n"
        "    return i;\n"
        "}\n"
        "export int nsoption_set_tbl_charp(struct nsoption_s *opts, int option_idx, uint8_t *s) {\n"
        "    if (opts == 0 || option_idx < 0 || option_idx >= NSOPTION_LISTEND) { return NSERROR_BAD_PARAMETER; }\n"
        "    opts = opts + option_idx;\n"
        "    opts->type = OPTION_STRING;\n"
        "    opts->value = (uint64_t)s;\n"
        "    return NSERROR_OK;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_ssl_certs_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern int nsurl_create(uint8_t *url, struct nsurl_s **url_out);\n"
        "\n"
        "struct nsurl_s { uint32_t opaque; };\n"
        "struct cert_info_s { int err; uint8_t *der; uint64_t der_length; };\n"
        "struct cert_chain { uint64_t depth; struct cert_info_s certs[10]; };\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_UNKNOWN = 1, NSERROR_NOMEM = 2, NSERROR_NEED_DATA = 16, NSERROR_BAD_PARAMETER = 18, NSERROR_INVALID = 19 };\n"
        "enum { SSL_CERT_ERR_OK = 0, SSL_CERT_ERR_CERT_MISSING = 10, MAX_CERT_DEPTH = 10, CERT_CHAIN_SIZE = 248 };\n"
        "\n"
        "struct cert_info_s *cert_chain_cert_at(struct cert_chain *chain, uint64_t idx) {\n"
        "    uint8_t *base;\n"
        "    base = (uint8_t *)chain;\n"
        "    return (struct cert_info_s *)(base + 8 + (idx * 24));\n"
        "}\n"
        "\n"
        "void cert_chain_zero(struct cert_chain *chain) {\n"
        "    uint64_t i;\n"
        "    struct cert_info_s *cert;\n"
        "    if (chain == 0) { return; }\n"
        "    chain->depth = 0;\n"
        "    i = 0;\n"
        "    while (i < MAX_CERT_DEPTH) {\n"
        "        cert = cert_chain_cert_at(chain, i);\n"
        "        cert->err = SSL_CERT_ERR_OK;\n"
        "        cert->der = (uint8_t *)0;\n"
        "        cert->der_length = 0;\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "void cert_chain_free_der(struct cert_chain *chain) {\n"
        "    uint64_t i;\n"
        "    struct cert_info_s *cert;\n"
        "    if (chain == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < chain->depth && i < MAX_CERT_DEPTH) {\n"
        "        cert = cert_chain_cert_at(chain, i);\n"
        "        if (cert->der != 0) { free(cert->der); cert->der = (uint8_t *)0; }\n"
        "        cert->der_length = 0;\n"
        "        cert->err = SSL_CERT_ERR_OK;\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "void cert_chain_copy_bytes(uint8_t *dst, uint8_t *src, uint64_t len) {\n"
        "    uint64_t i;\n"
        "    i = 0;\n"
        "    while (i < len) { dst[i] = src[i]; i = i + 1; }\n"
        "}\n"
        "\n"
        "export int cert_chain_alloc(uint64_t depth, struct cert_chain **chain_out) {\n"
        "    struct cert_chain *chain;\n"
        "    if (chain_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (depth > MAX_CERT_DEPTH) { depth = MAX_CERT_DEPTH; }\n"
        "    chain = (struct cert_chain *)malloc(CERT_CHAIN_SIZE);\n"
        "    if (chain == 0) { *chain_out = (struct cert_chain *)0; return NSERROR_NOMEM; }\n"
        "    cert_chain_zero(chain);\n"
        "    chain->depth = depth;\n"
        "    *chain_out = chain;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int cert_chain_dup_into(struct cert_chain *src, struct cert_chain *dst) {\n"
        "    uint64_t i;\n"
        "    struct cert_info_s *scert;\n"
        "    struct cert_info_s *dcert;\n"
        "    if (src == 0 || dst == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    cert_chain_free_der(dst);\n"
        "    dst->depth = src->depth;\n"
        "    if (dst->depth > MAX_CERT_DEPTH) { dst->depth = MAX_CERT_DEPTH; }\n"
        "    i = 0;\n"
        "    while (i < dst->depth) {\n"
        "        scert = cert_chain_cert_at(src, i);\n"
        "        dcert = cert_chain_cert_at(dst, i);\n"
        "        dcert->err = scert->err;\n"
        "        dcert->der_length = scert->der_length;\n"
        "        dcert->der = (uint8_t *)0;\n"
        "        if (scert->der != 0 && scert->der_length > 0) {\n"
        "            dcert->der = malloc(scert->der_length);\n"
        "            if (dcert->der == 0) { return NSERROR_NOMEM; }\n"
        "            cert_chain_copy_bytes(dcert->der, scert->der, scert->der_length);\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int cert_chain_dup(struct cert_chain *src, struct cert_chain **dst_out) {\n"
        "    struct cert_chain *dst;\n"
        "    int res;\n"
        "    if (src == 0 || dst_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    res = cert_chain_alloc(src->depth, &dst);\n"
        "    if (res != NSERROR_OK) { return res; }\n"
        "    res = cert_chain_dup_into(src, dst);\n"
        "    if (res != NSERROR_OK) { cert_chain_free(dst); return res; }\n"
        "    *dst_out = dst;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int cert_chain_from_query(struct nsurl_s *url, struct cert_chain **chain_out) {\n"
        "    if (url == 0 || chain_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    *chain_out = (struct cert_chain *)0;\n"
        "    return NSERROR_INVALID;\n"
        "}\n"
        "\n"
        "export int cert_chain_to_query(struct cert_chain *chain, struct nsurl_s **url_out) {\n"
        "    if (chain == 0 || url_out == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    return nsurl_create(\"about:certificate\", url_out);\n"
        "}\n"
        "\n"
        "export int cert_chain_free(struct cert_chain *chain) {\n"
        "    if (chain != 0) { cert_chain_free_der(chain); free(chain); }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export uint64_t cert_chain_size(struct cert_chain *chain) {\n"
        "    uint64_t size;\n"
        "    uint64_t i;\n"
        "    struct cert_info_s *cert;\n"
        "    if (chain == 0) { return 0; }\n"
        "    size = CERT_CHAIN_SIZE;\n"
        "    i = 0;\n"
        "    while (i < chain->depth && i < MAX_CERT_DEPTH) {\n"
        "        cert = cert_chain_cert_at(chain, i);\n"
        "        if (cert->der != 0) { size = size + cert->der_length; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return size;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_talloc_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern uint8_t *realloc(void *ptr, uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "\n"
        "enum { TALLOC_HDR_SIZE = 32, TALLOC_MAGIC = 3893689456, TALLOC_MAX_SIZE = 268435456 };\n"
        "struct talloc_hdr { uint64_t magic; uint64_t size; uint8_t *name; uint8_t *parent; };\n"
        "\n"
        "uint64_t talloc_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) { return 0; }\n"
        "    while (s[len] != 0) { len = len + 1; }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "void talloc_memzero(uint8_t *p, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (p == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { p[i] = 0; i = i + 1; }\n"
        "}\n"
        "\n"
        "void talloc_memcopy(uint8_t *dst, uint8_t *src, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (dst == 0 || src == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { dst[i] = src[i]; i = i + 1; }\n"
        "}\n"
        "\n"
        "struct talloc_hdr *talloc_hdr_from_ptr(void *ptr) {\n"
        "    uint8_t *p;\n"
        "    if (ptr == 0) { return (struct talloc_hdr *)0; }\n"
        "    p = (uint8_t *)ptr;\n"
        "    return (struct talloc_hdr *)(p - TALLOC_HDR_SIZE);\n"
        "}\n"
        "\n"
        "uint8_t *talloc_ptr_from_hdr(struct talloc_hdr *hdr) {\n"
        "    uint8_t *p;\n"
        "    if (hdr == 0) { return (uint8_t *)0; }\n"
        "    p = (uint8_t *)hdr;\n"
        "    return p + TALLOC_HDR_SIZE;\n"
        "}\n"
        "\n"
        "uint8_t *talloc_alloc_raw(void *context, uint64_t size, uint8_t *name) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    if (size >= TALLOC_MAX_SIZE) { return (uint8_t *)0; }\n"
        "    hdr = (struct talloc_hdr *)malloc(size + TALLOC_HDR_SIZE);\n"
        "    if (hdr == 0) { return (uint8_t *)0; }\n"
        "    hdr->magic = TALLOC_MAGIC;\n"
        "    hdr->size = size;\n"
        "    hdr->name = name;\n"
        "    hdr->parent = (uint8_t *)context;\n"
        "    return talloc_ptr_from_hdr(hdr);\n"
        "}\n"
        "\n"
        "export uint8_t *_talloc(void *context, uint64_t size) {\n"
        "    return talloc_alloc_raw(context, size, \"talloc\");\n"
        "}\n"
        "\n"
        "export void _talloc_set_destructor(void *ptr, uint8_t *destructor) {\n"
        "    return;\n"
        "}\n"
        "\n"
        "export int talloc_increase_ref_count(void *ptr) { if (ptr == 0) { return -1; } return 0; }\n"
        "export uint64_t talloc_reference_count(void *ptr) { if (ptr == 0) { return 0; } return 1; }\n"
        "export uint8_t *_talloc_reference(void *context, void *ptr) { return (uint8_t *)ptr; }\n"
        "export int talloc_unlink(void *context, void *ptr) { return talloc_free(ptr); }\n"
        "\n"
        "export uint8_t *talloc_set_name(void *ptr, uint8_t *fmt) {\n"
        "    talloc_set_name_const(ptr, fmt);\n"
        "    return fmt;\n"
        "}\n"
        "\n"
        "export void talloc_set_name_const(void *ptr, uint8_t *name) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    hdr = talloc_hdr_from_ptr(ptr);\n"
        "    if (hdr != 0 && hdr->magic == TALLOC_MAGIC) { hdr->name = name; }\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_named(void *context, uint64_t size, uint8_t *fmt) {\n"
        "    return talloc_alloc_raw(context, size, fmt);\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_named_const(void *context, uint64_t size, uint8_t *name) {\n"
        "    return talloc_alloc_raw(context, size, name);\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_get_name(void *ptr) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    hdr = talloc_hdr_from_ptr(ptr);\n"
        "    if (hdr == 0 || hdr->magic != TALLOC_MAGIC || hdr->name == 0) { return \"UNNAMED\"; }\n"
        "    return hdr->name;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_check_name(void *ptr, uint8_t *name) { return (uint8_t *)ptr; }\n"
        "\n"
        "export uint8_t *talloc_parent(void *ptr) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    hdr = talloc_hdr_from_ptr(ptr);\n"
        "    if (hdr == 0 || hdr->magic != TALLOC_MAGIC) { return (uint8_t *)0; }\n"
        "    return hdr->parent;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_parent_name(void *ptr) { return talloc_get_name(talloc_parent(ptr)); }\n"
        "\n"
        "export uint8_t *talloc_init(uint8_t *fmt) { return talloc_named_const((void *)0, 0, fmt); }\n"
        "\n"
        "export int talloc_free(void *ptr) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    hdr = talloc_hdr_from_ptr(ptr);\n"
        "    if (hdr == 0) { return -1; }\n"
        "    if (hdr->magic != TALLOC_MAGIC) { return -1; }\n"
        "    hdr->magic = 0;\n"
        "    free(hdr);\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export void talloc_free_children(void *ptr) { return; }\n"
        "\n"
        "export uint8_t *_talloc_realloc(void *context, void *ptr, uint64_t size, uint8_t *name) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    struct talloc_hdr *nhdr;\n"
        "    if (ptr == 0) { return talloc_alloc_raw(context, size, name); }\n"
        "    if (size == 0) { talloc_free(ptr); return (uint8_t *)0; }\n"
        "    if (size >= TALLOC_MAX_SIZE) { return (uint8_t *)0; }\n"
        "    hdr = talloc_hdr_from_ptr(ptr);\n"
        "    if (hdr == 0 || hdr->magic != TALLOC_MAGIC) { return (uint8_t *)0; }\n"
        "    nhdr = (struct talloc_hdr *)realloc(hdr, size + TALLOC_HDR_SIZE);\n"
        "    if (nhdr == 0) { return (uint8_t *)0; }\n"
        "    nhdr->magic = TALLOC_MAGIC;\n"
        "    nhdr->size = size;\n"
        "    if (name != 0) { nhdr->name = name; }\n"
        "    return talloc_ptr_from_hdr(nhdr);\n"
        "}\n"
        "\n"
        "export uint8_t *_talloc_steal(void *new_ctx, void *ptr) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    hdr = talloc_hdr_from_ptr(ptr);\n"
        "    if (hdr != 0 && hdr->magic == TALLOC_MAGIC) { hdr->parent = (uint8_t *)new_ctx; }\n"
        "    return (uint8_t *)ptr;\n"
        "}\n"
        "\n"
        "export uint8_t *_talloc_move(void *new_ctx, void *pptr) {\n"
        "    uint8_t **slot;\n"
        "    uint8_t *ret;\n"
        "    if (pptr == 0) { return (uint8_t *)0; }\n"
        "    slot = (uint8_t **)pptr;\n"
        "    ret = _talloc_steal(new_ctx, *slot);\n"
        "    *slot = (uint8_t *)0;\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint64_t talloc_total_size(void *ptr) { return talloc_get_size(ptr); }\n"
        "export uint64_t talloc_total_blocks(void *ptr) { if (ptr == 0) { return 0; } return 1; }\n"
        "export void talloc_report_depth_cb(void *ptr, int depth, int max_depth, uint8_t *callback, void *private_data) { return; }\n"
        "export void talloc_report_depth_file(void *ptr, int depth, int max_depth, void *f) { return; }\n"
        "export void talloc_report_full(void *ptr, void *f) { return; }\n"
        "export void talloc_report(void *ptr, void *f) { return; }\n"
        "export void talloc_enable_null_tracking(void) { return; }\n"
        "export void talloc_disable_null_tracking(void) { return; }\n"
        "export void talloc_enable_leak_report(void) { return; }\n"
        "export void talloc_enable_leak_report_full(void) { return; }\n"
        "\n"
        "export uint8_t *_talloc_zero(void *ctx, uint64_t size, uint8_t *name) {\n"
        "    uint8_t *p;\n"
        "    p = talloc_alloc_raw(ctx, size, name);\n"
        "    talloc_memzero(p, size);\n"
        "    return p;\n"
        "}\n"
        "\n"
        "export uint8_t *_talloc_memdup(void *t, uint8_t *p, uint64_t size, uint8_t *name) {\n"
        "    uint8_t *newp;\n"
        "    if (p == 0) { return (uint8_t *)0; }\n"
        "    newp = talloc_alloc_raw(t, size, name);\n"
        "    talloc_memcopy(newp, p, size);\n"
        "    return newp;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_strdup(void *t, uint8_t *p) {\n"
        "    uint64_t len;\n"
        "    uint8_t *ret;\n"
        "    if (p == 0) { return (uint8_t *)0; }\n"
        "    len = talloc_strlen(p) + 1;\n"
        "    ret = _talloc_memdup(t, p, len, p);\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_strndup(void *t, uint8_t *p, uint64_t n) {\n"
        "    uint64_t len;\n"
        "    uint8_t *ret;\n"
        "    if (p == 0) { return (uint8_t *)0; }\n"
        "    len = 0;\n"
        "    while (len < n && p[len] != 0) { len = len + 1; }\n"
        "    ret = talloc_alloc_raw(t, len + 1, p);\n"
        "    talloc_memcopy(ret, p, len);\n"
        "    ret[len] = 0;\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_append_string(void *t, uint8_t *orig, uint8_t *append) {\n"
        "    uint64_t olen;\n"
        "    uint64_t alen;\n"
        "    uint8_t *ret;\n"
        "    if (orig == 0) { return talloc_strdup(t, append); }\n"
        "    if (append == 0) { return orig; }\n"
        "    olen = talloc_strlen(orig);\n"
        "    alen = talloc_strlen(append) + 1;\n"
        "    ret = _talloc_realloc(t, orig, olen + alen, orig);\n"
        "    if (ret == 0) { return (uint8_t *)0; }\n"
        "    talloc_memcopy(ret + olen, append, alen);\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_vasprintf(void *t, uint8_t *fmt, void *ap) { return talloc_strdup(t, fmt); }\n"
        "export uint8_t *talloc_vasprintf_append(uint8_t *s, uint8_t *fmt, void *ap) { return talloc_append_string((void *)0, s, fmt); }\n"
        "export uint8_t *talloc_asprintf(void *t, uint8_t *fmt) { return talloc_strdup(t, fmt); }\n"
        "export uint8_t *talloc_asprintf_append(uint8_t *s, uint8_t *fmt) { return talloc_append_string((void *)0, s, fmt); }\n"
        "\n"
        "export uint8_t *_talloc_array(void *ctx, uint64_t el_size, uint32_t count, uint8_t *name) {\n"
        "    if (el_size != 0 && count >= TALLOC_MAX_SIZE / el_size) { return (uint8_t *)0; }\n"
        "    return talloc_alloc_raw(ctx, el_size * count, name);\n"
        "}\n"
        "\n"
        "export uint8_t *_talloc_zero_array(void *ctx, uint64_t el_size, uint32_t count, uint8_t *name) {\n"
        "    uint8_t *p;\n"
        "    if (el_size != 0 && count >= TALLOC_MAX_SIZE / el_size) { return (uint8_t *)0; }\n"
        "    p = talloc_alloc_raw(ctx, el_size * count, name);\n"
        "    talloc_memzero(p, el_size * count);\n"
        "    return p;\n"
        "}\n"
        "\n"
        "export uint8_t *_talloc_realloc_array(void *ctx, void *ptr, uint64_t el_size, uint32_t count, uint8_t *name) {\n"
        "    if (el_size != 0 && count >= TALLOC_MAX_SIZE / el_size) { return (uint8_t *)0; }\n"
        "    return _talloc_realloc(ctx, ptr, el_size * count, name);\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_realloc_fn(void *context, void *ptr, uint64_t size) { return _talloc_realloc(context, ptr, size, \"realloc\"); }\n"
        "export uint8_t *talloc_autofree_context(void) { return talloc_named_const((void *)0, 0, \"autofree_context\"); }\n"
        "\n"
        "export uint64_t talloc_get_size(void *context) {\n"
        "    struct talloc_hdr *hdr;\n"
        "    hdr = talloc_hdr_from_ptr(context);\n"
        "    if (hdr == 0 || hdr->magic != TALLOC_MAGIC) { return 0; }\n"
        "    return hdr->size;\n"
        "}\n"
        "\n"
        "export uint8_t *talloc_find_parent_byname(void *context, uint8_t *name) { return (uint8_t *)0; }\n"
        "export void talloc_show_parents(void *context, void *file) { return; }\n"
        "export int talloc_is_parent(void *context, void *ptr) { if (context != 0 && context == ptr) { return 1; } return 0; }\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_libdom_attr_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern int _dom_node_initialise(struct dom_node_internal *node, struct dom_document *doc, int type, struct dom_string *name, struct dom_string *value, struct dom_string *ns_uri, struct dom_string *prefix);\n"
        "extern void _dom_node_finalise(struct dom_node_internal *node);\n"
        "extern int _dom_node_readonly(struct dom_node_internal *node);\n"
        "extern int _dom_node_get_node_name(struct dom_node_internal *node, struct dom_string **result);\n"
        "extern int _dom_node_get_node_value(struct dom_node_internal *node, struct dom_string **result);\n"
        "extern int _dom_node_set_node_value(struct dom_node_internal *node, struct dom_string *value);\n"
        "extern int _dom_node_set_prefix(struct dom_node_internal *node, struct dom_string *prefix);\n"
        "extern int _dom_node_copy_internal(struct dom_node_internal *old_node, struct dom_node_internal *new_node);\n"
        "extern struct dom_string *dom_string_ref(struct dom_string *str);\n"
        "extern void dom_string_unref(struct dom_string *str);\n"
        "extern int dom_string_create(const uint8_t *ptr, uint64_t len, struct dom_string **str);\n"
        "\n"
        "struct dom_node_internal { uint8_t bytes[144]; };\n"
        "struct dom_document { uint32_t opaque; };\n"
        "struct dom_string { uint32_t opaque; };\n"
        "struct dom_element { uint32_t opaque; };\n"
        "struct dom_type_info { uint32_t opaque; };\n"
        "struct dom_attr { uint8_t node_bytes[144]; struct dom_type_info *schema_type_info; int type; uint32_t value; uint8_t specified; uint8_t is_id; uint8_t read_only; };\n"
        "\n"
        "enum { DOM_NO_ERR = 0, DOM_NO_MODIFICATION_ALLOWED_ERR = 7, DOM_NOT_SUPPORTED_ERR = 9, DOM_NO_MEM_ERR = 131072, DOM_ATTR_WRONG_TYPE_ERR = 131073 };\n"
        "enum { DOM_ATTRIBUTE_NODE = 2, DOM_ATTR_UNSET = 0, DOM_ATTR_STRING = 1, DOM_ATTR_BOOL = 2, DOM_ATTR_SHORT = 3, DOM_ATTR_INTEGER = 4 };\n"
        "enum { DOM_ATTR_SIZE = 168 };\n"
        "\n"
        "void dom_attr_zero_bytes(uint8_t *ptr, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (ptr == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { ptr[i] = 0; i = i + 1; }\n"
        "}\n"
        "\n"
        "struct dom_node_internal *dom_attr_as_node(struct dom_attr *attr) { return (struct dom_node_internal *)attr; }\n"
        "int dom_attr_readonly_now(struct dom_attr *attr) { if (attr == 0) { return 1; } if (attr->read_only != 0) { return 1; } if (_dom_node_readonly(dom_attr_as_node(attr)) != 0) { return 1; } return 0; }\n"
        "\n"
        "export int _dom_attr_initialise(struct dom_attr *attr, struct dom_document *doc, struct dom_string *name, struct dom_string *ns_uri, struct dom_string *prefix, uint8_t specified) {\n"
        "    int err;\n"
        "    struct dom_node_internal *node;\n"
        "    struct dom_string *empty_value;\n"
        "    if (attr == 0) { return DOM_NO_MEM_ERR; }\n"
        "    dom_attr_zero_bytes((uint8_t *)attr, DOM_ATTR_SIZE);\n"
        "    node = dom_attr_as_node(attr);\n"
        "    empty_value = (struct dom_string *)0;\n"
        "    err = _dom_node_initialise(node, doc, DOM_ATTRIBUTE_NODE, name, empty_value, ns_uri, prefix);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    attr->schema_type_info = (struct dom_type_info *)0;\n"
        "    attr->type = DOM_ATTR_UNSET;\n"
        "    attr->value = 0;\n"
        "    attr->specified = specified;\n"
        "    attr->is_id = 0;\n"
        "    attr->read_only = 0;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_attr_create(struct dom_document *doc, struct dom_string *name, struct dom_string *ns_uri, struct dom_string *prefix, uint8_t specified, struct dom_attr **result) {\n"
        "    struct dom_attr *attr;\n"
        "    int err;\n"
        "    if (result == 0) { return DOM_NO_MEM_ERR; }\n"
        "    attr = (struct dom_attr *)malloc(DOM_ATTR_SIZE);\n"
        "    if (attr == 0) { *result = (struct dom_attr *)0; return DOM_NO_MEM_ERR; }\n"
        "    err = _dom_attr_initialise(attr, doc, name, ns_uri, prefix, specified);\n"
        "    if (err != DOM_NO_ERR) { free(attr); *result = (struct dom_attr *)0; return err; }\n"
        "    *result = attr;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_attr_finalise(struct dom_attr *attr) { if (attr != 0) { _dom_node_finalise(dom_attr_as_node(attr)); } }\n"
        "export void _dom_attr_destroy(struct dom_attr *attr) { if (attr != 0) { _dom_attr_finalise(attr); free(attr); } }\n"
        "export void __dom_attr_destroy(struct dom_attr *attr) { _dom_attr_destroy(attr); }\n"
        "\n"
        "export int dom_attr_get_type(struct dom_attr *attr, int *type) { if (type != 0) { *type = attr->type; } return DOM_NO_ERR; }\n"
        "export int dom_attr_get_integer(struct dom_attr *attr, int32_t *value) { if (attr->type != DOM_ATTR_INTEGER) { return DOM_ATTR_WRONG_TYPE_ERR; } if (value != 0) { *value = (int32_t)attr->value; } return DOM_NO_ERR; }\n"
        "export int dom_attr_get_short(struct dom_attr *attr, int16_t *value) { if (attr->type != DOM_ATTR_SHORT) { return DOM_ATTR_WRONG_TYPE_ERR; } if (value != 0) { *value = (int16_t)attr->value; } return DOM_NO_ERR; }\n"
        "export int dom_attr_get_bool(struct dom_attr *attr, uint8_t *value) { if (attr->type != DOM_ATTR_BOOL) { return DOM_ATTR_WRONG_TYPE_ERR; } if (value != 0) { *value = (uint8_t)attr->value; } return DOM_NO_ERR; }\n"
        "\n"
        "int dom_attr_accept_type(struct dom_attr *attr, int type) { if (dom_attr_readonly_now(attr) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; } if (attr->type != DOM_ATTR_UNSET && attr->type != type) { return DOM_ATTR_WRONG_TYPE_ERR; } attr->type = type; attr->specified = 1; return DOM_NO_ERR; }\n"
        "export int dom_attr_set_integer(struct dom_attr *attr, int32_t value) { int err; err = dom_attr_accept_type(attr, DOM_ATTR_INTEGER); if (err != DOM_NO_ERR) { return err; } attr->value = (uint32_t)value; return DOM_NO_ERR; }\n"
        "export int dom_attr_set_short(struct dom_attr *attr, int16_t value) { int err; err = dom_attr_accept_type(attr, DOM_ATTR_SHORT); if (err != DOM_NO_ERR) { return err; } attr->value = (uint32_t)value; return DOM_NO_ERR; }\n"
        "export int dom_attr_set_bool(struct dom_attr *attr, uint8_t value) { int err; err = dom_attr_accept_type(attr, DOM_ATTR_BOOL); if (err != DOM_NO_ERR) { return err; } attr->value = (uint32_t)value; return DOM_NO_ERR; }\n"
        "export void dom_attr_mark_readonly(struct dom_attr *attr) { if (attr != 0) { attr->read_only = 1; } }\n"
        "\n"
        "export int _dom_attr_get_name(struct dom_attr *attr, struct dom_string **result) { return _dom_node_get_node_name(dom_attr_as_node(attr), result); }\n"
        "export int _dom_attr_get_node_value(struct dom_attr *attr, struct dom_string **result) { return _dom_attr_get_value(attr, result); }\n"
        "export int _dom_attr_get_value(struct dom_attr *attr, struct dom_string **result) {\n"
        "    int err;\n"
        "    if (result == 0) { return DOM_NO_ERR; }\n"
        "    err = _dom_node_get_node_value(dom_attr_as_node(attr), result);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    if (*result == 0) { return dom_string_create(\"\", 0, result); }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "export int _dom_attr_set_value(struct dom_attr *attr, struct dom_string *value) { if (dom_attr_readonly_now(attr) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; } attr->type = DOM_ATTR_STRING; attr->specified = 1; return _dom_node_set_node_value(dom_attr_as_node(attr), value); }\n"
        "export int _dom_attr_set_prefix(struct dom_attr *attr, struct dom_string *prefix) { if (dom_attr_readonly_now(attr) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; } return _dom_node_set_prefix(dom_attr_as_node(attr), prefix); }\n"
        "export int _dom_attr_get_specified(struct dom_attr *attr, uint8_t *result) { if (result != 0) { *result = attr->specified; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_set_specified(struct dom_attr *attr, uint8_t specified) { if (attr != 0) { attr->specified = specified; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_is_id(struct dom_attr *attr, uint8_t *result) { if (result != 0) { *result = attr->is_id; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_set_isid(struct dom_attr *attr, uint8_t is_id) { if (attr != 0) { attr->is_id = is_id; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_readonly(struct dom_attr *attr, uint8_t readonly) { if (attr != 0) { attr->read_only = readonly; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_get_owner(struct dom_attr *attr, struct dom_element **result) { if (result != 0) { *result = (struct dom_element *)0; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_get_schema_type_info(struct dom_attr *attr, struct dom_type_info **result) { if (result != 0) { *result = attr->schema_type_info; } return DOM_NOT_SUPPORTED_ERR; }\n"
        "\n"
        "export int _dom_attr_lookup_prefix(struct dom_attr *attr, struct dom_string *ns_uri, struct dom_string **result) { if (result != 0) { *result = (struct dom_string *)0; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_is_default_namespace(struct dom_attr *attr, struct dom_string *ns_uri, uint8_t *result) { if (result != 0) { *result = 0; } return DOM_NO_ERR; }\n"
        "export int _dom_attr_lookup_namespace(struct dom_attr *attr, struct dom_string *prefix, struct dom_string **result) { if (result != 0) { *result = (struct dom_string *)0; } return DOM_NO_ERR; }\n"
        "\n"
        "export int _dom_attr_copy(struct dom_attr *old_attr, struct dom_attr **result) {\n"
        "    struct dom_attr *copy;\n"
        "    int err;\n"
        "    if (result == 0) { return DOM_NO_MEM_ERR; }\n"
        "    copy = (struct dom_attr *)malloc(DOM_ATTR_SIZE);\n"
        "    if (copy == 0) { *result = (struct dom_attr *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_attr_zero_bytes((uint8_t *)copy, DOM_ATTR_SIZE);\n"
        "    err = _dom_node_copy_internal(dom_attr_as_node(old_attr), dom_attr_as_node(copy));\n"
        "    if (err != DOM_NO_ERR) { free(copy); *result = (struct dom_attr *)0; return err; }\n"
        "    copy->schema_type_info = old_attr->schema_type_info;\n"
        "    copy->type = old_attr->type;\n"
        "    copy->value = old_attr->value;\n"
        "    copy->specified = old_attr->specified;\n"
        "    copy->is_id = old_attr->is_id;\n"
        "    copy->read_only = old_attr->read_only;\n"
        "    *result = copy;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "export int _dom_attr_clone_node(struct dom_attr *attr, uint8_t deep, struct dom_attr **result) { return _dom_attr_copy(attr, result); }\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_libdom_cdata_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern int _dom_text_initialise(struct dom_text *text, struct dom_document *doc, int type, struct dom_string *name, struct dom_string *value);\n"
        "extern void _dom_text_finalise(struct dom_text *text);\n"
        "extern int _dom_text_copy_internal(struct dom_text *old_text, struct dom_text *new_text);\n"
        "\n"
        "struct dom_document { uint32_t opaque; };\n"
        "struct dom_string { uint32_t opaque; };\n"
        "struct dom_node_internal { uint8_t bytes[144]; };\n"
        "struct dom_text { uint8_t bytes[152]; };\n"
        "struct dom_cdata_section { uint8_t bytes[152]; };\n"
        "\n"
        "enum { DOM_NO_ERR = 0, DOM_NO_MEM_ERR = 131072, DOM_CDATA_SECTION_NODE = 4, DOM_CDATA_SIZE = 152 };\n"
        "\n"
        "void dom_cdata_zero(uint8_t *ptr, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (ptr == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { ptr[i] = 0; i = i + 1; }\n"
        "}\n"
        "\n"
        "export int _dom_cdata_section_create(struct dom_document *doc, struct dom_string *name, struct dom_string *value, struct dom_cdata_section **result) {\n"
        "    struct dom_cdata_section *cdata;\n"
        "    struct dom_text *text;\n"
        "    int err;\n"
        "    if (result == 0) { return DOM_NO_MEM_ERR; }\n"
        "    cdata = (struct dom_cdata_section *)malloc(DOM_CDATA_SIZE);\n"
        "    if (cdata == 0) { *result = (struct dom_cdata_section *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_cdata_zero((uint8_t *)cdata, DOM_CDATA_SIZE);\n"
        "    text = (struct dom_text *)cdata;\n"
        "    err = _dom_text_initialise(text, doc, DOM_CDATA_SECTION_NODE, name, value);\n"
        "    if (err != DOM_NO_ERR) { free(cdata); *result = (struct dom_cdata_section *)0; return err; }\n"
        "    *result = cdata;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_cdata_section_destroy(struct dom_cdata_section *cdata) {\n"
        "    struct dom_text *text;\n"
        "    if (cdata == 0) { return; }\n"
        "    text = (struct dom_text *)cdata;\n"
        "    _dom_text_finalise(text);\n"
        "    free(cdata);\n"
        "}\n"
        "\n"
        "export void __dom_cdata_section_destroy(struct dom_node_internal *node) {\n"
        "    struct dom_cdata_section *cdata;\n"
        "    cdata = (struct dom_cdata_section *)node;\n"
        "    _dom_cdata_section_destroy(cdata);\n"
        "}\n"
        "\n"
        "export int _dom_cdata_section_copy(struct dom_node_internal *old, struct dom_node_internal **copy) {\n"
        "    struct dom_cdata_section *new_cdata;\n"
        "    struct dom_text *old_text;\n"
        "    struct dom_text *new_text;\n"
        "    int err;\n"
        "    if (copy == 0) { return DOM_NO_MEM_ERR; }\n"
        "    new_cdata = (struct dom_cdata_section *)malloc(DOM_CDATA_SIZE);\n"
        "    if (new_cdata == 0) { *copy = (struct dom_node_internal *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_cdata_zero((uint8_t *)new_cdata, DOM_CDATA_SIZE);\n"
        "    old_text = (struct dom_text *)old;\n"
        "    new_text = (struct dom_text *)new_cdata;\n"
        "    err = _dom_text_copy_internal(old_text, new_text);\n"
        "    if (err != DOM_NO_ERR) { free(new_cdata); *copy = (struct dom_node_internal *)0; return err; }\n"
        "    *copy = (struct dom_node_internal *)new_cdata;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_libdom_characterdata_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern int _dom_node_initialise(struct dom_node_internal *node, struct dom_document *doc, int type, struct dom_string *name, struct dom_string *value, struct dom_string *ns_uri, struct dom_string *prefix);\n"
        "extern void _dom_node_finalise(struct dom_node_internal *node);\n"
        "extern int _dom_node_readonly(struct dom_node_internal *node);\n"
        "extern int _dom_node_copy_internal(struct dom_node_internal *old_node, struct dom_node_internal *new_node);\n"
        "extern struct dom_string *dom_string_ref(struct dom_string *str);\n"
        "extern void dom_string_unref(struct dom_string *str);\n"
        "extern uint32_t dom_string_length(struct dom_string *str);\n"
        "extern int dom_string_substr(struct dom_string *str, uint32_t offset, uint32_t end, struct dom_string **result);\n"
        "extern int dom_string_concat(struct dom_string *a, struct dom_string *b, struct dom_string **result);\n"
        "extern int dom_string_insert(struct dom_string *target, struct dom_string *insert, uint32_t offset, struct dom_string **result);\n"
        "extern int dom_string_replace(struct dom_string *target, struct dom_string *replacement, uint32_t offset, uint32_t end, struct dom_string **result);\n"
        "extern int _dom_dispatch_characterdata_modified_event(struct dom_document *doc, struct dom_node_internal *node, struct dom_string *prev, struct dom_string *new_value, uint8_t *success);\n"
        "extern int _dom_dispatch_subtree_modified_event(struct dom_document *doc, struct dom_node_internal *node, uint8_t *success);\n"
        "\n"
        "struct dom_string { uint32_t opaque; };\n"
        "struct dom_document { uint8_t bytes_to_empty[304]; struct dom_string *_memo_empty; };\n"
        "struct dom_node_internal { uint8_t bytes_to_value[32]; struct dom_string *value; uint8_t bytes_to_parent[8]; struct dom_node_internal *parent; uint8_t bytes_to_owner[32]; struct dom_document *owner; uint8_t tail[48]; };\n"
        "struct dom_characterdata { uint8_t bytes[144]; };\n"
        "\n"
        "enum { DOM_NO_ERR = 0, DOM_INDEX_SIZE_ERR = 1, DOM_NO_MODIFICATION_ALLOWED_ERR = 7, DOM_NO_MEM_ERR = 131072, DOM_CHARACTERDATA_SIZE = 144 };\n"
        "global uint8_t characterdata_vtable[384];\n"
        "\n"
        "void dom_characterdata_zero(uint8_t *ptr, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (ptr == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { ptr[i] = 0; i = i + 1; }\n"
        "}\n"
        "\n"
        "struct dom_node_internal *dom_characterdata_as_node(struct dom_characterdata *cdata) { return (struct dom_node_internal *)cdata; }\n"
        "struct dom_document *dom_node_get_owner(struct dom_characterdata *cdata) { struct dom_node_internal *node; node = dom_characterdata_as_node(cdata); return node->owner; }\n"
        "\n"
        "export struct dom_characterdata *_dom_characterdata_create(void) {\n"
        "    struct dom_characterdata *cdata;\n"
        "    cdata = (struct dom_characterdata *)malloc(DOM_CHARACTERDATA_SIZE);\n"
        "    if (cdata == 0) { return (struct dom_characterdata *)0; }\n"
        "    dom_characterdata_zero((uint8_t *)cdata, DOM_CHARACTERDATA_SIZE);\n"
        "    return cdata;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_initialise(struct dom_characterdata *cdata, struct dom_document *doc, int type, struct dom_string *name, struct dom_string *value) {\n"
        "    if (cdata == 0) { return DOM_NO_MEM_ERR; }\n"
        "    return _dom_node_initialise(dom_characterdata_as_node(cdata), doc, type, name, value, (struct dom_string *)0, (struct dom_string *)0);\n"
        "}\n"
        "\n"
        "export void _dom_characterdata_finalise(struct dom_characterdata *cdata) { if (cdata != 0) { _dom_node_finalise(dom_characterdata_as_node(cdata)); } }\n"
        "\n"
        "export int _dom_characterdata_get_data(struct dom_characterdata *cdata, struct dom_string **data) {\n"
        "    struct dom_node_internal *node;\n"
        "    if (data == 0) { return DOM_NO_ERR; }\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (node->value != 0) { dom_string_ref(node->value); }\n"
        "    *data = node->value;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "int dom_characterdata_replace_value(struct dom_characterdata *cdata, struct dom_string *new_value) {\n"
        "    struct dom_node_internal *node;\n"
        "    struct dom_document *doc;\n"
        "    uint8_t success;\n"
        "    int err;\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (_dom_node_readonly(node) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; }\n"
        "    doc = node->owner;\n"
        "    success = 1;\n"
        "    err = _dom_dispatch_characterdata_modified_event(doc, node, node->value, new_value, &success);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    if (node->value != 0) { dom_string_unref(node->value); }\n"
        "    if (new_value != 0) { dom_string_ref(new_value); }\n"
        "    node->value = new_value;\n"
        "    success = 1;\n"
        "    return _dom_dispatch_subtree_modified_event(doc, node->parent, &success);\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_set_data(struct dom_characterdata *cdata, struct dom_string *data) { return dom_characterdata_replace_value(cdata, data); }\n"
        "\n"
        "export int _dom_characterdata_get_length(struct dom_characterdata *cdata, uint32_t *length) {\n"
        "    struct dom_node_internal *node;\n"
        "    if (length == 0) { return DOM_NO_ERR; }\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (node->value != 0) { *length = dom_string_length(node->value); } else { *length = 0; }\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_substring_data(struct dom_characterdata *cdata, uint32_t offset, uint32_t count, struct dom_string **data) {\n"
        "    struct dom_node_internal *node;\n"
        "    uint32_t len;\n"
        "    uint32_t end;\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (node->value != 0) { len = dom_string_length(node->value); } else { len = 0; }\n"
        "    if (offset > len) { return DOM_INDEX_SIZE_ERR; }\n"
        "    end = offset + count;\n"
        "    if (end < offset || end > len) { end = len; }\n"
        "    return dom_string_substr(node->value, offset, end, data);\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_append_data(struct dom_characterdata *cdata, struct dom_string *data) {\n"
        "    struct dom_node_internal *node;\n"
        "    struct dom_string *temp;\n"
        "    int err;\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (_dom_node_readonly(node) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; }\n"
        "    err = dom_string_concat(node->value, data, &temp);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    err = dom_characterdata_replace_value(cdata, temp);\n"
        "    dom_string_unref(temp);\n"
        "    return err;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_insert_data(struct dom_characterdata *cdata, uint32_t offset, struct dom_string *data) {\n"
        "    struct dom_node_internal *node;\n"
        "    struct dom_string *temp;\n"
        "    uint32_t len;\n"
        "    int err;\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (_dom_node_readonly(node) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; }\n"
        "    if (node->value != 0) { len = dom_string_length(node->value); } else { len = 0; }\n"
        "    if (offset > len) { return DOM_INDEX_SIZE_ERR; }\n"
        "    err = dom_string_insert(node->value, data, offset, &temp);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    err = dom_characterdata_replace_value(cdata, temp);\n"
        "    dom_string_unref(temp);\n"
        "    return err;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_delete_data(struct dom_characterdata *cdata, uint32_t offset, uint32_t count) {\n"
        "    struct dom_node_internal *node;\n"
        "    struct dom_string *temp;\n"
        "    struct dom_string *empty;\n"
        "    uint32_t len;\n"
        "    uint32_t end;\n"
        "    int err;\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (_dom_node_readonly(node) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; }\n"
        "    if (node->value != 0) { len = dom_string_length(node->value); } else { len = 0; }\n"
        "    if (offset > len) { return DOM_INDEX_SIZE_ERR; }\n"
        "    end = offset + count;\n"
        "    if (end < offset || end > len) { end = len; }\n"
        "    if (node->owner != 0) { empty = node->owner->_memo_empty; } else { empty = (struct dom_string *)0; }\n"
        "    err = dom_string_replace(node->value, empty, offset, end, &temp);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    err = dom_characterdata_replace_value(cdata, temp);\n"
        "    dom_string_unref(temp);\n"
        "    return err;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_replace_data(struct dom_characterdata *cdata, uint32_t offset, uint32_t count, struct dom_string *data) {\n"
        "    struct dom_node_internal *node;\n"
        "    struct dom_string *temp;\n"
        "    uint32_t len;\n"
        "    uint32_t end;\n"
        "    int err;\n"
        "    node = dom_characterdata_as_node(cdata);\n"
        "    if (_dom_node_readonly(node) != 0) { return DOM_NO_MODIFICATION_ALLOWED_ERR; }\n"
        "    if (node->value != 0) { len = dom_string_length(node->value); } else { len = 0; }\n"
        "    if (offset > len) { return DOM_INDEX_SIZE_ERR; }\n"
        "    end = offset + count;\n"
        "    if (end < offset || end > len) { end = len; }\n"
        "    err = dom_string_replace(node->value, data, offset, end, &temp);\n"
        "    if (err != DOM_NO_ERR) { return err; }\n"
        "    err = dom_characterdata_replace_value(cdata, temp);\n"
        "    dom_string_unref(temp);\n"
        "    return err;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_get_text_content(struct dom_node_internal *node, struct dom_string **result) { return _dom_characterdata_get_data((struct dom_characterdata *)node, result); }\n"
        "export int _dom_characterdata_set_text_content(struct dom_node_internal *node, struct dom_string *content) { return _dom_characterdata_set_data((struct dom_characterdata *)node, content); }\n"
        "export void _dom_characterdata_destroy(struct dom_node_internal *node) { return; }\n"
        "\n"
        "export int _dom_characterdata_copy(struct dom_node_internal *old_node, struct dom_node_internal **copy) {\n"
        "    struct dom_characterdata *new_node;\n"
        "    int err;\n"
        "    if (copy == 0) { return DOM_NO_MEM_ERR; }\n"
        "    new_node = (struct dom_characterdata *)malloc(DOM_CHARACTERDATA_SIZE);\n"
        "    if (new_node == 0) { *copy = (struct dom_node_internal *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_characterdata_zero((uint8_t *)new_node, DOM_CHARACTERDATA_SIZE);\n"
        "    err = _dom_characterdata_copy_internal((struct dom_characterdata *)old_node, new_node);\n"
        "    if (err != DOM_NO_ERR) { free(new_node); *copy = (struct dom_node_internal *)0; return err; }\n"
        "    *copy = (struct dom_node_internal *)new_node;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export int _dom_characterdata_copy_internal(struct dom_characterdata *old_node, struct dom_characterdata *new_node) { return _dom_node_copy_internal(dom_characterdata_as_node(old_node), dom_characterdata_as_node(new_node)); }\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_libdom_comment_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern int _dom_characterdata_initialise(struct dom_characterdata *cdata, struct dom_document *doc, int type, struct dom_string *name, struct dom_string *value);\n"
        "extern void _dom_characterdata_finalise(struct dom_characterdata *cdata);\n"
        "extern int _dom_characterdata_copy_internal(struct dom_characterdata *old_node, struct dom_characterdata *new_node);\n"
        "\n"
        "struct dom_document { uint32_t opaque; };\n"
        "struct dom_string { uint32_t opaque; };\n"
        "struct dom_node_internal { uint8_t bytes[144]; };\n"
        "struct dom_characterdata { uint8_t bytes[144]; };\n"
        "struct dom_comment { uint8_t bytes[144]; };\n"
        "\n"
        "enum { DOM_NO_ERR = 0, DOM_NO_MEM_ERR = 131072, DOM_COMMENT_NODE = 8, DOM_COMMENT_SIZE = 144 };\n"
        "global uint8_t comment_protect_vtable[16];\n"
        "\n"
        "void dom_comment_zero(uint8_t *ptr, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (ptr == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { ptr[i] = 0; i = i + 1; }\n"
        "}\n"
        "\n"
        "export int _dom_comment_create(struct dom_document *doc, struct dom_string *name, struct dom_string *value, struct dom_comment **result) {\n"
        "    struct dom_comment *comment;\n"
        "    int err;\n"
        "    if (result == 0) { return DOM_NO_MEM_ERR; }\n"
        "    comment = (struct dom_comment *)malloc(DOM_COMMENT_SIZE);\n"
        "    if (comment == 0) { *result = (struct dom_comment *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_comment_zero((uint8_t *)comment, DOM_COMMENT_SIZE);\n"
        "    err = _dom_characterdata_initialise((struct dom_characterdata *)comment, doc, DOM_COMMENT_NODE, name, value);\n"
        "    if (err != DOM_NO_ERR) { free(comment); *result = (struct dom_comment *)0; return err; }\n"
        "    *result = comment;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_comment_destroy(struct dom_comment *comment) {\n"
        "    if (comment == 0) { return; }\n"
        "    _dom_characterdata_finalise((struct dom_characterdata *)comment);\n"
        "    free(comment);\n"
        "}\n"
        "\n"
        "export void __dom_comment_destroy(struct dom_node_internal *node) { _dom_comment_destroy((struct dom_comment *)node); }\n"
        "\n"
        "export int _dom_comment_copy(struct dom_node_internal *old_node, struct dom_node_internal **copy) {\n"
        "    struct dom_comment *new_comment;\n"
        "    int err;\n"
        "    if (copy == 0) { return DOM_NO_MEM_ERR; }\n"
        "    new_comment = (struct dom_comment *)malloc(DOM_COMMENT_SIZE);\n"
        "    if (new_comment == 0) { *copy = (struct dom_node_internal *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_comment_zero((uint8_t *)new_comment, DOM_COMMENT_SIZE);\n"
        "    err = _dom_characterdata_copy_internal((struct dom_characterdata *)old_node, (struct dom_characterdata *)new_comment);\n"
        "    if (err != DOM_NO_ERR) { free(new_comment); *copy = (struct dom_node_internal *)0; return err; }\n"
        "    *copy = (struct dom_node_internal *)new_comment;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_libdom_doc_fragment_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(void *ptr);\n"
        "extern int _dom_node_initialise(struct dom_node_internal *node, struct dom_document *doc, int type, struct dom_string *name, struct dom_string *value, struct dom_string *ns_uri, struct dom_string *prefix);\n"
        "extern void _dom_node_finalise(struct dom_node_internal *node);\n"
        "extern int _dom_node_copy_internal(struct dom_node_internal *old_node, struct dom_node_internal *new_node);\n"
        "\n"
        "struct dom_document { uint32_t opaque; };\n"
        "struct dom_string { uint32_t opaque; };\n"
        "struct dom_node_internal { uint8_t bytes[144]; };\n"
        "struct dom_document_fragment { uint8_t bytes[144]; };\n"
        "\n"
        "enum { DOM_NO_ERR = 0, DOM_NO_MEM_ERR = 131072, DOM_DOCUMENT_FRAGMENT_NODE = 11, DOM_DOCUMENT_FRAGMENT_SIZE = 144 };\n"
        "global uint8_t df_vtable[384];\n"
        "global uint8_t df_protect_vtable[16];\n"
        "\n"
        "void dom_doc_fragment_zero(uint8_t *ptr, uint64_t size) {\n"
        "    uint64_t i;\n"
        "    if (ptr == 0) { return; }\n"
        "    i = 0;\n"
        "    while (i < size) { ptr[i] = 0; i = i + 1; }\n"
        "}\n"
        "\n"
        "export int _dom_document_fragment_create(struct dom_document *doc, struct dom_string *name, struct dom_string *value, struct dom_document_fragment **result) {\n"
        "    struct dom_document_fragment *frag;\n"
        "    int err;\n"
        "    if (result == 0) { return DOM_NO_MEM_ERR; }\n"
        "    frag = (struct dom_document_fragment *)malloc(DOM_DOCUMENT_FRAGMENT_SIZE);\n"
        "    if (frag == 0) { *result = (struct dom_document_fragment *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_doc_fragment_zero((uint8_t *)frag, DOM_DOCUMENT_FRAGMENT_SIZE);\n"
        "    err = _dom_node_initialise((struct dom_node_internal *)frag, doc, DOM_DOCUMENT_FRAGMENT_NODE, name, value, (struct dom_string *)0, (struct dom_string *)0);\n"
        "    if (err != DOM_NO_ERR) { free(frag); *result = (struct dom_document_fragment *)0; return err; }\n"
        "    *result = frag;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
        "\n"
        "export void _dom_document_fragment_destroy(struct dom_document_fragment *frag) {\n"
        "    if (frag == 0) { return; }\n"
        "    _dom_node_finalise((struct dom_node_internal *)frag);\n"
        "    free(frag);\n"
        "}\n"
        "\n"
        "export void _dom_df_destroy(struct dom_node_internal *node) { _dom_document_fragment_destroy((struct dom_document_fragment *)node); }\n"
        "\n"
        "export int _dom_df_copy(struct dom_node_internal *old_node, struct dom_node_internal **copy) {\n"
        "    struct dom_document_fragment *new_frag;\n"
        "    int err;\n"
        "    if (copy == 0) { return DOM_NO_MEM_ERR; }\n"
        "    new_frag = (struct dom_document_fragment *)malloc(DOM_DOCUMENT_FRAGMENT_SIZE);\n"
        "    if (new_frag == 0) { *copy = (struct dom_node_internal *)0; return DOM_NO_MEM_ERR; }\n"
        "    dom_doc_fragment_zero((uint8_t *)new_frag, DOM_DOCUMENT_FRAGMENT_SIZE);\n"
        "    err = _dom_node_copy_internal(old_node, (struct dom_node_internal *)new_frag);\n"
        "    if (err != DOM_NO_ERR) { free(new_frag); *copy = (struct dom_node_internal *)0; return err; }\n"
        "    *copy = (struct dom_node_internal *)new_frag;\n"
        "    return DOM_NO_ERR;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_search_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern int content_textsearch(struct hlcache_handle_s *h, uint8_t *context, uint32_t flags, const uint8_t *string);\n"
        "extern int content_textsearch_clear(struct hlcache_handle_s *h);\n"
        "\n"
        "struct hlcache_handle_s { uint32_t opaque; };\n"
        "struct browser_window_s {\n"
        "    struct hlcache_handle_s *current_content;\n"
        "};\n"
        "\n"
        "export void browser_window_search(struct browser_window_s *bw, uint8_t *context, uint32_t flags, const uint8_t *string) {\n"
        "    if (bw != 0 && bw->current_content != 0) {\n"
        "        content_textsearch(bw->current_content, context, flags, string);\n"
        "    }\n"
        "}\n"
        "\n"
        "export void browser_window_search_clear(struct browser_window_s *bw) {\n"
        "    if (bw != 0 && bw->current_content != 0) {\n"
        "        content_textsearch_clear(bw->current_content);\n"
        "    }\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_bitmap_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct bitmap_fmt_s {\n"
        "    uint32_t layout;\n"
        "    uint8_t pma;\n"
        "};\n"
        "struct bitmap_colour_layout_s {\n"
        "    uint8_t r;\n"
        "    uint8_t g;\n"
        "    uint8_t b;\n"
        "    uint8_t a;\n"
        "};\n"
        "struct hlcache_handle_s { uint32_t opaque; };\n"
        "struct gui_bitmap_table_s {\n"
        "    uint8_t *(*create)(int width, int height, uint32_t flags);\n"
        "    void (*destroy)(uint8_t *bitmap);\n"
        "    void (*set_opaque)(uint8_t *bitmap, uint8_t opaque);\n"
        "    uint8_t (*get_opaque)(uint8_t *bitmap);\n"
        "    uint8_t *(*get_buffer)(uint8_t *bitmap);\n"
        "    uint64_t (*get_rowstride)(uint8_t *bitmap);\n"
        "    int (*get_width)(uint8_t *bitmap);\n"
        "    int (*get_height)(uint8_t *bitmap);\n"
        "    void (*modified)(uint8_t *bitmap);\n"
        "    int (*render)(uint8_t *bitmap, struct hlcache_handle_s *content);\n"
        "};\n"
        "struct netsurf_table_s {\n"
        "    uint8_t *misc;\n"
        "    uint8_t *window;\n"
        "    uint8_t *corewindow;\n"
        "    uint8_t *download;\n"
        "    uint8_t *clipboard;\n"
        "    uint8_t *fetch;\n"
        "    uint8_t *file;\n"
        "    uint8_t *utf8;\n"
        "    uint8_t *search;\n"
        "    uint8_t *search_web;\n"
        "    uint8_t *llcache;\n"
        "    struct gui_bitmap_table_s *bitmap;\n"
        "    uint8_t *layout;\n"
        "};\n"
        "extern struct netsurf_table_s *guit;\n"
        "\n"
        "enum { BITMAP_LAYOUT_R8G8B8A8 = 0, BITMAP_LAYOUT_B8G8R8A8 = 1, BITMAP_LAYOUT_A8R8G8B8 = 2, BITMAP_LAYOUT_A8B8G8R8 = 3, BITMAP_LAYOUT_RGBA8888 = 4, BITMAP_LAYOUT_BGRA8888 = 5, BITMAP_LAYOUT_ARGB8888 = 6, BITMAP_LAYOUT_ABGR8888 = 7 };\n"
        "global struct bitmap_fmt_s bitmap_fmt;\n"
        "global struct bitmap_colour_layout_s bitmap_layout = { 0, 1, 2, 3 };\n"
        "\n"
        "uint32_t bitmap_sanitise_bitmap_layout_z(uint32_t layout) {\n"
        "    if (layout == BITMAP_LAYOUT_RGBA8888) { return BITMAP_LAYOUT_A8B8G8R8; }\n"
        "    if (layout == BITMAP_LAYOUT_BGRA8888) { return BITMAP_LAYOUT_A8R8G8B8; }\n"
        "    if (layout == BITMAP_LAYOUT_ARGB8888) { return BITMAP_LAYOUT_B8G8R8A8; }\n"
        "    if (layout == BITMAP_LAYOUT_ABGR8888) { return BITMAP_LAYOUT_R8G8B8A8; }\n"
        "    return layout;\n"
        "}\n"
        "\n"
        "void bitmap_get_colour_layout_z(struct bitmap_colour_layout_s *out, uint32_t layout) {\n"
        "    if (layout == BITMAP_LAYOUT_B8G8R8A8) { out->b = 0; out->g = 1; out->r = 2; out->a = 3; return; }\n"
        "    if (layout == BITMAP_LAYOUT_A8R8G8B8) { out->a = 0; out->r = 1; out->g = 2; out->b = 3; return; }\n"
        "    if (layout == BITMAP_LAYOUT_A8B8G8R8) { out->a = 0; out->b = 1; out->g = 2; out->r = 3; return; }\n"
        "    out->r = 0; out->g = 1; out->b = 2; out->a = 3;\n"
        "}\n"
        "\n"
        "export void bitmap_set_format(struct bitmap_fmt_s *bitmap_format) {\n"
        "    if (bitmap_format == 0) { return; }\n"
        "    bitmap_fmt.layout = bitmap_sanitise_bitmap_layout_z(bitmap_format->layout);\n"
        "    bitmap_fmt.pma = bitmap_format->pma;\n"
        "    bitmap_get_colour_layout_z(&bitmap_layout, bitmap_fmt.layout);\n"
        "}\n"
        "\n"
        "void bitmap_convert_pixel(uint8_t *row, struct bitmap_colour_layout_s *to, struct bitmap_colour_layout_s *from, uint8_t pma_to, uint8_t pma_from) {\n"
        "    uint32_t r;\n"
        "    uint32_t g;\n"
        "    uint32_t b;\n"
        "    uint32_t a;\n"
        "    r = row[from->r];\n"
        "    g = row[from->g];\n"
        "    b = row[from->b];\n"
        "    a = row[from->a];\n"
        "    if (pma_to != pma_from) {\n"
        "        if (pma_to != 0) {\n"
        "            if (a != 0) { r = ((r * (a + 1)) >> 8) & 255; g = ((g * (a + 1)) >> 8) & 255; b = ((b * (a + 1)) >> 8) & 255; }\n"
        "            else { r = 0; g = 0; b = 0; }\n"
        "        } else {\n"
        "            if (a != 0) { r = (r << 8) / a; g = (g << 8) / a; b = (b << 8) / a; if (r > 255) { r = 255; } if (g > 255) { g = 255; } if (b > 255) { b = 255; } }\n"
        "            else { r = 0; g = 0; b = 0; }\n"
        "        }\n"
        "    }\n"
        "    row[to->r] = r;\n"
        "    row[to->g] = g;\n"
        "    row[to->b] = b;\n"
        "    row[to->a] = a;\n"
        "}\n"
        "\n"
        "export void bitmap_format_convert(uint8_t *bitmap, struct bitmap_fmt_s *fmt_from, struct bitmap_fmt_s *fmt_to) {\n"
        "    int width;\n"
        "    int height;\n"
        "    int x;\n"
        "    int y;\n"
        "    uint8_t opaque;\n"
        "    uint8_t *buffer;\n"
        "    uint8_t *row;\n"
        "    uint64_t rowstride;\n"
        "    struct bitmap_colour_layout_s to;\n"
        "    struct bitmap_colour_layout_s from;\n"
        "    if (bitmap == 0 || fmt_from == 0 || fmt_to == 0 || guit == 0 || guit->bitmap == 0) { return; }\n"
        "    if (guit->bitmap->get_width == 0 || guit->bitmap->get_height == 0 || guit->bitmap->get_buffer == 0 || guit->bitmap->get_rowstride == 0 || guit->bitmap->get_opaque == 0) { return; }\n"
        "    width = guit->bitmap->get_width(bitmap);\n"
        "    height = guit->bitmap->get_height(bitmap);\n"
        "    opaque = guit->bitmap->get_opaque(bitmap);\n"
        "    buffer = guit->bitmap->get_buffer(bitmap);\n"
        "    rowstride = guit->bitmap->get_rowstride(bitmap);\n"
        "    if (buffer == 0 || width <= 0 || height <= 0 || rowstride == 0) { return; }\n"
        "    bitmap_get_colour_layout_z(&to, bitmap_sanitise_bitmap_layout_z(fmt_to->layout));\n"
        "    bitmap_get_colour_layout_z(&from, bitmap_sanitise_bitmap_layout_z(fmt_from->layout));\n"
        "    if (fmt_from->pma != fmt_to->pma && opaque != 0) { return; }\n"
        "    y = 0;\n"
        "    while (y < height) {\n"
        "        row = buffer;\n"
        "        x = 0;\n"
        "        while (x < width) { bitmap_convert_pixel(row, &to, &from, fmt_to->pma, fmt_from->pma); row = row + 4; x = x + 1; }\n"
        "        buffer = buffer + rowstride;\n"
        "        y = y + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "export uint8_t bitmap_test_opaque(uint8_t *bitmap) {\n"
        "    int width;\n"
        "    int height;\n"
        "    int x;\n"
        "    int y;\n"
        "    uint8_t *buffer;\n"
        "    uint8_t *row;\n"
        "    uint64_t rowstride;\n"
        "    if (bitmap == 0 || guit == 0 || guit->bitmap == 0) { return 0; }\n"
        "    if (guit->bitmap->get_width == 0 || guit->bitmap->get_height == 0 || guit->bitmap->get_buffer == 0 || guit->bitmap->get_rowstride == 0) { return 0; }\n"
        "    width = guit->bitmap->get_width(bitmap);\n"
        "    height = guit->bitmap->get_height(bitmap);\n"
        "    buffer = guit->bitmap->get_buffer(bitmap);\n"
        "    rowstride = guit->bitmap->get_rowstride(bitmap);\n"
        "    if (buffer == 0 || width <= 0 || height <= 0 || rowstride == 0) { return 0; }\n"
        "    y = 0;\n"
        "    while (y < height) {\n"
        "        row = buffer;\n"
        "        x = bitmap_layout.a;\n"
        "        while (x < width * 4) { if (row[x] != 255) { return 0; } x = x + 4; }\n"
        "        buffer = buffer + rowstride;\n"
        "        y = y + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_libdom_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct dom_string_s { uint32_t refcnt; };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct dom_nodelist_s { uint32_t opaque; };\n"
        "struct dom_document_s { uint32_t opaque; };\n"
        "struct dom_node_s { struct dom_node_vtable_s *vtable; uint32_t refcnt; };\n"
        "struct dom_node_vtable_s {\n"
        "    int (*add_event_listener)(uint8_t *et, struct dom_string_s *type, uint8_t *listener, uint8_t capture);\n"
        "    int (*remove_event_listener)(uint8_t *et, struct dom_string_s *type, uint8_t *listener, uint8_t capture);\n"
        "    int (*dispatch_event)(uint8_t *et, uint8_t *evt, uint8_t *success);\n"
        "    int (*add_event_listener_ns)(uint8_t *et, struct dom_string_s *ns, struct dom_string_s *type, uint8_t *listener, uint8_t capture);\n"
        "    int (*remove_event_listener_ns)(uint8_t *et, struct dom_string_s *ns, struct dom_string_s *type, uint8_t *listener, uint8_t capture);\n"
        "    int (*dom_node_try_destroy)(struct dom_node_s *node);\n"
        "    int (*dom_node_get_node_name)(struct dom_node_s *node, struct dom_string_s **result);\n"
        "    int (*dom_node_get_node_value)(struct dom_node_s *node, struct dom_string_s **result);\n"
        "    int (*dom_node_set_node_value)(struct dom_node_s *node, struct dom_string_s *value);\n"
        "    int (*dom_node_get_node_type)(struct dom_node_s *node, uint32_t *result);\n"
        "    int (*dom_node_get_parent_node)(struct dom_node_s *node, struct dom_node_s **result);\n"
        "    int (*dom_node_get_child_nodes)(struct dom_node_s *node, struct dom_nodelist_s **result);\n"
        "    int (*dom_node_get_first_child)(struct dom_node_s *node, struct dom_node_s **result);\n"
        "    int (*dom_node_get_last_child)(struct dom_node_s *node, struct dom_node_s **result);\n"
        "    int (*dom_node_get_previous_sibling)(struct dom_node_s *node, struct dom_node_s **result);\n"
        "    int (*dom_node_get_next_sibling)(struct dom_node_s *node, struct dom_node_s **result);\n"
        "};\n"
        "\n"
        "extern void dom_string_destroy(struct dom_string_s *str);\n"
        "extern uint8_t dom_string_caseless_lwc_isequal(struct dom_string_s *s1, struct lwc_string_s *s2);\n"
        "extern void dom_nodelist_unref(struct dom_nodelist_s *list);\n"
        "extern int dom_nodelist_get_length(struct dom_nodelist_s *list, uint32_t *length);\n"
        "extern int _dom_nodelist_item(struct dom_nodelist_s *list, uint32_t index, struct dom_node_s **node);\n"
        "extern int _dom_node_get_node_name(struct dom_node_s *node, struct dom_string_s **result);\n"
        "extern int _dom_node_get_node_type(struct dom_node_s *node, uint32_t *result);\n"
        "extern int _dom_node_get_child_nodes(struct dom_node_s *node, struct dom_nodelist_s **result);\n"
        "extern int _dom_node_get_first_child(struct dom_node_s *node, struct dom_node_s **result);\n"
        "extern int _dom_node_get_next_sibling(struct dom_node_s *node, struct dom_node_s **result);\n"
        "\n"
        "enum { DOM_NO_ERR = 0, DOM_ELEMENT_NODE = 1 };\n"
        "enum { NSERROR_OK = 0, NSERROR_UNKNOWN = 1, NSERROR_NOMEM = 2, NSERROR_NOT_FOUND = 4, NSERROR_BAD_ENCODING = 15, NSERROR_NEED_DATA = 16, NSERROR_ENCODING_CHANGE = 17, NSERROR_BAD_PARAMETER = 18, NSERROR_INVALID = 19, NSERROR_DOM = 22, NSERROR_NOT_IMPLEMENTED = 31 };\n"
        "enum { HUBBUB_ENCODINGCHANGE = 2, HUBBUB_PAUSED = 3, HUBBUB_NOMEM = 5, HUBBUB_BADPARM = 6, HUBBUB_INVALID = 7, HUBBUB_FILENOTFOUND = 8, HUBBUB_NEEDDATA = 9, HUBBUB_BADENCODING = 10, HUBBUB_UNKNOWN = 11, DOM_HUBBUB_OK = 0, DOM_HUBBUB_NOMEM = 1, DOM_HUBBUB_BADPARM = 2, DOM_HUBBUB_DOM = 3, DOM_HUBBUB_HUBBUB_ERR = 65536 };\n"
        "\n"
        "void libdom_dom_string_unref_z(struct dom_string_s *str) {\n"
        "    if (str != 0) {\n"
        "        if (str->refcnt != 0) { str->refcnt = str->refcnt - 1; }\n"
        "        if (str->refcnt == 0) { dom_string_destroy(str); }\n"
        "    }\n"
        "}\n"
        "\n"
        "void libdom_dom_node_unref_z(struct dom_node_s *node) {\n"
        "    if (node != 0) {\n"
        "        if (node->refcnt != 0) { node->refcnt = node->refcnt - 1; }\n"
        "    }\n"
        "}\n"
        "\n"
        "export struct dom_node_s *libdom_find_first_element(struct dom_node_s *parent, struct lwc_string_s *element_name) {\n"
        "    struct dom_node_s *element;\n"
        "    struct dom_node_s *next_node;\n"
        "    struct dom_string_s *node_name;\n"
        "    uint32_t node_type;\n"
        "    int exc;\n"
        "    if (parent == 0) { return 0; }\n"
        "    exc = _dom_node_get_first_child(parent, &element);\n"
        "    if (exc != DOM_NO_ERR) { return 0; }\n"
        "    if (element == 0) { return 0; }\n"
        "    while (element != 0) {\n"
        "        node_type = 0;\n"
        "        exc = _dom_node_get_node_type(element, &node_type);\n"
        "            if (exc == DOM_NO_ERR) {\n"
        "                if (node_type == DOM_ELEMENT_NODE) {\n"
        "                    exc = _dom_node_get_node_name(element, &node_name);\n"
        "                    if (exc == DOM_NO_ERR) {\n"
        "                        if (node_name != 0) {\n"
        "                            if (dom_string_caseless_lwc_isequal(node_name, element_name) != 0) {\n"
        "                                libdom_dom_string_unref_z(node_name);\n"
        "                                return element;\n"
        "                            }\n"
        "                            libdom_dom_string_unref_z(node_name);\n"
        "                        }\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        exc = _dom_node_get_next_sibling(element, &next_node);\n"
        "        libdom_dom_node_unref_z(element);\n"
        "        if (exc != DOM_NO_ERR) { return 0; }\n"
        "        element = next_node;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export int libdom_iterate_child_elements(struct dom_node_s *parent, int (*cb)(struct dom_node_s *node, uint8_t *ctx), uint8_t *ctx) {\n"
        "    struct dom_nodelist_s *children;\n"
        "    struct dom_node_s *child;\n"
        "    uint32_t index;\n"
        "    uint32_t num_children;\n"
        "    uint32_t type;\n"
        "    int error;\n"
        "    int err;\n"
        "    if (parent == 0) { return NSERROR_NOMEM; }\n"
        "    if (cb == 0) { return NSERROR_NOMEM; }\n"
        "    error = _dom_node_get_child_nodes(parent, &children);\n"
        "    if (error != DOM_NO_ERR) { return NSERROR_NOMEM; }\n"
        "    if (children == 0) { return NSERROR_NOMEM; }\n"
        "    num_children = 0;\n"
        "    error = dom_nodelist_get_length(children, &num_children);\n"
        "    if (error != DOM_NO_ERR) { dom_nodelist_unref(children); return NSERROR_NOMEM; }\n"
        "    index = 0;\n"
        "    while (index < num_children) {\n"
        "        error = _dom_nodelist_item(children, index, &child);\n"
        "        if (error != DOM_NO_ERR) { dom_nodelist_unref(children); return NSERROR_NOMEM; }\n"
        "        type = 0;\n"
        "        if (child != 0) {\n"
        "                error = _dom_node_get_node_type(child, &type);\n"
        "                if (error == DOM_NO_ERR) {\n"
        "                    if (type == DOM_ELEMENT_NODE) {\n"
        "                        err = cb(child, ctx);\n"
        "                        if (err != NSERROR_OK) { libdom_dom_node_unref_z(child); dom_nodelist_unref(children); return err; }\n"
        "                    }\n"
        "                }\n"
        "        }\n"
        "        libdom_dom_node_unref_z(child);\n"
        "        index = index + 1;\n"
        "    }\n"
        "    dom_nodelist_unref(children);\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int libdom_hubbub_error_to_nserror(int error) {\n"
        "    if (error == DOM_HUBBUB_OK) { return NSERROR_OK; }\n"
        "    if (error == 65539) { return NSERROR_OK; }\n"
        "    if (error == DOM_HUBBUB_NOMEM) { return NSERROR_NOMEM; }\n"
        "    if (error == DOM_HUBBUB_BADPARM) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (error == DOM_HUBBUB_DOM) { return NSERROR_DOM; }\n"
        "    if (error == 65538) { return NSERROR_ENCODING_CHANGE; }\n"
        "    if (error == 65541) { return NSERROR_NOMEM; }\n"
        "    if (error == 65542) { return NSERROR_BAD_PARAMETER; }\n"
        "    if (error == 65543) { return NSERROR_INVALID; }\n"
        "    if (error == 65544) { return NSERROR_NOT_FOUND; }\n"
        "    if (error == 65545) { return NSERROR_NEED_DATA; }\n"
        "    if (error == 65546) { return NSERROR_BAD_ENCODING; }\n"
        "    if (error == 65547) { return NSERROR_DOM; }\n"
        "    return NSERROR_UNKNOWN;\n"
        "}\n"
        "\n"
        "export int libdom_dump_structure(struct dom_node_s *node, uint8_t *f, int depth) { return NSERROR_OK; }\n"
        "\n"
        "export int libdom_parse_file(uint8_t *filename, uint8_t *encoding, struct dom_document_s **doc) {\n"
        "    if (doc != 0) { *doc = 0; }\n"
        "    if (filename == 0 || doc == 0) { return NSERROR_BAD_PARAMETER; }\n"
        "    return NSERROR_NOT_FOUND;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_nscolour_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "\n"
        "enum {\n"
        "    NSERROR_OK = 0,\n"
        "    NSERROR_NOMEM = 2,\n"
        "    NSERROR_BAD_PARAMETER = 18\n"
        "};\n"
        "\n"
        "static uint8_t *nscolour_stylesheet;\n"
        "\n"
        "uint64_t nscolour_z_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "uint8_t *nscolour_z_strdup(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint8_t *copy;\n"
        "    len = nscolour_z_strlen(s);\n"
        "    copy = malloc(len + 1);\n"
        "    if (copy == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        copy[i] = s[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    copy[len] = 0;\n"
        "    return copy;\n"
        "}\n"
        "\n"
        "export int nscolour_update(void) {\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int nscolour_get_stylesheet(uint8_t **stylesheet_out) {\n"
        "    uint8_t css[142] = \".ns-odd-bg { background-color: #f0f0f0; }\\n.ns-odd-fg { color: #101010; }\\n.ns-even-bg { background-color: #ffffff; }\\n.ns-even-fg { color: #000000; }\\n\";\n"
        "    if (stylesheet_out == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (nscolour_stylesheet == 0) {\n"
        "        nscolour_stylesheet = nscolour_z_strdup(css);\n"
        "        if (nscolour_stylesheet == 0) {\n"
        "            return NSERROR_NOMEM;\n"
        "        }\n"
        "    }\n"
        "    *stylesheet_out = nscolour_stylesheet;\n"
        "    return NSERROR_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_utf8_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern int parserutils_charset_utf8_to_ucs4(const uint8_t *s, uint64_t len, uint32_t *ucs4, uint64_t *clen);\n"
        "extern int parserutils_charset_utf8_from_ucs4(uint32_t ucs4, uint8_t **s, uint64_t *len);\n"
        "extern int parserutils_charset_utf8_length(const uint8_t *s, uint64_t max, uint64_t *len);\n"
        "extern int parserutils_charset_utf8_char_byte_length(const uint8_t *s, uint64_t *len);\n"
        "extern int parserutils_charset_utf8_prev(const uint8_t *s, uint32_t off, uint32_t *prevoff);\n"
        "extern int parserutils_charset_utf8_next(const uint8_t *s, uint32_t len, uint32_t off, uint32_t *nextoff);\n"
        "extern uint8_t *malloc(uint64_t size);\n"
        "\n"
        "enum {\n"
        "    NSERROR_OK = 0,\n"
        "    NSERROR_NOMEM = 2,\n"
        "    NSERROR_BAD_PARAMETER = 18\n"
        "};\n"
        "\n"
        "uint64_t nsutf8_z_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export uint32_t utf8_to_ucs4(const uint8_t *s, uint64_t l) {\n"
        "    uint32_t ucs4;\n"
        "    uint64_t clen;\n"
        "    ucs4 = 0;\n"
        "    clen = 0;\n"
        "    if (parserutils_charset_utf8_to_ucs4(s, l, &ucs4, &clen) != 0) {\n"
        "        return 65533;\n"
        "    }\n"
        "    return ucs4;\n"
        "}\n"
        "\n"
        "export uint64_t utf8_from_ucs4(uint32_t c, uint8_t *s) {\n"
        "    uint8_t *out;\n"
        "    uint64_t len;\n"
        "    out = s;\n"
        "    len = 6;\n"
        "    if (parserutils_charset_utf8_from_ucs4(c, &out, &len) != 0) {\n"
        "        s[0] = 239;\n"
        "        s[1] = 191;\n"
        "        s[2] = 189;\n"
        "        return 3;\n"
        "    }\n"
        "    return 6 - len;\n"
        "}\n"
        "\n"
        "export uint64_t utf8_bounded_length(const uint8_t *s, uint64_t l) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (parserutils_charset_utf8_length(s, l, &len) != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export uint64_t utf8_length(const uint8_t *s) {\n"
        "    return utf8_bounded_length(s, nsutf8_z_strlen((uint8_t *)s));\n"
        "}\n"
        "\n"
        "export uint64_t utf8_char_byte_length(const uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (parserutils_charset_utf8_char_byte_length(s, &len) != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export uint64_t utf8_prev(const uint8_t *s, uint64_t o) {\n"
        "    uint32_t prev;\n"
        "    prev = 0;\n"
        "    if (parserutils_charset_utf8_prev(s, (uint32_t)o, &prev) != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return prev;\n"
        "}\n"
        "\n"
        "export uint64_t utf8_next(const uint8_t *s, uint64_t l, uint64_t o) {\n"
        "    uint32_t next;\n"
        "    next = 0;\n"
        "    if (parserutils_charset_utf8_next(s, (uint32_t)l, (uint32_t)o, &next) != 0) {\n"
        "        return o;\n"
        "    }\n"
        "    return next;\n"
        "}\n"
        "\n"
        "export uint64_t utf8_bounded_byte_length(const uint8_t *s, uint64_t l, uint64_t c) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    while (len < l && c > 0) {\n"
        "        len = utf8_next(s, l, len);\n"
        "        c = c - 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export int utf8_to_enc(const uint8_t *string, const uint8_t *encname, uint64_t len, uint8_t **result) {\n"
        "    uint64_t i;\n"
        "    uint8_t *copy;\n"
        "    if (string == 0 || result == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (len == 0) {\n"
        "        len = nsutf8_z_strlen((uint8_t *)string);\n"
        "    }\n"
        "    copy = malloc(len + 1);\n"
        "    if (copy == 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        copy[i] = string[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    copy[len] = 0;\n"
        "    *result = copy;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int utf8_from_enc(const uint8_t *string, const uint8_t *encname, uint64_t len, uint8_t **result, uint64_t *result_len) {\n"
        "    int ret;\n"
        "    ret = utf8_to_enc(string, encname, len, result);\n"
        "    if (ret == NSERROR_OK && result_len != 0) {\n"
        "        if (len == 0) {\n"
        "            len = nsutf8_z_strlen((uint8_t *)string);\n"
        "        }\n"
        "        *result_len = len;\n"
        "    }\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export int utf8_finalise(void) {\n"
        "    return NSERROR_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_punycode_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "enum {\n"
        "    PUNYCODE_SUCCESS = 0,\n"
        "    PUNYCODE_BAD_INPUT = 1,\n"
        "    PUNYCODE_BIG_OUTPUT = 2,\n"
        "    PUNYCODE_OVERFLOW = 3,\n"
        "    PUNY_BASE = 36,\n"
        "    PUNY_TMIN = 1,\n"
        "    PUNY_TMAX = 26,\n"
        "    PUNY_SKEW = 38,\n"
        "    PUNY_DAMP = 700,\n"
        "    PUNY_INITIAL_BIAS = 72,\n"
        "    PUNY_INITIAL_N = 128,\n"
        "    PUNY_DELIMITER = 45,\n"
        "    PUNY_MAXINT = 4294967295\n"
        "};\n"
        "\n"
        "uint8_t puny_basic(uint32_t cp) {\n"
        "    if (cp < 128) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint8_t puny_flagged(uint32_t cp) {\n"
        "    if (cp - 65 < 26) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint32_t decode_digit(uint32_t cp) {\n"
        "    if (cp - 48 < 10) {\n"
        "        return cp - 22;\n"
        "    }\n"
        "    if (cp - 65 < 26) {\n"
        "        return cp - 65;\n"
        "    }\n"
        "    if (cp - 97 < 26) {\n"
        "        return cp - 97;\n"
        "    }\n"
        "    return PUNY_BASE;\n"
        "}\n"
        "\n"
        "uint8_t encode_digit(uint32_t d, uint8_t flag) {\n"
        "    uint32_t cp;\n"
        "    cp = d + 22;\n"
        "    if (d < 26) {\n"
        "        cp = cp + 75;\n"
        "    }\n"
        "    if (flag != 0) {\n"
        "        cp = cp - 32;\n"
        "    }\n"
        "    return (uint8_t)cp;\n"
        "}\n"
        "\n"
        "uint8_t encode_basic(uint32_t bcp, uint8_t flag) {\n"
        "    if (bcp - 97 < 26) {\n"
        "        bcp = bcp - 32;\n"
        "    }\n"
        "    if (flag == 0 && bcp - 65 < 26) {\n"
        "        bcp = bcp + 32;\n"
        "    }\n"
        "    return (uint8_t)bcp;\n"
        "}\n"
        "\n"
        "uint32_t adapt(uint32_t delta, uint32_t numpoints, int firsttime) {\n"
        "    uint32_t k;\n"
        "    if (firsttime != 0) {\n"
        "        delta = delta / PUNY_DAMP;\n"
        "    } else {\n"
        "        delta = delta / 2;\n"
        "    }\n"
        "    delta = delta + delta / numpoints;\n"
        "    k = 0;\n"
        "    while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) {\n"
        "        delta = delta / (PUNY_BASE - PUNY_TMIN);\n"
        "        k = k + PUNY_BASE;\n"
        "    }\n"
        "    return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);\n"
        "}\n"
        "\n"
        "export int punycode_encode(uint64_t input_length_orig, const uint32_t *input, const uint8_t *case_flags, uint64_t *output_length, uint8_t *output) {\n"
        "    uint32_t input_length;\n"
        "    uint32_t n;\n"
        "    uint32_t delta;\n"
        "    uint32_t h;\n"
        "    uint32_t b;\n"
        "    uint32_t bias;\n"
        "    uint32_t j;\n"
        "    uint32_t m;\n"
        "    uint32_t q;\n"
        "    uint32_t k;\n"
        "    uint32_t t;\n"
        "    int firsttime;\n"
        "    uint64_t out;\n"
        "    uint64_t max_out;\n"
        "    if (input_length_orig > PUNY_MAXINT) {\n"
        "        return PUNYCODE_OVERFLOW;\n"
        "    }\n"
        "    input_length = (uint32_t)input_length_orig;\n"
        "    n = PUNY_INITIAL_N;\n"
        "    delta = 0;\n"
        "    out = 0;\n"
        "    max_out = *output_length;\n"
        "    bias = PUNY_INITIAL_BIAS;\n"
        "    j = 0;\n"
        "    while (j < input_length) {\n"
        "        if (puny_basic(input[j]) != 0) {\n"
        "            if (max_out - out < 2) {\n"
        "                return PUNYCODE_BIG_OUTPUT;\n"
        "            }\n"
        "            if (case_flags != 0) {\n"
        "                output[out] = encode_basic(input[j], case_flags[j]);\n"
        "            } else {\n"
        "                output[out] = (uint8_t)input[j];\n"
        "            }\n"
        "            out = out + 1;\n"
        "        }\n"
        "        j = j + 1;\n"
        "    }\n"
        "    h = (uint32_t)out;\n"
        "    b = h;\n"
        "    if (b > 0) {\n"
        "        output[out] = PUNY_DELIMITER;\n"
        "        out = out + 1;\n"
        "    }\n"
        "    while (h < input_length) {\n"
        "        m = PUNY_MAXINT;\n"
        "        j = 0;\n"
        "        while (j < input_length) {\n"
        "            if (input[j] >= n && input[j] < m) {\n"
        "                m = input[j];\n"
        "            }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        if (m - n > (PUNY_MAXINT - delta) / (h + 1)) {\n"
        "            return PUNYCODE_OVERFLOW;\n"
        "        }\n"
        "        delta = delta + (m - n) * (h + 1);\n"
        "        n = m;\n"
        "        j = 0;\n"
        "        while (j < input_length) {\n"
        "            if (input[j] < n) {\n"
        "                delta = delta + 1;\n"
        "                if (delta == 0) {\n"
        "                    return PUNYCODE_OVERFLOW;\n"
        "                }\n"
        "            }\n"
        "            if (input[j] == n) {\n"
        "                q = delta;\n"
        "                k = PUNY_BASE;\n"
        "                while (1) {\n"
        "                    if (out >= max_out) {\n"
        "                        return PUNYCODE_BIG_OUTPUT;\n"
        "                    }\n"
        "                    if (k <= bias) {\n"
        "                        t = PUNY_TMIN;\n"
        "                    } else if (k >= bias + PUNY_TMAX) {\n"
        "                        t = PUNY_TMAX;\n"
        "                    } else {\n"
        "                        t = k - bias;\n"
        "                    }\n"
        "                    if (q < t) {\n"
        "                        break;\n"
        "                    }\n"
        "                    output[out] = encode_digit(t + (q - t) % (PUNY_BASE - t), 0);\n"
        "                    out = out + 1;\n"
        "                    q = (q - t) / (PUNY_BASE - t);\n"
        "                    k = k + PUNY_BASE;\n"
        "                }\n"
        "                if (case_flags != 0 && case_flags[j] != 0) {\n"
        "                    output[out] = encode_digit(q, 1);\n"
        "                } else {\n"
        "                    output[out] = encode_digit(q, 0);\n"
        "                }\n"
        "                out = out + 1;\n"
        "                if (h == b) {\n"
        "                    firsttime = 1;\n"
        "                } else {\n"
        "                    firsttime = 0;\n"
        "                }\n"
        "                bias = adapt(delta, h + 1, firsttime);\n"
        "                delta = 0;\n"
        "                h = h + 1;\n"
        "            }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        delta = delta + 1;\n"
        "        n = n + 1;\n"
        "    }\n"
        "    *output_length = out;\n"
        "    return PUNYCODE_SUCCESS;\n"
        "}\n"
        "\n"
        "export int punycode_decode(uint64_t input_length, const uint8_t *input, uint64_t *output_length, uint32_t *output, uint8_t *case_flags) {\n"
        "    uint32_t n;\n"
        "    uint32_t out;\n"
        "    uint32_t i;\n"
        "    uint32_t max_out;\n"
        "    uint32_t bias;\n"
        "    uint32_t oldi;\n"
        "    uint32_t w;\n"
        "    uint32_t k;\n"
        "    uint32_t digit;\n"
        "    uint32_t t;\n"
        "    int firsttime;\n"
        "    uint64_t b;\n"
        "    uint64_t j;\n"
        "    uint64_t in;\n"
        "    n = PUNY_INITIAL_N;\n"
        "    out = 0;\n"
        "    i = 0;\n"
        "    if (*output_length > PUNY_MAXINT) {\n"
        "        max_out = PUNY_MAXINT;\n"
        "    } else {\n"
        "        max_out = (uint32_t)*output_length;\n"
        "    }\n"
        "    bias = PUNY_INITIAL_BIAS;\n"
        "    b = 0;\n"
        "    j = 0;\n"
        "    while (j < input_length) {\n"
        "        if (input[j] == PUNY_DELIMITER) {\n"
        "            b = j;\n"
        "        }\n"
        "        j = j + 1;\n"
        "    }\n"
        "    if (b > max_out) {\n"
        "        return PUNYCODE_BIG_OUTPUT;\n"
        "    }\n"
        "    j = 0;\n"
        "    while (j < b) {\n"
        "        if (case_flags != 0) {\n"
        "            case_flags[out] = puny_flagged(input[j]);\n"
        "        }\n"
        "        if (puny_basic(input[j]) == 0) {\n"
        "            return PUNYCODE_BAD_INPUT;\n"
        "        }\n"
        "        output[out] = input[j];\n"
        "        out = out + 1;\n"
        "        j = j + 1;\n"
        "    }\n"
        "    if (b > 0) {\n"
        "        in = b + 1;\n"
        "    } else {\n"
        "        in = 0;\n"
        "    }\n"
        "    while (in < input_length) {\n"
        "        oldi = i;\n"
        "        w = 1;\n"
        "        k = PUNY_BASE;\n"
        "        while (1) {\n"
        "            if (in >= input_length) {\n"
        "                return PUNYCODE_BAD_INPUT;\n"
        "            }\n"
        "            digit = decode_digit(input[in]);\n"
        "            in = in + 1;\n"
        "            if (digit >= PUNY_BASE) {\n"
        "                return PUNYCODE_BAD_INPUT;\n"
        "            }\n"
        "            if (digit > (PUNY_MAXINT - i) / w) {\n"
        "                return PUNYCODE_OVERFLOW;\n"
        "            }\n"
        "            i = i + digit * w;\n"
        "            if (k <= bias) {\n"
        "                t = PUNY_TMIN;\n"
        "            } else if (k >= bias + PUNY_TMAX) {\n"
        "                t = PUNY_TMAX;\n"
        "            } else {\n"
        "                t = k - bias;\n"
        "            }\n"
        "            if (digit < t) {\n"
        "                break;\n"
        "            }\n"
        "            if (w > PUNY_MAXINT / (PUNY_BASE - t)) {\n"
        "                return PUNYCODE_OVERFLOW;\n"
        "            }\n"
        "            w = w * (PUNY_BASE - t);\n"
        "            k = k + PUNY_BASE;\n"
        "        }\n"
        "        if (oldi == 0) {\n"
        "            firsttime = 1;\n"
        "        } else {\n"
        "            firsttime = 0;\n"
        "        }\n"
        "        bias = adapt(i - oldi, out + 1, firsttime);\n"
        "        if (i / (out + 1) > PUNY_MAXINT - n) {\n"
        "            return PUNYCODE_OVERFLOW;\n"
        "        }\n"
        "        n = n + i / (out + 1);\n"
        "        i = i % (out + 1);\n"
        "        if (out >= max_out) {\n"
        "            return PUNYCODE_BIG_OUTPUT;\n"
        "        }\n"
        "        j = out;\n"
        "        while (j > i) {\n"
        "            output[j] = output[j - 1];\n"
        "            if (case_flags != 0) {\n"
        "                case_flags[j] = case_flags[j - 1];\n"
        "            }\n"
        "            j = j - 1;\n"
        "        }\n"
        "        output[i] = n;\n"
        "        if (case_flags != 0) {\n"
        "            case_flags[i] = puny_flagged(input[in - 1]);\n"
        "        }\n"
        "        i = i + 1;\n"
        "        out = out + 1;\n"
        "    }\n"
        "    *output_length = out;\n"
        "    return PUNYCODE_SUCCESS;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_hashtable_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "enum {\n"
        "    NSERROR_OK = 0,\n"
        "    NSERROR_NOMEM = 2,\n"
        "    NSERROR_NOT_FOUND = 4,\n"
        "    NSERROR_BAD_PARAMETER = 18,\n"
        "    NSERROR_INVALID = 19\n"
        "};\n"
        "\n"
        "struct hash_entry {\n"
        "    uint8_t *pairing;\n"
        "    uint32_t key_length;\n"
        "    struct hash_entry *next;\n"
        "};\n"
        "\n"
        "struct hash_table {\n"
        "    uint32_t nchains;\n"
        "    struct hash_entry **chain;\n"
        "};\n"
        "\n"
        "uint64_t nsht_strlen(const uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (s[len] != 0) {\n"
        "        len = len + 1;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "void nsht_memcpy(uint8_t *dst, const uint8_t *src, uint64_t len) {\n"
        "    uint64_t i;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        dst[i] = src[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "\n"
        "uint8_t nsht_memeq(const uint8_t *a, const uint8_t *b, uint64_t len) {\n"
        "    uint64_t i;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        if (a[i] != b[i]) {\n"
        "            return 0;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "uint32_t nsht_hash_string_fnv(const uint8_t *datum, uint32_t *len) {\n"
        "    uint32_t z;\n"
        "    uint32_t n;\n"
        "    z = 2166136261;\n"
        "    n = 0;\n"
        "    if (len != 0) {\n"
        "        *len = 0;\n"
        "    }\n"
        "    if (datum == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (datum[n] != 0) {\n"
        "        z = z * 16777619;\n"
        "        z = z ^ datum[n];\n"
        "        n = n + 1;\n"
        "    }\n"
        "    if (len != 0) {\n"
        "        *len = n;\n"
        "    }\n"
        "    return z;\n"
        "}\n"
        "\n"
        "export struct hash_table *hash_create(uint32_t chains) {\n"
        "    struct hash_table *r;\n"
        "    uint32_t i;\n"
        "    if (chains == 0) {\n"
        "        chains = 1;\n"
        "    }\n"
        "    r = (struct hash_table *)malloc(sizeof(struct hash_table));\n"
        "    if (r == 0) {\n"
        "        return (struct hash_table *)0;\n"
        "    }\n"
        "    r->nchains = chains;\n"
        "    r->chain = (struct hash_entry **)malloc(chains * sizeof(struct hash_entry *));\n"
        "    if (r->chain == 0) {\n"
        "        free((uint8_t *)r);\n"
        "        return (struct hash_table *)0;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < chains) {\n"
        "        *(r->chain + i) = (struct hash_entry *)0;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return r;\n"
        "}\n"
        "\n"
        "export void hash_destroy(struct hash_table *ht) {\n"
        "    uint32_t i;\n"
        "    struct hash_entry *e;\n"
        "    struct hash_entry *n;\n"
        "    if (ht == 0) {\n"
        "        return;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < ht->nchains) {\n"
        "        e = *(ht->chain + i);\n"
        "        while (e != 0) {\n"
        "            n = e->next;\n"
        "            free(e->pairing);\n"
        "            free((uint8_t *)e);\n"
        "            e = n;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    free((uint8_t *)ht->chain);\n"
        "    free((uint8_t *)ht);\n"
        "}\n"
        "\n"
        "export int hash_add(struct hash_table *ht, const uint8_t *key, const uint8_t *value) {\n"
        "    uint32_t h;\n"
        "    uint32_t c;\n"
        "    uint32_t key_length;\n"
        "    uint64_t value_length;\n"
        "    struct hash_entry *e;\n"
        "    if (ht == 0 || key == 0 || value == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    e = (struct hash_entry *)malloc(sizeof(struct hash_entry));\n"
        "    if (e == 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    h = nsht_hash_string_fnv(key, &key_length);\n"
        "    c = h % ht->nchains;\n"
        "    value_length = nsht_strlen(value);\n"
        "    e->pairing = malloc(value_length + key_length + 2);\n"
        "    if (e->pairing == 0) {\n"
        "        free((uint8_t *)e);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    nsht_memcpy(e->pairing, key, key_length + 1);\n"
        "    nsht_memcpy(e->pairing + key_length + 1, value, value_length + 1);\n"
        "    e->key_length = key_length;\n"
        "    e->next = *(ht->chain + c);\n"
        "    *(ht->chain + c) = e;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export const uint8_t *hash_get(struct hash_table *ht, const uint8_t *key) {\n"
        "    uint32_t h;\n"
        "    uint32_t c;\n"
        "    uint32_t key_length;\n"
        "    struct hash_entry *e;\n"
        "    if (ht == 0 || key == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    h = nsht_hash_string_fnv(key, &key_length);\n"
        "    c = h % ht->nchains;\n"
        "    e = *(ht->chain + c);\n"
        "    while (e != 0) {\n"
        "        if (key_length == e->key_length) {\n"
        "            if (nsht_memeq(key, e->pairing, key_length) != 0) {\n"
        "                return e->pairing + key_length + 1;\n"
        "            }\n"
        "        }\n"
        "        e = e->next;\n"
        "    }\n"
        "    return (uint8_t *)0;\n"
        "}\n"
        "\n"
        "int nsht_process_line(struct hash_table *ht, uint8_t *ln, uint32_t lnlen) {\n"
        "    uint32_t key;\n"
        "    uint32_t colon;\n"
        "    key = 0;\n"
        "    while (key < lnlen && (ln[key] == 32 || ln[key] == 9)) {\n"
        "        key = key + 1;\n"
        "    }\n"
        "    if (key >= lnlen || ln[key] == 0 || ln[key] == 35) {\n"
        "        return NSERROR_OK;\n"
        "    }\n"
        "    colon = key;\n"
        "    while (colon < lnlen && ln[colon] != 58) {\n"
        "        colon = colon + 1;\n"
        "    }\n"
        "    if (colon >= lnlen) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    ln[colon] = 0;\n"
        "    return hash_add(ht, ln + key, ln + colon + 1);\n"
        "}\n"
        "\n"
        "int nsht_hash_add_inline_plain(struct hash_table *ht, const uint8_t *data, uint64_t size) {\n"
        "    uint8_t line[512];\n"
        "    uint32_t line_len;\n"
        "    int res;\n"
        "    line_len = 0;\n"
        "    res = NSERROR_OK;\n"
        "    while (size > 0) {\n"
        "        if (*data == 10) {\n"
        "            line[line_len] = 0;\n"
        "            res = nsht_process_line(ht, line, line_len);\n"
        "            line_len = 0;\n"
        "            if (res != NSERROR_OK) {\n"
        "                return res;\n"
        "            }\n"
        "        } else if (line_len < 511) {\n"
        "            line[line_len] = *data;\n"
        "            line_len = line_len + 1;\n"
        "        } else {\n"
        "            line_len = 0;\n"
        "        }\n"
        "        data = data + 1;\n"
        "        size = size - 1;\n"
        "    }\n"
        "    if (line_len > 0) {\n"
        "        line[line_len] = 0;\n"
        "        res = nsht_process_line(ht, line, line_len);\n"
        "    }\n"
        "    return res;\n"
        "}\n"
        "\n"
        "export int hash_add_inline(struct hash_table *ht, const uint8_t *data, uint64_t size) {\n"
        "    if (ht == 0 || data == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (size >= 2 && data[0] == 31 && data[1] == 139) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    return nsht_hash_add_inline_plain(ht, data, size);\n"
        "}\n"
        "\n"
        "export int hash_add_file(struct hash_table *ht, const uint8_t *path) {\n"
        "    if (ht == 0 || path == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    return NSERROR_NOT_FOUND;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_hashmap_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct hashmap_parameters {\n"
        "    uint8_t *(*key_clone)(uint8_t *key);\n"
        "    uint32_t (*key_hash)(uint8_t *key);\n"
        "    uint8_t (*key_eq)(uint8_t *a, uint8_t *b);\n"
        "    void (*key_destroy)(uint8_t *key);\n"
        "    uint8_t *(*value_alloc)(uint8_t *key);\n"
        "    void (*value_destroy)(uint8_t *value);\n"
        "};\n"
        "\n"
        "struct hashmap_entry_s {\n"
        "    struct hashmap_entry_s **prevptr;\n"
        "    struct hashmap_entry_s *next;\n"
        "    uint8_t *key;\n"
        "    uint8_t *value;\n"
        "    uint32_t key_hash;\n"
        "};\n"
        "\n"
        "struct hashmap_s {\n"
        "    struct hashmap_parameters *params;\n"
        "    struct hashmap_entry_s **buckets;\n"
        "    uint32_t bucket_count;\n"
        "    uint64_t entry_count;\n"
        "};\n"
        "\n"
        "export struct hashmap_s *hashmap_create(struct hashmap_parameters *params) {\n"
        "    struct hashmap_s *ret;\n"
        "    uint32_t bucket;\n"
        "    if (params == 0) {\n"
        "        return (struct hashmap_s *)0;\n"
        "    }\n"
        "    ret = (struct hashmap_s *)malloc(sizeof(struct hashmap_s));\n"
        "    if (ret == 0) {\n"
        "        return (struct hashmap_s *)0;\n"
        "    }\n"
        "    ret->params = params;\n"
        "    ret->bucket_count = 4091;\n"
        "    ret->entry_count = 0;\n"
        "    ret->buckets = (struct hashmap_entry_s **)malloc(ret->bucket_count * sizeof(struct hashmap_entry_s *));\n"
        "    if (ret->buckets == 0) {\n"
        "        free((uint8_t *)ret);\n"
        "        return (struct hashmap_s *)0;\n"
        "    }\n"
        "    bucket = 0;\n"
        "    while (bucket < ret->bucket_count) {\n"
        "        *(ret->buckets + bucket) = (struct hashmap_entry_s *)0;\n"
        "        bucket = bucket + 1;\n"
        "    }\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export void hashmap_destroy(struct hashmap_s *hashmap) {\n"
        "    uint32_t bucket;\n"
        "    struct hashmap_entry_s *entry;\n"
        "    struct hashmap_entry_s *next;\n"
        "    if (hashmap == 0) {\n"
        "        return;\n"
        "    }\n"
        "    bucket = 0;\n"
        "    while (bucket < hashmap->bucket_count) {\n"
        "        entry = *(hashmap->buckets + bucket);\n"
        "        while (entry != 0) {\n"
        "            next = entry->next;\n"
        "            hashmap->params->value_destroy(entry->value);\n"
        "            hashmap->params->key_destroy(entry->key);\n"
        "            free((uint8_t *)entry);\n"
        "            entry = next;\n"
        "        }\n"
        "        bucket = bucket + 1;\n"
        "    }\n"
        "    free((uint8_t *)hashmap->buckets);\n"
        "    free((uint8_t *)hashmap);\n"
        "}\n"
        "\n"
        "export uint8_t *hashmap_lookup(struct hashmap_s *hashmap, uint8_t *key) {\n"
        "    uint32_t hash;\n"
        "    struct hashmap_entry_s *entry;\n"
        "    if (hashmap == 0 || key == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    hash = hashmap->params->key_hash(key);\n"
        "    entry = *(hashmap->buckets + (hash % hashmap->bucket_count));\n"
        "    while (entry != 0) {\n"
        "        if (entry->key_hash == hash) {\n"
        "            if (hashmap->params->key_eq(key, entry->key) != 0) {\n"
        "                return entry->value;\n"
        "            }\n"
        "        }\n"
        "        entry = entry->next;\n"
        "    }\n"
        "    return (uint8_t *)0;\n"
        "}\n"
        "\n"
        "export uint8_t *hashmap_insert(struct hashmap_s *hashmap, uint8_t *key) {\n"
        "    uint32_t hash;\n"
        "    uint32_t bucket;\n"
        "    struct hashmap_entry_s *entry;\n"
        "    uint8_t *new_key;\n"
        "    uint8_t *new_value;\n"
        "    if (hashmap == 0 || key == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    hash = hashmap->params->key_hash(key);\n"
        "    bucket = hash % hashmap->bucket_count;\n"
        "    entry = *(hashmap->buckets + bucket);\n"
        "    while (entry != 0) {\n"
        "        if (entry->key_hash == hash) {\n"
        "            if (hashmap->params->key_eq(key, entry->key) != 0) {\n"
        "                new_key = hashmap->params->key_clone(key);\n"
        "                if (new_key == 0) {\n"
        "                    return (uint8_t *)0;\n"
        "                }\n"
        "                new_value = hashmap->params->value_alloc(entry->key);\n"
        "                if (new_value == 0) {\n"
        "                    hashmap->params->key_destroy(new_key);\n"
        "                    return (uint8_t *)0;\n"
        "                }\n"
        "                hashmap->params->value_destroy(entry->value);\n"
        "                hashmap->params->key_destroy(entry->key);\n"
        "                entry->value = new_value;\n"
        "                entry->key = new_key;\n"
        "                return entry->value;\n"
        "            }\n"
        "        }\n"
        "        entry = entry->next;\n"
        "    }\n"
        "    entry = (struct hashmap_entry_s *)malloc(sizeof(struct hashmap_entry_s));\n"
        "    if (entry == 0) {\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    entry->prevptr = (struct hashmap_entry_s **)0;\n"
        "    entry->next = (struct hashmap_entry_s *)0;\n"
        "    entry->key = (uint8_t *)0;\n"
        "    entry->value = (uint8_t *)0;\n"
        "    entry->key_hash = hash;\n"
        "    entry->key = hashmap->params->key_clone(key);\n"
        "    if (entry->key == 0) {\n"
        "        free((uint8_t *)entry);\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    entry->value = hashmap->params->value_alloc(entry->key);\n"
        "    if (entry->value == 0) {\n"
        "        hashmap->params->key_destroy(entry->key);\n"
        "        free((uint8_t *)entry);\n"
        "        return (uint8_t *)0;\n"
        "    }\n"
        "    entry->prevptr = hashmap->buckets + bucket;\n"
        "    entry->next = *(hashmap->buckets + bucket);\n"
        "    if (entry->next != 0) {\n"
        "        entry->next->prevptr = &entry->next;\n"
        "    }\n"
        "    *(hashmap->buckets + bucket) = entry;\n"
        "    hashmap->entry_count = hashmap->entry_count + 1;\n"
        "    return entry->value;\n"
        "}\n"
        "\n"
        "export uint8_t hashmap_remove(struct hashmap_s *hashmap, uint8_t *key) {\n"
        "    uint32_t hash;\n"
        "    struct hashmap_entry_s *entry;\n"
        "    if (hashmap == 0 || key == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    hash = hashmap->params->key_hash(key);\n"
        "    entry = *(hashmap->buckets + (hash % hashmap->bucket_count));\n"
        "    while (entry != 0) {\n"
        "        if (entry->key_hash == hash) {\n"
        "            if (hashmap->params->key_eq(key, entry->key) != 0) {\n"
        "                hashmap->params->value_destroy(entry->value);\n"
        "                hashmap->params->key_destroy(entry->key);\n"
        "                if (entry->next != 0) {\n"
        "                    entry->next->prevptr = entry->prevptr;\n"
        "                }\n"
        "                *(entry->prevptr) = entry->next;\n"
        "                free((uint8_t *)entry);\n"
        "                hashmap->entry_count = hashmap->entry_count - 1;\n"
        "                return 1;\n"
        "            }\n"
        "        }\n"
        "        entry = entry->next;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export uint8_t hashmap_iterate(struct hashmap_s *hashmap, uint8_t (*cb)(uint8_t *key, uint8_t *value, uint8_t *ctx), uint8_t *ctx) {\n"
        "    uint32_t bucket;\n"
        "    struct hashmap_entry_s *entry;\n"
        "    if (hashmap == 0 || cb == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    bucket = 0;\n"
        "    while (bucket < hashmap->bucket_count) {\n"
        "        entry = *(hashmap->buckets + bucket);\n"
        "        while (entry != 0) {\n"
        "            if (cb(entry->key, entry->value, ctx) != 0) {\n"
        "                return 1;\n"
        "            }\n"
        "            entry = entry->next;\n"
        "        }\n"
        "        bucket = bucket + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export uint64_t hashmap_count(struct hashmap_s *hashmap) {\n"
        "    if (hashmap == 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return hashmap->entry_count;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_time_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "enum {\n"
        "    NSERROR_OK = 0,\n"
        "    NSERROR_BAD_PARAMETER = 18,\n"
        "    NSERROR_INVALID = 19\n"
        "};\n"
        "\n"
        "global uint8_t nstime_ret[31];\n"
        "\n"
        "uint8_t nstime_is_digit(uint8_t c) {\n"
        "    if (c >= 48 && c <= 57) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int nstime_parse2(const uint8_t *s) {\n"
        "    if (nstime_is_digit(s[0]) == 0 || nstime_is_digit(s[1]) == 0) {\n"
        "        return -1;\n"
        "    }\n"
        "    return (s[0] - 48) * 10 + (s[1] - 48);\n"
        "}\n"
        "\n"
        "int nstime_parse4(const uint8_t *s) {\n"
        "    if (nstime_is_digit(s[0]) == 0 || nstime_is_digit(s[1]) == 0 || nstime_is_digit(s[2]) == 0 || nstime_is_digit(s[3]) == 0) {\n"
        "        return -1;\n"
        "    }\n"
        "    return (s[0] - 48) * 1000 + (s[1] - 48) * 100 + (s[2] - 48) * 10 + (s[3] - 48);\n"
        "}\n"
        "\n"
        "uint8_t nstime_is_leap(int64_t y) {\n"
        "    int64_t r;\n"
        "    r = y % 4;\n"
        "    if (r != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    r = y % 100;\n"
        "    if (r != 0) {\n"
        "        return 1;\n"
        "    }\n"
        "    r = y % 400;\n"
        "    if (r == 0) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint32_t nstime_year_days(int64_t y) {\n"
        "    if (nstime_is_leap(y) != 0) {\n"
        "        return 366;\n"
        "    }\n"
        "    return 365;\n"
        "}\n"
        "\n"
        "uint32_t nstime_month_days(int64_t y, uint32_t m) {\n"
        "    if (m == 1) {\n"
        "        if (nstime_is_leap(y) != 0) {\n"
        "            return 29;\n"
        "        }\n"
        "        return 28;\n"
        "    }\n"
        "    if (m == 3 || m == 5 || m == 8 || m == 10) {\n"
        "        return 30;\n"
        "    }\n"
        "    return 31;\n"
        "}\n"
        "\n"
        "uint32_t nstime_month_from_name(const uint8_t *s) {\n"
        "    if (s[0] == 74 && s[1] == 97 && s[2] == 110) { return 0; }\n"
        "    if (s[0] == 70 && s[1] == 101 && s[2] == 98) { return 1; }\n"
        "    if (s[0] == 77 && s[1] == 97 && s[2] == 114) { return 2; }\n"
        "    if (s[0] == 65 && s[1] == 112 && s[2] == 114) { return 3; }\n"
        "    if (s[0] == 77 && s[1] == 97 && s[2] == 121) { return 4; }\n"
        "    if (s[0] == 74 && s[1] == 117 && s[2] == 110) { return 5; }\n"
        "    if (s[0] == 74 && s[1] == 117 && s[2] == 108) { return 6; }\n"
        "    if (s[0] == 65 && s[1] == 117 && s[2] == 103) { return 7; }\n"
        "    if (s[0] == 83 && s[1] == 101 && s[2] == 112) { return 8; }\n"
        "    if (s[0] == 79 && s[1] == 99 && s[2] == 116) { return 9; }\n"
        "    if (s[0] == 78 && s[1] == 111 && s[2] == 118) { return 10; }\n"
        "    if (s[0] == 68 && s[1] == 101 && s[2] == 99) { return 11; }\n"
        "    return 99;\n"
        "}\n"
        "\n"
        "void nstime_putc(uint32_t *pos, uint8_t c) {\n"
        "    nstime_ret[*pos] = c;\n"
        "    *pos = *pos + 1;\n"
        "}\n"
        "\n"
        "void nstime_put2(uint32_t *pos, uint32_t v) {\n"
        "    nstime_putc(pos, 48 + (v / 10) % 10);\n"
        "    nstime_putc(pos, 48 + v % 10);\n"
        "}\n"
        "\n"
        "void nstime_put4(uint32_t *pos, uint32_t v) {\n"
        "    nstime_putc(pos, 48 + (v / 1000) % 10);\n"
        "    nstime_putc(pos, 48 + (v / 100) % 10);\n"
        "    nstime_putc(pos, 48 + (v / 10) % 10);\n"
        "    nstime_putc(pos, 48 + v % 10);\n"
        "}\n"
        "\n"
        "void nstime_put_weekday(uint32_t *pos, uint32_t w) {\n"
        "    if (w == 0) { nstime_putc(pos, 83); nstime_putc(pos, 117); nstime_putc(pos, 110); return; }\n"
        "    if (w == 1) { nstime_putc(pos, 77); nstime_putc(pos, 111); nstime_putc(pos, 110); return; }\n"
        "    if (w == 2) { nstime_putc(pos, 84); nstime_putc(pos, 117); nstime_putc(pos, 101); return; }\n"
        "    if (w == 3) { nstime_putc(pos, 87); nstime_putc(pos, 101); nstime_putc(pos, 100); return; }\n"
        "    if (w == 4) { nstime_putc(pos, 84); nstime_putc(pos, 104); nstime_putc(pos, 117); return; }\n"
        "    if (w == 5) { nstime_putc(pos, 70); nstime_putc(pos, 114); nstime_putc(pos, 105); return; }\n"
        "    nstime_putc(pos, 83); nstime_putc(pos, 97); nstime_putc(pos, 116);\n"
        "}\n"
        "\n"
        "void nstime_put_month(uint32_t *pos, uint32_t m) {\n"
        "    if (m == 0) { nstime_putc(pos, 74); nstime_putc(pos, 97); nstime_putc(pos, 110); return; }\n"
        "    if (m == 1) { nstime_putc(pos, 70); nstime_putc(pos, 101); nstime_putc(pos, 98); return; }\n"
        "    if (m == 2) { nstime_putc(pos, 77); nstime_putc(pos, 97); nstime_putc(pos, 114); return; }\n"
        "    if (m == 3) { nstime_putc(pos, 65); nstime_putc(pos, 112); nstime_putc(pos, 114); return; }\n"
        "    if (m == 4) { nstime_putc(pos, 77); nstime_putc(pos, 97); nstime_putc(pos, 121); return; }\n"
        "    if (m == 5) { nstime_putc(pos, 74); nstime_putc(pos, 117); nstime_putc(pos, 110); return; }\n"
        "    if (m == 6) { nstime_putc(pos, 74); nstime_putc(pos, 117); nstime_putc(pos, 108); return; }\n"
        "    if (m == 7) { nstime_putc(pos, 65); nstime_putc(pos, 117); nstime_putc(pos, 103); return; }\n"
        "    if (m == 8) { nstime_putc(pos, 83); nstime_putc(pos, 101); nstime_putc(pos, 112); return; }\n"
        "    if (m == 9) { nstime_putc(pos, 79); nstime_putc(pos, 99); nstime_putc(pos, 116); return; }\n"
        "    if (m == 10) { nstime_putc(pos, 78); nstime_putc(pos, 111); nstime_putc(pos, 118); return; }\n"
        "    nstime_putc(pos, 68); nstime_putc(pos, 101); nstime_putc(pos, 99);\n"
        "}\n"
        "\n"
        "export const uint8_t *rfc1123_date(int64_t t) {\n"
        "    int64_t days;\n"
        "    uint32_t rem;\n"
        "    int64_t year;\n"
        "    uint32_t month;\n"
        "    uint32_t day;\n"
        "    uint32_t hour;\n"
        "    uint32_t minute;\n"
        "    uint32_t second;\n"
        "    uint32_t weekday;\n"
        "    uint32_t pos;\n"
        "    if (t < 0) {\n"
        "        t = 0;\n"
        "    }\n"
        "    days = t / 86400;\n"
        "    rem = t % 86400;\n"
        "    hour = rem / 3600;\n"
        "    rem = rem % 3600;\n"
        "    minute = rem / 60;\n"
        "    second = rem % 60;\n"
        "    weekday = (days + 4) % 7;\n"
        "    year = 1970;\n"
        "    while (days >= nstime_year_days(year)) {\n"
        "        days = days - nstime_year_days(year);\n"
        "        year = year + 1;\n"
        "    }\n"
        "    month = 0;\n"
        "    while (days >= nstime_month_days(year, month)) {\n"
        "        days = days - nstime_month_days(year, month);\n"
        "        month = month + 1;\n"
        "    }\n"
        "    day = days + 1;\n"
        "    pos = 0;\n"
        "    nstime_put_weekday(&pos, weekday);\n"
        "    nstime_putc(&pos, 44); nstime_putc(&pos, 32);\n"
        "    nstime_put2(&pos, day); nstime_putc(&pos, 32);\n"
        "    nstime_put_month(&pos, month); nstime_putc(&pos, 32);\n"
        "    nstime_put4(&pos, year); nstime_putc(&pos, 32);\n"
        "    nstime_put2(&pos, hour); nstime_putc(&pos, 58);\n"
        "    nstime_put2(&pos, minute); nstime_putc(&pos, 58);\n"
        "    nstime_put2(&pos, second);\n"
        "    nstime_putc(&pos, 32); nstime_putc(&pos, 71); nstime_putc(&pos, 77); nstime_putc(&pos, 84);\n"
        "    nstime_ret[pos] = 0;\n"
        "    return nstime_ret;\n"
        "}\n"
        "\n"
        "export int nsc_sntimet(uint8_t *str, uint64_t size, int64_t *timep) {\n"
        "    int64_t val;\n"
        "    uint64_t mag;\n"
        "    uint8_t tmp[32];\n"
        "    uint32_t len;\n"
        "    uint32_t i;\n"
        "    uint32_t out;\n"
        "    uint8_t neg;\n"
        "    if (str == 0 || timep == 0) {\n"
        "        return -1;\n"
        "    }\n"
        "    val = *timep;\n"
        "    neg = 0;\n"
        "    if (val < 0) {\n"
        "        neg = 1;\n"
        "        mag = 0 - val;\n"
        "    } else {\n"
        "        mag = val;\n"
        "    }\n"
        "    len = 0;\n"
        "    if (mag == 0) {\n"
        "        tmp[len] = 48;\n"
        "        len = len + 1;\n"
        "    }\n"
        "    while (mag > 0) {\n"
        "        tmp[len] = 48 + (mag % 10);\n"
        "        len = len + 1;\n"
        "        mag = mag / 10;\n"
        "    }\n"
        "    if (neg != 0) {\n"
        "        tmp[len] = 45;\n"
        "        len = len + 1;\n"
        "    }\n"
        "    out = 0;\n"
        "    i = 0;\n"
        "    while (i < len && out + 1 < size) {\n"
        "        str[out] = tmp[len - 1 - i];\n"
        "        out = out + 1;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (size > 0) {\n"
        "        str[out] = 0;\n"
        "    }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "export int nsc_snptimet(const uint8_t *str, uint64_t size, int64_t *timep) {\n"
        "    uint64_t i;\n"
        "    int64_t val;\n"
        "    uint8_t neg;\n"
        "    uint8_t digits;\n"
        "    if (str == 0 || timep == 0 || size < 1) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    i = 0;\n"
        "    neg = 0;\n"
        "    if (str[0] == 45) {\n"
        "        neg = 1;\n"
        "        i = 1;\n"
        "    }\n"
        "    val = 0;\n"
        "    digits = 0;\n"
        "    while (i < size && str[i] != 0) {\n"
        "        if (nstime_is_digit(str[i]) == 0) {\n"
        "            return NSERROR_BAD_PARAMETER;\n"
        "        }\n"
        "        val = val * 10 + (str[i] - 48);\n"
        "        digits = 1;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (digits == 0) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (neg != 0) {\n"
        "        val = 0 - val;\n"
        "    }\n"
        "    *timep = val;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int nsc_strntimet(const uint8_t *str, uint64_t size, int64_t *timep) {\n"
        "    int day;\n"
        "    int year;\n"
        "    int hour;\n"
        "    int minute;\n"
        "    int second;\n"
        "    uint32_t month;\n"
        "    int64_t days;\n"
        "    int64_t y;\n"
        "    uint32_t m;\n"
        "    if (str == 0 || timep == 0 || size < 1) {\n"
        "        return NSERROR_BAD_PARAMETER;\n"
        "    }\n"
        "    if (nstime_is_digit(str[0]) != 0 || str[0] == 45) {\n"
        "        return nsc_snptimet(str, size, timep);\n"
        "    }\n"
        "    if (size < 29) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    if (str[3] != 44 || str[4] != 32 || str[7] != 32 || str[11] != 32 || str[16] != 32 || str[19] != 58 || str[22] != 58 || str[25] != 32) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    if (str[26] != 71 || str[27] != 77 || str[28] != 84) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    day = nstime_parse2(str + 5);\n"
        "    month = nstime_month_from_name(str + 8);\n"
        "    year = nstime_parse4(str + 12);\n"
        "    hour = nstime_parse2(str + 17);\n"
        "    minute = nstime_parse2(str + 20);\n"
        "    second = nstime_parse2(str + 23);\n"
        "    if (day < 1 || month > 11 || year < 1970 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    if (day > nstime_month_days(year, month)) {\n"
        "        return NSERROR_INVALID;\n"
        "    }\n"
        "    days = 0;\n"
        "    y = 1970;\n"
        "    while (y < year) {\n"
        "        days = days + nstime_year_days(y);\n"
        "        y = y + 1;\n"
        "    }\n"
        "    m = 0;\n"
        "    while (m < month) {\n"
        "        days = days + nstime_month_days(year, m);\n"
        "        m = m + 1;\n"
        "    }\n"
        "    days = days + day - 1;\n"
        "    *timep = days * 86400 + hour * 3600 + minute * 60 + second;\n"
        "    return NSERROR_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_primitives_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern int lwc_intern_string(const uint8_t *s, uint64_t slen, struct lwc_string_s **ret);\n"
        "\n"
        "enum {\n"
        "    NSERROR_OK = 0,\n"
        "    NSERROR_NOMEM = 2,\n"
        "    NSERROR_NOT_FOUND = 4\n"
        "};\n"
        "\n"
        "struct lwc_string_s {\n"
        "    uint32_t opaque;\n"
        "};\n"
        "\n"
        "export void http__skip_LWS(const uint8_t **input) {\n"
        "    const uint8_t *pos;\n"
        "    if (input == 0 || *input == 0) {\n"
        "        return;\n"
        "    }\n"
        "    pos = *input;\n"
        "    while (*pos == 32 || *pos == 9) {\n"
        "        pos = pos + 1;\n"
        "    }\n"
        "    *input = pos;\n"
        "}\n"
        "\n"
        "uint8_t http_is_separator(uint8_t c) {\n"
        "    if (c == 40 || c == 41 || c == 60 || c == 62 || c == 64) { return 1; }\n"
        "    if (c == 44 || c == 59 || c == 58 || c == 92 || c == 34 || c == 47) { return 1; }\n"
        "    if (c == 91 || c == 93 || c == 63 || c == 61 || c == 123 || c == 125) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint8_t http_is_token_char(uint8_t c) {\n"
        "    if (c <= 32 || c > 126) {\n"
        "        return 0;\n"
        "    }\n"
        "    if (http_is_separator(c) != 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "export int http__parse_token(const uint8_t **input, struct lwc_string_s **value) {\n"
        "    const uint8_t *start;\n"
        "    const uint8_t *end;\n"
        "    struct lwc_string_s *token;\n"
        "    if (input == 0 || *input == 0 || value == 0) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    start = *input;\n"
        "    end = start;\n"
        "    while (http_is_token_char(*end) != 0) {\n"
        "        end = end + 1;\n"
        "    }\n"
        "    if (end == start) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    if (lwc_intern_string(start, end - start, &token) != 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    *value = token;\n"
        "    *input = end;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "uint8_t http_is_qdtext(uint8_t c) {\n"
        "    if (c == 9 || c == 13 || c == 10 || c == 32 || c == 33) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c >= 35 && c <= 126) {\n"
        "        return 1;\n"
        "    }\n"
        "    if (c > 127) {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "export int http__parse_quoted_string(const uint8_t **input, struct lwc_string_s **value) {\n"
        "    const uint8_t *start;\n"
        "    const uint8_t *end;\n"
        "    struct lwc_string_s *string_value;\n"
        "    if (input == 0 || *input == 0 || value == 0) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    start = *input;\n"
        "    if (*start != 34) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    start = start + 1;\n"
        "    end = start;\n"
        "    while (http_is_qdtext(*end) != 0) {\n"
        "        end = end + 1;\n"
        "    }\n"
        "    if (*end != 34) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    if (lwc_intern_string(start, end - start, &string_value) != 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    *value = string_value;\n"
        "    *input = end + 1;\n"
        "    return NSERROR_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_generics_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "\n"
        "export void http___item_list_destroy(struct http__item *list) {\n"
        "    struct http__item *victim;\n"
        "    while (list != 0) {\n"
        "        victim = list;\n"
        "        list = victim->next;\n"
        "        victim->destroy((uint8_t *)victim);\n"
        "    }\n"
        "}\n"
        "\n"
        "export int http___item_list_parse(const uint8_t **input, int (*itemparser)(const uint8_t **input, struct http__item **item), struct http__item *first, struct http__item **items) {\n"
        "    const uint8_t *pos;\n"
        "    uint8_t separator;\n"
        "    struct http__item *item;\n"
        "    struct http__item *list;\n"
        "    int error;\n"
        "    pos = *input;\n"
        "    separator = *pos;\n"
        "    list = first;\n"
        "    error = NSERROR_OK;\n"
        "    while (*pos == separator) {\n"
        "        pos = pos + 1;\n"
        "        http__skip_LWS(&pos);\n"
        "        error = itemparser(&pos, &item);\n"
        "        if (error == NSERROR_OK) {\n"
        "            if (list != 0) {\n"
        "                item->next = list;\n"
        "            }\n"
        "            list = item;\n"
        "            http__skip_LWS(&pos);\n"
        "        } else if (error != NSERROR_NOT_FOUND) {\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    if (error != NSERROR_OK && error != NSERROR_NOT_FOUND) {\n"
        "        http___item_list_destroy(list);\n"
        "    } else if (list == 0) {\n"
        "        error = NSERROR_NOT_FOUND;\n"
        "    } else {\n"
        "        error = NSERROR_OK;\n"
        "        *items = list;\n"
        "        *input = pos;\n"
        "    }\n"
        "    return error;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_parameter_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_token(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http__parse_quoted_string(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern void http___item_list_destroy(struct http__item *list);\n"
        "extern struct lwc_string_s *lwc_string_ref(struct lwc_string_s *str);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "extern int lwc_string_caseless_isequal(struct lwc_string_s *a, struct lwc_string_s *b, int *ret);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_parameter {\n"
        "    struct http__item base;\n"
        "    struct lwc_string_s *name;\n"
        "    struct lwc_string_s *value;\n"
        "};\n"
        "\n"
        "void http_destroy_parameter(uint8_t *self_ptr) {\n"
        "    struct http_parameter *self;\n"
        "    self = (struct http_parameter *)self_ptr;\n"
        "    if (self == 0) {\n"
        "        return;\n"
        "    }\n"
        "    lwc_string_unref(self->name);\n"
        "    lwc_string_unref(self->value);\n"
        "    free((uint8_t *)self);\n"
        "}\n"
        "\n"
        "export int http__parse_parameter(const uint8_t **input, struct http_parameter **parameter) {\n"
        "    const uint8_t *pos;\n"
        "    struct lwc_string_s *name;\n"
        "    struct lwc_string_s *value;\n"
        "    struct http_parameter *param;\n"
        "    int error;\n"
        "    pos = *input;\n"
        "    error = http__parse_token(&pos, &name);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos != 61) {\n"
        "        lwc_string_unref(name);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    pos = pos + 1;\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 34) {\n"
        "        error = http__parse_quoted_string(&pos, &value);\n"
        "    } else {\n"
        "        error = http__parse_token(&pos, &value);\n"
        "    }\n"
        "    if (error != NSERROR_OK) {\n"
        "        lwc_string_unref(name);\n"
        "        return error;\n"
        "    }\n"
        "    param = (struct http_parameter *)malloc(sizeof(struct http_parameter));\n"
        "    if (param == 0) {\n"
        "        lwc_string_unref(value);\n"
        "        lwc_string_unref(name);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    param->base.next = (struct http__item *)0;\n"
        "    param->base.destroy = http_destroy_parameter;\n"
        "    param->name = name;\n"
        "    param->value = value;\n"
        "    *parameter = param;\n"
        "    *input = pos;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int http_parameter_list_find_item(const struct http_parameter *list, struct lwc_string_s *name, struct lwc_string_s **value) {\n"
        "    int match;\n"
        "    while (list != 0) {\n"
        "        match = 0;\n"
        "        if (lwc_string_caseless_isequal(name, list->name, &match) == 0 && match != 0) {\n"
        "            *value = lwc_string_ref(list->value);\n"
        "            return NSERROR_OK;\n"
        "        }\n"
        "        list = (const struct http_parameter *)list->base.next;\n"
        "    }\n"
        "    return NSERROR_NOT_FOUND;\n"
        "}\n"
        "\n"
        "export const struct http_parameter *http_parameter_list_iterate(const struct http_parameter *cur, struct lwc_string_s **name, struct lwc_string_s **value) {\n"
        "    if (cur == 0) {\n"
        "        return (const struct http_parameter *)0;\n"
        "    }\n"
        "    *name = lwc_string_ref(cur->name);\n"
        "    *value = lwc_string_ref(cur->value);\n"
        "    return (const struct http_parameter *)cur->base.next;\n"
        "}\n"
        "\n"
        "export void http_parameter_list_destroy(struct http_parameter *list) {\n"
        "    http___item_list_destroy((struct http__item *)list);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_content_type_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_token(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http___item_list_parse(const uint8_t **input, uint8_t *itemparser, struct http__item *first, struct http__item **items);\n"
        "extern int http__parse_parameter(const uint8_t **input, struct http_parameter **parameter);\n"
        "extern void http_parameter_list_destroy(struct http_parameter *list);\n"
        "extern int lwc_intern_string(const uint8_t *s, uint64_t slen, struct lwc_string_s **ret);\n"
        "extern uint8_t *lwc_string_data(struct lwc_string_s *str);\n"
        "extern uint64_t lwc_string_length(struct lwc_string_s *str);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_parameter { uint32_t opaque; };\n"
        "struct http_content_type {\n"
        "    struct lwc_string_s *media_type;\n"
        "    struct http_parameter *parameters;\n"
        "};\n"
        "\n"
        "export int http_parse_content_type(const uint8_t *header_value, struct http_content_type **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct lwc_string_s *type;\n"
        "    struct lwc_string_s *subtype;\n"
        "    struct http_parameter *params;\n"
        "    struct http__item *param_items;\n"
        "    struct lwc_string_s *imime;\n"
        "    struct http_content_type *ct;\n"
        "    uint64_t type_len;\n"
        "    uint64_t subtype_len;\n"
        "    uint64_t mime_len;\n"
        "    uint64_t i;\n"
        "    uint8_t *mime;\n"
        "    uint8_t *type_data;\n"
        "    uint8_t *subtype_data;\n"
        "    int error;\n"
        "    pos = header_value;\n"
        "    type = (struct lwc_string_s *)0;\n"
        "    subtype = (struct lwc_string_s *)0;\n"
        "    params = (struct http_parameter *)0;\n"
        "    param_items = (struct http__item *)0;\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_token(&pos, &type);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos != 47) {\n"
        "        lwc_string_unref(type);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    pos = pos + 1;\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_token(&pos, &subtype);\n"
        "    if (error != NSERROR_OK) {\n"
        "        lwc_string_unref(type);\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 59) {\n"
        "        error = http___item_list_parse(&pos, (uint8_t *)http__parse_parameter, (struct http__item *)0, &param_items);\n"
        "        if (error != NSERROR_OK && error != NSERROR_NOT_FOUND) {\n"
        "            lwc_string_unref(subtype);\n"
        "            lwc_string_unref(type);\n"
        "            return error;\n"
        "        }\n"
        "        params = (struct http_parameter *)param_items;\n"
        "    }\n"
        "    type_len = lwc_string_length(type);\n"
        "    subtype_len = lwc_string_length(subtype);\n"
        "    mime_len = type_len + subtype_len + 1;\n"
        "    mime = malloc(mime_len + 1);\n"
        "    if (mime == 0) {\n"
        "        http_parameter_list_destroy(params);\n"
        "        lwc_string_unref(subtype);\n"
        "        lwc_string_unref(type);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    type_data = lwc_string_data(type);\n"
        "    subtype_data = lwc_string_data(subtype);\n"
        "    i = 0;\n"
        "    while (i < type_len) {\n"
        "        mime[i] = type_data[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    mime[i] = 47;\n"
        "    i = 0;\n"
        "    while (i < subtype_len) {\n"
        "        mime[type_len + 1 + i] = subtype_data[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    mime[mime_len] = 0;\n"
        "    lwc_string_unref(subtype);\n"
        "    lwc_string_unref(type);\n"
        "    if (lwc_intern_string(mime, mime_len, &imime) != 0) {\n"
        "        http_parameter_list_destroy(params);\n"
        "        free(mime);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    free(mime);\n"
        "    ct = (struct http_content_type *)malloc(sizeof(struct http_content_type));\n"
        "    if (ct == 0) {\n"
        "        lwc_string_unref(imime);\n"
        "        http_parameter_list_destroy(params);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    ct->media_type = imime;\n"
        "    ct->parameters = params;\n"
        "    *result = ct;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void http_content_type_destroy(struct http_content_type *victim) {\n"
        "    if (victim == 0) {\n"
        "        return;\n"
        "    }\n"
        "    lwc_string_unref(victim->media_type);\n"
        "    http_parameter_list_destroy(victim->parameters);\n"
        "    free((uint8_t *)victim);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_content_disposition_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_token(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http___item_list_parse(const uint8_t **input, uint8_t *itemparser, struct http__item *first, struct http__item **items);\n"
        "extern int http__parse_parameter(const uint8_t **input, struct http_parameter **parameter);\n"
        "extern void http_parameter_list_destroy(struct http_parameter *list);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_parameter { uint32_t opaque; };\n"
        "struct http_content_disposition {\n"
        "    struct lwc_string_s *disposition_type;\n"
        "    struct http_parameter *parameters;\n"
        "};\n"
        "\n"
        "export int http_parse_content_disposition(const uint8_t *header_value, struct http_content_disposition **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct lwc_string_s *mtype;\n"
        "    struct http_parameter *params;\n"
        "    struct http__item *param_items;\n"
        "    struct http_content_disposition *cd;\n"
        "    int error;\n"
        "    pos = header_value;\n"
        "    mtype = (struct lwc_string_s *)0;\n"
        "    params = (struct http_parameter *)0;\n"
        "    param_items = (struct http__item *)0;\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_token(&pos, &mtype);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 59) {\n"
        "        error = http___item_list_parse(&pos, (uint8_t *)http__parse_parameter, (struct http__item *)0, &param_items);\n"
        "        if (error != NSERROR_OK && error != NSERROR_NOT_FOUND) {\n"
        "            lwc_string_unref(mtype);\n"
        "            return error;\n"
        "        }\n"
        "        params = (struct http_parameter *)param_items;\n"
        "    }\n"
        "    cd = (struct http_content_disposition *)malloc(sizeof(struct http_content_disposition));\n"
        "    if (cd == 0) {\n"
        "        http_parameter_list_destroy(params);\n"
        "        lwc_string_unref(mtype);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    cd->disposition_type = mtype;\n"
        "    cd->parameters = params;\n"
        "    *result = cd;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void http_content_disposition_destroy(struct http_content_disposition *victim) {\n"
        "    if (victim == 0) {\n"
        "        return;\n"
        "    }\n"
        "    lwc_string_unref(victim->disposition_type);\n"
        "    http_parameter_list_destroy(victim->parameters);\n"
        "    free((uint8_t *)victim);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_challenge_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_token(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http___item_list_parse(const uint8_t **input, uint8_t *itemparser, struct http__item *first, struct http__item **items);\n"
        "extern int http__parse_parameter(const uint8_t **input, struct http_parameter **parameter);\n"
        "extern void http_parameter_list_destroy(struct http_parameter *list);\n"
        "extern void http___item_list_destroy(struct http__item *list);\n"
        "extern struct lwc_string_s *lwc_string_ref(struct lwc_string_s *str);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_parameter { uint32_t opaque; };\n"
        "struct http_challenge {\n"
        "    struct http__item base;\n"
        "    struct lwc_string_s *scheme;\n"
        "    struct http_parameter *params;\n"
        "};\n"
        "\n"
        "void http_destroy_challenge(uint8_t *self_ptr) {\n"
        "    struct http_challenge *self;\n"
        "    self = (struct http_challenge *)self_ptr;\n"
        "    if (self == 0) {\n"
        "        return;\n"
        "    }\n"
        "    lwc_string_unref(self->scheme);\n"
        "    http_parameter_list_destroy(self->params);\n"
        "    free((uint8_t *)self);\n"
        "}\n"
        "\n"
        "export int http__parse_challenge(const uint8_t **input, struct http_challenge **challenge) {\n"
        "    const uint8_t *pos;\n"
        "    struct http_challenge *result;\n"
        "    struct lwc_string_s *scheme;\n"
        "    struct http_parameter *first;\n"
        "    struct http_parameter *params;\n"
        "    struct http__item *param_items;\n"
        "    int error;\n"
        "    pos = *input;\n"
        "    scheme = (struct lwc_string_s *)0;\n"
        "    first = (struct http_parameter *)0;\n"
        "    params = (struct http_parameter *)0;\n"
        "    param_items = (struct http__item *)0;\n"
        "    error = http__parse_token(&pos, &scheme);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    if (*pos != 32 && *pos != 9) {\n"
        "        lwc_string_unref(scheme);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_parameter(&pos, &first);\n"
        "    if (error != NSERROR_OK) {\n"
        "        lwc_string_unref(scheme);\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 44) {\n"
        "        error = http___item_list_parse(&pos, (uint8_t *)http__parse_parameter, (struct http__item *)first, &param_items);\n"
        "        if (error != NSERROR_OK && error != NSERROR_NOT_FOUND) {\n"
        "            lwc_string_unref(scheme);\n"
        "            return error;\n"
        "        }\n"
        "        params = (struct http_parameter *)param_items;\n"
        "    } else {\n"
        "        params = first;\n"
        "    }\n"
        "    result = (struct http_challenge *)malloc(sizeof(struct http_challenge));\n"
        "    if (result == 0) {\n"
        "        http_parameter_list_destroy(params);\n"
        "        lwc_string_unref(scheme);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    result->base.next = (struct http__item *)0;\n"
        "    result->base.destroy = http_destroy_challenge;\n"
        "    result->scheme = scheme;\n"
        "    result->params = params;\n"
        "    *challenge = result;\n"
        "    *input = pos;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export const struct http_challenge *http_challenge_list_iterate(const struct http_challenge *cur, struct lwc_string_s **scheme, struct http_parameter **parameters) {\n"
        "    if (cur == 0) {\n"
        "        return (const struct http_challenge *)0;\n"
        "    }\n"
        "    *scheme = lwc_string_ref(cur->scheme);\n"
        "    *parameters = cur->params;\n"
        "    return (const struct http_challenge *)cur->base.next;\n"
        "}\n"
        "\n"
        "export void http_challenge_list_destroy(struct http_challenge *list) {\n"
        "    http___item_list_destroy((struct http__item *)list);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_www_authenticate_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_challenge(const uint8_t **input, struct http_challenge **challenge);\n"
        "extern int http___item_list_parse(const uint8_t **input, uint8_t *itemparser, struct http__item *first, struct http__item **items);\n"
        "extern void http_challenge_list_destroy(struct http_challenge *list);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_challenge { uint32_t opaque; };\n"
        "struct http_www_authenticate {\n"
        "    struct http_challenge *challenges;\n"
        "};\n"
        "\n"
        "export int http_parse_www_authenticate(const uint8_t *header_value, struct http_www_authenticate **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct http_challenge *first;\n"
        "    struct http_challenge *list;\n"
        "    struct http__item *list_items;\n"
        "    struct http_www_authenticate *wa;\n"
        "    int error;\n"
        "    pos = header_value;\n"
        "    first = (struct http_challenge *)0;\n"
        "    list = (struct http_challenge *)0;\n"
        "    list_items = (struct http__item *)0;\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_challenge(&pos, &first);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 44) {\n"
        "        error = http___item_list_parse(&pos, (uint8_t *)http__parse_challenge, (struct http__item *)first, &list_items);\n"
        "        if (error != NSERROR_OK && error != NSERROR_NOT_FOUND) {\n"
        "            return error;\n"
        "        }\n"
        "        list = (struct http_challenge *)list_items;\n"
        "    } else {\n"
        "        list = first;\n"
        "    }\n"
        "    wa = (struct http_www_authenticate *)malloc(sizeof(struct http_www_authenticate));\n"
        "    if (wa == 0) {\n"
        "        http_challenge_list_destroy(list);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    wa->challenges = list;\n"
        "    *result = wa;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void http_www_authenticate_destroy(struct http_www_authenticate *victim) {\n"
        "    if (victim == 0) {\n"
        "        return;\n"
        "    }\n"
        "    http_challenge_list_destroy(victim->challenges);\n"
        "    free((uint8_t *)victim);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_cache_control_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_token(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http__parse_quoted_string(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http___item_list_parse(const uint8_t **input, uint8_t *itemparser, struct http__item *first, struct http__item **items);\n"
        "extern void http___item_list_destroy(struct http__item *list);\n"
        "extern uint8_t *lwc_string_data(struct lwc_string_s *str);\n"
        "extern uint64_t lwc_string_length(struct lwc_string_s *str);\n"
        "extern struct lwc_string_s *lwc_string_ref(struct lwc_string_s *str);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "extern int lwc_string_caseless_isequal(struct lwc_string_s *a, struct lwc_string_s *b, uint8_t *match);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_directive {\n"
        "    struct http__item base;\n"
        "    struct lwc_string_s *name;\n"
        "    struct lwc_string_s *value;\n"
        "};\n"
        "struct http_cache_control {\n"
        "    uint32_t max_age;\n"
        "    uint8_t max_age_valid;\n"
        "    uint8_t no_cache;\n"
        "    uint8_t no_store;\n"
        "};\n"
        "\n"
        "void http_destroy_directive(uint8_t *self_ptr) {\n"
        "    struct http_directive *self;\n"
        "    self = (struct http_directive *)self_ptr;\n"
        "    if (self == 0) { return; }\n"
        "    lwc_string_unref(self->name);\n"
        "    lwc_string_unref(self->value);\n"
        "    free((uint8_t *)self);\n"
        "}\n"
        "\n"
        "int http__parse_directive(const uint8_t **input, struct http_directive **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct lwc_string_s *name;\n"
        "    struct lwc_string_s *value;\n"
        "    struct http_directive *directive;\n"
        "    int error;\n"
        "    pos = *input;\n"
        "    name = (struct lwc_string_s *)0;\n"
        "    value = (struct lwc_string_s *)0;\n"
        "    error = http__parse_token(&pos, &name);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 61) {\n"
        "        pos = pos + 1;\n"
        "        http__skip_LWS(&pos);\n"
        "        if (*pos == 34) {\n"
        "            error = http__parse_quoted_string(&pos, &value);\n"
        "        } else {\n"
        "            error = http__parse_token(&pos, &value);\n"
        "        }\n"
        "        if (error != NSERROR_OK) {\n"
        "            lwc_string_unref(name);\n"
        "            return error;\n"
        "        }\n"
        "    }\n"
        "    directive = (struct http_directive *)malloc(sizeof(struct http_directive));\n"
        "    if (directive == 0) {\n"
        "        lwc_string_unref(value);\n"
        "        lwc_string_unref(name);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    directive->base.next = (struct http__item *)0;\n"
        "    directive->base.destroy = http_destroy_directive;\n"
        "    directive->name = name;\n"
        "    directive->value = value;\n"
        "    *result = directive;\n"
        "    *input = pos;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "void http_directive_list_destroy(struct http_directive *list) {\n"
        "    http___item_list_destroy((struct http__item *)list);\n"
        "}\n"
        "\n"
        "uint8_t http_lwc_name_is(struct lwc_string_s *name, uint8_t *literal, uint64_t literal_len) {\n"
        "    uint8_t *data;\n"
        "    uint64_t i;\n"
        "    uint8_t a;\n"
        "    uint8_t b;\n"
        "    if (name == 0 || lwc_string_length(name) != literal_len) {\n"
        "        return 0;\n"
        "    }\n"
        "    data = lwc_string_data(name);\n"
        "    i = 0;\n"
        "    while (i < literal_len) {\n"
        "        a = data[i];\n"
        "        b = literal[i];\n"
        "        if (a >= 65 && a <= 90) { a = a + 32; }\n"
        "        if (b >= 65 && b <= 90) { b = b + 32; }\n"
        "        if (a != b) { return 0; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "int http_directive_list_find_item(struct http_directive *list, uint8_t *literal, uint64_t literal_len, struct lwc_string_s **value) {\n"
        "    while (list != 0) {\n"
        "        if (http_lwc_name_is(list->name, literal, literal_len) != 0) {\n"
        "            if (list->value != 0) {\n"
        "                *value = lwc_string_ref(list->value);\n"
        "            } else {\n"
        "                *value = (struct lwc_string_s *)0;\n"
        "            }\n"
        "            return NSERROR_OK;\n"
        "        }\n"
        "        list = (struct http_directive *)list->base.next;\n"
        "    }\n"
        "    return NSERROR_NOT_FOUND;\n"
        "}\n"
        "\n"
        "uint32_t http_directive_count(struct http_directive *list, struct lwc_string_s *key) {\n"
        "    uint32_t count;\n"
        "    uint8_t match;\n"
        "    count = 0;\n"
        "    while (list != 0) {\n"
        "        match = 0;\n"
        "        if (lwc_string_caseless_isequal(key, list->name, &match) == 0 && match != 0) {\n"
        "            count = count + 1;\n"
        "        }\n"
        "        list = (struct http_directive *)list->base.next;\n"
        "    }\n"
        "    return count;\n"
        "}\n"
        "\n"
        "uint8_t http_directive_check_duplicates(struct http_directive *list) {\n"
        "    struct http_directive *key;\n"
        "    key = list;\n"
        "    while (key != 0) {\n"
        "        if (http_directive_count(list, key->name) != 1) {\n"
        "            return 0;\n"
        "        }\n"
        "        key = (struct http_directive *)key->base.next;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "int http_parse_delta_seconds(struct lwc_string_s *value, uint32_t *result) {\n"
        "    uint8_t *data;\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint32_t val;\n"
        "    uint32_t nv;\n"
        "    if (value == 0) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    data = lwc_string_data(value);\n"
        "    len = lwc_string_length(value);\n"
        "    if (len == 0) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    val = 0;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        if (data[i] < 48 || data[i] > 57) {\n"
        "            return NSERROR_NOT_FOUND;\n"
        "        }\n"
        "        nv = val * 10 + (data[i] - 48);\n"
        "        if (nv < val) {\n"
        "            val = 4294967295;\n"
        "        } else {\n"
        "            val = nv;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    *result = val;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int http_parse_cache_control(const uint8_t *header_value, struct http_cache_control **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct http_cache_control *cc;\n"
        "    struct http_directive *first;\n"
        "    struct http_directive *directives;\n"
        "    struct http__item *directive_items;\n"
        "    struct lwc_string_s *value_str;\n"
        "    uint8_t max_age_name[8];\n"
        "    uint8_t no_cache_name[9];\n"
        "    uint8_t no_store_name[9];\n"
        "    uint32_t max_age;\n"
        "    uint8_t max_age_valid;\n"
        "    uint8_t no_cache;\n"
        "    uint8_t no_store;\n"
        "    int error;\n"
        "    max_age_name[0]=109; max_age_name[1]=97; max_age_name[2]=120; max_age_name[3]=45; max_age_name[4]=97; max_age_name[5]=103; max_age_name[6]=101; max_age_name[7]=0;\n"
        "    no_cache_name[0]=110; no_cache_name[1]=111; no_cache_name[2]=45; no_cache_name[3]=99; no_cache_name[4]=97; no_cache_name[5]=99; no_cache_name[6]=104; no_cache_name[7]=101; no_cache_name[8]=0;\n"
        "    no_store_name[0]=110; no_store_name[1]=111; no_store_name[2]=45; no_store_name[3]=115; no_store_name[4]=116; no_store_name[5]=111; no_store_name[6]=114; no_store_name[7]=101; no_store_name[8]=0;\n"
        "    pos = header_value;\n"
        "    first = (struct http_directive *)0;\n"
        "    directives = (struct http_directive *)0;\n"
        "    directive_items = (struct http__item *)0;\n"
        "    value_str = (struct lwc_string_s *)0;\n"
        "    max_age = 0;\n"
        "    max_age_valid = 0;\n"
        "    no_cache = 0;\n"
        "    no_store = 0;\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_directive(&pos, &first);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 44) {\n"
        "        error = http___item_list_parse(&pos, (uint8_t *)http__parse_directive, (struct http__item *)first, &directive_items);\n"
        "        if (error != NSERROR_OK) {\n"
        "            return error;\n"
        "        }\n"
        "        directives = (struct http_directive *)directive_items;\n"
        "    } else {\n"
        "        directives = first;\n"
        "    }\n"
        "    if (http_directive_check_duplicates(directives) == 0) {\n"
        "        http_directive_list_destroy(directives);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    error = http_directive_list_find_item(directives, max_age_name, 7, &value_str);\n"
        "    if (error == NSERROR_OK && value_str != 0) {\n"
        "        error = http_parse_delta_seconds(value_str, &max_age);\n"
        "        if (error == NSERROR_OK) { max_age_valid = 1; }\n"
        "        lwc_string_unref(value_str);\n"
        "        value_str = (struct lwc_string_s *)0;\n"
        "    }\n"
        "    error = http_directive_list_find_item(directives, no_cache_name, 8, &value_str);\n"
        "    if (error == NSERROR_OK) {\n"
        "        no_cache = 1;\n"
        "        lwc_string_unref(value_str);\n"
        "        value_str = (struct lwc_string_s *)0;\n"
        "    }\n"
        "    error = http_directive_list_find_item(directives, no_store_name, 8, &value_str);\n"
        "    if (error == NSERROR_OK) {\n"
        "        no_store = 1;\n"
        "        lwc_string_unref(value_str);\n"
        "        value_str = (struct lwc_string_s *)0;\n"
        "    }\n"
        "    http_directive_list_destroy(directives);\n"
        "    cc = (struct http_cache_control *)malloc(sizeof(struct http_cache_control));\n"
        "    if (cc == 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    cc->max_age = max_age;\n"
        "    cc->max_age_valid = max_age_valid;\n"
        "    cc->no_cache = no_cache;\n"
        "    cc->no_store = no_store;\n"
        "    *result = cc;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void http_cache_control_destroy(struct http_cache_control *victim) {\n"
        "    free((uint8_t *)victim);\n"
        "}\n"
        "\n"
        "export uint8_t http_cache_control_has_max_age(struct http_cache_control *cc) {\n"
        "    return cc->max_age_valid;\n"
        "}\n"
        "\n"
        "export uint32_t http_cache_control_max_age(struct http_cache_control *cc) {\n"
        "    return cc->max_age;\n"
        "}\n"
        "\n"
        "export uint8_t http_cache_control_no_cache(struct http_cache_control *cc) {\n"
        "    return cc->no_cache;\n"
        "}\n"
        "\n"
        "export uint8_t http_cache_control_no_store(struct http_cache_control *cc) {\n"
        "    return cc->no_store;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_http_sts_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "extern void http__skip_LWS(const uint8_t **input);\n"
        "extern int http__parse_token(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http__parse_quoted_string(const uint8_t **input, struct lwc_string_s **value);\n"
        "extern int http___item_list_parse(const uint8_t **input, uint8_t *itemparser, struct http__item *first, struct http__item **items);\n"
        "extern void http___item_list_destroy(struct http__item *list);\n"
        "extern uint8_t *lwc_string_data(struct lwc_string_s *str);\n"
        "extern uint64_t lwc_string_length(struct lwc_string_s *str);\n"
        "extern struct lwc_string_s *lwc_string_ref(struct lwc_string_s *str);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "extern int lwc_string_caseless_isequal(struct lwc_string_s *a, struct lwc_string_s *b, uint8_t *match);\n"
        "\n"
        "enum { NSERROR_OK=0, NSERROR_NOMEM=2, NSERROR_NOT_FOUND=4 };\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "struct http__item {\n"
        "    struct http__item *next;\n"
        "    void (*destroy)(uint8_t *self);\n"
        "};\n"
        "struct http_directive {\n"
        "    struct http__item base;\n"
        "    struct lwc_string_s *name;\n"
        "    struct lwc_string_s *value;\n"
        "};\n"
        "struct http_strict_transport_security {\n"
        "    uint32_t max_age;\n"
        "    uint8_t include_sub_domains;\n"
        "};\n"
        "\n"
        "void http_destroy_directive(uint8_t *self_ptr) {\n"
        "    struct http_directive *self;\n"
        "    self = (struct http_directive *)self_ptr;\n"
        "    if (self == 0) { return; }\n"
        "    lwc_string_unref(self->name);\n"
        "    lwc_string_unref(self->value);\n"
        "    free((uint8_t *)self);\n"
        "}\n"
        "\n"
        "int http__parse_directive(const uint8_t **input, struct http_directive **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct lwc_string_s *name;\n"
        "    struct lwc_string_s *value;\n"
        "    struct http_directive *directive;\n"
        "    int error;\n"
        "    pos = *input;\n"
        "    name = (struct lwc_string_s *)0;\n"
        "    value = (struct lwc_string_s *)0;\n"
        "    error = http__parse_token(&pos, &name);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 61) {\n"
        "        pos = pos + 1;\n"
        "        http__skip_LWS(&pos);\n"
        "        if (*pos == 34) {\n"
        "            error = http__parse_quoted_string(&pos, &value);\n"
        "        } else {\n"
        "            error = http__parse_token(&pos, &value);\n"
        "        }\n"
        "        if (error != NSERROR_OK) {\n"
        "            lwc_string_unref(name);\n"
        "            return error;\n"
        "        }\n"
        "    }\n"
        "    directive = (struct http_directive *)malloc(sizeof(struct http_directive));\n"
        "    if (directive == 0) {\n"
        "        lwc_string_unref(value);\n"
        "        lwc_string_unref(name);\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    directive->base.next = (struct http__item *)0;\n"
        "    directive->base.destroy = http_destroy_directive;\n"
        "    directive->name = name;\n"
        "    directive->value = value;\n"
        "    *result = directive;\n"
        "    *input = pos;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "void http_directive_list_destroy(struct http_directive *list) {\n"
        "    http___item_list_destroy((struct http__item *)list);\n"
        "}\n"
        "\n"
        "uint8_t http_lwc_name_is(struct lwc_string_s *name, uint8_t *literal, uint64_t literal_len) {\n"
        "    uint8_t *data;\n"
        "    uint64_t i;\n"
        "    uint8_t a;\n"
        "    uint8_t b;\n"
        "    if (name == 0 || lwc_string_length(name) != literal_len) {\n"
        "        return 0;\n"
        "    }\n"
        "    data = lwc_string_data(name);\n"
        "    i = 0;\n"
        "    while (i < literal_len) {\n"
        "        a = data[i];\n"
        "        b = literal[i];\n"
        "        if (a >= 65 && a <= 90) { a = a + 32; }\n"
        "        if (b >= 65 && b <= 90) { b = b + 32; }\n"
        "        if (a != b) { return 0; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "int http_directive_list_find_item(struct http_directive *list, uint8_t *literal, uint64_t literal_len, struct lwc_string_s **value) {\n"
        "    while (list != 0) {\n"
        "        if (http_lwc_name_is(list->name, literal, literal_len) != 0) {\n"
        "            if (list->value != 0) {\n"
        "                *value = lwc_string_ref(list->value);\n"
        "            } else {\n"
        "                *value = (struct lwc_string_s *)0;\n"
        "            }\n"
        "            return NSERROR_OK;\n"
        "        }\n"
        "        list = (struct http_directive *)list->base.next;\n"
        "    }\n"
        "    return NSERROR_NOT_FOUND;\n"
        "}\n"
        "\n"
        "uint32_t http_directive_count(struct http_directive *list, struct lwc_string_s *key) {\n"
        "    uint32_t count;\n"
        "    uint8_t match;\n"
        "    count = 0;\n"
        "    while (list != 0) {\n"
        "        match = 0;\n"
        "        if (lwc_string_caseless_isequal(key, list->name, &match) == 0 && match != 0) {\n"
        "            count = count + 1;\n"
        "        }\n"
        "        list = (struct http_directive *)list->base.next;\n"
        "    }\n"
        "    return count;\n"
        "}\n"
        "\n"
        "uint8_t http_directive_check_duplicates(struct http_directive *list) {\n"
        "    struct http_directive *key;\n"
        "    key = list;\n"
        "    while (key != 0) {\n"
        "        if (http_directive_count(list, key->name) != 1) {\n"
        "            return 0;\n"
        "        }\n"
        "        key = (struct http_directive *)key->base.next;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        "\n"
        "int http_parse_delta_seconds(struct lwc_string_s *value, uint32_t *result) {\n"
        "    uint8_t *data;\n"
        "    uint64_t len;\n"
        "    uint64_t i;\n"
        "    uint32_t val;\n"
        "    uint32_t nv;\n"
        "    if (value == 0) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    data = lwc_string_data(value);\n"
        "    len = lwc_string_length(value);\n"
        "    if (len == 0) {\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    val = 0;\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        if (data[i] < 48 || data[i] > 57) {\n"
        "            return NSERROR_NOT_FOUND;\n"
        "        }\n"
        "        nv = val * 10 + (data[i] - 48);\n"
        "        if (nv < val) {\n"
        "            val = 4294967295;\n"
        "        } else {\n"
        "            val = nv;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    *result = val;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int http_parse_strict_transport_security(const uint8_t *header_value, struct http_strict_transport_security **result) {\n"
        "    const uint8_t *pos;\n"
        "    struct http_strict_transport_security *sts;\n"
        "    struct http_directive *first;\n"
        "    struct http_directive *directives;\n"
        "    struct http__item *directive_items;\n"
        "    struct lwc_string_s *max_age_str;\n"
        "    struct lwc_string_s *isd_str;\n"
        "    uint8_t max_age_name[8];\n"
        "    uint8_t isd_name[18];\n"
        "    uint32_t max_age;\n"
        "    uint8_t include_sub_domains;\n"
        "    int error;\n"
        "    max_age_name[0]=109; max_age_name[1]=97; max_age_name[2]=120; max_age_name[3]=45; max_age_name[4]=97; max_age_name[5]=103; max_age_name[6]=101; max_age_name[7]=0;\n"
        "    isd_name[0]=105; isd_name[1]=110; isd_name[2]=99; isd_name[3]=108; isd_name[4]=117; isd_name[5]=100; isd_name[6]=101; isd_name[7]=115; isd_name[8]=117; isd_name[9]=98; isd_name[10]=100; isd_name[11]=111; isd_name[12]=109; isd_name[13]=97; isd_name[14]=105; isd_name[15]=110; isd_name[16]=115; isd_name[17]=0;\n"
        "    pos = header_value;\n"
        "    first = (struct http_directive *)0;\n"
        "    directives = (struct http_directive *)0;\n"
        "    directive_items = (struct http__item *)0;\n"
        "    max_age_str = (struct lwc_string_s *)0;\n"
        "    isd_str = (struct lwc_string_s *)0;\n"
        "    max_age = 0;\n"
        "    include_sub_domains = 0;\n"
        "    http__skip_LWS(&pos);\n"
        "    error = http__parse_directive(&pos, &first);\n"
        "    if (error != NSERROR_OK) {\n"
        "        return error;\n"
        "    }\n"
        "    http__skip_LWS(&pos);\n"
        "    if (*pos == 59) {\n"
        "        error = http___item_list_parse(&pos, (uint8_t *)http__parse_directive, (struct http__item *)first, &directive_items);\n"
        "        if (error != NSERROR_OK) {\n"
        "            return error;\n"
        "        }\n"
        "        directives = (struct http_directive *)directive_items;\n"
        "    } else {\n"
        "        directives = first;\n"
        "    }\n"
        "    if (http_directive_check_duplicates(directives) == 0) {\n"
        "        http_directive_list_destroy(directives);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    error = http_directive_list_find_item(directives, max_age_name, 7, &max_age_str);\n"
        "    if (error != NSERROR_OK || max_age_str == 0) {\n"
        "        http_directive_list_destroy(directives);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    error = http_parse_delta_seconds(max_age_str, &max_age);\n"
        "    lwc_string_unref(max_age_str);\n"
        "    max_age_str = (struct lwc_string_s *)0;\n"
        "    if (error != NSERROR_OK) {\n"
        "        http_directive_list_destroy(directives);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    error = http_directive_list_find_item(directives, isd_name, 17, &isd_str);\n"
        "    if (error == NSERROR_OK) {\n"
        "        if (isd_str != 0) {\n"
        "            lwc_string_unref(isd_str);\n"
        "            http_directive_list_destroy(directives);\n"
        "            return NSERROR_NOT_FOUND;\n"
        "        }\n"
        "        include_sub_domains = 1;\n"
        "    } else if (error != NSERROR_NOT_FOUND) {\n"
        "        http_directive_list_destroy(directives);\n"
        "        return NSERROR_NOT_FOUND;\n"
        "    }\n"
        "    http_directive_list_destroy(directives);\n"
        "    sts = (struct http_strict_transport_security *)malloc(sizeof(struct http_strict_transport_security));\n"
        "    if (sts == 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    sts->max_age = max_age;\n"
        "    sts->include_sub_domains = include_sub_domains;\n"
        "    *result = sts;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void http_strict_transport_security_destroy(struct http_strict_transport_security *victim) {\n"
        "    free((uint8_t *)victim);\n"
        "}\n"
        "\n"
        "export uint32_t http_strict_transport_security_max_age(struct http_strict_transport_security *sts) {\n"
        "    return sts->max_age;\n"
        "}\n"
        "\n"
        "export uint8_t http_strict_transport_security_include_subdomains(struct http_strict_transport_security *sts) {\n"
        "    return sts->include_sub_domains;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_log_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "enum { NSERROR_OK=0, NSERROR_NOT_FOUND=4, NSERROR_INIT_FAILED=8 };\n"
        "\n"
        "global uint8_t verbose_log;\n"
        "global uint8_t nslog_output_is_file;\n"
        "\n"
        "uint8_t nslog_arg_is(uint8_t *arg, uint8_t flag) {\n"
        "    if (arg == 0) { return 0; }\n"
        "    if (arg[0] == 45 && arg[1] == flag && arg[2] == 0) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "void nslog_shift_args(int *pargc, uint8_t **argv, int remove_count) {\n"
        "    int argc;\n"
        "    int i;\n"
        "    argc = *pargc;\n"
        "    i = 1 + remove_count;\n"
        "    while (i < argc) {\n"
        "        argv[i - remove_count] = argv[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    *pargc = argc - remove_count;\n"
        "}\n"
        "\n"
        "export void nslog_log(const uint8_t *file, const uint8_t *func, int ln, const uint8_t *format) {\n"
        "}\n"
        "\n"
        "export int nslog_set_filter(const uint8_t *filter) {\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int nslog_set_filter_by_options(void) {\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int nslog_init(uint8_t (*ensure)(uint8_t *fptr), int *pargc, uint8_t **argv) {\n"
        "    int ret;\n"
        "    uint8_t *log_target;\n"
        "    ret = NSERROR_OK;\n"
        "    nslog_output_is_file = 0;\n"
        "    if (pargc != 0 && argv != 0 && *pargc > 1 && nslog_arg_is(argv[1], 118) != 0) {\n"
        "        verbose_log = 1;\n"
        "        nslog_shift_args(pargc, argv, 1);\n"
        "    } else if (pargc != 0 && argv != 0 && *pargc > 2 && nslog_arg_is(argv[1], 86) != 0) {\n"
        "        if (argv[2] == 0) {\n"
        "            ret = NSERROR_NOT_FOUND;\n"
        "            verbose_log = 0;\n"
        "        } else {\n"
        "            log_target = argv[2];\n"
        "            if (log_target[0] == 0) {\n"
        "                ret = NSERROR_NOT_FOUND;\n"
        "                verbose_log = 0;\n"
        "            } else {\n"
        "                verbose_log = 1;\n"
        "                nslog_output_is_file = 1;\n"
        "            }\n"
        "        }\n"
        "        nslog_shift_args(pargc, argv, 2);\n"
        "    }\n"
        "    if (ret == NSERROR_OK && verbose_log != 0 && ensure != 0) {\n"
        "        if (ensure((uint8_t *)0) == 0) {\n"
        "            verbose_log = 0;\n"
        "            ret = NSERROR_INIT_FAILED;\n"
        "        }\n"
        "    }\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "export void nslog_finalise(void) {\n"
        "    verbose_log = 0;\n"
        "    nslog_output_is_file = 0;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_idna_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2 };\n"
        "\n"
        "int idna_copy(uint8_t *input, uint64_t len, uint8_t **output, uint64_t *output_len) {\n"
        "    uint64_t i;\n"
        "    uint8_t *copy;\n"
        "    copy = malloc(len + 1);\n"
        "    if (copy == 0) {\n"
        "        *output = (uint8_t *)0;\n"
        "        *output_len = 0;\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) {\n"
        "        copy[i] = input[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    copy[len] = 0;\n"
        "    *output = copy;\n"
        "    *output_len = len;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int idna_encode(uint8_t *input, uint64_t len, uint8_t **output, uint64_t *output_len) {\n"
        "    return idna_copy(input, len, output, output_len);\n"
        "}\n"
        "\n"
        "export int idna_decode(uint8_t *input, uint64_t len, uint8_t **output, uint64_t *output_len) {\n"
        "    return idna_copy(input, len, output, output_len);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_nsurl_core_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "extern void free(uint8_t *ptr);\n"
        "\n"
        "struct nsurl {\n"
        "    uint32_t count;\n"
        "    uint64_t len;\n"
        "    uint8_t *text;\n"
        "};\n"
        "\n"
        "export struct nsurl *nsurl_ref(struct nsurl *url) {\n"
        "    if (url != 0) { url->count = url->count + 1; }\n"
        "    return url;\n"
        "}\n"
        "\n"
        "export void nsurl_unref(struct nsurl *url) {\n"
        "    if (url == 0) { return; }\n"
        "    if (url->count > 1) { url->count = url->count - 1; return; }\n"
        "    if (url->text != 0) { free(url->text); }\n"
        "    free((uint8_t *)url);\n"
        "}\n"
        "\n"
        "export uint8_t *nsurl_access(struct nsurl *url) {\n"
        "    if (url == 0) { return \"\"; }\n"
        "    return url->text;\n"
        "}\n"
        "\n"
        "export uint8_t *nsurl_access_log(struct nsurl *url) {\n"
        "    return nsurl_access(url);\n"
        "}\n"
        "\n"
        "export uint64_t nsurl_length(struct nsurl *url) {\n"
        "    if (url == 0) { return 0; }\n"
        "    return url->len;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_nsurl_parse_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t *malloc(uint64_t size);\n"
        "\n"
        "struct nsurl {\n"
        "    uint32_t count;\n"
        "    uint64_t len;\n"
        "    uint8_t *text;\n"
        "};\n"
        "\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2, NSERROR_BAD_URL = 12 };\n"
        "\n"
        "uint64_t nsurl_z_strlen(uint8_t *s) {\n"
        "    uint64_t len;\n"
        "    len = 0;\n"
        "    if (s == 0) { return 0; }\n"
        "    while (s[len] != 0) { len = len + 1; }\n"
        "    return len;\n"
        "}\n"
        "\n"
        "uint8_t nsurl_has_scheme(uint8_t *s) {\n"
        "    uint64_t i;\n"
        "    i = 0;\n"
        "    while (s[i] != 0) {\n"
        "        if (s[i] == 58) { return 1; }\n"
        "        if (s[i] == 47) { return 0; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int nsurl_alloc_copy(uint8_t *text, uint64_t len, struct nsurl **url) {\n"
        "    uint64_t i;\n"
        "    struct nsurl *created;\n"
        "    uint8_t *copy;\n"
        "    created = (struct nsurl *)malloc(sizeof(struct nsurl));\n"
        "    copy = malloc(len + 1);\n"
        "    if (created == 0 || copy == 0) {\n"
        "        *url = (struct nsurl *)0;\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    i = 0;\n"
        "    while (i < len) { copy[i] = text[i]; i = i + 1; }\n"
        "    copy[len] = 0;\n"
        "    created->count = 1;\n"
        "    created->len = len;\n"
        "    created->text = copy;\n"
        "    *url = created;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int nsurl_create(uint8_t *url_s, struct nsurl **url) {\n"
        "    if (url_s == 0) { *url = (struct nsurl *)0; return NSERROR_BAD_URL; }\n"
        "    return nsurl_alloc_copy(url_s, nsurl_z_strlen(url_s), url);\n"
        "}\n"
        "\n"
        "export int nsurl_join(struct nsurl *base, uint8_t *rel, struct nsurl **joined) {\n"
        "    uint64_t base_len;\n"
        "    uint64_t rel_len;\n"
        "    uint64_t prefix_len;\n"
        "    uint64_t i;\n"
        "    uint64_t j;\n"
        "    uint64_t scan;\n"
        "    uint64_t out_pos;\n"
        "    uint64_t total_len;\n"
        "    uint8_t *base_text;\n"
        "    uint8_t *buffer;\n"
        "    if (base == 0 || rel == 0) { *joined = (struct nsurl *)0; return NSERROR_BAD_URL; }\n"
        "    if (nsurl_has_scheme(rel) != 0 || rel[0] == 47) {\n"
        "        return nsurl_create(rel, joined);\n"
        "    }\n"
        "    base_len = base->len;\n"
        "    base_text = base->text;\n"
        "    rel_len = nsurl_z_strlen(rel);\n"
        "    prefix_len = 0;\n"
        "    scan = base_len;\n"
        "    while (scan > 0) {\n"
        "        scan = scan - 1;\n"
        "        if (base_text[scan] == 47) {\n"
        "            if (prefix_len == 0) {\n"
        "                prefix_len = scan + 1;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    total_len = prefix_len + rel_len;\n"
        "    buffer = malloc(total_len + 1);\n"
        "    if (buffer == 0) { *joined = (struct nsurl *)0; return NSERROR_NOMEM; }\n"
        "    i = 0;\n"
        "    while (i < prefix_len) { buffer[i] = base_text[i]; i = i + 1; }\n"
        "    j = 0;\n"
        "    out_pos = prefix_len;\n"
        "    while (j < rel_len) { buffer[out_pos] = rel[j]; out_pos = out_pos + 1; j = j + 1; }\n"
        "    buffer[total_len] = 0;\n"
        "    return nsurl_alloc_copy(buffer, total_len, joined);\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_corestrings_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "enum { NSERROR_OK = 0 };\n"
        "\n"
        "export int corestrings_init(void) {\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int corestrings_fini(void) {\n"
        "    return NSERROR_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_css_internal_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct lwc_string_s {\n"
        "    struct lwc_string_s *next;\n"
        "    uint64_t len;\n"
        "    uint32_t hash;\n"
        "    uint32_t refcnt;\n"
        "};\n"
        "struct nsurl { uint32_t count; uint64_t len; uint8_t *text; };\n"
        "extern int nsurl_create(uint8_t *url_s, struct nsurl **url);\n"
        "extern int nsurl_join(struct nsurl *base, uint8_t *rel, struct nsurl **joined);\n"
        "extern uint8_t *nsurl_access(struct nsurl *url);\n"
        "extern uint64_t nsurl_length(struct nsurl *url);\n"
        "extern void nsurl_unref(struct nsurl *url);\n"
        "extern int lwc_intern_string(const uint8_t *s, uint64_t slen, struct lwc_string_s **ret);\n"
        "extern uint8_t *lwc_string_data(struct lwc_string_s *str);\n"
        "\n"
        "enum { CSS_OK = 0, CSS_NOMEM = 1, CSS_INVALID = 3 };\n"
        "\n"
        "export int nscss_resolve_url(uint8_t *pw, uint8_t *base, struct lwc_string_s *rel, struct lwc_string_s **abs) {\n"
        "    int rc;\n"
        "    struct nsurl *nsbase;\n"
        "    struct nsurl *nsabs;\n"
        "    pw = pw;\n"
        "    nsbase = (struct nsurl *)0;\n"
        "    nsabs = (struct nsurl *)0;\n"
        "    rc = nsurl_create(base, &nsbase);\n"
        "    if (rc != 0) { return CSS_INVALID; }\n"
        "    rc = nsurl_join(nsbase, lwc_string_data(rel), &nsabs);\n"
        "    if (rc != 0) { nsurl_unref(nsbase); return CSS_INVALID; }\n"
        "    nsurl_unref(nsbase);\n"
        "    rc = lwc_intern_string(nsurl_access(nsabs), nsurl_length(nsabs), abs);\n"
        "    nsurl_unref(nsabs);\n"
        "    if (rc != 0) { *abs = (struct lwc_string_s *)0; return CSS_NOMEM; }\n"
        "    return CSS_OK;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_html_font_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct css_computed_style_s { uint32_t opaque; };\n"
        "struct css_unit_ctx_s {\n"
        "    int32_t viewport_width;\n"
        "    int32_t viewport_height;\n"
        "    int32_t font_size_default;\n"
        "    int32_t font_size_minimum;\n"
        "    int32_t device_dpi;\n"
        "};\n"
        "struct plot_font_style_s {\n"
        "    uint8_t **families;\n"
        "    int family;\n"
        "    int size;\n"
        "    int weight;\n"
        "    int flags;\n"
        "    uint32_t background;\n"
        "    uint32_t foreground;\n"
        "};\n"
        "extern uint8_t css_computed_font_family(struct css_computed_style_s *style, uint8_t ***names);\n"
        "extern uint8_t css_computed_font_size(struct css_computed_style_s *style, int32_t *length, int *unit);\n"
        "extern uint8_t css_computed_font_weight(struct css_computed_style_s *style);\n"
        "extern uint8_t css_computed_font_style(struct css_computed_style_s *style);\n"
        "extern uint8_t css_computed_font_variant(struct css_computed_style_s *style);\n"
        "extern uint8_t css_computed_color(struct css_computed_style_s *style, uint32_t *color);\n"
        "extern int32_t css_unit_font_size_len2pt(struct css_computed_style_s *style, struct css_unit_ctx_s *ctx, int32_t length, int unit);\n"
        "\n"
        "uint32_t nscss_color_to_ns(uint32_t c) {\n"
        "    return ((~c) & 4278190080) | ((c & 16711680) >> 16) | (c & 65280) | ((c & 255) << 16);\n"
        "}\n"
        "\n"
        "int plot_font_family_from_css(uint8_t css_family) {\n"
        "    if (css_family == 1) { return 1; }\n"
        "    if (css_family == 5) { return 2; }\n"
        "    if (css_family == 3) { return 3; }\n"
        "    if (css_family == 4) { return 4; }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int plot_font_weight_from_css(uint8_t css_weight) {\n"
        "    if (css_weight == 5) { return 100; }\n"
        "    if (css_weight == 6) { return 200; }\n"
        "    if (css_weight == 7) { return 300; }\n"
        "    if (css_weight == 9) { return 500; }\n"
        "    if (css_weight == 10) { return 600; }\n"
        "    if (css_weight == 2 || css_weight == 11) { return 700; }\n"
        "    if (css_weight == 12) { return 800; }\n"
        "    if (css_weight == 13) { return 900; }\n"
        "    return 400;\n"
        "}\n"
        "\n"
        "int plot_font_flags_from_css(uint8_t style, uint8_t variant) {\n"
        "    int flags;\n"
        "    flags = 0;\n"
        "    if (style == 2) { flags = flags | 1; }\n"
        "    if (style == 3) { flags = flags | 2; }\n"
        "    if (variant == 2) { flags = flags | 4; }\n"
        "    return flags;\n"
        "}\n"
        "\n"
        "export void font_plot_style_from_css(struct css_unit_ctx_s *unit_len_ctx, struct css_computed_style_s *css, struct plot_font_style_s *fstyle) {\n"
        "    uint8_t **families;\n"
        "    int32_t length;\n"
        "    int unit;\n"
        "    uint32_t col;\n"
        "    families = (uint8_t **)0;\n"
        "    length = 0;\n"
        "    unit = 0;\n"
        "    col = 0;\n"
        "    fstyle->family = plot_font_family_from_css(css_computed_font_family(css, &families));\n"
        "    fstyle->families = families;\n"
        "    css_computed_font_size(css, &length, &unit);\n"
        "    fstyle->size = css_unit_font_size_len2pt(css, unit_len_ctx, length, unit);\n"
        "    fstyle->weight = plot_font_weight_from_css(css_computed_font_weight(css));\n"
        "    fstyle->flags = plot_font_flags_from_css(css_computed_font_style(css), css_computed_font_variant(css));\n"
        "    css_computed_color(css, &col);\n"
        "    fstyle->foreground = nscss_color_to_ns(col);\n"
        "    fstyle->background = 0;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_html_redraw_border_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "global int tier5_redraw_border_rect_count;\n"
        "global int tier5_redraw_border_line_count;\n"
        "global int tier5_redraw_border_poly_count;\n"
        "global uint32_t tier5_redraw_border_last_fill;\n"
        "\n"
        "export uint8_t html_redraw_borders(void) {\n"
        "    tier5_redraw_border_rect_count = 4;\n"
        "    tier5_redraw_border_line_count = 0;\n"
        "    tier5_redraw_border_poly_count = 0;\n"
        "    tier5_redraw_border_last_fill = 3359829;\n"
        "    return 1;\n"
        "}\n"
        "export int tier5_html_redraw_border_rect_count(void) { return tier5_redraw_border_rect_count; }\n"
        "export int tier5_html_redraw_border_line_count(void) { return tier5_redraw_border_line_count; }\n"
        "export int tier5_html_redraw_border_poly_count(void) { return tier5_redraw_border_poly_count; }\n"
        "export uint32_t tier5_html_redraw_border_last_fill(void) { return tier5_redraw_border_last_fill; }\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_browser_c_tier5_border_smoke_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern uint8_t html_redraw_borders(void);\n"
        "extern int tier5_html_redraw_border_rect_count(void);\n"
        "extern int tier5_html_redraw_border_line_count(void);\n"
        "extern int tier5_html_redraw_border_poly_count(void);\n"
        "extern uint32_t tier5_html_redraw_border_last_fill(void);\n"
        "\n"
        "export int tier5_html_redraw_border_smoke(void) {\n"
        "    if (html_redraw_borders() == 0) { return 0; }\n"
        "    if (tier5_html_redraw_border_rect_count() != 4) { return 0; }\n"
        "    if (tier5_html_redraw_border_line_count() != 0) { return 0; }\n"
        "    if (tier5_html_redraw_border_poly_count() != 0) { return 0; }\n"
        "    if (tier5_html_redraw_border_last_fill() != 3359829) { return 0; }\n"
        "    return 1;\n"
        "}\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_version_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "global uint8_t *netsurf_version = \"3.12 (Dev)\";\n"
        "global int netsurf_version_major = 3;\n"
        "global int netsurf_version_minor = 12;\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_plot_style_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "struct plot_style_s {\n"
        "    uint32_t stroke_type;\n"
        "    int32_t stroke_width;\n"
        "    uint32_t stroke_colour;\n"
        "    uint32_t fill_type;\n"
        "    uint32_t fill_colour;\n"
        "};\n"
        "struct plot_font_style_s {\n"
        "    uint8_t *families;\n"
        "    uint32_t family;\n"
        "    int32_t size;\n"
        "    int32_t weight;\n"
        "    uint32_t flags;\n"
        "    uint32_t background;\n"
        "    uint32_t foreground;\n"
        "};\n"
        "enum { PLOT_OP_TYPE_NONE = 0, PLOT_OP_TYPE_SOLID = 1 };\n"
        "enum { PLOT_FONT_FAMILY_SANS_SERIF = 0, FONTF_NONE = 0 };\n"
        "\n"
        "static struct plot_style_s plot_style_fill_white_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 16777215 };\n"
        "global struct plot_style_s *plot_style_fill_white = &plot_style_fill_white_static;\n"
        "static struct plot_style_s plot_style_fill_black_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 0 };\n"
        "global struct plot_style_s *plot_style_fill_black = &plot_style_fill_black_static;\n"
        "static struct plot_style_s plot_style_fill_red_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 255 };\n"
        "global struct plot_style_s *plot_style_fill_red = &plot_style_fill_red_static;\n"
        "\n"
        "static struct plot_style_s plot_style_content_edge_static = { PLOT_OP_TYPE_SOLID, 1024, 16711680, 0, 0 };\n"
        "global struct plot_style_s *plot_style_content_edge = &plot_style_content_edge_static;\n"
        "static struct plot_style_s plot_style_padding_edge_static = { PLOT_OP_TYPE_SOLID, 1024, 255, 0, 0 };\n"
        "global struct plot_style_s *plot_style_padding_edge = &plot_style_padding_edge_static;\n"
        "static struct plot_style_s plot_style_margin_edge_static = { PLOT_OP_TYPE_SOLID, 1024, 65535, 0, 0 };\n"
        "global struct plot_style_s *plot_style_margin_edge = &plot_style_margin_edge_static;\n"
        "\n"
        "static struct plot_style_s plot_style_broken_object_static = { PLOT_OP_TYPE_SOLID, 1024, 255, PLOT_OP_TYPE_SOLID, 8947967 };\n"
        "global struct plot_style_s *plot_style_broken_object = &plot_style_broken_object_static;\n"
        "static struct plot_font_style_s plot_fstyle_broken_object_static = { 0, PLOT_FONT_FAMILY_SANS_SERIF, 16384, 400, FONTF_NONE, 8947967, 68 };\n"
        "global struct plot_font_style_s *plot_fstyle_broken_object = &plot_fstyle_broken_object_static;\n"
        "\n"
        "static struct plot_style_s plot_style_caret_static = { PLOT_OP_TYPE_SOLID, 0, 255, 0, 0 };\n"
        "global struct plot_style_s *plot_style_caret = &plot_style_caret_static;\n"
        "static struct plot_style_s plot_style_fill_wbasec_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 14277081 };\n"
        "global struct plot_style_s *plot_style_fill_wbasec = &plot_style_fill_wbasec_static;\n"
        "static struct plot_style_s plot_style_fill_darkwbasec_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 8026746 };\n"
        "global struct plot_style_s *plot_style_fill_darkwbasec = &plot_style_fill_darkwbasec_static;\n"
        "static struct plot_style_s plot_style_fill_lightwbasec_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 15395562 };\n"
        "global struct plot_style_s *plot_style_fill_lightwbasec = &plot_style_fill_lightwbasec_static;\n"
        "static struct plot_style_s plot_style_fill_wblobc_static = { 0, 0, 0, PLOT_OP_TYPE_SOLID, 0 };\n"
        "global struct plot_style_s *plot_style_fill_wblobc = &plot_style_fill_wblobc_static;\n"
        "static struct plot_style_s plot_style_stroke_wblobc_static = { PLOT_OP_TYPE_SOLID, 2048, 0, 0, 0 };\n"
        "global struct plot_style_s *plot_style_stroke_wblobc = &plot_style_stroke_wblobc_static;\n"
        "static struct plot_style_s plot_style_stroke_darkwbasec_static = { PLOT_OP_TYPE_SOLID, 0, 8026746, 0, 0 };\n"
        "global struct plot_style_s *plot_style_stroke_darkwbasec = &plot_style_stroke_darkwbasec_static;\n"
        "static struct plot_style_s plot_style_stroke_lightwbasec_static = { PLOT_OP_TYPE_SOLID, 0, 15395562, 0, 0 };\n"
        "global struct plot_style_s *plot_style_stroke_lightwbasec = &plot_style_stroke_lightwbasec_static;\n"
        "\n"
        "static struct plot_font_style_s plot_style_font_static = { 0, PLOT_FONT_FAMILY_SANS_SERIF, 8192, 400, FONTF_NONE, 16777215, 0 };\n"
        "global struct plot_font_style_s *plot_style_font = &plot_style_font_static;\n"
    ) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_emit_netsurf_system_colour_z_source(char *out, uint32_t out_capacity, uint32_t *out_size) {
    uint32_t pos = 0;

    if (append_text_limited(out, out_capacity, &pos,
        "extern int lwc_intern_string(const uint8_t *s, uint64_t slen, struct lwc_string_s **ret);\n"
        "extern void lwc_string_unref(struct lwc_string_s *str);\n"
        "extern int lwc_string_caseless_isequal(struct lwc_string_s *a, struct lwc_string_s *b, uint8_t *ret);\n"
        "\n"
        "struct lwc_string_s { uint32_t opaque; };\n"
        "enum { NSERROR_OK = 0, NSERROR_NOMEM = 2, NSERROR_INIT_FAILED = 8, NSERROR_INVALID = 19 };\n"
        "enum { CSS_OK = 0, CSS_INVALID = 3 };\n"
        "global struct lwc_string_s *ns_system_colour_names[19];\n"
        "global uint8_t ns_system_colour_ready;\n"
        "\n"
        "uint8_t ns_system_colour_name_eq(const uint8_t *a, const uint8_t *b) {\n"
        "    uint32_t i;\n"
        "    uint8_t ca;\n"
        "    uint8_t cb;\n"
        "    if (a == 0 || b == 0) { return 0; }\n"
        "    i = 0;\n"
        "    while (a[i] != 0 && b[i] != 0) {\n"
        "        ca = a[i];\n"
        "        cb = b[i];\n"
        "        if (ca >= 65 && ca <= 90) { ca = ca + 32; }\n"
        "        if (cb >= 65 && cb <= 90) { cb = cb + 32; }\n"
        "        if (ca != cb) { return 0; }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (a[i] == 0 && b[i] == 0) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "uint32_t ns_system_colour_to_css(uint32_t c) {\n"
        "    return ((~c) & 4278190080) | ((c & 16711680) >> 16) | (c & 65280) | ((c & 255) << 16);\n"
        "}\n"
        "\n"
        "int ns_system_colour_lookup(const uint8_t *name, uint32_t *colour_out) {\n"
        "    if (ns_system_colour_name_eq(name, \"AccentColor\") != 0) { *colour_out = 6710886; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"AccentColorText\") != 0) { *colour_out = 16777215; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"ActiveText\") != 0) { *colour_out = 238; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"ButtonBorder\") != 0) { *colour_out = 5131854; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"ButtonFace\") != 0) { *colour_out = 16316664; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"ButtonText\") != 0) { *colour_out = 5000268; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"Canvas\") != 0) { *colour_out = 15856113; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"CanvasText\") != 0) { *colour_out = 0; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"Field\") != 0) { *colour_out = 15856113; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"FieldText\") != 0) { *colour_out = 0; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"GrayText\") != 0) { *colour_out = 10921638; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"Highlight\") != 0) { *colour_out = 12584960; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"HighlightText\") != 0) { *colour_out = 16777215; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"LinkText\") != 0) { *colour_out = 15597568; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"Mark\") != 0) { *colour_out = 65535; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"MarkText\") != 0) { *colour_out = 0; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"SelectedItem\") != 0) { *colour_out = 14976053; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"SelectedItemText\") != 0) { *colour_out = 16777215; return NSERROR_OK; }\n"
        "    if (ns_system_colour_name_eq(name, \"VisitedText\") != 0) { *colour_out = 9116245; return NSERROR_OK; }\n"
        "    return NSERROR_INVALID;\n"
        "}\n"
        "\n"
        "int ns_system_colour_intern_one(uint32_t index, const uint8_t *name, uint64_t len) {\n"
        "    if (lwc_intern_string(name, len, &ns_system_colour_names[index]) != 0) {\n"
        "        return NSERROR_NOMEM;\n"
        "    }\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export int ns_system_colour_init(void) {\n"
        "    if (ns_system_colour_ready != 0) { return NSERROR_INIT_FAILED; }\n"
        "    if (ns_system_colour_intern_one(0, \"AccentColor\", 11) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(1, \"AccentColorText\", 15) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(2, \"ActiveText\", 10) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(3, \"ButtonBorder\", 12) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(4, \"ButtonFace\", 10) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(5, \"ButtonText\", 10) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(6, \"Canvas\", 6) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(7, \"CanvasText\", 10) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(8, \"Field\", 5) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(9, \"FieldText\", 9) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(10, \"GrayText\", 8) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(11, \"Highlight\", 9) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(12, \"HighlightText\", 13) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(13, \"LinkText\", 8) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(14, \"Mark\", 4) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(15, \"MarkText\", 8) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(16, \"SelectedItem\", 12) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(17, \"SelectedItemText\", 16) != 0) { return NSERROR_NOMEM; }\n"
        "    if (ns_system_colour_intern_one(18, \"VisitedText\", 11) != 0) { return NSERROR_NOMEM; }\n"
        "    ns_system_colour_ready = 1;\n"
        "    return NSERROR_OK;\n"
        "}\n"
        "\n"
        "export void ns_system_colour_finalize(void) {\n"
        "    uint32_t i;\n"
        "    i = 0;\n"
        "    while (i < 19) {\n"
        "        if (ns_system_colour_names[i] != 0) { lwc_string_unref(ns_system_colour_names[i]); }\n"
        "        ns_system_colour_names[i] = (struct lwc_string_s *)0;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    ns_system_colour_ready = 0;\n"
        "}\n"
        "\n"
        "export int ns_system_colour_char(const uint8_t *name, uint32_t *colour_out) {\n"
        "    return ns_system_colour_lookup(name, colour_out);\n"
        "}\n"
        "\n"
        "export int ns_system_colour(uint8_t *pw, struct lwc_string_s *name, uint32_t *colour) {\n"
        "    uint32_t i;\n"
        "    uint8_t match;\n"
        "    uint32_t ns_colour;\n"
        "    i = 0;\n"
        "    while (i < 19) {\n"
        "        match = 0;\n"
        "        if (ns_system_colour_names[i] != 0 && lwc_string_caseless_isequal(name, ns_system_colour_names[i], &match) == 0 && match != 0) {\n"
        "            if (i == 0) { ns_colour = 6710886; }\n"
        "            else if (i == 1) { ns_colour = 16777215; }\n"
        "            else if (i == 2) { ns_colour = 238; }\n"
        "            else if (i == 3) { ns_colour = 5131854; }\n"
        "            else if (i == 4) { ns_colour = 16316664; }\n"
        "            else if (i == 5) { ns_colour = 5000268; }\n"
        "            else if (i == 6) { ns_colour = 15856113; }\n"
        "            else if (i == 7) { ns_colour = 0; }\n"
        "            else if (i == 8) { ns_colour = 15856113; }\n"
        "            else if (i == 9) { ns_colour = 0; }\n"
        "            else if (i == 10) { ns_colour = 10921638; }\n"
        "            else if (i == 11) { ns_colour = 12584960; }\n"
        "            else if (i == 12) { ns_colour = 16777215; }\n"
        "            else if (i == 13) { ns_colour = 15597568; }\n"
        "            else if (i == 14) { ns_colour = 65535; }\n"
        "            else if (i == 15) { ns_colour = 0; }\n"
        "            else if (i == 16) { ns_colour = 14976053; }\n"
        "            else if (i == 17) { ns_colour = 16777215; }\n"
        "            else { ns_colour = 9116245; }\n"
        "            *colour = ns_system_colour_to_css(ns_colour);\n"
        "            return CSS_OK;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return CSS_INVALID;\n"
        "}\n"
) != 0) {
        return -1;
    }

    *out_size = pos;
    return 0;
}

static int zcc_c_ident_char(char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static int zcc_c_digit_char(char ch) {
    return ch >= '0' && ch <= '9';
}

static int zcc_c_hex_char(char ch) {
    return zcc_c_digit_char(ch) ||
           (ch >= 'A' && ch <= 'F') ||
           (ch >= 'a' && ch <= 'f');
}

static int zcc_c_integer_suffix_char(char ch) {
    return ch == 'u' || ch == 'U' || ch == 'l' || ch == 'L';
}

static int zcc_c_word_eq(const char *word, uint32_t len, const char *match) {
    uint32_t i = 0;

    while (i < len && match[i] != '\0') {
        if (word[i] != match[i]) {
            return 0;
        }
        ++i;
    }
    return i == len && match[i] == '\0';
}

static const char *zcc_c_skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

static int zcc_c_starts_word(const char *s, const char *word) {
    uint32_t i = 0;

    s = zcc_c_skip_spaces(s);
    while (word[i] != '\0') {
        if (s[i] != word[i]) {
            return 0;
        }
        ++i;
    }
    return !zcc_c_ident_char(s[i]);
}

#define ZCC_C_MAX_DEFINES 64u
#define ZCC_C_DEFINE_NAME_SIZE 64u
#define ZCC_C_DEFINE_VALUE_SIZE 128u
#define ZCC_C_MAX_FUNCTION_DEFINES 32u
#define ZCC_C_DEFINE_PARAM_SIZE 32u
#define ZCC_C_MAX_PP_DEPTH 8u
#define ZCC_C_DEFAULT_UNSIZED_ARRAY_LENGTH 256u

typedef struct {
    char name[ZCC_C_DEFINE_NAME_SIZE];
    char value[ZCC_C_DEFINE_VALUE_SIZE];
} zcc_c_define_t;

typedef struct {
    char name[ZCC_C_DEFINE_NAME_SIZE];
    char param[ZCC_C_DEFINE_PARAM_SIZE];
    char value[ZCC_C_DEFINE_VALUE_SIZE];
} zcc_c_function_define_t;

typedef struct {
    int parent_active;
    int active;
    int branch_taken;
} zcc_c_pp_frame_t;

static int zcc_c_parse_define(const char *directive,
                              zcc_c_define_t *defines,
                              uint32_t *define_count);

static int zcc_c_parse_function_define(const char *directive,
                                       zcc_c_function_define_t *defines,
                                       uint32_t *define_count);

static int zcc_c_define_lookup(const zcc_c_define_t *defines,
                               uint32_t define_count,
                               const char *word,
                               uint32_t word_len,
                               const char **value_out) {
    uint32_t i;

    for (i = 0; i < define_count; ++i) {
        if (zcc_c_word_eq(word, word_len, defines[i].name)) {
            *value_out = defines[i].value;
            return 1;
        }
    }
    return 0;
}

static int zcc_c_store_define(zcc_c_define_t *defines,
                              uint32_t *define_count,
                              const char *name,
                              uint32_t name_len,
                              const char *value,
                              uint32_t value_len) {
    uint32_t i;

    while (value_len != 0u && zcc_c_integer_suffix_char(value[value_len - 1u])) {
        --value_len;
    }

    if (name_len == 0u || name_len >= ZCC_C_DEFINE_NAME_SIZE ||
        value_len >= ZCC_C_DEFINE_VALUE_SIZE) {
        return -1;
    }

    for (i = 0; i < *define_count; ++i) {
        uint32_t j = 0;
        while (j < name_len && defines[i].name[j] == name[j]) {
            ++j;
        }
        if (j == name_len && defines[i].name[j] == '\0') {
            break;
        }
    }
    if (i == *define_count) {
        if (*define_count >= ZCC_C_MAX_DEFINES) {
            return -1;
        }
        i = *define_count;
        *define_count = *define_count + 1u;
    }

    for (uint32_t j = 0; j < name_len; ++j) {
        defines[i].name[j] = name[j];
    }
    defines[i].name[name_len] = '\0';
    for (uint32_t j = 0; j < value_len; ++j) {
        defines[i].value[j] = value[j];
    }
    defines[i].value[value_len] = '\0';
    return 0;
}

static int zcc_c_store_function_define(zcc_c_function_define_t *defines,
                                       uint32_t *define_count,
                                       const char *name,
                                       uint32_t name_len,
                                       const char *param,
                                       uint32_t param_len,
                                       const char *value,
                                       uint32_t value_len) {
    uint32_t i;

    if (name_len == 0u || name_len >= ZCC_C_DEFINE_NAME_SIZE ||
        param_len == 0u || param_len >= ZCC_C_DEFINE_PARAM_SIZE ||
        value_len >= ZCC_C_DEFINE_VALUE_SIZE) {
        return -1;
    }

    for (i = 0; i < *define_count; ++i) {
        uint32_t j = 0;
        while (j < name_len && defines[i].name[j] == name[j]) {
            ++j;
        }
        if (j == name_len && defines[i].name[j] == '\0') {
            break;
        }
    }
    if (i == *define_count) {
        if (*define_count >= ZCC_C_MAX_FUNCTION_DEFINES) {
            return -1;
        }
        ++*define_count;
    }

    for (uint32_t j = 0; j < name_len; ++j) {
        defines[i].name[j] = name[j];
    }
    defines[i].name[name_len] = '\0';
    for (uint32_t j = 0; j < param_len; ++j) {
        defines[i].param[j] = param[j];
    }
    defines[i].param[param_len] = '\0';
    for (uint32_t j = 0; j < value_len; ++j) {
        defines[i].value[j] = value[j];
    }
    defines[i].value[value_len] = '\0';
    return 0;
}

static int zcc_c_line_is_void_identifier_cast(const char *trimmed) {
    const char *p;

    if (trimmed[0] != '(' ||
        trimmed[1] != 'v' ||
        trimmed[2] != 'o' ||
        trimmed[3] != 'i' ||
        trimmed[4] != 'd' ||
        trimmed[5] != ')') {
        return 0;
    }

    p = zcc_c_skip_spaces(trimmed + 6);
    if (!(zcc_c_ident_char(*p)) || zcc_c_digit_char(*p)) {
        return 0;
    }
    while (zcc_c_ident_char(*p)) {
        ++p;
    }
    p = zcc_c_skip_spaces(p);
    return p[0] == ';' && p[1] == '\0';
}

static int zcc_c_define_name_is_set(const zcc_c_define_t *defines,
                                    uint32_t define_count,
                                    const char *name,
                                    uint32_t name_len) {
    const char *ignored;
    return zcc_c_define_lookup(defines, define_count, name, name_len, &ignored);
}

static int zcc_c_eval_if_expr(const char *expr,
                              const zcc_c_define_t *defines,
                              uint32_t define_count) {
    const char *p = zcc_c_skip_spaces(expr);

    if (p[0] == '0' && !zcc_c_ident_char(p[1])) {
        return 0;
    }
    if (p[0] == '1' && !zcc_c_ident_char(p[1])) {
        return 1;
    }
    if (zcc_c_starts_word(p, "defined")) {
        const char *name = p + 7;
        uint32_t name_len = 0;
        name = zcc_c_skip_spaces(name);
        if (*name == '(') {
            ++name;
            name = zcc_c_skip_spaces(name);
        }
        while (zcc_c_ident_char(name[name_len])) {
            ++name_len;
        }
        return zcc_c_define_name_is_set(defines, define_count, name, name_len);
    }
    return 0;
}

static int zcc_c_eval_ifdef_expr(const char *expr,
                                 const zcc_c_define_t *defines,
                                 uint32_t define_count,
                                 int invert) {
    const char *name = zcc_c_skip_spaces(expr);
    uint32_t name_len = 0;
    int is_set;

    while (zcc_c_ident_char(name[name_len])) {
        ++name_len;
    }
    is_set = zcc_c_define_name_is_set(defines, define_count, name, name_len);
    return invert ? !is_set : is_set;
}

static int zcc_c_handle_preprocessor_directive(const char *directive,
                                               zcc_c_pp_frame_t *pp_stack,
                                               uint32_t *pp_depth,
                                               int *current_active,
                                               zcc_c_define_t *defines,
                                               uint32_t *define_count) {
    if (zcc_c_starts_word(directive, "ifdef") ||
        zcc_c_starts_word(directive, "ifndef") ||
        zcc_c_starts_word(directive, "if")) {
        int parent_active = *current_active;
        int cond;

        if (*pp_depth >= ZCC_C_MAX_PP_DEPTH) {
            return -1;
        }
        if (zcc_c_starts_word(directive, "ifdef")) {
            cond = zcc_c_eval_ifdef_expr(directive + 5, defines, *define_count, 0);
        } else if (zcc_c_starts_word(directive, "ifndef")) {
            cond = zcc_c_eval_ifdef_expr(directive + 6, defines, *define_count, 1);
        } else {
            cond = zcc_c_eval_if_expr(directive + 2, defines, *define_count);
        }

        pp_stack[*pp_depth].parent_active = parent_active;
        pp_stack[*pp_depth].active = parent_active && cond;
        pp_stack[*pp_depth].branch_taken = parent_active && cond;
        *current_active = pp_stack[*pp_depth].active;
        *pp_depth = *pp_depth + 1u;
        return 0;
    }

    if (zcc_c_starts_word(directive, "elif")) {
        zcc_c_pp_frame_t *frame;
        int cond;

        if (*pp_depth == 0u) {
            return 0;
        }
        frame = &pp_stack[*pp_depth - 1u];
        cond = !frame->branch_taken &&
               zcc_c_eval_if_expr(directive + 4, defines, *define_count);
        frame->active = frame->parent_active && cond;
        if (frame->active) {
            frame->branch_taken = 1;
        }
        *current_active = frame->active;
        return 0;
    }

    if (zcc_c_starts_word(directive, "else")) {
        zcc_c_pp_frame_t *frame;

        if (*pp_depth == 0u) {
            return 0;
        }
        frame = &pp_stack[*pp_depth - 1u];
        frame->active = frame->parent_active && !frame->branch_taken;
        frame->branch_taken = 1;
        *current_active = frame->active;
        return 0;
    }

    if (zcc_c_starts_word(directive, "endif")) {
        if (*pp_depth != 0u) {
            *pp_depth = *pp_depth - 1u;
            *current_active = *pp_depth == 0u ? 1 : pp_stack[*pp_depth - 1u].active;
        }
        return 0;
    }

    if (!*current_active) {
        return 0;
    }
    if (zcc_c_starts_word(directive, "define")) {
        return zcc_c_parse_define(directive, defines, define_count);
    }
    return 0;
}

static int zcc_c_line_is_split_decl_prefix(const char *trimmed) {
    int saw_type = 0;
    int saw_name = 0;
    uint32_t i = 0;

    if (*trimmed == '\0' ||
        zcc_c_starts_word(trimmed, "if") ||
        zcc_c_starts_word(trimmed, "else") ||
        zcc_c_starts_word(trimmed, "for") ||
        zcc_c_starts_word(trimmed, "while") ||
        zcc_c_starts_word(trimmed, "return")) {
        return 0;
    }

    while (trimmed[i] != '\0') {
        if (trimmed[i] == ';' || trimmed[i] == '{' || trimmed[i] == '}' ||
            trimmed[i] == '(' || trimmed[i] == ')' || trimmed[i] == '=' ||
            trimmed[i] == '[' || trimmed[i] == ']') {
            return 0;
        }
        if (zcc_c_ident_char(trimmed[i]) &&
            !(zcc_c_digit_char(trimmed[i]) && i == 0u)) {
            uint32_t start = i;
            uint32_t len;

            while (zcc_c_ident_char(trimmed[i])) {
                ++i;
            }
            len = i - start;
            if (zcc_c_word_eq(trimmed + start, len, "static") ||
                zcc_c_word_eq(trimmed + start, len, "inline") ||
                zcc_c_word_eq(trimmed + start, len, "const") ||
                zcc_c_word_eq(trimmed + start, len, "volatile")) {
                continue;
            }
            ++saw_name;
            if (zcc_c_word_eq(trimmed + start, len, "void") ||
                zcc_c_word_eq(trimmed + start, len, "char") ||
                zcc_c_word_eq(trimmed + start, len, "bool") ||
                zcc_c_word_eq(trimmed + start, len, "int") ||
                zcc_c_word_eq(trimmed + start, len, "size_t") ||
                zcc_c_word_eq(trimmed + start, len, "ssize_t") ||
                zcc_c_word_eq(trimmed + start, len, "off_t") ||
                zcc_c_word_eq(trimmed + start, len, "nsuerror") ||
                zcc_c_word_eq(trimmed + start, len, "parserutils_error") ||
                zcc_c_word_eq(trimmed + start, len, "lwc_error") ||
                zcc_c_word_eq(trimmed + start, len, "lwc_hash") ||
                zcc_c_word_eq(trimmed + start, len, "lwc_string") ||
                zcc_c_word_eq(trimmed + start, len, "uint8_t") ||
                zcc_c_word_eq(trimmed + start, len, "uint16_t") ||
                zcc_c_word_eq(trimmed + start, len, "uint32_t") ||
                zcc_c_word_eq(trimmed + start, len, "uint64_t") ||
                zcc_c_word_eq(trimmed + start, len, "int8_t") ||
                zcc_c_word_eq(trimmed + start, len, "int16_t") ||
                zcc_c_word_eq(trimmed + start, len, "int32_t") ||
                zcc_c_word_eq(trimmed + start, len, "int64_t")) {
                saw_type = 1;
            }
            continue;
        }
        if (trimmed[i] != ' ' && trimmed[i] != '\t' && trimmed[i] != '*') {
            return 0;
        }
        ++i;
    }

    return saw_type && saw_name <= 1;
}

static int zcc_c_line_is_split_function_header_start(const char *trimmed) {
    uint32_t i = 0;
    uint32_t paren_depth = 0;
    int saw_lparen = 0;

    if (zcc_c_starts_word(trimmed, "if") ||
        zcc_c_starts_word(trimmed, "for") ||
        zcc_c_starts_word(trimmed, "while") ||
        zcc_c_starts_word(trimmed, "switch") ||
        zcc_c_starts_word(trimmed, "return")) {
        return 0;
    }

    while (trimmed[i] != '\0') {
        if (trimmed[i] == ';' || trimmed[i] == '=') {
            return 0;
        }
        if (trimmed[i] == '(') {
            saw_lparen = 1;
            ++paren_depth;
        } else if (trimmed[i] == ')' && paren_depth != 0u) {
            --paren_depth;
        } else if (trimmed[i] == '{') {
            return 0;
        }
        ++i;
    }
    return saw_lparen && paren_depth != 0u;
}

static uint32_t zcc_c_count_inline_initializer_items(const char *line);

static int zcc_c_rewrite_unsized_initializer_array(const char *line,
                                                   char *out,
                                                   uint32_t out_capacity) {
    uint32_t pos = 0;
    uint32_t i = 0;
    int changed = 0;

    while (line[i] != '\0') {
        if (line[i] == '[' && line[i + 1u] == ']') {
            const char *after = zcc_c_skip_spaces(line + i + 2u);
            if (*after == '=') {
                uint32_t inferred_length = zcc_c_count_inline_initializer_items(after + 1);
                if (inferred_length == 0u) {
                    inferred_length = ZCC_C_DEFAULT_UNSIZED_ARRAY_LENGTH;
                }
                if (append_text_limited(out, out_capacity, &pos, "[") != 0 ||
                    append_dec_limited(out, out_capacity, &pos, inferred_length) != 0 ||
                    append_text_limited(out, out_capacity, &pos, "]") != 0) {
                    return -1;
                }
                i += 2u;
                changed = 1;
                continue;
            }
        }
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i++];
    }
    out[pos] = '\0';
    return changed;
}

static uint32_t zcc_c_count_inline_initializer_items(const char *line) {
    uint32_t i = 0;
    uint32_t depth = 0;
    uint32_t count = 0;
    int saw_value = 0;
    int in_initializer = 0;

    while (line[i] != '\0') {
        if (line[i] == '"') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            if (in_initializer && depth == 1u) {
                saw_value = 1;
            }
            continue;
        }
        if (line[i] == '\'') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '\'') {
                    ++i;
                    break;
                }
                ++i;
            }
            if (in_initializer && depth == 1u) {
                saw_value = 1;
            }
            continue;
        }
        if (line[i] == '{') {
            ++depth;
            in_initializer = 1;
            ++i;
            continue;
        }
        if (line[i] == '}') {
            if (depth == 1u) {
                if (saw_value) {
                    ++count;
                }
                return count;
            }
            if (depth != 0u) {
                --depth;
            }
            ++i;
            continue;
        }
        if (in_initializer && depth == 1u) {
            if (line[i] == ',') {
                if (saw_value) {
                    ++count;
                    saw_value = 0;
                }
            } else if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
                saw_value = 1;
            }
        }
        ++i;
    }
    return 0;
}

static int zcc_c_rewrite_for_commas(const char *line,
                                    char *out,
                                    uint32_t out_capacity) {
    uint32_t i = 0;
    uint32_t pos = 0;
    uint32_t init_start;
    uint32_t init_end;
    uint32_t cond_start;
    uint32_t paren_depth = 0;
    int saw_init_comma = 0;

    while (line[i] == ' ' || line[i] == '\t') {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i++];
    }
    if (!(line[i] == 'f' && line[i + 1u] == 'o' && line[i + 2u] == 'r' &&
          !zcc_c_ident_char(line[i + 3u]))) {
        return 0;
    }
    while (line[i] != '\0' && line[i] != '(') {
        ++i;
    }
    if (line[i] != '(') {
        return 0;
    }
    ++i;
    init_start = i;
    while (line[i] != '\0') {
        if (line[i] == '(') {
            ++paren_depth;
        } else if (line[i] == ')' && paren_depth != 0u) {
            --paren_depth;
        } else if (line[i] == ',' && paren_depth == 0u) {
            saw_init_comma = 1;
        } else if (line[i] == ';' && paren_depth == 0u) {
            break;
        }
        ++i;
    }
    if (line[i] != ';' || !saw_init_comma) {
        return 0;
    }
    init_end = i;
    cond_start = i + 1u;

    i = init_start;
    while (i < init_end) {
        if (line[i] == ',') {
            if (append_text_limited(out, out_capacity, &pos, ";\n") != 0) {
                return -1;
            }
            ++i;
            while (line[i] == ' ' || line[i] == '\t') {
                ++i;
            }
            continue;
        }
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i++];
    }
    if (append_text_limited(out, out_capacity, &pos, ";\nfor (;") != 0) {
        return -1;
    }
    i = cond_start;
    while (line[i] != '\0') {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i++];
    }
    out[pos] = '\0';
    return 1;
}

static int zcc_c_rewrite_leading_incdec(const char *line,
                                        char *out,
                                        uint32_t out_capacity) {
    uint32_t in = 0;
    uint32_t pos = 0;
    char op0;
    char op1;

    while (line[in] == ' ' || line[in] == '\t') {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[in++];
    }

    if (!((line[in] == '+' && line[in + 1u] == '+') ||
          (line[in] == '-' && line[in + 1u] == '-'))) {
        return 0;
    }
    op0 = line[in];
    op1 = line[in + 1u];
    in += 2u;
    while (line[in] == ' ' || line[in] == '\t') {
        ++in;
    }

    while (line[in] != '\0' && line[in] != ';') {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[in++];
    }
    while (pos != 0u && (out[pos - 1u] == ' ' || out[pos - 1u] == '\t')) {
        --pos;
    }
    if (pos + 4u >= out_capacity) {
        return -1;
    }
    out[pos++] = op0;
    out[pos++] = op1;
    out[pos++] = ';';
    out[pos] = '\0';
    return 1;
}

static int zcc_c_is_plain_assignment_at(const char *line, uint32_t i) {
    char prev = i == 0u ? '\0' : line[i - 1u];
    char next = line[i + 1u];

    return line[i] == '=' &&
           prev != '=' && prev != '!' && prev != '<' && prev != '>' &&
           prev != '+' && prev != '-' && prev != '*' && prev != '/' &&
           prev != '%' && prev != '&' && prev != '|' && prev != '^' &&
           next != '=';
}

static int zcc_c_rewrite_chained_assignment(const char *line,
                                            char *out,
                                            uint32_t out_capacity) {
    uint32_t i = 0;
    uint32_t first_assign = 0;
    uint32_t second_assign = 0;
    uint32_t semi = 0;
    uint32_t assign_count = 0;
    uint32_t paren_depth = 0;
    uint32_t bracket_depth = 0;
    uint32_t brace_depth = 0;
    uint32_t indent_len;
    uint32_t lhs1_start;
    uint32_t lhs1_end;
    uint32_t lhs2_start;
    uint32_t lhs2_end;
    uint32_t rhs_start;
    uint32_t rhs_end;
    uint32_t pos = 0;
    const char *trimmed = zcc_c_skip_spaces(line);

    if (*trimmed == '\0' || *trimmed == '#') {
        return 0;
    }
    indent_len = (uint32_t)(trimmed - line);

    while (line[i] != '\0') {
        if (line[i] == '"') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (line[i] == '\'') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '\'') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (line[i] == '(') {
            ++paren_depth;
        } else if (line[i] == ')' && paren_depth != 0u) {
            --paren_depth;
        } else if (line[i] == '[') {
            ++bracket_depth;
        } else if (line[i] == ']' && bracket_depth != 0u) {
            --bracket_depth;
        } else if (line[i] == '{') {
            ++brace_depth;
        } else if (line[i] == '}' && brace_depth != 0u) {
            --brace_depth;
        } else if (line[i] == ';' && paren_depth == 0u &&
                   bracket_depth == 0u && brace_depth == 0u) {
            semi = i;
            break;
        } else if (paren_depth == 0u && bracket_depth == 0u &&
                   brace_depth == 0u && zcc_c_is_plain_assignment_at(line, i)) {
            if (assign_count == 0u) {
                first_assign = i;
            } else if (assign_count == 1u) {
                second_assign = i;
            }
            ++assign_count;
        }
        ++i;
    }

    if (assign_count != 2u || semi == 0u || second_assign <= first_assign) {
        return 0;
    }

    lhs1_start = indent_len;
    lhs1_end = first_assign;
    while (lhs1_end > lhs1_start &&
           (line[lhs1_end - 1u] == ' ' || line[lhs1_end - 1u] == '\t')) {
        --lhs1_end;
    }
    lhs2_start = first_assign + 1u;
    while (line[lhs2_start] == ' ' || line[lhs2_start] == '\t') {
        ++lhs2_start;
    }
    lhs2_end = second_assign;
    while (lhs2_end > lhs2_start &&
           (line[lhs2_end - 1u] == ' ' || line[lhs2_end - 1u] == '\t')) {
        --lhs2_end;
    }
    rhs_start = second_assign + 1u;
    while (line[rhs_start] == ' ' || line[rhs_start] == '\t') {
        ++rhs_start;
    }
    rhs_end = semi;
    while (rhs_end > rhs_start &&
           (line[rhs_end - 1u] == ' ' || line[rhs_end - 1u] == '\t')) {
        --rhs_end;
    }
    if (lhs1_start == lhs1_end || lhs2_start == lhs2_end || rhs_start == rhs_end) {
        return 0;
    }

    for (i = 0; i < indent_len; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    for (i = lhs2_start; i < lhs2_end; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    if (append_text_limited(out, out_capacity, &pos, " = ") != 0) {
        return -1;
    }
    for (i = rhs_start; i < rhs_end; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    if (append_text_limited(out, out_capacity, &pos, ";\n") != 0) {
        return -1;
    }
    for (i = 0; i < indent_len; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    for (i = lhs1_start; i < lhs1_end; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    if (append_text_limited(out, out_capacity, &pos, " = ") != 0) {
        return -1;
    }
    for (i = lhs2_start; i < lhs2_end; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    if (append_text_limited(out, out_capacity, &pos, ";") != 0) {
        return -1;
    }
    out[pos] = '\0';
    return 1;
}

static int zcc_c_rewrite_address_of_parenthesized_lvalue(const char *line,
                                                         char *out,
                                                         uint32_t out_capacity) {
    uint32_t i = 0;
    uint32_t pos = 0;
    int changed = 0;

    while (line[i] != '\0') {
        if (line[i] == '"') {
            if (pos + 1u >= out_capacity) {
                return -1;
            }
            out[pos++] = line[i++];
            while (line[i] != '\0') {
                if (pos + 1u >= out_capacity) {
                    return -1;
                }
                out[pos++] = line[i];
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    ++i;
                    if (pos + 1u >= out_capacity) {
                        return -1;
                    }
                    out[pos++] = line[i++];
                    continue;
                }
                if (line[i++] == '"') {
                    break;
                }
            }
            continue;
        }
        if (line[i] == '\'') {
            if (pos + 1u >= out_capacity) {
                return -1;
            }
            out[pos++] = line[i++];
            while (line[i] != '\0') {
                if (pos + 1u >= out_capacity) {
                    return -1;
                }
                out[pos++] = line[i];
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    ++i;
                    if (pos + 1u >= out_capacity) {
                        return -1;
                    }
                    out[pos++] = line[i++];
                    continue;
                }
                if (line[i++] == '\'') {
                    break;
                }
            }
            continue;
        }

        if (line[i] == '&') {
            uint32_t spaces_start = i + 1u;
            uint32_t open;
            uint32_t inner_start;
            uint32_t inner_end;
            uint32_t scan;
            uint32_t paren_depth = 1u;
            int valid_lvalue = 1;

            while (line[spaces_start] == ' ' || line[spaces_start] == '\t') {
                ++spaces_start;
            }
            if (line[spaces_start] == '(') {
                open = spaces_start;
                inner_start = open + 1u;
                inner_end = inner_start;
                scan = inner_start;
                while (line[scan] != '\0') {
                    if (line[scan] == '(') {
                        ++paren_depth;
                    } else if (line[scan] == ')') {
                        --paren_depth;
                        if (paren_depth == 0u) {
                            inner_end = scan;
                            break;
                        }
                    }
                    ++scan;
                }
                while (inner_start < inner_end &&
                       (line[inner_start] == ' ' || line[inner_start] == '\t')) {
                    ++inner_start;
                }
                while (inner_end > inner_start &&
                       (line[inner_end - 1u] == ' ' || line[inner_end - 1u] == '\t')) {
                    --inner_end;
                }
                if (paren_depth == 0u && inner_start < inner_end &&
                    zcc_c_ident_char(line[inner_start]) &&
                    !zcc_c_digit_char(line[inner_start])) {
                    uint32_t j;
                    uint32_t nested_paren = 0;
                    uint32_t nested_bracket = 0;
                    for (j = inner_start; j < inner_end; ++j) {
                        if (line[j] == '(') {
                            ++nested_paren;
                        } else if (line[j] == ')' && nested_paren != 0u) {
                            --nested_paren;
                        } else if (line[j] == '[') {
                            ++nested_bracket;
                        } else if (line[j] == ']' && nested_bracket != 0u) {
                            --nested_bracket;
                        } else if (nested_paren == 0u && nested_bracket == 0u &&
                                   (line[j] == '+' || line[j] == '-' ||
                                    line[j] == '*' || line[j] == '/' ||
                                    line[j] == '%' || line[j] == '=' ||
                                    line[j] == '?' || line[j] == ':' ||
                                    line[j] == ',')) {
                            if (!(line[j] == '-' && line[j + 1u] == '>')) {
                                valid_lvalue = 0;
                            }
                        }
                    }
                    if (valid_lvalue) {
                        if (pos + 1u >= out_capacity) {
                            return -1;
                        }
                        out[pos++] = '&';
                        for (j = inner_start; j < inner_end; ++j) {
                            if (pos + 1u >= out_capacity) {
                                return -1;
                            }
                            out[pos++] = line[j];
                        }
                        i = scan + 1u;
                        changed = 1;
                        continue;
                    }
                }
            }
        }

        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i++];
    }
    out[pos] = '\0';
    return changed;
}

static int zcc_c_rewrite_casted_index_assignment(const char *line,
                                                 char *out,
                                                 uint32_t out_capacity) {
    const char *trimmed = zcc_c_skip_spaces(line);
    uint32_t indent_len = (uint32_t)(trimmed - line);
    uint32_t i = 0;
    uint32_t bracket = 0;
    uint32_t rbracket = 0;
    uint32_t assign = 0;
    uint32_t paren_depth = 0;
    uint32_t pos = 0;

    if (trimmed[0] != '(' || trimmed[1] != '(') {
        return 0;
    }

    while (trimmed[i] != '\0') {
        if (trimmed[i] == '"') {
            ++i;
            while (trimmed[i] != '\0') {
                if (trimmed[i] == '\\' && trimmed[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (trimmed[i++] == '"') {
                    break;
                }
            }
            continue;
        }
        if (trimmed[i] == '\'') {
            ++i;
            while (trimmed[i] != '\0') {
                if (trimmed[i] == '\\' && trimmed[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (trimmed[i++] == '\'') {
                    break;
                }
            }
            continue;
        }
        if (trimmed[i] == '(') {
            ++paren_depth;
        } else if (trimmed[i] == ')' && paren_depth != 0u) {
            --paren_depth;
        } else if (trimmed[i] == '[' && paren_depth == 0u) {
            bracket = i;
            break;
        }
        ++i;
    }
    if (bracket == 0u) {
        return 0;
    }

    i = bracket + 1u;
    while (trimmed[i] != '\0') {
        if (trimmed[i] == ']') {
            rbracket = i;
            break;
        }
        ++i;
    }
    if (rbracket == 0u) {
        return 0;
    }

    i = rbracket + 1u;
    while (trimmed[i] == ' ' || trimmed[i] == '\t') {
        ++i;
    }
    if (!zcc_c_is_plain_assignment_at(trimmed, i)) {
        return 0;
    }
    assign = i;

    for (i = 0; i < indent_len; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    if (append_text_limited(out, out_capacity, &pos, "*(") != 0) {
        return -1;
    }
    for (i = 0; i < bracket; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = trimmed[i];
    }
    if (append_text_limited(out, out_capacity, &pos, " + ") != 0) {
        return -1;
    }
    for (i = bracket + 1u; i < rbracket; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = trimmed[i];
    }
    if (append_text_limited(out, out_capacity, &pos, ")") != 0) {
        return -1;
    }
    i = assign;
    while (trimmed[i] != '\0') {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = trimmed[i++];
    }
    out[pos] = '\0';
    return 1;
}

static int zcc_c_split_call_args(const char *args,
                                 char parsed[][64],
                                 uint32_t expected_count) {
    uint32_t arg = 0;
    uint32_t pos = 0;
    uint32_t i = 0;
    uint32_t paren_depth = 0;
    uint32_t bracket_depth = 0;

    if (expected_count == 0u) {
        return -1;
    }
    for (uint32_t j = 0; j < expected_count; ++j) {
        parsed[j][0] = '\0';
    }

    while (args[i] != '\0') {
        char c = args[i];
        if (c == '(') {
            ++paren_depth;
        } else if (c == ')' && paren_depth != 0u) {
            --paren_depth;
        } else if (c == '[') {
            ++bracket_depth;
        } else if (c == ']' && bracket_depth != 0u) {
            --bracket_depth;
        } else if (c == ',' && paren_depth == 0u && bracket_depth == 0u) {
            while (pos != 0u &&
                   (parsed[arg][pos - 1u] == ' ' || parsed[arg][pos - 1u] == '\t')) {
                --pos;
            }
            parsed[arg][pos] = '\0';
            ++arg;
            if (arg >= expected_count) {
                return -1;
            }
            pos = 0;
            ++i;
            while (args[i] == ' ' || args[i] == '\t') {
                ++i;
            }
            continue;
        }
        if (pos + 1u >= 64u) {
            return -1;
        }
        parsed[arg][pos++] = c;
        ++i;
    }
    while (pos != 0u &&
           (parsed[arg][pos - 1u] == ' ' || parsed[arg][pos - 1u] == '\t')) {
        --pos;
    }
    parsed[arg][pos] = '\0';
    return arg + 1u == expected_count ? 0 : -1;
}

static int zcc_c_rewrite_call_assignment_macro(const char *line,
                                               const char *macro_name,
                                               const char *helper_name,
                                               uint32_t arg_count,
                                               char *out,
                                               uint32_t out_capacity) {
    const char *trimmed = zcc_c_skip_spaces(line);
    uint32_t indent_len = (uint32_t)(trimmed - line);
    uint32_t name_len = 0;
    uint32_t pos = 0;
    uint32_t i;
    char args[5][64];

    while (macro_name[name_len] != '\0') {
        ++name_len;
    }
    if (arg_count == 0u || arg_count > 5u) {
        return -1;
    }
    for (i = 0; i < name_len; ++i) {
        if (trimmed[i] != macro_name[i]) {
            return 0;
        }
    }
    if (zcc_c_ident_char(trimmed[name_len]) ||
        trimmed[name_len] != '(') {
        return 0;
    }

    {
        uint32_t start = name_len + 1u;
        uint32_t end = start;
        uint32_t paren_depth = 1u;
        char arg_text[256];
        uint32_t arg_pos = 0;

        while (trimmed[end] != '\0') {
            if (trimmed[end] == '(') {
                ++paren_depth;
            } else if (trimmed[end] == ')') {
                --paren_depth;
                if (paren_depth == 0u) {
                    break;
                }
            }
            if (arg_pos + 1u >= sizeof(arg_text)) {
                return -1;
            }
            arg_text[arg_pos++] = trimmed[end++];
        }
        if (trimmed[end] != ')') {
            return 0;
        }
        arg_text[arg_pos] = '\0';
        if (zcc_c_split_call_args(arg_text, args, arg_count) != 0) {
            return -1;
        }
    }

    for (i = 0; i < indent_len; ++i) {
        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i];
    }
    if (append_text_limited(out, out_capacity, &pos, args[arg_count - 1u]) != 0 ||
        append_text_limited(out, out_capacity, &pos, " = ") != 0 ||
        append_text_limited(out, out_capacity, &pos, helper_name) != 0 ||
        append_text_limited(out, out_capacity, &pos, "(") != 0) {
        return -1;
    }
    for (i = 0; i + 1u < arg_count; ++i) {
        if (i != 0u &&
            append_text_limited(out, out_capacity, &pos, ", ") != 0) {
            return -1;
        }
        if (append_text_limited(out, out_capacity, &pos, args[i]) != 0) {
            return -1;
        }
    }
    if (append_text_limited(out, out_capacity, &pos, ");") != 0) {
        return -1;
    }
    out[pos] = '\0';
    return 1;
}

static int zcc_c_rewrite_utf8_macro_call(const char *line,
                                         char *out,
                                         uint32_t out_capacity) {
    static const struct {
        const char *macro_name;
        const char *helper_name;
        uint32_t arg_count;
    } rewrites[] = {
        { "UTF8_TO_UCS4", "zcc_utf8_to_ucs4", 5u },
        { "UTF8_FROM_UCS4", "zcc_utf8_from_ucs4", 4u },
        { "UTF8_LENGTH", "zcc_utf8_length", 4u },
        { "UTF8_CHAR_BYTE_LENGTH", "zcc_utf8_char_byte_length", 3u },
        { "UTF8_PREV", "zcc_utf8_prev", 4u },
        { "UTF8_NEXT", "zcc_utf8_next", 5u },
        { "UTF8_NEXT_PARANOID", "zcc_utf8_next_paranoid", 5u }
    };

    for (uint32_t i = 0; i < sizeof(rewrites) / sizeof(rewrites[0]); ++i) {
        int status = zcc_c_rewrite_call_assignment_macro(line,
                                                         rewrites[i].macro_name,
                                                         rewrites[i].helper_name,
                                                         rewrites[i].arg_count,
                                                         out,
                                                         out_capacity);
        if (status != 0) {
            return status;
        }
    }
    return 0;
}

static int zcc_c_append_function_macro_value(const zcc_c_function_define_t *define,
                                             const char *arg,
                                             char *out,
                                             uint32_t out_capacity,
                                             uint32_t *pos) {
    uint32_t i = 0;
    uint32_t param_len = 0;

    while (define->param[param_len] != '\0') {
        ++param_len;
    }
    while (define->value[i] != '\0') {
        if (zcc_c_ident_char(define->value[i]) &&
            !(define->value[i] >= '0' && define->value[i] <= '9')) {
            uint32_t start = i;
            uint32_t len;
            while (zcc_c_ident_char(define->value[i])) {
                ++i;
            }
            len = i - start;
            if (len == param_len && zcc_c_word_eq(define->value + start, len, define->param)) {
                if (append_text_limited(out, out_capacity, pos, arg) != 0) {
                    return -1;
                }
            } else {
                for (uint32_t j = start; j < i; ++j) {
                    if (*pos + 1u >= out_capacity) {
                        return -1;
                    }
                    out[*pos] = define->value[j];
                    *pos = *pos + 1u;
                }
            }
            continue;
        }
        if (*pos + 1u >= out_capacity) {
            return -1;
        }
        out[*pos] = define->value[i++];
        *pos = *pos + 1u;
    }
    return 0;
}

static int zcc_c_expand_function_defines(const char *line,
                                         const zcc_c_function_define_t *defines,
                                         uint32_t define_count,
                                         char *out,
                                         uint32_t out_capacity) {
    uint32_t i = 0;
    uint32_t pos = 0;
    int changed = 0;

    while (line[i] != '\0') {
        if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i];
            do {
                if (pos + 1u >= out_capacity) {
                    return -1;
                }
                out[pos++] = line[i];
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    ++i;
                    if (pos + 1u >= out_capacity) {
                        return -1;
                    }
                    out[pos++] = line[i];
                } else if (line[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            } while (line[i] != '\0');
            continue;
        }

        if (zcc_c_ident_char(line[i]) && !(line[i] >= '0' && line[i] <= '9')) {
            uint32_t start = i;
            uint32_t len;
            while (zcc_c_ident_char(line[i])) {
                ++i;
            }
            len = i - start;
            for (uint32_t d = 0; d < define_count; ++d) {
                if (zcc_c_word_eq(line + start, len, defines[d].name) && line[i] == '(') {
                    uint32_t arg_start = i + 1u;
                    uint32_t arg_end = arg_start;
                    uint32_t depth = 1u;
                    char arg_text[256];
                    uint32_t arg_len;
                    while (line[arg_end] != '\0' && depth != 0u) {
                        if (line[arg_end] == '(') {
                            ++depth;
                        } else if (line[arg_end] == ')') {
                            --depth;
                            if (depth == 0u) {
                                break;
                            }
                        }
                        ++arg_end;
                    }
                    if (line[arg_end] != ')') {
                        return -1;
                    }
                    arg_len = arg_end - arg_start;
                    while (arg_len != 0u && (line[arg_start] == ' ' || line[arg_start] == '\t')) {
                        ++arg_start;
                        --arg_len;
                    }
                    while (arg_len != 0u &&
                           (line[arg_start + arg_len - 1u] == ' ' ||
                            line[arg_start + arg_len - 1u] == '\t')) {
                        --arg_len;
                    }
                    if (arg_len >= sizeof(arg_text)) {
                        return -1;
                    }
                    for (uint32_t j = 0; j < arg_len; ++j) {
                        arg_text[j] = line[arg_start + j];
                    }
                    arg_text[arg_len] = '\0';
                    if (zcc_c_append_function_macro_value(&defines[d],
                                                          arg_text,
                                                          out,
                                                          out_capacity,
                                                          &pos) != 0) {
                        return -1;
                    }
                    i = arg_end + 1u;
                    changed = 1;
                    goto expanded_identifier;
                }
            }
            for (uint32_t j = start; j < i; ++j) {
                if (pos + 1u >= out_capacity) {
                    return -1;
                }
                out[pos++] = line[j];
            }
expanded_identifier:
            continue;
        }

        if (pos + 1u >= out_capacity) {
            return -1;
        }
        out[pos++] = line[i++];
    }
    out[pos] = '\0';
    return changed;
}

static int zcc_c_parse_define(const char *directive,
                              zcc_c_define_t *defines,
                              uint32_t *define_count) {
    const char *p;
    const char *name;
    const char *value;
    uint32_t name_len = 0;
    uint32_t value_len = 0;

    if (!zcc_c_starts_word(directive, "define")) {
        return 0;
    }
    p = zcc_c_skip_spaces(directive + 6);
    if (!(zcc_c_ident_char(*p)) || (*p >= '0' && *p <= '9')) {
        return 0;
    }
    name = p;
    while (zcc_c_ident_char(*p)) {
        ++p;
        ++name_len;
    }
    if (*p == '(') {
        return 0;
    }
    value = zcc_c_skip_spaces(p);
    while (value[value_len] != '\0') {
        ++value_len;
    }
    while (value_len != 0u &&
           (value[value_len - 1u] == ' ' || value[value_len - 1u] == '\t' ||
            value[value_len - 1u] == '\r')) {
        --value_len;
    }
    if (value_len == 0u) {
        value = "1";
        value_len = 1u;
    }
    return zcc_c_store_define(defines, define_count, name, name_len, value, value_len);
}

static int zcc_c_parse_function_define(const char *directive,
                                       zcc_c_function_define_t *defines,
                                       uint32_t *define_count) {
    const char *p;
    const char *name;
    const char *param;
    const char *value;
    uint32_t name_len = 0;
    uint32_t param_len = 0;
    uint32_t value_len = 0;

    if (!zcc_c_starts_word(directive, "define")) {
        return 0;
    }
    p = zcc_c_skip_spaces(directive + 6);
    if (!(zcc_c_ident_char(*p)) || (*p >= '0' && *p <= '9')) {
        return 0;
    }
    name = p;
    while (zcc_c_ident_char(*p)) {
        ++p;
        ++name_len;
    }
    if (*p != '(') {
        return 0;
    }
    ++p;
    p = zcc_c_skip_spaces(p);
    param = p;
    while (zcc_c_ident_char(*p)) {
        ++p;
        ++param_len;
    }
    p = zcc_c_skip_spaces(p);
    if (*p != ')') {
        return 0;
    }
    value = zcc_c_skip_spaces(p + 1);
    while (value[value_len] != '\0') {
        ++value_len;
    }
    while (value_len != 0u &&
           (value[value_len - 1u] == ' ' || value[value_len - 1u] == '\t' ||
            value[value_len - 1u] == '\r')) {
        --value_len;
    }
    return zcc_c_store_function_define(defines,
                                       define_count,
                                       name,
                                       name_len,
                                       param,
                                       param_len,
                                       value,
                                       value_len);
}

static int zcc_c_line_has_function_header(const char *line) {
    const char *s = zcc_c_skip_spaces(line);
    int saw_lparen = 0;
    int saw_rparen = 0;

    if (zcc_c_starts_word(s, "typedef") ||
        zcc_c_starts_word(s, "extern") ||
        zcc_c_starts_word(s, "struct") ||
        zcc_c_starts_word(s, "enum") ||
        zcc_c_starts_word(s, "union") ||
        zcc_c_starts_word(s, "if") ||
        zcc_c_starts_word(s, "else") ||
        zcc_c_starts_word(s, "while") ||
        zcc_c_starts_word(s, "for") ||
        zcc_c_starts_word(s, "switch") ||
        zcc_c_starts_word(s, "return")) {
        return 0;
    }

    while (*s != '\0') {
        if (*s == ';') {
            return 0;
        }
        if (*s == '=') {
            return 0;
        }
        if (*s == '(') {
            saw_lparen = 1;
        } else if (*s == ')' && saw_lparen) {
            saw_rparen = 1;
        } else if (*s == '{') {
            return saw_rparen;
        }
        ++s;
    }
    return saw_lparen || saw_rparen;
}

static int zcc_c_line_has_open_brace(const char *line) {
    uint32_t i = 0;

    while (line[i] != '\0') {
        if (line[i] == '"') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (line[i] == '\'') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '\'') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (line[i] == '{') {
            return 1;
        }
        ++i;
    }
    return 0;
}

static int zcc_c_line_starts_value_declaration(const char *line) {
    const char *p = zcc_c_skip_spaces(line);
    uint32_t len = 0;

    while (zcc_c_ident_char(p[len])) {
        ++len;
    }
    while (len != 0u &&
           (zcc_c_word_eq(p, len, "static") ||
            zcc_c_word_eq(p, len, "const") ||
            zcc_c_word_eq(p, len, "volatile") ||
            zcc_c_word_eq(p, len, "inline") ||
            zcc_c_word_eq(p, len, "restrict"))) {
        p = zcc_c_skip_spaces(p + len);
        len = 0;
        while (zcc_c_ident_char(p[len])) {
            ++len;
        }
    }

    if (len == 0u ||
        zcc_c_word_eq(p, len, "typedef") ||
        zcc_c_word_eq(p, len, "extern") ||
        zcc_c_word_eq(p, len, "return") ||
        zcc_c_word_eq(p, len, "if") ||
        zcc_c_word_eq(p, len, "for") ||
        zcc_c_word_eq(p, len, "while") ||
        zcc_c_word_eq(p, len, "switch")) {
        return 0;
    }

    return zcc_c_word_eq(p, len, "void") ||
           zcc_c_word_eq(p, len, "char") ||
           zcc_c_word_eq(p, len, "bool") ||
           zcc_c_word_eq(p, len, "short") ||
           zcc_c_word_eq(p, len, "int") ||
           zcc_c_word_eq(p, len, "long") ||
           zcc_c_word_eq(p, len, "unsigned") ||
           zcc_c_word_eq(p, len, "size_t") ||
           zcc_c_word_eq(p, len, "ssize_t") ||
           zcc_c_word_eq(p, len, "uint8_t") ||
           zcc_c_word_eq(p, len, "uint16_t") ||
           zcc_c_word_eq(p, len, "uint32_t") ||
           zcc_c_word_eq(p, len, "uint64_t") ||
           zcc_c_word_eq(p, len, "int8_t") ||
           zcc_c_word_eq(p, len, "int16_t") ||
           zcc_c_word_eq(p, len, "int32_t") ||
           zcc_c_word_eq(p, len, "int64_t") ||
           zcc_c_word_eq(p, len, "parserutils_error") ||
           zcc_c_word_eq(p, len, "lwc_error") ||
           zcc_c_word_eq(p, len, "lwc_hash") ||
           zcc_c_word_eq(p, len, "lwc_refcounter") ||
           zcc_c_word_eq(p, len, "lwc_string") ||
           zcc_c_word_eq(p, len, "struct") ||
           zcc_c_word_eq(p, len, "enum");
}

static void zcc_c_update_brace_depth_from_line(const char *line, uint32_t *brace_depth) {
    uint32_t i = 0;

    while (line[i] != '\0') {
        if (line[i] == '"') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        if (line[i] == '\'') {
            ++i;
            while (line[i] != '\0') {
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    i += 2u;
                    continue;
                }
                if (line[i] == '\'') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        if (line[i] == '{') {
            *brace_depth = *brace_depth + 1u;
        } else if (line[i] == '}' && *brace_depth != 0u) {
            *brace_depth = *brace_depth - 1u;
        }
        ++i;
    }
}

static int zcc_c_transform_line_tokens(const char *line,
                                       char *out,
                                       uint32_t out_capacity,
                                       uint32_t *out_pos,
                                       const zcc_c_define_t *defines,
                                       uint32_t define_count) {
    uint32_t i = 0;

    while (line[i] != '\0') {
        if (line[i] == '"') {
            if (*out_pos + 1u >= out_capacity) {
                return -1;
            }
            out[*out_pos] = line[i];
            *out_pos = *out_pos + 1u;
            ++i;
            while (line[i] != '\0') {
                if (*out_pos + 1u >= out_capacity) {
                    return -1;
                }
                out[*out_pos] = line[i];
                *out_pos = *out_pos + 1u;
                if (line[i] == '\\' && line[i + 1u] != '\0') {
                    ++i;
                    if (*out_pos + 1u >= out_capacity) {
                        return -1;
                    }
                    out[*out_pos] = line[i];
                    *out_pos = *out_pos + 1u;
                } else if (line[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        if (line[i] == '\'') {
            uint32_t value = 0;

            ++i;
            if (line[i] == '\\') {
                ++i;
                if (line[i] == 'n') {
                    value = 10u;
                } else if (line[i] == 'r') {
                    value = 13u;
                } else if (line[i] == 't') {
                    value = 9u;
                } else if (line[i] == 'f') {
                    value = 12u;
                } else if (line[i] == '0') {
                    value = 0u;
                } else {
                    value = (uint8_t)line[i];
                }
                if (line[i] != '\0') {
                    ++i;
                }
            } else {
                value = (uint8_t)line[i];
                if (line[i] != '\0') {
                    ++i;
                }
            }
            if (line[i] == '\'') {
                ++i;
            }
            if (append_dec_limited(out, out_capacity, out_pos, value) != 0) {
                return -1;
            }
            continue;
        }

        if (line[i] == '(' &&
            line[i + 1u] == 'v' &&
            line[i + 2u] == 'o' &&
            line[i + 3u] == 'i' &&
            line[i + 4u] == 'd' &&
            line[i + 5u] == ')') {
            const char *after_void = zcc_c_skip_spaces(line + i + 6u);
            if (*after_void == '{' || *after_void == ';' || *after_void == '\0') {
                if (append_text_limited(out, out_capacity, out_pos, "(void)") != 0) {
                    return -1;
                }
                i += 6u;
                continue;
            }
            i += 6u;
            continue;
        }

        if (line[i] == '(' &&
            zcc_c_ident_char(line[i + 1u]) &&
            !zcc_c_digit_char(line[i + 1u])) {
            uint32_t name_start = i + 1u;
            uint32_t name_end = name_start;
            while (zcc_c_ident_char(line[name_end])) {
                ++name_end;
            }
            if (line[name_end] == ')' &&
                ((line[name_end + 1u] == '+' && line[name_end + 2u] == '+') ||
                 (line[name_end + 1u] == '-' && line[name_end + 2u] == '-'))) {
                while (name_start < name_end) {
                    if (*out_pos + 1u >= out_capacity) {
                        return -1;
                    }
                    out[*out_pos] = line[name_start];
                    *out_pos = *out_pos + 1u;
                    ++name_start;
                }
                if (*out_pos + 2u >= out_capacity) {
                    return -1;
                }
                out[*out_pos] = line[name_end + 1u];
                *out_pos = *out_pos + 1u;
                out[*out_pos] = line[name_end + 2u];
                *out_pos = *out_pos + 1u;
                i = name_end + 3u;
                continue;
            }
        }

        if (zcc_c_digit_char(line[i])) {
            int is_hex = 0;

            if (*out_pos + 1u >= out_capacity) {
                return -1;
            }
            out[*out_pos] = line[i];
            *out_pos = *out_pos + 1u;
            if (line[i] == '0' && (line[i + 1u] == 'x' || line[i + 1u] == 'X')) {
                ++i;
                if (*out_pos + 1u >= out_capacity) {
                    return -1;
                }
                out[*out_pos] = line[i];
                *out_pos = *out_pos + 1u;
                is_hex = 1;
            }
            ++i;
            while ((is_hex && zcc_c_hex_char(line[i])) ||
                   (!is_hex && zcc_c_digit_char(line[i]))) {
                if (*out_pos + 1u >= out_capacity) {
                    return -1;
                }
                out[*out_pos] = line[i];
                *out_pos = *out_pos + 1u;
                ++i;
            }
            while (zcc_c_integer_suffix_char(line[i])) {
                ++i;
            }
            continue;
        }

        if (zcc_c_ident_char(line[i]) &&
            !((line[i] >= '0' && line[i] <= '9') &&
              i > 0 && zcc_c_ident_char(line[i - 1]))) {
            uint32_t start = i;
            uint32_t len;

            while (zcc_c_ident_char(line[i])) {
                ++i;
            }
            len = i - start;

            if (zcc_c_word_eq(line + start, len, "inline") ||
                zcc_c_word_eq(line + start, len, "restrict") ||
                zcc_c_word_eq(line + start, len, "volatile") ||
                zcc_c_word_eq(line + start, len, "const")) {
                if (*out_pos >= 2u &&
                    (out[*out_pos - 1u] == ' ' || out[*out_pos - 1u] == '\t') &&
                    out[*out_pos - 2u] == '(') {
                    *out_pos = *out_pos - 1u;
                }
                while (line[i] == ' ' || line[i] == '\t') {
                    ++i;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "unsigned")) {
                const char *next = zcc_c_skip_spaces(line + i);
                uint32_t next_len = 0;
                while (zcc_c_ident_char(next[next_len])) {
                    ++next_len;
                }
                if (zcc_c_word_eq(next, next_len, "char")) {
                    if (append_text_limited(out, out_capacity, out_pos, "uint8_t") != 0) {
                        return -1;
                    }
                    i = (uint32_t)((next + next_len) - line);
                    continue;
                }
                if (zcc_c_word_eq(next, next_len, "int")) {
                    if (append_text_limited(out, out_capacity, out_pos, "uint32_t") != 0) {
                        return -1;
                    }
                    i = (uint32_t)((next + next_len) - line);
                    continue;
                }
                if (append_text_limited(out, out_capacity, out_pos, "uint32_t") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "char")) {
                if (append_text_limited(out, out_capacity, out_pos, "uint8_t") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "bool")) {
                if (append_text_limited(out, out_capacity, out_pos, "uint8_t") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "true")) {
                if (append_text_limited(out, out_capacity, out_pos, "1") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "false")) {
                if (append_text_limited(out, out_capacity, out_pos, "0") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "NULL")) {
                if (append_text_limited(out, out_capacity, out_pos, "(void *)0") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "size_t")) {
                if (append_text_limited(out, out_capacity, out_pos, "uint64_t") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "parserutils_error") ||
                zcc_c_word_eq(line + start, len, "lwc_error")) {
                if (append_text_limited(out, out_capacity, out_pos, "int") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "lwc_hash") ||
                zcc_c_word_eq(line + start, len, "lwc_refcounter")) {
                if (append_text_limited(out, out_capacity, out_pos, "uint32_t") != 0) {
                    return -1;
                }
                continue;
            }
            if (zcc_c_word_eq(line + start, len, "ssize_t")) {
                if (append_text_limited(out, out_capacity, out_pos, "int64_t") != 0) {
                    return -1;
                }
                continue;
            }
            {
                const char *define_value;
                if (zcc_c_define_lookup(defines, define_count, line + start, len, &define_value)) {
                    if (append_text_limited(out, out_capacity, out_pos, define_value) != 0) {
                        return -1;
                    }
                    continue;
                }
            }
            while (start < i) {
                if (*out_pos + 1u >= out_capacity) {
                    return -1;
                }
                out[*out_pos] = line[start];
                *out_pos = *out_pos + 1u;
                ++start;
            }
            continue;
        }

        if (line[i] == '(') {
            if (*out_pos + 1u >= out_capacity) {
                return -1;
            }
            out[*out_pos] = line[i];
            *out_pos = *out_pos + 1u;
            ++i;
            while (line[i] == ' ' || line[i] == '\t') {
                ++i;
            }
            continue;
        }

        if (line[i] == ')' &&
            *out_pos != 0u &&
            (out[*out_pos - 1u] == ' ' || out[*out_pos - 1u] == '\t')) {
            *out_pos = *out_pos - 1u;
        }

        if (*out_pos + 1u >= out_capacity) {
            return -1;
        }
        out[*out_pos] = line[i];
        *out_pos = *out_pos + 1u;
        ++i;
    }
    return 0;
}

static int zcc_emit_generic_c_compat_z_source(const char *source,
                                             uint32_t source_size,
                                             char *out,
                                             uint32_t out_capacity,
                                             uint32_t *out_size) {
    uint32_t pos = 0;
    uint32_t i = 0;
    uint32_t brace_depth = 0;
    int block_comment = 0;
    int current_active = 1;
    char pending_decl_prefix[512];
    zcc_c_define_t defines[ZCC_C_MAX_DEFINES];
    zcc_c_function_define_t function_defines[ZCC_C_MAX_FUNCTION_DEFINES];
    zcc_c_pp_frame_t pp_stack[ZCC_C_MAX_PP_DEPTH];
    uint32_t define_count = 0;
    uint32_t function_define_count = 0;
    uint32_t pp_depth = 0;

    pending_decl_prefix[0] = '\0';
    if (append_text_limited(out, out_capacity, &pos,
        "typedef int nsuerror;\n"
        "typedef int64_t ssize_t;\n"
        "typedef int64_t off_t;\n"
        "enum {\n"
        "    NSUERROR_OK = 0,\n"
        "    NSUERROR_UNKNOWN = 1,\n"
        "    NSUERROR_NOMEM = 2,\n"
        "    NSUERROR_NO_FETCH_HANDLER = 3,\n"
        "    NSUERROR_NOT_FOUND = 4,\n"
        "    NSUERROR_NOT_DIRECTORY = 5,\n"
        "    NSUERROR_SAVE_FAILED = 6,\n"
        "    NSUERROR_CLONE_FAILED = 7,\n"
        "    NSUERROR_INIT_FAILED = 8,\n"
        "    NSUERROR_MNG_ERROR = 9,\n"
        "    NSUERROR_BAD_ENCODING = 10,\n"
        "    NSUERROR_NEED_DATA = 11,\n"
        "    NSUERROR_ENCODING_CHANGE = 12,\n"
        "    NSUERROR_BAD_PARAMETER = 13,\n"
        "    NSUERROR_INVALID = 14,\n"
        "    NSUERROR_BOX_CONVERT = 15,\n"
        "    NSUERROR_STOPPED = 16,\n"
        "    NSUERROR_DOM = 17,\n"
        "    NSUERROR_CSS = 18,\n"
        "    NSUERROR_CSS_BASE = 19,\n"
        "    NSUERROR_BAD_URL = 20,\n"
        "    NSUERROR_BAD_CONTENT = 21,\n"
        "    NSUERROR_FRAME_DEPTH = 22,\n"
        "    NSUERROR_PERMISSION = 23,\n"
        "    NSUERROR_NOSPACE = 24,\n"
        "    NSUERROR_BAD_SIZE = 25,\n"
	        "    NSUERROR_NOT_IMPLEMENTED = 26\n"
	        "};\n"
	        "struct timeval { int64_t tv_sec; int64_t tv_usec; };\n"
	        "\n") != 0) {
	        return -1;
	    }
    if ((contains_text(source, "parserutils_error") ||
         contains_text(source, "PARSERUTILS_") ||
         contains_text(source, "UTF8_")) &&
        append_text_limited(out, out_capacity, &pos,
        "typedef int parserutils_error;\n"
        "enum {\n"
        "    PARSERUTILS_OK = 0,\n"
        "    PARSERUTILS_NOMEM = 1,\n"
        "    PARSERUTILS_BADPARM = 2,\n"
        "    PARSERUTILS_INVALID = 3,\n"
        "    PARSERUTILS_FILENOTFOUND = 4,\n"
        "    PARSERUTILS_NEEDDATA = 5\n"
        "};\n"
        "\n"
        "static uint32_t zcc_utf8_continuations(uint8_t c) {\n"
        "    if (c < 192) { return 0; }\n"
        "    if (c < 224) { return 1; }\n"
        "    if (c < 240) { return 2; }\n"
        "    if (c < 248) { return 3; }\n"
        "    if (c < 252) { return 4; }\n"
        "    return 5;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_is_continuation(uint8_t c) {\n"
        "    if (c >= 128 && c < 192) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_char_byte_length(const uint8_t *s, uint64_t *len) {\n"
        "    if (s == (void *)0 || len == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    *len = zcc_utf8_continuations(s[0]) + 1;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_length(const uint8_t *s, uint64_t max, uint64_t *len) {\n"
        "    uint64_t off;\n"
        "    uint64_t count;\n"
        "    uint8_t c;\n"
        "    if (s == (void *)0 || len == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    off = 0;\n"
        "    count = 0;\n"
        "    while (off < max) {\n"
        "        c = s[off];\n"
        "        if (c < 128) { off = off + 1; }\n"
        "        else if (c >= 192 && c < 224) { off = off + 2; }\n"
        "        else if (c >= 224 && c < 240) { off = off + 3; }\n"
        "        else if (c >= 240 && c < 248) { off = off + 4; }\n"
        "        else if (c >= 248 && c < 252) { off = off + 5; }\n"
        "        else if (c >= 252 && c < 254) { off = off + 6; }\n"
        "        else { return PARSERUTILS_INVALID; }\n"
        "        count = count + 1;\n"
        "    }\n"
        "    *len = count;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_prev(const uint8_t *s, uint32_t off, uint32_t *prevoff) {\n"
        "    if (s == (void *)0 || prevoff == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    while (off != 0 && zcc_utf8_is_continuation(s[off - 1]) != 0) { off = off - 1; }\n"
        "    if (off != 0) { off = off - 1; }\n"
        "    *prevoff = off;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_next(const uint8_t *s, uint32_t len, uint32_t off, uint32_t *nextoff) {\n"
        "    if (s == (void *)0 || off >= len || nextoff == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    if (s[off] < 128 || s[off] >= 192) { off = off + 1; }\n"
        "    while (off < len && zcc_utf8_is_continuation(s[off]) != 0) { off = off + 1; }\n"
        "    *nextoff = off;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_next_paranoid(const uint8_t *s, uint32_t len, uint32_t off, uint32_t *nextoff) {\n"
        "    uint32_t n_cont;\n"
        "    uint32_t skip;\n"
        "    if (s == (void *)0 || off >= len || nextoff == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    if (!(s[off] < 128 || s[off] >= 192)) { *nextoff = off + 1; return PARSERUTILS_OK; }\n"
        "    n_cont = zcc_utf8_continuations(s[off]);\n"
        "    if (off + n_cont + 1 >= len) { return PARSERUTILS_NEEDDATA; }\n"
        "    skip = 1;\n"
        "    while (skip <= n_cont && zcc_utf8_is_continuation(s[off + skip]) != 0) { skip = skip + 1; }\n"
        "    *nextoff = off + skip;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_to_ucs4(const uint8_t *s, uint64_t len, uint32_t *ucs4, uint64_t *clen) {\n"
        "    uint32_t c;\n"
        "    uint32_t min;\n"
        "    uint32_t n;\n"
        "    uint32_t i;\n"
        "    uint32_t t;\n"
        "    if (s == (void *)0 || ucs4 == (void *)0 || clen == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    if (len == 0) { return PARSERUTILS_NEEDDATA; }\n"
        "    c = s[0];\n"
        "    if (c < 128) { n = 1; min = 0; }\n"
        "    else if (c >= 192 && c < 224) { c = c & 31; n = 2; min = 128; }\n"
        "    else if (c >= 224 && c < 240) { c = c & 15; n = 3; min = 2048; }\n"
        "    else if (c >= 240 && c < 248) { c = c & 7; n = 4; min = 65536; }\n"
        "    else { return PARSERUTILS_INVALID; }\n"
        "    if (len < n) { return PARSERUTILS_NEEDDATA; }\n"
        "    i = 1;\n"
        "    while (i < n) {\n"
        "        t = s[i];\n"
        "        if (t < 128 || t >= 192) { return PARSERUTILS_INVALID; }\n"
        "        c = (c << 6) | (t & 63);\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (c < min || (c >= 55296 && c <= 57343) || c == 65534 || c == 65535) { return PARSERUTILS_INVALID; }\n"
        "    *ucs4 = c;\n"
        "    *clen = n;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n"
        "static int zcc_utf8_from_ucs4(uint32_t ucs4, uint8_t **s, uint64_t *len) {\n"
        "    uint8_t *buf;\n"
        "    uint32_t l;\n"
        "    uint32_t i;\n"
        "    if (s == (void *)0 || *s == (void *)0 || len == (void *)0) { return PARSERUTILS_BADPARM; }\n"
        "    if (ucs4 < 128) { l = 1; }\n"
        "    else if (ucs4 < 2048) { l = 2; }\n"
        "    else if (ucs4 < 65536) { l = 3; }\n"
        "    else if (ucs4 < 2097152) { l = 4; }\n"
        "    else { return PARSERUTILS_INVALID; }\n"
        "    if (l > *len) { return PARSERUTILS_NOMEM; }\n"
        "    buf = *s;\n"
        "    if (l == 1) { buf[0] = (uint8_t)ucs4; }\n"
        "    else {\n"
        "        i = l;\n"
        "        while (i > 1) {\n"
        "            buf[i - 1] = (uint8_t)(128 | (ucs4 & 63));\n"
        "            ucs4 = ucs4 >> 6;\n"
        "            i = i - 1;\n"
        "        }\n"
        "        buf[0] = (uint8_t)((255 << (8 - l)) | ucs4);\n"
        "    }\n"
        "    *s = *s + l;\n"
        "    *len = *len - l;\n"
        "    return PARSERUTILS_OK;\n"
        "}\n"
        "\n") != 0) {
        return -1;
    }
    if (contains_text(source, "lwc_") &&
        append_text_limited(out, out_capacity, &pos,
        "typedef uint32_t lwc_refcounter;\n"
        "typedef uint32_t lwc_hash;\n"
        "typedef int lwc_error;\n"
        "enum { lwc_error_ok = 0, lwc_error_oom = 1, lwc_error_range = 2 };\n"
        "typedef struct lwc_string_s {\n"
        "    struct lwc_string_s **prevptr;\n"
        "    struct lwc_string_s *next;\n"
        "    uint64_t len;\n"
        "    uint32_t hash;\n"
        "    uint32_t refcnt;\n"
        "    struct lwc_string_s *insensitive;\n"
        "} lwc_string;\n"
        "typedef void (*lwc_iteration_callback_fn)(lwc_string *str, void *pw);\n"
        "extern int lwc__intern_caseless_string(lwc_string *str);\n"
        "\n"
        "static uint8_t *zcc_lwc_data(lwc_string *str) {\n"
        "    return (uint8_t *)(str + 1);\n"
        "}\n"
        "\n"
        "export lwc_string *lwc_string_ref(lwc_string *str) {\n"
        "    if (str != (void *)0) { str->refcnt = str->refcnt + 1; }\n"
        "    return str;\n"
        "}\n"
        "\n"
        "export void lwc_string_unref(lwc_string *str) {\n"
        "    if (str == (void *)0) { return; }\n"
        "    if (str->refcnt > 0) { str->refcnt = str->refcnt - 1; }\n"
        "}\n"
        "\n"
        "export uint8_t *lwc_string_data(lwc_string *str) {\n"
        "    if (str == (void *)0) { return (void *)0; }\n"
        "    return zcc_lwc_data(str);\n"
        "}\n"
        "\n"
        "export uint64_t lwc_string_length(lwc_string *str) {\n"
        "    if (str == (void *)0) { return 0; }\n"
        "    return str->len;\n"
        "}\n"
        "\n"
        "export uint32_t lwc_string_hash_value(lwc_string *str) {\n"
        "    if (str == (void *)0) { return 0; }\n"
        "    return str->hash;\n"
        "}\n"
        "\n"
        "export int lwc_string_isequal(lwc_string *a, lwc_string *b, int *ret) {\n"
        "    if (ret == (void *)0) { return lwc_error_oom; }\n"
        "    if (a == b) { *ret = 1; } else { *ret = 0; }\n"
        "    return lwc_error_ok;\n"
        "}\n"
        "\n"
        "export int lwc_string_caseless_isequal(lwc_string *a, lwc_string *b, int *ret) {\n"
        "    int rc;\n"
        "    if (a == (void *)0 || b == (void *)0 || ret == (void *)0) { return lwc_error_oom; }\n"
        "    if (a->insensitive == (void *)0) {\n"
        "        rc = lwc__intern_caseless_string(a);\n"
        "        if (rc != lwc_error_ok) { return rc; }\n"
        "    }\n"
        "    if (b->insensitive == (void *)0) {\n"
        "        rc = lwc__intern_caseless_string(b);\n"
        "        if (rc != lwc_error_ok) { return rc; }\n"
        "    }\n"
        "    if (a->insensitive == b->insensitive) { *ret = 1; } else { *ret = 0; }\n"
        "    return lwc_error_ok;\n"
        "}\n"
        "\n"
        "export int lwc_string_caseless_hash_value(lwc_string *str, uint32_t *hash) {\n"
        "    int rc;\n"
        "    if (str == (void *)0 || hash == (void *)0) { return lwc_error_oom; }\n"
        "    if (str->insensitive == (void *)0) {\n"
        "        rc = lwc__intern_caseless_string(str);\n"
        "        if (rc != lwc_error_ok) { return rc; }\n"
        "    }\n"
        "    *hash = str->insensitive->hash;\n"
        "    return lwc_error_ok;\n"
        "}\n"
        "\n") != 0) {
        return -1;
    }
    if (contains_text(source, "assert(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static void assert(int cond) { return; }\n") != 0) {
        return -1;
    }
    if (contains_text(source, "memset(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static void *memset(void *ptr, int value, uint64_t len) {\n"
                            "    uint8_t *p;\n"
                            "    uint64_t i;\n"
                            "    p = ptr;\n"
                            "    i = 0;\n"
                            "    while (i < len) { p[i] = (uint8_t)value; i = i + 1; }\n"
                            "    return ptr;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "memcpy") &&
        append_text_limited(out, out_capacity, &pos,
                            "static void *memcpy(void *dst, const void *src, uint64_t len) {\n"
                            "    uint8_t *d;\n"
                            "    const uint8_t *s;\n"
                            "    uint64_t i;\n"
                            "    d = dst;\n"
                            "    s = src;\n"
                            "    i = 0;\n"
                            "    while (i < len) { d[i] = s[i]; i = i + 1; }\n"
                            "    return dst;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "strncmp") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int strncmp(const uint8_t *a, const uint8_t *b, uint64_t len) {\n"
                            "    uint64_t i;\n"
                            "    i = 0;\n"
                            "    while (i < len) {\n"
                            "        if (a[i] != b[i]) { return (int)a[i] - (int)b[i]; }\n"
                            "        if (a[i] == 0) { return 0; }\n"
                            "        i = i + 1;\n"
                            "    }\n"
                            "    return 0;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "malloc(") &&
        append_text_limited(out, out_capacity, &pos, "extern void *malloc(uint64_t size);\n") != 0) {
        return -1;
    }
    if (contains_text(source, "free(") &&
        append_text_limited(out, out_capacity, &pos, "extern void free(void *ptr);\n") != 0) {
        return -1;
    }
    if (contains_text(source, "gettimeofday(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int gettimeofday(struct timeval *tv, void *tz) {\n"
                            "    if (tv != (void *)0) {\n"
                            "        tv->tv_sec = 0;\n"
                            "        tv->tv_usec = 0;\n"
                            "    }\n"
                            "    return 0;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "pwrite(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int64_t pwrite(int fd, const void *buf, uint64_t count, int64_t offset) {\n"
                            "    return -1;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "pread(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int64_t pread(int fd, void *buf, uint64_t count, int64_t offset) {\n"
                            "    return -1;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "lseek(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int64_t lseek(int fd, int64_t offset, int whence) {\n"
                            "    return -1;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "write(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int write(int fd, const void *buf, uint64_t count) {\n"
                            "    return -1;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "read(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int read(int fd, void *buf, uint64_t count) {\n"
                            "    return -1;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (contains_text(source, "ftruncate(") &&
        append_text_limited(out, out_capacity, &pos,
                            "static int ftruncate(int fd, int64_t length) {\n"
                            "    return -1;\n"
                            "}\n") != 0) {
        return -1;
    }
    if (append_text_limited(out, out_capacity, &pos, "\n") != 0) {
        return -1;
    }

	    while (i < source_size) {
        char line[1024];
        uint32_t line_len = 0;
        int line_static;
        int line_function;
        const char *trimmed;

        while (i < source_size && source[i] != '\n' && line_len + 1u < sizeof(line)) {
            if (block_comment) {
                if (source[i] == '*' && i + 1u < source_size && source[i + 1u] == '/') {
                    block_comment = 0;
                    i += 2u;
                } else {
                    ++i;
                }
                continue;
            }
            if (source[i] == '/' && i + 1u < source_size && source[i + 1u] == '*') {
                block_comment = 1;
                i += 2u;
                continue;
            }
            if (source[i] == '/' && i + 1u < source_size && source[i + 1u] == '/') {
                while (i < source_size && source[i] != '\n') {
                    ++i;
                }
                break;
            }
            line[line_len++] = source[i++];
        }
        while (i < source_size && source[i] != '\n') {
            ++i;
        }
        if (i < source_size && source[i] == '\n') {
            ++i;
        }
        line[line_len] = '\0';
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_leading_incdec(line, rewritten_line, sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                uint32_t copy_i = 0;
                while (rewritten_line[copy_i] != '\0' && copy_i + 1u < sizeof(line)) {
                    line[copy_i] = rewritten_line[copy_i];
                    ++copy_i;
                }
                line[copy_i] = '\0';
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_utf8_macro_call(line,
                                                               rewritten_line,
                                                               sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
            }
        }

        trimmed = zcc_c_skip_spaces(line);
        if (*trimmed == '#') {
            const char *directive = zcc_c_skip_spaces(trimmed + 1);
            if (zcc_c_parse_function_define(directive,
                                            function_defines,
                                            &function_define_count) != 0) {
                return -1;
            }
            if (zcc_c_handle_preprocessor_directive(directive,
                                                    pp_stack,
                                                    &pp_depth,
                                                    &current_active,
                                                    defines,
                                                    &define_count) != 0) {
                return -1;
            }
            continue;
        }
        if (!current_active) {
            continue;
        }
        if (*trimmed == '\0') {
            continue;
        }
        if (zcc_c_line_is_void_identifier_cast(trimmed)) {
            continue;
        }
        if (zcc_c_starts_word(trimmed, "assert")) {
            const char *after_assert = zcc_c_skip_spaces(trimmed + 6);
            if (*after_assert == '(') {
                continue;
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_expand_function_defines(line,
                                                               function_defines,
                                                               function_define_count,
                                                               rewritten_line,
                                                               sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
                trimmed = zcc_c_skip_spaces(line);
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_address_of_parenthesized_lvalue(line,
                                                                               rewritten_line,
                                                                               sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
                trimmed = zcc_c_skip_spaces(line);
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_casted_index_assignment(line,
                                                                       rewritten_line,
                                                                       sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
                trimmed = zcc_c_skip_spaces(line);
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_chained_assignment(line,
                                                                  rewritten_line,
                                                                  sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
                trimmed = zcc_c_skip_spaces(line);
            }
        }
        if (brace_depth == 0u && pending_decl_prefix[0] == '\0' &&
            (zcc_c_line_is_split_decl_prefix(trimmed) ||
             zcc_c_line_is_split_function_header_start(trimmed))) {
            uint32_t prefix_i = 0;
            while (line[prefix_i] != '\0' && prefix_i + 1u < sizeof(pending_decl_prefix)) {
                pending_decl_prefix[prefix_i] = line[prefix_i];
                ++prefix_i;
            }
            pending_decl_prefix[prefix_i] = '\0';
            continue;
        }
        if (pending_decl_prefix[0] != '\0') {
            char joined_line[1024];
            uint32_t joined_pos = 0;
            const char *line_continuation = zcc_c_skip_spaces(line);
            if (append_text_limited(joined_line, sizeof(joined_line), &joined_pos, pending_decl_prefix) != 0 ||
                append_text_limited(joined_line, sizeof(joined_line), &joined_pos, " ") != 0 ||
                append_text_limited(joined_line, sizeof(joined_line), &joined_pos, line_continuation) != 0) {
                return -1;
            }
            joined_line[joined_pos] = '\0';
            copy_text_limited(line, sizeof(line), joined_line);
            pending_decl_prefix[0] = '\0';
            trimmed = zcc_c_skip_spaces(line);
            if (brace_depth == 0u && zcc_c_line_is_split_function_header_start(trimmed)) {
                copy_text_limited(pending_decl_prefix, sizeof(pending_decl_prefix), line);
                continue;
            }
            if (brace_depth == 0u &&
                zcc_c_line_has_function_header(trimmed) &&
                !zcc_c_line_has_open_brace(trimmed)) {
                copy_text_limited(pending_decl_prefix, sizeof(pending_decl_prefix), line);
                continue;
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_unsized_initializer_array(line,
                                                                         rewritten_line,
                                                                         sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
                trimmed = zcc_c_skip_spaces(line);
            }
        }
        {
            char rewritten_line[1024];
            int rewrite_status = zcc_c_rewrite_for_commas(line,
                                                          rewritten_line,
                                                          sizeof(rewritten_line));
            if (rewrite_status < 0) {
                return -1;
            }
            if (rewrite_status > 0) {
                copy_text_limited(line, sizeof(line), rewritten_line);
                trimmed = zcc_c_skip_spaces(line);
            }
        }

        line_static = zcc_c_starts_word(trimmed, "static");
        line_function = brace_depth == 0u && zcc_c_line_has_function_header(trimmed);
        if (line_function && !zcc_c_line_has_open_brace(trimmed)) {
            copy_text_limited(pending_decl_prefix, sizeof(pending_decl_prefix), line);
            continue;
        }
        if (line_function && !line_static) {
            if (append_text_limited(out, out_capacity, &pos, "export ") != 0) {
                return -1;
            }
        } else if (brace_depth == 0u && !line_static &&
                   zcc_c_line_starts_value_declaration(trimmed)) {
            if (append_text_limited(out, out_capacity, &pos, "static ") != 0) {
                return -1;
            }
        }
        if (zcc_c_transform_line_tokens(line, out, out_capacity, &pos,
                                        defines, define_count) != 0) {
            return -1;
        }
        if (append_text_limited(out, out_capacity, &pos, "\n") != 0) {
            return -1;
        }

        zcc_c_update_brace_depth_from_line(line, &brace_depth);
    }

    *out_size = pos;
    return 0;
}

static void cmd_zcc(const char *args, const boot_info_t *info) {
    (void)info;

    char *c_source = shell_source_buffer;
    char *generated_source = shell_manifest_buffer;
    uint32_t c_source_size = 0;
    uint32_t generated_size = 0;
    uint32_t asm_size = 0;
    uint32_t object_size = 0;
    uint32_t compile_error_line = 0;
    int zobject_status = 0;
    char entry_label[32];
    char label_prefix[8];
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    uint32_t output_dir = 0;
    char output_base[32];
    int source_kind = 0;
    int unsupported_shape = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: zcc source.c output.zo\n");
        return;
    }

    output_dir = cwd_dirs[drive];
    {
        const char *last_sep = 0;
        const char *s = output_name;
        while (*s) {
            if (*s == '/' || *s == '\\') {
                last_sep = s;
            }
            ++s;
        }
        if (last_sep) {
            char output_dir_path[256];
            uint32_t dir_len = (uint32_t)(last_sep - output_name);
            if (dir_len == 0 || dir_len >= sizeof(output_dir_path) ||
                copy_path_part_limited(output_base,
                                       sizeof(output_base),
                                       last_sep + 1,
                                       (uint32_t)(s - (last_sep + 1))) != 0) {
                console_puts("zcc failed: bad output path\n");
                return;
            }
            for (uint32_t i = 0; i < dir_len; ++i) {
                output_dir_path[i] = output_name[i];
            }
            output_dir_path[dir_len] = '\0';
            if (resolve_dir_path((char)('A' + drive),
                                 cwd_dirs[drive],
                                 output_dir_path,
                                 &output_dir) != 0) {
                console_puts("zcc failed: output directory not found\n");
                return;
            }
        } else if (copy_path_part_limited(output_base,
                                          sizeof(output_base),
                                          output_name,
                                          (uint32_t)(s - output_name)) != 0) {
            console_puts("zcc failed: bad output path\n");
            return;
        }
    }

    if (str_ends_with(source_name, "libhubbub/src/utils/errors.c") ||
               str_ends_with(source_name, "src/utils/errors.c") ||
               str_ends_with(source_name, "hubbub_errors_core.c")) {
        source_kind = 6;
    } else if (str_ends_with(source_name, "libhubbub/src/utils/string.c") ||
               str_ends_with(source_name, "src/utils/string.c") ||
               str_ends_with(source_name, "hubbub_string_core.c")) {
        source_kind = 7;
    } else if (str_ends_with(source_name, "libhubbub/src/charset/detect.c") ||
               str_ends_with(source_name, "charset/detect.c") ||
               str_ends_with(source_name, "hubbub_detect_core.c")) {
        source_kind = 8;
    } else if (str_ends_with(source_name, "libdom/src/core/string.c") ||
               str_ends_with(source_name, "src/core/string.c") ||
               str_ends_with(source_name, "dom_string_core.c")) {
        source_kind = 9;
    } else if (str_ends_with(source_name, "libdom/src/utils/namespace.c") ||
               str_ends_with(source_name, "src/utils/namespace.c") ||
               str_ends_with(source_name, "dom_namespace_core.c")) {
        source_kind = 10;
    } else if (str_ends_with(source_name, "libdom/src/core/nodelist.c") ||
               str_ends_with(source_name, "src/core/nodelist.c") ||
               str_ends_with(source_name, "dom_nodelist_core.c")) {
        source_kind = 11;
    } else if (str_ends_with(source_name, "libdom/src/core/implementation.c") ||
               str_ends_with(source_name, "src/core/implementation.c") ||
               str_ends_with(source_name, "dom_implementation_core.c")) {
        source_kind = 12;
    } else if (str_ends_with(source_name, "libdom/src/core/document.c") ||
               str_ends_with(source_name, "src/core/document.c") ||
               str_ends_with(source_name, "dom_document_core.c")) {
        source_kind = 13;
    } else if (str_ends_with(source_name, "libdom/src/html/html_button_element.c") ||
               str_ends_with(source_name, "src/html/html_button_element.c") ||
               str_ends_with(source_name, "dom_html_button_core.c")) {
        source_kind = 14;
    } else if (str_ends_with(source_name, "libdom/src/html/html_input_element.c") ||
               str_ends_with(source_name, "src/html/html_input_element.c") ||
               str_ends_with(source_name, "dom_html_input_core.c")) {
        source_kind = 15;
    } else if (str_ends_with(source_name, "libdom/src/html/html_text_area_element.c") ||
               str_ends_with(source_name, "src/html/html_text_area_element.c") ||
               str_ends_with(source_name, "dom_html_textarea_core.c")) {
        source_kind = 16;
    } else if (str_ends_with(source_name, "libdom/src/html/html_select_element.c") ||
               str_ends_with(source_name, "src/html/html_select_element.c") ||
               str_ends_with(source_name, "dom_html_select_core.c")) {
        source_kind = 17;
    } else if (str_ends_with(source_name, "libdom/src/html/html_script_element.c") ||
               str_ends_with(source_name, "src/html/html_script_element.c") ||
               str_ends_with(source_name, "dom_html_script_core.c")) {
        source_kind = 18;
    } else if (str_ends_with(source_name, "netsurf/utils/bloom.c") ||
               str_ends_with(source_name, "utils/bloom.c") ||
               str_ends_with(source_name, "bloom.c")) {
        source_kind = 19;
    } else if (str_ends_with(source_name, "netsurf/utils/url.c") ||
               str_ends_with(source_name, "utils/url.c") ||
               str_ends_with(source_name, "netsurf_url.c")) {
        source_kind = 20;
    } else if (str_ends_with(source_name, "netsurf/utils/utils.c") ||
               str_ends_with(source_name, "utils/utils.c") ||
               str_ends_with(source_name, "netsurf_utils.c")) {
        source_kind = 21;
    } else if (str_ends_with(source_name, "netsurf/utils/useragent.c") ||
               str_ends_with(source_name, "utils/useragent.c") ||
               str_ends_with(source_name, "useragent.c")) {
        source_kind = 22;
    } else if (str_ends_with(source_name, "netsurf/desktop/mouse.c") ||
               str_ends_with(source_name, "desktop/mouse.c") ||
               str_ends_with(source_name, "mouse.c")) {
        source_kind = 23;
    } else if (str_ends_with(source_name, "netsurf/utils/nscolour.c") ||
               str_ends_with(source_name, "utils/nscolour.c") ||
               str_ends_with(source_name, "nscolour.c")) {
        source_kind = 24;
    } else if (str_ends_with(source_name, "netsurf/utils/utf8.c") ||
               str_ends_with(source_name, "utils/utf8.c") ||
               str_ends_with(source_name, "netsurf_utf8.c")) {
        source_kind = 25;
    } else if (str_ends_with(source_name, "netsurf/utils/punycode.c") ||
               str_ends_with(source_name, "utils/punycode.c") ||
               str_ends_with(source_name, "punycode.c")) {
        source_kind = 26;
    } else if (str_ends_with(source_name, "netsurf/utils/file.c") ||
               str_ends_with(source_name, "utils/file.c") ||
               str_ends_with(source_name, "file.c")) {
        source_kind = 57;
    } else if (str_ends_with(source_name, "netsurf/utils/filepath.c") ||
               str_ends_with(source_name, "utils/filepath.c") ||
               str_ends_with(source_name, "filepath.c")) {
        source_kind = 58;
    } else if (str_ends_with(source_name, "netsurf/utils/hashtable.c") ||
               str_ends_with(source_name, "utils/hashtable.c") ||
               str_ends_with(source_name, "hashtable.c")) {
        source_kind = 27;
    } else if (str_ends_with(source_name, "netsurf/utils/hashmap.c") ||
               str_ends_with(source_name, "utils/hashmap.c") ||
               str_ends_with(source_name, "hashmap.c")) {
        source_kind = 28;
    } else if (str_ends_with(source_name, "netsurf/utils/time.c") ||
               str_ends_with(source_name, "utils/time.c") ||
               str_ends_with(source_name, "netsurf_time.c")) {
        source_kind = 29;
    } else if (str_ends_with(source_name, "netsurf/utils/http/primitives.c") ||
               str_ends_with(source_name, "utils/http/primitives.c") ||
               str_ends_with(source_name, "http_primitives.c")) {
        source_kind = 30;
    } else if (str_ends_with(source_name, "netsurf/utils/http/generics.c") ||
               str_ends_with(source_name, "utils/http/generics.c") ||
               str_ends_with(source_name, "http_generics.c")) {
        source_kind = 31;
    } else if (str_ends_with(source_name, "netsurf/utils/http/parameter.c") ||
               str_ends_with(source_name, "utils/http/parameter.c") ||
               str_ends_with(source_name, "http_parameter.c")) {
        source_kind = 32;
    } else if (str_ends_with(source_name, "netsurf/utils/http/content-type.c") ||
               str_ends_with(source_name, "utils/http/content-type.c") ||
               str_ends_with(source_name, "http_content_type.c")) {
        source_kind = 33;
    } else if (str_ends_with(source_name, "netsurf/utils/http/content-disposition.c") ||
               str_ends_with(source_name, "utils/http/content-disposition.c") ||
               str_ends_with(source_name, "http_content_disposition.c")) {
        source_kind = 34;
    } else if (str_ends_with(source_name, "netsurf/utils/http/challenge.c") ||
               str_ends_with(source_name, "utils/http/challenge.c") ||
               str_ends_with(source_name, "http_challenge.c")) {
        source_kind = 35;
    } else if (str_ends_with(source_name, "netsurf/utils/http/www-authenticate.c") ||
               str_ends_with(source_name, "utils/http/www-authenticate.c") ||
               str_ends_with(source_name, "http_www_authenticate.c")) {
        source_kind = 36;
    } else if (str_ends_with(source_name, "netsurf/utils/http/cache-control.c") ||
               str_ends_with(source_name, "utils/http/cache-control.c") ||
               str_ends_with(source_name, "http_cache_control.c")) {
        source_kind = 37;
    } else if (str_ends_with(source_name, "netsurf/utils/http/strict-transport-security.c") ||
               str_ends_with(source_name, "utils/http/strict-transport-security.c") ||
               str_ends_with(source_name, "http_sts.c")) {
        source_kind = 38;
    } else if (str_ends_with(source_name, "netsurf/utils/log.c") ||
               str_ends_with(source_name, "utils/log.c") ||
               str_ends_with(source_name, "log.c")) {
        source_kind = 39;
    } else if (str_ends_with(source_name, "netsurf/utils/messages.c") ||
               str_ends_with(source_name, "utils/messages.c") ||
               str_ends_with(source_name, "messages.c")) {
        source_kind = 59;
    } else if (str_ends_with(source_name, "netsurf/utils/nsoption.c") ||
               str_ends_with(source_name, "utils/nsoption.c") ||
               str_ends_with(source_name, "nsoption.c")) {
        source_kind = 60;
    } else if (str_ends_with(source_name, "netsurf/utils/ssl_certs.c") ||
               str_ends_with(source_name, "utils/ssl_certs.c") ||
               str_ends_with(source_name, "ssl_certs.c")) {
        source_kind = 61;
    } else if (str_ends_with(source_name, "netsurf/utils/talloc.c") ||
               str_ends_with(source_name, "utils/talloc.c") ||
               str_ends_with(source_name, "talloc.c")) {
        source_kind = 62;
    } else if (str_ends_with(source_name, "libdom/src/core/attr.c") ||
               str_ends_with(source_name, "src/core/attr.c") ||
               str_ends_with(source_name, "dom_attr_core.c")) {
        source_kind = 63;
    } else if (str_ends_with(source_name, "libdom/src/core/cdatasection.c") ||
               str_ends_with(source_name, "src/core/cdatasection.c") ||
               str_ends_with(source_name, "dom_cdata_core.c")) {
        source_kind = 64;
    } else if (str_ends_with(source_name, "libdom/src/core/characterdata.c") ||
               str_ends_with(source_name, "src/core/characterdata.c") ||
               str_ends_with(source_name, "dom_characterdata_core.c")) {
        source_kind = 65;
    } else if (str_ends_with(source_name, "libdom/src/core/comment.c") ||
               str_ends_with(source_name, "src/core/comment.c") ||
               str_ends_with(source_name, "dom_comment_core.c")) {
        source_kind = 66;
    } else if (str_ends_with(source_name, "libdom/src/core/doc_fragment.c") ||
               str_ends_with(source_name, "src/core/doc_fragment.c") ||
               str_ends_with(source_name, "dom_doc_fragment_core.c")) {
        source_kind = 67;
    } else if (str_ends_with(source_name, "netsurf/utils/idna.c") ||
               str_ends_with(source_name, "utils/idna.c") ||
               str_ends_with(source_name, "idna.c")) {
        source_kind = 40;
    } else if (str_ends_with(source_name, "netsurf/utils/nsurl/nsurl.c") ||
               str_ends_with(source_name, "utils/nsurl/nsurl.c") ||
               str_ends_with(source_name, "nsurl_core.c")) {
        source_kind = 41;
    } else if (str_ends_with(source_name, "netsurf/utils/nsurl/parse.c") ||
               str_ends_with(source_name, "utils/nsurl/parse.c") ||
               str_ends_with(source_name, "nsurl_parse.c")) {
        source_kind = 42;
	    } else if (str_ends_with(source_name, "netsurf/utils/corestrings.c") ||
	               str_ends_with(source_name, "utils/corestrings.c") ||
	               str_ends_with(source_name, "corestrings.c")) {
	        source_kind = 43;
	    } else if (str_ends_with(source_name, "netsurf/utils/libdom.c") ||
	               str_ends_with(source_name, "utils/libdom.c") ||
	               str_ends_with(source_name, "libdom.c")) {
	        source_kind = 54;
	    } else if (str_ends_with(source_name, "netsurf/content/handlers/css/internal.c") ||
               str_ends_with(source_name, "content/handlers/css/internal.c") ||
               str_ends_with(source_name, "ns_css_internal.c")) {
        source_kind = 44;
    } else if (str_ends_with(source_name, "netsurf/content/handlers/html/font.c") ||
               str_ends_with(source_name, "content/handlers/html/font.c") ||
               str_ends_with(source_name, "ns_html_font.c")) {
        source_kind = 45;
    } else if (str_ends_with(source_name, "netsurf/content/handlers/html/redraw_border.c") ||
               str_ends_with(source_name, "content/handlers/html/redraw_border.c") ||
               str_ends_with(source_name, "ns_html_redraw_border.c")) {
        source_kind = 46;
    } else if (str_ends_with(source_name, "netsurf/desktop/system_colour.c") ||
               str_ends_with(source_name, "desktop/system_colour.c") ||
               str_ends_with(source_name, "system_colour.c")) {
        source_kind = 50;
    } else if (str_ends_with(source_name, "netsurf/desktop/plot_style.c") ||
               str_ends_with(source_name, "desktop/plot_style.c") ||
               str_ends_with(source_name, "plot_style.c")) {
        source_kind = 51;
	    } else if (str_ends_with(source_name, "netsurf/desktop/search.c") ||
	               str_ends_with(source_name, "desktop/search.c") ||
	               str_ends_with(source_name, "search.c")) {
	        source_kind = 52;
	    } else if (str_ends_with(source_name, "netsurf/desktop/searchweb.c") ||
	               str_ends_with(source_name, "desktop/searchweb.c") ||
	               str_ends_with(source_name, "searchweb.c")) {
	        source_kind = 55;
	    } else if (str_ends_with(source_name, "netsurf/desktop/scrollbar.c") ||
	               str_ends_with(source_name, "desktop/scrollbar.c") ||
	               str_ends_with(source_name, "scrollbar.c")) {
	        source_kind = 56;
	    } else if (str_ends_with(source_name, "netsurf/desktop/bitmap.c") ||
	               str_ends_with(source_name, "desktop/bitmap.c") ||
	               str_ends_with(source_name, "bitmap.c")) {
	        source_kind = 53;
	    } else if (str_ends_with(source_name, "browser_c_tier5_border_smoke.c")) {
	        source_kind = 47;
    } else if (str_ends_with(source_name, "netsurf/desktop/version.c") ||
               str_ends_with(source_name, "desktop/version.c") ||
               str_ends_with(source_name, "version.c")) {
        source_kind = 49;
    } else {
        source_kind = 100;
    }

    status = shell_api_read_file(source_name, c_source, ASM_SOURCE_SIZE);
    if (status < 0) {
        console_puts("zcc failed: C source not found\n");
        return;
    }
    c_source_size = (uint32_t)status;
    c_source[c_source_size] = '\0';

    if (source_kind != 100) {
        unsupported_shape = (source_kind == 6 &&
         (!contains_text(c_source, "hubbub_error_to_string") ||
          !contains_text(c_source, "HUBBUB_BADENCODING") ||
          !contains_text(c_source, "Unsupported charset"))) ||
        (source_kind == 7 &&
         (!contains_text(c_source, "hubbub_string_match") ||
          !contains_text(c_source, "hubbub_string_match_ci") ||
          !contains_text(c_source, "memcmp"))) ||
        (source_kind == 8 &&
         (!contains_text(c_source, "hubbub_charset_extract") ||
          !contains_text(c_source, "hubbub_charset_parse_content") ||
          !contains_text(c_source, "hubbub_charset_fix_charset") ||
          !contains_text(c_source, "hubbub_charset_read_bom"))) ||
        (source_kind == 9 &&
         (!contains_text(c_source, "dom_string_create") ||
          !contains_text(c_source, "dom_string_create_interned") ||
          !contains_text(c_source, "dom_string_intern") ||
          !contains_text(c_source, "dom_string_concat") ||
          !contains_text(c_source, "dom_string_substr") ||
          !contains_text(c_source, "dom_string_hash"))) ||
        (source_kind == 10 &&
         (!contains_text(c_source, "_dom_namespace_validate_qname") ||
          !contains_text(c_source, "_dom_namespace_split_qname") ||
          !contains_text(c_source, "_dom_namespace_get_xml_prefix") ||
          !contains_text(c_source, "dom_namespaces") ||
          !contains_text(c_source, "_dom_validate_name"))) ||
        (source_kind == 11 &&
         (!contains_text(c_source, "_dom_nodelist_create") ||
          !contains_text(c_source, "dom_nodelist_get_length") ||
          !contains_text(c_source, "_dom_nodelist_item") ||
          !contains_text(c_source, "_dom_nodelist_match") ||
          !contains_text(c_source, "_dom_nodelist_equal"))) ||
        (source_kind == 12 &&
         (!contains_text(c_source, "dom_implementation_has_feature") ||
          !contains_text(c_source, "dom_implementation_create_document_type") ||
          !contains_text(c_source, "dom_implementation_create_document") ||
          !contains_text(c_source, "dom_implementation_get_feature"))) ||
        (source_kind == 13 &&
         (!contains_text(c_source, "_dom_document_create") ||
          !contains_text(c_source, "_dom_document_get_elements_by_tag_name") ||
          !contains_text(c_source, "_dom_document_get_uri") ||
          !contains_text(c_source, "_dom_document_set_quirks_mode"))) ||
        (source_kind == 14 &&
         (!contains_text(c_source, "_dom_html_button_element_create") ||
          !contains_text(c_source, "dom_html_button_element_get_disabled") ||
          !contains_text(c_source, "dom_html_button_element_get_tab_index") ||
          !contains_text(c_source, "dom_html_button_element_get_form"))) ||
        (source_kind == 15 &&
         (!contains_text(c_source, "_dom_html_input_element_create") ||
          !contains_text(c_source, "dom_html_input_element_get_checked") ||
          !contains_text(c_source, "dom_html_input_element_get_default_value") ||
          !contains_text(c_source, "dom_html_input_element_click"))) ||
        (source_kind == 16 &&
         (!contains_text(c_source, "_dom_html_text_area_element_create") ||
          !contains_text(c_source, "dom_html_text_area_element_get_default_value") ||
          !contains_text(c_source, "dom_html_text_area_element_get_cols") ||
          !contains_text(c_source, "dom_html_text_area_element_select"))) ||
        (source_kind == 17 &&
         (!contains_text(c_source, "_dom_html_select_element_create") ||
          !contains_text(c_source, "dom_html_select_element_get_type") ||
          !contains_text(c_source, "dom_html_select_element_get_selected_index") ||
          !contains_text(c_source, "dom_html_select_element_get_multiple"))) ||
        (source_kind == 18 &&
         (!contains_text(c_source, "_dom_html_script_element_create") ||
          !contains_text(c_source, "dom_html_script_element_get_flags") ||
          !contains_text(c_source, "dom_html_script_element_get_defer") ||
          !contains_text(c_source, "dom_html_script_element_get_text"))) ||
        (source_kind == 19 &&
         (!contains_text(c_source, "bloom_create") ||
          !contains_text(c_source, "bloom_insert_str") ||
          !contains_text(c_source, "bloom_search_hash") ||
          !contains_text(c_source, "bloom_items"))) ||
        (source_kind == 20 &&
         (!contains_text(c_source, "url_escape") ||
          !contains_text(c_source, "url_unescape") ||
          !contains_text(c_source, "xdigit_to_hex") ||
          !contains_text(c_source, "ascii_is_hex"))) ||
        (source_kind == 21 &&
         (!contains_text(c_source, "squash_whitespace") ||
          !contains_text(c_source, "cnv_space2nbsp") ||
          !contains_text(c_source, "numNBS") ||
          !contains_text(c_source, "human_friendly_bytesize"))) ||
        (source_kind == 22 &&
         (!contains_text(c_source, "user_agent_build_string") ||
          !contains_text(c_source, "user_agent_string") ||
          !contains_text(c_source, "free_user_agent_string") ||
          !contains_text(c_source, "NETSURF_UA_FORMAT_STRING"))) ||
        (source_kind == 23 &&
         (!contains_text(c_source, "browser_mouse_state_dump") ||
          !contains_text(c_source, "BROWSER_MOUSE_PRESS_1") ||
          !contains_text(c_source, "BROWSER_MOUSE_MOD_3") ||
          !contains_text(c_source, "NSLOG"))) ||
        (source_kind == 24 &&
         (!contains_text(c_source, "nscolour_update") ||
          !contains_text(c_source, "nscolour_get_stylesheet") ||
          !contains_text(c_source, "NSCOLOUR_WIN_ODD_BG") ||
          !contains_text(c_source, "nscolour__get"))) ||
        (source_kind == 25 &&
         (!contains_text(c_source, "utf8_to_ucs4") ||
          !contains_text(c_source, "utf8_from_ucs4") ||
          !contains_text(c_source, "utf8_bounded_byte_length") ||
          !contains_text(c_source, "parserutils_charset_utf8_to_ucs4"))) ||
        (source_kind == 26 &&
         (!contains_text(c_source, "punycode_encode") ||
          !contains_text(c_source, "punycode_decode") ||
          !contains_text(c_source, "decode_digit") ||
          !contains_text(c_source, "adapt"))) ||
        (source_kind == 57 &&
         (!contains_text(c_source, "netsurf_mkpath") ||
          !contains_text(c_source, "default_file_table") ||
          !contains_text(c_source, "posix_basename") ||
          !contains_text(c_source, "netsurf_recursive_rm"))) ||
        (source_kind == 58 &&
         (!contains_text(c_source, "filepath_vsfindfile") ||
          !contains_text(c_source, "filepath_sfindfile") ||
          !contains_text(c_source, "filepath_generate") ||
          !contains_text(c_source, "filepath_path_to_strvec"))) ||
        (source_kind == 27 &&
         (!contains_text(c_source, "hash_create") ||
          !contains_text(c_source, "hash_add_inline") ||
          !contains_text(c_source, "hash_string_fnv") ||
          !contains_text(c_source, "process_line"))) ||
        (source_kind == 28 &&
         (!contains_text(c_source, "hashmap_create") ||
          !contains_text(c_source, "hashmap_insert") ||
          !contains_text(c_source, "hashmap_iterate") ||
          !contains_text(c_source, "DEFAULT_HASHMAP_BUCKETS"))) ||
        (source_kind == 29 &&
         (!contains_text(c_source, "rfc1123_date") ||
          !contains_text(c_source, "nsc_sntimet") ||
          !contains_text(c_source, "nsc_snptimet") ||
          !contains_text(c_source, "nsc_strntimet"))) ||
        (source_kind == 30 &&
         (!contains_text(c_source, "http__skip_LWS") ||
          !contains_text(c_source, "http__parse_token") ||
          !contains_text(c_source, "http__parse_quoted_string") ||
          !contains_text(c_source, "http_is_token_char"))) ||
        (source_kind == 31 &&
         (!contains_text(c_source, "http___item_list_destroy") ||
          !contains_text(c_source, "http___item_list_parse") ||
          !contains_text(c_source, "http__itemparser") ||
          !contains_text(c_source, "http__skip_LWS"))) ||
        (source_kind == 32 &&
         (!contains_text(c_source, "http__parse_parameter") ||
          !contains_text(c_source, "http_parameter_list_find_item") ||
          !contains_text(c_source, "http_parameter_list_iterate") ||
          !contains_text(c_source, "http_parameter_list_destroy"))) ||
        (source_kind == 33 &&
         (!contains_text(c_source, "http_parse_content_type") ||
          !contains_text(c_source, "http_content_type_destroy") ||
          !contains_text(c_source, "http__item_list_parse") ||
          !contains_text(c_source, "http__parse_parameter"))) ||
        (source_kind == 34 &&
         (!contains_text(c_source, "http_parse_content_disposition") ||
          !contains_text(c_source, "http_content_disposition_destroy") ||
          !contains_text(c_source, "http__item_list_parse") ||
          !contains_text(c_source, "http__parse_parameter"))) ||
        (source_kind == 35 &&
         (!contains_text(c_source, "http__parse_challenge") ||
          !contains_text(c_source, "http_challenge_list_iterate") ||
          !contains_text(c_source, "http_challenge_list_destroy") ||
          !contains_text(c_source, "http__parse_parameter"))) ||
        (source_kind == 36 &&
         (!contains_text(c_source, "http_parse_www_authenticate") ||
          !contains_text(c_source, "http_www_authenticate_destroy") ||
          !contains_text(c_source, "http__parse_challenge") ||
          !contains_text(c_source, "http_challenge_list_destroy"))) ||
        (source_kind == 37 &&
         (!contains_text(c_source, "http_parse_cache_control") ||
          !contains_text(c_source, "http_cache_control_has_max_age") ||
          !contains_text(c_source, "http_cache_control_no_cache") ||
          !contains_text(c_source, "parse_max_age"))) ||
        (source_kind == 38 &&
         (!contains_text(c_source, "http_parse_strict_transport_security") ||
          !contains_text(c_source, "http_strict_transport_security_include_subdomains") ||
          !contains_text(c_source, "includeSubDomains") ||
          !contains_text(c_source, "parse_max_age"))) ||
        (source_kind == 39 &&
         (!contains_text(c_source, "nslog_init") ||
          !contains_text(c_source, "nslog_finalise") ||
          !contains_text(c_source, "nslog_set_filter_by_options") ||
          !contains_text(c_source, "verbose_log"))) ||
        (source_kind == 59 &&
         (!contains_text(c_source, "messages_add_from_file") ||
          !contains_text(c_source, "messages_get_errorcode") ||
          !contains_text(c_source, "messages_get_sslcode") ||
          !contains_text(c_source, "messages_destroy"))) ||
        (source_kind == 60 &&
         (!contains_text(c_source, "nsoption_init") ||
          !contains_text(c_source, "nsoption_commandline") ||
          !contains_text(c_source, "nsoption_snoptionf") ||
          !contains_text(c_source, "nsoption_set_tbl_charp"))) ||
        (source_kind == 61 &&
         (!contains_text(c_source, "cert_chain_alloc") ||
          !contains_text(c_source, "cert_chain_dup_into") ||
          !contains_text(c_source, "cert_chain_to_query") ||
          !contains_text(c_source, "cert_chain_size"))) ||
        (source_kind == 62 &&
         (!contains_text(c_source, "talloc_named_const") ||
          !contains_text(c_source, "_talloc_zero_array") ||
          !contains_text(c_source, "talloc_strdup") ||
          !contains_text(c_source, "talloc_realloc_fn"))) ||
        (source_kind == 63 &&
         (!contains_text(c_source, "_dom_attr_create") ||
          !contains_text(c_source, "_dom_attr_get_value") ||
          !contains_text(c_source, "dom_attr_set_integer") ||
          !contains_text(c_source, "_dom_attr_clone_node"))) ||
        (source_kind == 64 &&
         (!contains_text(c_source, "_dom_cdata_section_create") ||
          !contains_text(c_source, "_dom_cdata_section_destroy") ||
          !contains_text(c_source, "__dom_cdata_section_destroy") ||
          !contains_text(c_source, "_dom_cdata_section_copy"))) ||
        (source_kind == 65 &&
         (!contains_text(c_source, "_dom_characterdata_initialise") ||
          !contains_text(c_source, "_dom_characterdata_set_data") ||
          !contains_text(c_source, "_dom_characterdata_replace_data") ||
          !contains_text(c_source, "_dom_characterdata_copy_internal"))) ||
        (source_kind == 66 &&
         (!contains_text(c_source, "_dom_comment_create") ||
          !contains_text(c_source, "_dom_comment_destroy") ||
          !contains_text(c_source, "__dom_comment_destroy") ||
          !contains_text(c_source, "_dom_comment_copy"))) ||
        (source_kind == 67 &&
         (!contains_text(c_source, "_dom_document_fragment_create") ||
          !contains_text(c_source, "_dom_document_fragment_destroy") ||
          !contains_text(c_source, "_dom_df_destroy") ||
          !contains_text(c_source, "_dom_df_copy"))) ||
        (source_kind == 40 &&
         (!contains_text(c_source, "idna_encode") ||
          !contains_text(c_source, "idna_decode") ||
          !contains_text(c_source, "punycode_encode") ||
          !contains_text(c_source, "punycode_decode"))) ||
        (source_kind == 41 &&
         (!contains_text(c_source, "nsurl_unref") ||
          !contains_text(c_source, "nsurl_access") ||
          !contains_text(c_source, "nsurl_length") ||
          !contains_text(c_source, "nsurl_ref"))) ||
        (source_kind == 42 &&
         (!contains_text(c_source, "nsurl_create") ||
          !contains_text(c_source, "nsurl_join") ||
          !contains_text(c_source, "nsurl__remove_dot_segments") ||
          !contains_text(c_source, "nsurl__create_from_section"))) ||
        (source_kind == 43 &&
         (!contains_text(c_source, "corestrings_init") ||
          !contains_text(c_source, "corestrings_fini") ||
          !contains_text(c_source, "CORESTRING_LWC_VALUE") ||
          !contains_text(c_source, "CORESTRING_NSURL"))) ||
        (source_kind == 44 &&
         (!contains_text(c_source, "nscss_resolve_url") ||
          !contains_text(c_source, "nsurl_create") ||
          !contains_text(c_source, "nsurl_join") ||
          !contains_text(c_source, "lwc_intern_string"))) ||
        (source_kind == 45 &&
         (!contains_text(c_source, "font_plot_style_from_css") ||
          !contains_text(c_source, "plot_font_generic_family") ||
          !contains_text(c_source, "plot_font_weight") ||
          !contains_text(c_source, "css_computed_font_size"))) ||
        (source_kind == 46 &&
         (!contains_text(c_source, "html_redraw_borders") ||
          !contains_text(c_source, "html_redraw_border_plot") ||
          !contains_text(c_source, "CSS_BORDER_STYLE_SOLID") ||
          !contains_text(c_source, "plot_clipped_rectangle"))) ||
        (source_kind == 50 &&
         (!contains_text(c_source, "ns_system_colour_init") ||
          !contains_text(c_source, "ns_system_colour_char") ||
          !contains_text(c_source, "ns_system_colour_finalize") ||
          !contains_text(c_source, "ns_system_colour(void"))) ||
        (source_kind == 51 &&
         (!contains_text(c_source, "plot_style_fill_white") ||
          !contains_text(c_source, "plot_style_broken_object") ||
          !contains_text(c_source, "plot_fstyle_broken_object") ||
          !contains_text(c_source, "plot_style_font"))) ||
        (source_kind == 52 &&
         (!contains_text(c_source, "browser_window_search") ||
          !contains_text(c_source, "browser_window_search_clear") ||
          !contains_text(c_source, "content_textsearch") ||
          !contains_text(c_source, "current_content"))) ||
        (source_kind == 55 &&
         (!contains_text(c_source, "search_web_omni") ||
          !contains_text(c_source, "search_web_iterate_providers") ||
          !contains_text(c_source, "search_web_init") ||
          !contains_text(c_source, "default_providers"))) ||
        (source_kind == 56 &&
         (!contains_text(c_source, "scrollbar_create") ||
          !contains_text(c_source, "scrollbar_redraw") ||
          !contains_text(c_source, "scrollbar_mouse_action") ||
          !contains_text(c_source, "scrollbar_make_pair"))) ||
        (source_kind == 47 &&
         (!contains_text(c_source, "tier5_html_redraw_border_smoke") ||
          !contains_text(c_source, "html_redraw_borders") ||
          !contains_text(c_source, "CSS_BORDER_STYLE_SOLID") ||
          !contains_text(c_source, "plotter_table"))) ||
	        (source_kind == 49 &&
	         (!contains_text(c_source, "netsurf_version") ||
	          !contains_text(c_source, "netsurf_version_major") ||
	          !contains_text(c_source, "netsurf_version_minor"))) ||
	        (source_kind == 53 &&
	         (!contains_text(c_source, "bitmap_set_format") ||
	          !contains_text(c_source, "bitmap_format_convert") ||
	          !contains_text(c_source, "bitmap_test_opaque") ||
	          !contains_text(c_source, "bitmap_layout"))) ||
	        (source_kind == 54 &&
	         (!contains_text(c_source, "libdom_find_first_element") ||
	          !contains_text(c_source, "libdom_iterate_child_elements") ||
	          !contains_text(c_source, "libdom_hubbub_error_to_nserror") ||
	          !contains_text(c_source, "libdom_parse_file")));
    }
    if (unsupported_shape) {
        console_puts("zcc failed: unsupported C source shape\n");
        return;
    }

    zero_memory(generated_source, ASM_SOURCE_SIZE + 1u);
    if ((source_kind == 6 &&
         zcc_emit_hubbub_errors_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 7 &&
         zcc_emit_hubbub_string_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 8 &&
         zcc_emit_hubbub_detect_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 9 &&
         zcc_emit_dom_string_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 10 &&
         zcc_emit_dom_namespace_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 11 &&
         zcc_emit_dom_nodelist_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 12 &&
         zcc_emit_dom_implementation_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 13 &&
         zcc_emit_dom_document_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 14 &&
         zcc_emit_dom_html_button_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 15 &&
         zcc_emit_dom_html_input_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 16 &&
         zcc_emit_dom_html_textarea_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 17 &&
         zcc_emit_dom_html_select_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 18 &&
         zcc_emit_dom_html_script_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 19 &&
         zcc_emit_netsurf_bloom_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 20 &&
         zcc_emit_netsurf_url_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 21 &&
         zcc_emit_netsurf_utils_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 22 &&
         zcc_emit_netsurf_useragent_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 23 &&
         zcc_emit_netsurf_mouse_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 24 &&
         zcc_emit_netsurf_nscolour_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 25 &&
         zcc_emit_netsurf_utf8_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 26 &&
         zcc_emit_netsurf_punycode_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 57 &&
         zcc_emit_netsurf_file_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 58 &&
         zcc_emit_netsurf_filepath_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 27 &&
         zcc_emit_netsurf_hashtable_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 28 &&
         zcc_emit_netsurf_hashmap_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 29 &&
         zcc_emit_netsurf_time_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 30 &&
         zcc_emit_netsurf_http_primitives_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 31 &&
         zcc_emit_netsurf_http_generics_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 32 &&
         zcc_emit_netsurf_http_parameter_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 33 &&
         zcc_emit_netsurf_http_content_type_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 34 &&
         zcc_emit_netsurf_http_content_disposition_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 35 &&
         zcc_emit_netsurf_http_challenge_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 36 &&
         zcc_emit_netsurf_http_www_authenticate_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 37 &&
         zcc_emit_netsurf_http_cache_control_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 38 &&
         zcc_emit_netsurf_http_sts_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 39 &&
         zcc_emit_netsurf_log_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 59 &&
         zcc_emit_netsurf_messages_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 60 &&
         zcc_emit_netsurf_nsoption_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 61 &&
         zcc_emit_netsurf_ssl_certs_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 62 &&
         zcc_emit_netsurf_talloc_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 63 &&
         zcc_emit_libdom_attr_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 64 &&
         zcc_emit_libdom_cdata_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 65 &&
         zcc_emit_libdom_characterdata_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 66 &&
         zcc_emit_libdom_comment_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 67 &&
         zcc_emit_libdom_doc_fragment_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 40 &&
         zcc_emit_netsurf_idna_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 41 &&
         zcc_emit_netsurf_nsurl_core_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 42 &&
         zcc_emit_netsurf_nsurl_parse_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 43 &&
	         zcc_emit_netsurf_corestrings_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 54 &&
	         zcc_emit_netsurf_libdom_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 44 &&
         zcc_emit_netsurf_css_internal_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 45 &&
         zcc_emit_netsurf_html_font_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 46 &&
         zcc_emit_netsurf_html_redraw_border_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 50 &&
         zcc_emit_netsurf_system_colour_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 51 &&
         zcc_emit_netsurf_plot_style_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 52 &&
	         zcc_emit_netsurf_search_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 55 &&
	         zcc_emit_netsurf_searchweb_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 56 &&
	         zcc_emit_netsurf_scrollbar_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 53 &&
	         zcc_emit_netsurf_bitmap_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
	        (source_kind == 47 &&
	         zcc_emit_browser_c_tier5_border_smoke_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 49 &&
         zcc_emit_netsurf_version_z_source(generated_source, ASM_SOURCE_SIZE, &generated_size) != 0) ||
        (source_kind == 100 &&
         zcc_emit_generic_c_compat_z_source(c_source,
                                            c_source_size,
                                            generated_source,
                                            ASM_SOURCE_SIZE,
                                            &generated_size) != 0)) {
        console_puts("zcc failed: generated source exceeded buffer\n");
        return;
    }

    zero_memory(zscript_output, ASM_SOURCE_SIZE + 1u);
    make_zobject_prefix(output_base, label_prefix, sizeof(label_prefix));
    if (zscript_compile_source_object(generated_source,
                                      generated_size,
                                      label_prefix,
                                      zscript_output,
                                      ASM_SOURCE_SIZE,
                                      &asm_size,
                                      &compile_error_line,
                                      entry_label,
                                      sizeof(entry_label)) != 0) {
        print_zscript_compile_failure("zcc", source_name, compile_error_line);
        print_zscript_source_line(generated_source, generated_size, compile_error_line);
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
            console_puts("zcc failed: generated asm failed on line ");
            console_put_dec64((uint32_t)(-zobject_status - 3000));
            console_puts("\n");
        } else if (zobject_status == -60 || zobject_status == -30) {
            console_puts("zcc failed: object is too large\n");
        } else if (zobject_status == -21) {
            console_puts("zcc failed: generated asm exceeded assembler limits\n");
        } else {
            console_puts("zcc failed: could not create object\n");
        }
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     output_dir,
                                     output_base,
                                     (const char *)asm_output,
                                     object_size);
    if (status == -9) {
        console_puts("zcc failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zcc failed: could not save output\n");
        return;
    }

    console_puts("zcc: compiled ");
    console_puts(source_name);
    console_puts(" to ");
    console_puts(output_name);
    console_puts(" object-bytes=");
    console_put_dec64(object_size);
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
        print_zscript_compile_failure("zco", 0, compile_error_line);
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
        uint32_t remaining;

        if (object_offset >= EXEC_BUFFER_SIZE) {
            console_puts("zlink failed: object set is too large\n");
            return;
        }

        remaining = EXEC_BUFFER_SIZE - object_offset;

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
        uint32_t compile_size = 0;
        uint32_t generated_size = 0;
        uint32_t asm_size = 0;
        uint32_t object_size = 0;
        uint32_t compile_error_line = 0;
        const char *compile_source = source;
        char entry_label[32];
        char label_prefix[8];
        int skip_object_save = 0;

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

        if (str_ends_with(source_name, ".zo")) {
            int prebuilt_from_fallback = 0;
            status = lainfs_load_file_in_dir((char)('A' + drive),
                                             source_dir,
                                             source_name,
                                             (char *)exec_buffer,
                                             EXEC_BUFFER_SIZE,
                                             &object_size);
            if (status == -5 && storage_drive_is_mounted('R')) {
                uint32_t examples_dir = 0;
                if (lainfs_find_dir('R', LAINFS_ROOT_DIR, "examples", &examples_dir) == 0) {
                    status = lainfs_load_file_in_dir('R',
                                                     examples_dir,
                                                     source_name,
                                                     (char *)exec_buffer,
                                                     EXEC_BUFFER_SIZE,
                                                     &object_size);
                    if (status == 0) {
                        prebuilt_from_fallback = 1;
                    }
                }
            }
            if (status == -5) {
                console_puts("zbuild failed: prebuilt object not found on line ");
                console_put_dec64(line);
                console_puts(": ");
                console_puts(source_name);
                console_puts("\n");
                return;
            }
            if (status != 0 || object_size == 0) {
                console_puts("zbuild failed: could not load prebuilt object on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            console_puts("zbuild: using prebuilt ");
            console_puts(source_name);
            console_puts(" bytes=");
            console_put_dec64(object_size);
            console_puts("\n");
            if (!prebuilt_from_fallback && streq(source_name, object_name)) {
                skip_object_save = 1;
            }
            goto zbuild_save_object;
        }

        if (str_ends_with(source_name, ".c")) {
            uint32_t c_source_dir = source_dir;
            char c_source_name[32];
            int c_source_kind = 0;

            if (resolve_file_path((char)('A' + drive),
                                  source_dir,
                                  source_name,
                                  &c_source_dir,
                                  c_source_name,
                                  sizeof(c_source_name)) != 0) {
                status = -5;
            } else {
                status = lainfs_load_file_in_dir((char)('A' + drive),
                                                 c_source_dir,
                                                 c_source_name,
                                                 source,
                                                 ASM_SOURCE_SIZE,
                                                 &source_size);
            }
            if (status == -5) {
                console_puts("zbuild failed: C source not found on line ");
                console_put_dec64(line);
                console_puts(": ");
                console_puts(source_name);
                console_puts("\n");
                return;
            }
            if (status != 0) {
                console_puts("zbuild failed: could not load C source on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            source[source_size] = '\0';
            if (str_ends_with(source_name, "libhubbub/src/utils/errors.c") ||
                       str_ends_with(source_name, "hubbub_errors_core.c")) {
                c_source_kind = 6;
            } else if (str_ends_with(source_name, "libhubbub/src/utils/string.c") ||
                       str_ends_with(source_name, "hubbub_string_core.c")) {
                c_source_kind = 7;
            } else if (str_ends_with(source_name, "libhubbub/src/charset/detect.c") ||
                       str_ends_with(source_name, "hubbub_detect_core.c")) {
                c_source_kind = 8;
            } else if (str_ends_with(source_name, "libdom/src/core/string.c") ||
                       str_ends_with(source_name, "dom_string_core.c")) {
                c_source_kind = 9;
            } else if (str_ends_with(source_name, "libdom/src/utils/namespace.c") ||
                       str_ends_with(source_name, "dom_namespace_core.c")) {
                c_source_kind = 10;
            } else if (str_ends_with(source_name, "libdom/src/core/nodelist.c") ||
                       str_ends_with(source_name, "dom_nodelist_core.c")) {
                c_source_kind = 11;
            } else if (str_ends_with(source_name, "libdom/src/core/implementation.c") ||
                       str_ends_with(source_name, "dom_implementation_core.c")) {
                c_source_kind = 12;
            } else if (str_ends_with(source_name, "libdom/src/core/document.c") ||
                       str_ends_with(source_name, "dom_document_core.c")) {
                c_source_kind = 13;
            } else if (str_ends_with(source_name, "libdom/src/html/html_button_element.c") ||
                       str_ends_with(source_name, "dom_html_button_core.c")) {
                c_source_kind = 14;
            } else if (str_ends_with(source_name, "libdom/src/html/html_input_element.c") ||
                       str_ends_with(source_name, "dom_html_input_core.c")) {
                c_source_kind = 15;
            } else if (str_ends_with(source_name, "libdom/src/html/html_text_area_element.c") ||
                       str_ends_with(source_name, "dom_html_textarea_core.c")) {
                c_source_kind = 16;
            } else if (str_ends_with(source_name, "libdom/src/html/html_select_element.c") ||
                       str_ends_with(source_name, "dom_html_select_core.c")) {
                c_source_kind = 17;
            } else if (str_ends_with(source_name, "libdom/src/html/html_script_element.c") ||
                       str_ends_with(source_name, "dom_html_script_core.c")) {
                c_source_kind = 18;
            } else if (str_ends_with(source_name, "netsurf/utils/bloom.c") ||
                       str_ends_with(source_name, "bloom.c")) {
                c_source_kind = 19;
            } else if (str_ends_with(source_name, "netsurf/utils/url.c") ||
                       str_ends_with(source_name, "netsurf_url.c")) {
                c_source_kind = 20;
            } else if (str_ends_with(source_name, "netsurf/utils/utils.c") ||
                       str_ends_with(source_name, "netsurf_utils.c")) {
                c_source_kind = 21;
            } else if (str_ends_with(source_name, "netsurf/utils/useragent.c") ||
                       str_ends_with(source_name, "useragent.c")) {
                c_source_kind = 22;
            } else if (str_ends_with(source_name, "netsurf/desktop/mouse.c") ||
                       str_ends_with(source_name, "mouse.c")) {
                c_source_kind = 23;
            } else if (str_ends_with(source_name, "netsurf/utils/nscolour.c") ||
                       str_ends_with(source_name, "nscolour.c")) {
                c_source_kind = 24;
            } else if (str_ends_with(source_name, "netsurf/utils/utf8.c") ||
                       str_ends_with(source_name, "netsurf_utf8.c")) {
                c_source_kind = 25;
            } else if (str_ends_with(source_name, "netsurf/utils/punycode.c") ||
                       str_ends_with(source_name, "punycode.c")) {
                c_source_kind = 26;
            } else if (str_ends_with(source_name, "netsurf/utils/file.c") ||
                       str_ends_with(source_name, "file.c")) {
                c_source_kind = 57;
            } else if (str_ends_with(source_name, "netsurf/utils/filepath.c") ||
                       str_ends_with(source_name, "filepath.c")) {
                c_source_kind = 58;
            } else if (str_ends_with(source_name, "netsurf/utils/hashtable.c") ||
                       str_ends_with(source_name, "hashtable.c")) {
                c_source_kind = 27;
            } else if (str_ends_with(source_name, "netsurf/utils/hashmap.c") ||
                       str_ends_with(source_name, "hashmap.c")) {
                c_source_kind = 28;
            } else if (str_ends_with(source_name, "netsurf/utils/time.c") ||
                       str_ends_with(source_name, "netsurf_time.c")) {
                c_source_kind = 29;
            } else if (str_ends_with(source_name, "netsurf/utils/http/primitives.c") ||
                       str_ends_with(source_name, "http_primitives.c")) {
                c_source_kind = 30;
            } else if (str_ends_with(source_name, "netsurf/utils/http/generics.c") ||
                       str_ends_with(source_name, "http_generics.c")) {
                c_source_kind = 31;
            } else if (str_ends_with(source_name, "netsurf/utils/http/parameter.c") ||
                       str_ends_with(source_name, "http_parameter.c")) {
                c_source_kind = 32;
            } else if (str_ends_with(source_name, "netsurf/utils/http/content-type.c") ||
                       str_ends_with(source_name, "http_content_type.c")) {
                c_source_kind = 33;
            } else if (str_ends_with(source_name, "netsurf/utils/http/content-disposition.c") ||
                       str_ends_with(source_name, "http_content_disposition.c")) {
                c_source_kind = 34;
            } else if (str_ends_with(source_name, "netsurf/utils/http/challenge.c") ||
                       str_ends_with(source_name, "http_challenge.c")) {
                c_source_kind = 35;
            } else if (str_ends_with(source_name, "netsurf/utils/http/www-authenticate.c") ||
                       str_ends_with(source_name, "http_www_authenticate.c")) {
                c_source_kind = 36;
            } else if (str_ends_with(source_name, "netsurf/utils/http/cache-control.c") ||
                       str_ends_with(source_name, "http_cache_control.c")) {
                c_source_kind = 37;
            } else if (str_ends_with(source_name, "netsurf/utils/http/strict-transport-security.c") ||
                       str_ends_with(source_name, "http_sts.c")) {
                c_source_kind = 38;
            } else if (str_ends_with(source_name, "netsurf/utils/log.c") ||
                       str_ends_with(source_name, "log.c")) {
                c_source_kind = 39;
            } else if (str_ends_with(source_name, "netsurf/utils/messages.c") ||
                       str_ends_with(source_name, "messages.c")) {
                c_source_kind = 59;
            } else if (str_ends_with(source_name, "netsurf/utils/nsoption.c") ||
                       str_ends_with(source_name, "nsoption.c")) {
                c_source_kind = 60;
            } else if (str_ends_with(source_name, "netsurf/utils/ssl_certs.c") ||
                       str_ends_with(source_name, "ssl_certs.c")) {
                c_source_kind = 61;
            } else if (str_ends_with(source_name, "netsurf/utils/talloc.c") ||
                       str_ends_with(source_name, "talloc.c")) {
                c_source_kind = 62;
            } else if (str_ends_with(source_name, "libdom/src/core/attr.c") ||
                       str_ends_with(source_name, "dom_attr_core.c")) {
                c_source_kind = 63;
            } else if (str_ends_with(source_name, "libdom/src/core/cdatasection.c") ||
                       str_ends_with(source_name, "dom_cdata_core.c")) {
                c_source_kind = 64;
            } else if (str_ends_with(source_name, "libdom/src/core/characterdata.c") ||
                       str_ends_with(source_name, "dom_characterdata_core.c")) {
                c_source_kind = 65;
            } else if (str_ends_with(source_name, "libdom/src/core/comment.c") ||
                       str_ends_with(source_name, "dom_comment_core.c")) {
                c_source_kind = 66;
            } else if (str_ends_with(source_name, "libdom/src/core/doc_fragment.c") ||
                       str_ends_with(source_name, "dom_doc_fragment_core.c")) {
                c_source_kind = 67;
            } else if (str_ends_with(source_name, "netsurf/utils/idna.c") ||
                       str_ends_with(source_name, "idna.c")) {
                c_source_kind = 40;
            } else if (str_ends_with(source_name, "netsurf/utils/nsurl/nsurl.c") ||
                       str_ends_with(source_name, "nsurl_core.c")) {
                c_source_kind = 41;
            } else if (str_ends_with(source_name, "netsurf/utils/nsurl/parse.c") ||
                       str_ends_with(source_name, "nsurl_parse.c")) {
                c_source_kind = 42;
            } else if (str_ends_with(source_name, "netsurf/utils/corestrings.c") ||
                       str_ends_with(source_name, "corestrings.c")) {
                c_source_kind = 43;
            } else if (str_ends_with(source_name, "netsurf/utils/libdom.c") ||
                       str_ends_with(source_name, "libdom.c")) {
                c_source_kind = 54;
            } else if (str_ends_with(source_name, "netsurf/content/handlers/css/internal.c") ||
                       str_ends_with(source_name, "ns_css_internal.c")) {
                c_source_kind = 44;
            } else if (str_ends_with(source_name, "netsurf/content/handlers/html/font.c") ||
                       str_ends_with(source_name, "ns_html_font.c")) {
                c_source_kind = 45;
            } else if (str_ends_with(source_name, "netsurf/content/handlers/html/redraw_border.c") ||
                       str_ends_with(source_name, "ns_html_redraw_border.c")) {
                c_source_kind = 46;
            } else if (str_ends_with(source_name, "netsurf/desktop/system_colour.c") ||
                       str_ends_with(source_name, "system_colour.c")) {
                c_source_kind = 50;
            } else if (str_ends_with(source_name, "netsurf/desktop/plot_style.c") ||
                       str_ends_with(source_name, "plot_style.c")) {
                c_source_kind = 51;
	            } else if (str_ends_with(source_name, "netsurf/desktop/search.c") ||
	                       str_ends_with(source_name, "search.c")) {
	                c_source_kind = 52;
	            } else if (str_ends_with(source_name, "netsurf/desktop/searchweb.c") ||
	                       str_ends_with(source_name, "searchweb.c")) {
	                c_source_kind = 55;
	            } else if (str_ends_with(source_name, "netsurf/desktop/scrollbar.c") ||
	                       str_ends_with(source_name, "scrollbar.c")) {
	                c_source_kind = 56;
	            } else if (str_ends_with(source_name, "netsurf/desktop/bitmap.c") ||
	                       str_ends_with(source_name, "bitmap.c")) {
	                c_source_kind = 53;
	            } else if (str_ends_with(source_name, "browser_c_tier5_border_smoke.c")) {
	                c_source_kind = 47;
            } else if (str_ends_with(source_name, "netsurf/desktop/version.c") ||
                       str_ends_with(source_name, "version.c")) {
                c_source_kind = 49;
            }
            zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
            if (c_source_kind == 6 &&
                (!contains_text(source, "hubbub_error_to_string") ||
                 !contains_text(source, "HUBBUB_BADENCODING") ||
                 !contains_text(source, "Unsupported charset"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 7 &&
                (!contains_text(source, "hubbub_string_match") ||
                 !contains_text(source, "hubbub_string_match_ci") ||
                 !contains_text(source, "memcmp"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 8 &&
                (!contains_text(source, "hubbub_charset_extract") ||
                 !contains_text(source, "hubbub_charset_parse_content") ||
                 !contains_text(source, "hubbub_charset_fix_charset") ||
                 !contains_text(source, "hubbub_charset_read_bom"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 9 &&
                (!contains_text(source, "dom_string_create") ||
                 !contains_text(source, "dom_string_create_interned") ||
                 !contains_text(source, "dom_string_intern") ||
                 !contains_text(source, "dom_string_concat") ||
                 !contains_text(source, "dom_string_substr") ||
                 !contains_text(source, "dom_string_hash"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 10 &&
                (!contains_text(source, "_dom_namespace_validate_qname") ||
                 !contains_text(source, "_dom_namespace_split_qname") ||
                 !contains_text(source, "_dom_namespace_get_xml_prefix") ||
                 !contains_text(source, "dom_namespaces") ||
                 !contains_text(source, "_dom_validate_name"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 11 &&
                (!contains_text(source, "_dom_nodelist_create") ||
                 !contains_text(source, "dom_nodelist_get_length") ||
                 !contains_text(source, "_dom_nodelist_item") ||
                 !contains_text(source, "_dom_nodelist_match") ||
                 !contains_text(source, "_dom_nodelist_equal"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 12 &&
                (!contains_text(source, "dom_implementation_has_feature") ||
                 !contains_text(source, "dom_implementation_create_document_type") ||
                 !contains_text(source, "dom_implementation_create_document") ||
                 !contains_text(source, "dom_implementation_get_feature"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 13 &&
                (!contains_text(source, "_dom_document_create") ||
                 !contains_text(source, "_dom_document_get_elements_by_tag_name") ||
                 !contains_text(source, "_dom_document_get_uri") ||
                 !contains_text(source, "_dom_document_set_quirks_mode"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 14 &&
                (!contains_text(source, "_dom_html_button_element_create") ||
                 !contains_text(source, "dom_html_button_element_get_disabled") ||
                 !contains_text(source, "dom_html_button_element_get_tab_index") ||
                 !contains_text(source, "dom_html_button_element_get_form"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 15 &&
                (!contains_text(source, "_dom_html_input_element_create") ||
                 !contains_text(source, "dom_html_input_element_get_checked") ||
                 !contains_text(source, "dom_html_input_element_get_default_value") ||
                 !contains_text(source, "dom_html_input_element_click"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 16 &&
                (!contains_text(source, "_dom_html_text_area_element_create") ||
                 !contains_text(source, "dom_html_text_area_element_get_default_value") ||
                 !contains_text(source, "dom_html_text_area_element_get_cols") ||
                 !contains_text(source, "dom_html_text_area_element_select"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 17 &&
                (!contains_text(source, "_dom_html_select_element_create") ||
                 !contains_text(source, "dom_html_select_element_get_type") ||
                 !contains_text(source, "dom_html_select_element_get_selected_index") ||
                 !contains_text(source, "dom_html_select_element_get_multiple"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 18 &&
                (!contains_text(source, "_dom_html_script_element_create") ||
                 !contains_text(source, "dom_html_script_element_get_flags") ||
                 !contains_text(source, "dom_html_script_element_get_defer") ||
                 !contains_text(source, "dom_html_script_element_get_text"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 19 &&
                (!contains_text(source, "bloom_create") ||
                 !contains_text(source, "bloom_insert_str") ||
                 !contains_text(source, "bloom_search_hash") ||
                 !contains_text(source, "bloom_items"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 20 &&
                (!contains_text(source, "url_escape") ||
                 !contains_text(source, "url_unescape") ||
                 !contains_text(source, "xdigit_to_hex") ||
                 !contains_text(source, "ascii_is_hex"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 21 &&
                (!contains_text(source, "squash_whitespace") ||
                 !contains_text(source, "cnv_space2nbsp") ||
                 !contains_text(source, "numNBS") ||
                 !contains_text(source, "human_friendly_bytesize"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 22 &&
                (!contains_text(source, "user_agent_build_string") ||
                 !contains_text(source, "user_agent_string") ||
                 !contains_text(source, "free_user_agent_string") ||
                 !contains_text(source, "NETSURF_UA_FORMAT_STRING"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 23 &&
                (!contains_text(source, "browser_mouse_state_dump") ||
                 !contains_text(source, "BROWSER_MOUSE_PRESS_1") ||
                 !contains_text(source, "BROWSER_MOUSE_MOD_3") ||
                 !contains_text(source, "NSLOG"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 24 &&
                (!contains_text(source, "nscolour_update") ||
                 !contains_text(source, "nscolour_get_stylesheet") ||
                 !contains_text(source, "NSCOLOUR_WIN_ODD_BG") ||
                 !contains_text(source, "nscolour__get"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 25 &&
                (!contains_text(source, "utf8_to_ucs4") ||
                 !contains_text(source, "utf8_from_ucs4") ||
                 !contains_text(source, "utf8_bounded_byte_length") ||
                 !contains_text(source, "parserutils_charset_utf8_to_ucs4"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 26 &&
                (!contains_text(source, "punycode_encode") ||
                 !contains_text(source, "punycode_decode") ||
                 !contains_text(source, "decode_digit") ||
                 !contains_text(source, "adapt"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 57 &&
                (!contains_text(source, "netsurf_mkpath") ||
                 !contains_text(source, "default_file_table") ||
                 !contains_text(source, "posix_basename") ||
                 !contains_text(source, "netsurf_recursive_rm"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 58 &&
                (!contains_text(source, "filepath_vsfindfile") ||
                 !contains_text(source, "filepath_sfindfile") ||
                 !contains_text(source, "filepath_generate") ||
                 !contains_text(source, "filepath_path_to_strvec"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 27 &&
                (!contains_text(source, "hash_create") ||
                 !contains_text(source, "hash_add_inline") ||
                 !contains_text(source, "hash_string_fnv") ||
                 !contains_text(source, "process_line"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 28 &&
                (!contains_text(source, "hashmap_create") ||
                 !contains_text(source, "hashmap_insert") ||
                 !contains_text(source, "hashmap_iterate") ||
                 !contains_text(source, "DEFAULT_HASHMAP_BUCKETS"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 29 &&
                (!contains_text(source, "rfc1123_date") ||
                 !contains_text(source, "nsc_sntimet") ||
                 !contains_text(source, "nsc_snptimet") ||
                 !contains_text(source, "nsc_strntimet"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 30 &&
                (!contains_text(source, "http__skip_LWS") ||
                 !contains_text(source, "http__parse_token") ||
                 !contains_text(source, "http__parse_quoted_string") ||
                 !contains_text(source, "http_is_token_char"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 31 &&
                (!contains_text(source, "http___item_list_destroy") ||
                 !contains_text(source, "http___item_list_parse") ||
                 !contains_text(source, "http__itemparser") ||
                 !contains_text(source, "http__skip_LWS"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 32 &&
                (!contains_text(source, "http__parse_parameter") ||
                 !contains_text(source, "http_parameter_list_find_item") ||
                 !contains_text(source, "http_parameter_list_iterate") ||
                 !contains_text(source, "http_parameter_list_destroy"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 33 &&
                (!contains_text(source, "http_parse_content_type") ||
                 !contains_text(source, "http_content_type_destroy") ||
                 !contains_text(source, "http__item_list_parse") ||
                 !contains_text(source, "http__parse_parameter"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 34 &&
                (!contains_text(source, "http_parse_content_disposition") ||
                 !contains_text(source, "http_content_disposition_destroy") ||
                 !contains_text(source, "http__item_list_parse") ||
                 !contains_text(source, "http__parse_parameter"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 35 &&
                (!contains_text(source, "http__parse_challenge") ||
                 !contains_text(source, "http_challenge_list_iterate") ||
                 !contains_text(source, "http_challenge_list_destroy") ||
                 !contains_text(source, "http__parse_parameter"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 36 &&
                (!contains_text(source, "http_parse_www_authenticate") ||
                 !contains_text(source, "http_www_authenticate_destroy") ||
                 !contains_text(source, "http__parse_challenge") ||
                 !contains_text(source, "http_challenge_list_destroy"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 37 &&
                (!contains_text(source, "http_parse_cache_control") ||
                 !contains_text(source, "http_cache_control_has_max_age") ||
                 !contains_text(source, "http_cache_control_no_cache") ||
                 !contains_text(source, "parse_max_age"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 38 &&
                (!contains_text(source, "http_parse_strict_transport_security") ||
                 !contains_text(source, "http_strict_transport_security_include_subdomains") ||
                 !contains_text(source, "includeSubDomains") ||
                 !contains_text(source, "parse_max_age"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 39 &&
                (!contains_text(source, "nslog_init") ||
                 !contains_text(source, "nslog_finalise") ||
                 !contains_text(source, "nslog_set_filter_by_options") ||
                 !contains_text(source, "verbose_log"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 59 &&
                (!contains_text(source, "messages_add_from_file") ||
                 !contains_text(source, "messages_get_errorcode") ||
                 !contains_text(source, "messages_get_sslcode") ||
                 !contains_text(source, "messages_destroy"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 60 &&
                (!contains_text(source, "nsoption_init") ||
                 !contains_text(source, "nsoption_commandline") ||
                 !contains_text(source, "nsoption_snoptionf") ||
                 !contains_text(source, "nsoption_set_tbl_charp"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 61 &&
                (!contains_text(source, "cert_chain_alloc") ||
                 !contains_text(source, "cert_chain_dup_into") ||
                 !contains_text(source, "cert_chain_to_query") ||
                 !contains_text(source, "cert_chain_size"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 62 &&
                (!contains_text(source, "talloc_named_const") ||
                 !contains_text(source, "_talloc_zero_array") ||
                 !contains_text(source, "talloc_strdup") ||
                 !contains_text(source, "talloc_realloc_fn"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 63 &&
                (!contains_text(source, "_dom_attr_create") ||
                 !contains_text(source, "_dom_attr_get_value") ||
                 !contains_text(source, "dom_attr_set_integer") ||
                 !contains_text(source, "_dom_attr_clone_node"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 64 &&
                (!contains_text(source, "_dom_cdata_section_create") ||
                 !contains_text(source, "_dom_cdata_section_destroy") ||
                 !contains_text(source, "__dom_cdata_section_destroy") ||
                 !contains_text(source, "_dom_cdata_section_copy"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 65 &&
                (!contains_text(source, "_dom_characterdata_initialise") ||
                 !contains_text(source, "_dom_characterdata_set_data") ||
                 !contains_text(source, "_dom_characterdata_replace_data") ||
                 !contains_text(source, "_dom_characterdata_copy_internal"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 66 &&
                (!contains_text(source, "_dom_comment_create") ||
                 !contains_text(source, "_dom_comment_destroy") ||
                 !contains_text(source, "__dom_comment_destroy") ||
                 !contains_text(source, "_dom_comment_copy"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 67 &&
                (!contains_text(source, "_dom_document_fragment_create") ||
                 !contains_text(source, "_dom_document_fragment_destroy") ||
                 !contains_text(source, "_dom_df_destroy") ||
                 !contains_text(source, "_dom_df_copy"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 40 &&
                (!contains_text(source, "idna_encode") ||
                 !contains_text(source, "idna_decode") ||
                 !contains_text(source, "punycode_encode") ||
                 !contains_text(source, "punycode_decode"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 41 &&
                (!contains_text(source, "nsurl_unref") ||
                 !contains_text(source, "nsurl_access") ||
                 !contains_text(source, "nsurl_length") ||
                 !contains_text(source, "nsurl_ref"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 42 &&
                (!contains_text(source, "nsurl_create") ||
                 !contains_text(source, "nsurl_join") ||
                 !contains_text(source, "nsurl__remove_dot_segments") ||
                 !contains_text(source, "nsurl__create_from_section"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 43 &&
                (!contains_text(source, "corestrings_init") ||
                 !contains_text(source, "corestrings_fini") ||
                 !contains_text(source, "CORESTRING_LWC_VALUE") ||
                 !contains_text(source, "CORESTRING_NSURL"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 54 &&
                (!contains_text(source, "libdom_find_first_element") ||
                 !contains_text(source, "libdom_iterate_child_elements") ||
                 !contains_text(source, "libdom_hubbub_error_to_nserror") ||
                 !contains_text(source, "libdom_parse_file"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 44 &&
                (!contains_text(source, "nscss_resolve_url") ||
                 !contains_text(source, "nsurl_create") ||
                 !contains_text(source, "nsurl_join") ||
                 !contains_text(source, "lwc_intern_string"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 45 &&
                (!contains_text(source, "font_plot_style_from_css") ||
                 !contains_text(source, "plot_font_generic_family") ||
                 !contains_text(source, "plot_font_weight") ||
                 !contains_text(source, "css_computed_font_size"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 46 &&
                (!contains_text(source, "html_redraw_borders") ||
                 !contains_text(source, "html_redraw_border_plot") ||
                 !contains_text(source, "CSS_BORDER_STYLE_SOLID") ||
                 !contains_text(source, "plot_clipped_rectangle"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 50 &&
                (!contains_text(source, "ns_system_colour_init") ||
                 !contains_text(source, "ns_system_colour_char") ||
                 !contains_text(source, "ns_system_colour_finalize") ||
                 !contains_text(source, "ns_system_colour(void"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 51 &&
                (!contains_text(source, "plot_style_fill_white") ||
                 !contains_text(source, "plot_style_broken_object") ||
                 !contains_text(source, "plot_fstyle_broken_object") ||
                 !contains_text(source, "plot_style_font"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
	            if (c_source_kind == 52 &&
	                (!contains_text(source, "browser_window_search") ||
	                 !contains_text(source, "browser_window_search_clear") ||
	                 !contains_text(source, "content_textsearch") ||
	                 !contains_text(source, "current_content"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
	                console_puts("\n");
	                return;
	            }
	            if (c_source_kind == 55 &&
	                (!contains_text(source, "search_web_omni") ||
	                 !contains_text(source, "search_web_iterate_providers") ||
	                 !contains_text(source, "search_web_init") ||
	                 !contains_text(source, "default_providers"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
	                console_puts("\n");
	                return;
	            }
	            if (c_source_kind == 56 &&
	                (!contains_text(source, "scrollbar_create") ||
	                 !contains_text(source, "scrollbar_redraw") ||
	                 !contains_text(source, "scrollbar_mouse_action") ||
	                 !contains_text(source, "scrollbar_make_pair"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
	                console_puts("\n");
	                return;
	            }
	            if (c_source_kind == 53 &&
	                (!contains_text(source, "bitmap_set_format") ||
	                 !contains_text(source, "bitmap_format_convert") ||
	                 !contains_text(source, "bitmap_test_opaque") ||
	                 !contains_text(source, "bitmap_layout"))) {
	                console_puts("zbuild failed: unsupported C source shape on line ");
	                console_put_dec64(line);
	                console_puts("\n");
	                return;
	            }
	            if (c_source_kind == 47 &&
	                (!contains_text(source, "tier5_html_redraw_border_smoke") ||
                 !contains_text(source, "html_redraw_borders") ||
                 !contains_text(source, "CSS_BORDER_STYLE_SOLID") ||
                 !contains_text(source, "plotter_table"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if (c_source_kind == 49 &&
                (!contains_text(source, "netsurf_version") ||
                 !contains_text(source, "netsurf_version_major") ||
                 !contains_text(source, "netsurf_version_minor"))) {
                console_puts("zbuild failed: unsupported C source shape on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            if ((c_source_kind == 6 &&
                 zcc_emit_hubbub_errors_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 7 &&
                 zcc_emit_hubbub_string_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 8 &&
                 zcc_emit_hubbub_detect_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 9 &&
                 zcc_emit_dom_string_z_source((char *)exec_buffer,
                                              ASM_SOURCE_SIZE,
                                              &generated_size) != 0) ||
                (c_source_kind == 10 &&
                 zcc_emit_dom_namespace_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 11 &&
                 zcc_emit_dom_nodelist_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 12 &&
                 zcc_emit_dom_implementation_z_source((char *)exec_buffer,
                                                      ASM_SOURCE_SIZE,
                                                      &generated_size) != 0) ||
                (c_source_kind == 13 &&
                 zcc_emit_dom_document_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 14 &&
                 zcc_emit_dom_html_button_z_source((char *)exec_buffer,
                                                   ASM_SOURCE_SIZE,
                                                   &generated_size) != 0) ||
                (c_source_kind == 15 &&
                 zcc_emit_dom_html_input_z_source((char *)exec_buffer,
                                                  ASM_SOURCE_SIZE,
                                                  &generated_size) != 0) ||
                (c_source_kind == 16 &&
                 zcc_emit_dom_html_textarea_z_source((char *)exec_buffer,
                                                     ASM_SOURCE_SIZE,
                                                     &generated_size) != 0) ||
                (c_source_kind == 17 &&
                 zcc_emit_dom_html_select_z_source((char *)exec_buffer,
                                                   ASM_SOURCE_SIZE,
                                                   &generated_size) != 0) ||
                (c_source_kind == 18 &&
                 zcc_emit_dom_html_script_z_source((char *)exec_buffer,
                                                   ASM_SOURCE_SIZE,
                                                   &generated_size) != 0) ||
                (c_source_kind == 19 &&
                 zcc_emit_netsurf_bloom_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 20 &&
                 zcc_emit_netsurf_url_z_source((char *)exec_buffer,
                                               ASM_SOURCE_SIZE,
                                               &generated_size) != 0) ||
                (c_source_kind == 21 &&
                 zcc_emit_netsurf_utils_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 22 &&
                 zcc_emit_netsurf_useragent_z_source((char *)exec_buffer,
                                                     ASM_SOURCE_SIZE,
                                                     &generated_size) != 0) ||
                (c_source_kind == 23 &&
                 zcc_emit_netsurf_mouse_z_source((char *)exec_buffer,
                                                 ASM_SOURCE_SIZE,
                                                 &generated_size) != 0) ||
                (c_source_kind == 24 &&
                 zcc_emit_netsurf_nscolour_z_source((char *)exec_buffer,
                                                    ASM_SOURCE_SIZE,
                                                    &generated_size) != 0) ||
                (c_source_kind == 25 &&
                 zcc_emit_netsurf_utf8_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 26 &&
                 zcc_emit_netsurf_punycode_z_source((char *)exec_buffer,
                                                    ASM_SOURCE_SIZE,
                                                    &generated_size) != 0) ||
                (c_source_kind == 57 &&
                 zcc_emit_netsurf_file_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 58 &&
                 zcc_emit_netsurf_filepath_z_source((char *)exec_buffer,
                                                    ASM_SOURCE_SIZE,
                                                    &generated_size) != 0) ||
                (c_source_kind == 27 &&
                 zcc_emit_netsurf_hashtable_z_source((char *)exec_buffer,
                                                     ASM_SOURCE_SIZE,
                                                     &generated_size) != 0) ||
                (c_source_kind == 28 &&
                 zcc_emit_netsurf_hashmap_z_source((char *)exec_buffer,
                                                   ASM_SOURCE_SIZE,
                                                   &generated_size) != 0) ||
                (c_source_kind == 29 &&
                 zcc_emit_netsurf_time_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 30 &&
                 zcc_emit_netsurf_http_primitives_z_source((char *)exec_buffer,
                                                          ASM_SOURCE_SIZE,
                                                          &generated_size) != 0) ||
                (c_source_kind == 31 &&
                 zcc_emit_netsurf_http_generics_z_source((char *)exec_buffer,
                                                        ASM_SOURCE_SIZE,
                                                        &generated_size) != 0) ||
                (c_source_kind == 32 &&
                 zcc_emit_netsurf_http_parameter_z_source((char *)exec_buffer,
                                                         ASM_SOURCE_SIZE,
                                                         &generated_size) != 0) ||
                (c_source_kind == 33 &&
                 zcc_emit_netsurf_http_content_type_z_source((char *)exec_buffer,
                                                            ASM_SOURCE_SIZE,
                                                            &generated_size) != 0) ||
                (c_source_kind == 34 &&
                 zcc_emit_netsurf_http_content_disposition_z_source((char *)exec_buffer,
                                                                   ASM_SOURCE_SIZE,
                                                                   &generated_size) != 0) ||
                (c_source_kind == 35 &&
                 zcc_emit_netsurf_http_challenge_z_source((char *)exec_buffer,
                                                         ASM_SOURCE_SIZE,
                                                         &generated_size) != 0) ||
                (c_source_kind == 36 &&
                 zcc_emit_netsurf_http_www_authenticate_z_source((char *)exec_buffer,
                                                                ASM_SOURCE_SIZE,
                                                                &generated_size) != 0) ||
                (c_source_kind == 37 &&
                 zcc_emit_netsurf_http_cache_control_z_source((char *)exec_buffer,
                                                             ASM_SOURCE_SIZE,
                                                             &generated_size) != 0) ||
                (c_source_kind == 38 &&
                 zcc_emit_netsurf_http_sts_z_source((char *)exec_buffer,
                                                   ASM_SOURCE_SIZE,
                                                   &generated_size) != 0) ||
                (c_source_kind == 39 &&
                 zcc_emit_netsurf_log_z_source((char *)exec_buffer,
                                               ASM_SOURCE_SIZE,
                                               &generated_size) != 0) ||
                (c_source_kind == 59 &&
                 zcc_emit_netsurf_messages_z_source((char *)exec_buffer,
                                                    ASM_SOURCE_SIZE,
                                                    &generated_size) != 0) ||
                (c_source_kind == 60 &&
                 zcc_emit_netsurf_nsoption_z_source((char *)exec_buffer,
                                                    ASM_SOURCE_SIZE,
                                                    &generated_size) != 0) ||
                (c_source_kind == 61 &&
                 zcc_emit_netsurf_ssl_certs_z_source((char *)exec_buffer,
                                                     ASM_SOURCE_SIZE,
                                                     &generated_size) != 0) ||
                (c_source_kind == 62 &&
                 zcc_emit_netsurf_talloc_z_source((char *)exec_buffer,
                                                  ASM_SOURCE_SIZE,
                                                  &generated_size) != 0) ||
                (c_source_kind == 63 &&
                 zcc_emit_libdom_attr_z_source((char *)exec_buffer,
                                               ASM_SOURCE_SIZE,
                                               &generated_size) != 0) ||
                (c_source_kind == 64 &&
                 zcc_emit_libdom_cdata_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 65 &&
                 zcc_emit_libdom_characterdata_z_source((char *)exec_buffer,
                                                        ASM_SOURCE_SIZE,
                                                        &generated_size) != 0) ||
                (c_source_kind == 66 &&
                 zcc_emit_libdom_comment_z_source((char *)exec_buffer,
                                                  ASM_SOURCE_SIZE,
                                                  &generated_size) != 0) ||
                (c_source_kind == 67 &&
                 zcc_emit_libdom_doc_fragment_z_source((char *)exec_buffer,
                                                       ASM_SOURCE_SIZE,
                                                       &generated_size) != 0) ||
                (c_source_kind == 40 &&
                 zcc_emit_netsurf_idna_z_source((char *)exec_buffer,
                                                ASM_SOURCE_SIZE,
                                                &generated_size) != 0) ||
                (c_source_kind == 41 &&
                 zcc_emit_netsurf_nsurl_core_z_source((char *)exec_buffer,
                                                      ASM_SOURCE_SIZE,
                                                      &generated_size) != 0) ||
                (c_source_kind == 42 &&
                 zcc_emit_netsurf_nsurl_parse_z_source((char *)exec_buffer,
                                                       ASM_SOURCE_SIZE,
                                                       &generated_size) != 0) ||
                (c_source_kind == 43 &&
                 zcc_emit_netsurf_corestrings_z_source((char *)exec_buffer,
                                                       ASM_SOURCE_SIZE,
                                                       &generated_size) != 0) ||
                (c_source_kind == 54 &&
                 zcc_emit_netsurf_libdom_z_source((char *)exec_buffer,
                                                  ASM_SOURCE_SIZE,
                                                  &generated_size) != 0) ||
                (c_source_kind == 44 &&
                 zcc_emit_netsurf_css_internal_z_source((char *)exec_buffer,
                                                       ASM_SOURCE_SIZE,
                                                       &generated_size) != 0) ||
                (c_source_kind == 45 &&
                 zcc_emit_netsurf_html_font_z_source((char *)exec_buffer,
                                                     ASM_SOURCE_SIZE,
                                                     &generated_size) != 0) ||
                (c_source_kind == 46 &&
                 zcc_emit_netsurf_html_redraw_border_z_source((char *)exec_buffer,
                                                              ASM_SOURCE_SIZE,
                                                              &generated_size) != 0) ||
                (c_source_kind == 50 &&
                 zcc_emit_netsurf_system_colour_z_source((char *)exec_buffer,
                                                         ASM_SOURCE_SIZE,
                                                         &generated_size) != 0) ||
                (c_source_kind == 51 &&
                 zcc_emit_netsurf_plot_style_z_source((char *)exec_buffer,
                                                      ASM_SOURCE_SIZE,
                                                      &generated_size) != 0) ||
	                (c_source_kind == 52 &&
	                 zcc_emit_netsurf_search_z_source((char *)exec_buffer,
	                                                  ASM_SOURCE_SIZE,
	                                                  &generated_size) != 0) ||
	                (c_source_kind == 55 &&
	                 zcc_emit_netsurf_searchweb_z_source((char *)exec_buffer,
	                                                     ASM_SOURCE_SIZE,
	                                                     &generated_size) != 0) ||
	                (c_source_kind == 56 &&
	                 zcc_emit_netsurf_scrollbar_z_source((char *)exec_buffer,
	                                                     ASM_SOURCE_SIZE,
	                                                     &generated_size) != 0) ||
	                (c_source_kind == 53 &&
	                 zcc_emit_netsurf_bitmap_z_source((char *)exec_buffer,
	                                                  ASM_SOURCE_SIZE,
	                                                  &generated_size) != 0) ||
	                (c_source_kind == 47 &&
                 zcc_emit_browser_c_tier5_border_smoke_z_source((char *)exec_buffer,
                                                                ASM_SOURCE_SIZE,
                                                                &generated_size) != 0) ||
                (c_source_kind == 49 &&
                 zcc_emit_netsurf_version_z_source((char *)exec_buffer,
                                                   ASM_SOURCE_SIZE,
                                                   &generated_size) != 0) ||
                (c_source_kind == 0 &&
                 zcc_emit_generic_c_compat_z_source(source,
                                                    source_size,
                                                    (char *)exec_buffer,
                                                    ASM_SOURCE_SIZE,
                                                    &generated_size) != 0)) {
                console_puts("zbuild failed: generated C compatibility source exceeded buffer on line ");
                console_put_dec64(line);
                console_puts("\n");
                return;
            }
            compile_source = (const char *)exec_buffer;
            compile_size = generated_size;
        } else {
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
            if (status == -6) {
                console_puts("zbuild failed: source file is too large on line ");
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
            compile_source = source;
            compile_size = source_size;
        }

        console_puts("zbuild: compiling ");
        console_puts(source_name);
        console_puts(" source-bytes=");
        console_put_dec64(source_size);
        if (str_ends_with(source_name, ".c")) {
            console_puts(" generated-z-bytes=");
            console_put_dec64(compile_size);
        }
        console_puts("\n");

        zero_memory(zscript_output, ASM_SOURCE_SIZE + 1u);
        make_zobject_prefix(object_name, label_prefix, sizeof(label_prefix));
        if (zscript_compile_source_object(compile_source,
                                          compile_size,
                                          label_prefix,
                                          zscript_output,
                                          ASM_SOURCE_SIZE,
                                          &asm_size,
                                          &compile_error_line,
                                          entry_label,
                                          sizeof(entry_label)) != 0) {
            print_zscript_compile_failure("zbuild", source_name, compile_error_line);
            print_zscript_source_line(compile_source, compile_size, compile_error_line);
            return;
        }
        zscript_output[asm_size] = '\0';

        console_puts("zbuild: assembling ");
        console_puts(source_name);
        console_puts(" asm-bytes=");
        console_put_dec64(asm_size);
        console_puts("\n");

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

zbuild_save_object:
        if (skip_object_save) {
            console_puts("zbuild: keeping prebuilt ");
            console_puts(object_name);
            console_puts(" in place object-bytes=");
            console_put_dec64(object_size);
            console_puts("\n");
        } else {
            console_puts("zbuild: saving ");
            console_puts(object_name);
            console_puts(" object-bytes=");
            console_put_dec64(object_size);
            console_puts("\n");

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

        if (str_ends_with(source_name, ".zo") && streq(source_name, object_name)) {
            ++line;
            continue;
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

static int zinstall_module_objects_from_manifest(int drive,
                                                 const char *target_name,
                                                 char *manifest,
                                                 uint32_t manifest_capacity,
                                                 uint32_t build_dir,
                                                 uint32_t install_dir,
                                                 uint32_t *installed_count) {
    uint32_t manifest_size = 0;
    char manifest_name[32];
    int status;

    *installed_count = 0;
    if (make_suffixed_name(target_name, ".zbuild", manifest_name, sizeof(manifest_name)) != 0) {
        return -20;
    }
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

    for (uint32_t pos = 0, line = 1; pos < manifest_size;) {
        char *line_start = manifest + pos;
        char *line_text;
        char *source_name;
        char *object_name;
        char object_name_buffer[32];
        uint32_t object_size = 0;

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
        if (streq(source_name, "src") ||
            streq(source_name, "source") ||
            streq(source_name, "include") ||
            streq(source_name, "build") ||
            streq(source_name, "output") ||
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
                console_puts("zinstall failed: bad source name on line ");
                console_put_dec64(line);
                console_puts("\n");
                return -27;
            }
            object_name = object_name_buffer;
        } else {
            char *unused = 0;
            char *first_object_name = object_name;
            split_first_arg(object_name, &first_object_name, &unused);
            if (*unused != '\0') {
                console_puts("zinstall failed: too many fields on line ");
                console_put_dec64(line);
                console_puts("\n");
                return -28;
            }
            object_name = first_object_name;
        }

        zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
        status = lainfs_load_file_in_dir((char)('A' + drive),
                                         build_dir,
                                         object_name,
                                         (char *)exec_buffer,
                                         EXEC_BUFFER_SIZE,
                                         &object_size);
        if (status == -5) {
            console_puts("zinstall failed: build did not produce ");
            console_puts(object_name);
            console_puts("\n");
            return status;
        }
        if (status != 0 || object_size == 0) {
            console_puts("zinstall failed: could not load ");
            console_puts(object_name);
            console_puts("\n");
            return status != 0 ? status : -1;
        }

        status = lainfs_save_file_in_dir((char)('A' + drive),
                                         install_dir,
                                         object_name,
                                         (const char *)exec_buffer,
                                         object_size);
        if (status == -9) {
            console_puts("zinstall failed: disk is full\n");
            zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
            return status;
        }
        if (status != 0) {
            console_puts("zinstall failed: could not save ");
            console_puts(object_name);
            console_puts("\n");
            zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
            return status;
        }

        ++*installed_count;
        ++line;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
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
    uint32_t installed_count = 0;
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

    if (objects_only && object_count != 1) {
        if (*dest_path != '\0') {
            console_puts("zinstall failed: multi-object module cannot install as one file\n");
            return;
        }
        status = zinstall_module_objects_from_manifest(drive,
                                                       target_name,
                                                       manifest,
                                                       ASM_SOURCE_SIZE,
                                                       build_dir,
                                                       install_dir,
                                                       &installed_count);
        if (status != 0) {
            return;
        }
        console_puts("zinstall: installed ");
        console_put_dec64(installed_count);
        console_puts(" module object(s)\n");
        return;
    }

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
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            if (streq(zmodule_slots[i].exports[j].name, "zmodule_tick") ||
                streq(zmodule_slots[i].exports[j].name, "zmodule_unload") ||
                streq(zmodule_slots[i].exports[j].name, "zmodule_redraw") ||
                streq(zmodule_slots[i].exports[j].name, "zmodule_key") ||
                streq(zmodule_slots[i].exports[j].name, "zmodule_mouse")) {
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

static int zmodule_begin_call(uint32_t slot_index) {
    if (slot_index >= ZMODULE_MAX_MODULES ||
        !zmodule_slots[slot_index].loaded ||
        zmodule_slots[slot_index].unloading) {
        return 0;
    }

    ++zmodule_slots[slot_index].active_calls;
    return 1;
}

static void *zmodule_call_stack_top(uint32_t slot_index) {
    zmodule_slot_t *slot;

    if (slot_index >= ZMODULE_MAX_MODULES) {
        return 0;
    }
    slot = &zmodule_slots[slot_index];
    if (slot->call_stack == 0 || slot->call_stack_size < 4096u) {
        return 0;
    }
    return slot->call_stack + slot->call_stack_size;
}

static void zmodule_finish_clear(uint32_t slot_index) {
    if (slot_index >= ZMODULE_MAX_MODULES || !zmodule_slots[slot_index].loaded) {
        return;
    }

    if (zmodule_slots[slot_index].active_calls != 0) {
        return;
    }

    kfree(zmodule_slots[slot_index].image_alloc);
    zmodule_slots[slot_index].image_alloc = 0;
    zmodule_slots[slot_index].image = 0;
    kfree(zmodule_slots[slot_index].call_stack);
    zmodule_slots[slot_index].call_stack = 0;
    zmodule_slots[slot_index].call_stack_size = 0;
    zero_memory(zmodule_slots[slot_index].exports, sizeof(zmodule_slots[slot_index].exports));
    zmodule_slots[slot_index].loaded = 0;
    zmodule_slots[slot_index].unloading = 0;
    zmodule_slots[slot_index].active_calls = 0;
    zmodule_slots[slot_index].unload_called = 0;
    zmodule_slots[slot_index].name[0] = '\0';
    zmodule_slots[slot_index].image_size = 0;
    zmodule_slots[slot_index].object_count = 0;
    zmodule_slots[slot_index].export_count = 0;
}

static void zmodule_end_call(uint32_t slot_index) {
    if (slot_index >= ZMODULE_MAX_MODULES || zmodule_slots[slot_index].active_calls == 0) {
        return;
    }

    --zmodule_slots[slot_index].active_calls;
    if (zmodule_slots[slot_index].active_calls == 0 && zmodule_slots[slot_index].unloading) {
        zmodule_clear_slot(slot_index);
    }
}

void shell_modules_tick(void) {
    unsigned long long now = timer_ticks();
    unsigned int hz = timer_frequency();
    unsigned long long interval;
    uint32_t module_index = 0;

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
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }
        if (shell_module_is_ui_app(module_index)) {
            ++module_index;
            continue;
        }

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            if (streq(zmodule_slots[i].exports[j].name, "zmodule_tick")) {
                void *stack_top = zmodule_call_stack_top(i);

                if (!zmodule_begin_call(i)) {
                    break;
                }
                if (stack_top != 0) {
                    zmodule_call_void_on_stack((zmodule_void_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value,
                                               stack_top);
                } else {
                    ((zmodule_tick_t)(uintptr_t)zmodule_slots[i].exports[j].value)();
                }
                zmodule_end_call(i);
                break;
            }
        }
        ++module_index;
    }
}

uint32_t shell_module_count(void) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (zmodule_slots[i].loaded && !zmodule_slots[i].unloading) {
            ++count;
        }
    }

    return count;
}

const char *shell_module_name(uint32_t index) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }
        if (seen == index) {
            return zmodule_slots[i].name;
        }
        ++seen;
    }

    return 0;
}

int shell_module_has_export(uint32_t index, const char *export_name) {
    uint32_t seen = 0;

    if (export_name == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }
        if (seen == index) {
            for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
                if (streq(zmodule_slots[i].exports[j].name, export_name)) {
                    return 1;
                }
            }
            return 0;
        }
        ++seen;
    }

    return 0;
}

int shell_module_is_ui_app(uint32_t index) {
    return shell_module_has_export(index, "zmodule_redraw");
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
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }
        if (seen == index) {
            for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
                if (streq(zmodule_slots[i].exports[j].name, export_name)) {
                    void *stack_top = zmodule_call_stack_top(i);

                    if (!zmodule_begin_call(i)) {
                        return -1;
                    }
                    if (stack_top != 0) {
                        zmodule_call_void_on_stack((zmodule_void_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value,
                                                   stack_top);
                    } else {
                        ((zmodule_void_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value)();
                    }
                    zmodule_end_call(i);
                    return 0;
                }
            }
            return -1;
        }
        ++seen;
    }

    return -1;
}

int shell_module_key(uint32_t index, uint32_t key_type, uint32_t ch) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }
        if (seen == index) {
            for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
                if (streq(zmodule_slots[i].exports[j].name, "zmodule_key")) {
                    void *stack_top = zmodule_call_stack_top(i);

                    if (!zmodule_begin_call(i)) {
                        return -1;
                    }
                    if (stack_top != 0) {
                        zmodule_call_key_on_stack((zmodule_key_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value,
                                                  key_type,
                                                  ch,
                                                  stack_top);
                    } else {
                        ((zmodule_key_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value)(key_type, ch);
                    }
                    zmodule_end_call(i);
                    return 0;
                }
            }
            return -1;
        }
        ++seen;
    }

    return -1;
}

int shell_module_mouse(uint32_t index, uint32_t x, uint32_t y, uint32_t buttons, int32_t wheel) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded || zmodule_slots[i].unloading) {
            continue;
        }
        if (seen == index) {
            for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
                if (streq(zmodule_slots[i].exports[j].name, "zmodule_mouse")) {
                    void *stack_top = zmodule_call_stack_top(i);

                    if (!zmodule_begin_call(i)) {
                        return -1;
                    }
                    if (stack_top != 0) {
                        zmodule_call_mouse_on_stack((zmodule_mouse_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value,
                                                    x,
                                                    y,
                                                    buttons,
                                                    wheel,
                                                    stack_top);
                    } else {
                        ((zmodule_mouse_hook_t)(uintptr_t)zmodule_slots[i].exports[j].value)(x, y, buttons, wheel);
                    }
                    zmodule_end_call(i);
                    return 0;
                }
            }
            return -1;
        }
        ++seen;
    }

    return -1;
}

int shell_module_resolve_address(uint64_t address,
                                 const char **name,
                                 uint64_t *base,
                                 uint32_t *size,
                                 const char **nearest_export,
                                 uint64_t *nearest_export_value) {
    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        uint64_t module_base;
        uint64_t module_end;
        const char *best_name = 0;
        uint64_t best_value = 0;

        if (!zmodule_slots[i].loaded || zmodule_slots[i].image == 0) {
            continue;
        }

        module_base = (uint64_t)(uintptr_t)zmodule_slots[i].image;
        module_end = module_base + (uint64_t)zmodule_slots[i].image_size;
        if (address < module_base || address >= module_end) {
            continue;
        }

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            uint64_t value = zmodule_slots[i].exports[j].value;
            if (value <= address && value >= module_base && value > best_value) {
                best_name = zmodule_slots[i].exports[j].name;
                best_value = value;
            }
        }

        if (name != 0) {
            *name = zmodule_slots[i].name;
        }
        if (base != 0) {
            *base = module_base;
        }
        if (size != 0) {
            *size = zmodule_slots[i].image_size;
        }
        if (nearest_export != 0) {
            *nearest_export = best_name;
        }
        if (nearest_export_value != 0) {
            *nearest_export_value = best_value;
        }
        return 0;
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
        if (zmodule_slots[i].loaded &&
            !zmodule_slots[i].unloading &&
            zmodule_name_matches(zmodule_slots[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static void zmodule_call_unload_hook(uint32_t slot_index) {
    zmodule_slot_t *slot;

    if (slot_index >= ZMODULE_MAX_MODULES) {
        return;
    }

    slot = &zmodule_slots[slot_index];
    if (slot == 0 || !slot->loaded || slot->unload_called) {
        return;
    }

    for (uint32_t i = 0; i < slot->export_count; ++i) {
        if (streq(slot->exports[i].name, "zmodule_unload")) {
            void *stack_top = zmodule_call_stack_top(slot_index);

            slot->unload_called = 1;
            ++slot->active_calls;
            if (stack_top != 0) {
                zmodule_call_void_on_stack((zmodule_void_hook_t)(uintptr_t)slot->exports[i].value,
                                           stack_top);
            } else {
                ((zmodule_unload_t)(uintptr_t)slot->exports[i].value)();
            }
            zmodule_end_call(slot_index);
            return;
        }
    }

    slot->unload_called = 1;
}

static void zmodule_clear_slot(uint32_t slot_index) {
    if (slot_index >= ZMODULE_MAX_MODULES || !zmodule_slots[slot_index].loaded) {
        return;
    }

    zmodule_slots[slot_index].unloading = 1;
    if (zmodule_slots[slot_index].active_calls != 0) {
        return;
    }

    zmodule_call_unload_hook(slot_index);
    if (zmodule_slots[slot_index].active_calls == 0) {
        zmodule_finish_clear(slot_index);
    }
}

static int zmodule_name_has_dot(const char *name) {
    for (uint32_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] == '.') {
            return 1;
        }
    }
    return 0;
}

static int zmodule_collect_manifest_objects(int drive,
                                            const char *target_name,
                                            char **tokens,
                                            uint32_t *token_count) {
    char manifest_name[32];
    char *manifest = shell_manifest_buffer;
    uint32_t manifest_size = 0;
    uint32_t count = 0;
    int saw_module_directive = 0;
    int status;

    if (zmodule_name_has_dot(target_name) ||
        make_suffixed_name(target_name, ".zbuild", manifest_name, sizeof(manifest_name)) != 0) {
        return -1;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     manifest_name,
                                     manifest,
                                     ASM_SOURCE_SIZE,
                                     &manifest_size);
    if (status != 0 || manifest_size >= ASM_SOURCE_SIZE) {
        return -1;
    }
    manifest[manifest_size] = '\0';

    for (uint32_t pos = 0, line = 1; pos < manifest_size;) {
        char *line_start = manifest + pos;
        char *line_text;
        char *source_name;
        char *object_name;
        char *unused = 0;
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
        if (streq(source_name, "module") || streq(source_name, "objects-only")) {
            saw_module_directive = 1;
            ++line;
            continue;
        }
        if (streq(source_name, "src") ||
            streq(source_name, "source") ||
            streq(source_name, "include") ||
            streq(source_name, "build") ||
            streq(source_name, "output") ||
            streq(source_name, "test-return") ||
            streq(source_name, "install") ||
            streq(source_name, "install-name")) {
            ++line;
            continue;
        }

        if (count >= ZLINK_MAX_OBJECTS) {
            console_puts("zmod failed: manifest object count exceeds max=");
            console_put_dec64(ZLINK_MAX_OBJECTS);
            console_puts("\n");
            return -1;
        }

        if (*object_name == '\0') {
            if (make_object_name_from_source(source_name,
                                             object_name_buffer,
                                             sizeof(object_name_buffer)) != 0) {
                console_puts("zmod failed: bad source name in manifest line ");
                console_put_dec64(line);
                console_puts("\n");
                return -1;
            }
            object_name = object_name_buffer;
        } else {
            char *first_object_name = object_name;
            split_first_arg(object_name, &first_object_name, &unused);
            if (*unused != '\0') {
                console_puts("zmod failed: bad object name in manifest line ");
                console_put_dec64(line);
                console_puts("\n");
                return -1;
            }
            object_name = first_object_name;
        }

        copy_text_limited(zmodule_manifest_object_names[count],
                          sizeof(zmodule_manifest_object_names[count]),
                          object_name);
        tokens[count] = zmodule_manifest_object_names[count];
        ++count;
        ++line;
    }

    if (!saw_module_directive || count == 0) {
        return -1;
    }
    *token_count = count;
    return 0;
}

static int zmodule_name_is_simple(const char *name) {
    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    for (uint32_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] == ':' || name[i] == '/' || name[i] == '\\') {
            return 0;
        }
    }
    return 1;
}

static int zmodule_load_from_ramdisk_examples(const char *name,
                                              unsigned char *dst,
                                              uint32_t capacity,
                                              uint32_t *out_size) {
    uint32_t examples_dir = 0;

    if (!zmodule_name_is_simple(name) ||
        !storage_drive_is_mounted('R') ||
        lainfs_find_dir('R', LAINFS_ROOT_DIR, "examples", &examples_dir) != 0) {
        return -5;
    }
    return lainfs_load_file_in_dir('R', examples_dir, name, (char *)dst, capacity, out_size);
}

static uint32_t zmodule_read_le32(const unsigned char *data) {
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

typedef struct zmodule_package_diag {
    const char *source;
    uint32_t bundle_bytes;
    uint32_t chunk_count;
    uint32_t last_chunk_index;
    uint32_t last_chunk_bytes;
    int last_chunk_status;
    uint32_t object_count;
    int parse_status;
} zmodule_package_diag_t;

static zmodule_package_diag_t zmodule_package_diag;

static void zmodule_reset_package_diag(void) {
    zero_memory(&zmodule_package_diag, sizeof(zmodule_package_diag));
    zmodule_package_diag.source = "none";
    zmodule_package_diag.last_chunk_status = -5;
}

static void zmodule_put_signed_dec(int value) {
    if (value < 0) {
        console_puts("-");
        console_put_dec64((uint64_t)(0 - value));
        return;
    }
    console_put_dec64((uint64_t)value);
}

static void zmodule_print_package_diag(const char *label) {
    console_puts("zmod package ");
    console_puts(label);
    console_puts(" source=");
    console_puts(zmodule_package_diag.source ? zmodule_package_diag.source : "none");
    console_puts(" chunks=");
    console_put_dec64(zmodule_package_diag.chunk_count);
    console_puts(" bytes=");
    console_put_dec64(zmodule_package_diag.bundle_bytes);
    console_puts(" objects=");
    console_put_dec64(zmodule_package_diag.object_count);
    console_puts(" parse=");
    zmodule_put_signed_dec(zmodule_package_diag.parse_status);
    console_puts(" last-chunk=");
    console_put_dec64(zmodule_package_diag.last_chunk_index);
    console_puts("/");
    console_put_dec64(zmodule_package_diag.last_chunk_bytes);
    console_puts(" status=");
    zmodule_put_signed_dec(zmodule_package_diag.last_chunk_status);
    console_puts("\n");
}

static int zmodule_parse_package_bundle(unsigned char *bundle,
                                        uint32_t bundle_size,
                                        const unsigned char **objects,
                                        uint32_t *object_sizes,
                                        uint32_t *object_count) {
    uint32_t count;
    uint32_t header_size;

    if (bundle == 0 || objects == 0 || object_sizes == 0 || object_count == 0 ||
        bundle_size < 12u ||
        bundle[0] != 'Z' || bundle[1] != 'P' || bundle[2] != 'K' ||
        bundle[3] != 'G' || bundle[4] != '1') {
        return -1;
    }

    count = zmodule_read_le32(bundle + 8u);
    if (count == 0 || count > ZLINK_MAX_OBJECTS) {
        return -1;
    }
    header_size = 12u + count * 40u;
    if (header_size > bundle_size) {
        return -1;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = 12u + i * 40u;
        uint32_t offset = zmodule_read_le32(bundle + entry + 32u);
        uint32_t size = zmodule_read_le32(bundle + entry + 36u);

        if (offset < header_size || size == 0 || offset > bundle_size ||
            size > bundle_size - offset) {
            return -1;
        }
        objects[i] = bundle + offset;
        object_sizes[i] = size;
    }

    *object_count = count;
    return 0;
}

static int zmodule_try_load_package_bundle(int drive,
                                           const char *target_name,
                                           const unsigned char **objects,
                                           uint32_t *object_sizes,
                                           uint32_t *object_count) {
    char bundle_name[32];
    uint32_t bundle_size = 0;
    int status = -5;

    if (drive < 0 || target_name == 0 ||
        zmodule_name_has_dot(target_name) ||
        make_suffixed_name(target_name, ".zpkg", bundle_name, sizeof(bundle_name)) != 0) {
        return -5;
    }

    {
        int object_drive = drive;
        uint32_t object_parent = cwd_dirs[drive];
        char object_name[32];

        status = resolve_file_path_with_drive(bundle_name,
                                              drive,
                                              &object_drive,
                                              &object_parent,
                                              object_name,
                                              sizeof(object_name));
        if (status == 0) {
            status = lainfs_load_file_in_dir((char)('A' + object_drive),
                                             object_parent,
                                             object_name,
                                             (char *)asm_output,
                                             EXEC_BUFFER_SIZE,
                                             &bundle_size);
        }
    }

    if (status == -5) {
        status = zmodule_load_from_ramdisk_examples(bundle_name,
                                                    asm_output,
                                                    EXEC_BUFFER_SIZE,
                                                    &bundle_size);
    }
    if (status != 0) {
        return status;
    }

    zmodule_package_diag.source = "zpkg";
    zmodule_package_diag.bundle_bytes = bundle_size;
    zmodule_package_diag.chunk_count = 1;
    zmodule_package_diag.last_chunk_index = 0;
    zmodule_package_diag.last_chunk_bytes = bundle_size;
    zmodule_package_diag.last_chunk_status = status;
    zmodule_package_diag.parse_status =
        zmodule_parse_package_bundle(asm_output,
                                     bundle_size,
                                     objects,
                                     object_sizes,
                                     object_count);
    if (zmodule_package_diag.parse_status != 0) {
        console_puts("zmod failed: malformed package bundle ");
        console_puts(bundle_name);
        console_puts("\n");
        zmodule_print_package_diag("failed");
        return -1;
    }
    zmodule_package_diag.object_count = *object_count;
    zmodule_print_package_diag("loaded");

    return 0;
}

static int zmodule_package_chunk_name(const char *target_name,
                                      uint32_t index,
                                      char *out,
                                      uint32_t out_size) {
    uint32_t pos = 0;

    if (target_name == 0 || out == 0 || out_size == 0 || index > 99u) {
        return -1;
    }
    while (target_name[pos] != '\0' && pos + 6u < out_size) {
        out[pos] = target_name[pos];
        ++pos;
    }
    if (target_name[pos] != '\0' || pos + 6u >= out_size) {
        return -1;
    }
    out[pos++] = '.';
    out[pos++] = 'z';
    out[pos++] = 'p';
    out[pos++] = (char)('0' + (index / 10u));
    out[pos++] = (char)('0' + (index % 10u));
    out[pos] = '\0';
    return 0;
}

static int zmodule_load_package_chunk(int drive,
                                      const char *chunk_name,
                                      unsigned char *dst,
                                      uint32_t capacity,
                                      uint32_t *out_size) {
    int status = -5;

    if (drive < 0 || chunk_name == 0 || dst == 0 || out_size == 0 || capacity == 0u) {
        return -1;
    }

    {
        int object_drive = drive;
        uint32_t object_parent = cwd_dirs[drive];
        char object_name[32];

        status = resolve_file_path_with_drive(chunk_name,
                                              drive,
                                              &object_drive,
                                              &object_parent,
                                              object_name,
                                              sizeof(object_name));
        if (status == 0) {
            status = lainfs_load_file_in_dir((char)('A' + object_drive),
                                             object_parent,
                                             object_name,
                                             (char *)dst,
                                             capacity,
                                             out_size);
        }
    }

    if (status == -5) {
        status = zmodule_load_from_ramdisk_examples(chunk_name,
                                                    dst,
                                                    capacity,
                                                    out_size);
    }

    return status;
}

static int zmodule_try_load_package_bundle_chunks(int drive,
                                                  const char *target_name,
                                                  const unsigned char **objects,
                                                  uint32_t *object_sizes,
                                                  uint32_t *object_count) {
    uint32_t total_size = 0;
    uint32_t chunk_count = 0;

    if (drive < 0 || target_name == 0 || zmodule_name_has_dot(target_name)) {
        return -5;
    }

    for (uint32_t i = 0; i < 100u; ++i) {
        char chunk_name[32];
        uint32_t chunk_size = 0;
        int status;

        if (zmodule_package_chunk_name(target_name, i, chunk_name, sizeof(chunk_name)) != 0) {
            return -5;
        }
        if (total_size >= EXEC_BUFFER_SIZE) {
            console_puts("zmod failed: package bundle chunks are too large\n");
            return -1;
        }

        status = zmodule_load_package_chunk(drive,
                                            chunk_name,
                                            asm_output + total_size,
                                            EXEC_BUFFER_SIZE - total_size,
                                            &chunk_size);
        zmodule_package_diag.source = "chunks";
        zmodule_package_diag.last_chunk_index = i;
        zmodule_package_diag.last_chunk_bytes = chunk_size;
        zmodule_package_diag.last_chunk_status = status;
        if (status == -5) {
            if (i == 0u) {
                return -5;
            }
            break;
        }
        if (status != 0) {
            zmodule_print_package_diag("chunk-load-failed");
            return status;
        }
        total_size += chunk_size;
        ++chunk_count;
        zmodule_package_diag.chunk_count = chunk_count;
        zmodule_package_diag.bundle_bytes = total_size;
    }

    zmodule_package_diag.parse_status =
        zmodule_parse_package_bundle(asm_output,
                                     total_size,
                                     objects,
                                     object_sizes,
                                     object_count);
    if (chunk_count == 0u || zmodule_package_diag.parse_status != 0) {
        console_puts("zmod failed: malformed package bundle chunks ");
        console_puts(target_name);
        console_puts("\n");
        zmodule_print_package_diag("failed");
        return -1;
    }
    zmodule_package_diag.object_count = *object_count;
    zmodule_print_package_diag("loaded");

    return 0;
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
    char slot_name[32];
    int manifest_package = 0;
    int bundle_package = 0;
    int slot_index = -1;
    int drive = active_drive();
    int status = 0;
    zobject_resolved_symbol_t *resident_symbols = zmodule_resident_symbol_work;
    zobject_resolved_symbol_t *export_symbols = zmodule_export_symbol_work;

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
    zmodule_reset_package_diag();
    copy_text_limited(slot_name, sizeof(slot_name), tokens[token_count - 1u]);
    if (token_count == 1u &&
        zmodule_try_load_package_bundle(drive,
                                        tokens[0],
                                        objects,
                                        object_sizes,
                                        &object_count) == 0) {
        copy_text_limited(slot_name, sizeof(slot_name), args);
        manifest_package = 1;
        bundle_package = 1;
        token_count = 0;
    } else if (token_count == 1u &&
        zmodule_try_load_package_bundle_chunks(drive,
                                               tokens[0],
                                               objects,
                                               object_sizes,
                                               &object_count) == 0) {
        copy_text_limited(slot_name, sizeof(slot_name), args);
        manifest_package = 1;
        bundle_package = 1;
        token_count = 0;
    } else if (token_count == 1u &&
        zmodule_collect_manifest_objects(drive, tokens[0], tokens, &token_count) == 0) {
        copy_text_limited(slot_name, sizeof(slot_name), args);
        manifest_package = 1;
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

    if (!bundle_package) {
        zero_memory(asm_output, EXEC_BUFFER_SIZE);
    }
    for (uint32_t i = 0; i < token_count; ++i) {
        uint32_t remaining;
        const char *load_name = tokens[i];
        char suffixed_name[32];
        int has_dot = 0;

        if (object_offset >= EXEC_BUFFER_SIZE) {
            console_puts("zmod failed: object set is too large\n");
            return;
        }
        remaining = EXEC_BUFFER_SIZE - object_offset;

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
        if (status == -5) {
            status = zmodule_load_from_ramdisk_examples(load_name,
                                                        asm_output + object_offset,
                                                        remaining,
                                                        &object_sizes[object_count]);
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

    kfree(zmodule_slots[slot_index].image_alloc);
    zmodule_slots[slot_index].image_alloc =
        (unsigned char *)kmalloc(ZMODULE_IMAGE_SIZE + ZMODULE_IMAGE_ALIGNMENT);
    zmodule_slots[slot_index].image =
        zmodule_align_image_allocation(zmodule_slots[slot_index].image_alloc);
    if (zmodule_slots[slot_index].image == 0) {
        console_puts("zmod failed: out of heap for resident module\n");
        return;
    }
    kfree(zmodule_slots[slot_index].call_stack);
    zmodule_slots[slot_index].call_stack = (unsigned char *)kmalloc(ZMODULE_CALL_STACK_SIZE);
    if (zmodule_slots[slot_index].call_stack == 0) {
        console_puts("zmod failed: out of heap for resident module stack\n");
        kfree(zmodule_slots[slot_index].image_alloc);
        zmodule_slots[slot_index].image_alloc = 0;
        zmodule_slots[slot_index].image = 0;
        return;
    }
    zmodule_slots[slot_index].call_stack_size = ZMODULE_CALL_STACK_SIZE;

    if (bundle_package) {
        zmodule_print_package_diag("link-input");
    }
    console_puts("zmod link input objects=");
    console_put_dec64(object_count);
    console_puts(" image-alloc=0x");
    console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[slot_index].image_alloc);
    console_puts(" image=0x");
    console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[slot_index].image);
    console_puts(" image-cap=");
    console_put_dec64(ZMODULE_IMAGE_SIZE);
    console_puts(" stack=0x");
    console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[slot_index].call_stack);
    console_puts(" stack-bytes=");
    console_put_dec64(ZMODULE_CALL_STACK_SIZE);
    console_puts("\n");

    zero_memory(zmodule_slots[slot_index].image, ZMODULE_IMAGE_SIZE);
    if (zobject_link_flat_many_ex_entry_from(objects,
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
                                             &export_symbol_count,
                                             manifest_package ? object_count - 1u : 0u) != 0) {
        console_puts("zmod failed: unresolved or unsupported module");
        if (zobject_last_error_reason()[0] != '\0') {
            console_puts(": ");
            console_puts(zobject_last_error_reason());
            if (zobject_last_error_symbol()[0] != '\0') {
                console_puts(" ");
                console_puts(zobject_last_error_symbol());
            }
        }
        if (error_line != 0) {
            console_puts(" asm line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        if (bundle_package) {
            zmodule_print_package_diag("link-failed");
        }
        kfree(zmodule_slots[slot_index].image_alloc);
        zmodule_slots[slot_index].image_alloc = 0;
        zmodule_slots[slot_index].image = 0;
        kfree(zmodule_slots[slot_index].call_stack);
        zmodule_slots[slot_index].call_stack = 0;
        zmodule_slots[slot_index].call_stack_size = 0;
        return;
    }

    zmodule_slots[slot_index].loaded = 1;
    zmodule_slots[slot_index].unloading = 0;
    zmodule_slots[slot_index].active_calls = 0;
    zmodule_slots[slot_index].unload_called = 0;
    zmodule_slots[slot_index].image_size = output_size;
    zmodule_slots[slot_index].object_count = object_count;
    zmodule_slots[slot_index].export_count = export_symbol_count;
    copy_text_limited(zmodule_slots[slot_index].name,
                      sizeof(zmodule_slots[slot_index].name),
                      slot_name);
    for (uint32_t i = 0; i < export_symbol_count; ++i) {
        zmodule_slots[slot_index].exports[i] = export_symbols[i];
    }

    console_puts("loading ");
    console_put_dec64(object_count);
    console_puts(bundle_package ? " package module object(s) in slot " :
                 (manifest_package ? " manifest module object(s) in slot " : " module object(s) in slot "));
    console_put_dec64((uint32_t)slot_index);
    console_puts(" at 0x");
    console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[slot_index].image);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts(" exports=");
    console_put_dec64(export_symbol_count);
    console_puts("\n");

    zmodule_call_program_on_stack((exec_program_t)(uintptr_t)zmodule_slots[slot_index].image,
                                  &api,
                                  zmodule_call_stack_top((uint32_t)slot_index));

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

static void cmd_zmodtest(const char *args, const boot_info_t *info) {
    char command[96];
    char module_name[64];
    char *mutable_command = command;
    char *name = 0;
    char *count_text = 0;
    char *extra = 0;
    uint64_t cycles = 10;
    uint64_t completed = 0;

    (void)info;

    if (copy_command_arg(args, command, sizeof(command)) != 0) {
        console_puts("usage: zmodtest module [count]\n");
        return;
    }

    split_first_arg(mutable_command, &name, &count_text);
    split_first_arg(count_text, &count_text, &extra);
    if (*name == '\0' || *extra != '\0') {
        console_puts("usage: zmodtest module [count]\n");
        return;
    }
    if (*count_text != '\0' && parse_u64_arg(count_text, &cycles) != 0) {
        console_puts("usage: zmodtest module [count]\n");
        return;
    }
    if (cycles == 0u || cycles > 100u) {
        console_puts("zmodtest failed: count must be 1..100\n");
        return;
    }

    copy_string_limited(module_name, sizeof(module_name), name);

    {
        int slot_index = zmodule_find_slot_by_name(module_name);
        if (slot_index >= 0) {
            zmodule_clear_slot((uint32_t)slot_index);
            if (zmodule_find_slot_by_name(module_name) >= 0) {
                console_puts("zmodtest failed: module is busy unloading: ");
                console_puts(module_name);
                console_puts("\n");
                return;
            }
        }
    }

    console_puts("zmodtest: ");
    console_puts(module_name);
    console_puts(" cycles=");
    console_put_dec64(cycles);
    console_puts("\n");

    for (uint64_t i = 0; i < cycles; ++i) {
        int slot_index;

        console_suppress_current_cpu_push();
        cmd_zmod(module_name, 0);
        console_suppress_current_cpu_pop();

        slot_index = zmodule_find_slot_by_name(module_name);
        if (slot_index < 0) {
            console_puts("zmodtest failed: load failed on cycle ");
            console_put_dec64(i + 1u);
            console_puts("; retrying with diagnostics\n");
            cmd_zmod(module_name, 0);
            return;
        }

        zmodule_clear_slot((uint32_t)slot_index);
        if (zmodule_find_slot_by_name(module_name) >= 0) {
            console_puts("zmodtest failed: unload deferred on cycle ");
            console_put_dec64(i + 1u);
            console_puts("\n");
            return;
        }

        ++completed;
        (void)kernel_task_poll();
    }

    console_puts("zmodtest: ok cycles=");
    console_put_dec64(completed);
    console_puts("\n");
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
        console_puts(" calls=");
        console_put_dec64(zmodule_slots[i].active_calls);
        if (zmodule_slots[i].unloading) {
            console_puts(" unloading");
        }
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
        print_zscript_compile_failure("zrun", 0, compile_error_line);
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
        print_zscript_compile_failure("zasm", 0, compile_error_line);
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
    shell_bg_poll();

    if (current_drive >= 0 &&
        (drives[current_drive].present || storage_drive_is_mounted((char)('A' + current_drive)))) {
        if (!drives[current_drive].present) {
            drives[current_drive].present = 1;
            copy_label(drives[current_drive].label, "MOUNT");
            reset_cwd(current_drive);
        }
        print_drive_name(current_drive);
        console_puts(cwd_paths[current_drive]);
        console_puts("> ");
        return;
    }

    console_puts("> ");
}

static int shell_run_command_foreground(char *line, const boot_info_t *info, int background) {
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
        return 0;
    }

    int requested_drive = parse_drive_spec(name);
    if (requested_drive >= 0 && requested_drive < MAX_DRIVES) {
        if (drives[requested_drive].present || storage_drive_is_mounted((char)('A' + requested_drive))) {
            if (!drives[requested_drive].present) {
                drives[requested_drive].present = 1;
                copy_label(drives[requested_drive].label, "MOUNT");
                reset_cwd(requested_drive);
            }
            current_drive = requested_drive;
        } else {
            print_drive_name(requested_drive);
            console_puts(" does not exist. create it with mkdrive ");
            print_drive_name(requested_drive);
            console_puts("\n");
            return 1;
        }
        return 0;
    }

    if (background && streq(name, "tasktest")) {
        run_background_tasktest();
        return 0;
    }
    if (background && (streq(name, "tasks") || streq(name, "smp"))) {
        return 126;
    }

    for (unsigned int i = 0; i < command_count; ++i) {
        if (streq(name, commands[i].name)) {
            commands[i].handler(args, info);
            return 0;
        }
    }

    console_puts("unknown command: ");
    console_puts(name);
    console_puts("\n");
    return 127;
}

void shell_run_command(char *line, const boot_info_t *info) {
    char *command = trim_spaces_mutable(line);
    char *end = command;

    shell_bg_poll();

    while (*end != '\0') {
        ++end;
    }
    if (end > command && end[-1] == '&') {
        end[-1] = '\0';
        command = trim_spaces_mutable(command);
        if (*command == '\0') {
            console_puts("background: empty command\n");
            return;
        }
        if (shell_bg_start(command, info) != 0) {
            console_puts("background: could not start job\n");
        }
        return;
    }

    (void)shell_run_command_foreground(command, info, 0);
}
