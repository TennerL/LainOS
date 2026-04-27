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
void console_set_bg_color(unsigned int color);
void console_set_fg_color(unsigned int color);
void console_set_cursor(unsigned int col, unsigned int row);
void console_put_char_at(unsigned int col, unsigned int row, char ch);
void console_put_dec_at(unsigned int col, unsigned int row, unsigned long long value);
void console_put_char_at_screen(unsigned int col, unsigned int row, char ch);
void console_put_dec_at_screen(unsigned int col, unsigned int row, unsigned long long value);
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
void put_pixel(unsigned int x, unsigned int y, unsigned int color);
void console_set_margin(unsigned int x, unsigned int y);

void cpu_init_tables(void);
void interrupts_init(void);
void keyboard_init(void);
void timer_init(void);
void timer_irq_handler(void);
unsigned long long timer_ticks(void);
unsigned int timer_frequency(void);
unsigned long long status_memory_total_kb(void);
unsigned long long status_memory_free_kb(void);
unsigned long long status_memory_used_kb(void);
unsigned int status_cpu_core_count(void);
unsigned int status_cpu_usage_percent(unsigned int core);
void status_cpu_enter_idle(void);
void status_cpu_leave_idle(void);
void status_cpu_timer_tick(void);
void statusbar_enable(void);
void statusbar_update_if_due(void);

#endif
