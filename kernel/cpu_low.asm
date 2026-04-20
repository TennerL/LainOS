BITS 64
DEFAULT REL

GLOBAL cpu_load_gdt_and_segments
GLOBAL cpu_load_idt

SECTION .text
cpu_load_gdt_and_segments:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rax, [rel .reload_cs]
    push qword 0x08
    push rax
    retfq
.reload_cs:
    ret

cpu_load_idt:
    lidt [rdi]
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
