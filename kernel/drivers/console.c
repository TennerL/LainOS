#include <stdint.h>
#include "kernel.h"
#include "graphics.h"

#define FONT_W 8u
#define FONT_H 8u
#define DEFAULT_CONSOLE_MARGIN_X 16u
#define DEFAULT_CONSOLE_MARGIN_Y 16u
#define CONSOLE_ROW_ADVANCE (FONT_H + 2u)
#define DEFAULT_FG_COLOR 0x00F725FCu
#define DEFAULT_BG_COLOR 0x0035063Eu
#define PANIC_BG_COLOR 0x00000080u
#define ASCII_FIRST 32u
#define ASCII_COUNT 95u
#define CURSOR_BLINK_TICKS 30u
#define CONSOLE_MAX_PANES 2u
#define CONSOLE_SPLIT_GAP 16u
#define CONSOLE_SPLIT_DIVIDER_WIDTH 2u

typedef struct {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
    uint32_t cursor_x;
    uint32_t cursor_y;
} console_pane_t;

static console_pane_t console_panes[CONSOLE_MAX_PANES];
static uint32_t active_console_pane;
static uint32_t console_pane_count = 1;
static int cursor_enabled;
static int cursor_visible = 1;
static unsigned long long last_cursor_blink_tick = 0;
static uint64_t fb_base;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint32_t current_fg_color = DEFAULT_FG_COLOR;
static uint32_t current_bg_color = DEFAULT_BG_COLOR;
static char dec_buffer[32];
static void (*console_output_hook)(char ch);

static console_pane_t *active_pane(void) {
    return &console_panes[active_console_pane];
}

static const console_pane_t *active_pane_const(void) {
    return &console_panes[active_console_pane];
}

static uint32_t console_text_left(void) {
    return active_pane_const()->left;
}

static uint32_t console_text_top(void) {
    return active_pane_const()->top;
}

static uint32_t console_text_right(void) {
    return active_pane_const()->right;
}

static uint32_t console_text_bottom(void) {
    return active_pane_const()->bottom;
}

static void console_init_pane(console_pane_t *pane,
                              uint32_t left,
                              uint32_t top,
                              uint32_t right,
                              uint32_t bottom) {
    pane->left = left;
    pane->top = top;
    pane->right = right;
    pane->bottom = bottom;
    pane->cursor_x = left;
    pane->cursor_y = top;
}

static void clear_region(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    for (uint32_t y = top; y < bottom; ++y) {
        for (uint32_t x = left; x < right; ++x) {
            put_pixel(x, y, current_bg_color);
        }
    }
}

static void clear_pane(const console_pane_t *pane) {
    clear_region(pane->left, pane->top, pane->right, pane->bottom);
}

static void draw_split_divider(void) {
    if (console_pane_count < 2u) {
        return;
    }

    uint32_t divider_left = console_panes[0].right;
    uint32_t divider_right = console_panes[1].left;
    uint32_t color = current_fg_color;

    for (uint32_t y = console_panes[0].top; y < console_panes[0].bottom; ++y) {
        for (uint32_t x = divider_left; x < divider_right; ++x) {
            uint32_t mid = divider_left + ((divider_right - divider_left) / 2u);
            if (x >= mid && x < mid + CONSOLE_SPLIT_DIVIDER_WIDTH) {
                put_pixel(x, y, color);
            } else {
                put_pixel(x, y, current_bg_color);
            }
        }
    }
}

static const uint8_t font_data[ASCII_COUNT][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x12,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},
    {0x1C,0x36,0x1C,0x3B,0x66,0x66,0x3B,0x00}, {0x18,0x18,0x10,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0x7E,0x3C,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x10,0x20}, {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, {0x00,0x18,0x18,0x00,0x18,0x18,0x10,0x20},
    {0x0E,0x18,0x30,0x60,0x30,0x18,0x0E,0x00}, {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x70,0x18,0x0C,0x06,0x0C,0x18,0x70,0x00}, {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
    {0x3C,0x42,0x5A,0x5A,0x5E,0x40,0x3C,0x00}, {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00},
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00},
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, {0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C,0x00},
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0x40,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x18,0x3C,0x66,0x42,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    {0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x7C},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x6C,0x38}, {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, {0x00,0x00,0x6C,0x7E,0x7E,0x6B,0x63,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    {0x00,0x00,0x6E,0x70,0x60,0x60,0x60,0x00}, {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    {0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00}, {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x00,0x00,0x63,0x63,0x6B,0x7F,0x36,0x00},
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x7C},
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x31,0x6B,0x46,0x00,0x00,0x00,0x00,0x00}
};

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    graphics_put_pixel(x, y, color);
}

static void draw_cursor(void) {
    console_pane_t *pane = active_pane();

    if (!cursor_enabled) return;
    if (pane->cursor_x + FONT_W > fb_width || pane->cursor_y + FONT_H > fb_height) return;
    for (uint32_t row = 0; row < FONT_H; ++row) {
        for (uint32_t col = 0; col < FONT_W; ++col) {
            put_pixel(pane->cursor_x + col, pane->cursor_y + row, current_fg_color);
        }
    }
    cursor_visible = 1;
}

static void erase_cursor(void) {
    console_pane_t *pane = active_pane();

    if (!cursor_enabled) return;
    if (pane->cursor_x + FONT_W > fb_width || pane->cursor_y + FONT_H > fb_height) return;
    for (uint32_t row = 0; row < FONT_H; ++row) {
        for (uint32_t col = 0; col < FONT_W; ++col) {
            put_pixel(pane->cursor_x + col, pane->cursor_y + row, current_bg_color);
        }
    }
    cursor_visible = 0;
}

void console_cursor_tick(void){
    if(!cursor_enabled){
        return;
    }

    unsigned long long now = timer_ticks();
    if(now - last_cursor_blink_tick < CURSOR_BLINK_TICKS) {
        return;
    }
    last_cursor_blink_tick = now;

    if(cursor_visible){
        erase_cursor();
    } else {
        draw_cursor();
    }
}

void fill_screen_color(uint32_t color) {
    graphics_clear(color);
}

static void scroll_screen(void) {
    console_pane_t *pane = active_pane();
    uint32_t *fb = (uint32_t *)(uintptr_t)fb_base;
    uint32_t packed_bg = graphics_pack_color(current_bg_color);
    uint32_t text_top = pane->top;
    uint32_t text_right = console_text_right();
    uint32_t text_bottom = console_text_bottom();
    uint32_t clear_start;

    if (text_bottom <= text_top + CONSOLE_ROW_ADVANCE) {
        pane->cursor_y = text_top;
        return;
    }

    for (uint32_t y = text_top; y + CONSOLE_ROW_ADVANCE < text_bottom; ++y) {
        for (uint32_t x = pane->left; x < text_right; ++x) {
            fb[y * fb_pitch + x] = fb[(y + CONSOLE_ROW_ADVANCE) * fb_pitch + x];
        }
    }

    clear_start = text_bottom - CONSOLE_ROW_ADVANCE;
    for (uint32_t y = clear_start; y < text_bottom; ++y) {
        for (uint32_t x = pane->left; x < text_right; ++x) {
            fb[y * fb_pitch + x] = packed_bg;
        }
    }

    pane->cursor_y = clear_start;
}

static void putc_raw(char ch) {
    console_pane_t *pane = active_pane();

    erase_cursor();
    if ((uint8_t)ch < ASCII_FIRST || (uint8_t)ch >= ASCII_FIRST + ASCII_COUNT) return;
    if (console_output_hook != 0) {
        console_output_hook(ch);
    }
    uint32_t glyph = (uint8_t)ch - ASCII_FIRST;

    if (pane->cursor_x + FONT_W > console_text_right()) console_newline();

    for (uint32_t row = 0; row < FONT_H; ++row) {
        uint8_t bits = font_data[glyph][row];
        for (uint32_t col = 0; col < FONT_W; ++col) {
            uint32_t color = (bits & (1u << (7u - col))) ? current_fg_color : current_bg_color;
            put_pixel(pane->cursor_x + col, pane->cursor_y + row, color);
        }
    }
    pane->cursor_x += FONT_W;
    draw_cursor();
}

static void backspace(void) {
    console_pane_t *pane = active_pane();

    erase_cursor();
    if (pane->cursor_x <= pane->left) {
        draw_cursor();
        return;
    }
    pane->cursor_x -= FONT_W;
    for (uint32_t row = 0; row < FONT_H; ++row) {
        for (uint32_t col = 0; col < FONT_W; ++col) {
            put_pixel(pane->cursor_x + col, pane->cursor_y + row, current_bg_color);
        }
    }
    draw_cursor();
}

void console_init(unsigned long long framebuffer_base,
                  unsigned int framebuffer_width,
                  unsigned int framebuffer_height,
                  unsigned int framebuffer_pixels_per_scanline) {
    fb_base = framebuffer_base;
    fb_width = framebuffer_width;
    fb_height = framebuffer_height;
    fb_pitch = framebuffer_pixels_per_scanline;
    console_init_pane(&console_panes[0],
                      DEFAULT_CONSOLE_MARGIN_X,
                      DEFAULT_CONSOLE_MARGIN_Y,
                      fb_width > DEFAULT_CONSOLE_MARGIN_X ? fb_width - DEFAULT_CONSOLE_MARGIN_X : 0,
                      fb_height > DEFAULT_CONSOLE_MARGIN_Y ? fb_height - DEFAULT_CONSOLE_MARGIN_Y : 0);
    console_init_pane(&console_panes[1], 0, 0, 0, 0);
    active_console_pane = 0;
    console_pane_count = 1;
    cursor_enabled = 1;
    fill_screen_color(current_bg_color);
}

void console_clear(void) {
    console_pane_t *pane = active_pane();

    clear_pane(pane);
    pane->cursor_x = pane->left;
    pane->cursor_y = pane->top;
    if (console_pane_count > 1u) {
        draw_split_divider();
    }
    draw_cursor();
}

void console_set_bg_color(uint32_t color) {
    int redraw_cursor = cursor_enabled && cursor_visible;

    if (redraw_cursor) {
        erase_cursor();
    }

    current_bg_color = color;
    fill_screen_color(current_bg_color);
    if (console_pane_count > 1u) {
        draw_split_divider();
    }

    if (redraw_cursor) {
        draw_cursor();
    }
}

void console_set_fg_color(uint32_t color) {
    int redraw_cursor = cursor_enabled && cursor_visible;

    if (redraw_cursor) {
        erase_cursor();
    }

    current_fg_color = color;

    if (redraw_cursor) {
        draw_cursor();
    }
}

void console_newline(void) {
    console_pane_t *pane = active_pane();

    if (cursor_visible) {
        erase_cursor();
    }
    pane->cursor_x = pane->left;
    pane->cursor_y += CONSOLE_ROW_ADVANCE;
    if (pane->cursor_y + FONT_H > console_text_bottom()) scroll_screen();
    draw_cursor();
}

void console_set_cursor(unsigned int col, unsigned int row) {
    console_pane_t *pane = active_pane();
    uint32_t max_col = 0;
    uint32_t max_row = 0;

    if (cursor_visible) {
        erase_cursor();
    }

    if (console_text_right() > pane->left + FONT_W) {
        max_col = (console_text_right() - pane->left - FONT_W) / FONT_W;
    }

    if (console_text_bottom() > pane->top + FONT_H) {
        max_row = (console_text_bottom() - pane->top - FONT_H) / CONSOLE_ROW_ADVANCE;
    }

    if (col > max_col) {
        col = max_col;
    }

    if (row > max_row) {
        row = max_row;
    }

    pane->cursor_x = pane->left + col * FONT_W;
    pane->cursor_y = pane->top + row * CONSOLE_ROW_ADVANCE;
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    draw_cursor();
}

static void console_put_char_at_pixel_colors(uint32_t x,
                                             uint32_t y,
                                             char ch,
                                             uint32_t fg,
                                             uint32_t bg) {
    if ((uint8_t)ch < ASCII_FIRST || (uint8_t)ch >= ASCII_FIRST + ASCII_COUNT){
        ch = ' ';
    }

    uint32_t glyph = (uint8_t)ch - ASCII_FIRST;

    for (uint32_t glyph_row = 0; glyph_row < FONT_H; ++glyph_row) {
        uint8_t bits = font_data[glyph][glyph_row];

        for (uint32_t glyph_col = 0; glyph_col < FONT_W; ++glyph_col) {
            uint32_t color =
                (bits & (1u << (7u - glyph_col))) ? fg : bg;
            put_pixel(x + glyph_col, y + glyph_row, color);
        }
    }
}

static void console_put_char_at_pixel(uint32_t x, uint32_t y, char ch) {
    console_put_char_at_pixel_colors(x, y, ch, current_fg_color, current_bg_color);
}

void console_put_char_at(unsigned int col, unsigned int row, char ch){
    uint32_t x = console_text_left() + col * FONT_W;
    uint32_t y = console_text_top() + row * CONSOLE_ROW_ADVANCE;

    console_put_char_at_pixel(x, y, ch);
}

void console_put_char_at_screen(unsigned int col, unsigned int row, char ch) {
    uint32_t x = col * FONT_W;
    uint32_t y = row * CONSOLE_ROW_ADVANCE;

    console_put_char_at_pixel(x, y, ch);
}

void console_put_dec_at(unsigned int col, unsigned int row, unsigned long long value) {
    char digits[32];
    unsigned int count = 0;

    if (value == 0) {
        console_put_char_at(col, row, '0');
        return;
    }

    while (value != 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    }

    while (count > 0) {
        console_put_char_at(col++, row, digits[--count]);
    }
}

void console_put_dec_at_screen(unsigned int col, unsigned int row, unsigned long long value) {
    char digits[32];
    unsigned int count = 0;

    if (value == 0) {
        console_put_char_at_screen(col, row, '0');
        return;
    }

    while (value != 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    }

    while (count > 0) {
        console_put_char_at_screen(col++, row, digits[--count]);
    }
}

void console_draw_text_at_pixel(unsigned int x,
                                unsigned int y,
                                const char *text,
                                unsigned int fg,
                                unsigned int bg) {
    uint32_t px = x;

    if (text == 0) {
        return;
    }

    while (*text) {
        console_put_char_at_pixel_colors(px, y, *text++, fg, bg);
        px += FONT_W;
    }
}

void console_clear_line(unsigned int row) {
    console_pane_t *pane = active_pane();
    uint32_t y = pane->top + row * CONSOLE_ROW_ADVANCE;
    int redraw_cursor = cursor_enabled && cursor_visible;

    if (y + FONT_H > console_text_bottom()) {
        return;
    }

    if (redraw_cursor) {
        erase_cursor();
    }

    for (uint32_t py = 0; py < CONSOLE_ROW_ADVANCE; ++py) {
        if (y + py >= fb_height) {
            break;
        }

        for (uint32_t x = pane->left; x < console_text_right(); ++x) {
            put_pixel(x, y + py, current_bg_color);
        }
    }

    if (redraw_cursor) {
        draw_cursor();
    }
}

unsigned int console_columns(void) {
    if (console_text_right() <= console_text_left() + FONT_W) {
        return 0;
    }

    return (console_text_right() - console_text_left()) / FONT_W;
}

unsigned int console_rows(void) {
    if (console_text_bottom() <= console_text_top() + FONT_H) {
        return 0;
    }
    return (console_text_bottom() - console_text_top()) / CONSOLE_ROW_ADVANCE;
}

void console_cursor_enable(int enabled) {
    if (cursor_enabled) {
        erase_cursor();
    }
    cursor_enabled = enabled;
    if (cursor_enabled) {
        draw_cursor();
    }
}

void console_puts(const char *s) {
    while (*s) {
        char ch = *s++;
        if (ch == '\n') {
            if (console_output_hook != 0) {
                console_output_hook('\n');
            }
            console_newline();
        } else if (ch == '\t') {
            for (int i = 0; i < 4; ++i) putc_raw(' ');
        } else if (ch == '\b') {
            if (console_output_hook != 0) {
                console_output_hook('\b');
            }
            backspace();
        } else {
            putc_raw(ch);
        }
    }
}

void console_set_output_hook(void (*hook)(char ch)) {
    console_output_hook = hook;
}

void console_set_margin(uint32_t x, uint32_t y) {
    console_pane_t *pane = active_pane();
    int redraw_cursor = cursor_enabled && cursor_visible;
    uint32_t right_margin = x == 0 ? 0 : DEFAULT_CONSOLE_MARGIN_X;
    uint32_t bottom_margin = y == 0 ? 0 : DEFAULT_CONSOLE_MARGIN_Y;
    uint32_t right = fb_width > right_margin ? fb_width - right_margin : fb_width;
    uint32_t bottom = fb_height > bottom_margin ? fb_height - bottom_margin : fb_height;

    if(redraw_cursor) {
        erase_cursor();
    }

    if (x + FONT_W >= right) {
        x = right > FONT_W ? right - FONT_W : 0;
    }

    if (y + FONT_H >= bottom) {
        y = bottom > FONT_H ? bottom - FONT_H : 0;
    }

    pane->left = x;
    pane->top = y;
    pane->right = right;
    pane->bottom = bottom;
    pane->cursor_x = pane->left;
    pane->cursor_y = pane->top;
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    clear_pane(pane);
    if (console_pane_count > 1u) {
        draw_split_divider();
    }

    if(redraw_cursor){
        draw_cursor();
    }
}

void console_set_region(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    console_pane_t *pane;
    int redraw_cursor = cursor_enabled && cursor_visible;

    if(redraw_cursor) {
        erase_cursor();
    }

    if (right > fb_width) {
        right = fb_width;
    }
    if (bottom > fb_height) {
        bottom = fb_height;
    }
    if (left + FONT_W >= right) {
        left = right > FONT_W ? right - FONT_W : 0;
    }
    if (top + FONT_H >= bottom) {
        top = bottom > FONT_H ? bottom - FONT_H : 0;
    }

    console_split_disable();
    active_console_pane = 0;
    console_pane_count = 1;
    pane = active_pane();
    console_init_pane(pane, left, top, right, bottom);
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    clear_pane(pane);
    if(redraw_cursor) {
        draw_cursor();
    }
}

void console_reset_region(void) {
    console_set_region(DEFAULT_CONSOLE_MARGIN_X,
                       DEFAULT_CONSOLE_MARGIN_Y,
                       fb_width > DEFAULT_CONSOLE_MARGIN_X ? fb_width - DEFAULT_CONSOLE_MARGIN_X : fb_width,
                       fb_height > DEFAULT_CONSOLE_MARGIN_Y ? fb_height - DEFAULT_CONSOLE_MARGIN_Y : fb_height);
}

int console_point_to_cell(uint32_t x, uint32_t y, uint32_t *col, uint32_t *row) {
    const console_pane_t *pane = active_pane_const();

    if (x < pane->left || y < pane->top || x >= pane->right || y >= pane->bottom) {
        return 0;
    }

    if (col != 0) {
        *col = (x - pane->left) / FONT_W;
    }
    if (row != 0) {
        *row = (y - pane->top) / CONSOLE_ROW_ADVANCE;
    }

    return 1;
}

int console_split_enabled(void) {
    return console_pane_count > 1u;
}

int console_split_enable(void) {
    uint32_t outer_left = console_panes[0].left;
    uint32_t outer_top = console_panes[0].top;
    uint32_t outer_right = console_panes[0].right;
    uint32_t outer_bottom = console_panes[0].bottom;
    uint32_t total_width;
    uint32_t mid;
    uint32_t left_right;
    uint32_t right_left;
    uint32_t min_width = DEFAULT_CONSOLE_MARGIN_X + FONT_W * 12u;

    if (console_pane_count > 1u) {
        return 0;
    }

    if (outer_right <= outer_left) {
        return -1;
    }

    total_width = outer_right - outer_left;
    if (total_width <= min_width * 2u + CONSOLE_SPLIT_GAP) {
        return -1;
    }

    mid = outer_left + total_width / 2u;
    left_right = mid - CONSOLE_SPLIT_GAP / 2u;
    right_left = mid + CONSOLE_SPLIT_GAP / 2u;

    console_init_pane(&console_panes[0], outer_left, outer_top, left_right, outer_bottom);
    console_init_pane(&console_panes[1], right_left, outer_top, outer_right, outer_bottom);
    console_pane_count = 2;
    active_console_pane = 0;

    clear_pane(&console_panes[0]);
    clear_pane(&console_panes[1]);
    draw_split_divider();
    if (cursor_enabled) {
        draw_cursor();
    }

    return 0;
}

void console_split_disable(void) {
    uint32_t outer_left;
    uint32_t outer_top;
    uint32_t outer_right;
    uint32_t outer_bottom;
    uint32_t saved_cursor_x;
    uint32_t saved_cursor_y;

    if (console_pane_count < 2u) {
        return;
    }

    outer_left = console_panes[0].left;
    outer_top = console_panes[0].top;
    outer_right = console_panes[1].right;
    outer_bottom = console_panes[0].bottom;
    saved_cursor_x = console_panes[0].cursor_x;
    saved_cursor_y = console_panes[0].cursor_y;

    if (cursor_visible) {
        erase_cursor();
    }

    clear_region(console_panes[0].right, outer_top, outer_right, outer_bottom);
    console_init_pane(&console_panes[0], outer_left, outer_top, outer_right, outer_bottom);
    console_panes[0].cursor_x = saved_cursor_x;
    console_panes[0].cursor_y = saved_cursor_y;
    console_pane_count = 1;
    active_console_pane = 0;
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    if (cursor_enabled) {
        draw_cursor();
    }
}

void console_split_focus_next(void) {
    if (console_pane_count < 2u) {
        return;
    }

    if (cursor_visible) {
        erase_cursor();
    }

    active_console_pane = (active_console_pane + 1u) % console_pane_count;
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    if (cursor_enabled) {
        draw_cursor();
    }
}

unsigned int console_active_pane(void) {
    return active_console_pane;
}

static void put_hex_n(uint64_t value, unsigned digits) {
    for (unsigned i = 0; i < digits; ++i) {
        unsigned nibble = (unsigned)((value >> ((digits - 1u - i) * 4u)) & 0xFu);
        putc_raw((char)(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10)));
    }
}

void console_put_hex64(unsigned long long value) { put_hex_n(value, 16); }
void console_put_hex32(unsigned int value) { put_hex_n(value, 8); }

void console_put_dec64(unsigned long long value) {
    if (value == 0) {
        putc_raw('0');
        return;
    }
    dec_buffer[31] = '\0';
    int i = 30;
    while (value && i >= 0) {
        dec_buffer[i--] = (char)('0' + (value % 10));
        value /= 10;
    }
    console_puts(&dec_buffer[i + 1]);
}

static unsigned long long next_arg(int *idx, unsigned long long a1, unsigned long long a2) {
    unsigned long long v = (*idx == 0) ? a1 : a2;
    (*idx)++;
    return v;
}

static void kprintf_common(const char *fmt, unsigned long long a1, unsigned long long a2) {
    int idx = 0;
    while (*fmt) {
        char ch = *fmt++;
        if (ch != '%') {
            char tmp[2] = { ch, 0 };
            console_puts(tmp);
            continue;
        }
        ch = *fmt++;
        if (!ch) break;
        if (ch == '%') {
            putc_raw('%');
        } else if (ch == 's') {
            console_puts((const char *)(uintptr_t)next_arg(&idx, a1, a2));
        } else if (ch == 'x') {
            console_put_hex64(next_arg(&idx, a1, a2));
        } else if (ch == 'u') {
            console_put_dec64(next_arg(&idx, a1, a2));
        } else if (ch == 'c') {
            putc_raw((char)next_arg(&idx, a1, a2));
        } else {
            putc_raw(ch);
        }
    }
}

void console_kprintf1(const char *fmt, unsigned long long a1) { kprintf_common(fmt, a1, 0); }
void console_kprintf2(const char *fmt, unsigned long long a1, unsigned long long a2) { kprintf_common(fmt, a1, a2); }

void console_panic(const char *msg) {
    fill_screen_color(PANIC_BG_COLOR);
    console_pane_count = 1;
    active_console_pane = 0;
    console_init_pane(&console_panes[0], 16u, 16u, fb_width, fb_height);
    cursor_enabled = 0;
    console_puts("KERNEL PANIC: ");
    console_puts(msg);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
