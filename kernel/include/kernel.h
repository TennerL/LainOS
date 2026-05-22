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
void console_draw_text_at_pixel(unsigned int x, unsigned int y, const char *text, unsigned int fg, unsigned int bg);
unsigned int console_rows(void);
unsigned int console_columns(void);
void console_clear_line(unsigned int row);
void console_puts(const char *s);
void console_set_output_hook(void (*hook)(char ch));
void console_suppress_current_cpu_push(void);
void console_suppress_current_cpu_pop(void);
void console_newline(void);
void console_cursor_enable(int enabled);
void console_cursor_tick(void);
void console_put_hex64(unsigned long long value);
void console_put_hex32(unsigned int value);
void console_put_dec64(unsigned long long value);
void console_kprintf1(const char *fmt, unsigned long long a1);
void console_kprintf2(const char *fmt, unsigned long long a1, unsigned long long a2);
void console_panic(const char *msg);
int console_read_line(char *buffer, unsigned int max_len);
void put_pixel(unsigned int x, unsigned int y, unsigned int color);
unsigned int graphics_width(void);
unsigned int graphics_height(void);
unsigned int graphics_pitch(void);
unsigned int graphics_format(void);
int graphics_backbuffer_active(void);
int graphics_capture_rect_packed(unsigned int x,
                                 unsigned int y,
                                 unsigned int width,
                                 unsigned int height,
                                 unsigned int *out,
                                 unsigned int out_pixels);
int graphics_draw_rect_packed(unsigned int x,
                              unsigned int y,
                              unsigned int width,
                              unsigned int height,
                              const unsigned int *pixels,
                              unsigned int pixel_count);
void graphics_scroll_rect(unsigned int x,
                          unsigned int y,
                          unsigned int width,
                          unsigned int height,
                          int dy,
                          unsigned int fill_rgb_color);
unsigned int graphics_viewport_active(void);
unsigned int graphics_viewport_x(void);
unsigned int graphics_viewport_y(void);
unsigned int graphics_smp_last_workers(void);
uint64_t graphics_smp_jobs(void);
uint64_t graphics_smp_ops(void);
uint64_t graphics_smp_pixels(void);
unsigned int graphics_get_pixel(unsigned int x, unsigned int y);
void graphics_fill_rect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color);
void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color);
void graphics_draw_line(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1, unsigned int color);
void graphics_clear(unsigned int color);
void graphics_viewport_push(unsigned int x, unsigned int y, unsigned int width, unsigned int height);
void graphics_viewport_pop(void);
void console_set_margin(unsigned int x, unsigned int y);
void console_set_region(unsigned int left, unsigned int top, unsigned int right, unsigned int bottom);
void console_set_region_preserve(unsigned int left, unsigned int top, unsigned int right, unsigned int bottom);
void console_reset_region(void);
int console_point_to_cell(unsigned int x,
                          unsigned int y,
                          unsigned int *col,
                          unsigned int *row);
int console_split_enable(void);
void console_split_disable(void);
void console_split_focus_next(void);
int console_split_enabled(void);
unsigned int console_active_pane(void);

void cpu_init_tables(void);
void cpu_detect_topology(const boot_info_t *info);
unsigned int cpu_start_secondary_cores(void);
unsigned int cpu_core_count(void);
unsigned int cpu_online_core_count(void);
unsigned int cpu_current_index(void);
unsigned int cpu_lapic_id(unsigned int index);
unsigned long long cpu_lapic_base(void);
unsigned long long cpu_core_busy_ticks(unsigned int core);
unsigned long long cpu_core_local_timer_ticks(unsigned int core);
unsigned int cpu_core_local_timer_configured(unsigned int core);
int cpu_enable_lapic_timer(unsigned int hz);
unsigned int cpu_lapic_timer_frequency(void);
unsigned int cpu_lapic_timer_init_count(void);
typedef void (*smp_work_fn_t)(void *arg);
unsigned int smp_submit_work(smp_work_fn_t fn, void *arg);
int smp_work_done(unsigned int id);
void smp_wait_work(unsigned int id);
unsigned int smp_pending_work_count(void);
typedef void (*kernel_task_fn_t)(void *arg);
enum {
    KERNEL_TASK_STATE_FREE = 0u,
    KERNEL_TASK_STATE_QUEUED = 1u,
    KERNEL_TASK_STATE_RUNNING = 2u,
    KERNEL_TASK_STATE_DONE = 3u
};
#define KERNEL_TASK_NAME_SIZE 32u
typedef struct {
    unsigned int id;
    unsigned int state;
    unsigned int smp_id;
    unsigned long long submitted_ticks;
    unsigned long long started_ticks;
    unsigned long long finished_ticks;
    char name[KERNEL_TASK_NAME_SIZE];
} kernel_task_info_t;
unsigned int kernel_task_submit(kernel_task_fn_t fn, void *arg);
unsigned int kernel_task_submit_named(kernel_task_fn_t fn, void *arg, const char *name);
int kernel_task_async_supported(void);
int kernel_task_done(unsigned int id);
void kernel_task_release(unsigned int id);
void kernel_task_wait(unsigned int id);
unsigned int kernel_task_poll(void);
unsigned int kernel_task_pending_count(void);
unsigned int kernel_task_snapshot(kernel_task_info_t *out, unsigned int max_count);
void cpu_exception_handler(void *frame);
void interrupts_init(void);
void keyboard_init(void);
int mouse_init(void);
void mouse_irq_handler(void);
void mouse_handle_byte(uint8_t value);
void mouse_apply_usb_report(uint8_t report_buttons, int dx, int dy, int wheel);
#ifndef MOUSE_DEBUG_INFO_T_DEFINED
#define MOUSE_DEBUG_INFO_T_DEFINED
typedef struct {
    int enabled;
    int ps2_enabled;
    int ps2_has_wheel;
    int ps2_packet_size;
    int last_source;
    int x;
    int y;
    int buttons;
    int dx;
    int dy;
    int wheel;
    int pending_wheel;
    uint32_t ps2_packets;
    uint32_t usb_reports;
    uint32_t rejected_packets;
} mouse_debug_info_t;
#endif
int mouse_enabled(void);
int mouse_x(void);
int mouse_y(void);
int mouse_buttons(void);
void mouse_snapshot(int *out_x, int *out_y, int *out_buttons);
void mouse_set_position(int x, int y);
void mouse_consume_motion(int *out_dx, int *out_dy, int *out_buttons);
int mouse_consume_wheel(void);
int mouse_dx(void);
int mouse_dy(void);
int mouse_wheel(void);
void mouse_debug_info(mouse_debug_info_t *out);
void timer_init(void);
void timer_irq_handler(void);
unsigned long long timer_ticks(void);
unsigned int timer_frequency(void);
typedef struct {
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
} rtc_time_t;
void clock_init(void);
int clock_read_rtc(rtc_time_t *out);
int clock_get_rtc_time(rtc_time_t *out);
int clock_rtc_time_valid(const rtc_time_t *time);
uint32_t clock_days_since_year0(uint32_t year, uint32_t month, uint32_t day);
uint32_t clock_seconds_since_midnight(uint32_t hour, uint32_t minute, uint32_t second);
uint64_t clock_unix_time_from_rtc(const rtc_time_t *time);
uint64_t clock_unix_time(void);
void cpu_lapic_timer_handler(void);
void power_reboot(void);
int power_poweroff(const boot_info_t *info);
unsigned long long status_memory_total_kb(void);
unsigned long long status_memory_free_kb(void);
unsigned long long status_memory_used_kb(void);
unsigned int status_cpu_core_count(void);
unsigned int status_cpu_usage_percent(unsigned int core);
unsigned long long status_busy_percent_from_ticks(unsigned long long delta_ticks,
                                                  unsigned long long delta_idle_ticks);
void status_cpu_enter_idle(void);
void status_cpu_leave_idle(void);
void status_cpu_timer_tick(void);
void statusbar_enable(void);
void statusbar_update_if_due(void);
void shell_modules_tick(void);

#endif
