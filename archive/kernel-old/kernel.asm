BITS 64
DEFAULT REL
GLOBAL _start

%include "boot/shared/bootinfo.inc"

EXTERN console_init
EXTERN console_clear
EXTERN console_puts
EXTERN console_newline
EXTERN console_kprintf1
EXTERN console_kprintf2
EXTERN console_panic
EXTERN console_read_line
EXTERN cpu_init_tables
EXTERN interrupts_init

SECTION .text

_start:
    cli

    mov rbx, rdi
    test rbx, rbx
    jz panic_no_bootinfo_path

    mov rax, [rbx + boot_info.magic]
    mov rdx, BOOTINFO_MAGIC
    cmp rax, rdx
    jne panic_bad_magic_path

    mov rdi, [rbx + boot_info.framebuffer_base]
    mov esi, [rbx + boot_info.framebuffer_width]
    mov edx, [rbx + boot_info.framebuffer_height]
    mov ecx, [rbx + boot_info.framebuffer_pixels_per_scanline]
    call console_init

    test rdi, rdi
    jz panic_no_framebuffer_path

    call console_clear
    call cpu_init_tables
    call interrupts_init

    lea rsi, [rel msg_title]
    call console_puts
    lea rsi, [rel msg_line1]
    mov rdx, [rbx + boot_info.kernel_base]
    mov rcx, [rbx + boot_info.framebuffer_base]
    call console_kprintf2
    lea rsi, [rel msg_line2]
    mov edx, [rbx + boot_info.framebuffer_width]
    mov ecx, [rbx + boot_info.framebuffer_height]
    call console_kprintf2
    lea rsi, [rel msg_line3]
    mov rdx, [rbx + boot_info.memory_map_size]
    call console_kprintf1
    lea rsi, [rel msg_line4]
    call console_puts
    lea rsi, [rel msg_line5]
    call console_puts

shell_loop:
    lea rsi, [rel prompt]
    call console_puts

    lea rdi, [rel input_buffer]
    mov ecx, INPUT_BUFFER_SIZE
    call console_read_line

    lea rsi, [rel you_typed]
    call console_puts

    lea rsi, [rel input_buffer]
    call console_puts

    lea rsi, [rel newline]
    call console_puts

    jmp shell_loop

.hang:
    hlt
    jmp .hang

panic_no_bootinfo_path:
    lea rsi, [rel panic_no_bootinfo]
    jmp panic_path

panic_bad_magic_path:
    lea rsi, [rel panic_bad_magic]
    jmp panic_path

panic_no_framebuffer_path:
    lea rsi, [rel panic_no_framebuffer]

panic_path:
    call console_panic

SECTION .rodata
INPUT_BUFFER_SIZE equ 128

msg_title db 'Lain kernel says hi :3', 10, 0
msg_line1 db 10,'Kernel base: 0x%x, framebuffer: 0x%x', 10, 0
msg_line2 db 'Resolution: %u x %u', 10, 0
msg_line3 db 'Memory map bytes: %u', 10, 0
msg_line4 db 'GDT, IDT, and exception stubs loaded.', 10, 0
msg_line5 db 10, 'Have fun hacking on it.', 10, 0

prompt       db '> ', 0
you_typed    db 'You typed: ', 0
newline      db 10, 0

panic_no_bootinfo db 'boot_info pointer was null', 0
panic_bad_magic db 'boot_info magic mismatch', 0
panic_no_framebuffer db 'framebuffer base was null', 0

SECTION .bss
input_buffer resb INPUT_BUFFER_SIZE
