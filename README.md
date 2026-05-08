# UEFI x86_64 LainOS

Minimal starter project for:
- a **UEFI bootloader** in C
- a **64 bit kernel** in NASM
- loading an **ELF64 kernel** from the EFI filesystem
- collecting boot information
- calling `ExitBootServices`
- jumping into 64 bit kernel code with a boot info pointer
- drawing visible text through a tiny framebuffer console

## Project layout

- `bootloader/main.c` - UEFI loader entry point and ELF64 loader
- `kernel/include/` - shared kernel-side API declarations
- `kernel/arch/x86_64/` - entry, interrupt stubs, and low-level CPU helpers
- `kernel/core/` - kernel entry path, CPU tables, timer, and export table
- `kernel/drivers/` - framebuffer, console, PS/2 input, PCI, AHCI, USB, storage
- `kernel/fs/` - lainfs filesystem support
- `kernel/ui/` - shell, editor, browser, and desktop UI
- `kernel/z/` - in-kernel assembler, `.Z` compiler/object support, and `.Z` probe
- `kernel/linker.ld` - kernel linker script
- `archive/kernel-old/` - superseded ASM implementations kept for reference
- `Makefile` - build and run helpers

## GNU-EFI build flow

The bootloader now uses a more canonical GNU-EFI link path:
- `crt0-efi-x86_64.o`
- `elf_x86_64_efi.lds`
- `libefi.a`
- `libgnuefi.a`
- PE/COFF conversion via `objcopy --target efi-app-x86_64`

On the target system these default to:
- `EFI_CRT0=/usr/lib/crt0-efi-x86_64.o`
- `EFI_LDS=/usr/lib/elf_x86_64_efi.lds`
- `EFI_LIBDIR=/usr/lib`

This is still GNU-EFI, just using the distro-provided startup object and linker flow instead of the looser earlier setup.

## What it does

1. UEFI launches `BOOTX64.EFI`
2. The bootloader optionally tries to start `\\EFI\\BOOT\\drivers\\ntfs_x64.efi`
3. The bootloader scans UEFI-visible filesystems and opens `\\kernel.elf`
4. The loader parses the ELF64 header and program headers
5. Every `PT_LOAD` segment is copied to its target physical address
6. A `boot_info` structure is allocated by UEFI and passed to the kernel by its real address
7. The loader collects:
   - memory map
   - framebuffer info via GOP
   - ACPI RSDP pointer
8. The loader retries `GetMemoryMap` and `ExitBootServices` carefully until it gets a stable map key
9. The loader jumps to the ELF entry point and passes `boot_info*`
10. The kernel clears the framebuffer and renders a few status lines in software

## Boot protocol

Current kernel entry convention:

```c
void kernel_main(struct boot_info* info);
```

After the loader exits boot services, it jumps to the kernel using the SysV x86_64 calling convention and passes `boot_info*` in `RDI`.

Current `boot_info` fields:
- magic
- kernel_base
- kernel_size
- memory_map
- memory_map_size
- memory_map_descriptor_size
- memory_map_descriptor_version
- framebuffer_base
- framebuffer_width
- framebuffer_height
- framebuffer_pixels_per_scanline
- framebuffer_format
- rsdp

Magic value:

```c
0x4C41494E424F4F54ULL
```

## Tiny framebuffer graphics and console

The kernel now includes a very small framebuffer graphics driver and software
text renderer:
- fixed 8x8 bitmap glyphs
- contiguous ASCII font table for readable output
- GOP framebuffer setup with RGB/BGR pixel packing
- direct pixel writes plus filled rectangles, stroked rectangles, and lines
- restored darker blue-toned background
- configurable foreground text
- line wrapping
- scrolling
- simple 32 bit and 64 bit hex output support
- decimal output support
- basic tab and backspace handling
- tiny `kprintf`-style formatting helpers
- panic screen support
- early GDT/IDT setup groundwork
- basic exception stubs and fault panic path

Right now it is intentionally simple:
- formatter currently supports only a small subset like `%s`, `%x`, `%u`, `%c`
- no color escape support
- assumes a 32 bit linear framebuffer
- supports the common UEFI RGB/BGR layouts, but not arbitrary GOP bitmasks yet
- panic path currently depends on console initialization succeeding

## Requirements

On Debian/Ubuntu-like systems you typically need:

```bash
sudo apt install gcc make nasm binutils qemu-system-x86 ovmf gnu-efi dosfstools mtools gdisk xorriso
```

For a bootable UEFI ISO, install one ISO builder as well:

```bash
sudo apt install xorriso
```

On macOS, install `xorriso` with Homebrew:

```bash
brew install xorriso
```

If your distro uses different GNU-EFI paths, you can override them:

```bash
make EFI_INC=/usr/include/efi EFI_LIBDIR=/usr/lib EFI_CRT0=/usr/lib/crt0-efi-x86_64.o EFI_LDS=/usr/lib/elf_x86_64_efi.lds
```

To inspect the currently configured EFI paths:

```bash
make print-efi-config
```

## Shared boot protocol

The boot protocol now lives in shared files so the C bootloader and NASM kernel stay synchronized:
- `boot/shared/bootinfo.h`
- `boot/shared/bootinfo.inc`

That reduces the chance of silent ABI drift while the project grows.

## Build

```bash
make clean
make
```

To request a boot-time GOP framebuffer resolution, rebuild with:

```bash
make BOOT_RES_WIDTH=1024 BOOT_RES_HEIGHT=768
```

If firmware does not expose that exact RGB/BGR mode, the loader prints a message
and keeps the firmware default. Resolution changes happen in the UEFI loader
before `ExitBootServices`; the kernel receives the selected mode through
`boot_info`.

You can also set the next boot's requested mode from inside the OS after
mounting a lainfs drive:

```text
resolution 1024 768
```

This writes `bootres.cfg` at the lainfs root. On the next boot, the UEFI loader
scans lainfs block devices and MBR partitions for that file, then applies the
requested GOP mode if firmware exposes it.

This creates:
- `build/bootloader.so`
- `build/BOOTX64.EFI`
- `build/kernel.elf`
- `build/kernel.bin`
- `build/image/EFI/BOOT/BOOTX64.EFI`
- `build/image/kernel.elf`
- `build/esp.img`
- `build/bootdisk.img`
- `build/bootdisk-gpt.img`
- `build/data.img`
- `build/boot.iso`

`kernel.bin` is still produced as a convenience artifact for inspection, but the UEFI loader now boots from `kernel.elf`.
`esp.img` is a raw FAT32 ESP image that is convenient for QEMU. `bootdisk.img`
is now a USB-style MBR disk with one FAT32 partition for broader real-firmware
compatibility when you flash it to removable media. `bootdisk-gpt.img` keeps the
older GPT-wrapped layout for VMs or firmware that prefers GPT. `boot.iso` is a
UEFI optical image that embeds the ESP as an El Torito boot image, which is the
right format for VMs and firmware expecting an ISO instead of a raw disk image.

## Verify the EFI image

Before copying to Windows, inspect the generated EFI binary:

```bash
make inspect-efi
```

You want to see sane PE fields, especially:
- non-zero `SectionAlignment`
- non-zero `FileAlignment`
- `Subsystem` set to EFI application, not `00000000`

## Run in QEMU on Linux

```bash
make run
```

This boots from the raw FAT32 ESP image.

To boot the default USB-style disk image with the persistent data disk attached through AHCI:

```bash
make run-bootdisk
```

To boot the GPT variant instead:

```bash
make run-bootdisk-gpt
```

To build and boot the ISO form:

```bash
make run-iso
```

## Optional UEFI NTFS driver

The bootloader can now start an optional UEFI NTFS driver before looking for
`\\kernel.elf`. This does not make NTFS universal by itself; it lets a driver
publish NTFS volumes through UEFI's normal `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`.
After that, the loader scans all UEFI-visible filesystems and opens the first
`\\kernel.elf` it finds.

Build with a driver copied into the ESP image like this:

```bash
make NTFS_DRIVER=/path/to/ntfs_x64.efi
```

The driver is placed at:

```text
\\EFI\\BOOT\\drivers\\ntfs_x64.efi
```

If the driver is missing or fails to start, the loader prints a notice and
continues with normal FAT32 boot. This is still bootloader-time support only;
after `ExitBootServices`, the kernel will need its own storage and filesystem
drivers for long-term NTFS access.

## Run in QEMU on Windows

After rebuilding and copying the new artifacts to Windows, boot the ESP image directly:

```powershell
qemu-system-x86_64 `
  -m 256M `
  -smp 4 `
  -drive if=pflash,format=raw,readonly=on,file="C:/Users/j.klaus/Downloads/OVMF_CODE_4M.fd" `
  -drive if=pflash,format=raw,file="C:/Users/j.klaus/Downloads/OVMF_VARS_4M.fd" `
  -drive format=raw,file="C:/Users/j.klaus/Desktop/uefi-starter/build/esp.img" `
  -boot menu=on
```

Use matching OVMF images, for example `CODE_4M` with `VARS_4M`.

## Run in VirtualBox

Build the raw images:

```bash
make
```

This creates:
- `build/bootdisk.img` - MBR boot disk with one FAT32 EFI partition for flashing to USB media
- `build/bootdisk-gpt.img` - GPT boot disk with a FAT32 EFI System Partition
- `build/data.img` - persistent data disk with an MBR partition for `lainfs`

Convert them to VDI for VirtualBox:

```bash
VBoxManage convertfromraw build/bootdisk.img build/bootdisk.vdi --format VDI
VBoxManage convertfromraw build/data.img build/data.vdi --format VDI
```

Create a 64-bit VM and enable EFI:
- System -> Motherboard -> Enable EFI
- Memory: 256 MiB or more
- Boot order: Hard Disk first

Attach storage like this:
- SATA Controller, AHCI enabled
- Port 0: `build/bootdisk.vdi`
- Port 1: `build/data.vdi`

On boot, the firmware should load:

```text
\EFI\BOOT\BOOTX64.EFI
```

Once the kernel console appears, check storage:

```text
ahci
usb
blk
part
```

If AHCI works, the data disk should appear as `sd0` or `sd1`. Mount whichever
partition appears in `part`, for example:

```text
mount C: sd0p1
format C:
write hello hello-from-virtualbox
ls
cat hello
```

After rebooting the VM, do not format again. Remount and read:

```text
mount C: sd0p1
C:
ls
cat hello
```

If VirtualBox does not boot `bootdisk.vdi`, open the VM's EFI boot manager and
select the disk entry or `EFI/BOOT/BOOTX64.EFI`. If the VM boots but `ahci`
reports zero disks, keep the boot disk on SATA port 0 and the data disk on SATA
port 1, then retry; the AHCI driver is still intentionally small.

## ELF loading model

This loader currently handles a simple static ELF64 kernel by:
- validating the ELF magic and x86_64 class
- iterating over `PT_LOAD` program headers
- allocating pages covering the loadable physical address range
- copying file-backed bytes into each segment
- zeroing the remaining `p_memsz - p_filesz` tail
- jumping to `e_entry`

That is the right basic shape for a real kernel loader.

## Important caveats

This is now a clean milestone-1 starter: UEFI boot, ELF64 loading, boot info handoff, a C kernel main path, a C console path, visible kernel output, mostly-C descriptor-table setup, and minimal PS/2 keyboard plus mouse input paths all work.

Current keyboard input assumes classic QEMU PS/2 keyboard behavior and simple set-1 scancodes.
It now includes basic Shift and Caps Lock handling plus a simple visible text cursor during line input.
Keyboard layouts can be switched with `keymap us` or `keymap de`.
The German layout covers the ASCII keys needed for shell and `.Z` work,
including AltGr combinations for `@`, `{}`, `[]`, `\`, `~`, and `|`.
While typing at the shell prompt, `Ctrl+W` enables a two-pane split console and
then switches focus between the left and right pane. Each pane keeps its own
shell session state, including the selected drive and current working directory.
While focused in the right pane, `Ctrl+E` closes that pane and returns to the
main shell prompt on the left.

## Shell commands

The kernel console has a tiny command shell:
- `help` - show commands
- `bgcolor 0xRRGGBB` - set the background color
- `fgcolor 0xRRGGBB` - set the text color
- `resolution [width height]` - show the current framebuffer mode or write a
  next-boot `bootres.cfg` request to lainfs
- `keymap us|de` - set the keyboard layout; put `keymap de` in `autoexec` to apply it at boot
- `clear` - clear the screen
- `echo text` - print text
- `info` - show boot/kernel info
- `cpus` - show ACPI MADT CPU topology and local APIC ids
- `mkdrive C:` - create a virtual `C:` drive
- `drives` - list virtual drives
- `C:` - switch to an existing virtual `C:` drive
- `blk` - list block devices
- `part` - list discovered MBR or GPT partitions, including NTFS volumes when detected
- `mount C: rd0p1` - mount a partition at a drive letter
- `mounts` - list mounted filesystems
- `ahci` - show detected AHCI controllers and disks
- `usb` - show detected USB host controllers
- `desktop` - enter the framebuffer desktop UI; press `Esc`/`Ctrl+Q` or hold
  left+right mouse buttons to return. A real Terminal window opens on the
  desktop and runs shell commands directly; drag its title bar to move it,
  resize from the bottom-right corner, and use its close box to hide it. Drag
  and resize use a lightweight outline preview, and terminal text is repainted
  from a backing buffer after geometry changes. The `Terminal` launcher brings
  it back. Click `Files` or open the Start menu for a native file browser with
  clickable directories and an Up button. The Start menu also lists resident
  modules. Clicking a resident module opens a draggable module window; while
  it ticks, framebuffer drawing is clipped and translated into that window's
  content area. In desktop mode, `edit name.Z` opens a native syntax-highlighted
  editor window; `Ctrl+S` saves and `Esc`/`Ctrl+Q` closes it.
  Put it at the end of `autoexec` to boot straight into the UI.
- `ticks` - show PIT timer ticks
- `format C:` - format a mounted drive as `lainfs`
- `format hd1p1` - format a discovered partition without mounting it first
- `format hd1` - create a single `lainfs` partition on a raw disk and format it
- `ls [C:]` - list `lainfs` entries in a table
- `cd name` / `cd ..` / `cd \` - change directory
- `pwd` - show the current drive and directory
- `mkdir name` - create a directory entry
- `rm name` / `del name` - delete a file or directory entry
- `rename old new` / `mv old new` - rename or move a file or directory entry
- `cp source dest` - copy a file; paths may include mounted drive prefixes,
  such as `cp R:/examples/hwinfo.Z S:/examples`
- `write name text` - write a text file to the current drive
- `cat name` - print a text file from the current drive
- `browse [path-or-drive:]` - open a two-pane browser; each pane can use a
  different mounted drive, `C` copies the selected file to the other pane, and
  `M` moves within a drive or moves a file across drives
- `asm source.asm output.bin` - assemble a tiny x86_64 source file
- `zc source.Z output.bin` - compile a tiny `.Z` source file into a flat binary
- `zco source.Z output.zo` - compile a tiny `.Z` source file into a `.zo` object
- `zlink input.zo [more.zo ...] output.bin` - link `.zo` objects into a flat binary
- `zmod input.zo [more.zo ...]` - link `.zo` objects in memory and run the module initializer
- `zunload module` - unload a resident `.zo` module by name and call
  `zmodule_unload` if the module exports it
- `zreload module` - unload an existing resident module by name, then run
  `zmod module`
- `zmods` - list resident `.zo` modules and their exported symbols
- `zrun source.Z` - compile and run a tiny `.Z` source file directly
- `zasm source.Z [output.asm]` - print or save the generated asm for a `.Z` source file
- `exec file.bin` - run a flat binary from the current drive

The storage stack currently registers a small RAM-backed demo block device
named `rd0`. Its first sector contains a simple MBR with one NTFS-like partition
named `rd0p1`, so you can exercise the real block-device, partition-discovery,
and mount-table paths from the console:

```text
blk
part
mount C: rd0p1
mounts
C:
```

The kernel auto-formats that RAM-backed `rd0p1` partition as `lainfs` and
seeds it with the `examples/` tree. If no real writable HDD or AHCI disk is
attached, it mounts the live workspace as `S:`. If a real lainfs disk is
available and mounted as `S:`, the seeded live workspace is still mounted as
`R:` so the small self-hosting demos are always available.

The kernel also probes storage in two hardware-facing ways:
- legacy primary-slave ATA/IDE, which appears as `hd1`
- PCI SATA/AHCI, which appears as `sd0`, `sd1`, and so on

The build creates `build/data.img` once, gives it an MBR partition, and attaches
it to QEMU. Normal rebuilds leave that image intact, so files written inside the
kernel persist across reboots until you delete `build/data.img` or run
`make clean`.

Persistent `lainfs` workflow:

```text
blk
part
mount C: hd1p1
format C:
write hello hello-from-disk
ls
cat hello
```

Then reboot with `make run`, mount it again, and the file should still be there:

```text
mount C: hd1p1
C:
ls
cat hello
```

To test the AHCI path in QEMU instead of the legacy IDE path:

```bash
make run-ahci
```

To exercise the early xHCI path with a USB keyboard and tablet attached:

```bash
make run-usb
```

Then use the same flow with the AHCI partition name:

```text
ahci
blk
part
mount C: sd0p1
format C:
write hello hello-from-ahci
ls
cat hello
```

`lainfs` is deliberately tiny right now: up to 32 entries total and up to
64 KiB per file, stored as contiguous 512-byte blocks. Directory entries have
parent links, so `cd`, `pwd`, scoped `ls`, and moving entries into directories
work. Full path operands such as `programs/demo.asm` are not implemented yet;
change into the directory first or use `mv file dirname` / `mv file ..`. The
AHCI driver is intentionally early but follows the block-device API, so
VirtualBox SATA disks should be the next realistic target to shake out.

## Tiny assembler

The `asm` shell command accepts a small NASM-like subset and emits a flat binary
that can be run with `exec`. Directives such as `global`, `section .text`,
`bits 64`, and `default rel` are accepted for source compatibility. Labels,
NASM-style data labels, and `db` / `dq` data are supported.

The instruction subset is intentionally small but covers the common flat-binary
building blocks:
- registers: `rax`..`r15` and `eax`..`r15d`
- control: `nop`, `hlt`, `ret`, `ret imm16`, `leave`, `int imm8`, `iretq`,
  `syscall`, `cli`, `sti`, `cqo`, `idiv`
- data movement: `mov r64, imm64`, `mov r32, imm32`, `mov reg, reg`,
  `mov r64, [label]`, `mov r64, [rbp-8]`, `mov [label], r64`,
  `mov [rbp-8], r64`, `mov qword [label], imm32`, `mov qword [rbp-8], imm32`,
  sized memory forms such as `movzx eax, byte [rbp-1]`, `movsx rax, word [rbp-3]`,
  `mov eax, dword [rbp-7]`, `mov byte [rbp-1], al`, `mov word [rbp-3], ax`,
  and `mov dword [rbp-7], eax`,
  `push r64`, `pop r64`
- arithmetic and logic: `add`, `sub`, `imul`, `cmp`, `and`, `or`, `xor`,
  `test`, `inc`, `dec`; `add/sub/cmp/and/or/xor/test` support `r64, [label]`;
  `imul` supports `imul reg, reg`, `imul reg, imm32`, and `imul reg, reg, imm32`
- flow: `call label`, `call r64`, `jmp label`, `jmp r64`, and rel32
  conditional jumps such as `je`, `jne`, `jl`, `jle`, `jg`, `jge`, `jb`,
  `jbe`, `ja`, and `jae`
- data directives: `db` accepts decimal/hex bytes and quoted strings with
  escapes such as `\n`, `\r`, `\t`, `\0`, `\"`, `\\`; `dq` accepts numbers,
  labels, and exposed kernel symbols
- exposed kernel symbols come from `kernel/kernel_exports.c` and include `puts`, `put_hex64`,
  `put_dec64`, `ticks`, `mem_total_kb`, `mem_free_kb`, `mem_used_kb`,
  `cpu_count`, `cpu_usage`, `put_pixel`, `put_char_at`, `put_dec_at`,
  `put_char_at_screen`, `put_dec_at_screen`, `set_margin`, and
  `statusbar_enable`

Memory operands currently support:
- RIP-relative label forms such as `[counter]` and sized forms such as
  `byte [counter]`, `word [counter]`, `dword [counter]`, `qword [counter]`
- simple base-plus-displacement forms such as `[rbp-8]`, `[rsp+16]`, and `[rax]`,
  with optional `byte` / `word` / `dword` / `qword` prefixes

Scaled index forms such as `[rdi + rax * 8]` are not implemented yet.

Example:

```nasm
global main

section .text
main:
    mov eax, 42
    ret
```

## Tiny `.Z` compiler

The kernel now also includes a deliberately tiny C-like compiler for script-sized
programs. `.Z` source is compiled into the assembler subset above, then assembled
into the same flat binary format used by `exec`.

Current workflow:
- `zc demo.Z demo.bin`
- `exec demo.bin`
- or `zrun demo.Z`
- object/link path: `zco demo.Z demo.zo`, then `zlink demo.zo demo.bin`,
  then `exec demo.bin`; multiple objects can be linked with
  `zlink first.zo second.zo combined.bin`
- kernel-module path: `zco provider.Z provider.zo`, `zco user.Z user.zo`,
  then `zmod provider.zo user.zo`; this links in memory, resolves object
  exports plus exposed kernel symbols, and calls each object's initializer
- resident module path: after `zmod provider.zo`, later `zmod user.zo` can
  resolve `extern` references against the provider's resident exported symbols;
  use `zmods` to inspect loaded module slots and export addresses
- object functions can cross-call by name with `export int name(...) { ... }`
  in the defining object and `extern int name(...);` in the caller; the linker
  validates exported/extern symbol records against both linked objects and
  the shared kernel export table before applying relocation records
- self-hosted build path: `zbuild kernel` reads `kernel.zbuild`, compiles each
  listed `.Z` file to a `.zo`, links the objects, reports the first failing
  source/link error, writes all artifacts back to lainfs, and saves a
  `kernel.buildlog`; `zclean kernel` removes those build artifacts and
  `ztest kernel` rebuilds/runs the linked output and saves `kernel.testlog`
- shared source declarations with `#include "file.Z"` are expanded by the
  in-OS `.Z` commands before compilation, with nested includes capped at 4
- use `zasm demo.Z` to print the generated asm
- or `zasm demo.Z demo.asm` to save it as a text file

Supported `.Z` subset:
- integer variables: `int counter;`, `int total = 3;`
- explicit global storage declarations before functions/top-level statements:
  `global uint64_t counter = 5;`, `global int values[4];`
- file-local static storage declarations before functions/top-level statements:
  `static uint32_t tick_cache = {0,};`
- fixed-width scalar integer variables and parameters such as `uint8_t`, `int8_t`,
  `uint16_t`, `int16_t`, `uint32_t`, `int32_t`, `uint64_t`, `int64_t`
- `const` and `volatile` qualifiers are accepted on declarations and casts;
  they document intent but do not yet enforce read-only or volatile access rules
- enum integer constants, including anonymous enums:
  `enum { Width = 80, Height = 25, Error = -1, };`
- object-like numeric `#define` constants:
  `#define WIDTH 80`, `#define FLAGS 0x20u`, `#define ERROR (-1)`
- simple include guards and header conditionals with `#ifndef`, `#ifdef`,
  numeric `#if`, `#else`, and `#endif`; `#pragma once` is accepted as a no-op
- plain struct forward declarations: `struct Node;`
- typedef aliases for scalar, pointer, function-pointer, and struct types:
  `typedef uint32_t u32;`, `typedef struct Node Node;`,
  `typedef struct Pair { u32 left; u32 right; } Pair;`
- integer and void functions with up to 6 parameters:
  `int add(int a, int b) { return a + b; }`
  `void line(void) { print("\n"); return; }`
- forward function prototypes before function bodies, including unnamed
  prototype parameters: `int add(int, int);`
- function pointer declarations, parameters, struct fields, assignments from
  known function names, and indirect calls:
  `int (*op)(int); int (*table[4])(int); op = add; value = op(41); ops->draw(7); table[i](9);`
- function pointer signatures are checked for known prototypes/definitions,
  including assignment compatibility, argument count, and argument types
- top-level statements and declarations outside explicit functions; these are
  lowered into the implicit entry routine that `zrun`/`exec` starts from
- local variables inside functions
- pointer variables and parameters such as `int *p`
- fixed-size local arrays such as `int values[4];`
- brace initialization for fixed-size local arrays such as `int values[4] = {1, 2, 3, 4};`,
  including trailing commas; scalar declarations also accept one-value braces
- string-literal initialization for local byte arrays such as
  `uint8_t name[16] = "sysstat";`
- multidimensional local arrays such as `int grid[2][3];`
- top-level struct definitions with integer fields
- assignment: `counter = counter + 1;`
- compound assignment and increment/decrement: `+=`, `-=`, `*=`, `/=`, `%=`,
  `++`, `--`
- `sizeof(type)` and `sizeof(name)` for storage sizes known to the compiler
- arithmetic expressions: `+`, `-`, `*`, `/`, `%`, unary `-`, parentheses,
  decimal and `0x` literals
- scalar casts such as `(uint8_t)value`, `(int16_t)value`, `(uint64_t)value`,
  and pointer casts such as `(uint8_t *)ptr`
- comparisons and boolean expressions: `==`, `!=`, `<`, `<=`, `>`, `>=`,
  `&&`, `||`, `!`; `<`, `<=`, `>`, `>=` now use unsigned jumps when the
  comparison operands resolve to an unsigned scalar type
- control flow: `if`, `else`, `while`, `for`, `do ... while`, `switch`,
  `case`, `default`, `break`, `continue`
- returns: `return expr;` and `return;`
- function calls inside expressions: `print(add(2, 3));`
- pointer operations:
  - address-of locals/parameters: `p = &value;`
  - dereference in expressions: `print(*p);`
  - dereference store: `*p = *p + 1;`
  - pointer arithmetic scaled by pointee size: `p + 1`, `p - 1`
- array operations:
  - indexed read: `print(values[2]);`
  - indexed write: `values[i] = 42;`
  - array-to-pointer decay in expressions and function calls: `sum4(values);`
  - chained indexing for multidimensional arrays: `grid[i][j]`
- struct operations:
  - declare a local struct variable: `struct Point p;`
  - read a field: `print(p.x);`
  - write a field: `p.y = 42;`
  - whole-struct assignment for matching struct types: `dst = src;`
  - struct parameters are copied into local struct values: `int area(struct Size s) { return s.w * s.h; }`
  - struct returns into a known destination: `size = make_size(80, 25);`
  - pointer-to-struct field access: `p->x`, `p->y = 42;`, `p->count += 1;`
  - nested field chains such as `outer.inner.x`, `outer.ptr->x`, and
    `outer_ptr->inner.y += 1;`
  - mixed-width fixed-layout fields such as `uint8_t`, `uint16_t`, `uint32_t`,
    `uint64_t`, and pointer fields inside structs
- builtin calls:
  - `print("text");`
  - `print(expr);` for decimal output
  - `print_hex(expr);`
  - `put_pixel(x, y, color);`
  - `gfx_width()`, `gfx_height()`, `gfx_pitch()`, `gfx_format()`
  - `gfx_fill_rect(x, y, w, h, color);`
  - `gfx_draw_rect(x, y, w, h, color);`
  - `gfx_draw_line(x0, y0, x1, y1, color);`
  - `gfx_clear(color);`
  - `mouse_enabled()`, `mouse_x()`, `mouse_y()`, `mouse_buttons()`
  - `mouse_dx()` and `mouse_dy()` for the most recent PS/2 packet delta
  - `put_char_at(col, row, ch);`
  - `put_dec_at(col, row, expr);`
  - `put_char_at_screen(col, row, ch);`
  - `put_dec_at_screen(col, row, expr);`
  - `set_margin(x, y);` to reserve screen space, for example below a custom status bar
  - `statusbar_enable();`
  - `ticks()` inside expressions
  - `mem_total_kb()`, `mem_free_kb()`, `mem_used_kb()`
  - `cpu_count()` and `cpu_usage(core)`
- comments: `// line comment` and `/* block comment */`

Example:

```c
int add(int a, int b) {
    int total = a + b;
    return total;
}

int value = 9;
int *ptr = &value;

*ptr = add(*ptr, 3);
print(*ptr);
print("\n");

return value;
```

This is still not full C. Function-like macros, textual macro expansion,
conditional preprocessing, bitfields, and broad C-style type checking are not
implemented yet. Current pointer support is intentionally narrow:
address-of works for compiler-known locals, globals, array elements, and struct
fields, dereference is scalar-only, and pointer arithmetic is limited to
`pointer +/- integer` scaling. Current array support is also narrow: arrays are
fixed-size compiler-backed storage, brace initialization is limited to flat element lists,
multidimensional arrays currently use chained local-array indexing only, and
current struct support is still incomplete: structs must be declared at top level,
and struct return values currently need a known destination such as assignment,
initialization, or `return make_struct(...)`; they are not general-purpose
expression values yet. Mixed-width scalar, pointer, and nested
struct fields now use packed offsets and width-correct memory access, so layouts such as
`uint8_t` + `uint16_t` + `uint32_t` + `uint64_t` no longer collapse into
8-byte `int` slots. `.` and `->` can now be chained through nested struct fields.
Function pointers currently use pointer-sized storage and support calls through
local/global variables, parameters, struct fields, and indexed pointer tables.
Known function-pointer signatures are checked, but unprototyped/unknown
function symbols still fall back to permissive pointer behavior.
The normal host build now also compiles `kernel/z/zlink_probe.Z` into a NASM ELF
object and links it into `kernel.elf`, proving the kernel can contain selected
`.Z` objects alongside C and ASM objects. That file now contains the
status-bar CPU busy-percent helper used by C code, so the normal kernel path
has its first small `.Z` utility in live use.
Explicit `global` and `static` declarations emit real labels in the generated data section,
and functions/top-level code can read, write, index, take addresses of, and use
compound updates on those globals. Global initializers are still intentionally
limited to numeric scalar constants; arrays and structs are zero-initialized.

Fixed-width scalar support is still partial: `.Z` now preserves truncation
and sign/zero-extension for scalar locals, parameters, indexed local-array
elements, mixed-width struct fields, and explicit scalar casts. Basic unsigned
comparison semantics are now wired into relational operators, but `.Z` still
does not provide general-purpose struct-return expressions.

The `.zo` object format is a first in-OS toolchain checkpoint, not the final
kernel object ABI. Version 5 stores one assembled load image, explicit `.text`,
`.data`, and `.bss` section records describing that image, exported and external
symbol records, an entry offset, and relocation records. The current relocation
set covers `ABS64` named extern addresses, `RELATIVE64` internal absolute
addresses, and `RIP32` label-based data references. `zlink` validates
duplicate/missing symbols, lays out linked `.text`, `.data`, and `.bss` sections
independently, applies relocations through that layout, and emits the flat
executable format used by `exec`. `zmod` uses the same linker in memory, also
allowing extern references to resolve against the shared kernel export table
and any resident module exports, then calls the linked module initializer
and keeps the module image resident. `zmods` lists resident module slots and
final export addresses.

`zbuild` is the first in-OS multi-file build command. A target name maps to a
manifest in the current lainfs directory:

```text
zbuild kernel
```

loads `kernel.zbuild`. Blank lines and lines beginning with `#`, `;`, or `//`
are ignored. Each source line is either `source.Z` or `source.Z output.zo`.
The optional directive `output name.bin` changes the linked output name;
otherwise `zbuild kernel` writes `kernel.bin`. `src dir`, `include dir`, and
`build dir` directives let a project use a small layout such as `src/`,
`include/`, and `build/`; object files, the linked binary, and a
`target.buildlog` report are written to the build directory. Every object is
saved before the final link, so partial build products are inspectable with
`zmods`, `zlink`, or `exec` workflows. A manifest can include `module` or
`objects-only` to skip the final executable link; single-object module targets
can still be installed with `zinstall target`, which copies the generated `.zo`.
`zclean target` reads the same manifest
and removes the generated objects, linked output, build log, and test log from
the build directory. `ztest target` cleans, builds, runs the linked output from the build
directory, and writes `target.testlog` with the returned value. A manifest can
also include `test-return N`; when present, `ztest` records `status ok` only if
the program returns that exact integer. `zinstall target` builds the target,
loads the linked output from the build directory, and saves it to the manifest's
`install dir` plus optional `install-name name.bin`; without those directives it
installs into the current directory using the output name. `zinstall target
dest.bin` can override the manifest destination.

The `.Z` shell commands now expand simple include lines before compiling:

```c
#include "kernel_api.Z"
include "project_defs.Z"
```

Included files are loaded beside the current source file, and the same expansion
path is used by `zc`, `zco`, `zrun`, `zasm`, and `zbuild`. Includes also support
relative paths such as `src/foo.Z`, root-style paths such as
`/include/kernel_api.Z`, and `#pragma once` for duplicate-safe shared headers.
`zbuild` also searches manifest `include` directories when an include is not
found beside the current source file. See `examples/selfhost_project/` for a
small `src`/`include`/`build` layout.

The shared kernel API header also exposes a small self-hosting filesystem
surface to `.Z`: `os_mkdir(path)`, `os_delete(path)`,
`os_write_file(path, text)`, `os_cat_file(path)`, `os_file_size(path)`,
`os_read_file(path, buffer, capacity)`, `os_rename(old_path, new_path)`,
`os_copy_file(src_path, dst_path)`,
`os_strlen(text)`, `os_strcmp(a, b)`, `os_starts_with(text, prefix)`,
`os_atoi(text)`,
`os_list_dir(path)`, `os_chdir(path)`, `os_dir_count(path)`,
`os_dir_name(path, index, buffer, capacity)`, `os_dir_type(path, index)`,
`os_dir_size(path, index)`,
`os_zbuild(target)`, `os_ztest(target)`, `os_zinstall(target)`,
`os_zmod(target)`, `os_zunload(target)`, and `os_zreload(target)`.
These operate on the shell's active lainfs drive and current directory, and
paths can include the same simple relative path forms used by `zbuild`.
The build/test/install API calls return `0` only after verifying their expected
artifact or `status ok` test log, while the module API calls return `0` only
after the resident module table changed as expected, so `.Z` tools can branch
on failures.
`examples/sysstat.Z` is the first small leaf tool in this flow: copy
`examples/kernel_api.Z`, `examples/sysstat.Z`, and `examples/sysstat.zbuild`
into lainfs, then run `ztest sysstat`, `zinstall sysstat`, and
`exec sysstat.bin`. `examples/zreport.Z` uses the directory-entry API to list
the current project directory and print build/test logs, then follows the same
`ztest zreport`, `zinstall zreport`, `exec zreport.bin` flow.
`examples/zmake.Z` is the first self-hosted orchestrator: it calls `os_ztest`
and `os_zinstall` for `sysstat` and `zreport`, stopping on the first failure.

Resident `.Z` modules can export `zmodule_tick`; after `zmod` loads the module,
the kernel calls that hook while the shell is idle or waiting for input.
`examples/hwdash_module.Z` uses this to keep memory and CPU bars updating as a
resident dashboard module:

```text
zbuild hwdash_module
zmod hwdash_module.zo
zmods
```

In desktop mode, the native `.Z` editor has Save, Build, Inst, and Load buttons.
Build derives the target from the open `.Z` or `.zbuild` filename and runs
`zbuild target`; Inst runs `zinstall target`; Load runs `zinstall target`
followed by `zreload target`, making the edit-build-load loop usable without
leaving the desktop.

The remaining ABI milestones are real nonzero `.bss` emission from `.Z`,
dependency-aware unload hooks, and enough relocation/runtime surface for
kernel-shaped code before `.Z` can build loadable kernel modules or the kernel
image itself.

## Timer

The kernel programs the legacy PIT at 100 Hz, remaps the PIC so hardware IRQs
start at IDT vector 32, installs IRQ0, and enables interrupts with `sti`.

To verify it from the shell:

```text
ticks
```

Wait a moment, then run:

```text
ticks
```

The tick count should increase by roughly 100 per second. This timer is the
right place to drive small time-based kernel behavior such as cursor blinking.

It still does **not yet**:
- apply relocations for position-independent kernels
- set up a higher-half virtual memory layout
- create new page tables for the kernel
- map virtual addresses distinct from physical addresses
- support dynamic linking
- read directories or files from NTFS inside the kernel
- support full path operands or non-contiguous file extents in `lainfs`
- discover real disk sizes through IDENTIFY data
- use APIC/HPET or per-core timers instead of the legacy PIT/PIC path
- handle every GOP pixel format correctly
- scroll text output
- set up an IDT/GDT/TSS for later kernel work

## Why no long-mode switch?

Because this is UEFI on x86_64, your loader already runs in 64 bit mode. The old BIOS path of real mode -> protected mode -> long mode is not needed here.
