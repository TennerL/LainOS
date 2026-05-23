#include <stdint.h>
#include "desktop.h"
#include "graphics.h"
#include "image.h"
#include "keyboard.h"
#include "kernel.h"
#include "kmem.h"
#include "lainfs.h"
#include "mouse.h"
#include "registry.h"
#include "shell.h"
#include "usb.h"

#define CURSOR_W 14u
#define CURSOR_H 20u
#define DESKTOP_LINE_MAX 128u
#define WINDOW_TITLE_H 30u
#define WINDOW_RESIZE_GRIP 18u
#define WINDOW_MIN_W 240u
#define WINDOW_MIN_H 140u
#define WINDOW_CONTROL_SIZE 14u
#define WINDOW_CONTROL_GAP 5u
#define TERMINAL_BUFFER_ROWS 160u
#define TERMINAL_BUFFER_COLS 160u
#define WINDOW_PREVIEW_MAX_PIXELS 8192u
#define FILE_BROWSER_MAX_ITEMS 48u
#define FILE_BROWSER_PATH_SIZE 128u
#define START_MENU_W 220u
#define START_MENU_ITEM_H 24u
#define DESKTOP_MODULE_INDEX_NONE 0xffffffffu
#define DESKTOP_MODULE_NAME_SIZE 64u
#define DESKTOP_MODS_DIR "/mods"
#define DESKTOP_MODULE_TICK_HZ 10u
#define DESKTOP_MODULE_APP_WINDOWS_MAX 6u
#define DESKTOP_EDITOR_BUFFER_SIZE 262144u
#define DESKTOP_EDITOR_NAME_SIZE 64u
#define DESKTOP_EDITOR_TOOLBAR_H 24u
#define DESKTOP_EDITOR_BUTTON_W 50u
#define DESKTOP_EDITOR_OUTPUT_SIZE 1536u
#define DESKTOP_EDITOR_OUTPUT_H 58u
#define FILE_ACTION_H 28u
#define FILE_ACTION_BUTTON_W 58u
#define DESKTOP_WM_PREVIEW_HZ 60u
#define DESKTOP_DAMAGE_MAX_RECTS 16u
#define DESKTOP_MOUSE_IDLE_REDRAW_HZ 20u
#define DESKTOP_MOUSE_DRAG_MAX_STEP 64
#define TERMINAL_CARET_W 8u
#define TERMINAL_CARET_H 2u
#define TERMINAL_CARET_BLINK_HZ 2u
#define DESKTOP_CLOCK_TEXT_W 152u
#define DESKTOP_CLOCK_PANEL_W 168u

typedef enum {
    FILE_KIND_OTHER = 0,
    FILE_KIND_SOURCE,
    FILE_KIND_MANIFEST,
    FILE_KIND_OBJECT,
    FILE_KIND_BINARY,
    FILE_KIND_LOG,
    FILE_KIND_IMAGE
} file_kind_t;

typedef enum {
    DESKTOP_APP_NONE = 0,
    DESKTOP_APP_TERMINAL,
    DESKTOP_APP_BROWSER,
    DESKTOP_APP_MODULES,
    DESKTOP_APP_MODULE_APP,
    DESKTOP_APP_EDITOR
} desktop_app_t;

typedef enum {
    DESKTOP_WM_IDLE = 0,
    DESKTOP_WM_DRAG,
    DESKTOP_WM_RESIZE
} desktop_wm_action_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    desktop_app_t app;
    const char *title;
} desktop_launcher_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t content_x;
    uint32_t content_y;
    uint32_t content_w;
    uint32_t content_h;
} desktop_window_t;

typedef struct {
    int maximized;
    desktop_window_t restore;
} desktop_window_state_t;

typedef struct {
    desktop_window_t window;
    desktop_window_state_t state;
    int open;
    int minimized;
    int bounds_ready;
    int unload_on_close;
    uint32_t module_index;
    char module_name[DESKTOP_MODULE_NAME_SIZE];
    unsigned long long last_tick;
} desktop_module_window_t;

static uint32_t cursor_back[CURSOR_W * CURSOR_H];
static uint32_t cursor_x;
static uint32_t cursor_y;
static int cursor_drawn;
static void cursor_restore(void);
static void cursor_draw_at(uint32_t x, uint32_t y);
static void desktop_begin_paint(void);
static desktop_window_t terminal_window;
static desktop_window_state_t terminal_window_state;
static int terminal_open;
static int terminal_minimized;
static int terminal_ready;
static int terminal_console_active;
static int terminal_bounds_ready;
static unsigned long long terminal_caret_last_tick;
static int terminal_caret_visible;
static uint32_t terminal_caret_back[TERMINAL_CARET_W * TERMINAL_CARET_H];
static int terminal_caret_back_valid;
static desktop_wm_action_t wm_action;
static desktop_window_t *wm_target_window;
static int wm_drag_dx;
static int wm_drag_dy;
static desktop_window_t wm_preview_window;
static desktop_window_t wm_last_preview_window;
static int wm_preview_drawn;
static unsigned long long wm_preview_last_tick;
static uint32_t wm_preview_count;
static uint32_t *wm_preview_x;
static uint32_t *wm_preview_y;
static uint32_t *wm_preview_color;
static char terminal_line[DESKTOP_LINE_MAX];
static uint32_t terminal_len;
static char terminal_buffer[TERMINAL_BUFFER_ROWS][TERMINAL_BUFFER_COLS];
static uint32_t terminal_buffer_row;
static uint32_t terminal_buffer_col;
static desktop_window_t files_window;
static desktop_window_state_t files_window_state;
static int files_open;
static int files_minimized;
static int files_bounds_ready;
static char files_path[FILE_BROWSER_PATH_SIZE];
static char files_selected_name[32];
static file_kind_t files_selected_kind;
static uint32_t files_selected_type;
static int files_context_open;
static uint32_t files_context_x;
static uint32_t files_context_y;
static char desktop_image_path[FILE_BROWSER_PATH_SIZE];
static int start_menu_open;
static desktop_window_t modules_window;
static desktop_window_state_t modules_window_state;
static int modules_open;
static int modules_minimized;
static int modules_bounds_ready;
static desktop_module_window_t *module_app_windows;
static desktop_module_window_t *module_focused;
static int desktop_deferred_redraw;
static desktop_window_t editor_window;
static desktop_window_state_t editor_window_state;
static int editor_open;
static int editor_minimized;
static int editor_bounds_ready;
static int editor_focused;
static int desktop_first_redraw = 1;
static uint32_t desktop_damage_count;
static uint32_t desktop_damage_x[DESKTOP_DAMAGE_MAX_RECTS];
static uint32_t desktop_damage_y[DESKTOP_DAMAGE_MAX_RECTS];
static uint32_t desktop_damage_w[DESKTOP_DAMAGE_MAX_RECTS];
static uint32_t desktop_damage_h[DESKTOP_DAMAGE_MAX_RECTS];
static char editor_name[DESKTOP_EDITOR_NAME_SIZE];
static char *editor_buffer;
static uint32_t editor_len;
static uint32_t editor_cursor;
static uint32_t editor_top_line;
static const char *editor_status;
static char editor_output[DESKTOP_EDITOR_OUTPUT_SIZE];
static uint32_t editor_output_len;
static int editor_scroll_drag;
static unsigned long long desktop_mouse_last_activity_tick;
static uint64_t desktop_clock_last_stamp = 0xffffffffffffffffull;
static const desktop_launcher_t launchers[] = {
    { 20u, 48u, 54u, 54u, DESKTOP_APP_TERMINAL, "Terminal" },
    { 20u, 124u, 54u, 54u, DESKTOP_APP_BROWSER, "Files" },
};

static int desktop_alloc_state(void) {
    if (wm_preview_x == 0) {
        wm_preview_x = (uint32_t *)kmalloc(sizeof(uint32_t) * WINDOW_PREVIEW_MAX_PIXELS);
    }
    if (wm_preview_y == 0) {
        wm_preview_y = (uint32_t *)kmalloc(sizeof(uint32_t) * WINDOW_PREVIEW_MAX_PIXELS);
    }
    if (wm_preview_color == 0) {
        wm_preview_color = (uint32_t *)kmalloc(sizeof(uint32_t) * WINDOW_PREVIEW_MAX_PIXELS);
    }
    if (module_app_windows == 0) {
        module_app_windows = (desktop_module_window_t *)kzalloc(sizeof(desktop_module_window_t) *
                                                                DESKTOP_MODULE_APP_WINDOWS_MAX);
    }

    return wm_preview_x != 0 &&
           wm_preview_y != 0 &&
           wm_preview_color != 0 &&
           module_app_windows != 0;
}

static inline void cpu_pause(void) {
    status_cpu_enter_idle();
    __asm__ __volatile__("sti; hlt");
    status_cpu_leave_idle();
}

static void desktop_mouse_snapshot(uint32_t *out_x, uint32_t *out_y, int *out_buttons) {
    int x;
    int y;
    int buttons;

    mouse_snapshot(&x, &y, &buttons);

    if (out_x != 0) {
        *out_x = x < 0 ? 0u : (uint32_t)x;
    }
    if (out_y != 0) {
        *out_y = y < 0 ? 0u : (uint32_t)y;
    }
    if (out_buttons != 0) {
        *out_buttons = buttons & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
    }
}

static void desktop_begin_paint(void) {
    cursor_restore();
    console_cursor_enable(0);
}

static int desktop_mouse_background_redraw_ready(void) {
    unsigned int hz = timer_frequency();
    unsigned long long interval;
    unsigned long long now;

    if (hz == 0u) {
        hz = 100u;
    }
    interval = hz / DESKTOP_MOUSE_IDLE_REDRAW_HZ;
    if (interval == 0ull) {
        interval = 1ull;
    }

    now = timer_ticks();
    return now - desktop_mouse_last_activity_tick >= interval;
}

static void desktop_mouse_apply_drag_motion(uint32_t *x,
                                            uint32_t *y,
                                            uint32_t last_x,
                                            uint32_t last_y,
                                            int motion_dx,
                                            int motion_dy) {
    int next_x;
    int next_y;

    if (x == 0 || y == 0) {
        return;
    }

    if (motion_dx > DESKTOP_MOUSE_DRAG_MAX_STEP) {
        motion_dx = DESKTOP_MOUSE_DRAG_MAX_STEP;
    } else if (motion_dx < -DESKTOP_MOUSE_DRAG_MAX_STEP) {
        motion_dx = -DESKTOP_MOUSE_DRAG_MAX_STEP;
    }

    if (motion_dy > DESKTOP_MOUSE_DRAG_MAX_STEP) {
        motion_dy = DESKTOP_MOUSE_DRAG_MAX_STEP;
    } else if (motion_dy < -DESKTOP_MOUSE_DRAG_MAX_STEP) {
        motion_dy = -DESKTOP_MOUSE_DRAG_MAX_STEP;
    }

    next_x = (int)last_x + motion_dx;
    next_y = (int)last_y + motion_dy;

    if (next_x < 0) {
        next_x = 0;
    }
    if (next_y < 0) {
        next_y = 0;
    }
    if (graphics_width() != 0u && (uint32_t)next_x >= graphics_width()) {
        next_x = (int)(graphics_width() - 1u);
    }
    if (graphics_height() != 0u && (uint32_t)next_y >= graphics_height()) {
        next_y = (int)(graphics_height() - 1u);
    }

    if ((uint32_t)next_x != *x || (uint32_t)next_y != *y) {
        mouse_set_position(next_x, next_y);
    }
    *x = (uint32_t)next_x;
    *y = (uint32_t)next_y;
}

static void desktop_terminal_draw(int fresh);
static void desktop_redraw_all(void);
static void desktop_damage_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static void desktop_damage_window(const desktop_window_t *win);
static void desktop_damage_taskbar(void);
static void desktop_damage_module_windows(void);
static void desktop_damage_app(desktop_app_t app);
static void desktop_damage_full(void);
static int desktop_damage_intersects_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static void desktop_start_menu_rect(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
static void desktop_terminal_open(void);
static void desktop_terminal_sync_console_cursor(void);
static void desktop_terminal_after_output(void);
static void desktop_terminal_draw_caret(void);
static void desktop_terminal_erase_caret(void);
static int desktop_terminal_caret_tick_due(void);
static void desktop_open_module_app(uint32_t index);
static int desktop_open_module_app_by_name(const char *name, int load_from_mods);
static uint32_t desktop_app_catalog_count(void);
static int desktop_app_catalog_name_at(uint32_t app_index, char *out, uint32_t out_size);
static int desktop_any_module_app_open(void);
static int desktop_any_module_app_minimized(void);
static desktop_module_window_t *desktop_find_module_app_window(uint32_t index);
static desktop_module_window_t *desktop_allocate_module_app_window(void);
static desktop_module_window_t *desktop_top_module_app_window_at(uint32_t x, uint32_t y);
static void desktop_terminal_blur(void);
static int point_in_rect(uint32_t px, uint32_t py, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static uint32_t desktop_taskbar_height(void);
static int text_equals(const char *a, const char *b);
static void text_copy_limited(char *dst, uint32_t dst_size, const char *src);
static void desktop_draw_taskbar(void);
static int desktop_taskbar_clock_tick_due(void);

typedef struct {
    uint32_t bg_top;
    uint32_t bg_bottom;
    uint32_t topbar;
    uint32_t taskbar;
    uint32_t panel;
    uint32_t panel_inner;
    uint32_t accent;
    uint32_t accent_soft;
    uint32_t text;
    uint32_t button;
    uint32_t task_active;
    uint32_t task_inactive;
} desktop_theme_t;

static uint32_t *desktop_background_cache;
static uint32_t desktop_background_cache_capacity;
static uint32_t desktop_background_cache_width;
static uint32_t desktop_background_cache_height;
static desktop_theme_t desktop_background_cache_theme;
static char desktop_background_cache_path[FILE_BROWSER_PATH_SIZE];
static int desktop_background_cache_valid;

static desktop_theme_t desktop_theme(void) {
    desktop_theme_t theme;

    (void)registry_get_u32("desktop.bg.top", 0x35063eu, &theme.bg_top);
    (void)registry_get_u32("desktop.bg.bottom", 0x2b1d3du, &theme.bg_bottom);
    (void)registry_get_u32("desktop.topbar", 0x100b18u, &theme.topbar);
    (void)registry_get_u32("desktop.taskbar", 0x171020u, &theme.taskbar);
    (void)registry_get_u32("desktop.panel", 0x221a2du, &theme.panel);
    (void)registry_get_u32("desktop.panel.inner", 0x3a2f49u, &theme.panel_inner);
    (void)registry_get_u32("desktop.accent", 0xe05f4fu, &theme.accent);
    (void)registry_get_u32("desktop.accent.soft", 0xb98556u, &theme.accent_soft);
    (void)registry_get_u32("desktop.text", 0xf6eadbu, &theme.text);
    (void)registry_get_u32("desktop.button", 0x8f3f62u, &theme.button);
    (void)registry_get_u32("desktop.task.active", 0x4b2347u, &theme.task_active);
    (void)registry_get_u32("desktop.task.inactive", 0x2d2038u, &theme.task_inactive);
    return theme;
}

static int desktop_theme_equals(const desktop_theme_t *a, const desktop_theme_t *b) {
    return a != 0 && b != 0 &&
           a->bg_top == b->bg_top &&
           a->bg_bottom == b->bg_bottom &&
           a->topbar == b->topbar &&
           a->taskbar == b->taskbar &&
           a->panel == b->panel &&
           a->panel_inner == b->panel_inner &&
           a->accent == b->accent &&
           a->accent_soft == b->accent_soft &&
           a->text == b->text &&
           a->button == b->button &&
           a->task_active == b->task_active &&
           a->task_inactive == b->task_inactive;
}

static int desktop_background_cache_ready(uint32_t width,
                                          uint32_t height,
                                          const desktop_theme_t *theme,
                                          const char *image_path) {
    return desktop_background_cache_valid &&
           desktop_background_cache != 0 &&
           desktop_background_cache_width == width &&
           desktop_background_cache_height == height &&
           desktop_theme_equals(&desktop_background_cache_theme, theme) &&
           text_equals(desktop_background_cache_path, image_path);
}

static int desktop_background_ensure_cache(uint32_t width, uint32_t height) {
    uint32_t pixels;

    if (width == 0u || height == 0u || width > 0xffffffffu / height) {
        return 0;
    }

    pixels = width * height;
    if (desktop_background_cache != 0 && desktop_background_cache_capacity >= pixels) {
        return 1;
    }

    if (desktop_background_cache != 0) {
        kfree(desktop_background_cache);
        desktop_background_cache = 0;
        desktop_background_cache_capacity = 0;
        desktop_background_cache_valid = 0;
    }

    desktop_background_cache = (uint32_t *)kmalloc(pixels * sizeof(uint32_t));
    if (desktop_background_cache == 0) {
        return 0;
    }
    desktop_background_cache_capacity = pixels;
    return 1;
}

static void desktop_background(void) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    desktop_theme_t theme = desktop_theme();
    const char *image_path = registry_get("desktop.bg.image");

    if (image_path == 0) {
        image_path = "";
    }

    if (desktop_background_cache_ready(width, height, &theme, image_path) &&
        graphics_draw_rect_packed(0,
                                  0,
                                  width,
                                  height,
                                  desktop_background_cache,
                                  desktop_background_cache_width * desktop_background_cache_height) == 0) {
        return;
    }

    graphics_fill_vertical_gradient(0, 0, width, height, theme.bg_top, theme.bg_bottom);
    if (image_path != 0 && image_path[0] != '\0') {
        int size = shell_api_load_file_shared(image_path);
        if (size > 0) {
            (void)image_decode_to_screen(shell_api_file_buffer(), (uint32_t)size, 0, 0, 0);
        }
    }

    if (desktop_background_ensure_cache(width, height) &&
        graphics_capture_rect_packed(0,
                                     0,
                                     width,
                                     height,
                                     desktop_background_cache,
                                     desktop_background_cache_capacity) == 0) {
        desktop_background_cache_width = width;
        desktop_background_cache_height = height;
        desktop_background_cache_theme = theme;
        text_copy_limited(desktop_background_cache_path, sizeof(desktop_background_cache_path), image_path);
        desktop_background_cache_valid = 1;
    }
}

static void desktop_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill) {
    desktop_theme_t theme = desktop_theme();

    graphics_fill_rect(x, y, w, h, fill);
    graphics_draw_rect(x, y, w, h, theme.text);
    if (w > 2u && h > 2u) {
        graphics_draw_rect(x + 1u, y + 1u, w - 2u, h - 2u, theme.panel_inner);
    }
}

static void desktop_icon(uint32_t x, uint32_t y, uint32_t color) {
    desktop_theme_t theme = desktop_theme();

    graphics_fill_rect(x, y, 34, 28, color);
    graphics_draw_rect(x, y, 34, 28, theme.text);
    graphics_fill_rect(x + 7u, y + 33u, 20, 4, theme.text);
}

static void desktop_draw_launcher(const desktop_launcher_t *launcher) {
    desktop_theme_t theme = desktop_theme();
    uint32_t icon_x = launcher->x + 10u;
    uint32_t icon_y = launcher->y;
    uint32_t color = launcher->app == DESKTOP_APP_TERMINAL ? theme.button : theme.accent;

    desktop_icon(icon_x, icon_y, color);
    console_draw_text_at_pixel(launcher->x,
                               launcher->y + 42u,
                               launcher->title,
                               theme.text,
                               theme.bg_bottom);
}

static uint32_t desktop_draw_task_button(uint32_t x, const char *label, int active) {
    uint32_t height = graphics_height();
    uint32_t task_h = desktop_taskbar_height();
    desktop_theme_t theme = desktop_theme();
    uint32_t fill = active ? theme.task_active : theme.task_inactive;

    graphics_fill_rect(x, height - task_h + 10u, 88u, task_h - 20u, fill);
    graphics_draw_rect(x, height - task_h + 10u, 88u, task_h - 20u, active ? theme.accent : theme.panel_inner);
    console_draw_text_at_pixel(x + 8u, height - task_h + 14u, label, theme.text, fill);
    return x + 96u;
}

static uint32_t desktop_taskbar_button_limit(void) {
    uint32_t width = graphics_width();

    if (width > DESKTOP_CLOCK_PANEL_W + 24u) {
        return width - DESKTOP_CLOCK_PANEL_W - 16u;
    }
    return width;
}

static int desktop_taskbar_button_visible(uint32_t x) {
    uint32_t limit = desktop_taskbar_button_limit();

    return x + 88u <= limit;
}

static void desktop_draw_task_buttons(void) {
    uint32_t x = 76u;

    if ((terminal_open || terminal_minimized) && desktop_taskbar_button_visible(x)) {
        x = desktop_draw_task_button(x, "Terminal", terminal_open);
    }
    if ((files_open || files_minimized) && desktop_taskbar_button_visible(x)) {
        x = desktop_draw_task_button(x, "Files", files_open);
    }
    if ((modules_open || modules_minimized) && desktop_taskbar_button_visible(x)) {
        x = desktop_draw_task_button(x, "Modules", modules_open);
    }
    if ((desktop_any_module_app_open() || desktop_any_module_app_minimized()) && desktop_taskbar_button_visible(x)) {
        x = desktop_draw_task_button(x, "Module", desktop_any_module_app_open());
    }
    if ((editor_open || editor_minimized) && desktop_taskbar_button_visible(x)) {
        x = desktop_draw_task_button(x, "Editor", editor_open);
    }
    (void)x;
}

static desktop_app_t desktop_task_button_at(uint32_t px, uint32_t py) {
    uint32_t x = 76u;
    uint32_t height = graphics_height();
    uint32_t task_h = desktop_taskbar_height();

    if (py < height - task_h + 10u || py >= height - task_h + 10u + task_h - 20u) {
        return DESKTOP_APP_NONE;
    }
    if (terminal_open || terminal_minimized) {
        if (!desktop_taskbar_button_visible(x)) return DESKTOP_APP_NONE;
        if (point_in_rect(px, py, x, height - task_h + 10u, 88u, task_h - 20u)) return DESKTOP_APP_TERMINAL;
        x += 96u;
    }
    if (files_open || files_minimized) {
        if (!desktop_taskbar_button_visible(x)) return DESKTOP_APP_NONE;
        if (point_in_rect(px, py, x, height - task_h + 10u, 88u, task_h - 20u)) return DESKTOP_APP_BROWSER;
        x += 96u;
    }
    if (modules_open || modules_minimized) {
        if (!desktop_taskbar_button_visible(x)) return DESKTOP_APP_NONE;
        if (point_in_rect(px, py, x, height - task_h + 10u, 88u, task_h - 20u)) return DESKTOP_APP_MODULES;
        x += 96u;
    }
    if (desktop_any_module_app_open() || desktop_any_module_app_minimized()) {
        if (!desktop_taskbar_button_visible(x)) return DESKTOP_APP_NONE;
        if (point_in_rect(px, py, x, height - task_h + 10u, 88u, task_h - 20u)) return DESKTOP_APP_MODULE_APP;
        x += 96u;
    }
    if (editor_open || editor_minimized) {
        if (!desktop_taskbar_button_visible(x)) return DESKTOP_APP_NONE;
        if (point_in_rect(px, py, x, height - task_h + 10u, 88u, task_h - 20u)) return DESKTOP_APP_EDITOR;
    }

    return DESKTOP_APP_NONE;
}

static void desktop_put_two_digits(char *out, uint32_t value) {
    out[0] = (char)('0' + ((value / 10u) % 10u));
    out[1] = (char)('0' + (value % 10u));
}

static void desktop_put_four_digits(char *out, uint32_t value) {
    out[0] = (char)('0' + ((value / 1000u) % 10u));
    out[1] = (char)('0' + ((value / 100u) % 10u));
    out[2] = (char)('0' + ((value / 10u) % 10u));
    out[3] = (char)('0' + (value % 10u));
}

static int desktop_format_clock(char *out, uint32_t out_size) {
    rtc_time_t now;

    if (out == 0 || out_size < 20u || clock_get_rtc_time(&now) != 0 || !clock_rtc_time_valid(&now)) {
        return -1;
    }

    desktop_put_four_digits(out, now.year);
    out[4] = '-';
    desktop_put_two_digits(out + 5, now.month);
    out[7] = '-';
    desktop_put_two_digits(out + 8, now.day);
    out[10] = ' ';
    desktop_put_two_digits(out + 11, now.hour);
    out[13] = ':';
    desktop_put_two_digits(out + 14, now.minute);
    out[16] = ':';
    desktop_put_two_digits(out + 17, now.second);
    out[19] = '\0';
    return 0;
}

static int desktop_taskbar_clock_rect(uint32_t *out_x, uint32_t *out_y, uint32_t *out_w, uint32_t *out_h) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t task_h = desktop_taskbar_height();
    uint32_t rect_y;
    uint32_t rect_h;

    if (width < DESKTOP_CLOCK_PANEL_W + 96u || height < task_h) {
        return 0;
    }

    rect_y = height - task_h;
    rect_h = task_h;
    if (task_h >= 30u) {
        rect_y = height - task_h + 8u;
        rect_h = task_h - 16u;
    }

    if (out_x != 0) {
        *out_x = width - DESKTOP_CLOCK_PANEL_W - 8u;
    }
    if (out_y != 0) {
        *out_y = rect_y;
    }
    if (out_w != 0) {
        *out_w = DESKTOP_CLOCK_PANEL_W;
    }
    if (out_h != 0) {
        *out_h = rect_h;
    }
    return 1;
}

static void desktop_draw_taskbar_clock(void) {
    uint32_t height = graphics_height();
    uint32_t task_h = desktop_taskbar_height();
    desktop_theme_t theme = desktop_theme();
    char text[24];
    uint32_t panel_x;
    uint32_t panel_y;
    uint32_t panel_w;
    uint32_t panel_h;
    uint32_t text_y;

    if (!desktop_taskbar_clock_rect(&panel_x, &panel_y, &panel_w, &panel_h) ||
        desktop_format_clock(text, sizeof(text)) != 0) {
        return;
    }

    text_y = height - task_h + (task_h >= 30u ? 13u : 8u);
    graphics_fill_rect(panel_x, panel_y, panel_w, panel_h, task_h >= 30u ? theme.task_inactive : theme.taskbar);
    if (task_h >= 30u) {
        graphics_draw_rect(panel_x, panel_y, panel_w, panel_h, theme.panel_inner);
    }
    console_draw_text_at_pixel(panel_x + 8u, text_y, text, theme.text, task_h >= 30u ? theme.task_inactive : theme.taskbar);
}

static int desktop_taskbar_clock_tick_due(void) {
    rtc_time_t now;
    uint64_t stamp;

    if (clock_get_rtc_time(&now) != 0 || !clock_rtc_time_valid(&now)) {
        return 0;
    }

    stamp = clock_unix_time_from_rtc(&now);
    if (desktop_clock_last_stamp == 0xffffffffffffffffull) {
        desktop_clock_last_stamp = stamp;
        return 0;
    }
    if (stamp == desktop_clock_last_stamp) {
        return 0;
    }
    desktop_clock_last_stamp = stamp;
    return 1;
}

static void desktop_draw_taskbar(void) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t task_h = height >= 160u ? 34u : 24u;
    desktop_theme_t theme = desktop_theme();

    if (height < task_h) {
        return;
    }

    graphics_fill_rect(0, height - task_h, width, task_h, theme.taskbar);
    graphics_fill_rect(0, height - task_h, width, 1, theme.accent_soft);
    if (width > 120u) {
        graphics_fill_rect(12u, height - task_h + 8u, 48u, task_h - 16u, theme.button);
        graphics_draw_rect(12u, height - task_h + 8u, 48u, task_h - 16u, theme.text);
        console_draw_text_at_pixel(22u, height - task_h + 13u, "Apps", theme.text, theme.button);
        desktop_draw_task_buttons();
    }
    desktop_draw_taskbar_clock();
}

static void desktop_draw_base(void) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t top_h = height >= 160u ? 24u : 16u;
    desktop_theme_t theme = desktop_theme();
    unsigned int i;

    desktop_background();

    graphics_fill_rect(0, 0, width, top_h, theme.topbar);
    graphics_fill_rect(0, top_h - 1u, width, 1, theme.accent);
    console_draw_text_at_pixel(12u, 8u, "LainOS Desktop", theme.text, theme.topbar);

    if (width > 180u && height > 140u) {
        for (i = 0; i < sizeof(launchers) / sizeof(launchers[0]); ++i) {
            desktop_draw_launcher(&launchers[i]);
        }
    }

    desktop_draw_taskbar();
}

static int point_in_rect(uint32_t px, uint32_t py, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static int desktop_rects_intersect(uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                                   uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh) {
    return aw != 0u && ah != 0u && bw != 0u && bh != 0u &&
           ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

static int desktop_cursor_intersects_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return cursor_drawn &&
           desktop_rects_intersect(cursor_x, cursor_y, CURSOR_W, CURSOR_H, x, y, w, h);
}

static uint32_t desktop_taskbar_height(void) {
    return graphics_height() >= 160u ? 34u : 24u;
}

static desktop_app_t launcher_at(uint32_t x, uint32_t y) {
    for (unsigned int i = 0; i < sizeof(launchers) / sizeof(launchers[0]); ++i) {
        const desktop_launcher_t *launcher = &launchers[i];
        if (point_in_rect(x, y, launcher->x, launcher->y, launcher->w, launcher->h)) {
            return launcher->app;
        }
    }

    return DESKTOP_APP_NONE;
}

static void desktop_update_window_content(desktop_window_t *win) {
    win->content_x = win->x + 10u;
    win->content_y = win->y + 34u;
    win->content_w = win->w > 20u ? win->w - 20u : win->w;
    win->content_h = win->h > 44u ? win->h - 44u : win->h;
}

static void desktop_clamp_window(desktop_window_t *win) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t bottom = height > desktop_taskbar_height() + 4u ? height - desktop_taskbar_height() - 4u : height;

    if (win->w < WINDOW_MIN_W) {
        win->w = WINDOW_MIN_W;
    }
    if (win->h < WINDOW_MIN_H) {
        win->h = WINDOW_MIN_H;
    }
    if (win->w > width) {
        win->w = width;
    }
    if (win->h > bottom) {
        win->h = bottom;
    }
    if (win->x + win->w > width) {
        win->x = width > win->w ? width - win->w : 0;
    }
    if (win->y < 28u) {
        win->y = 28u;
    }
    if (win->y + win->h > bottom) {
        win->y = bottom > win->h ? bottom - win->h : 28u;
    }

    desktop_update_window_content(win);
}

static void desktop_make_window(desktop_window_t *win) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t task_h = desktop_taskbar_height();
    uint32_t margin = width >= 700u ? 72u : 24u;
    uint32_t top = height >= 420u ? 58u : 34u;

    win->x = margin;
    win->y = top;
    win->w = width > margin * 2u ? width - margin * 2u : width;
    win->h = height > top + task_h + 24u ? height - top - task_h - 24u : height - top;

    if (win->w < 220u) {
        win->x = 8u;
        win->w = width > 16u ? width - 16u : width;
    }
    if (win->h < 120u) {
        win->y = 28u;
        win->h = height > task_h + 36u ? height - task_h - 36u : height;
    }

    desktop_clamp_window(win);
}

static void desktop_make_terminal_window(desktop_window_t *win) {
    desktop_make_window(win);
    win->w /= 2u;
    win->h /= 2u;
    desktop_clamp_window(win);
}

static void desktop_draw_window(const desktop_window_t *win, const char *title) {
    uint32_t close_x = win->x + win->w - 23u;
    uint32_t max_x = close_x - WINDOW_CONTROL_SIZE - WINDOW_CONTROL_GAP;
    uint32_t min_x = max_x - WINDOW_CONTROL_SIZE - WINDOW_CONTROL_GAP;

    graphics_fill_rect(win->x + 4u, win->y + 5u, win->w, win->h, 0x08060du);
    desktop_panel(win->x, win->y, win->w, win->h, 0x221a2du);
    graphics_fill_rect(win->x + 2u, win->y + 2u, win->w - 4u, 26u, 0x5c2d5bu);
    graphics_fill_rect(win->x + 2u, win->y + 24u, win->w - 4u, 4u, 0xe05f4fu);
    graphics_draw_rect(min_x, win->y + 7u, WINDOW_CONTROL_SIZE, WINDOW_CONTROL_SIZE, 0xf6eadbu);
    graphics_draw_rect(max_x, win->y + 7u, WINDOW_CONTROL_SIZE, WINDOW_CONTROL_SIZE, 0xf6eadbu);
    graphics_draw_rect(close_x, win->y + 7u, WINDOW_CONTROL_SIZE, WINDOW_CONTROL_SIZE, 0xf6eadbu);
    graphics_draw_line(min_x + 3u, win->y + 17u, min_x + 10u, win->y + 17u, 0xf6eadbu);
    graphics_draw_rect(max_x + 3u, win->y + 10u, 8u, 8u, 0xf6eadbu);
    graphics_draw_line(close_x + 3u, win->y + 10u, close_x + 10u, win->y + 17u, 0xf6eadbu);
    graphics_draw_line(close_x + 10u, win->y + 10u, close_x + 3u, win->y + 17u, 0xf6eadbu);
    graphics_draw_line(win->x + win->w - WINDOW_RESIZE_GRIP,
                       win->y + win->h - 4u,
                       win->x + win->w - 4u,
                       win->y + win->h - WINDOW_RESIZE_GRIP,
                       0xb98556u);
    graphics_draw_line(win->x + win->w - 11u,
                       win->y + win->h - 4u,
                       win->x + win->w - 4u,
                       win->y + win->h - 11u,
                       0xf6eadbu);
    console_draw_text_at_pixel(win->x + 10u, win->y + 10u, title, 0xf6eadbu, 0x5c2d5bu);
}

static void desktop_draw_button(uint32_t x, uint32_t y, uint32_t w, const char *label, int active) {
    desktop_theme_t theme = desktop_theme();
    uint32_t fill = active ? theme.button : theme.task_inactive;
    graphics_fill_rect(x, y, w, START_MENU_ITEM_H - 2u, fill);
    graphics_draw_rect(x, y, w, START_MENU_ITEM_H - 2u, active ? theme.text : theme.panel_inner);
    console_draw_text_at_pixel(x + 8u, y + 7u, label, theme.text, fill);
}

static void desktop_start_menu_rect(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t menu_h = 122u + desktop_app_catalog_count() * START_MENU_ITEM_H;
    uint32_t menu_y = graphics_height() > desktop_taskbar_height() + menu_h ?
                      graphics_height() - desktop_taskbar_height() - menu_h :
                      28u;

    if (x != 0) {
        *x = 12u;
    }
    if (y != 0) {
        *y = menu_y;
    }
    if (w != 0) {
        *w = START_MENU_W;
    }
    if (h != 0) {
        *h = menu_h;
    }
}

static void desktop_draw_start_menu(void) {
    uint32_t menu_x;
    uint32_t menu_y;
    uint32_t menu_w;
    uint32_t menu_h;
    desktop_theme_t theme = desktop_theme();

    if (!start_menu_open) {
        return;
    }

    desktop_start_menu_rect(&menu_x, &menu_y, &menu_w, &menu_h);

    desktop_panel(menu_x, menu_y, menu_w, menu_h, theme.panel);
    console_draw_text_at_pixel(menu_x + 12u, menu_y + 10u, "Start", theme.text, theme.panel);
    desktop_draw_button(menu_x + 12u, menu_y + 32u, menu_w - 24u, "Terminal", 0);
    desktop_draw_button(menu_x + 12u, menu_y + 56u, menu_w - 24u, "Files", 0);
    desktop_draw_button(menu_x + 12u, menu_y + 80u, menu_w - 24u, "Modules", 0);

    uint32_t module_count = desktop_app_catalog_count();
    for (uint32_t i = 0; i < module_count; ++i) {
        char name[DESKTOP_MODULE_NAME_SIZE];
        if (desktop_app_catalog_name_at(i, name, sizeof(name)) != 0) {
            text_copy_limited(name, sizeof(name), "module");
        }
        desktop_draw_button(menu_x + 22u, menu_y + 110u + i * START_MENU_ITEM_H, menu_w - 44u, name, 0);
    }
}

static desktop_app_t desktop_start_menu_hit(uint32_t x, uint32_t y, uint32_t *module_index) {
    uint32_t menu_x;
    uint32_t menu_y;
    uint32_t menu_w;
    uint32_t menu_h;

    desktop_start_menu_rect(&menu_x, &menu_y, &menu_w, &menu_h);
    if (!start_menu_open || !point_in_rect(x, y, menu_x, menu_y, menu_w, menu_h)) {
        return DESKTOP_APP_NONE;
    }
    if (point_in_rect(x, y, menu_x + 12u, menu_y + 32u, menu_w - 24u, START_MENU_ITEM_H)) {
        return DESKTOP_APP_TERMINAL;
    }
    if (point_in_rect(x, y, menu_x + 12u, menu_y + 56u, menu_w - 24u, START_MENU_ITEM_H)) {
        return DESKTOP_APP_BROWSER;
    }
    if (point_in_rect(x, y, menu_x + 12u, menu_y + 80u, menu_w - 24u, START_MENU_ITEM_H)) {
        if (module_index != 0) {
            *module_index = DESKTOP_MODULE_INDEX_NONE;
        }
        return DESKTOP_APP_MODULES;
    }

    uint32_t count = desktop_app_catalog_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (point_in_rect(x, y, menu_x + 22u, menu_y + 110u + i * START_MENU_ITEM_H, menu_w - 44u, START_MENU_ITEM_H)) {
            if (module_index != 0) {
                *module_index = i;
            }
            return DESKTOP_APP_MODULES;
        }
    }

    return DESKTOP_APP_NONE;
}

static int desktop_window_close_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    if (win->w < 25u) {
        return 0;
    }
    return point_in_rect(x, y, win->x + win->w - 23u, win->y + 7u, WINDOW_CONTROL_SIZE, WINDOW_CONTROL_SIZE);
}

static int desktop_window_maximize_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    uint32_t close_x;
    uint32_t max_x;

    if (win->w < 64u) {
        return 0;
    }
    close_x = win->x + win->w - 23u;
    max_x = close_x - WINDOW_CONTROL_SIZE - WINDOW_CONTROL_GAP;
    return point_in_rect(x, y, max_x, win->y + 7u, WINDOW_CONTROL_SIZE, WINDOW_CONTROL_SIZE);
}

static int desktop_window_minimize_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    uint32_t close_x;
    uint32_t max_x;
    uint32_t min_x;

    if (win->w < 83u) {
        return 0;
    }
    close_x = win->x + win->w - 23u;
    max_x = close_x - WINDOW_CONTROL_SIZE - WINDOW_CONTROL_GAP;
    min_x = max_x - WINDOW_CONTROL_SIZE - WINDOW_CONTROL_GAP;
    return point_in_rect(x, y, min_x, win->y + 7u, WINDOW_CONTROL_SIZE, WINDOW_CONTROL_SIZE);
}

static int desktop_window_title_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    if (desktop_window_close_hit(win, x, y) ||
        desktop_window_maximize_hit(win, x, y) ||
        desktop_window_minimize_hit(win, x, y)) {
        return 0;
    }
    return point_in_rect(x, y, win->x, win->y, win->w, WINDOW_TITLE_H);
}

static void desktop_window_toggle_maximize(desktop_window_t *win, desktop_window_state_t *state) {
    if (state->maximized) {
        *win = state->restore;
        state->maximized = 0;
        desktop_clamp_window(win);
        return;
    }

    state->restore = *win;
    state->maximized = 1;
    win->x = 0;
    win->y = 28u;
    win->w = graphics_width();
    win->h = graphics_height() > desktop_taskbar_height() + 32u ? graphics_height() - desktop_taskbar_height() - 32u : graphics_height();
    desktop_clamp_window(win);
}

static int desktop_window_resize_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    if (win->w < WINDOW_RESIZE_GRIP || win->h < WINDOW_RESIZE_GRIP) {
        return 0;
    }
    return point_in_rect(x,
                         y,
                         win->x + win->w - WINDOW_RESIZE_GRIP,
                         win->y + win->h - WINDOW_RESIZE_GRIP,
                         WINDOW_RESIZE_GRIP,
                         WINDOW_RESIZE_GRIP);
}

static void desktop_preview_restore(void) {
    if (!wm_preview_drawn) {
        return;
    }

    for (uint32_t i = 0; i < wm_preview_count; ++i) {
        graphics_put_pixel(wm_preview_x[i], wm_preview_y[i], wm_preview_color[i]);
    }
    wm_preview_count = 0;
    wm_preview_drawn = 0;
}

static void desktop_preview_begin(const desktop_window_t *win) {
    wm_preview_window = *win;
    wm_last_preview_window = *win;
    wm_preview_count = 0;
    wm_preview_drawn = 0;
    wm_preview_last_tick = 0;
}

static void desktop_preview_pixel(uint32_t x, uint32_t y) {
    if (x >= graphics_width() || y >= graphics_height()) {
        return;
    }
    if (wm_preview_count >= WINDOW_PREVIEW_MAX_PIXELS) {
        return;
    }

    wm_preview_x[wm_preview_count] = x;
    wm_preview_y[wm_preview_count] = y;
    wm_preview_color[wm_preview_count] = graphics_get_pixel(x, y);
    ++wm_preview_count;
    graphics_put_pixel(x, y, 0xf5fbf7u);
}

static void desktop_preview_draw(const desktop_window_t *win) {
    desktop_preview_restore();
    wm_preview_count = 0;

    for (uint32_t x = 0; x < win->w; ++x) {
        desktop_preview_pixel(win->x + x, win->y);
        desktop_preview_pixel(win->x + x, win->y + win->h - 1u);
    }
    for (uint32_t y = 1u; y + 1u < win->h; ++y) {
        desktop_preview_pixel(win->x, win->y + y);
        desktop_preview_pixel(win->x + win->w - 1u, win->y + y);
    }

    wm_preview_drawn = 1;
    wm_last_preview_window = *win;
    wm_preview_last_tick = timer_ticks();
}

static int desktop_window_same_geometry(const desktop_window_t *a, const desktop_window_t *b) {
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static int desktop_preview_due(const desktop_window_t *win, int force) {
    unsigned int hz;
    unsigned long long interval;
    unsigned long long now;

    if (!wm_preview_drawn || force || !desktop_window_same_geometry(win, &wm_last_preview_window)) {
        if (force || !wm_preview_drawn) {
            return 1;
        }
    } else {
        return 0;
    }

    hz = timer_frequency();
    if (hz == 0u) {
        hz = 100u;
    }
    interval = hz / DESKTOP_WM_PREVIEW_HZ;
    if (interval == 0ull) {
        interval = 1ull;
    }

    now = timer_ticks();
    return now - wm_preview_last_tick >= interval;
}

static int desktop_preview_update(const desktop_window_t *win, int force) {
    if (desktop_preview_due(win, force)) {
        desktop_preview_draw(win);
        return 1;
    }
    return 0;
}

static int text_equals(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint32_t desktop_text_len(const char *text) {
    uint32_t len = 0;

    while (text != 0 && text[len] != '\0') {
        ++len;
    }
    return len;
}

static const char *desktop_path_basename(const char *path) {
    const char *base = path;

    if (path == 0) {
        return "";
    }
    while (*path) {
        if (*path == '/' || *path == '\\') {
            base = path + 1;
        }
        ++path;
    }
    return base;
}

static int desktop_name_has_zo_suffix(const char *name) {
    uint32_t len = desktop_text_len(name);

    return len > 3u &&
           name[len - 3u] == '.' &&
           name[len - 2u] == 'z' &&
           name[len - 1u] == 'o';
}

static int desktop_text_prefix_equals(const char *a, const char *b, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int desktop_module_name_pair_matches(const char *loaded_name, const char *query) {
    uint32_t loaded_len;
    uint32_t query_len;

    if (loaded_name == 0 || query == 0 || *query == '\0') {
        return 0;
    }
    if (text_equals(loaded_name, query)) {
        return 1;
    }

    loaded_len = desktop_text_len(loaded_name);
    query_len = desktop_text_len(query);
    if (desktop_name_has_zo_suffix(loaded_name) &&
        loaded_len == query_len + 3u &&
        desktop_text_prefix_equals(loaded_name, query, query_len)) {
        return 1;
    }
    if (desktop_name_has_zo_suffix(query) &&
        query_len == loaded_len + 3u &&
        desktop_text_prefix_equals(loaded_name, query, loaded_len)) {
        return 1;
    }

    return 0;
}

static int desktop_module_name_matches(const char *loaded_name, const char *query) {
    return desktop_module_name_pair_matches(loaded_name, query) ||
           desktop_module_name_pair_matches(desktop_path_basename(loaded_name),
                                            desktop_path_basename(query));
}

static int desktop_find_module(const char *name) {
    uint32_t count = shell_module_count();

    for (uint32_t i = 0; i < count; ++i) {
        const char *module_name = shell_module_name(i);
        if (module_name != 0 &&
            shell_module_is_ui_app(i) &&
            desktop_module_name_matches(module_name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int desktop_mods_file_is_app(const char *name) {
    return desktop_name_has_zo_suffix(name) &&
           !text_equals(name, "zbrowser_html.zo");
}

static uint32_t desktop_app_catalog_count(void) {
    int count = shell_api_dir_count(DESKTOP_MODS_DIR);
    uint32_t app_count = 0;
    char name[DESKTOP_MODULE_NAME_SIZE];

    if (count <= 0) {
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        if (shell_api_dir_type(DESKTOP_MODS_DIR, (uint32_t)i) == LAINFS_ENTRY_TYPE_FILE &&
            shell_api_dir_name(DESKTOP_MODS_DIR, (uint32_t)i, name, sizeof(name)) == 0 &&
            desktop_mods_file_is_app(name)) {
            ++app_count;
        }
    }

    return app_count;
}

static int desktop_app_catalog_name_at(uint32_t app_index, char *out, uint32_t out_size) {
    int count = shell_api_dir_count(DESKTOP_MODS_DIR);
    uint32_t seen = 0;
    char name[DESKTOP_MODULE_NAME_SIZE];

    if (out == 0 || out_size == 0 || count <= 0) {
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        if (shell_api_dir_type(DESKTOP_MODS_DIR, (uint32_t)i) != LAINFS_ENTRY_TYPE_FILE ||
            shell_api_dir_name(DESKTOP_MODS_DIR, (uint32_t)i, name, sizeof(name)) != 0 ||
            !desktop_mods_file_is_app(name)) {
            continue;
        }
        if (seen == app_index) {
            text_copy_limited(out, out_size, name);
            return 0;
        }
        ++seen;
    }

    return -1;
}

static void desktop_mods_path_for_name(const char *name, char *out, uint32_t out_size) {
    uint32_t pos = 0;

    if (out == 0 || out_size == 0) {
        return;
    }
    out[0] = '\0';
    while (DESKTOP_MODS_DIR[pos] != '\0' && pos + 1u < out_size) {
        out[pos] = DESKTOP_MODS_DIR[pos];
        ++pos;
    }
    if (pos + 1u < out_size) {
        out[pos++] = '/';
    }
    for (uint32_t i = 0; name != 0 && name[i] != '\0' && pos + 1u < out_size; ++i) {
        out[pos++] = name[i];
    }
    out[pos] = '\0';
}

static int desktop_module_index_for_slot(desktop_module_window_t *slot) {
    const char *name;
    int index;

    if (slot == 0 || (!slot->open && !slot->minimized)) {
        return -1;
    }

    name = shell_module_name(slot->module_index);
    if (name != 0 && desktop_module_name_matches(name, slot->module_name)) {
        return (int)slot->module_index;
    }

    index = desktop_find_module(slot->module_name);
    if (index >= 0) {
        slot->module_index = (uint32_t)index;
    }
    return index;
}

#include "desktop_modules.inc.c"

static int text_starts_command(const char *line, const char *command) {
    uint32_t i = 0;

    while (line[i] == ' ' || line[i] == '\t') {
        ++i;
    }

    while (*command != '\0') {
        if (line[i] != *command) {
            return 0;
        }
        ++i;
        ++command;
    }

    return line[i] == '\0' || line[i] == ' ' || line[i] == '\t';
}

static uint32_t text_len(const char *s) {
    uint32_t len = 0;
    while (s != 0 && s[len] != '\0') {
        ++len;
    }
    return len;
}

static void text_copy_limited(char *dst, uint32_t dst_size, const char *src) {
    uint32_t i = 0;

    if (dst_size == 0u) {
        return;
    }
    while (src != 0 && src[i] != '\0' && i + 1u < dst_size) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static int text_ends_with(const char *text, const char *suffix) {
    uint32_t text_size = text_len(text);
    uint32_t suffix_size = text_len(suffix);

    if (suffix_size > text_size) {
        return 0;
    }

    return text_equals(text + text_size - suffix_size, suffix);
}

static const char *skip_spaces_const(const char *s) {
    while (s != 0 && (*s == ' ' || *s == '\t')) {
        ++s;
    }
    return s == 0 ? "" : s;
}

static void files_clear_selection(void);

static void files_go_root(void) {
    files_path[0] = '\\';
    files_path[1] = '\0';
    files_clear_selection();
}

static void files_go_up(void) {
    uint32_t len = text_len(files_path);

    if (len <= 1u) {
        files_go_root();
        return;
    }

    while (len > 1u && files_path[len - 1u] != '\\') {
        --len;
    }
    if (len <= 1u) {
        files_go_root();
    } else {
        files_path[len - 1u] = '\0';
    }
    files_clear_selection();
}

static void files_enter_dir(const char *name) {
    uint32_t len = text_len(files_path);
    uint32_t i = 0;

    if (len > 1u && len + 1u < sizeof(files_path)) {
        files_path[len++] = '\\';
    }
    while (name[i] != '\0' && len + 1u < sizeof(files_path)) {
        files_path[len++] = name[i++];
    }
    files_path[len] = '\0';
    files_clear_selection();
}

static void files_child_path(const char *name, char *out, uint32_t out_size) {
    uint32_t len = 0;
    uint32_t i = 0;

    if (out_size == 0u) {
        return;
    }
    while (files_path[len] != '\0' && len + 1u < out_size) {
        out[len] = files_path[len];
        ++len;
    }
    if (len > 1u && len + 1u < out_size) {
        out[len++] = '\\';
    }
    while (name[i] != '\0' && len + 1u < out_size) {
        out[len++] = name[i++];
    }
    out[len] = '\0';
}

static void files_clear_selection(void) {
    files_selected_name[0] = '\0';
    files_selected_kind = FILE_KIND_OTHER;
    files_selected_type = 0;
    files_context_open = 0;
}

static file_kind_t files_kind_for_name(const char *name) {
    if (text_ends_with(name, ".Z") || text_ends_with(name, ".z")) {
        return FILE_KIND_SOURCE;
    }
    if (text_ends_with(name, ".zbuild")) {
        return FILE_KIND_MANIFEST;
    }
    if (text_ends_with(name, ".zo")) {
        return FILE_KIND_OBJECT;
    }
    if (text_ends_with(name, ".bin")) {
        return FILE_KIND_BINARY;
    }
    if (text_ends_with(name, ".buildlog") || text_ends_with(name, ".testlog")) {
        return FILE_KIND_LOG;
    }
    if (text_ends_with(name, ".jpg") ||
        text_ends_with(name, ".jpeg") ||
        text_ends_with(name, ".JPG") ||
        text_ends_with(name, ".JPEG")) {
        return FILE_KIND_IMAGE;
    }
    return FILE_KIND_OTHER;
}

static const char *files_badge_for_kind(file_kind_t kind, uint32_t type) {
    if (type == LAINFS_ENTRY_TYPE_DIR) {
        return "DIR";
    }
    if (kind == FILE_KIND_SOURCE) {
        return "SRC";
    }
    if (kind == FILE_KIND_MANIFEST) {
        return "BUILD";
    }
    if (kind == FILE_KIND_OBJECT) {
        return "OBJ";
    }
    if (kind == FILE_KIND_BINARY) {
        return "BIN";
    }
    if (kind == FILE_KIND_LOG) {
        return "LOG";
    }
    if (kind == FILE_KIND_IMAGE) {
        return "IMG";
    }
    return "FILE";
}

static int files_target_from_name(const char *name, char *out, uint32_t out_size) {
    uint32_t len;

    text_copy_limited(out, out_size, name);
    len = text_len(out);
    if (text_ends_with(out, ".zbuild")) {
        out[len - 7u] = '\0';
    } else if (text_ends_with(out, ".Z")) {
        out[len - 2u] = '\0';
    } else if (text_ends_with(out, ".z")) {
        out[len - 2u] = '\0';
    } else if (text_ends_with(out, ".zo")) {
        out[len - 3u] = '\0';
    }
    return out[0] == '\0' ? -1 : 0;
}

static int files_action_hit(uint32_t x, uint32_t y, uint32_t index) {
    uint32_t button_x = files_window.content_x + 82u + index * (FILE_ACTION_BUTTON_W + 6u);
    uint32_t button_y = files_window.content_y + 28u;

    return point_in_rect(x, y, button_x, button_y, FILE_ACTION_BUTTON_W, START_MENU_ITEM_H - 2u);
}

static int files_context_delete_hit(uint32_t x, uint32_t y) {
    return files_context_open &&
           point_in_rect(x, y, files_context_x + 4u, files_context_y + 4u, 84u, START_MENU_ITEM_H - 2u);
}

static int files_delete_button_hit(uint32_t x, uint32_t y) {
    if (!files_open || files_window.content_w < 80u) {
        return 0;
    }
    return point_in_rect(x,
                         y,
                         files_window.content_x + files_window.content_w - 70u,
                         files_window.content_y + 28u,
                         FILE_ACTION_BUTTON_W,
                         START_MENU_ITEM_H - 2u);
}

static int desktop_editor_target_name(char *out, uint32_t out_size) {
    const char *base = editor_name;
    uint32_t len;

    if (out_size == 0u || editor_name[0] == '\0') {
        return -1;
    }

    for (uint32_t i = 0; editor_name[i] != '\0'; ++i) {
        if (editor_name[i] == '\\' || editor_name[i] == '/') {
            base = editor_name + i + 1u;
        }
    }

    text_copy_limited(out, out_size, base);
    len = text_len(out);
    if (text_ends_with(out, ".zbuild")) {
        out[len - 7u] = '\0';
    } else if (text_ends_with(out, ".Z")) {
        out[len - 2u] = '\0';
    } else if (text_ends_with(out, ".z")) {
        out[len - 2u] = '\0';
    }

    return out[0] == '\0' ? -1 : 0;
}

static int desktop_editor_project_dir(char *out, uint32_t out_size) {
    uint32_t last_sep = 0xffffffffu;
    uint32_t i;

    if (out_size == 0u) {
        return -1;
    }

    for (i = 0; editor_name[i] != '\0'; ++i) {
        if (editor_name[i] == '\\' || editor_name[i] == '/') {
            last_sep = i;
        }
    }

    if (last_sep == 0xffffffffu) {
        text_copy_limited(out, out_size, ".");
        return 0;
    }
    if (last_sep == 0u) {
        text_copy_limited(out, out_size, "\\");
        return 0;
    }
    if (last_sep + 1u > out_size) {
        return -1;
    }

    for (i = 0; i < last_sep && i + 1u < out_size; ++i) {
        out[i] = editor_name[i];
    }
    out[i] = '\0';
    return 0;
}

static int desktop_editor_enter_project_dir(void) {
    char dir[FILE_BROWSER_PATH_SIZE];

    if (desktop_editor_project_dir(dir, sizeof(dir)) != 0) {
        return -1;
    }

    return shell_api_chdir(dir);
}

static void desktop_editor_output_clear(void) {
    editor_output_len = 0;
    editor_output[0] = '\0';
}

static void desktop_editor_close(void) {
    editor_open = 0;
    editor_minimized = 0;
    editor_focused = 0;
    editor_window_state.maximized = 0;
    kfree(editor_buffer);
    editor_buffer = 0;
    editor_len = 0;
    editor_cursor = 0;
    editor_top_line = 0;
}

static void desktop_editor_output_append_char(char ch) {
    if (ch == '\b') {
        if (editor_output_len > 0u) {
            --editor_output_len;
            editor_output[editor_output_len] = '\0';
        }
        return;
    }

    if (editor_output_len + 1u >= sizeof(editor_output)) {
        uint32_t keep = sizeof(editor_output) / 2u;
        uint32_t start = editor_output_len > keep ? editor_output_len - keep : 0u;
        uint32_t i = 0;

        while (start + i < editor_output_len) {
            editor_output[i] = editor_output[start + i];
            ++i;
        }
        editor_output_len = i;
        editor_output[editor_output_len] = '\0';
    }

    editor_output[editor_output_len++] = ch;
    editor_output[editor_output_len] = '\0';
}

static void desktop_editor_output_append(const char *text) {
    uint32_t i = 0;

    while (text != 0 && text[i] != '\0') {
        desktop_editor_output_append_char(text[i++]);
    }
}

static void desktop_editor_output_capture(char ch) {
    desktop_editor_output_append_char(ch);
}

static void desktop_editor_output_begin(const char *action, const char *target) {
    desktop_editor_output_clear();
    desktop_editor_output_append(action);
    desktop_editor_output_append(" ");
    desktop_editor_output_append(target);
    desktop_editor_output_append("\n");
    console_cursor_enable(0);
    console_set_output_hook(desktop_editor_output_capture);
}

static void desktop_editor_output_end(void) {
    console_set_output_hook(0);
    console_cursor_enable(0);
    console_reset_region();
    terminal_console_active = 0;
}

static int desktop_editor_target_buildlog(char *out, uint32_t out_size) {
    char target[32];
    uint32_t len;

    if (desktop_editor_target_name(target, sizeof(target)) != 0 || out_size == 0u) {
        return -1;
    }

    text_copy_limited(out, out_size, target);
    len = text_len(out);
    if (len + 9u >= out_size) {
        out[0] = '\0';
        return -1;
    }
    out[len++] = '.';
    out[len++] = 'b';
    out[len++] = 'u';
    out[len++] = 'i';
    out[len++] = 'l';
    out[len++] = 'd';
    out[len++] = 'l';
    out[len++] = 'o';
    out[len++] = 'g';
    out[len] = '\0';
    return 0;
}

static void desktop_editor_load_buildlog(void) {
    char log_name[48];
    char log_buffer[512];

    if (desktop_editor_enter_project_dir() != 0 ||
        desktop_editor_target_buildlog(log_name, sizeof(log_name)) != 0) {
        editor_status = "bad log target";
        return;
    }

    if (shell_api_read_file(log_name, log_buffer, sizeof(log_buffer) - 1u) < 0) {
        editor_status = "no build log";
        return;
    }
    log_buffer[sizeof(log_buffer) - 1u] = '\0';

    desktop_editor_output_clear();
    desktop_editor_output_append(log_buffer);
    editor_status = "build log";
}

static void desktop_terminal_buffer_clear_row(uint32_t row) {
    if (row >= TERMINAL_BUFFER_ROWS) {
        return;
    }

    for (uint32_t col = 0; col < TERMINAL_BUFFER_COLS; ++col) {
        terminal_buffer[row][col] = ' ';
    }
}

static void desktop_terminal_buffer_clear(void) {
    for (uint32_t row = 0; row < TERMINAL_BUFFER_ROWS; ++row) {
        desktop_terminal_buffer_clear_row(row);
    }
    terminal_buffer_row = 0;
    terminal_buffer_col = 0;
}

static void desktop_terminal_buffer_scroll(void) {
    for (uint32_t row = 1; row < TERMINAL_BUFFER_ROWS; ++row) {
        for (uint32_t col = 0; col < TERMINAL_BUFFER_COLS; ++col) {
            terminal_buffer[row - 1u][col] = terminal_buffer[row][col];
        }
    }
    desktop_terminal_buffer_clear_row(TERMINAL_BUFFER_ROWS - 1u);
}

static void desktop_terminal_buffer_newline(void) {
    terminal_buffer_col = 0;
    if (terminal_buffer_row + 1u >= TERMINAL_BUFFER_ROWS) {
        desktop_terminal_buffer_scroll();
    } else {
        ++terminal_buffer_row;
    }
}

static void desktop_terminal_capture(char ch) {
    if (ch == '\n') {
        desktop_terminal_buffer_newline();
        return;
    }

    if (ch == '\b') {
        if (terminal_buffer_col > 0u) {
            --terminal_buffer_col;
            terminal_buffer[terminal_buffer_row][terminal_buffer_col] = ' ';
        }
        return;
    }

    if ((uint8_t)ch < 32u || (uint8_t)ch > 126u) {
        return;
    }

    if (terminal_buffer_col >= TERMINAL_BUFFER_COLS) {
        desktop_terminal_buffer_newline();
    }

    terminal_buffer[terminal_buffer_row][terminal_buffer_col++] = ch;
    if (terminal_buffer_col >= TERMINAL_BUFFER_COLS) {
        desktop_terminal_buffer_newline();
    }
}

static void desktop_launch_app(desktop_app_t app, const boot_info_t *info) {
    if (app == DESKTOP_APP_TERMINAL) {
        int was_open = terminal_open;
        int was_minimized = terminal_minimized;
        if (was_open || was_minimized) {
            desktop_damage_window(&terminal_window);
        }
        if (!was_open && !was_minimized) {
            desktop_terminal_open();
            desktop_damage_full();
        } else {
            terminal_open = 1;
            if (!terminal_bounds_ready) {
                desktop_make_terminal_window(&terminal_window);
                terminal_bounds_ready = 1;
                desktop_damage_full();
            }
        }
        terminal_minimized = 0;
        desktop_damage_window(&terminal_window);
        if (was_minimized) {
            desktop_damage_full();
        }
    } else if (app == DESKTOP_APP_BROWSER) {
        (void)info;
        if (desktop_open_module_app_by_name("filemgr_module.zo", 1) != 0) {
            if (files_open || files_minimized) {
                desktop_damage_window(&files_window);
            }
            files_open = 1;
            files_minimized = 0;
            if (!files_bounds_ready) {
                desktop_make_window(&files_window);
                files_window.x += 24u;
                files_window.y += 24u;
                desktop_clamp_window(&files_window);
                files_bounds_ready = 1;
                files_go_root();
            }
            desktop_damage_window(&files_window);
        }
    } else if (app == DESKTOP_APP_MODULES) {
        if (modules_open || modules_minimized) {
            desktop_damage_window(&modules_window);
        }
        modules_open = 1;
        modules_minimized = 0;
        if (!modules_bounds_ready) {
            desktop_make_window(&modules_window);
            modules_window.x += 48u;
            modules_window.y += 48u;
            modules_window.w = modules_window.w > 320u ? 320u : modules_window.w;
            modules_window.h = 220u;
            desktop_clamp_window(&modules_window);
            modules_bounds_ready = 1;
        }
        desktop_damage_window(&modules_window);
    }
    desktop_damage_taskbar();
}

static void desktop_close_module_app(desktop_module_window_t *slot) {
    char module_name[DESKTOP_MODULE_NAME_SIZE];
    int unload_on_close;

    if (slot == 0) {
        return;
    }

    text_copy_limited(module_name, sizeof(module_name), slot->module_name);
    unload_on_close = slot->unload_on_close;
    slot->open = 0;
    slot->minimized = 0;
    slot->state.maximized = 0;
    slot->unload_on_close = 0;
    slot->module_index = DESKTOP_MODULE_INDEX_NONE;
    slot->module_name[0] = '\0';

    if (unload_on_close && module_name[0] != '\0') {
        (void)shell_api_zunload(module_name);
    }
}

static void desktop_open_module_app(uint32_t index) {
    desktop_module_window_t *slot;
    const char *name = shell_module_name(index);

    if (name == 0 || !shell_module_is_ui_app(index)) {
        return;
    }

    slot = desktop_find_module_app_window(index);
    if (slot == 0) {
        slot = desktop_allocate_module_app_window();
    }
    if (slot == 0) {
        return;
    }

    desktop_damage_full();
    slot->open = 1;
    slot->minimized = 0;
    slot->module_index = index;
    text_copy_limited(slot->module_name, sizeof(slot->module_name), name);
    slot->last_tick = 0;
    if (!slot->bounds_ready) {
        uint32_t ordinal = (uint32_t)(slot - module_app_windows);
        desktop_make_window(&slot->window);
        slot->window.x += 72u + ordinal * 24u;
        slot->window.y += 72u + ordinal * 24u;
        slot->window.w = slot->window.w > 420u ? 420u : slot->window.w;
        slot->window.h = slot->window.h > 260u ? 260u : slot->window.h;
        desktop_clamp_window(&slot->window);
        slot->bounds_ready = 1;
    }
    desktop_damage_full();
}

static int desktop_open_module_app_by_name(const char *name, int load_from_mods) {
    int module_index;
    int loaded_by_desktop = 0;
    char module_path[DESKTOP_MODULE_NAME_SIZE];
    desktop_module_window_t *slot;

    if (name == 0 || name[0] == '\0') {
        return -1;
    }

    module_index = desktop_find_module(name);
    if (module_index < 0 && load_from_mods) {
        if (text_equals(name, "zbrowser_module.zo")) {
            text_copy_limited(module_path,
                              sizeof(module_path),
                              "/mods/zbrowser_html.zo /mods/zbrowser_module.zo");
        } else {
            desktop_mods_path_for_name(name, module_path, sizeof(module_path));
        }
        if (shell_api_zmod(module_path) != 0) {
            return -1;
        }
        loaded_by_desktop = 1;
        module_index = desktop_find_module(name);
        if (module_index < 0 || !shell_module_is_ui_app((uint32_t)module_index)) {
            (void)shell_api_zunload(module_path);
            return -1;
        }
    }
    if (module_index < 0) {
        return -1;
    }

    desktop_open_module_app((uint32_t)module_index);
    slot = desktop_find_module_app_window((uint32_t)module_index);
    if (slot != 0 && loaded_by_desktop) {
        slot->unload_on_close = 1;
    }
    return 0;
}

const char *desktop_api_image_path(void) {
    return desktop_image_path;
}

int desktop_api_open_image(const char *path) {
    const char *image_path = skip_spaces_const(path);

    if (*image_path == '\0') {
        return -1;
    }

    text_copy_limited(desktop_image_path, sizeof(desktop_image_path), image_path);
    return 0;
}

int desktop_api_open_module(const char *name) {
    const char *module_name = skip_spaces_const(name);

    if (*module_name == '\0') {
        return -1;
    }
    if (!desktop_alloc_state()) {
        return -3;
    }

    if (desktop_open_module_app_by_name(module_name, 1) != 0) {
        return -1;
    }

    desktop_deferred_redraw = 1;
    return 0;
}

static void desktop_open_editor(const char *name) {
    const char *path = skip_spaces_const(name);
    int status;

    if (*path == '\0') {
        console_puts("usage: edit name\n");
        return;
    }

    if (editor_open || editor_minimized) {
        desktop_damage_window(&editor_window);
    }
    if (editor_buffer == 0) {
        editor_buffer = (char *)kmalloc(DESKTOP_EDITOR_BUFFER_SIZE);
        if (editor_buffer == 0) {
            console_puts("desktop editor: out of heap\n");
            return;
        }
    }

    text_copy_limited(editor_name, sizeof(editor_name), path);
    editor_len = 0;
    editor_cursor = 0;
    editor_top_line = 0;
    editor_status = "loaded";
    desktop_editor_output_clear();
    editor_buffer[0] = '\0';

    status = shell_api_read_file(editor_name, editor_buffer, DESKTOP_EDITOR_BUFFER_SIZE);
    if (status < 0) {
        editor_buffer[0] = '\0';
        editor_status = "new file";
    }
    editor_len = text_len(editor_buffer);
    if (editor_len >= DESKTOP_EDITOR_BUFFER_SIZE) {
        editor_len = DESKTOP_EDITOR_BUFFER_SIZE - 1u;
        editor_buffer[editor_len] = '\0';
    }

    editor_open = 1;
    editor_minimized = 0;
    editor_focused = 1;
    if (!editor_bounds_ready) {
        desktop_make_window(&editor_window);
        editor_window.x += 96u;
        editor_window.y += 36u;
        editor_window.w = editor_window.w > 560u ? 560u : editor_window.w;
        editor_window.h = editor_window.h > 360u ? 360u : editor_window.h;
        desktop_clamp_window(&editor_window);
        editor_bounds_ready = 1;
    }
    desktop_damage_window(&editor_window);
    desktop_damage_taskbar();
}

int desktop_api_open_editor(const char *path) {
    if (path == 0 || *skip_spaces_const(path) == '\0') {
        return -1;
    }

    desktop_open_editor(path);
    desktop_deferred_redraw = 1;
    return 0;
}

static void desktop_restore_task_app(desktop_app_t app) {
    uint32_t i;

    if (app == DESKTOP_APP_TERMINAL) {
        desktop_damage_window(&terminal_window);
        terminal_open = 1;
        terminal_minimized = 0;
        desktop_damage_full();
    } else if (app == DESKTOP_APP_BROWSER) {
        desktop_damage_window(&files_window);
        files_open = 1;
        files_minimized = 0;
    } else if (app == DESKTOP_APP_MODULES) {
        desktop_damage_window(&modules_window);
        modules_open = 1;
        modules_minimized = 0;
    } else if (app == DESKTOP_APP_MODULE_APP) {
        desktop_damage_full();
        for (i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
            if (module_app_windows[i].minimized) {
                module_app_windows[i].open = 1;
                module_app_windows[i].minimized = 0;
            }
        }
    } else if (app == DESKTOP_APP_EDITOR) {
        desktop_damage_window(&editor_window);
        editor_open = 1;
        editor_minimized = 0;
        editor_focused = 1;
    }
    desktop_damage_taskbar();
}

static void desktop_terminal_focus(void) {
    if (!terminal_open) {
        return;
    }

    console_set_region_preserve(terminal_window.content_x,
                                terminal_window.content_y,
                                terminal_window.content_x + terminal_window.content_w,
                                terminal_window.content_y + terminal_window.content_h);
    terminal_console_active = 1;
    desktop_terminal_sync_console_cursor();
    console_cursor_enable(0);
    console_set_output_hook(desktop_terminal_capture);
    terminal_caret_visible = 1;
    terminal_caret_last_tick = timer_ticks();
    desktop_terminal_draw_caret();
}

static void desktop_terminal_blur(void) {
    desktop_terminal_erase_caret();
    console_cursor_enable(0);
    console_set_output_hook(0);
    console_reset_region();
    terminal_console_active = 0;
}

static void desktop_terminal_prompt(void) {
    terminal_len = 0;
    terminal_line[0] = '\0';
    shell_print_prompt();
}

static void desktop_terminal_open(void) {
    int was_open = terminal_open;

    terminal_open = 1;
    terminal_ready = 0;
    terminal_console_active = 1;
    terminal_caret_visible = 1;
    terminal_caret_last_tick = timer_ticks();
    if (!was_open) {
        desktop_terminal_buffer_clear();
    }
    if (!terminal_bounds_ready) {
        desktop_make_terminal_window(&terminal_window);
        terminal_bounds_ready = 1;
    }
}

static void desktop_terminal_sync_console_cursor(void) {
    uint32_t visible_rows = console_rows();
    uint32_t visible_cols = console_columns();
    uint32_t first_row = 0;
    uint32_t cursor_row;
    uint32_t cursor_col;

    if (visible_rows > TERMINAL_BUFFER_ROWS) {
        visible_rows = TERMINAL_BUFFER_ROWS;
    }
    if (visible_cols > TERMINAL_BUFFER_COLS) {
        visible_cols = TERMINAL_BUFFER_COLS;
    }

    if (terminal_buffer_row + 1u > visible_rows) {
        first_row = terminal_buffer_row + 1u - visible_rows;
    }

    cursor_row = terminal_buffer_row >= first_row ? terminal_buffer_row - first_row : 0;
    cursor_col = terminal_buffer_col < visible_cols ? terminal_buffer_col : (visible_cols > 0u ? visible_cols - 1u : 0u);
    if (visible_rows > 0u && cursor_row >= visible_rows) {
        cursor_row = visible_rows - 1u;
    }

    console_set_cursor(cursor_col, cursor_row);
    console_cursor_enable(0);
}

static void desktop_terminal_after_output(void) {
    if (!terminal_open) {
        return;
    }

    console_set_region_preserve(terminal_window.content_x,
                                terminal_window.content_y,
                                terminal_window.content_x + terminal_window.content_w,
                                terminal_window.content_y + terminal_window.content_h);
    terminal_console_active = 1;
    terminal_caret_visible = 1;
    terminal_caret_last_tick = timer_ticks();
    desktop_terminal_sync_console_cursor();
    console_cursor_enable(0);
    console_set_output_hook(desktop_terminal_capture);
    desktop_damage_full();
    desktop_redraw_all();
}

static void desktop_terminal_caret_rect(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t visible_rows = terminal_window.content_h / 10u;
    uint32_t visible_cols = terminal_window.content_w / 8u;
    uint32_t first_row = 0;
    uint32_t cursor_row;
    uint32_t cursor_col;

    if (visible_rows == 0u) {
        visible_rows = 1u;
    } else if (visible_rows > TERMINAL_BUFFER_ROWS) {
        visible_rows = TERMINAL_BUFFER_ROWS;
    }
    if (visible_cols == 0u) {
        visible_cols = 1u;
    } else if (visible_cols > TERMINAL_BUFFER_COLS) {
        visible_cols = TERMINAL_BUFFER_COLS;
    }
    if (terminal_buffer_row + 1u > visible_rows) {
        first_row = terminal_buffer_row + 1u - visible_rows;
    }

    cursor_row = terminal_buffer_row >= first_row ? terminal_buffer_row - first_row : 0;
    cursor_col = terminal_buffer_col < visible_cols ? terminal_buffer_col : (visible_cols > 0u ? visible_cols - 1u : 0u);
    if (visible_rows > 0u && cursor_row >= visible_rows) {
        cursor_row = visible_rows - 1u;
    }

    if (x != 0) {
        *x = terminal_window.content_x + cursor_col * 8u;
    }
    if (y != 0) {
        *y = terminal_window.content_y + cursor_row * 10u + 8u;
    }
    if (w != 0) {
        *w = TERMINAL_CARET_W;
    }
    if (h != 0) {
        *h = TERMINAL_CARET_H;
    }
}

static void desktop_terminal_draw_caret(void) {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;

    if (!terminal_open || !terminal_console_active || editor_focused || !terminal_caret_visible || terminal_caret_back_valid) {
        return;
    }

    desktop_terminal_caret_rect(&x, &y, &w, &h);
    if (w == TERMINAL_CARET_W && h == TERMINAL_CARET_H) {
        for (uint32_t cy = 0; cy < TERMINAL_CARET_H; ++cy) {
            for (uint32_t cx = 0; cx < TERMINAL_CARET_W; ++cx) {
                terminal_caret_back[cy * TERMINAL_CARET_W + cx] = graphics_get_pixel(x + cx, y + cy);
            }
        }
        terminal_caret_back_valid = 1;
    }
    graphics_fill_rect(x, y, w, h, 0xf6eadbu);
}

static void desktop_terminal_erase_caret(void) {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;

    if (!terminal_caret_back_valid) {
        return;
    }

    desktop_terminal_caret_rect(&x, &y, &w, &h);
    if (w == TERMINAL_CARET_W && h == TERMINAL_CARET_H) {
        for (uint32_t cy = 0; cy < TERMINAL_CARET_H; ++cy) {
            for (uint32_t cx = 0; cx < TERMINAL_CARET_W; ++cx) {
                graphics_put_pixel(x + cx, y + cy, terminal_caret_back[cy * TERMINAL_CARET_W + cx]);
            }
        }
    }
    terminal_caret_back_valid = 0;
}

static void desktop_terminal_update_caret_pixels(void) {
    if (terminal_caret_visible) {
        desktop_terminal_draw_caret();
    } else {
        desktop_terminal_erase_caret();
    }
}

static int desktop_terminal_caret_tick_due(void) {
    unsigned int hz;
    unsigned long long interval;
    unsigned long long now;

    if (!terminal_open || !terminal_console_active || editor_focused) {
        return 0;
    }

    hz = timer_frequency();
    if (hz == 0u) {
        hz = 100u;
    }
    interval = hz / TERMINAL_CARET_BLINK_HZ;
    if (interval == 0ull) {
        interval = 1ull;
    }

    now = timer_ticks();
    if (now - terminal_caret_last_tick < interval) {
        return 0;
    }
    terminal_caret_last_tick = now;
    terminal_caret_visible = !terminal_caret_visible;
    return 1;
}

static void desktop_terminal_draw(int fresh) {
    if (!terminal_open) {
        return;
    }

    if (!terminal_bounds_ready) {
        desktop_make_terminal_window(&terminal_window);
        terminal_bounds_ready = 1;
    } else {
        desktop_clamp_window(&terminal_window);
    }
    desktop_draw_window(&terminal_window, "Terminal");
    console_set_region_preserve(terminal_window.content_x,
                                terminal_window.content_y,
                                terminal_window.content_x + terminal_window.content_w,
                                terminal_window.content_y + terminal_window.content_h);
    console_cursor_enable(0);
    console_set_output_hook(0);

    if (fresh || !terminal_ready) {
        console_set_output_hook(0);
        console_clear();
        if (!terminal_ready) {
            console_set_output_hook(desktop_terminal_capture);
            console_puts("Desktop terminal. Commands run inside this real window.\n");
            console_puts("Drag the title bar. Resize from the bottom-right corner.\n");
            console_puts("Click Browse to open the file browser. Close box hides this window.\n\n");
            desktop_terminal_prompt();
            console_set_output_hook(0);
        }
        uint32_t visible_rows = console_rows();
        uint32_t visible_cols = console_columns();
        if (visible_rows > TERMINAL_BUFFER_ROWS) {
            visible_rows = TERMINAL_BUFFER_ROWS;
        }
        if (visible_cols > TERMINAL_BUFFER_COLS) {
            visible_cols = TERMINAL_BUFFER_COLS;
        }
        uint32_t first_row = 0;
        if (terminal_buffer_row + 1u > visible_rows) {
            first_row = terminal_buffer_row + 1u - visible_rows;
        }
        for (uint32_t row = 0; row < visible_rows; ++row) {
            uint32_t source_row = first_row + row;
            if (source_row >= TERMINAL_BUFFER_ROWS) {
                break;
            }
            for (uint32_t col = 0; col < visible_cols; ++col) {
                console_put_char_at(col, row, terminal_buffer[source_row][col]);
            }
        }
        desktop_terminal_sync_console_cursor();
        if (terminal_console_active && !editor_focused) {
            console_set_output_hook(desktop_terminal_capture);
        } else {
            console_set_output_hook(0);
            console_reset_region();
            terminal_console_active = 0;
        }
        terminal_ready = 1;
    } else if (terminal_console_active && !editor_focused) {
        console_cursor_enable(0);
        console_set_output_hook(desktop_terminal_capture);
    } else {
        console_cursor_enable(0);
        console_set_output_hook(0);
        console_reset_region();
        terminal_console_active = 0;
    }
    desktop_terminal_draw_caret();
}

static void desktop_redraw_after_geometry_change(void) {
    console_cursor_enable(0);
    console_set_output_hook(0);
    console_reset_region();
    terminal_console_active = 0;
    desktop_damage_full();
    desktop_redraw_all();
}

static void desktop_terminal_close(void) {
    if (!terminal_open) {
        return;
    }

    desktop_damage_full();
    console_cursor_enable(0);
    console_set_output_hook(0);
    console_reset_region();
    terminal_console_active = 0;
    terminal_open = 0;
    terminal_ready = 0;
    terminal_bounds_ready = 0;
    terminal_len = 0;
    terminal_line[0] = '\0';
    desktop_terminal_buffer_clear();
    desktop_redraw_all();
    terminal_console_active = 0;
}

static void desktop_draw_files(void) {
    if (!files_open) {
        return;
    }

    desktop_draw_window(&files_window, "Files");
    graphics_fill_rect(files_window.content_x, files_window.content_y, files_window.content_w, 22u, 0x151c22u);
    console_draw_text_at_pixel(files_window.content_x + 8u, files_window.content_y + 7u, files_path, 0xf5fbf7u, 0x151c22u);
    desktop_draw_button(files_window.content_x, files_window.content_y + 28u, 72u, "Up", 0);
    if (files_selected_name[0] != '\0') {
        console_draw_text_at_pixel(files_window.content_x + 82u, files_window.content_y + 7u, files_selected_name, 0xf7d154u, 0x151c22u);
        if (files_selected_kind == FILE_KIND_SOURCE ||
            files_selected_kind == FILE_KIND_MANIFEST ||
            files_selected_kind == FILE_KIND_LOG) {
            desktop_draw_button(files_window.content_x + 82u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "Open", 0);
        }
        if (files_selected_kind == FILE_KIND_IMAGE) {
            desktop_draw_button(files_window.content_x + 82u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "View", 0);
        }
        if (files_selected_kind == FILE_KIND_SOURCE || files_selected_kind == FILE_KIND_MANIFEST) {
            desktop_draw_button(files_window.content_x + 146u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "Build", 0);
            desktop_draw_button(files_window.content_x + 210u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "Inst", 0);
            desktop_draw_button(files_window.content_x + 274u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "Load", 0);
        } else if (files_selected_kind == FILE_KIND_OBJECT) {
            desktop_draw_button(files_window.content_x + 82u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "Load", 0);
        } else if (files_selected_kind == FILE_KIND_BINARY) {
            desktop_draw_button(files_window.content_x + 82u, files_window.content_y + 28u, FILE_ACTION_BUTTON_W, "Run", 0);
        }
        desktop_draw_button(files_window.content_x + files_window.content_w - 70u,
                            files_window.content_y + 28u,
                            FILE_ACTION_BUTTON_W,
                            "Del",
                            0);
    }

    int count = shell_api_dir_count(files_path);
    if (count < 0) {
        console_draw_text_at_pixel(files_window.content_x + 8u, files_window.content_y + 60u, "directory unavailable", 0xf5fbf7u, 0x222c34u);
        return;
    }
    if ((uint32_t)count > FILE_BROWSER_MAX_ITEMS) {
        count = FILE_BROWSER_MAX_ITEMS;
    }

    for (int i = 0; i < count; ++i) {
        char name[32];
        if (shell_api_dir_name(files_path, (uint32_t)i, name, sizeof(name)) != 0) {
            continue;
        }
        int type = shell_api_dir_type(files_path, (uint32_t)i);
        file_kind_t kind = files_kind_for_name(name);
        uint32_t row_y = files_window.content_y + 58u + (uint32_t)i * 22u;
        if (row_y + 20u >= files_window.y + files_window.h) {
            break;
        }
        graphics_fill_rect(files_window.content_x, row_y, files_window.content_w, 20u, i & 1 ? 0x26323au : 0x202a31u);
        if (text_equals(files_selected_name, name)) {
            graphics_draw_rect(files_window.content_x, row_y, files_window.content_w, 20u, 0xe05f4fu);
        }
        console_draw_text_at_pixel(files_window.content_x + 8u, row_y + 6u, files_badge_for_kind(kind, (uint32_t)type), 0xd9e4deu, i & 1 ? 0x26323au : 0x202a31u);
        console_draw_text_at_pixel(files_window.content_x + 64u, row_y + 6u, name, 0xf5fbf7u, i & 1 ? 0x26323au : 0x202a31u);
    }

    if (files_context_open && files_selected_name[0] != '\0') {
        uint32_t menu_x = files_context_x;
        uint32_t menu_y = files_context_y;

        if (menu_x + 92u > files_window.x + files_window.w) {
            menu_x = files_window.x + files_window.w > 96u ? files_window.x + files_window.w - 96u : files_window.x;
        }
        if (menu_y + START_MENU_ITEM_H > files_window.y + files_window.h) {
            menu_y = files_window.y + files_window.h > START_MENU_ITEM_H + 4u ?
                     files_window.y + files_window.h - START_MENU_ITEM_H - 4u :
                     files_window.y;
        }
        files_context_x = menu_x;
        files_context_y = menu_y;
        desktop_panel(menu_x, menu_y, 92u, START_MENU_ITEM_H + 8u, 0x221a2du);
        desktop_draw_button(menu_x + 4u, menu_y + 4u, 84u, "Delete", 0);
    }
}

static uint32_t editor_line_start(uint32_t line) {
    uint32_t pos = 0;
    uint32_t current = 0;

    while (editor_buffer[pos] != '\0' && current < line) {
        if (editor_buffer[pos] == '\n') {
            ++current;
        }
        ++pos;
    }
    return pos;
}

static uint32_t editor_cursor_line(void) {
    uint32_t line = 0;
    for (uint32_t i = 0; i < editor_cursor && i < editor_len; ++i) {
        if (editor_buffer[i] == '\n') {
            ++line;
        }
    }
    return line;
}

static uint32_t editor_line_count(void) {
    uint32_t lines = 1u;

    for (uint32_t i = 0; i < editor_len; ++i) {
        if (editor_buffer[i] == '\n') {
            ++lines;
        }
    }

    return lines;
}

static uint32_t editor_text_top(void) {
    return editor_window.content_y + DESKTOP_EDITOR_TOOLBAR_H + 10u;
}

static uint32_t editor_status_top(void) {
    uint32_t status_y = editor_window.content_y + editor_window.content_h - 18u;

    if (editor_window.content_h > DESKTOP_EDITOR_OUTPUT_H + 36u) {
        status_y -= DESKTOP_EDITOR_OUTPUT_H;
    }

    return status_y;
}

static uint32_t editor_text_cols(void) {
    uint32_t text_w = editor_window.content_w > 14u ? editor_window.content_w - 14u : editor_window.content_w;

    return text_w / 8u;
}

static uint32_t editor_visible_rows(void) {
    uint32_t top = editor_text_top();
    uint32_t bottom = editor_status_top();

    return bottom > top + 8u ? (bottom - top - 8u) / 10u : 0u;
}

static uint32_t editor_scroll_track_y(void) {
    return editor_text_top();
}

static uint32_t editor_scroll_track_h(void) {
    uint32_t top = editor_text_top();
    uint32_t bottom = editor_status_top();

    return bottom > top + 4u ? bottom - top - 4u : 1u;
}

static uint32_t editor_text_width(void) {
    return editor_window.content_w > 14u ? editor_window.content_w - 14u : editor_window.content_w;
}

static uint32_t editor_text_height(void) {
    uint32_t top = editor_text_top();
    uint32_t bottom = editor_status_top();

    return bottom > top ? bottom - top : 0u;
}

static uint32_t editor_scroll_thumb_h(uint32_t total, uint32_t rows, uint32_t track_h) {
    uint32_t thumb_h = total > rows && total != 0u ? (track_h * rows) / total : track_h;

    if (thumb_h < 12u) {
        thumb_h = 12u;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }
    return thumb_h;
}

static uint32_t editor_scroll_thumb_y(uint32_t total, uint32_t rows, uint32_t track_y, uint32_t track_h, uint32_t thumb_h) {
    if (total > rows) {
        return track_y + ((track_h - thumb_h) * editor_top_line) / (total - rows);
    }
    return track_y;
}

static int editor_scrollbar_hit(uint32_t x, uint32_t y) {
    return point_in_rect(x,
                         y,
                         editor_window.content_x + editor_window.content_w - 12u,
                         editor_scroll_track_y(),
                         10u,
                         editor_scroll_track_h());
}

static void editor_scroll_to_point(uint32_t y) {
    uint32_t total = editor_line_count();
    uint32_t rows = editor_visible_rows();
    uint32_t track_y = editor_scroll_track_y();
    uint32_t track_h = editor_scroll_track_h();
    uint32_t thumb_h = editor_scroll_thumb_h(total, rows, track_h);
    uint32_t range;
    uint32_t pos;

    if (rows == 0u || total <= rows) {
        editor_top_line = 0;
        return;
    }

    range = track_h > thumb_h ? track_h - thumb_h : 1u;
    if (y <= track_y) {
        pos = 0;
    } else if (y >= track_y + range) {
        pos = range;
    } else {
        pos = y - track_y;
    }
    editor_top_line = (pos * (total - rows)) / range;
}

static int editor_scroll_by_wheel(int wheel_delta) {
    uint32_t total = editor_line_count();
    uint32_t rows = editor_visible_rows();
    uint32_t step;
    uint32_t old_top = editor_top_line;

    if (wheel_delta == 0 || rows == 0u || total <= rows) {
        return 0;
    }

    step = (uint32_t)(wheel_delta < 0 ? -wheel_delta : wheel_delta) * 3u;
    if (step == 0u) {
        step = 1u;
    }

    if (wheel_delta > 0) {
        editor_top_line = editor_top_line > step ? editor_top_line - step : 0u;
    } else {
        uint32_t max_top = total - rows;
        editor_top_line = editor_top_line + step;
        if (editor_top_line > max_top) {
            editor_top_line = max_top;
        }
    }

    if (editor_cursor_line() < editor_top_line) {
        editor_cursor = editor_line_start(editor_top_line);
    } else if (editor_cursor_line() >= editor_top_line + rows) {
        editor_cursor = editor_line_start(editor_top_line + rows - 1u);
    }
    return editor_top_line != old_top;
}

static void editor_place_cursor_at(uint32_t x, uint32_t y) {
    uint32_t cols = editor_text_cols();
    uint32_t rows = editor_visible_rows();
    uint32_t text_x = editor_window.content_x + 6u;
    uint32_t text_y = editor_text_top();
    uint32_t row;
    uint32_t col;
    uint32_t line;
    uint32_t pos;

    if (cols == 0u || rows == 0u || y < text_y) {
        return;
    }

    row = (y - text_y) / 10u;
    if (row >= rows) {
        row = rows - 1u;
    }
    col = x <= text_x ? 0u : (x - text_x) / 8u;
    if (col >= cols) {
        col = cols - 1u;
    }

    line = editor_top_line + row;
    pos = editor_line_start(line);
    while (col > 0u && editor_buffer[pos] != '\0' && editor_buffer[pos] != '\n') {
        ++pos;
        --col;
    }
    editor_cursor = pos;
}

static int editor_text_area_hit(uint32_t x, uint32_t y) {
    uint32_t text_y = editor_text_top();
    uint32_t status_y = editor_status_top();

    return point_in_rect(x,
                         y,
                         editor_window.content_x,
                         text_y,
                         editor_text_width(),
                         status_y > text_y ? status_y - text_y : 0u);
}

static int z_keyword_at(const char *s, uint32_t pos, const char *kw) {
    uint32_t i = 0;
    char before = pos == 0u ? ' ' : s[pos - 1u];
    char after;

    if ((before >= 'A' && before <= 'Z') || (before >= 'a' && before <= 'z') ||
        (before >= '0' && before <= '9') || before == '_') {
        return 0;
    }

    while (kw[i] != '\0') {
        if (s[pos + i] != kw[i]) {
            return 0;
        }
        ++i;
    }
    after = s[pos + i];
    if ((after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z') ||
        (after >= '0' && after <= '9') || after == '_') {
        return 0;
    }
    return 1;
}

static uint32_t editor_color_at(uint32_t pos) {
    char ch = editor_buffer[pos];

    if (ch == '"' || ch == '\'') {
        return 0x8bdc9bu;
    }
    if (ch >= '0' && ch <= '9') {
        return 0xf7d154u;
    }
    if (ch == '/' && editor_buffer[pos + 1u] == '/') {
        return 0x7f8b91u;
    }
    if (ch == '#') {
        return 0xc099ffu;
    }
    if (z_keyword_at(editor_buffer, pos, "if") ||
        z_keyword_at(editor_buffer, pos, "else") ||
        z_keyword_at(editor_buffer, pos, "while") ||
        z_keyword_at(editor_buffer, pos, "for") ||
        z_keyword_at(editor_buffer, pos, "return") ||
        z_keyword_at(editor_buffer, pos, "global") ||
        z_keyword_at(editor_buffer, pos, "export") ||
        z_keyword_at(editor_buffer, pos, "extern") ||
        z_keyword_at(editor_buffer, pos, "uint32_t") ||
        z_keyword_at(editor_buffer, pos, "uint64_t") ||
        z_keyword_at(editor_buffer, pos, "void")) {
        return 0x65d6ffu;
    }
    return 0xf5fbf7u;
}

static void desktop_draw_editor_toolbar(void) {
    graphics_fill_rect(editor_window.content_x, editor_window.content_y, editor_window.content_w, DESKTOP_EDITOR_TOOLBAR_H + 4u, 0x1c1428u);
    desktop_draw_button(editor_window.content_x + 6u, editor_window.content_y + 3u, DESKTOP_EDITOR_BUTTON_W, "Save", 0);
    desktop_draw_button(editor_window.content_x + 62u, editor_window.content_y + 3u, DESKTOP_EDITOR_BUTTON_W, "Build", 0);
    desktop_draw_button(editor_window.content_x + 118u, editor_window.content_y + 3u, DESKTOP_EDITOR_BUTTON_W, "Inst", 0);
    desktop_draw_button(editor_window.content_x + 174u, editor_window.content_y + 3u, DESKTOP_EDITOR_BUTTON_W, "Load", 0);
    desktop_draw_button(editor_window.content_x + 230u, editor_window.content_y + 3u, DESKTOP_EDITOR_BUTTON_W, "Log", 0);
}

static void desktop_draw_editor_status(void) {
    uint32_t status_y = editor_window.content_y + editor_window.content_h - 18u;

    graphics_fill_rect(editor_window.content_x, status_y, editor_window.content_w, 18u, 0x2d2038u);
    console_draw_text_at_pixel(editor_window.content_x + 6u, status_y + 5u, editor_status ? editor_status : "", 0xe8dccdu, 0x2d2038u);
}

static void desktop_draw_editor_output(void) {
    uint32_t status_y = editor_window.content_y + editor_window.content_h - 18u;
    uint32_t output_y;
    uint32_t line_starts[5];
    uint32_t line_count = 0;
    uint32_t text_w = editor_text_width();

    if (editor_window.content_h <= DESKTOP_EDITOR_OUTPUT_H + 36u) {
        return;
    }

    output_y = status_y - DESKTOP_EDITOR_OUTPUT_H;
    graphics_fill_rect(editor_window.content_x, output_y, editor_window.content_w, DESKTOP_EDITOR_OUTPUT_H, 0x1a1224u);
    graphics_draw_rect(editor_window.content_x, output_y, editor_window.content_w, DESKTOP_EDITOR_OUTPUT_H, 0x6e5876u);
    console_draw_text_at_pixel(editor_window.content_x + 6u, output_y + 5u, "Output", 0xe05f4fu, 0x1a1224u);

    line_starts[line_count++] = 0;
    for (uint32_t i = 0; i < editor_output_len; ++i) {
        if (editor_output[i] == '\n' && i + 1u < editor_output_len) {
            if (line_count < sizeof(line_starts) / sizeof(line_starts[0])) {
                line_starts[line_count++] = i + 1u;
            } else {
                for (uint32_t j = 1; j < sizeof(line_starts) / sizeof(line_starts[0]); ++j) {
                    line_starts[j - 1u] = line_starts[j];
                }
                line_starts[line_count - 1u] = i + 1u;
            }
        }
    }

    for (uint32_t row = 0; row < line_count && row < 4u; ++row) {
        uint32_t p = line_starts[row];
        uint32_t col = 0;
        uint32_t y = output_y + 17u + row * 9u;

        while (editor_output[p] != '\0' && editor_output[p] != '\n' && col + 1u < text_w / 8u) {
            char ch[2];
            ch[0] = editor_output[p++];
            ch[1] = '\0';
            console_draw_text_at_pixel(editor_window.content_x + 6u + col * 8u, y, ch, 0xe8dccdu, 0x1a1224u);
            ++col;
        }
    }
}

static void desktop_draw_editor_text_area(void) {
    uint32_t cols;
    uint32_t rows;
    uint32_t pos;
    uint32_t cursor_line;
    uint32_t text_y;

    if (!editor_open) {
        return;
    }

    text_y = editor_text_top();
    cols = editor_text_cols();
    rows = editor_visible_rows();
    if (cols == 0u || rows == 0u) {
        return;
    }

    graphics_fill_rect(editor_window.content_x, text_y, editor_window.content_w, editor_text_height(), 0x120d1bu);

    cursor_line = editor_cursor_line();
    if (cursor_line < editor_top_line) {
        editor_top_line = cursor_line;
    }
    if (cursor_line >= editor_top_line + rows) {
        editor_top_line = cursor_line - rows + 1u;
    }

    pos = editor_line_start(editor_top_line);
    for (uint32_t row = 0; row < rows && pos <= editor_len; ++row) {
        uint32_t x = editor_window.content_x + 6u;
        uint32_t y = text_y + row * 10u;
        uint32_t col = 0;
        int comment = 0;

        while (editor_buffer[pos] != '\0' && editor_buffer[pos] != '\n' && col < cols - 1u) {
            uint32_t color = comment ? 0x7f8b91u : editor_color_at(pos);
            char text[2];
            if (editor_buffer[pos] == '/' && editor_buffer[pos + 1u] == '/') {
                comment = 1;
                color = 0x7f8b91u;
            }
            text[0] = editor_buffer[pos];
            text[1] = '\0';
            console_draw_text_at_pixel(x, y, text, color, 0x120d1bu);
            x += 8u;
            ++col;
            ++pos;
        }
        while (editor_buffer[pos] != '\0' && editor_buffer[pos] != '\n') {
            ++pos;
        }
        if (editor_buffer[pos] == '\n') {
            ++pos;
        }
    }

    {
        uint32_t line = editor_cursor_line();
        uint32_t line_start = editor_line_start(line);
        uint32_t col = editor_cursor > line_start ? editor_cursor - line_start : 0u;
        if (line >= editor_top_line && line < editor_top_line + rows && col < cols) {
            uint32_t cx = editor_window.content_x + 6u + col * 8u;
            uint32_t cy = text_y + (line - editor_top_line) * 10u;
            graphics_fill_rect(cx, cy + 8u, 8u, 2u, editor_focused ? 0xf5fbf7u : 0x7f8b91u);
        }
    }

    {
        uint32_t total = editor_line_count();
        uint32_t track_x = editor_window.content_x + editor_window.content_w - 9u;
        uint32_t track_y = editor_scroll_track_y();
        uint32_t track_h = editor_scroll_track_h();
        uint32_t thumb_h = editor_scroll_thumb_h(total, rows, track_h);
        uint32_t thumb_y = editor_scroll_thumb_y(total, rows, track_y, track_h, thumb_h);
        graphics_fill_rect(track_x, track_y, 5u, track_h, 0x2d2038u);
        graphics_fill_rect(track_x, thumb_y, 5u, thumb_h, 0xe05f4fu);
    }
}

static void desktop_draw_editor_content(void) {
    if (!editor_open) {
        return;
    }

    graphics_fill_rect(editor_window.content_x, editor_window.content_y, editor_window.content_w, editor_window.content_h, 0x120d1bu);
    desktop_draw_editor_toolbar();
    desktop_draw_editor_status();
    desktop_draw_editor_output();
    desktop_draw_editor_text_area();
}

static void desktop_draw_editor(void) {
    if (!editor_open) {
        return;
    }

    desktop_draw_window(&editor_window, editor_name);
    desktop_draw_editor_content();
}

static void desktop_damage_reset(void) {
    desktop_damage_count = 0;
}

static void desktop_damage_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t right = x + w;
    uint32_t bottom = y + h;
    uint32_t index;

    if (w == 0u || h == 0u || x >= width || y >= height) {
        return;
    }
    if (right < x || right > width) {
        right = width;
    }
    if (bottom < y || bottom > height) {
        bottom = height;
    }

    if (right <= x || bottom <= y) {
        return;
    }

    for (index = 0; index < desktop_damage_count; ++index) {
        uint32_t old_left = desktop_damage_x[index];
        uint32_t old_top = desktop_damage_y[index];
        uint32_t old_right = old_left + desktop_damage_w[index];
        uint32_t old_bottom = old_top + desktop_damage_h[index];

        if (x >= old_left && y >= old_top && right <= old_right && bottom <= old_bottom) {
            return;
        }
    }

    if (desktop_damage_count >= DESKTOP_DAMAGE_MAX_RECTS) {
        desktop_damage_reset();
        desktop_damage_x[0] = 0;
        desktop_damage_y[0] = 0;
        desktop_damage_w[0] = width;
        desktop_damage_h[0] = height;
        desktop_damage_count = 1;
        return;
    }

    desktop_damage_x[desktop_damage_count] = x;
    desktop_damage_y[desktop_damage_count] = y;
    desktop_damage_w[desktop_damage_count] = right - x;
    desktop_damage_h[desktop_damage_count] = bottom - y;
    ++desktop_damage_count;
}

static void desktop_damage_window(const desktop_window_t *win) {
    if (win == 0) {
        return;
    }
    desktop_damage_rect(win->x, win->y, win->w + 6u, win->h + 6u);
}

static void desktop_damage_taskbar(void) {
    uint32_t height = graphics_height();
    uint32_t task_h = desktop_taskbar_height();

    if (height < task_h) {
        desktop_damage_rect(0, 0, graphics_width(), height);
        return;
    }
    desktop_damage_rect(0, height - task_h, graphics_width(), task_h);
}

static void desktop_damage_module_windows(void) {
    for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
        if (module_app_windows[i].open) {
            desktop_damage_window(&module_app_windows[i].window);
        }
    }
}

static void desktop_damage_app(desktop_app_t app) {
    if (app == DESKTOP_APP_TERMINAL) {
        desktop_damage_window(&terminal_window);
    } else if (app == DESKTOP_APP_BROWSER) {
        desktop_damage_window(&files_window);
    } else if (app == DESKTOP_APP_MODULES) {
        desktop_damage_window(&modules_window);
    } else if (app == DESKTOP_APP_MODULE_APP) {
        desktop_damage_module_windows();
    } else if (app == DESKTOP_APP_EDITOR) {
        desktop_damage_window(&editor_window);
    }
    desktop_damage_taskbar();
}

static int desktop_damage_intersects_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t right;
    uint32_t bottom;

    if (w == 0u || h == 0u || desktop_damage_count == 0u) {
        return 1;
    }

    right = x + w;
    bottom = y + h;
    if (right < x) {
        right = 0xffffffffu;
    }
    if (bottom < y) {
        bottom = 0xffffffffu;
    }

    for (uint32_t i = 0; i < desktop_damage_count; ++i) {
        uint32_t damage_left = desktop_damage_x[i];
        uint32_t damage_top = desktop_damage_y[i];
        uint32_t damage_right = damage_left + desktop_damage_w[i];
        uint32_t damage_bottom = damage_top + desktop_damage_h[i];

        if (x < damage_right && right > damage_left &&
            y < damage_bottom && bottom > damage_top) {
            return 1;
        }
    }

    return 0;
}

static void desktop_damage_full(void) {
    desktop_damage_reset();
    desktop_damage_x[0] = 0;
    desktop_damage_y[0] = 0;
    desktop_damage_w[0] = graphics_width();
    desktop_damage_h[0] = graphics_height();
    desktop_damage_count = 1;
}

static void desktop_redraw_editor_only(uint32_t cursor_x_pos, uint32_t cursor_y_pos) {
    desktop_damage_full();
    desktop_redraw_all();
    cursor_draw_at(cursor_x_pos, cursor_y_pos);
}

static void desktop_redraw_clock_only(void) {
    desktop_draw_taskbar_clock();
    desktop_damage_reset();
}

static void desktop_redraw_all(void) {
    int buffered;

    desktop_begin_paint();
    buffered = desktop_first_redraw ? 0 : graphics_backbuffer_enable();
    desktop_first_redraw = 0;
    desktop_damage_full();
    terminal_caret_back_valid = 0;

    desktop_draw_base();
    if (terminal_open) {
        desktop_terminal_draw(1);
    }
    desktop_draw_files();
    desktop_draw_modules();
    desktop_draw_module_app();
    desktop_draw_editor();
    desktop_draw_start_menu();

    if (buffered) {
        graphics_backbuffer_flush();
        graphics_backbuffer_disable();
    }
    desktop_damage_reset();
}

static void desktop_terminal_submit(const boot_info_t *info) {
    console_puts("\n");
    terminal_line[terminal_len] = '\0';

    if (text_equals(terminal_line, "exit") || text_equals(terminal_line, "quit")) {
        desktop_terminal_close();
        return;
    }

    if (text_starts_command(terminal_line, "desktop")) {
        console_puts("desktop is already running here.\n");
        desktop_terminal_prompt();
        desktop_terminal_focus();
        return;
    }
    // if (text_starts_command(terminal_line, "browse")) {
    //     console_puts("Use the Files launcher or Start > Files in desktop mode.\n");
    //     desktop_terminal_prompt();
    //     desktop_terminal_focus();
    //     return;
    // }
    if (text_starts_command(terminal_line, "edit")) {
        desktop_open_editor(skip_spaces_const(terminal_line + 4u));
        console_puts("opened editor window\n");
        desktop_terminal_prompt();
        desktop_terminal_focus();
        return;
    }

    shell_run_command(terminal_line, info);
    if (terminal_open) {
        desktop_terminal_prompt();
        desktop_terminal_focus();
    }
}

static void desktop_terminal_key(const key_event_t *key, const boot_info_t *info) {
    char out[2];

    if (!terminal_open || key == 0) {
        return;
    }

    desktop_terminal_focus();
    desktop_terminal_erase_caret();
    if (key->type == KEY_ENTER) {
        desktop_terminal_submit(info);
        desktop_terminal_after_output();
        return;
    }

    if (key->type == KEY_BACKSPACE) {
        if (terminal_len > 0u) {
            --terminal_len;
            terminal_line[terminal_len] = '\0';
            console_puts("\b");
        }
        desktop_terminal_after_output();
        return;
    }

    if (key->type == KEY_TAB) {
        if (terminal_len + 1u < sizeof(terminal_line)) {
            terminal_line[terminal_len++] = ' ';
            terminal_line[terminal_len] = '\0';
            console_puts(" ");
        }
        desktop_terminal_after_output();
        return;
    }

    if (key->type != KEY_CHAR || terminal_len + 1u >= sizeof(terminal_line)) {
        terminal_caret_visible = 1;
        terminal_caret_last_tick = timer_ticks();
        desktop_terminal_sync_console_cursor();
        desktop_terminal_draw_caret();
        return;
    }

    terminal_line[terminal_len++] = key->ch;
    terminal_line[terminal_len] = '\0';
    out[0] = key->ch;
    out[1] = '\0';
    console_puts(out);
    desktop_terminal_after_output();
}

static void desktop_editor_save(void) {
    int status;

    editor_buffer[editor_len] = '\0';
    status = shell_api_write_file(editor_name, editor_buffer);
    editor_status = status == 0 ? "saved" : "save failed";
}

static void desktop_editor_build(void) {
    char target[32];

    desktop_editor_save();
    if (desktop_editor_target_name(target, sizeof(target)) != 0 ||
        desktop_editor_enter_project_dir() != 0) {
        editor_status = "bad build target";
        desktop_editor_output_clear();
        return;
    }

    desktop_editor_output_begin("Build", target);
    editor_status = shell_api_zbuild(target) == 0 ? "build ok" : "build failed";
    desktop_editor_output_end();
    if (text_equals(editor_status, "build ok")) {
        desktop_editor_load_buildlog();
        editor_status = "build ok";
    }
}

static void desktop_editor_install(void) {
    char target[32];

    desktop_editor_save();
    if (desktop_editor_target_name(target, sizeof(target)) != 0 ||
        desktop_editor_enter_project_dir() != 0) {
        editor_status = "bad install target";
        desktop_editor_output_clear();
        return;
    }

    desktop_editor_output_begin("Install", target);
    editor_status = shell_api_zinstall(target) == 0 ? "install ok" : "install failed";
    desktop_editor_output_end();
}

static void desktop_editor_load_module(void) {
    char target[32];

    desktop_editor_save();
    if (desktop_editor_target_name(target, sizeof(target)) != 0 ||
        desktop_editor_enter_project_dir() != 0) {
        editor_status = "bad module target";
        desktop_editor_output_clear();
        return;
    }

    desktop_editor_output_begin("Load", target);
    if (shell_api_zinstall(target) != 0) {
        editor_status = "install failed";
        desktop_editor_output_end();
        return;
    }
    editor_status = shell_api_zreload(target) == 0 ? "module reloaded" : "module load failed";
    desktop_editor_output_end();
}

static void desktop_files_build_selected(void) {
    char target[32];
    char path[FILE_BROWSER_PATH_SIZE];

    if (files_target_from_name(files_selected_name, target, sizeof(target)) != 0 ||
        shell_api_chdir(files_path) != 0) {
        return;
    }
    files_child_path(files_selected_name, path, sizeof(path));
    desktop_open_editor(path);
    desktop_editor_output_begin("Build", target);
    editor_status = shell_api_zbuild(target) == 0 ? "build ok" : "build failed";
    desktop_editor_output_end();
    if (text_equals(editor_status, "build ok")) {
        desktop_editor_load_buildlog();
        editor_status = "build ok";
    }
}

static void desktop_files_install_selected(void) {
    char target[32];
    char path[FILE_BROWSER_PATH_SIZE];

    if (files_target_from_name(files_selected_name, target, sizeof(target)) != 0 ||
        shell_api_chdir(files_path) != 0) {
        return;
    }
    files_child_path(files_selected_name, path, sizeof(path));
    desktop_open_editor(path);
    desktop_editor_output_begin("Install", target);
    editor_status = shell_api_zinstall(target) == 0 ? "install ok" : "install failed";
    desktop_editor_output_end();
}

static void desktop_files_load_selected(void) {
    char target[32];
    char path[FILE_BROWSER_PATH_SIZE];

    if (files_target_from_name(files_selected_name, target, sizeof(target)) != 0 ||
        shell_api_chdir(files_path) != 0) {
        return;
    }
    files_child_path(files_selected_name, path, sizeof(path));
    desktop_open_editor(path);
    desktop_editor_output_begin("Load", target);
    if (files_selected_kind == FILE_KIND_SOURCE || files_selected_kind == FILE_KIND_MANIFEST) {
        if (shell_api_zinstall(target) != 0) {
            editor_status = "install failed";
            desktop_editor_output_end();
            return;
        }
    }
    editor_status = shell_api_zreload(target) == 0 ? "module reloaded" : "module load failed";
    desktop_editor_output_end();
}

static void desktop_files_run_selected(const boot_info_t *info) {
    char command[64];
    char path[FILE_BROWSER_PATH_SIZE];

    if (files_selected_name[0] == '\0' || shell_api_chdir(files_path) != 0) {
        return;
    }

    text_copy_limited(command, sizeof(command), "exec ");
    text_copy_limited(command + 5u, sizeof(command) - 5u, files_selected_name);
    files_child_path(files_selected_name, path, sizeof(path));
    desktop_open_editor(path);
    desktop_editor_output_begin("Run", files_selected_name);
    shell_run_command(command, info);
    editor_status = "program run";
    desktop_editor_output_end();
}

static int desktop_files_view_selected(void) {
    char path[FILE_BROWSER_PATH_SIZE];

    if (files_selected_name[0] == '\0') {
        return -1;
    }

    files_child_path(files_selected_name, path, sizeof(path));
    return desktop_api_open_image(path);
}

static void desktop_files_delete_selected(void) {
    char path[FILE_BROWSER_PATH_SIZE];
    int status;

    if (files_selected_name[0] == '\0') {
        return;
    }

    files_child_path(files_selected_name, path, sizeof(path));
    status = shell_api_delete(path);
    files_context_open = 0;
    if (status == 0) {
        files_clear_selection();
        editor_status = "deleted";
    } else if (status == -9) {
        editor_status = "delete failed: directory not empty";
    } else {
        editor_status = "delete failed";
    }
}

static int desktop_editor_button_hit(uint32_t x, uint32_t y, uint32_t index) {
    uint32_t button_x = editor_window.content_x + 6u + index * (DESKTOP_EDITOR_BUTTON_W + 6u);
    uint32_t button_y = editor_window.content_y + 3u;

    return point_in_rect(x, y, button_x, button_y, DESKTOP_EDITOR_BUTTON_W, START_MENU_ITEM_H - 2u);
}

static void desktop_editor_insert_char(char ch) {
    if (editor_len + 1u >= DESKTOP_EDITOR_BUFFER_SIZE) {
        editor_status = "buffer full";
        return;
    }
    for (uint32_t i = editor_len + 1u; i > editor_cursor; --i) {
        editor_buffer[i] = editor_buffer[i - 1u];
    }
    editor_buffer[editor_cursor++] = ch;
    ++editor_len;
    editor_buffer[editor_len] = '\0';
    editor_status = "modified";
}

static void desktop_editor_backspace(void) {
    if (editor_cursor == 0u) {
        return;
    }
    for (uint32_t i = editor_cursor - 1u; i < editor_len; ++i) {
        editor_buffer[i] = editor_buffer[i + 1u];
    }
    --editor_cursor;
    --editor_len;
    editor_status = "modified";
}

static void desktop_editor_move_vertical(int delta) {
    uint32_t line = editor_cursor_line();
    uint32_t start = editor_line_start(line);
    uint32_t col = editor_cursor - start;
    uint32_t target_line;
    uint32_t target;

    if (delta < 0 && line == 0u) {
        return;
    }
    target_line = delta < 0 ? line - 1u : line + 1u;
    target = editor_line_start(target_line);
    if (target >= editor_len && delta > 0) {
        return;
    }
    while (col > 0u && editor_buffer[target] != '\0' && editor_buffer[target] != '\n') {
        ++target;
        --col;
    }
    editor_cursor = target;
}

static void desktop_editor_key(const key_event_t *key) {
    if (!editor_open || key == 0) {
        return;
    }

    if (key->type == KEY_CTRL_S) {
        desktop_editor_save();
    } else if (key->type == KEY_ESC || key->type == KEY_CTRL_Q) {
        desktop_damage_full();
        desktop_editor_close();
    } else if (key->type == KEY_LEFT) {
        if (editor_cursor > 0u) {
            --editor_cursor;
        }
    } else if (key->type == KEY_RIGHT) {
        if (editor_cursor < editor_len) {
            ++editor_cursor;
        }
    } else if (key->type == KEY_UP) {
        desktop_editor_move_vertical(-1);
    } else if (key->type == KEY_DOWN) {
        desktop_editor_move_vertical(1);
    } else if (key->type == KEY_BACKSPACE) {
        desktop_editor_backspace();
    } else if (key->type == KEY_ENTER) {
        desktop_editor_insert_char('\n');
    } else if (key->type == KEY_TAB) {
        desktop_editor_insert_char(' ');
        desktop_editor_insert_char(' ');
    } else if (key->type == KEY_CHAR) {
        desktop_editor_insert_char(key->ch);
    }
}

static int cursor_fill_pixel(int x, int y) {
    if (y >= 0 && y <= 12 && x >= 0 && x <= (y / 2)) {
        return 1;
    }
    if (y >= 11 && y <= 18 && x >= 4 && x <= 6) {
        return 1;
    }
    if (y >= 14 && y <= 18 && x >= 7 && x <= 9) {
        return 1;
    }
    return 0;
}

static int cursor_outline_pixel(int x, int y) {
    if (cursor_fill_pixel(x, y)) {
        return 0;
    }

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (cursor_fill_pixel(x + dx, y + dy)) {
                return 1;
            }
        }
    }

    return 0;
}

static void cursor_restore(void) {
    if (!cursor_drawn) {
        return;
    }

    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    for (uint32_t y = 0; y < CURSOR_H; ++y) {
        uint32_t py = cursor_y + y;
        if (py >= height) {
            continue;
        }
        for (uint32_t x = 0; x < CURSOR_W; ++x) {
            uint32_t px = cursor_x + x;
            if (px < width) {
                graphics_put_pixel(px, py, cursor_back[y * CURSOR_W + x]);
            }
        }
    }

    cursor_drawn = 0;
}

static void cursor_draw_at(uint32_t x, uint32_t y) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();

    cursor_restore();
    cursor_x = x;
    cursor_y = y;
    for (uint32_t cy = 0; cy < CURSOR_H; ++cy) {
        uint32_t py = y + cy;
        for (uint32_t cx = 0; cx < CURSOR_W; ++cx) {
            uint32_t px = x + cx;
            uint32_t i = cy * CURSOR_W + cx;
            cursor_back[i] = (px < width && py < height) ? graphics_get_pixel(px, py) : 0;
        }
    }

    for (uint32_t cy = 0; cy < CURSOR_H; ++cy) {
        uint32_t py = y + cy;
        if (py >= height) {
            continue;
        }
        for (uint32_t cx = 0; cx < CURSOR_W; ++cx) {
            uint32_t px = x + cx;
            if (px >= width) {
                continue;
            }
            if (cursor_fill_pixel((int)cx, (int)cy)) {
                graphics_put_pixel(px, py, 0xffffffu);
            } else if (cursor_outline_pixel((int)cx, (int)cy)) {
                graphics_put_pixel(px, py, 0x0b0d10u);
            }
        }
    }

    cursor_drawn = 1;
}

void desktop_run(const boot_info_t *info) {
    if (!desktop_alloc_state()) {
        console_puts("desktop: out of heap for window state\n");
        return;
    }

    if (!mouse_enabled()) {
        (void)mouse_init();
    }

    console_cursor_enable(0);
    desktop_terminal_open();
    desktop_redraw_all();
    cursor_drawn = 0;

    uint32_t last_x;
    uint32_t last_y;
    int last_buttons;
    int exit_requested = 0;

    desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
    desktop_mouse_last_activity_tick = timer_ticks();
    cursor_draw_at(last_x, last_y);

    for (;;) {
        key_event_t key;
        uint32_t x;
        uint32_t y;
        int buttons;
        int motion_dx = 0;
        int motion_dy = 0;
        int wheel_delta = 0;

        usb_poll();
        (void)kernel_task_poll();
        desktop_mouse_snapshot(&x, &y, &buttons);
        mouse_consume_motion(&motion_dx, &motion_dy, 0);
        wheel_delta = mouse_consume_wheel();
        int left_pressed = (buttons & MOUSE_LEFT) != 0;
        int left_was_pressed = (last_buttons & MOUSE_LEFT) != 0;
        int right_pressed = (buttons & MOUSE_RIGHT) != 0;
        int right_was_pressed = (last_buttons & MOUSE_RIGHT) != 0;

        while (keyboard_poll_key(&key)) {
            if (editor_open && editor_focused) {
                desktop_editor_key(&key);
                desktop_mouse_snapshot(&x, &y, &buttons);
                if (editor_open) {
                    desktop_redraw_editor_only(x, y);
                } else {
                    cursor_restore();
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                }
                continue;
            }
            if (module_focused != 0 && module_focused->open) {
                cursor_restore();
                if (desktop_module_app_send_key(module_focused, &key)) {
                    (void)desktop_tick_module_app(module_focused, 1);
                    cursor_draw_at(x, y);
                    continue;
                }
                cursor_draw_at(x, y);
            }
            if (key.type == KEY_CTRL_Q) {
                exit_requested = 1;
                break;
            }
            if (key.type == KEY_ESC) {
                continue;
            }
            cursor_restore();
            desktop_terminal_key(&key, info);
            desktop_mouse_snapshot(&x, &y, &buttons);
        }
        left_pressed = (buttons & MOUSE_LEFT) != 0;
        right_pressed = (buttons & MOUSE_RIGHT) != 0;
        if (wheel_delta != 0) {
            desktop_module_window_t *wheel_module;

            desktop_mouse_last_activity_tick = timer_ticks();
            if (editor_open &&
                point_in_rect(x,
                              y,
                              editor_window.content_x,
                              editor_window.content_y,
                              editor_window.content_w,
                              editor_window.content_h) &&
                editor_scroll_by_wheel(wheel_delta)) {
                cursor_restore();
                editor_focused = 1;
                desktop_terminal_blur();
                desktop_redraw_editor_only(x, y);
                last_x = x;
                last_y = y;
                last_buttons = buttons;
                continue;
            }
            wheel_module = desktop_top_module_app_window_at(x, y);
            if (wheel_module != 0) {
                cursor_restore();
                if (desktop_module_app_send_mouse(wheel_module, x, y, (uint32_t)buttons, wheel_delta)) {
                    desktop_focus_module_app(wheel_module);
                    editor_focused = 0;
                    desktop_terminal_blur();
                    cursor_draw_at(x, y);
                    last_x = x;
                    last_y = y;
                    last_buttons = buttons;
                    continue;
                }
                cursor_draw_at(x, y);
            }
        }
        if (left_pressed && (wm_action != DESKTOP_WM_IDLE || editor_scroll_drag)) {
            desktop_mouse_apply_drag_motion(&x, &y, last_x, last_y, motion_dx, motion_dy);
        }
        if (x != last_x || y != last_y || buttons != last_buttons) {
            desktop_mouse_last_activity_tick = timer_ticks();
        }

        if (exit_requested) {
            break;
        }

        if (!left_pressed && editor_scroll_drag) {
            editor_scroll_drag = 0;
        }

        if (!left_pressed && wm_action != DESKTOP_WM_IDLE) {
            cursor_restore();
            desktop_preview_restore();
            if (wm_target_window != 0) {
                if (!desktop_window_same_geometry(wm_target_window, &wm_preview_window)) {
                    *wm_target_window = wm_preview_window;
                    desktop_clamp_window(wm_target_window);
                    desktop_redraw_after_geometry_change();
                }
            }
            wm_action = DESKTOP_WM_IDLE;
            wm_target_window = 0;
            desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
            cursor_draw_at(last_x, last_y);
            continue;
        }

        if (right_pressed && !right_was_pressed && files_open &&
            point_in_rect(x, y, files_window.content_x, files_window.content_y + 58u, files_window.content_w, files_window.content_h)) {
            uint32_t row = (y - (files_window.content_y + 58u)) / 22u;
            char name[32];

            if (shell_api_dir_name(files_path, row, name, sizeof(name)) == 0) {
                int type = shell_api_dir_type(files_path, row);
                cursor_restore();
                files_context_open = 1;
                files_context_x = x;
                files_context_y = y;
                text_copy_limited(files_selected_name, sizeof(files_selected_name), name);
                files_selected_kind = files_kind_for_name(name);
                files_selected_type = (uint32_t)type;
                desktop_damage_window(&files_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }
        }

        if (right_pressed && !right_was_pressed) {
            desktop_module_window_t *module_slot = desktop_top_module_app_window_at(x, y);

            if (module_slot != 0 &&
                point_in_rect(x,
                              y,
                              module_slot->window.content_x,
                              module_slot->window.content_y,
                              module_slot->window.content_w,
                              module_slot->window.content_h)) {
                cursor_restore();
                desktop_terminal_blur();
                (void)desktop_tick_module_app(module_slot, 1);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }
        }

        if (left_pressed && editor_scroll_drag) {
            uint32_t old_top = editor_top_line;
            cursor_restore();
            editor_scroll_to_point(y);
            editor_focused = 1;
            desktop_terminal_blur();
            if (editor_top_line != old_top) {
                desktop_redraw_editor_only(x, y);
            } else {
                cursor_draw_at(x, y);
            }
            last_x = x;
            last_y = y;
            last_buttons = buttons;
            continue;
        }

        if (left_pressed && wm_action == DESKTOP_WM_DRAG && wm_target_window != 0) {
            int pointer_changed = x != last_x || y != last_y;
            wm_preview_window = *wm_target_window;
            wm_preview_window.x = (int)x - wm_drag_dx < 0 ? 0u : (uint32_t)((int)x - wm_drag_dx);
            wm_preview_window.y = (int)y - wm_drag_dy < 0 ? 0u : (uint32_t)((int)y - wm_drag_dy);
            desktop_clamp_window(&wm_preview_window);
            if (pointer_changed || desktop_preview_due(&wm_preview_window, 0) || !cursor_drawn) {
                cursor_restore();
                (void)desktop_preview_update(&wm_preview_window, 0);
                cursor_draw_at(x, y);
            }
            last_x = x;
            last_y = y;
            last_buttons = buttons;
            continue;
        }

        if (left_pressed && wm_action == DESKTOP_WM_RESIZE && wm_target_window != 0) {
            int pointer_changed = x != last_x || y != last_y;
            wm_preview_window = *wm_target_window;
            {
                int new_w = (int)x - (int)wm_preview_window.x + 1;
                int new_h = (int)y - (int)wm_preview_window.y + 1;
                wm_preview_window.w = new_w < (int)WINDOW_MIN_W ? WINDOW_MIN_W : (uint32_t)new_w;
                wm_preview_window.h = new_h < (int)WINDOW_MIN_H ? WINDOW_MIN_H : (uint32_t)new_h;
            }
            desktop_clamp_window(&wm_preview_window);
            if (pointer_changed || desktop_preview_due(&wm_preview_window, 0) || !cursor_drawn) {
                cursor_restore();
                (void)desktop_preview_update(&wm_preview_window, 0);
                cursor_draw_at(x, y);
            }
            last_x = x;
            last_y = y;
            last_buttons = buttons;
            continue;
        }

        if (left_pressed && !left_was_pressed) {
            desktop_app_t app;
            desktop_module_window_t *module_slot;

            if (files_context_open) {
                cursor_restore();
                if (files_context_delete_hit(x, y)) {
                    desktop_files_delete_selected();
                } else {
                    files_context_open = 0;
                }
                desktop_damage_window(&files_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_window_minimize_hit(&editor_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                editor_open = 0;
                editor_minimized = 1;
                editor_focused = 0;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_window_maximize_hit(&editor_window, x, y)) {
                cursor_restore();
                desktop_window_toggle_maximize(&editor_window, &editor_window_state);
                editor_focused = 1;
                desktop_terminal_blur();
                desktop_redraw_after_geometry_change();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_window_close_hit(&editor_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                desktop_editor_close();
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_window_resize_hit(&editor_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &editor_window;
                desktop_preview_begin(&editor_window);
                editor_focused = 1;
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_window_title_hit(&editor_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_DRAG;
                wm_target_window = &editor_window;
                wm_drag_dx = (int)x - (int)editor_window.x;
                wm_drag_dy = (int)y - (int)editor_window.y;
                desktop_preview_begin(&editor_window);
                editor_focused = 1;
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && editor_scrollbar_hit(x, y)) {
                cursor_restore();
                editor_scroll_drag = 1;
                editor_scroll_to_point(y);
                editor_focused = 1;
                desktop_blur_module_app();
                desktop_terminal_blur();
                desktop_redraw_editor_only(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_editor_button_hit(x, y, 0u)) {
                cursor_restore();
                desktop_editor_save();
                desktop_damage_window(&editor_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_editor_button_hit(x, y, 1u)) {
                cursor_restore();
                desktop_editor_build();
                desktop_damage_window(&editor_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_editor_button_hit(x, y, 2u)) {
                cursor_restore();
                desktop_editor_install();
                desktop_damage_window(&editor_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_editor_button_hit(x, y, 3u)) {
                cursor_restore();
                desktop_editor_load_module();
                desktop_damage_window(&editor_window);
                desktop_damage_module_windows();
                desktop_damage_taskbar();
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && desktop_editor_button_hit(x, y, 4u)) {
                cursor_restore();
                desktop_editor_load_buildlog();
                desktop_damage_window(&editor_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (editor_open && editor_text_area_hit(x, y)) {
                uint32_t old_cursor = editor_cursor;
                cursor_restore();
                editor_place_cursor_at(x, y);
                editor_focused = 1;
                desktop_blur_module_app();
                desktop_terminal_blur();
                if (editor_cursor != old_cursor) {
                    desktop_redraw_editor_only(x, y);
                } else {
                    cursor_draw_at(x, y);
                }
                last_buttons = buttons;
                continue;
            }

            if (editor_open && point_in_rect(x, y, editor_window.x, editor_window.y, editor_window.w, editor_window.h)) {
                cursor_restore();
                editor_focused = 1;
                desktop_blur_module_app();
                desktop_terminal_blur();
                desktop_damage_window(&editor_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            module_slot = desktop_top_module_app_window_at(x, y);
            if (module_slot != 0 && desktop_window_minimize_hit(&module_slot->window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                if (module_focused == module_slot) {
                    desktop_blur_module_app();
                }
                module_slot->open = 0;
                module_slot->minimized = 1;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (module_slot != 0 && desktop_window_maximize_hit(&module_slot->window, x, y)) {
                cursor_restore();
                desktop_window_toggle_maximize(&module_slot->window, &module_slot->state);
                desktop_terminal_blur();
                desktop_redraw_after_geometry_change();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (module_slot != 0 && desktop_window_close_hit(&module_slot->window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                if (module_focused == module_slot) {
                    desktop_blur_module_app();
                }
                desktop_close_module_app(module_slot);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (module_slot != 0 && desktop_window_resize_hit(&module_slot->window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &module_slot->window;
                desktop_preview_begin(&module_slot->window);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (module_slot != 0 && desktop_window_title_hit(&module_slot->window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_DRAG;
                wm_target_window = &module_slot->window;
                wm_drag_dx = (int)x - (int)module_slot->window.x;
                wm_drag_dy = (int)y - (int)module_slot->window.y;
                desktop_preview_begin(&module_slot->window);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (module_slot != 0) {
                cursor_restore();
                desktop_focus_module_app(module_slot);
                editor_focused = 0;
                desktop_terminal_blur();
                if (point_in_rect(x,
                                  y,
                                  module_slot->window.content_x,
                                  module_slot->window.content_y,
                                  module_slot->window.content_w,
                                  module_slot->window.content_h)) {
                    if (!desktop_module_app_send_mouse(module_slot, x, y, (uint32_t)buttons, 0)) {
                        (void)desktop_tick_module_app(module_slot, 1);
                    }
                }
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (modules_open && desktop_window_minimize_hit(&modules_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                modules_open = 0;
                modules_minimized = 1;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (modules_open && desktop_window_maximize_hit(&modules_window, x, y)) {
                cursor_restore();
                desktop_window_toggle_maximize(&modules_window, &modules_window_state);
                desktop_terminal_blur();
                desktop_redraw_after_geometry_change();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (modules_open && desktop_window_close_hit(&modules_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                modules_open = 0;
                modules_minimized = 0;
                modules_window_state.maximized = 0;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (modules_open && desktop_window_resize_hit(&modules_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &modules_window;
                desktop_preview_begin(&modules_window);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (modules_open && desktop_window_title_hit(&modules_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_DRAG;
                wm_target_window = &modules_window;
                wm_drag_dx = (int)x - (int)modules_window.x;
                wm_drag_dy = (int)y - (int)modules_window.y;
                desktop_preview_begin(&modules_window);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_minimize_hit(&files_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                files_open = 0;
                files_minimized = 1;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_maximize_hit(&files_window, x, y)) {
                cursor_restore();
                desktop_window_toggle_maximize(&files_window, &files_window_state);
                desktop_terminal_blur();
                desktop_redraw_after_geometry_change();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_close_hit(&files_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                files_open = 0;
                files_minimized = 0;
                files_window_state.maximized = 0;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_resize_hit(&files_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &files_window;
                desktop_preview_begin(&files_window);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_title_hit(&files_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_DRAG;
                wm_target_window = &files_window;
                wm_drag_dx = (int)x - (int)files_window.x;
                wm_drag_dy = (int)y - (int)files_window.y;
                desktop_preview_begin(&files_window);
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open && desktop_window_minimize_hit(&terminal_window, x, y)) {
                cursor_restore();
                desktop_damage_full();
                terminal_open = 0;
                terminal_minimized = 1;
                desktop_terminal_blur();
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open && desktop_window_maximize_hit(&terminal_window, x, y)) {
                cursor_restore();
                desktop_terminal_blur();
                desktop_window_toggle_maximize(&terminal_window, &terminal_window_state);
                desktop_redraw_after_geometry_change();
                desktop_terminal_focus();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open && desktop_window_close_hit(&terminal_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_IDLE;
                wm_target_window = 0;
                desktop_terminal_close();
                terminal_minimized = 0;
                terminal_window_state.maximized = 0;
                desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
                cursor_draw_at(last_x, last_y);
                continue;
            }

            if (terminal_open && desktop_window_resize_hit(&terminal_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &terminal_window;
                desktop_preview_begin(&terminal_window);
                desktop_terminal_focus();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open && desktop_window_title_hit(&terminal_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_DRAG;
                wm_target_window = &terminal_window;
                wm_drag_dx = (int)x - (int)terminal_window.x;
                wm_drag_dy = (int)y - (int)terminal_window.y;
                desktop_preview_begin(&terminal_window);
                desktop_terminal_focus();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            app = desktop_task_button_at(x, y);
            if (app != DESKTOP_APP_NONE) {
                cursor_restore();
                desktop_damage_app(app);
                desktop_restore_task_app(app);
                desktop_terminal_blur();
                desktop_redraw_all();
                if (app == DESKTOP_APP_TERMINAL) {
                    desktop_terminal_focus();
                }
                desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
                cursor_draw_at(last_x, last_y);
                continue;
            }

            app = launcher_at(x, y);
            if (app != DESKTOP_APP_NONE) {
                cursor_restore();
                desktop_launch_app(app, info);
                if (app != DESKTOP_APP_TERMINAL) {
                    desktop_terminal_blur();
                }
                desktop_redraw_all();
                if (app == DESKTOP_APP_TERMINAL) {
                    desktop_terminal_focus();
                }
                desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
                cursor_draw_at(last_x, last_y);
                continue;
            }

            if (point_in_rect(x, y, 12u, graphics_height() - desktop_taskbar_height() + 8u, 48u, desktop_taskbar_height() - 16u)) {
                cursor_restore();
                start_menu_open = !start_menu_open;
                desktop_damage_full();
                desktop_redraw_all();
                desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
                cursor_draw_at(last_x, last_y);
                continue;
            }

            if (start_menu_open) {
                uint32_t module_index = 0;
                desktop_app_t menu_app = desktop_start_menu_hit(x, y, &module_index);
                if (menu_app != DESKTOP_APP_NONE) {
                    cursor_restore();
                    start_menu_open = 0;
                    desktop_launch_app(menu_app, info);
                    if (menu_app == DESKTOP_APP_MODULES && module_index != DESKTOP_MODULE_INDEX_NONE) {
                        char module_name[DESKTOP_MODULE_NAME_SIZE];
                        if (desktop_app_catalog_name_at(module_index, module_name, sizeof(module_name)) == 0) {
                            (void)desktop_open_module_app_by_name(module_name, 1);
                        }
                    }
                    if (menu_app != DESKTOP_APP_TERMINAL) {
                        desktop_terminal_blur();
                    }
                    desktop_damage_full();
                    desktop_redraw_all();
                    if (menu_app == DESKTOP_APP_TERMINAL) {
                        desktop_terminal_focus();
                    }
                    desktop_mouse_snapshot(&last_x, &last_y, &last_buttons);
                    cursor_draw_at(last_x, last_y);
                    continue;
                }
                start_menu_open = 0;
                cursor_restore();
                desktop_damage_full();
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && point_in_rect(x, y, files_window.content_x, files_window.content_y + 28u, 72u, START_MENU_ITEM_H)) {
                cursor_restore();
                desktop_damage_window(&files_window);
                files_go_up();
                desktop_damage_window(&files_window);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && files_selected_name[0] != '\0') {
                if (files_delete_button_hit(x, y)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    desktop_files_delete_selected();
                    desktop_damage_window(&files_window);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if ((files_selected_kind == FILE_KIND_SOURCE ||
                     files_selected_kind == FILE_KIND_MANIFEST ||
                     files_selected_kind == FILE_KIND_LOG) &&
                    files_action_hit(x, y, 0u)) {
                    char path[FILE_BROWSER_PATH_SIZE];
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    files_child_path(files_selected_name, path, sizeof(path));
                    desktop_open_editor(path);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if ((files_selected_kind == FILE_KIND_SOURCE ||
                     files_selected_kind == FILE_KIND_MANIFEST) &&
                    files_action_hit(x, y, 1u)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    desktop_files_build_selected();
                    desktop_damage_window(&editor_window);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if ((files_selected_kind == FILE_KIND_SOURCE ||
                     files_selected_kind == FILE_KIND_MANIFEST) &&
                    files_action_hit(x, y, 2u)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    desktop_files_install_selected();
                    desktop_damage_window(&editor_window);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if ((files_selected_kind == FILE_KIND_SOURCE ||
                     files_selected_kind == FILE_KIND_MANIFEST) &&
                    files_action_hit(x, y, 3u)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    desktop_files_load_selected();
                    desktop_damage_window(&editor_window);
                    desktop_damage_module_windows();
                    desktop_damage_taskbar();
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if (files_selected_kind == FILE_KIND_OBJECT && files_action_hit(x, y, 0u)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    desktop_files_load_selected();
                    desktop_damage_module_windows();
                    desktop_damage_taskbar();
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if (files_selected_kind == FILE_KIND_BINARY && files_action_hit(x, y, 0u)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    desktop_files_run_selected(info);
                    desktop_damage_window(&editor_window);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
                if (files_selected_kind == FILE_KIND_IMAGE && files_action_hit(x, y, 0u)) {
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    (void)desktop_files_view_selected();
                    desktop_damage_window(&editor_window);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
            }

            if (files_open && point_in_rect(x, y, files_window.content_x, files_window.content_y + 58u, files_window.content_w, files_window.content_h)) {
                uint32_t row = (y - (files_window.content_y + 58u)) / 22u;
                char name[32];
                if (shell_api_dir_name(files_path, row, name, sizeof(name)) == 0) {
                    int type = shell_api_dir_type(files_path, row);
                    cursor_restore();
                    desktop_damage_window(&files_window);
                    if (type == LAINFS_ENTRY_TYPE_DIR) {
                        files_enter_dir(name);
                    } else {
                        text_copy_limited(files_selected_name, sizeof(files_selected_name), name);
                        files_selected_kind = files_kind_for_name(name);
                        files_selected_type = (uint32_t)type;
                        files_context_open = 0;
                        if (files_selected_kind == FILE_KIND_IMAGE) {
                            char path[FILE_BROWSER_PATH_SIZE];
                            files_child_path(name, path, sizeof(path));
                            (void)desktop_api_open_image(path);
                        } else if (files_selected_kind == FILE_KIND_SOURCE ||
                                   files_selected_kind == FILE_KIND_MANIFEST ||
                                   files_selected_kind == FILE_KIND_LOG ||
                                   files_selected_kind == FILE_KIND_OTHER) {
                            char path[FILE_BROWSER_PATH_SIZE];
                            files_child_path(name, path, sizeof(path));
                            desktop_open_editor(path);
                        }
                    }
                    desktop_damage_window(&files_window);
                    if (editor_open) {
                        desktop_damage_window(&editor_window);
                    }
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
            }

            if (modules_open && point_in_rect(x, y, modules_window.content_x, modules_window.content_y, modules_window.content_w, modules_window.content_h)) {
                uint32_t row = (y - (modules_window.content_y + 8u)) / 26u;
                char module_name[DESKTOP_MODULE_NAME_SIZE];
                cursor_restore();
                if (desktop_app_catalog_name_at(row, module_name, sizeof(module_name)) == 0) {
                    (void)desktop_open_module_app_by_name(module_name, 1);
                }
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open &&
                point_in_rect(x, y, terminal_window.x, terminal_window.y, terminal_window.w, terminal_window.h)) {
                cursor_restore();
                editor_focused = 0;
                desktop_blur_module_app();
                desktop_terminal_focus();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }
        }

        if (x != last_x || y != last_y) {
            cursor_restore();
            cursor_draw_at(x, y);
            last_x = x;
            last_y = y;
        }
        if (!cursor_drawn) {
            cursor_draw_at(x, y);
        }

        if (wm_action == DESKTOP_WM_IDLE &&
            !left_pressed &&
            desktop_taskbar_clock_tick_due()) {
            uint32_t clock_x;
            uint32_t clock_y;
            uint32_t clock_w;
            uint32_t clock_h;

            if (desktop_taskbar_clock_rect(&clock_x, &clock_y, &clock_w, &clock_h)) {
                int cursor_over_clock = desktop_cursor_intersects_rect(clock_x, clock_y, clock_w, clock_h);
                if (cursor_over_clock) {
                    cursor_restore();
                }
                desktop_redraw_clock_only();
                if (cursor_over_clock) {
                    cursor_draw_at(x, y);
                }
            }
        }

        if (wm_action == DESKTOP_WM_IDLE &&
            !left_pressed &&
            buttons == 0 &&
            desktop_mouse_background_redraw_ready()) {
            if (desktop_terminal_caret_tick_due()) {
                uint32_t caret_x;
                uint32_t caret_y;
                uint32_t caret_w;
                uint32_t caret_h;
                int cursor_over_caret;

                desktop_terminal_caret_rect(&caret_x, &caret_y, &caret_w, &caret_h);
                cursor_over_caret = desktop_cursor_intersects_rect(caret_x, caret_y, caret_w, caret_h);
                if (cursor_over_caret) {
                    cursor_restore();
                }
                desktop_terminal_update_caret_pixels();
                if (cursor_over_caret) {
                    cursor_draw_at(x, y);
                }
            }
            for (uint32_t i = 0; i < DESKTOP_MODULE_APP_WINDOWS_MAX; ++i) {
                desktop_module_window_t *slot = &module_app_windows[i];
                if (desktop_module_app_tick_due(slot, 0, timer_ticks())) {
                    cursor_restore();
                    (void)desktop_tick_module_app(slot, 0);
                    cursor_draw_at(x, y);
                    break;
                }
            }
        }

        last_buttons = buttons;

        cpu_pause();
    }

    cursor_restore();
    console_set_output_hook(0);
    console_cursor_enable(1);
    console_reset_region();
    terminal_console_active = 0;
    console_clear();
}
