#include "kernel_exports.h"
#include "desktop.h"
#include "kernel.h"
#include "shell.h"

static int kernel_export_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

#define KERNEL_EXPORT(name, symbol) { name, (uint64_t)(uintptr_t)(symbol) }

static const kernel_export_t kernel_exports[] = {
    KERNEL_EXPORT("puts", console_puts),
    KERNEL_EXPORT("put_hex64", console_put_hex64),
    KERNEL_EXPORT("put_dec64", console_put_dec64),
    KERNEL_EXPORT("ticks", timer_ticks),
    KERNEL_EXPORT("status_memory_total_kb", status_memory_total_kb),
    KERNEL_EXPORT("mem_total_kb", status_memory_total_kb),
    KERNEL_EXPORT("status_memory_free_kb", status_memory_free_kb),
    KERNEL_EXPORT("mem_free_kb", status_memory_free_kb),
    KERNEL_EXPORT("status_memory_used_kb", status_memory_used_kb),
    KERNEL_EXPORT("mem_used_kb", status_memory_used_kb),
    KERNEL_EXPORT("status_cpu_core_count", status_cpu_core_count),
    KERNEL_EXPORT("cpu_count", status_cpu_core_count),
    KERNEL_EXPORT("cpu_usage", status_cpu_usage_percent),
    KERNEL_EXPORT("smp_submit_work", smp_submit_work),
    KERNEL_EXPORT("smp_work_done", smp_work_done),
    KERNEL_EXPORT("smp_wait_work", smp_wait_work),
    KERNEL_EXPORT("smp_pending_work_count", smp_pending_work_count),
    KERNEL_EXPORT("put_pixel", put_pixel),
    KERNEL_EXPORT("gfx_width", graphics_width),
    KERNEL_EXPORT("gfx_height", graphics_height),
    KERNEL_EXPORT("gfx_pitch", graphics_pitch),
    KERNEL_EXPORT("gfx_format", graphics_format),
    KERNEL_EXPORT("gfx_viewport_active", graphics_viewport_active),
    KERNEL_EXPORT("gfx_viewport_x", graphics_viewport_x),
    KERNEL_EXPORT("gfx_viewport_y", graphics_viewport_y),
    KERNEL_EXPORT("gfx_get_pixel", graphics_get_pixel),
    KERNEL_EXPORT("gfx_fill_rect", graphics_fill_rect),
    KERNEL_EXPORT("gfx_draw_rect", graphics_draw_rect),
    KERNEL_EXPORT("gfx_draw_line", graphics_draw_line),
    KERNEL_EXPORT("gfx_clear", graphics_clear),
    KERNEL_EXPORT("draw_text_at_pixel", console_draw_text_at_pixel),
    KERNEL_EXPORT("mouse_init", mouse_init),
    KERNEL_EXPORT("mouse_enabled", mouse_enabled),
    KERNEL_EXPORT("mouse_x", mouse_x),
    KERNEL_EXPORT("mouse_y", mouse_y),
    KERNEL_EXPORT("mouse_buttons", mouse_buttons),
    KERNEL_EXPORT("mouse_dx", mouse_dx),
    KERNEL_EXPORT("mouse_dy", mouse_dy),
    KERNEL_EXPORT("put_char_at", console_put_char_at),
    KERNEL_EXPORT("put_dec_at", console_put_dec_at),
    KERNEL_EXPORT("put_char_at_screen", console_put_char_at_screen),
    KERNEL_EXPORT("put_dec_at_screen", console_put_dec_at_screen),
    KERNEL_EXPORT("set_margin", console_set_margin),
    KERNEL_EXPORT("statusbar_enable", statusbar_enable),
    KERNEL_EXPORT("os_mkdir", shell_api_mkdir),
    KERNEL_EXPORT("os_delete", shell_api_delete),
    KERNEL_EXPORT("os_write_file", shell_api_write_file),
    KERNEL_EXPORT("os_cat_file", shell_api_cat_file),
    KERNEL_EXPORT("os_file_size", shell_api_file_size),
    KERNEL_EXPORT("os_read_file", shell_api_read_file),
    KERNEL_EXPORT("os_load_file_shared", shell_api_load_file_shared),
    KERNEL_EXPORT("os_file_buffer", shell_api_file_buffer),
    KERNEL_EXPORT("os_rename", shell_api_rename),
    KERNEL_EXPORT("os_copy_file", shell_api_copy_file),
    KERNEL_EXPORT("os_strlen", shell_api_strlen),
    KERNEL_EXPORT("os_strcmp", shell_api_strcmp),
    KERNEL_EXPORT("os_starts_with", shell_api_starts_with),
    KERNEL_EXPORT("os_atoi", shell_api_atoi),
    KERNEL_EXPORT("os_list_dir", shell_api_list_dir),
    KERNEL_EXPORT("os_chdir", shell_api_chdir),
    KERNEL_EXPORT("os_dir_count", shell_api_dir_count),
    KERNEL_EXPORT("os_dir_name", shell_api_dir_name),
    KERNEL_EXPORT("os_dir_type", shell_api_dir_type),
    KERNEL_EXPORT("os_dir_size", shell_api_dir_size),
    KERNEL_EXPORT("os_zbuild", shell_api_zbuild),
    KERNEL_EXPORT("os_ztest", shell_api_ztest),
    KERNEL_EXPORT("os_zinstall", shell_api_zinstall),
    KERNEL_EXPORT("os_zmod", shell_api_zmod),
    KERNEL_EXPORT("os_zunload", shell_api_zunload),
    KERNEL_EXPORT("os_zreload", shell_api_zreload),
    KERNEL_EXPORT("os_open_editor", desktop_api_open_editor),
};

#undef KERNEL_EXPORT

const kernel_export_t *kernel_exports_table(uint32_t *out_count) {
    if (out_count != 0) {
        *out_count = (uint32_t)(sizeof(kernel_exports) / sizeof(kernel_exports[0]));
    }
    return kernel_exports;
}

int kernel_export_value(const char *name, uint64_t *out) {
    uint32_t count = 0;
    const kernel_export_t *exports = kernel_exports_table(&count);

    if (name == 0 || out == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (kernel_export_streq(exports[i].name, name)) {
            *out = exports[i].value;
            return 0;
        }
    }

    return -1;
}
