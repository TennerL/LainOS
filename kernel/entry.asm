BITS 64
DEFAULT REL
GLOBAL _start

%include "bootinfo.inc"

EXTERN kernel_main
EXTERN console_panic

%define KERNEL_STACK_SIZE (256 * 1024)

SECTION .text
_start:
    cli
    lea rsp, [rel kernel_stack_top]
    xor rbp, rbp

    mov rbx, rdi
    test rbx, rbx
    jz .panic_no_bootinfo

    mov rax, [rbx + boot_info.magic]
    mov rdx, BOOTINFO_MAGIC
    cmp rax, rdx
    jne .panic_bad_magic

    mov rdi, rbx
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

.panic_no_bootinfo:
    lea rsi, [rel panic_no_bootinfo]
    jmp .panic

.panic_bad_magic:
    lea rsi, [rel panic_bad_magic]

.panic:
    call console_panic

SECTION .bss
align 16
kernel_stack_bottom:
    resb KERNEL_STACK_SIZE
kernel_stack_top:

SECTION .rodata
panic_no_bootinfo db 'boot_info pointer was null', 0
panic_bad_magic db 'boot_info magic mismatch', 0

SECTION .note.GNU-stack noalloc noexec nowrite progbits
