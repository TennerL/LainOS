#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#ifdef __unix__
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __unix__
#include <sys/mman.h>
#endif

#include "zobject.h"
#include "zscript.h"
#include "kernel_exports.h"
#include "weblayout.h"

#define HOST_RUN_EXEC_API_MAGIC 0x4C41494E45584543ull
#define HOST_MAX_INCLUDE_DEPTH 16u
#define HOST_MAX_INCLUDE_DIRS 4u
#define HOST_MAX_SOURCE_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_ASM_SIZE (8u * 1024u * 1024u)
#define HOST_MAX_OBJECT_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_LINK_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_OBJECTS 32u
#define HOST_MAX_ONCE_PATHS 128u
#define HOST_MAX_PATH 768u
#define HOST_RUNTIME_ENTRY_FILE 1
#define HOST_RUNTIME_ENTRY_DIR 2

typedef struct {
    const char *const *include_dirs;
    uint32_t include_dir_count;
    char once_paths[HOST_MAX_ONCE_PATHS][HOST_MAX_PATH];
    uint32_t once_count;
} host_source_context_t;

typedef struct {
    uint64_t magic;
    uint64_t version;
    void (*puts)(const char *s);
    void (*put_hex64)(unsigned long long value);
    void (*put_dec64)(unsigned long long value);
    unsigned long long (*ticks)(void);
} host_exec_api_t;

typedef uint64_t (*host_exec_program_ret_t)(const host_exec_api_t *api);

static int host_runtime_mode = 0;
static int host_runtime_paths_ready = 0;
static char host_runtime_repo_root[HOST_MAX_PATH];
static char host_runtime_cwd[HOST_MAX_PATH];
static char host_runtime_manifest_dir[HOST_MAX_PATH];
static unsigned char *host_runtime_file_buffer = 0;
static uint32_t host_runtime_file_buffer_size = 0;
static uint32_t host_runtime_task_next_id = 1u;
static uint32_t host_runtime_dom_status = 0u;

#define HOST_RUNTIME_TASK_SLOTS 16u
typedef struct {
    uint32_t id;
    char name[32];
} host_runtime_task_slot_t;

static host_runtime_task_slot_t host_runtime_task_slots[HOST_RUNTIME_TASK_SLOTS];

static int join_path(const char *dir, const char *name, char *out, uint32_t out_capacity);
static int path_is_absolute(const char *path);

void *kmalloc(uint32_t size) {
    return malloc(size);
}

void *kzalloc(uint32_t size) {
    return calloc(1u, size);
}

void kfree(void *ptr) {
    free(ptr);
}

uint32_t kmalloc_size(void *ptr) {
    (void)ptr;
    return 0;
}

static const char *const host_kernel_exports[] = {
    "puts",
    "put_hex64",
    "put_dec64",
    "ticks",
    "clock_unix_time",
    "clock_get_rtc_time",
    "status_memory_total_kb",
    "mem_total_kb",
    "status_memory_free_kb",
    "mem_free_kb",
    "status_memory_used_kb",
    "mem_used_kb",
    "kmalloc",
    "kzalloc",
    "kfree",
    "kmalloc_size",
    "malloc",
    "calloc",
    "realloc",
    "free",
    "abort",
    "time",
    "errno_location",
    "__errno_location",
    "kernel_task_submit_named",
    "kernel_task_submit",
    "kernel_task_submit_async_named",
    "kernel_task_async_supported",
    "kernel_task_done",
    "kernel_task_release",
    "kernel_task_wait",
    "kernel_task_poll",
    "kernel_task_pending_count",
    "kernel_task_snapshot",
    "memcpy",
    "memset",
    "memmove",
    "memcmp",
    "strlen",
    "strcpy",
    "strncpy",
    "strcat",
    "strcmp",
    "strncmp",
    "strcasecmp",
    "strncasecmp",
    "strchr",
    "strrchr",
    "strstr",
    "strdup",
    "strtol",
    "strtoul",
    "snprintf",
    "vsnprintf",
    "tolower",
    "toupper",
    "bsearch",
    "page_alloc",
    "page_free",
    "heap_used_bytes",
    "registry_set",
    "registry_get",
    "registry_get_u32",
    "registry_count",
    "registry_key_at",
    "registry_value_at",
    "status_cpu_core_count",
    "cpu_count",
    "cpu_usage",
    "cpu_local_timer_ticks",
    "cpu_lapic_timer_frequency",
    "smp_submit_work",
    "smp_work_done",
    "smp_wait_work",
    "smp_pending_work_count",
    "put_pixel",
    "gfx_width",
    "gfx_height",
    "gfx_pitch",
    "gfx_format",
    "gfx_viewport_active",
    "gfx_viewport_x",
    "gfx_viewport_y",
    "gfx_smp_last_workers",
    "gfx_smp_jobs",
    "gfx_smp_ops",
    "gfx_smp_pixels",
    "gfx_get_pixel",
    "gfx_draw_rect_packed",
    "gfx_fill_rect",
    "gfx_scroll_rect",
    "gfx_draw_rect",
    "gfx_draw_line",
    "gfx_clear",
    "draw_text_at_pixel",
    "draw_text_scaled_at_pixel",
    "put_char_at",
    "put_dec_at",
    "put_char_at_screen",
    "put_dec_at_screen",
    "mouse_x",
    "mouse_y",
    "mouse_dx",
    "mouse_dy",
    "mouse_buttons",
    "mouse_wheel",
    "mouse_consume_wheel",
    "mouse_enabled",
    "mouse_init",
    "os_read_file",
    "os_write_file",
    "os_load_file_shared",
    "os_file_buffer",
    "os_file_size",
    "os_cat_file",
    "os_copy_file",
    "os_delete",
    "os_rename",
    "os_mkdir",
    "os_http_get",
    "os_http_get_ex",
    "os_strlen",
    "os_strcmp",
    "os_starts_with",
    "os_atoi",
    "os_list_dir",
    "os_chdir",
    "os_dir_count",
    "os_dir_name",
    "os_dir_type",
    "os_dir_size",
    "os_zbuild",
    "os_ztest",
    "os_zinstall",
    "os_zmod",
    "os_zunload",
    "os_zreload",
    "os_open_editor",
    "os_open_image",
    "os_open_module",
    "os_image_path",
    "image_probe",
    "image_supported_formats",
    "image_decode_rgba32",
    "image_decode_rgb24",
    "image_decode_to_screen",
    "image_decode_to_screen_scaled",
    "image_decode_scaled_to_packed",
    "image_decode_to_screen_tiled",
    "jpg_probe",
    "jpg_decode_rgb24",
    "jpg_decode_to_screen",
    "jpg_huffman_code_count",
    "jpg_huffman_value",
    "jpg_debug_scan_offset",
    "jpg_debug_stream_pos",
    "jpg_debug_bits_left",
    "jpg_debug_byte_at",
    "jpg_debug_zigzag",
    "jpg_debug_first_entropy_bits",
    "jpg_decode_first_block_probe",
    "jpg_decode_two_block_probe",
    "jpg_decode_first_ac_probe",
    "jpg_entropy_probe_first",
    "jpg_entropy_error_block",
    "jpg_entropy_error_detail",
    "set_margin",
    "statusbar_enable",
    "web_style_prepare_document",
    "web_style_for_tag",
    "web_style_for_cached_rules",
    "webcompat_lwc_smoke",
    "webcompat_lwc_last_status",
    "webcompat_pu_smoke",
    "webcompat_pu_status",
    "webcompat_css_smoke",
    "webcompat_css_status",
    "netsurf_port_dom_smoke",
    "netsurf_port_dom_status",
    "netsurf_port_parse_html_smoke",
    "netsurf_port_render_smoke",
    "netsurf_port_rewrite_html",
    "netsurf_port_rewrite_render_html",
    "netsurf_port_style_hint_for_tag",
    "netsurf_port_status",
    "netsurf_kernel_layout_table",
    "netsurf_kernel_plotter_table",
    "netsurf_kernel_redraw_context",
    "netsurf_kernel_plot_stats_reset",
    "netsurf_kernel_plot_stats_snapshot",
    "netsurf_kernel_frontend_smoke",
    "netsurf_kernel_frontend_status",
    "netsurf_browser_render_html",
    "netsurf_browser_render_html_view",
    "netsurf_browser_prepare_html_view",
    "netsurf_browser_mouse_html_view",
    "netsurf_browser_scroll_html_view",
    "netsurf_browser_key_event",
    "netsurf_browser_consume_navigation",
    "netsurf_browser_invalidate_cache",
    "netsurf_browser_poll",
    "netsurf_browser_status",
};

static int host_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int host_copy_text(char *out, uint32_t out_capacity, const char *text) {
    size_t len;

    if (!out || out_capacity == 0 || !text) {
        return -1;
    }

    len = strlen(text);
    if (len + 1u > out_capacity) {
        return -1;
    }
    memcpy(out, text, len + 1u);
    return 0;
}

static int host_runtime_init_paths(void) {
    const char *repo_root = getenv("ZMOD_HOST_REPO_ROOT");
    const char *run_root = getenv("ZMOD_HOST_RUN_ROOT");
    const char *manifest_dir = getenv("ZMOD_HOST_MANIFEST_DIR");

    if (host_runtime_paths_ready) {
        return 0;
    }

    if (!repo_root || repo_root[0] == '\0') {
        repo_root = ".";
    }
    if (!run_root || run_root[0] == '\0') {
#ifdef __unix__
        if (getcwd(host_runtime_cwd, sizeof(host_runtime_cwd)) == 0) {
            return -1;
        }
#else
        run_root = ".";
#endif
    } else if (host_copy_text(host_runtime_cwd, sizeof(host_runtime_cwd), run_root) != 0) {
        return -1;
    }

    if (host_copy_text(host_runtime_repo_root, sizeof(host_runtime_repo_root), repo_root) != 0) {
        return -1;
    }
    if (manifest_dir && manifest_dir[0] != '\0') {
        if (host_copy_text(host_runtime_manifest_dir, sizeof(host_runtime_manifest_dir), manifest_dir) != 0) {
            return -1;
        }
    } else {
        host_runtime_manifest_dir[0] = '\0';
    }
#ifndef __unix__
    if (run_root && run_root[0] != '\0' &&
        host_copy_text(host_runtime_cwd, sizeof(host_runtime_cwd), run_root) != 0) {
        return -1;
    }
#endif

    host_runtime_paths_ready = 1;
    return 0;
}

static int host_runtime_resolve_path(const char *path, char *out, uint32_t out_capacity) {
    if (!path || !out || out_capacity == 0 || host_runtime_init_paths() != 0) {
        return -1;
    }
    if (path_is_absolute(path)) {
        return host_copy_text(out, out_capacity, path);
    }
    return join_path(host_runtime_cwd, path, out, out_capacity);
}

static int host_runtime_resolve_repo_path(const char *path, char *out, uint32_t out_capacity) {
    if (!path || !out || out_capacity == 0 || host_runtime_init_paths() != 0) {
        return -1;
    }
    if (path_is_absolute(path)) {
        return host_copy_text(out, out_capacity, path);
    }
    return join_path(host_runtime_repo_root, path, out, out_capacity);
}

static int host_runtime_resolve_input_path(const char *path, char *out, uint32_t out_capacity) {
    char candidate[HOST_MAX_PATH];
    struct stat st;

    if (!path || !out || out_capacity == 0 || host_runtime_init_paths() != 0) {
        return -1;
    }
    if (path_is_absolute(path)) {
        return host_copy_text(out, out_capacity, path);
    }
    if (join_path(host_runtime_cwd, path, candidate, sizeof(candidate)) == 0 &&
        stat(candidate, &st) == 0) {
        return host_copy_text(out, out_capacity, candidate);
    }
    if (host_runtime_manifest_dir[0] != '\0' &&
        join_path(host_runtime_manifest_dir, path, candidate, sizeof(candidate)) == 0 &&
        stat(candidate, &st) == 0) {
        return host_copy_text(out, out_capacity, candidate);
    }
    return join_path(host_runtime_cwd, path, out, out_capacity);
}

static int host_runtime_open_path(const char *path, const char *mode, FILE **out_file) {
    char resolved[HOST_MAX_PATH];
    FILE *file;

    if (!mode || !out_file || host_runtime_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    file = fopen(resolved, mode);
    if (!file) {
        return -1;
    }
    *out_file = file;
    return 0;
}

static int host_runtime_stat_path(const char *path, struct stat *st) {
    char resolved[HOST_MAX_PATH];

    if (!st || host_runtime_resolve_input_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    return stat(resolved, st);
}

static int host_runtime_scandir(const char *path, struct dirent ***out_list, int *out_count) {
#ifdef __unix__
    char resolved[HOST_MAX_PATH];
    struct dirent **entries = 0;
    int count;
    int kept = 0;

    if (!out_list || !out_count || host_runtime_resolve_input_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }

    count = scandir(resolved, &entries, 0, alphasort);
    if (count < 0) {
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i]->d_name, ".") == 0 || strcmp(entries[i]->d_name, "..") == 0) {
            free(entries[i]);
            entries[i] = 0;
            continue;
        }
        entries[kept++] = entries[i];
    }

    *out_list = entries;
    *out_count = kept;
    return 0;
#else
    (void)path;
    (void)out_list;
    (void)out_count;
    return -1;
#endif
}

static void host_runtime_free_scandir(struct dirent **list, int count) {
#ifdef __unix__
    if (!list) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(list[i]);
    }
    free(list);
#else
    (void)list;
    (void)count;
#endif
}

static int host_runtime_read_file_full_path(const char *path, unsigned char **out_data, uint32_t *out_size) {
    FILE *file = 0;
    long file_size;
    unsigned char *data = 0;

    if (!out_data || !out_size) {
        return -1;
    }

    file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    file_size = ftell(file);
    if (file_size < 0 || file_size > (long)HOST_MAX_SOURCE_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    data = (unsigned char *)malloc((size_t)file_size + 1u);
    if (!data) {
        fclose(file);
        return -1;
    }
    if (file_size != 0 && fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        free(data);
        return -1;
    }
    fclose(file);
    data[file_size] = '\0';
    *out_data = data;
    *out_size = (uint32_t)file_size;
    return 0;
}

static int host_runtime_read_file_resolved(const char *path, unsigned char **out_data, uint32_t *out_size) {
    char resolved[HOST_MAX_PATH];

    if (host_runtime_resolve_input_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    return host_runtime_read_file_full_path(resolved, out_data, out_size);
}

static int host_runtime_write_bytes(const char *path, const unsigned char *data, uint32_t size) {
    FILE *file = 0;

    if (host_runtime_open_path(path, "wb", &file) != 0) {
        return -1;
    }
    if (size != 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    return 0;
}

static int host_runtime_run_script(const char *script_name, const char *manifest_path, const char *root_path) {
#ifdef __unix__
    char script_path[HOST_MAX_PATH];
    pid_t child;
    int status = 0;

    if (!script_name || !manifest_path || !root_path ||
        host_runtime_resolve_repo_path(script_name, script_path, sizeof(script_path)) != 0) {
        return -1;
    }

    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execl("/bin/bash", "bash", script_path, manifest_path, root_path, (char *)0);
        _exit(127);
    }

    if (waitpid(child, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
#else
    (void)script_name;
    (void)manifest_path;
    (void)root_path;
    return -1;
#endif
}

static int host_runtime_find_manifest(const char *target, char *manifest_path, uint32_t manifest_capacity) {
    char candidate[HOST_MAX_PATH];
    char target_name[64];

    if (!target || !manifest_path || manifest_capacity == 0 || host_runtime_init_paths() != 0) {
        return -1;
    }
    if (snprintf(target_name, sizeof(target_name), "%s.zbuild", target) >= (int)sizeof(target_name)) {
        return -1;
    }

    if (snprintf(candidate, sizeof(candidate), "%s/examples/%s", host_runtime_repo_root, target_name) < (int)sizeof(candidate) &&
        access(candidate, F_OK) == 0) {
        return host_copy_text(manifest_path, manifest_capacity, candidate);
    }
    if (snprintf(candidate, sizeof(candidate), "%s/examples/zlang/%s", host_runtime_repo_root, target_name) < (int)sizeof(candidate) &&
        access(candidate, F_OK) == 0) {
        return host_copy_text(manifest_path, manifest_capacity, candidate);
    }

    return -1;
}

static void host_runtime_puts(const char *s) {
    fputs(s ? s : "", stdout);
    fflush(stdout);
}

static void host_runtime_put_hex64(unsigned long long value) {
    fprintf(stdout, "%llx", value);
    fflush(stdout);
}

static void host_runtime_put_dec64(unsigned long long value) {
    fprintf(stdout, "%llu", value);
    fflush(stdout);
}

static unsigned long long host_runtime_ticks(void) {
    time_t now = time(0);

    if (now == (time_t)-1) {
        return 0;
    }
    return (unsigned long long)now * 1000ull;
}

static uint64_t host_runtime_mem_total_kb(void) {
    return 1024ull * 1024ull;
}

static uint64_t host_runtime_mem_free_kb(void) {
    return 768ull * 1024ull;
}

static uint64_t host_runtime_mem_used_kb(void) {
    return host_runtime_mem_total_kb() - host_runtime_mem_free_kb();
}

static uint32_t host_runtime_cpu_count(void) {
    return 4u;
}

static uint32_t host_runtime_cpu_usage(uint32_t core) {
    (void)core;
    return 17u;
}

static uint32_t host_runtime_gfx_width(void) {
    return 1024u;
}

static uint32_t host_runtime_gfx_height(void) {
    return 768u;
}

static uint32_t host_runtime_gfx_pitch(void) {
    return host_runtime_gfx_width() * 4u;
}

static uint32_t host_runtime_gfx_format(void) {
    return 1u;
}

static void host_runtime_gfx_fill_rect(uint32_t x,
                                       uint32_t y,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t color) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

static void host_runtime_gfx_draw_rect(uint32_t x,
                                       uint32_t y,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t color) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

static void host_runtime_gfx_draw_line(uint32_t x0,
                                       uint32_t y0,
                                       uint32_t x1,
                                       uint32_t y1,
                                       uint32_t color) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)color;
}

static void host_runtime_gfx_clear(uint32_t color) {
    (void)color;
}

static void host_runtime_set_margin(uint32_t x, uint32_t y) {
    (void)x;
    (void)y;
}

static int host_runtime_mouse_enabled(void) {
    return 1;
}

static int host_runtime_mouse_x(void) {
    return 320;
}

static int host_runtime_mouse_y(void) {
    return 240;
}

static int host_runtime_mouse_buttons(void) {
    return 0;
}

static int host_runtime_mouse_dx(void) {
    return 0;
}

static int host_runtime_mouse_dy(void) {
    return 0;
}

static int host_runtime_mouse_wheel(void) {
    return 0;
}

static int host_runtime_mkdir(const char *path) {
#ifdef __unix__
    char resolved[HOST_MAX_PATH];
    struct stat st;

    if (host_runtime_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    if (mkdir(resolved, 0777) == 0) {
        return 0;
    }
    if (errno == EEXIST && stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    return -1;
#else
    (void)path;
    return -1;
#endif
}

static int host_runtime_delete(const char *path) {
    char resolved[HOST_MAX_PATH];

    if (host_runtime_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    return remove(resolved) == 0 ? 0 : -1;
}

static int host_runtime_write_file(const char *path, const char *text) {
    if (!text) {
        return -1;
    }
    return host_runtime_write_bytes(path, (const unsigned char *)text, (uint32_t)strlen(text)) == 0 ? 0 : -1;
}

static int host_runtime_cat_file(const char *path) {
    unsigned char *data = 0;
    uint32_t size = 0;
    int status = -1;

    if (host_runtime_read_file_resolved(path, &data, &size) != 0) {
        return -1;
    }
    if (size == 0 || fwrite(data, 1, size, stdout) == size) {
        fflush(stdout);
        status = 0;
    }
    free(data);
    return status;
}

static int host_runtime_file_size(const char *path) {
    struct stat st;

    if (host_runtime_stat_path(path, &st) != 0 || st.st_size < 0 || st.st_size > INT32_MAX) {
        return -1;
    }
    return (int)st.st_size;
}

static int host_runtime_read_file(const char *path, char *buffer, uint32_t capacity) {
    unsigned char *data = 0;
    uint32_t size = 0;
    int result = -1;

    if (!buffer || capacity == 0 || host_runtime_read_file_resolved(path, &data, &size) != 0 || size >= capacity) {
        free(data);
        return -1;
    }

    if (size != 0) {
        memcpy(buffer, data, size);
    }
    buffer[size] = '\0';
    result = (int)size;
    free(data);
    return result;
}

static int host_runtime_load_file_shared(const char *path) {
    unsigned char *data = 0;
    uint32_t size = 0;

    if (host_runtime_read_file_resolved(path, &data, &size) != 0) {
        return -1;
    }

    free(host_runtime_file_buffer);
    host_runtime_file_buffer = data;
    host_runtime_file_buffer_size = size;
    return (int)size;
}

static uint8_t *host_runtime_file_buffer_ptr(void) {
    return host_runtime_file_buffer;
}

static int host_runtime_rename(const char *old_path, const char *new_path) {
    char old_resolved[HOST_MAX_PATH];
    char new_resolved[HOST_MAX_PATH];

    if (host_runtime_resolve_path(old_path, old_resolved, sizeof(old_resolved)) != 0 ||
        host_runtime_resolve_path(new_path, new_resolved, sizeof(new_resolved)) != 0) {
        return -1;
    }
    return rename(old_resolved, new_resolved) == 0 ? 0 : -1;
}

static int host_runtime_copy_file(const char *src_path, const char *dst_path) {
    unsigned char *data = 0;
    uint32_t size = 0;
    int status = -1;

    if (host_runtime_read_file_resolved(src_path, &data, &size) != 0) {
        return -1;
    }
    if (host_runtime_write_bytes(dst_path, data, size) == 0) {
        status = 0;
    }
    free(data);
    return status;
}

static int host_runtime_strlen(const char *text) {
    return text ? (int)strlen(text) : -1;
}

static int host_runtime_strcmp(const char *a, const char *b) {
    if (!a || !b) {
        return -1;
    }
    return strcmp(a, b);
}

static int host_runtime_starts_with(const char *text, const char *prefix) {
    size_t prefix_len;

    if (!text || !prefix) {
        return 0;
    }
    prefix_len = strlen(prefix);
    return strncmp(text, prefix, prefix_len) == 0 ? 1 : 0;
}

static int host_runtime_atoi(const char *text) {
    return text ? atoi(text) : 0;
}

static int host_runtime_list_dir(const char *path) {
    struct dirent **entries = 0;
    int count = 0;

    if (host_runtime_scandir(path, &entries, &count) != 0) {
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        puts(entries[i]->d_name);
    }
    host_runtime_free_scandir(entries, count);
    return 0;
}

static int host_runtime_chdir(const char *path) {
#ifdef __unix__
    char resolved[HOST_MAX_PATH];
    struct stat st;

    if (host_runtime_resolve_path(path, resolved, sizeof(resolved)) != 0 ||
        stat(resolved, &st) != 0 ||
        !S_ISDIR(st.st_mode) ||
        host_copy_text(host_runtime_cwd, sizeof(host_runtime_cwd), resolved) != 0) {
        return -1;
    }
    return 0;
#else
    (void)path;
    return -1;
#endif
}

static int host_runtime_dir_count(const char *path) {
    struct dirent **entries = 0;
    int count = 0;

    if (host_runtime_scandir(path, &entries, &count) != 0) {
        return -1;
    }
    host_runtime_free_scandir(entries, count);
    return count;
}

static int host_runtime_dir_name(const char *path, uint32_t index, char *buffer, uint32_t capacity) {
    struct dirent **entries = 0;
    int count = 0;
    int status = -1;

    if (!buffer || capacity == 0 || host_runtime_scandir(path, &entries, &count) != 0) {
        return -1;
    }
    if ((int)index < count && host_copy_text(buffer, capacity, entries[index]->d_name) == 0) {
        status = 0;
    }
    host_runtime_free_scandir(entries, count);
    return status;
}

static int host_runtime_dir_type(const char *path, uint32_t index) {
#ifdef __unix__
    struct dirent **entries = 0;
    int count = 0;
    int status = -1;
    char resolved_dir[HOST_MAX_PATH];
    char child_path[HOST_MAX_PATH];
    struct stat st;

    if (host_runtime_scandir(path, &entries, &count) != 0 ||
        (int)index >= count ||
        host_runtime_resolve_path(path, resolved_dir, sizeof(resolved_dir)) != 0 ||
        join_path(resolved_dir, entries[index]->d_name, child_path, sizeof(child_path)) != 0 ||
        stat(child_path, &st) != 0) {
        host_runtime_free_scandir(entries, count);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        status = HOST_RUNTIME_ENTRY_DIR;
    } else if (S_ISREG(st.st_mode)) {
        status = HOST_RUNTIME_ENTRY_FILE;
    }
    host_runtime_free_scandir(entries, count);
    return status;
#else
    (void)path;
    (void)index;
    return -1;
#endif
}

static int host_runtime_dir_size(const char *path, uint32_t index) {
#ifdef __unix__
    struct dirent **entries = 0;
    int count = 0;
    char resolved_dir[HOST_MAX_PATH];
    char child_path[HOST_MAX_PATH];
    struct stat st;
    int status = -1;

    if (host_runtime_scandir(path, &entries, &count) != 0 ||
        (int)index >= count ||
        host_runtime_resolve_path(path, resolved_dir, sizeof(resolved_dir)) != 0 ||
        join_path(resolved_dir, entries[index]->d_name, child_path, sizeof(child_path)) != 0 ||
        stat(child_path, &st) != 0 ||
        st.st_size < 0 ||
        st.st_size > INT32_MAX) {
        host_runtime_free_scandir(entries, count);
        return -1;
    }

    status = (int)st.st_size;
    host_runtime_free_scandir(entries, count);
    return status;
#else
    (void)path;
    (void)index;
    return -1;
#endif
}

static int host_runtime_ztest(const char *target) {
    char manifest_path[HOST_MAX_PATH];

    if (host_runtime_find_manifest(target, manifest_path, sizeof(manifest_path)) != 0) {
        return -1;
    }
    return host_runtime_run_script("scripts/ztest-host.sh", manifest_path, host_runtime_cwd);
}

static int host_runtime_zinstall(const char *target) {
    char manifest_path[HOST_MAX_PATH];

    if (host_runtime_find_manifest(target, manifest_path, sizeof(manifest_path)) != 0) {
        return -1;
    }
    return host_runtime_run_script("scripts/zinstall-host.sh", manifest_path, host_runtime_cwd);
}

static host_runtime_task_slot_t *host_runtime_find_task_slot(uint32_t id) {
    for (uint32_t i = 0; i < HOST_RUNTIME_TASK_SLOTS; ++i) {
        if (host_runtime_task_slots[i].id == id) {
            return &host_runtime_task_slots[i];
        }
    }
    return 0;
}

static host_runtime_task_slot_t *host_runtime_alloc_task_slot(uint32_t id, const char *name) {
    for (uint32_t i = 0; i < HOST_RUNTIME_TASK_SLOTS; ++i) {
        if (host_runtime_task_slots[i].id == 0u) {
            host_runtime_task_slots[i].id = id;
            if (host_copy_text(host_runtime_task_slots[i].name,
                               sizeof(host_runtime_task_slots[i].name),
                               name && name[0] ? name : "task") != 0) {
                host_runtime_task_slots[i].name[0] = '\0';
            }
            return &host_runtime_task_slots[i];
        }
    }
    return 0;
}

static uint32_t host_runtime_kernel_task_submit_named(void (*fn)(void *arg), void *arg, const char *name) {
    uint32_t id;

    if (!fn) {
        return 0u;
    }

    id = host_runtime_task_next_id++;
    if (id == 0u) {
        id = host_runtime_task_next_id++;
    }
    if (!host_runtime_alloc_task_slot(id, name)) {
        return 0u;
    }

    /* Host ztest runs execute tasks eagerly but preserve task ids for wait/release parity. */
    fn(arg);
    return id;
}

static uint32_t host_runtime_kernel_task_submit(void (*fn)(void *arg), void *arg) {
    return host_runtime_kernel_task_submit_named(fn, arg, "task");
}

static uint32_t host_runtime_kernel_task_submit_async_named(void (*fn)(void *arg), void *arg, const char *name) {
    return host_runtime_kernel_task_submit_named(fn, arg, name);
}

static int host_runtime_kernel_task_async_supported(void) {
    return 1;
}

static int host_runtime_kernel_task_done(uint32_t id) {
    if (id == 0u) {
        return 1;
    }
    return host_runtime_find_task_slot(id) != 0 ? 1 : 0;
}

static void host_runtime_kernel_task_release(uint32_t id) {
    host_runtime_task_slot_t *slot;

    if (id == 0u) {
        return;
    }

    slot = host_runtime_find_task_slot(id);
    if (slot) {
        slot->id = 0u;
        slot->name[0] = '\0';
    }
}

static void host_runtime_kernel_task_wait(uint32_t id) {
    (void)id;
}

static int host_runtime_copy_rewritten_html(const uint8_t *html,
                                            uint32_t len,
                                            uint8_t *out,
                                            uint32_t out_capacity) {
    if (!html || !out || out_capacity == 0u || len >= out_capacity) {
        host_runtime_dom_status = 1u;
        return -1;
    }

    if (len != 0u) {
        memmove(out, html, len);
    }
    out[len] = '\0';
    host_runtime_dom_status = 0u;
    return (int)len;
}

static int host_runtime_web_style_prepare_document(const uint8_t *html,
                                                   uint32_t viewport_width,
                                                   uint32_t viewport_height) {
    (void)viewport_width;
    (void)viewport_height;
    return html ? 0 : -1;
}

static int host_runtime_web_style_for_tag(const uint8_t *html,
                                          uint32_t tag_pos,
                                          uint32_t viewport_width,
                                          uint32_t viewport_height,
                                          web_style_t *out_style) {
    (void)viewport_width;
    (void)viewport_height;

    if (!html || !out_style) {
        return -1;
    }
    if (html[tag_pos] == '\0') {
        return -1;
    }

    memset(out_style, 0, sizeof(*out_style));
    out_style->display = 1u;
    out_style->font_size = 16u;
    out_style->line_height = 16u;
    return 0;
}

static uint32_t host_runtime_netsurf_port_dom_status(void) {
    return host_runtime_dom_status;
}

static int host_runtime_netsurf_port_rewrite_html(const uint8_t *html,
                                                  uint32_t len,
                                                  uint8_t *out,
                                                  uint32_t out_capacity) {
    return host_runtime_copy_rewritten_html(html, len, out, out_capacity);
}

static int host_runtime_netsurf_port_rewrite_render_html(const uint8_t *html,
                                                         uint32_t len,
                                                         uint8_t *out,
                                                         uint32_t out_capacity) {
    return host_runtime_copy_rewritten_html(html, len, out, out_capacity);
}

static int host_runtime_export_value(const char *name, uint64_t *out) {
    if (host_streq(name, "puts")) {
        *out = (uint64_t)(uintptr_t)host_runtime_puts;
        return 0;
    }
    if (host_streq(name, "put_hex64")) {
        *out = (uint64_t)(uintptr_t)host_runtime_put_hex64;
        return 0;
    }
    if (host_streq(name, "put_dec64")) {
        *out = (uint64_t)(uintptr_t)host_runtime_put_dec64;
        return 0;
    }
    if (host_streq(name, "ticks")) {
        *out = (uint64_t)(uintptr_t)host_runtime_ticks;
        return 0;
    }
    if (host_streq(name, "kmalloc") || host_streq(name, "malloc")) {
        *out = (uint64_t)(uintptr_t)kmalloc;
        return 0;
    }
    if (host_streq(name, "kzalloc") || host_streq(name, "calloc")) {
        *out = (uint64_t)(uintptr_t)kzalloc;
        return 0;
    }
    if (host_streq(name, "kfree") || host_streq(name, "free")) {
        *out = (uint64_t)(uintptr_t)kfree;
        return 0;
    }
    if (host_streq(name, "kernel_task_submit_named")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_submit_named;
        return 0;
    }
    if (host_streq(name, "kernel_task_submit")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_submit;
        return 0;
    }
    if (host_streq(name, "kernel_task_submit_async_named")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_submit_async_named;
        return 0;
    }
    if (host_streq(name, "kernel_task_async_supported")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_async_supported;
        return 0;
    }
    if (host_streq(name, "kernel_task_done")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_done;
        return 0;
    }
    if (host_streq(name, "kernel_task_release")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_release;
        return 0;
    }
    if (host_streq(name, "kernel_task_wait")) {
        *out = (uint64_t)(uintptr_t)host_runtime_kernel_task_wait;
        return 0;
    }
    if (host_streq(name, "mem_total_kb") || host_streq(name, "status_memory_total_kb")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mem_total_kb;
        return 0;
    }
    if (host_streq(name, "mem_free_kb") || host_streq(name, "status_memory_free_kb")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mem_free_kb;
        return 0;
    }
    if (host_streq(name, "mem_used_kb") || host_streq(name, "status_memory_used_kb")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mem_used_kb;
        return 0;
    }
    if (host_streq(name, "cpu_count") || host_streq(name, "status_cpu_core_count")) {
        *out = (uint64_t)(uintptr_t)host_runtime_cpu_count;
        return 0;
    }
    if (host_streq(name, "cpu_usage")) {
        *out = (uint64_t)(uintptr_t)host_runtime_cpu_usage;
        return 0;
    }
    if (host_streq(name, "gfx_width")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_width;
        return 0;
    }
    if (host_streq(name, "gfx_height")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_height;
        return 0;
    }
    if (host_streq(name, "gfx_pitch")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_pitch;
        return 0;
    }
    if (host_streq(name, "gfx_format")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_format;
        return 0;
    }
    if (host_streq(name, "gfx_fill_rect")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_fill_rect;
        return 0;
    }
    if (host_streq(name, "gfx_draw_rect")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_draw_rect;
        return 0;
    }
    if (host_streq(name, "gfx_draw_line")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_draw_line;
        return 0;
    }
    if (host_streq(name, "gfx_clear")) {
        *out = (uint64_t)(uintptr_t)host_runtime_gfx_clear;
        return 0;
    }
    if (host_streq(name, "set_margin")) {
        *out = (uint64_t)(uintptr_t)host_runtime_set_margin;
        return 0;
    }
    if (host_streq(name, "web_style_prepare_document")) {
        *out = (uint64_t)(uintptr_t)host_runtime_web_style_prepare_document;
        return 0;
    }
    if (host_streq(name, "web_style_for_tag")) {
        *out = (uint64_t)(uintptr_t)host_runtime_web_style_for_tag;
        return 0;
    }
    if (host_streq(name, "netsurf_port_dom_status")) {
        *out = (uint64_t)(uintptr_t)host_runtime_netsurf_port_dom_status;
        return 0;
    }
    if (host_streq(name, "netsurf_port_rewrite_html")) {
        *out = (uint64_t)(uintptr_t)host_runtime_netsurf_port_rewrite_html;
        return 0;
    }
    if (host_streq(name, "netsurf_port_rewrite_render_html")) {
        *out = (uint64_t)(uintptr_t)host_runtime_netsurf_port_rewrite_render_html;
        return 0;
    }
    if (host_streq(name, "mouse_enabled")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_enabled;
        return 0;
    }
    if (host_streq(name, "mouse_x")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_x;
        return 0;
    }
    if (host_streq(name, "mouse_y")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_y;
        return 0;
    }
    if (host_streq(name, "mouse_buttons")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_buttons;
        return 0;
    }
    if (host_streq(name, "mouse_dx")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_dx;
        return 0;
    }
    if (host_streq(name, "mouse_dy")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_dy;
        return 0;
    }
    if (host_streq(name, "mouse_wheel")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mouse_wheel;
        return 0;
    }
    if (host_streq(name, "os_mkdir")) {
        *out = (uint64_t)(uintptr_t)host_runtime_mkdir;
        return 0;
    }
    if (host_streq(name, "os_delete")) {
        *out = (uint64_t)(uintptr_t)host_runtime_delete;
        return 0;
    }
    if (host_streq(name, "os_write_file")) {
        *out = (uint64_t)(uintptr_t)host_runtime_write_file;
        return 0;
    }
    if (host_streq(name, "os_cat_file")) {
        *out = (uint64_t)(uintptr_t)host_runtime_cat_file;
        return 0;
    }
    if (host_streq(name, "os_file_size")) {
        *out = (uint64_t)(uintptr_t)host_runtime_file_size;
        return 0;
    }
    if (host_streq(name, "os_read_file")) {
        *out = (uint64_t)(uintptr_t)host_runtime_read_file;
        return 0;
    }
    if (host_streq(name, "os_load_file_shared")) {
        *out = (uint64_t)(uintptr_t)host_runtime_load_file_shared;
        return 0;
    }
    if (host_streq(name, "os_file_buffer")) {
        *out = (uint64_t)(uintptr_t)host_runtime_file_buffer_ptr;
        return 0;
    }
    if (host_streq(name, "os_rename")) {
        *out = (uint64_t)(uintptr_t)host_runtime_rename;
        return 0;
    }
    if (host_streq(name, "os_copy_file")) {
        *out = (uint64_t)(uintptr_t)host_runtime_copy_file;
        return 0;
    }
    if (host_streq(name, "os_strlen")) {
        *out = (uint64_t)(uintptr_t)host_runtime_strlen;
        return 0;
    }
    if (host_streq(name, "os_strcmp")) {
        *out = (uint64_t)(uintptr_t)host_runtime_strcmp;
        return 0;
    }
    if (host_streq(name, "os_starts_with")) {
        *out = (uint64_t)(uintptr_t)host_runtime_starts_with;
        return 0;
    }
    if (host_streq(name, "os_atoi")) {
        *out = (uint64_t)(uintptr_t)host_runtime_atoi;
        return 0;
    }
    if (host_streq(name, "os_list_dir")) {
        *out = (uint64_t)(uintptr_t)host_runtime_list_dir;
        return 0;
    }
    if (host_streq(name, "os_chdir")) {
        *out = (uint64_t)(uintptr_t)host_runtime_chdir;
        return 0;
    }
    if (host_streq(name, "os_dir_count")) {
        *out = (uint64_t)(uintptr_t)host_runtime_dir_count;
        return 0;
    }
    if (host_streq(name, "os_dir_name")) {
        *out = (uint64_t)(uintptr_t)host_runtime_dir_name;
        return 0;
    }
    if (host_streq(name, "os_dir_type")) {
        *out = (uint64_t)(uintptr_t)host_runtime_dir_type;
        return 0;
    }
    if (host_streq(name, "os_dir_size")) {
        *out = (uint64_t)(uintptr_t)host_runtime_dir_size;
        return 0;
    }
    if (host_streq(name, "os_ztest")) {
        *out = (uint64_t)(uintptr_t)host_runtime_ztest;
        return 0;
    }
    if (host_streq(name, "os_zinstall")) {
        *out = (uint64_t)(uintptr_t)host_runtime_zinstall;
        return 0;
    }
    return -1;
}

int kernel_export_value(const char *name, uint64_t *out) {
    uint32_t count = (uint32_t)(sizeof(host_kernel_exports) / sizeof(host_kernel_exports[0]));

    if (host_runtime_mode && host_runtime_export_value(name, out) == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (host_streq(name, host_kernel_exports[i])) {
            if (out) {
                *out = 0x100000000ull + ((uint64_t)i * 0x100u);
            }
            return 0;
        }
    }

    return -1;
}

const kernel_export_t *kernel_exports_table(uint32_t *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    return 0;
}

static int append_bytes(char *out, uint32_t capacity, uint32_t *size, const char *data, uint32_t data_size) {
    if (*size > capacity || data_size > capacity - *size) {
        return -1;
    }

    if (data_size != 0) {
        memcpy(out + *size, data, data_size);
        *size += data_size;
    }
    return 0;
}

static int append_char(char *out, uint32_t capacity, uint32_t *size, char ch) {
    return append_bytes(out, capacity, size, &ch, 1);
}

static int dirname_from_path(const char *path, char *out, uint32_t out_capacity) {
    const char *slash = strrchr(path, '/');
    uint32_t len;

    if (!slash) {
        if (out_capacity == 0) {
            return -1;
        }
        out[0] = '\0';
        return 0;
    }

    len = (uint32_t)(slash - path + 1);
    if (len + 1u > out_capacity) {
        return -1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

static int join_path(const char *dir, const char *name, char *out, uint32_t out_capacity) {
    uint32_t dir_len = (uint32_t)strlen(dir);
    uint32_t name_len = (uint32_t)strlen(name);
    int needs_slash = dir_len != 0u && dir[dir_len - 1u] != '/';

    if (dir_len + (uint32_t)needs_slash + name_len + 1u > out_capacity) {
        return -1;
    }
    memcpy(out, dir, dir_len);
    if (needs_slash) {
        out[dir_len++] = '/';
    }
    memcpy(out + dir_len, name, name_len);
    out[dir_len + name_len] = '\0';
    return 0;
}

static int path_is_absolute(const char *path) {
    return path != 0 && path[0] == '/';
}

static const char *skip_line_spaces(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) {
        ++p;
    }
    return p;
}

static int parse_include_line(const char *line, const char *line_end, char *include_name, uint32_t include_capacity) {
    const char include_word[] = "include";
    const char *p = skip_line_spaces(line, line_end);
    uint32_t i;

    if (p >= line_end || *p != '#') {
        return 0;
    }
    ++p;
    p = skip_line_spaces(p, line_end);

    for (i = 0; include_word[i] != '\0'; ++i) {
        if (p + i >= line_end || p[i] != include_word[i]) {
            return 0;
        }
    }
    p += i;
    p = skip_line_spaces(p, line_end);

    if (p >= line_end || *p != '"') {
        return -1;
    }
    ++p;

    i = 0;
    while (p < line_end && *p != '"') {
        if (*p == '\n' || *p == '\r' || i + 1u >= include_capacity) {
            return -1;
        }
        include_name[i++] = *p++;
    }
    if (p >= line_end || *p != '"') {
        return -1;
    }
    include_name[i] = '\0';
    return 1;
}

static int parse_pragma_once_line(const char *line, const char *line_end) {
    const char pragma_word[] = "pragma";
    const char once_word[] = "once";
    const char *p = skip_line_spaces(line, line_end);
    uint32_t i;

    if (p >= line_end || *p != '#') {
        return 0;
    }
    ++p;
    p = skip_line_spaces(p, line_end);

    for (i = 0; pragma_word[i] != '\0'; ++i) {
        if (p + i >= line_end || p[i] != pragma_word[i]) {
            return 0;
        }
    }
    p += i;
    if (p < line_end && *p != ' ' && *p != '\t' && *p != '\r') {
        return 0;
    }
    p = skip_line_spaces(p, line_end);

    for (i = 0; once_word[i] != '\0'; ++i) {
        if (p + i >= line_end || p[i] != once_word[i]) {
            return 0;
        }
    }
    p += i;
    p = skip_line_spaces(p, line_end);
    return p == line_end;
}

static int source_once_index(const host_source_context_t *ctx, const char *path) {
    for (uint32_t i = 0; i < ctx->once_count; ++i) {
        if (strcmp(ctx->once_paths[i], path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int source_mark_once(host_source_context_t *ctx, const char *path) {
    size_t len = strlen(path);

    if (source_once_index(ctx, path) >= 0) {
        return 0;
    }
    if (ctx->once_count >= HOST_MAX_ONCE_PATHS || len + 1u > HOST_MAX_PATH) {
        return -1;
    }

    memcpy(ctx->once_paths[ctx->once_count], path, len + 1u);
    ++ctx->once_count;
    return 0;
}

static int read_file_raw(const char *path, char **out_data, uint32_t *out_size) {
    FILE *in;
    long input_size;
    char *data;

    in = fopen(path, "rb");
    if (!in) {
        return -1;
    }

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return -1;
    }
    input_size = ftell(in);
    if (input_size < 0 ||
        input_size > (long)HOST_MAX_SOURCE_SIZE ||
        fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return -1;
    }

    data = (char *)malloc((size_t)input_size + 1u);
    if (!data) {
        fclose(in);
        return -1;
    }

    if (fread(data, 1, (size_t)input_size, in) != (size_t)input_size) {
        fclose(in);
        free(data);
        return -1;
    }
    fclose(in);
    data[input_size] = '\0';

    *out_data = data;
    *out_size = (uint32_t)input_size;
    return 0;
}

static int expand_source_file(host_source_context_t *ctx,
                              const char *path,
                              char *out,
                              uint32_t out_capacity,
                              uint32_t *out_size,
                              uint32_t depth);

static int resolve_include_path(const char *source_dir,
                                const char *include_name,
                                const char *const *include_dirs,
                                uint32_t include_dir_count,
                                char *resolved_path,
                                uint32_t resolved_capacity) {
    if (path_is_absolute(include_name)) {
        if (strlen(include_name) + 1u > resolved_capacity) {
            return -1;
        }
        memcpy(resolved_path, include_name, strlen(include_name) + 1u);
        return 0;
    }

    if (join_path(source_dir, include_name, resolved_path, resolved_capacity) == 0) {
        FILE *probe = fopen(resolved_path, "rb");
        if (probe != 0) {
            fclose(probe);
            return 0;
        }
    }

    for (uint32_t i = 0; i < include_dir_count; ++i) {
        if (join_path(include_dirs[i], include_name, resolved_path, resolved_capacity) != 0) {
            continue;
        }
        FILE *probe = fopen(resolved_path, "rb");
        if (probe != 0) {
            fclose(probe);
            return 0;
        }
    }

    return -1;
}

static int expand_source_file(host_source_context_t *ctx,
                              const char *path,
                              char *out,
                              uint32_t out_capacity,
                              uint32_t *out_size,
                              uint32_t depth) {
    char *source = 0;
    uint32_t source_size = 0;
    char dir[512];
    uint32_t pos = 0;

    if (source_once_index(ctx, path) >= 0) {
        return 0;
    }

    if (depth >= HOST_MAX_INCLUDE_DEPTH ||
        dirname_from_path(path, dir, sizeof(dir)) != 0 ||
        read_file_raw(path, &source, &source_size) != 0) {
        return -1;
    }

    while (pos < source_size) {
        const char *line = source + pos;
        const char *line_end = line;
        char include_name[256];
        int include_status;

        while ((uint32_t)(line_end - source) < source_size && *line_end != '\n') {
            ++line_end;
        }

        include_status = parse_include_line(line, line_end, include_name, sizeof(include_name));
        if (include_status < 0) {
            free(source);
            return -1;
        }

        if (include_status > 0) {
            char include_path[HOST_MAX_PATH];
            if (resolve_include_path(dir,
                                     include_name,
                                     ctx->include_dirs,
                                     ctx->include_dir_count,
                                     include_path,
                                     sizeof(include_path)) != 0 ||
                expand_source_file(ctx,
                                   include_path,
                                   out,
                                   out_capacity,
                                   out_size,
                                   depth + 1u) != 0 ||
                append_char(out, out_capacity, out_size, '\n') != 0) {
                free(source);
                return -1;
            }
        } else if (parse_pragma_once_line(line, line_end)) {
            if (source_mark_once(ctx, path) != 0) {
                free(source);
                return -1;
            }
        } else if (append_bytes(out,
                                out_capacity,
                                out_size,
                                line,
                                (uint32_t)(line_end - line)) != 0 ||
                   append_char(out, out_capacity, out_size, '\n') != 0) {
            free(source);
            return -1;
        }

        pos = (uint32_t)(line_end - source);
        if (pos < source_size && source[pos] == '\n') {
            ++pos;
        }
    }

    free(source);
    return 0;
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

static int compile_object(const char *source_path,
                          const char *const *include_dirs,
                          uint32_t include_dir_count,
                          const char *object_name,
                          unsigned char *object,
                          uint32_t object_capacity,
                          uint32_t *object_size) {
    char *source = (char *)malloc(HOST_MAX_SOURCE_SIZE + 1u);
    char *asm_output = (char *)malloc(HOST_MAX_ASM_SIZE);
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t error_line = 0;
    char entry_label[64];
    char label_prefix[8];
    host_source_context_t source_ctx;
    int status = -1;

    if (!source || !asm_output) {
        goto out;
    }

    source_ctx.include_dirs = include_dirs;
    source_ctx.include_dir_count = include_dir_count;
    source_ctx.once_count = 0;
    if (expand_source_file(&source_ctx,
                           source_path,
                           source,
                           HOST_MAX_SOURCE_SIZE,
                           &source_size,
                           0) != 0) {
        fprintf(stderr, "%s: failed to load expanded source\n", source_path);
        goto out;
    }
    source[source_size] = '\0';

    make_zobject_prefix(object_name, label_prefix, sizeof(label_prefix));
    if (zscript_compile_source_object(source,
                                      source_size,
                                      label_prefix,
                                      asm_output,
                                      HOST_MAX_ASM_SIZE,
                                      &asm_size,
                                      &error_line,
                                      entry_label,
                                      sizeof(entry_label)) != 0) {
        if (zscript_last_error() == ZSCRIPT_ERROR_OUTPUT_FULL) {
            fprintf(stderr, "%s:%u: generated asm exceeded host build buffer\n", source_path, error_line);
        } else {
            fprintf(stderr, "%s:%u: unsupported .Z syntax\n", source_path, error_line);
        }
        goto out;
    }

    status = zobject_from_asm(asm_output,
                              asm_size,
                              entry_label,
                              object,
                              object_capacity,
                              object_size);
    if (status != 0) {
        fprintf(stderr, "%s: zobject_from_asm failed status=%d\n", source_path, status);
        if (getenv("ZMOD_DUMP_ASM") != 0) {
            char dump_path[256];
            FILE *dump;

            snprintf(dump_path, sizeof(dump_path), "/tmp/%s.asm", object_name);
            dump = fopen(dump_path, "wb");
            if (dump != 0) {
                fwrite(asm_output, 1, asm_size, dump);
                fclose(dump);
                fprintf(stderr, "%s: wrote asm dump %s\n", source_path, dump_path);
            }
        }
        goto out;
    }

    fprintf(stderr, "%s -> %s bytes=%u\n", source_path, object_name, *object_size);
    status = 0;

out:
    free(source);
    free(asm_output);
    return status;
}

static int write_file_raw(const char *path, const unsigned char *data, uint32_t size) {
    FILE *out = fopen(path, "wb");

    if (!out) {
        return -1;
    }
    if (size != 0 && fwrite(data, 1, size, out) != size) {
        fclose(out);
        return -1;
    }
    if (fclose(out) != 0) {
        return -1;
    }
    return 0;
}

static unsigned char *alloc_link_buffer(uint32_t size, int executable) {
    if (!executable) {
        return (unsigned char *)malloc(size);
    }

#ifdef __unix__
    {
        void *mapping = mmap(0,
                             size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS,
                             -1,
                             0);
        if (mapping == MAP_FAILED) {
            return 0;
        }
        return (unsigned char *)mapping;
    }
#else
    (void)size;
    return 0;
#endif
}

static void free_link_buffer(unsigned char *buffer, uint32_t size, int executable) {
    if (!buffer) {
        return;
    }

    if (!executable) {
        free(buffer);
        return;
    }

#ifdef __unix__
    munmap(buffer, size);
#else
    (void)size;
#endif
}

int main(int argc, char **argv) {
    unsigned char *objects_storage[HOST_MAX_OBJECTS];
    const unsigned char *objects[HOST_MAX_OBJECTS];
    uint32_t object_sizes[HOST_MAX_OBJECTS];
    const char *include_dirs[HOST_MAX_INCLUDE_DIRS];
    const char *output_path = 0;
    uint32_t include_dir_count = 0;
    uint32_t object_count;
    unsigned char *linked;
    uint32_t linked_size = 0;
    uint32_t error_line = 0;
    int arg_index = 1;
    int objects_only = 0;
    int run_after_link = 0;
    int module_link_check = 0;
    int has_expected_return = 0;
    uint64_t expected_return = 0;
    int status;

    while (arg_index < argc && argv[arg_index][0] == '-') {
        if (strcmp(argv[arg_index], "--include") == 0) {
            if (include_dir_count >= HOST_MAX_INCLUDE_DIRS || arg_index + 1 >= argc) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            include_dirs[include_dir_count++] = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (strcmp(argv[arg_index], "--objects-only") == 0) {
            if (objects_only) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            objects_only = 1;
            ++arg_index;
            continue;
        }
        if (strcmp(argv[arg_index], "--module-link") == 0) {
            if (module_link_check) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            module_link_check = 1;
            ++arg_index;
            continue;
        }
        if (strcmp(argv[arg_index], "--run") == 0) {
            if (run_after_link) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            run_after_link = 1;
            ++arg_index;
            continue;
        }
        if (strcmp(argv[arg_index], "--expect-return") == 0) {
            char *end = 0;

            if (has_expected_return || arg_index + 1 >= argc) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            expected_return = strtoull(argv[arg_index + 1], &end, 10);
            if (end == argv[arg_index + 1] || *end != '\0') {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            has_expected_return = 1;
            arg_index += 2;
            continue;
        }
        if (strcmp(argv[arg_index], "--output") == 0) {
            if (output_path != 0 || arg_index + 1 >= argc) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            output_path = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
        return 2;
    }

    if (argc - arg_index < 2 || ((argc - arg_index) % 2) != 0) {
        fprintf(stderr, "usage: zmod_link_host [--include dir/] [--objects-only] [--module-link] [--run] [--expect-return N] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
        return 2;
    }
    if (objects_only && run_after_link) {
        fprintf(stderr, "zmod_link_host: --run requires a linked executable target\n");
        return 2;
    }
    if (objects_only && module_link_check) {
        fprintf(stderr, "zmod_link_host: --module-link already writes object files before validating imports\n");
        return 2;
    }
    if (module_link_check && run_after_link) {
        fprintf(stderr, "zmod_link_host: --module-link cannot be combined with --run\n");
        return 2;
    }

    object_count = (uint32_t)(argc - arg_index) / 2u;
    if (object_count > HOST_MAX_OBJECTS) {
        fprintf(stderr, "too many objects\n");
        return 2;
    }

    memset(objects_storage, 0, sizeof(objects_storage));
    for (uint32_t i = 0; i < object_count; ++i) {
        objects_storage[i] = (unsigned char *)malloc(HOST_MAX_OBJECT_SIZE);
        if (!objects_storage[i]) {
            return 1;
        }

        if (compile_object(argv[arg_index + i * 2],
                           include_dirs,
                           include_dir_count,
                           argv[arg_index + i * 2 + 1],
                           objects_storage[i],
                           HOST_MAX_OBJECT_SIZE,
                           &object_sizes[i]) != 0) {
            for (uint32_t j = 0; j <= i; ++j) {
                free(objects_storage[j]);
            }
            return 1;
        }
        if (write_file_raw(argv[arg_index + i * 2 + 1], objects_storage[i], object_sizes[i]) != 0) {
            fprintf(stderr, "%s: failed to write object\n", argv[arg_index + i * 2 + 1]);
            for (uint32_t j = 0; j <= i; ++j) {
                free(objects_storage[j]);
            }
            return 1;
        }
        objects[i] = objects_storage[i];
    }

    if (objects_only) {
        fprintf(stderr, "built %u object(s) without host link validation\n", object_count);
        for (uint32_t i = 0; i < object_count; ++i) {
            free(objects_storage[i]);
        }
        return 0;
    }

    linked = alloc_link_buffer(HOST_MAX_LINK_SIZE, run_after_link);
    if (!linked) {
        for (uint32_t i = 0; i < object_count; ++i) {
            free(objects_storage[i]);
        }
        return 1;
    }

    host_runtime_mode = run_after_link || module_link_check;
    status = zobject_link_flat_many_ex(objects,
                                       object_sizes,
                                       object_count,
                                       linked,
                                       HOST_MAX_LINK_SIZE,
                                       (uint64_t)(uintptr_t)linked,
                                       &linked_size,
                                       &error_line,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0);
    host_runtime_mode = 0;
    if (status != 0) {
        fprintf(stderr, "link failed");
        if (zobject_last_error_reason()[0] != '\0') {
            fprintf(stderr, ": %s", zobject_last_error_reason());
            if (zobject_last_error_symbol()[0] != '\0') {
                fprintf(stderr, " %s", zobject_last_error_symbol());
            }
        }
        if (error_line != 0) {
            fprintf(stderr, " asm line %u", error_line);
        }
        fprintf(stderr, "\n");
        free_link_buffer(linked, HOST_MAX_LINK_SIZE, run_after_link);
        for (uint32_t i = 0; i < object_count; ++i) {
            free(objects_storage[i]);
        }
        return 1;
    }

    fprintf(stderr, "linked %u object(s), bytes=%u\n", object_count, linked_size);
    if (module_link_check) {
        fprintf(stderr, "validated module link against kernel exports\n");
    }
    if (output_path != 0) {
        if (write_file_raw(output_path, linked, linked_size) != 0) {
            fprintf(stderr, "%s: failed to write linked output\n", output_path);
            free_link_buffer(linked, HOST_MAX_LINK_SIZE, run_after_link);
            for (uint32_t i = 0; i < object_count; ++i) {
                free(objects_storage[i]);
            }
            return 1;
        }
        fprintf(stderr, "wrote linked output %s bytes=%u\n", output_path, linked_size);
    }

    if (run_after_link) {
        static const host_exec_api_t host_exec_api = {
            HOST_RUN_EXEC_API_MAGIC,
            1,
            host_runtime_puts,
            host_runtime_put_hex64,
            host_runtime_put_dec64,
            host_runtime_ticks,
        };
        uint64_t result = ((host_exec_program_ret_t)(uintptr_t)linked)(&host_exec_api);
        int passed = !has_expected_return || result == expected_return;

        fprintf(stderr, "run result=%llu", (unsigned long long)result);
        if (has_expected_return) {
            fprintf(stderr, " expected=%llu", (unsigned long long)expected_return);
        }
        fprintf(stderr, " status=%s\n", passed ? "ok" : "failed");
        if (!passed) {
            free_link_buffer(linked, HOST_MAX_LINK_SIZE, run_after_link);
            for (uint32_t i = 0; i < object_count; ++i) {
                free(objects_storage[i]);
            }
            return 1;
        }
    }

    free_link_buffer(linked, HOST_MAX_LINK_SIZE, run_after_link);
    for (uint32_t i = 0; i < object_count; ++i) {
        free(objects_storage[i]);
    }
    return 0;
}
