BITS 64
DEFAULT REL

GLOBAL cpu_init_tables
EXTERN isr_0
EXTERN isr_1
EXTERN isr_2
EXTERN isr_3
EXTERN isr_4
EXTERN isr_5
EXTERN isr_6
EXTERN isr_7
EXTERN isr_8
EXTERN isr_9
EXTERN isr_10
EXTERN isr_11
EXTERN isr_12
EXTERN isr_13
EXTERN isr_14
EXTERN isr_15
EXTERN isr_16
EXTERN isr_17
EXTERN isr_18
EXTERN isr_19
EXTERN isr_20
EXTERN isr_21
EXTERN isr_22
EXTERN isr_23
EXTERN isr_24
EXTERN isr_25
EXTERN isr_26
EXTERN isr_27
EXTERN isr_28
EXTERN isr_29
EXTERN isr_30
EXTERN isr_31

%macro SET_IDT_GATE 2
    mov rax, %2
    mov word  [idt64 + (%1 * 16) + 0], ax
    mov word  [idt64 + (%1 * 16) + 2], 0x08
    mov byte  [idt64 + (%1 * 16) + 4], 0
    mov byte  [idt64 + (%1 * 16) + 5], 0x8E
    shr rax, 16
    mov word  [idt64 + (%1 * 16) + 6], ax
    shr rax, 16
    mov dword [idt64 + (%1 * 16) + 8], eax
    mov dword [idt64 + (%1 * 16) + 12], 0
%endmacro

SECTION .text
cpu_init_tables:
    lgdt [rel gdt64.pointer]

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
    SET_IDT_GATE 0, isr_0
    SET_IDT_GATE 1, isr_1
    SET_IDT_GATE 2, isr_2
    SET_IDT_GATE 3, isr_3
    SET_IDT_GATE 4, isr_4
    SET_IDT_GATE 5, isr_5
    SET_IDT_GATE 6, isr_6
    SET_IDT_GATE 7, isr_7
    SET_IDT_GATE 8, isr_8
    SET_IDT_GATE 9, isr_9
    SET_IDT_GATE 10, isr_10
    SET_IDT_GATE 11, isr_11
    SET_IDT_GATE 12, isr_12
    SET_IDT_GATE 13, isr_13
    SET_IDT_GATE 14, isr_14
    SET_IDT_GATE 15, isr_15
    SET_IDT_GATE 16, isr_16
    SET_IDT_GATE 17, isr_17
    SET_IDT_GATE 18, isr_18
    SET_IDT_GATE 19, isr_19
    SET_IDT_GATE 20, isr_20
    SET_IDT_GATE 21, isr_21
    SET_IDT_GATE 22, isr_22
    SET_IDT_GATE 23, isr_23
    SET_IDT_GATE 24, isr_24
    SET_IDT_GATE 25, isr_25
    SET_IDT_GATE 26, isr_26
    SET_IDT_GATE 27, isr_27
    SET_IDT_GATE 28, isr_28
    SET_IDT_GATE 29, isr_29
    SET_IDT_GATE 30, isr_30
    SET_IDT_GATE 31, isr_31

    lidt [rel idt64.pointer]
    ret

SECTION .data
align 8
gdt64:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
.pointer:
    dw gdt64_end - gdt64 - 1
    dq gdt64

gdt64_end:

align 16
idt64:
    times 256 dq 0, 0
.pointer:
    dw idt64_end - idt64 - 1
    dq idt64

idt64_end:
