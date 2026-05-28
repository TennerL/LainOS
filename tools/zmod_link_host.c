#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zobject.h"
#include "zscript.h"
#include "kernel_exports.h"

#define HOST_MAX_INCLUDE_DEPTH 16u
#define HOST_MAX_INCLUDE_DIRS 16u
#define HOST_MAX_SOURCE_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_ASM_SIZE (8u * 1024u * 1024u)
#define HOST_MAX_OBJECT_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_LINK_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_OBJECTS 32u
#define HOST_MAX_ONCE_PATHS 128u
#define HOST_MAX_PATH 768u

typedef struct {
    const char *const *include_dirs;
    uint32_t include_dir_count;
    char once_paths[HOST_MAX_ONCE_PATHS][HOST_MAX_PATH];
    uint32_t once_count;
} host_source_context_t;

void *kmalloc(uint32_t size) {
    return malloc(size);
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

int kernel_export_value(const char *name, uint64_t *out) {
    uint32_t count = (uint32_t)(sizeof(host_kernel_exports) / sizeof(host_kernel_exports[0]));

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
    int status;

    while (arg_index < argc && argv[arg_index][0] == '-') {
        if (strcmp(argv[arg_index], "--include") == 0) {
            if (include_dir_count >= HOST_MAX_INCLUDE_DIRS || arg_index + 1 >= argc) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            include_dirs[include_dir_count++] = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (strcmp(argv[arg_index], "--output") == 0) {
            if (output_path != 0 || arg_index + 1 >= argc) {
                fprintf(stderr, "usage: zmod_link_host [--include dir/] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
                return 2;
            }
            output_path = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        fprintf(stderr, "usage: zmod_link_host [--include dir/] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
        return 2;
    }

    if (argc - arg_index < 2 || ((argc - arg_index) % 2) != 0) {
        fprintf(stderr, "usage: zmod_link_host [--include dir/] [--output target.bin] source.Z object.zo [source.Z object.zo ...]\n");
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

    linked = (unsigned char *)malloc(HOST_MAX_LINK_SIZE);
    if (!linked) {
        for (uint32_t i = 0; i < object_count; ++i) {
            free(objects_storage[i]);
        }
        return 1;
    }

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
        free(linked);
        for (uint32_t i = 0; i < object_count; ++i) {
            free(objects_storage[i]);
        }
        return 1;
    }

    fprintf(stderr, "linked %u object(s), bytes=%u\n", object_count, linked_size);
    if (output_path != 0) {
        if (write_file_raw(output_path, linked, linked_size) != 0) {
            fprintf(stderr, "%s: failed to write linked output\n", output_path);
            free(linked);
            for (uint32_t i = 0; i < object_count; ++i) {
                free(objects_storage[i]);
            }
            return 1;
        }
        fprintf(stderr, "wrote linked output %s bytes=%u\n", output_path, linked_size);
    }

    free(linked);
    for (uint32_t i = 0; i < object_count; ++i) {
        free(objects_storage[i]);
    }
    return 0;
}
