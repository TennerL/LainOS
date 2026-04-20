#ifndef KERNEL_H
#define KERNEL_H

#include "bootinfo.h"

void kernel_main(boot_info_t *info);

void console_init(unsigned long long framebuffer_base,
                  unsigned int framebuffer_width,
                  unsigned int framebuffer_height,
                  unsigned int framebuffer_pixels_per_scanline);
void console_clear(void);
void fill_screen_color(unsigned int);
void console_set_cursor(unsigned int col, unsigned int row);
void console_put_char_at(unsigned int col, unsigned int row, char ch);
unsigned int console_rows(void);
unsigned int console_columns(void);
void console_clear_line(unsigned int row);
void console_puts(const char *s);
void console_newline(void);
void console_cursor_enable(int enabled);
void console_cursor_tick(void);
void console_put_hex64(unsigned long long value);
void console_put_hex32(unsigned int value);
void console_put_dec64(unsigned long long value);
void console_kprintf1(const char *fmt, unsigned long long a1);
void console_kprintf2(const char *fmt, unsigned long long a1, unsigned long long a2);
void console_panic(const char *msg);
void console_read_line(char *buffer, unsigned int max_len);

void cpu_init_tables(void);
void interrupts_init(void);
void keyboard_init(void);
void timer_init(void);
void timer_irq_handler(void);
unsigned long long timer_ticks(void);
unsigned int timer_frequency(void);

#endif
