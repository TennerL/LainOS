BITS 64
DEFAULT REL

GLOBAL cpu_load_gdt_and_segments
GLOBAL cpu_load_idt
GLOBAL ap_trampoline_start
GLOBAL ap_trampoline_end
GLOBAL ap_trampoline_cr3
GLOBAL ap_trampoline_stack
GLOBAL ap_trampoline_entry

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

; Copied by the BSP to 0x8000 and entered by APs through SIPI vector 0x08.
; Keep every absolute reference anchored to AP_TRAMPOLINE_BASE.
%define AP_TRAMPOLINE_BASE 0x8000

align 16
ap_trampoline_start:
BITS 16
    cli
    xor ax, ax
    mov ds, ax
    lgdt [AP_TRAMPOLINE_BASE + ap_trampoline_gdtr - ap_trampoline_start]

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp 0x08:(AP_TRAMPOLINE_BASE + ap_trampoline_protected - ap_trampoline_start)

BITS 32
ap_trampoline_protected:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    mov eax, [AP_TRAMPOLINE_BASE + ap_trampoline_cr3 - ap_trampoline_start]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp 0x10:(AP_TRAMPOLINE_BASE + ap_trampoline_long - ap_trampoline_start)

BITS 64
ap_trampoline_long:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [abs AP_TRAMPOLINE_BASE + ap_trampoline_stack - ap_trampoline_start]
    xor rbp, rbp
    mov rax, [abs AP_TRAMPOLINE_BASE + ap_trampoline_entry - ap_trampoline_start]
    call rax

.halt:
    cli
    hlt
    jmp .halt

align 8
ap_trampoline_cr3:
    dq 0
ap_trampoline_stack:
    dq 0
ap_trampoline_entry:
    dq 0

align 8
ap_trampoline_gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
ap_trampoline_gdtr:
    dw ap_trampoline_gdtr - ap_trampoline_gdt - 1
    dd AP_TRAMPOLINE_BASE + ap_trampoline_gdt - ap_trampoline_start
ap_trampoline_end:
BITS 64

SECTION .note.GNU-stack noalloc noexec nowrite progbits
