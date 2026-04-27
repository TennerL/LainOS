#include <stdint.h>
#include "kernel.h"

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

static uint32_t cursor_x;
static uint32_t cursor_y;
static int cursor_enabled;
static int cursor_visible = 1;
static unsigned long long last_cursor_blink_tick = 0;
static uint64_t fb_base;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint32_t current_fg_color = DEFAULT_FG_COLOR;
static uint32_t current_bg_color = DEFAULT_BG_COLOR;
static uint32_t current_console_margin_x = DEFAULT_CONSOLE_MARGIN_X;
static uint32_t current_console_margin_y = DEFAULT_CONSOLE_MARGIN_Y;
static char dec_buffer[32];

static uint32_t console_text_right(void) {
    return fb_width > current_console_margin_x ? fb_width - current_console_margin_x : 0;
}

static uint32_t console_text_bottom(void) {
    return fb_height > current_console_margin_y ? fb_height - current_console_margin_y : 0;
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
    if (x >= fb_width || y >= fb_height) return;
    uint32_t *fb = (uint32_t *)(uintptr_t)fb_base;
    fb[(uint64_t)y * fb_pitch + x] = color;
}

static void draw_cursor(void) {
    if (!cursor_enabled) return;
    if (cursor_x + FONT_W > fb_width || cursor_y + FONT_H > fb_height) return;
    for (uint32_t row = 0; row < FONT_H; ++row) {
        for (uint32_t col = 0; col < FONT_W; ++col) {
            put_pixel(cursor_x + col, cursor_y + row, current_fg_color);
        }
    }
    cursor_visible = 1;
}

static void erase_cursor(void) {
    if (!cursor_enabled) return;
    if (cursor_x + FONT_W > fb_width || cursor_y + FONT_H > fb_height) return;
    for (uint32_t row = 0; row < FONT_H; ++row) {
        for (uint32_t col = 0; col < FONT_W; ++col) {
            put_pixel(cursor_x + col, cursor_y + row, current_bg_color);
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
    uint32_t *fb = (uint32_t *)(uintptr_t)fb_base;
    uint64_t total = (uint64_t)fb_pitch * fb_height;
    for (uint64_t i = 0; i < total; ++i) fb[i] = color;
}

static void scroll_screen(void) {
    uint32_t *fb = (uint32_t *)(uintptr_t)fb_base;
    uint32_t text_top = current_console_margin_y;
    uint32_t text_right = console_text_right();
    uint32_t text_bottom = console_text_bottom();
    uint32_t clear_start;

    if (text_bottom <= text_top + CONSOLE_ROW_ADVANCE) {
        cursor_y = text_top;
        return;
    }

    for (uint32_t y = text_top; y + CONSOLE_ROW_ADVANCE < text_bottom; ++y) {
        for (uint32_t x = current_console_margin_x; x < text_right; ++x) {
            fb[y * fb_pitch + x] = fb[(y + CONSOLE_ROW_ADVANCE) * fb_pitch + x];
        }
    }

    clear_start = text_bottom - CONSOLE_ROW_ADVANCE;
    for (uint32_t y = clear_start; y < text_bottom; ++y) {
        for (uint32_t x = current_console_margin_x; x < text_right; ++x) {
            fb[y * fb_pitch + x] = current_bg_color;
        }
    }

    cursor_y = clear_start;
}

static void putc_raw(char ch) {
    erase_cursor();
    if ((uint8_t)ch < ASCII_FIRST || (uint8_t)ch >= ASCII_FIRST + ASCII_COUNT) return;
    uint32_t glyph = (uint8_t)ch - ASCII_FIRST;

    if (cursor_x + FONT_W > console_text_right()) console_newline();

    for (uint32_t row = 0; row < FONT_H; ++row) {
        uint8_t bits = font_data[glyph][row];
        for (uint32_t col = 0; col < FONT_W; ++col) {
            uint32_t color = (bits & (1u << (7u - col))) ? current_fg_color : current_bg_color;
            put_pixel(cursor_x + col, cursor_y + row, color);
        }
    }
    cursor_x += FONT_W;
    draw_cursor();
}

static void backspace(void) {
    erase_cursor();
    if (cursor_x <= current_console_margin_x) {
        draw_cursor();
        return;
    }
    cursor_x -= FONT_W;
    for (uint32_t row = 0; row < FONT_H; ++row) {
        for (uint32_t col = 0; col < FONT_W; ++col) {
            put_pixel(cursor_x + col, cursor_y + row, current_bg_color);
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
    cursor_enabled = 1;
    fill_screen_color(current_bg_color);
}

void console_clear(void) {
    uint32_t text_right = console_text_right();
    uint32_t text_bottom = console_text_bottom();

    for (uint32_t y = current_console_margin_y; y < text_bottom; ++y) {
        for (uint32_t x = current_console_margin_x; x < text_right; ++x) {
            put_pixel(x, y, current_bg_color);
        }
    }

    cursor_x = current_console_margin_x;
    cursor_y = current_console_margin_y;
    draw_cursor();
}

void console_set_bg_color(uint32_t color) {
    int redraw_cursor = cursor_enabled && cursor_visible;

    if (redraw_cursor) {
        erase_cursor();
    }

    current_bg_color = color;
    fill_screen_color(current_bg_color);

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
    if (cursor_visible) {
        erase_cursor();
    }
    cursor_x = current_console_margin_x;
    cursor_y += CONSOLE_ROW_ADVANCE;
    if (cursor_y + FONT_H > console_text_bottom()) scroll_screen();
    draw_cursor();
}

void console_set_cursor(unsigned int col, unsigned int row) {
    uint32_t max_col = 0;
    uint32_t max_row = 0;

    if (cursor_visible) {
        erase_cursor();
    }

    if (console_text_right() > current_console_margin_x + FONT_W) {
        max_col = (console_text_right() - current_console_margin_x - FONT_W) / FONT_W;
    }

    if (console_text_bottom() > current_console_margin_y + FONT_H) {
        max_row = (console_text_bottom() - current_console_margin_y - FONT_H) / CONSOLE_ROW_ADVANCE;
    }

    if (col > max_col) {
        col = max_col;
    }

    if (row > max_row) {
        row = max_row;
    }

    cursor_x = current_console_margin_x + col * FONT_W;
    cursor_y = current_console_margin_y + row * CONSOLE_ROW_ADVANCE;
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    draw_cursor();
}

static void console_put_char_at_pixel(uint32_t x, uint32_t y, char ch) {
    if ((uint8_t)ch < ASCII_FIRST || (uint8_t)ch >= ASCII_FIRST + ASCII_COUNT){
        ch = ' ';
    }

    uint32_t glyph = (uint8_t)ch - ASCII_FIRST;

    for (uint32_t glyph_row = 0; glyph_row < FONT_H; ++glyph_row) {
        uint8_t bits = font_data[glyph][glyph_row];

        for (uint32_t glyph_col = 0; glyph_col < FONT_W; ++glyph_col) {
            uint32_t color =
                (bits & (1u << (7u - glyph_col))) ? current_fg_color : current_bg_color;
            put_pixel(x + glyph_col, y + glyph_row, color);
        }
    }
}

void console_put_char_at(unsigned int col, unsigned int row, char ch){
    uint32_t x = current_console_margin_x + col * FONT_W;
    uint32_t y = current_console_margin_y + row * CONSOLE_ROW_ADVANCE;

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

void console_clear_line(unsigned int row) {
    uint32_t y = current_console_margin_y + row * CONSOLE_ROW_ADVANCE;
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

        for (uint32_t x = current_console_margin_x; x < console_text_right(); ++x) {
            put_pixel(x, y + py, current_bg_color);
        }
    }

    if (redraw_cursor) {
        draw_cursor();
    }
}

unsigned int console_columns(void) {
    if (console_text_right() <= current_console_margin_x + FONT_W) {
        return 0;
    }

    return (console_text_right() - current_console_margin_x) / FONT_W;
}

unsigned int console_rows(void) {
    if (console_text_bottom() <= current_console_margin_y + FONT_H) {
        return 0;
    }
    return (console_text_bottom() - current_console_margin_y) / CONSOLE_ROW_ADVANCE;
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
            console_newline();
        } else if (ch == '\t') {
            for (int i = 0; i < 4; ++i) putc_raw(' ');
        } else if (ch == '\b') {
            backspace();
        } else {
            putc_raw(ch);
        }
    }
}

void console_set_margin(uint32_t x, uint32_t y) {
    int redraw_cursor = cursor_enabled && cursor_visible;

    if(redraw_cursor) {
        erase_cursor();
    }

    current_console_margin_x = x;
    current_console_margin_y = y;
    cursor_x = current_console_margin_x;
    cursor_y = current_console_margin_y;
    cursor_visible = 1;
    last_cursor_blink_tick = timer_ticks();

    if(redraw_cursor){
        draw_cursor();
    }
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
    cursor_enabled = 0;
    cursor_x = 16;
    cursor_y = 16;
    console_puts("KERNEL PANIC: ");
    console_puts(msg);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
