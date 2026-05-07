#include <stdint.h>
#include "desktop.h"
#include "graphics.h"
#include "keyboard.h"
#include "kernel.h"
#include "lainfs.h"
#include "mouse.h"
#include "shell.h"

#define CURSOR_W 14u
#define CURSOR_H 20u
#define DESKTOP_LINE_MAX 128u
#define WINDOW_TITLE_H 30u
#define WINDOW_RESIZE_GRIP 18u
#define WINDOW_MIN_W 240u
#define WINDOW_MIN_H 140u
#define TERMINAL_BUFFER_ROWS 160u
#define TERMINAL_BUFFER_COLS 160u
#define WINDOW_PREVIEW_MAX_PIXELS 8192u
#define FILE_BROWSER_MAX_ITEMS 48u
#define FILE_BROWSER_PATH_SIZE 128u
#define START_MENU_W 220u
#define START_MENU_ITEM_H 24u

typedef enum {
    DESKTOP_APP_NONE = 0,
    DESKTOP_APP_TERMINAL,
    DESKTOP_APP_BROWSER,
    DESKTOP_APP_MODULES
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

static uint32_t cursor_back[CURSOR_W * CURSOR_H];
static uint32_t cursor_x;
static uint32_t cursor_y;
static int cursor_drawn;
static void cursor_restore(void);
static void desktop_terminal_draw(int fresh);
static desktop_window_t terminal_window;
static int terminal_open;
static int terminal_ready;
static int terminal_console_active;
static int terminal_bounds_ready;
static desktop_wm_action_t wm_action;
static desktop_window_t *wm_target_window;
static int wm_drag_dx;
static int wm_drag_dy;
static desktop_window_t wm_preview_window;
static int wm_preview_drawn;
static uint32_t wm_preview_count;
static uint32_t wm_preview_x[WINDOW_PREVIEW_MAX_PIXELS];
static uint32_t wm_preview_y[WINDOW_PREVIEW_MAX_PIXELS];
static uint32_t wm_preview_color[WINDOW_PREVIEW_MAX_PIXELS];
static char terminal_line[DESKTOP_LINE_MAX];
static uint32_t terminal_len;
static char terminal_buffer[TERMINAL_BUFFER_ROWS][TERMINAL_BUFFER_COLS];
static uint32_t terminal_buffer_row;
static uint32_t terminal_buffer_col;
static desktop_window_t files_window;
static int files_open;
static int files_bounds_ready;
static char files_path[FILE_BROWSER_PATH_SIZE];
static int start_menu_open;
static desktop_window_t modules_window;
static int modules_open;
static int modules_bounds_ready;
static const desktop_launcher_t launchers[] = {
    { 20u, 48u, 54u, 54u, DESKTOP_APP_TERMINAL, "Terminal" },
    { 20u, 124u, 54u, 54u, DESKTOP_APP_BROWSER, "Files" },
};

static inline void cpu_pause(void) {
    __asm__ __volatile__("pause");
}

static void desktop_terminal_draw(int fresh);
static void desktop_redraw_all(void);

static uint32_t mix_color(uint32_t a, uint32_t b, uint32_t step, uint32_t steps) {
    uint32_t ar = (a >> 16) & 0xffu;
    uint32_t ag = (a >> 8) & 0xffu;
    uint32_t ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu;
    uint32_t bg = (b >> 8) & 0xffu;
    uint32_t bb = b & 0xffu;

    if (steps == 0) {
        steps = 1;
    }

    uint32_t r = (ar * (steps - step) + br * step) / steps;
    uint32_t g = (ag * (steps - step) + bg * step) / steps;
    uint32_t blue = (ab * (steps - step) + bb * step) / steps;
    return (r << 16) | (g << 8) | blue;
}

static void desktop_background(void) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t color = mix_color(0x102238u, 0x2f5d68u, y, height);
        graphics_fill_rect(0, y, width, 1, color);
    }

    uint32_t band_y = height / 3u;
    graphics_fill_rect(0, band_y, width, height / 12u, 0x315c54u);
    graphics_fill_rect(0, band_y + height / 12u, width, 2, 0x6da38bu);
}

static void desktop_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill) {
    graphics_fill_rect(x, y, w, h, fill);
    graphics_draw_rect(x, y, w, h, 0xd9e4deu);
    if (w > 2u && h > 2u) {
        graphics_draw_rect(x + 1u, y + 1u, w - 2u, h - 2u, 0x26333au);
    }
}

static void desktop_icon(uint32_t x, uint32_t y, uint32_t color) {
    graphics_fill_rect(x, y, 34, 28, color);
    graphics_draw_rect(x, y, 34, 28, 0xf1f5f0u);
    graphics_fill_rect(x + 7u, y + 33u, 20, 4, 0xd8e3dcu);
}

static void desktop_draw_launcher(const desktop_launcher_t *launcher) {
    uint32_t icon_x = launcher->x + 10u;
    uint32_t icon_y = launcher->y;
    uint32_t color = launcher->app == DESKTOP_APP_TERMINAL ? 0x49736au : 0x7c8faau;

    desktop_icon(icon_x, icon_y, color);
    console_draw_text_at_pixel(launcher->x,
                               launcher->y + 42u,
                               launcher->title,
                               0xeaf2edu,
                               0x102238u);
}

static void desktop_draw_base(void) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t task_h = height >= 160u ? 34u : 24u;
    uint32_t top_h = height >= 160u ? 24u : 16u;
    unsigned int i;

    desktop_background();

    graphics_fill_rect(0, 0, width, top_h, 0x171f27u);
    graphics_fill_rect(0, top_h - 1u, width, 1, 0x76a7a5u);
    graphics_fill_rect(0, height - task_h, width, task_h, 0x1d252bu);
    graphics_fill_rect(0, height - task_h, width, 1, 0x8bb8a9u);
    console_draw_text_at_pixel(12u, 8u, "LainOS Desktop", 0xeaf2edu, 0x171f27u);

    if (width > 180u && height > 140u) {
        for (i = 0; i < sizeof(launchers) / sizeof(launchers[0]); ++i) {
            desktop_draw_launcher(&launchers[i]);
        }
    }

    if (width > 120u) {
        graphics_fill_rect(12u, height - task_h + 8u, 48u, task_h - 16u, 0x49736au);
        graphics_draw_rect(12u, height - task_h + 8u, 48u, task_h - 16u, 0xdce8e0u);
        graphics_fill_rect(76u, height - task_h + 10u, 90u, task_h - 20u, 0x2c3840u);
        graphics_draw_rect(76u, height - task_h + 10u, 90u, task_h - 20u, 0x52616bu);
        console_draw_text_at_pixel(22u, height - task_h + 13u, "Apps", 0xeaf2edu, 0x49736au);
    }
}

static int point_in_rect(uint32_t px, uint32_t py, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return px >= x && py >= y && px < x + w && py < y + h;
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

static void desktop_draw_window(const desktop_window_t *win, const char *title) {
    graphics_fill_rect(win->x + 4u, win->y + 5u, win->w, win->h, 0x10161bu);
    desktop_panel(win->x, win->y, win->w, win->h, 0x222c34u);
    graphics_fill_rect(win->x + 2u, win->y + 2u, win->w - 4u, 26u, 0x315762u);
    graphics_draw_rect(win->x + win->w - 25u, win->y + 7u, 16u, 14u, 0xd9e4deu);
    graphics_draw_line(win->x + win->w - WINDOW_RESIZE_GRIP,
                       win->y + win->h - 4u,
                       win->x + win->w - 4u,
                       win->y + win->h - WINDOW_RESIZE_GRIP,
                       0x8bb8a9u);
    graphics_draw_line(win->x + win->w - 11u,
                       win->y + win->h - 4u,
                       win->x + win->w - 4u,
                       win->y + win->h - 11u,
                       0xd9e4deu);
    console_draw_text_at_pixel(win->x + 10u, win->y + 10u, title, 0xf5fbf7u, 0x315762u);
}

static void desktop_draw_button(uint32_t x, uint32_t y, uint32_t w, const char *label, int active) {
    uint32_t fill = active ? 0x315762u : 0x2c3840u;
    graphics_fill_rect(x, y, w, START_MENU_ITEM_H - 2u, fill);
    graphics_draw_rect(x, y, w, START_MENU_ITEM_H - 2u, active ? 0xd9e4deu : 0x52616bu);
    console_draw_text_at_pixel(x + 8u, y + 7u, label, 0xf5fbf7u, fill);
}

static void desktop_draw_start_menu(void) {
    if (!start_menu_open) {
        return;
    }

    uint32_t height = 122u + shell_module_count() * START_MENU_ITEM_H;
    uint32_t y = graphics_height() > desktop_taskbar_height() + height ?
                 graphics_height() - desktop_taskbar_height() - height :
                 28u;

    desktop_panel(12u, y, START_MENU_W, height, 0x202a31u);
    console_draw_text_at_pixel(24u, y + 10u, "Start", 0xf5fbf7u, 0x202a31u);
    desktop_draw_button(24u, y + 32u, START_MENU_W - 24u, "Terminal", 0);
    desktop_draw_button(24u, y + 56u, START_MENU_W - 24u, "Files", 0);
    desktop_draw_button(24u, y + 80u, START_MENU_W - 24u, "Modules", 0);

    uint32_t module_count = shell_module_count();
    for (uint32_t i = 0; i < module_count; ++i) {
        const char *name = shell_module_name(i);
        desktop_draw_button(34u, y + 110u + i * START_MENU_ITEM_H, START_MENU_W - 44u, name ? name : "module", 0);
    }
}

static desktop_app_t desktop_start_menu_hit(uint32_t x, uint32_t y, uint32_t *module_index) {
    uint32_t height = 122u + shell_module_count() * START_MENU_ITEM_H;
    uint32_t menu_y = graphics_height() > desktop_taskbar_height() + height ?
                      graphics_height() - desktop_taskbar_height() - height :
                      28u;

    if (!start_menu_open || !point_in_rect(x, y, 12u, menu_y, START_MENU_W, height)) {
        return DESKTOP_APP_NONE;
    }
    if (point_in_rect(x, y, 24u, menu_y + 32u, START_MENU_W - 24u, START_MENU_ITEM_H)) {
        return DESKTOP_APP_TERMINAL;
    }
    if (point_in_rect(x, y, 24u, menu_y + 56u, START_MENU_W - 24u, START_MENU_ITEM_H)) {
        return DESKTOP_APP_BROWSER;
    }
    if (point_in_rect(x, y, 24u, menu_y + 80u, START_MENU_W - 24u, START_MENU_ITEM_H)) {
        if (module_index != 0) {
            *module_index = 0xffffffffu;
        }
        return DESKTOP_APP_MODULES;
    }

    uint32_t count = shell_module_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (point_in_rect(x, y, 34u, menu_y + 110u + i * START_MENU_ITEM_H, START_MENU_W - 44u, START_MENU_ITEM_H)) {
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
    return point_in_rect(x, y, win->x + win->w - 25u, win->y + 7u, 16u, 14u);
}

static int desktop_window_title_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    if (desktop_window_close_hit(win, x, y)) {
        return 0;
    }
    return point_in_rect(x, y, win->x, win->y, win->w, WINDOW_TITLE_H);
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
}

static int text_equals(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint32_t text_len(const char *s) {
    uint32_t len = 0;
    while (s != 0 && s[len] != '\0') {
        ++len;
    }
    return len;
}

static void files_go_root(void) {
    files_path[0] = '\\';
    files_path[1] = '\0';
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
        terminal_open = 1;
        terminal_ready = 0;
    } else if (app == DESKTOP_APP_BROWSER) {
        (void)info;
        files_open = 1;
        if (!files_bounds_ready) {
            desktop_make_window(&files_window);
            files_window.x += 24u;
            files_window.y += 24u;
            desktop_clamp_window(&files_window);
            files_bounds_ready = 1;
            files_go_root();
        }
    } else if (app == DESKTOP_APP_MODULES) {
        modules_open = 1;
        if (!modules_bounds_ready) {
            desktop_make_window(&modules_window);
            modules_window.x += 48u;
            modules_window.y += 48u;
            modules_window.w = modules_window.w > 320u ? 320u : modules_window.w;
            modules_window.h = 220u;
            desktop_clamp_window(&modules_window);
            modules_bounds_ready = 1;
        }
    }
}

static void desktop_terminal_focus(void) {
    if (!terminal_open) {
        return;
    }

    if (!terminal_console_active) {
        console_set_region(terminal_window.content_x,
                           terminal_window.content_y,
                           terminal_window.content_x + terminal_window.content_w,
                           terminal_window.content_y + terminal_window.content_h);
        terminal_console_active = 1;
    }
    console_cursor_enable(1);
    console_set_output_hook(desktop_terminal_capture);
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
    if (!was_open) {
        desktop_terminal_buffer_clear();
    }
    if (!terminal_bounds_ready) {
        desktop_make_window(&terminal_window);
        terminal_bounds_ready = 1;
    }
}

static void desktop_terminal_draw(int fresh) {
    if (!terminal_open) {
        return;
    }

    if (!terminal_bounds_ready) {
        desktop_make_window(&terminal_window);
        terminal_bounds_ready = 1;
    } else {
        desktop_clamp_window(&terminal_window);
    }
    desktop_draw_window(&terminal_window, "Terminal");
    desktop_terminal_focus();

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
        uint32_t cursor_row = terminal_buffer_row >= first_row ? terminal_buffer_row - first_row : 0;
        uint32_t cursor_col = terminal_buffer_col < visible_cols ? terminal_buffer_col : (visible_cols > 0u ? visible_cols - 1u : 0u);
        if (visible_rows > 0u && cursor_row >= visible_rows) {
            cursor_row = visible_rows - 1u;
        }
        console_set_cursor(cursor_col, cursor_row);
        console_set_output_hook(desktop_terminal_capture);
        terminal_ready = 1;
    }
}

static void desktop_redraw_after_geometry_change(void) {
    console_cursor_enable(0);
    console_set_output_hook(0);
    console_reset_region();
    terminal_console_active = 0;
    desktop_redraw_all();
}

static void desktop_terminal_close(void) {
    if (!terminal_open) {
        return;
    }

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
        uint32_t row_y = files_window.content_y + 58u + (uint32_t)i * 22u;
        if (row_y + 20u >= files_window.y + files_window.h) {
            break;
        }
        graphics_fill_rect(files_window.content_x, row_y, files_window.content_w, 20u, i & 1 ? 0x26323au : 0x202a31u);
        console_draw_text_at_pixel(files_window.content_x + 8u, row_y + 6u, type == LAINFS_ENTRY_TYPE_DIR ? "[dir]" : "[file]", 0xd9e4deu, i & 1 ? 0x26323au : 0x202a31u);
        console_draw_text_at_pixel(files_window.content_x + 64u, row_y + 6u, name, 0xf5fbf7u, i & 1 ? 0x26323au : 0x202a31u);
    }
}

static void desktop_draw_modules(void) {
    if (!modules_open) {
        return;
    }

    desktop_draw_window(&modules_window, "Modules");
    uint32_t count = shell_module_count();
    if (count == 0u) {
        console_draw_text_at_pixel(modules_window.content_x + 8u, modules_window.content_y + 8u, "No resident modules", 0xf5fbf7u, 0x222c34u);
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const char *name = shell_module_name(i);
        uint32_t row_y = modules_window.content_y + 8u + i * 26u;
        if (row_y + 24u >= modules_window.y + modules_window.h) {
            break;
        }
        desktop_draw_button(modules_window.content_x + 4u, row_y, modules_window.content_w - 8u, name ? name : "module", 0);
    }
}

static void desktop_redraw_all(void) {
    desktop_draw_base();
    if (terminal_open) {
        desktop_terminal_draw(1);
    }
    desktop_draw_files();
    desktop_draw_modules();
    desktop_draw_start_menu();
}

static void desktop_terminal_submit(const boot_info_t *info) {
    console_puts("\n");
    terminal_line[terminal_len] = '\0';

    if (text_equals(terminal_line, "exit") || text_equals(terminal_line, "quit")) {
        desktop_terminal_close();
        return;
    }

    shell_run_command(terminal_line, info);
    if (terminal_open) {
        desktop_terminal_prompt();
    }
}

static void desktop_terminal_key(const key_event_t *key, const boot_info_t *info) {
    char out[2];

    if (!terminal_open || key == 0) {
        return;
    }

    desktop_terminal_focus();
    if (key->type == KEY_ENTER) {
        desktop_terminal_submit(info);
        return;
    }

    if (key->type == KEY_BACKSPACE) {
        if (terminal_len > 0u) {
            --terminal_len;
            terminal_line[terminal_len] = '\0';
            console_puts("\b");
        }
        return;
    }

    if (key->type == KEY_TAB) {
        if (terminal_len + 1u < sizeof(terminal_line)) {
            terminal_line[terminal_len++] = ' ';
            terminal_line[terminal_len] = '\0';
            console_puts(" ");
        }
        return;
    }

    if (key->type != KEY_CHAR || terminal_len + 1u >= sizeof(terminal_line)) {
        return;
    }

    terminal_line[terminal_len++] = key->ch;
    terminal_line[terminal_len] = '\0';
    out[0] = key->ch;
    out[1] = '\0';
    console_puts(out);
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
    if (!mouse_enabled()) {
        (void)mouse_init();
    }

    console_cursor_enable(0);
    desktop_draw_base();
    desktop_terminal_open();
    desktop_terminal_draw(1);
    desktop_draw_start_menu();
    cursor_drawn = 0;

    uint32_t last_x = (uint32_t)mouse_x();
    uint32_t last_y = (uint32_t)mouse_y();
    int last_buttons = mouse_buttons();
    cursor_draw_at(last_x, last_y);

    for (;;) {
        key_event_t key;
        uint32_t x = (uint32_t)mouse_x();
        uint32_t y = (uint32_t)mouse_y();
        int buttons = mouse_buttons();
        int left_pressed = (buttons & MOUSE_LEFT) != 0;
        int left_was_pressed = (last_buttons & MOUSE_LEFT) != 0;

        while (keyboard_poll_key(&key)) {
            if (key.type == KEY_CTRL_Q) {
                buttons = MOUSE_LEFT | MOUSE_RIGHT;
                break;
            }
            if (key.type == KEY_ESC) {
                continue;
            }
            cursor_restore();
            desktop_terminal_key(&key, info);
            x = (uint32_t)mouse_x();
            y = (uint32_t)mouse_y();
        }

        if ((buttons & (MOUSE_LEFT | MOUSE_RIGHT)) == (MOUSE_LEFT | MOUSE_RIGHT)) {
            break;
        }

        if (!left_pressed && wm_action != DESKTOP_WM_IDLE) {
            cursor_restore();
            desktop_preview_restore();
            if (wm_target_window != 0) {
                *wm_target_window = wm_preview_window;
                desktop_clamp_window(wm_target_window);
                desktop_redraw_after_geometry_change();
            }
            wm_action = DESKTOP_WM_IDLE;
            wm_target_window = 0;
            last_x = (uint32_t)mouse_x();
            last_y = (uint32_t)mouse_y();
            last_buttons = buttons;
            cursor_draw_at(last_x, last_y);
            continue;
        }

        if (left_pressed && wm_action == DESKTOP_WM_DRAG && wm_target_window != 0) {
            cursor_restore();
            wm_preview_window = *wm_target_window;
            wm_preview_window.x = (int)x - wm_drag_dx < 0 ? 0u : (uint32_t)((int)x - wm_drag_dx);
            wm_preview_window.y = (int)y - wm_drag_dy < 0 ? 0u : (uint32_t)((int)y - wm_drag_dy);
            desktop_clamp_window(&wm_preview_window);
            desktop_preview_draw(&wm_preview_window);
            last_x = x;
            last_y = y;
            last_buttons = buttons;
            cursor_draw_at(last_x, last_y);
            continue;
        }

        if (left_pressed && wm_action == DESKTOP_WM_RESIZE && wm_target_window != 0) {
            cursor_restore();
            wm_preview_window = *wm_target_window;
            {
                int new_w = (int)x - (int)wm_preview_window.x + 1;
                int new_h = (int)y - (int)wm_preview_window.y + 1;
                wm_preview_window.w = new_w < (int)WINDOW_MIN_W ? WINDOW_MIN_W : (uint32_t)new_w;
                wm_preview_window.h = new_h < (int)WINDOW_MIN_H ? WINDOW_MIN_H : (uint32_t)new_h;
            }
            desktop_clamp_window(&wm_preview_window);
            desktop_preview_draw(&wm_preview_window);
            last_x = x;
            last_y = y;
            last_buttons = buttons;
            cursor_draw_at(last_x, last_y);
            continue;
        }

        if (left_pressed && !left_was_pressed) {
            desktop_app_t app;

            if (modules_open && desktop_window_close_hit(&modules_window, x, y)) {
                cursor_restore();
                modules_open = 0;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (modules_open && desktop_window_resize_hit(&modules_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &modules_window;
                wm_preview_window = modules_window;
                wm_preview_drawn = 0;
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
                wm_preview_window = modules_window;
                wm_preview_drawn = 0;
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_close_hit(&files_window, x, y)) {
                cursor_restore();
                files_open = 0;
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && desktop_window_resize_hit(&files_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &files_window;
                wm_preview_window = files_window;
                wm_preview_drawn = 0;
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
                wm_preview_window = files_window;
                wm_preview_drawn = 0;
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open && desktop_window_close_hit(&terminal_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_IDLE;
                wm_target_window = 0;
                desktop_terminal_close();
                last_x = (uint32_t)mouse_x();
                last_y = (uint32_t)mouse_y();
                last_buttons = mouse_buttons();
                cursor_draw_at(last_x, last_y);
                continue;
            }

            if (terminal_open && desktop_window_resize_hit(&terminal_window, x, y)) {
                cursor_restore();
                wm_action = DESKTOP_WM_RESIZE;
                wm_target_window = &terminal_window;
                wm_preview_window = terminal_window;
                wm_preview_drawn = 0;
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
                wm_preview_window = terminal_window;
                wm_preview_drawn = 0;
                desktop_terminal_focus();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            app = launcher_at(x, y);
            if (app != DESKTOP_APP_NONE) {
                cursor_restore();
                desktop_launch_app(app, info);
                terminal_console_active = 0;
                desktop_redraw_all();
                last_x = (uint32_t)mouse_x();
                last_y = (uint32_t)mouse_y();
                last_buttons = mouse_buttons();
                cursor_draw_at(last_x, last_y);
                continue;
            }

            if (point_in_rect(x, y, 12u, graphics_height() - desktop_taskbar_height() + 8u, 48u, desktop_taskbar_height() - 16u)) {
                cursor_restore();
                start_menu_open = !start_menu_open;
                desktop_redraw_all();
                last_x = (uint32_t)mouse_x();
                last_y = (uint32_t)mouse_y();
                last_buttons = mouse_buttons();
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
                    if (menu_app == DESKTOP_APP_MODULES && module_index != 0xffffffffu) {
                        (void)shell_module_tick(module_index);
                    }
                    terminal_console_active = 0;
                    desktop_redraw_all();
                    last_x = (uint32_t)mouse_x();
                    last_y = (uint32_t)mouse_y();
                    last_buttons = mouse_buttons();
                    cursor_draw_at(last_x, last_y);
                    continue;
                }
                start_menu_open = 0;
                cursor_restore();
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && point_in_rect(x, y, files_window.content_x, files_window.content_y + 28u, 72u, START_MENU_ITEM_H)) {
                cursor_restore();
                files_go_up();
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (files_open && point_in_rect(x, y, files_window.content_x, files_window.content_y + 58u, files_window.content_w, files_window.content_h)) {
                uint32_t row = (y - (files_window.content_y + 58u)) / 22u;
                char name[32];
                if (shell_api_dir_name(files_path, row, name, sizeof(name)) == 0 &&
                    shell_api_dir_type(files_path, row) == LAINFS_ENTRY_TYPE_DIR) {
                    cursor_restore();
                    files_enter_dir(name);
                    desktop_redraw_all();
                    cursor_draw_at(x, y);
                    last_buttons = buttons;
                    continue;
                }
            }

            if (modules_open && point_in_rect(x, y, modules_window.content_x, modules_window.content_y, modules_window.content_w, modules_window.content_h)) {
                uint32_t index = (y - (modules_window.content_y + 8u)) / 26u;
                cursor_restore();
                (void)shell_module_tick(index);
                desktop_redraw_all();
                cursor_draw_at(x, y);
                last_buttons = buttons;
                continue;
            }

            if (terminal_open &&
                point_in_rect(x, y, terminal_window.x, terminal_window.y, terminal_window.w, terminal_window.h)) {
                cursor_restore();
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
        last_buttons = buttons;

        cpu_pause();
    }

    cursor_restore();
    console_cursor_enable(1);
    console_reset_region();
    terminal_console_active = 0;
    console_clear();
}
