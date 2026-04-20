#include "kernel.h"
#include "shell.h"
#include "storage.h"

#define INPUT_BUFFER_SIZE 128

static char input_buffer[INPUT_BUFFER_SIZE];

void kernel_main(boot_info_t *info) {
    if (!info) {
        console_panic("kernel_main got null boot_info");
    }

    if (!info->framebuffer_base) {
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    console_init(info->framebuffer_base,
                 info->framebuffer_width,
                 info->framebuffer_height,
                 info->framebuffer_pixels_per_scanline);

    console_clear();
    cpu_init_tables();
    interrupts_init();
    timer_init();
    keyboard_init();
    storage_init();

    console_puts("Lain kernel says hi from C :3\n");
    console_kprintf2("Kernel base: 0x%x, framebuffer: 0x%x\n",
                     info->kernel_base,
                     info->framebuffer_base);
    console_kprintf2("Resolution: %u x %u\n",
                     info->framebuffer_width,
                     info->framebuffer_height);
    console_kprintf1("Memory map bytes: %u\n", info->memory_map_size);
    console_puts("GDT, IDT, timer, PS/2 keyboard, and storage loaded.\n");
    console_puts("\nHave fun hacking on it.\n");

    shell_init();

    for (;;) {
        shell_print_prompt();
        console_read_line(input_buffer, INPUT_BUFFER_SIZE);

        if (input_buffer[0] == '\0') {
            console_puts("\n");
            continue;
        }

        shell_run_command(input_buffer, info);

    }
}
