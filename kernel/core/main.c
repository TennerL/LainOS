#include "kernel.h"
#include "registry.h"
#include "dma.h"
#include "graphics.h"
#include "kmem.h"
#include "net.h"
#include "shell.h"
#include "storage.h"
#include "usb.h"

#define INPUT_BUFFER_SIZE 128
#define SHELL_SESSION_COUNT 2u
#define EFI_CONVENTIONAL_MEMORY 7u
#define STATUSBAR_HEIGHT 20u
#define STATUSBAR_COLOR 0x0035063Eu
#define STATUSBAR_FONT_W 8u
#define STATUSBAR_TEXT_ROW 1u
#define STATUSBAR_RIGHT_PADDING_COLS 2u
#define STATUSBAR_MEM_LABEL_COLS 4u
#define STATUSBAR_MEM_VALUE_COLS 11u
#define STATUSBAR_CPU_LABEL_COLS 4u
#define STATUSBAR_CPU_VALUE_COLS 4u
#define STATUSBAR_SECTION_GAP_COLS 2u

static char input_buffers[SHELL_SESSION_COUNT][INPUT_BUFFER_SIZE];
static int input_prompt_active[SHELL_SESSION_COUNT];
static boot_info_t *kernel_boot_info;
static volatile unsigned int status_cpu_idle_depth;
static volatile unsigned long long status_cpu_idle_ticks;
static volatile unsigned long long status_cpu_idle_start_cycles;
static volatile unsigned long long status_cpu_idle_cycles;
static unsigned long long status_cpu_last_ticks[32];
static unsigned long long status_cpu_last_cycles[32];
static unsigned long long status_cpu_last_idle_cycles;
static unsigned long long status_cpu_last_busy_ticks[32];
static unsigned int status_cpu_cached_busy_percent[32];
static int status_cpu_sample_initialized[32];
static int statusbar_enabled;
static unsigned long long statusbar_last_update_tick;

typedef struct {
    unsigned int mem_label_col;
    unsigned int mem_value_col;
    unsigned int cpu_label_col;
    unsigned int cpu_value_col;
} statusbar_layout_t;

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} efi_memory_descriptor_t;

static void boot_stage(const char *name) {
    console_puts("[boot] ");
    console_puts(name);
    console_puts("\n");
}

static unsigned long long status_cpu_read_cycles(void) {
    uint32_t low;
    uint32_t high;

    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void memory_totals_kb(unsigned long long *total_kb, unsigned long long *free_kb) {
    unsigned long long total = 0;
    unsigned long long free = 0;

    if (kernel_boot_info &&
        kernel_boot_info->memory_map &&
        kernel_boot_info->memory_map_size &&
        kernel_boot_info->memory_map_descriptor_size) {
        uint8_t *map = (uint8_t *)(uintptr_t)kernel_boot_info->memory_map;
        uint64_t count = kernel_boot_info->memory_map_size / kernel_boot_info->memory_map_descriptor_size;

        for (uint64_t i = 0; i < count; ++i) {
            efi_memory_descriptor_t *desc =
                (efi_memory_descriptor_t *)(void *)(map + i * kernel_boot_info->memory_map_descriptor_size);
            unsigned long long kb = desc->number_of_pages * 4ull;

            total += kb;
            if (desc->type == EFI_CONVENTIONAL_MEMORY) {
                free += kb;
            }
        }
    }

    *total_kb = total;
    *free_kb = free;
}

unsigned long long status_memory_total_kb(void) {
    unsigned long long total = 0;
    unsigned long long free = 0;

    memory_totals_kb(&total, &free);
    (void)free;
    return total;
}

unsigned long long status_memory_free_kb(void) {
    unsigned long long total = 0;
    unsigned long long free = 0;

    if (kmem_total_pages() != 0) {
        return kmem_free_pages() * 4ull;
    }

    memory_totals_kb(&total, &free);
    (void)total;
    return free;
}

unsigned long long status_memory_used_kb(void) {
    unsigned long long total = 0;
    unsigned long long free = 0;

    memory_totals_kb(&total, &free);
    return total > free ? total - free : 0;
}

unsigned int status_cpu_core_count(void) {
    return cpu_online_core_count();
}

void status_cpu_enter_idle(void) {
    if (status_cpu_idle_depth == 0) {
        status_cpu_idle_start_cycles = status_cpu_read_cycles();
    }
    ++status_cpu_idle_depth;
}

void status_cpu_leave_idle(void) {
    if (status_cpu_idle_depth > 0) {
        if (status_cpu_idle_depth == 1) {
            unsigned long long now = status_cpu_read_cycles();
            status_cpu_idle_cycles += now - status_cpu_idle_start_cycles;
        }
        --status_cpu_idle_depth;
    }
}

void status_cpu_timer_tick(void) {
    if (status_cpu_idle_depth > 0) {
        ++status_cpu_idle_ticks;
    }
}

unsigned int status_cpu_usage_percent(unsigned int core) {
    unsigned long long current_ticks;
    unsigned long long current_cycles;
    unsigned long long current_idle_cycles;
    unsigned long long current_busy_ticks;
    unsigned long long delta_ticks;
    unsigned long long delta_cycles;
    unsigned long long delta_idle_cycles;
    unsigned long long delta_busy_ticks;

    if (core >= 32u || core >= status_cpu_core_count()) {
        return 0;
    }

    current_ticks = timer_ticks();
    if (status_cpu_sample_initialized[core] && current_ticks == status_cpu_last_ticks[core]) {
        return status_cpu_cached_busy_percent[core];
    }

    if (core == 0) {
        current_cycles = status_cpu_read_cycles();
        current_idle_cycles = status_cpu_idle_cycles;
        if (status_cpu_idle_depth > 0) {
            current_idle_cycles += current_cycles - status_cpu_idle_start_cycles;
        }

        if (!status_cpu_sample_initialized[core]) {
            delta_cycles = 0;
            delta_idle_cycles = 0;
            status_cpu_sample_initialized[core] = 1;
        } else {
            delta_cycles = current_cycles - status_cpu_last_cycles[core];
            delta_idle_cycles = current_idle_cycles - status_cpu_last_idle_cycles;
        }

        status_cpu_last_ticks[core] = current_ticks;
        status_cpu_last_cycles[core] = current_cycles;
        status_cpu_last_idle_cycles = current_idle_cycles;
        if (delta_cycles == 0 || delta_idle_cycles >= delta_cycles) {
            status_cpu_cached_busy_percent[core] = 0;
        } else {
            unsigned long long busy_cycles = delta_cycles - delta_idle_cycles;
            status_cpu_cached_busy_percent[core] =
                (unsigned int)((busy_cycles * 100ull + (delta_cycles / 2ull)) / delta_cycles);
            if (status_cpu_cached_busy_percent[core] > 100u) {
                status_cpu_cached_busy_percent[core] = 100u;
            }
        }
        return status_cpu_cached_busy_percent[core];
    }

    current_busy_ticks = cpu_core_busy_ticks(core);
    if (!status_cpu_sample_initialized[core]) {
        delta_ticks = current_ticks;
        delta_busy_ticks = current_busy_ticks;
        status_cpu_sample_initialized[core] = 1;
    } else {
        delta_ticks = current_ticks - status_cpu_last_ticks[core];
        delta_busy_ticks = current_busy_ticks - status_cpu_last_busy_ticks[core];
    }

    status_cpu_last_ticks[core] = current_ticks;
    status_cpu_last_busy_ticks[core] = current_busy_ticks;

    if (delta_ticks == 0) {
        status_cpu_cached_busy_percent[core] = 0;
    } else if (delta_busy_ticks >= delta_ticks) {
        status_cpu_cached_busy_percent[core] = 100;
    } else {
        status_cpu_cached_busy_percent[core] = (unsigned int)((delta_busy_ticks * 100ull) / delta_ticks);
    }
    return status_cpu_cached_busy_percent[core];
}

static void statusbar_put_label(unsigned int col, const char *text) {
    while (*text) {
        console_put_char_at_screen(col++, STATUSBAR_TEXT_ROW, *text++);
    }
}

static void statusbar_clear_field(unsigned int col, unsigned int width) {
    for (unsigned int i = 0; i < width; ++i) {
        console_put_char_at_screen(col + i, STATUSBAR_TEXT_ROW, ' ');
    }
}

static statusbar_layout_t statusbar_layout(void) {
    statusbar_layout_t layout = {0, 0, 0, 0};
    unsigned int width = kernel_boot_info ? kernel_boot_info->framebuffer_width : 0;
    unsigned int cols = width / STATUSBAR_FONT_W;
    unsigned int total_width = STATUSBAR_MEM_LABEL_COLS +
                               STATUSBAR_MEM_VALUE_COLS +
                               STATUSBAR_SECTION_GAP_COLS +
                               STATUSBAR_CPU_LABEL_COLS +
                               STATUSBAR_CPU_VALUE_COLS;
    unsigned int start_col;

    if (cols == 0) {
        return layout;
    }

    if (cols > total_width + STATUSBAR_RIGHT_PADDING_COLS) {
        start_col = cols - total_width - STATUSBAR_RIGHT_PADDING_COLS;
    } else if (cols > total_width) {
        start_col = cols - total_width;
    } else {
        start_col = 0;
    }

    layout.mem_label_col = start_col;
    layout.mem_value_col = layout.mem_label_col + STATUSBAR_MEM_LABEL_COLS;
    layout.cpu_label_col = layout.mem_value_col + STATUSBAR_MEM_VALUE_COLS + STATUSBAR_SECTION_GAP_COLS;
    layout.cpu_value_col = layout.cpu_label_col + STATUSBAR_CPU_LABEL_COLS;
    return layout;
}

static void statusbar_render_values(void) {
    statusbar_layout_t layout = statusbar_layout();

    statusbar_clear_field(layout.mem_value_col, STATUSBAR_MEM_VALUE_COLS);
    console_put_dec_at_screen(layout.mem_value_col, STATUSBAR_TEXT_ROW, status_memory_used_kb() / 1024ull);
    console_put_char_at_screen(layout.mem_value_col + 4, STATUSBAR_TEXT_ROW, '/');
    console_put_dec_at_screen(layout.mem_value_col + 5, STATUSBAR_TEXT_ROW, status_memory_total_kb() / 1024ull);
    console_put_char_at_screen(layout.mem_value_col + 10, STATUSBAR_TEXT_ROW, 'M');

    statusbar_clear_field(layout.cpu_value_col, STATUSBAR_CPU_VALUE_COLS);
    console_put_dec_at_screen(layout.cpu_value_col, STATUSBAR_TEXT_ROW, status_cpu_usage_percent(0));
    console_put_char_at_screen(layout.cpu_value_col + 3, STATUSBAR_TEXT_ROW, '%');
}

static void statusbar_render_frame(void) {
    statusbar_layout_t layout = statusbar_layout();
    unsigned int width = kernel_boot_info ? kernel_boot_info->framebuffer_width : 0;

    for (unsigned int y = 0; y < STATUSBAR_HEIGHT; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            put_pixel(x, y, STATUSBAR_COLOR);
        }
    }

    statusbar_put_label(layout.mem_label_col, "MEM ");
    statusbar_put_label(layout.cpu_label_col, "CPU ");
    statusbar_render_values();
}

void statusbar_enable(void) {
    statusbar_enabled = 1;
    statusbar_last_update_tick = timer_ticks();
    statusbar_render_frame();
}

void statusbar_update_if_due(void) {
    unsigned long long now;
    unsigned int hz;

    if (!statusbar_enabled) {
        return;
    }

    now = timer_ticks();
    hz = timer_frequency();
    if (hz == 0) {
        hz = 100u;
    }

    if (now - statusbar_last_update_tick >= hz) {
        statusbar_last_update_tick = now;
        statusbar_render_values();
    }
}

void kernel_main(boot_info_t *info) {
    if (!info) {
        console_panic("kernel_main got null boot_info");
    }

    if (!info->framebuffer_base) {
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    kernel_boot_info = info;

    graphics_init(info->framebuffer_base,
                  info->framebuffer_width,
                  info->framebuffer_height,
                  info->framebuffer_pixels_per_scanline,
                  info->framebuffer_format);

    console_init(info->framebuffer_base,
                 info->framebuffer_width,
                 info->framebuffer_height,
                 info->framebuffer_pixels_per_scanline);

    console_clear();
    console_cursor_enable(0);
    boot_stage("console ready");
    boot_stage("cpu tables");
    cpu_init_tables();
    boot_stage("cpu topology");
    cpu_detect_topology(info);
    boot_stage("secondary CPUs");
    cpu_start_secondary_cores();
    boot_stage("interrupt vectors");
    interrupts_init();
    boot_stage("timer");
    timer_init();
    boot_stage("clock");
    clock_init();
    boot_stage("local APIC timer");
    cpu_enable_lapic_timer(timer_frequency());
    boot_stage("dma");
    dma_init(info);
    boot_stage("heap");
    kmem_init(info);
    boot_stage("keyboard");
    keyboard_init();
    boot_stage("mouse");
    mouse_init();
    boot_stage("storage");
    storage_init();
    shell_init();
    shell_mount_first_lainfs('S');
    shell_boot_mode_load();
    if (shell_boot_usb_safe_mode_enabled()) {
        boot_stage("usb skipped by safe mode");
    } else {
        boot_stage("usb");
        usb_init();
    }
    boot_stage("network");
    net_init();

    console_puts("Lain kernel says hi from C :3\n");
    console_kprintf2("Kernel base: 0x%x, framebuffer: 0x%x\n",
                     info->kernel_base,
                     info->framebuffer_base);
    console_kprintf2("Resolution: %u x %u\n",
                     info->framebuffer_width,
                     info->framebuffer_height);
    console_kprintf1("Memory map bytes: %u\n", info->memory_map_size);
    console_kprintf2("CPU cores online/detected: %u/%u\n",
                     cpu_online_core_count(),
                     cpu_core_count());
    console_puts(shell_boot_usb_safe_mode_enabled()
                     ? "GDT, IDT, timer, PS/2 keyboard/mouse, storage, USB skipped, and network scan loaded.\n"
                     : "GDT, IDT, timer, PS/2 keyboard/mouse, storage, USB scan, and network scan loaded.\n");
    console_puts("\nHave fun hacking on it.\n");

    registry_init();
    shell_registry_load();
    registry_set_save_hook(shell_registry_save);
    shell_registry_save();
    if (shell_boot_debug_mode_enabled()) {
        console_puts("[boot] debug diagnostics mode active\n");
    }
    if (shell_boot_safe_mode_enabled()) {
        console_puts("[boot] safe mode: skipping autoexec\n");
    } else {
        shell_run_autoexec("autoexec", info);
    }

    for (;;) {
        unsigned int session = console_active_pane();

        statusbar_update_if_due();
        shell_modules_tick();
        (void)kernel_task_poll();
        if (session >= SHELL_SESSION_COUNT) {
            session = 0;
        }

        shell_set_session(session);
        if (!input_prompt_active[session]) {
            shell_print_prompt();
            input_prompt_active[session] = 1;
        }

        {
            int read_status = console_read_line(input_buffers[session], INPUT_BUFFER_SIZE);

            if (read_status == 3) {
                input_buffers[0][0] = '\0';
                input_buffers[1][0] = '\0';
                input_prompt_active[0] = 0;
                input_prompt_active[1] = 0;
                continue;
            }

            if (read_status == 2) {
                input_buffers[1][0] = '\0';
                input_prompt_active[1] = 0;
                continue;
            }

            if (read_status != 0) {
                continue;
            }
        }

        input_prompt_active[session] = 0;

        if (input_buffers[session][0] == '\0') {
            console_puts("\n");
            continue;
        }

        shell_run_command(input_buffers[session], info);
        input_buffers[session][0] = '\0';
        statusbar_update_if_due();

    }
}
