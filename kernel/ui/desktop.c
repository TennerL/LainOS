#include <stdint.h>
#include "desktop.h"
#include "graphics.h"
#include "keyboard.h"
#include "kernel.h"
#include "mouse.h"
#include "shell.h"

#define CURSOR_W 14u
#define CURSOR_H 20u
#define DESKTOP_LINE_MAX 128u

typedef enum {
    DESKTOP_APP_NONE = 0,
    DESKTOP_APP_TERMINAL,
    DESKTOP_APP_BROWSER
} desktop_app_t;

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
static char terminal_line[DESKTOP_LINE_MAX];
static uint32_t terminal_len;
static const desktop_launcher_t launchers[] = {
    { 20u, 48u, 54u, 54u, DESKTOP_APP_TERMINAL, "Terminal" },
    { 20u, 124u, 54u, 54u, DESKTOP_APP_BROWSER, "Browse" },
};

static inline void cpu_pause(void) {
    __asm__ __volatile__("pause");
}

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

static desktop_app_t launcher_at(uint32_t x, uint32_t y) {
    for (unsigned int i = 0; i < sizeof(launchers) / sizeof(launchers[0]); ++i) {
        const desktop_launcher_t *launcher = &launchers[i];
        if (point_in_rect(x, y, launcher->x, launcher->y, launcher->w, launcher->h)) {
            return launcher->app;
        }
    }

    return DESKTOP_APP_NONE;
}

static void desktop_make_window(desktop_window_t *win) {
    uint32_t width = graphics_width();
    uint32_t height = graphics_height();
    uint32_t task_h = height >= 160u ? 34u : 24u;
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

    win->content_x = win->x + 10u;
    win->content_y = win->y + 34u;
    win->content_w = win->w > 20u ? win->w - 20u : win->w;
    win->content_h = win->h > 44u ? win->h - 44u : win->h;
}

static void desktop_draw_window(const desktop_window_t *win, const char *title) {
    graphics_fill_rect(win->x + 4u, win->y + 5u, win->w, win->h, 0x10161bu);
    desktop_panel(win->x, win->y, win->w, win->h, 0x222c34u);
    graphics_fill_rect(win->x + 2u, win->y + 2u, win->w - 4u, 26u, 0x315762u);
    graphics_draw_rect(win->x + win->w - 25u, win->y + 7u, 16u, 14u, 0xd9e4deu);
    console_draw_text_at_pixel(win->x + 10u, win->y + 10u, title, 0xf5fbf7u, 0x315762u);
}

static int desktop_window_close_hit(const desktop_window_t *win, uint32_t x, uint32_t y) {
    if (win->w < 25u) {
        return 0;
    }
    return point_in_rect(x, y, win->x + win->w - 25u, win->y + 7u, 16u, 14u);
}

static int text_equals(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void desktop_host_console_begin(desktop_window_t *win, const char *title) {
    cursor_restore();
    terminal_console_active = 0;
    desktop_draw_base();
    desktop_make_window(win);
    desktop_draw_window(win, title);
    console_set_region(win->content_x,
                       win->content_y,
                       win->content_x + win->content_w,
                       win->content_y + win->content_h);
    console_cursor_enable(1);
}

static void desktop_host_console_end(void) {
    console_cursor_enable(0);
    console_reset_region();
    terminal_console_active = 0;
    desktop_draw_base();
}

static void desktop_run_browser(const boot_info_t *info) {
    desktop_window_t win;
    char command[] = "browse";

    desktop_host_console_begin(&win, "Browse");
    shell_run_command(command, info);
    desktop_host_console_end();
}

static void desktop_launch_app(desktop_app_t app, const boot_info_t *info) {
    if (app == DESKTOP_APP_TERMINAL) {
        terminal_open = 1;
        terminal_ready = 0;
    } else if (app == DESKTOP_APP_BROWSER) {
        desktop_run_browser(info);
        desktop_draw_base();
        if (terminal_open) {
            desktop_terminal_draw(1);
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
}

static void desktop_terminal_prompt(void) {
    terminal_len = 0;
    terminal_line[0] = '\0';
    shell_print_prompt();
}

static void desktop_terminal_open(void) {
    terminal_open = 1;
    terminal_ready = 0;
}

static void desktop_terminal_draw(int fresh) {
    if (!terminal_open) {
        return;
    }

    desktop_make_window(&terminal_window);
    desktop_draw_window(&terminal_window, "Terminal");
    desktop_terminal_focus();

    if (fresh || !terminal_ready) {
        console_clear();
        console_puts("Desktop terminal. Commands run inside this real window.\n");
        console_puts("Click Browse to open the file browser. Close box hides this window.\n\n");
        desktop_terminal_prompt();
        terminal_ready = 1;
    }
}

static void desktop_terminal_close(void) {
    if (!terminal_open) {
        return;
    }

    console_cursor_enable(0);
    console_reset_region();
    terminal_console_active = 0;
    terminal_open = 0;
    terminal_ready = 0;
    terminal_len = 0;
    terminal_line[0] = '\0';
    desktop_draw_base();
    terminal_console_active = 0;
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

        if (left_pressed && !left_was_pressed) {
            desktop_app_t app = launcher_at(x, y);
            if (app != DESKTOP_APP_NONE) {
                cursor_restore();
                desktop_launch_app(app, info);
                if (app == DESKTOP_APP_TERMINAL) {
                    desktop_draw_base();
                    terminal_console_active = 0;
                    desktop_terminal_draw(1);
                }
                last_x = (uint32_t)mouse_x();
                last_y = (uint32_t)mouse_y();
                last_buttons = mouse_buttons();
                cursor_draw_at(last_x, last_y);
                continue;
            }
            if (terminal_open && desktop_window_close_hit(&terminal_window, x, y)) {
                cursor_restore();
                desktop_terminal_close();
                last_x = (uint32_t)mouse_x();
                last_y = (uint32_t)mouse_y();
                last_buttons = mouse_buttons();
                cursor_draw_at(last_x, last_y);
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
