# UEFI x86_64 Starter Project

Minimal starter project for:
- a **UEFI bootloader** in C
- a **dummy 64 bit kernel** in NASM
- loading an **ELF64 kernel** from the EFI filesystem
- collecting boot information
- calling `ExitBootServices`
- jumping into 64 bit kernel code with a boot info pointer
- drawing visible text through a tiny framebuffer console

## Project layout

- `bootloader/main.c` - UEFI loader entry point and ELF64 loader
- `kernel/entry.asm` - tiny assembly kernel entry stub
- `kernel/main.c` - main kernel logic in C
- `kernel/kernel.h` - shared kernel-side API declarations
- `kernel/assembler.c` - tiny in-kernel assembler used by the `asm` shell command
- `kernel/console.c` - framebuffer console, formatting, and panic screen in C
- `kernel/cpu.c` - GDT/IDT table construction in C
- `kernel/cpu_low.asm` - tiny low-level CPU table load helpers
- `kernel/keyboard.c` - minimal PS/2 keyboard polling input
- `kernel/interrupts.asm` - exception stubs and fault entry path
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
6. A `boot_info` structure is placed at `0x90000`
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

On UEFI x86_64, the first argument arrives in `RCX`, which the NASM kernel reads directly.

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

## Tiny framebuffer console

The kernel now includes a very small software text renderer:
- fixed 8x8 bitmap glyphs
- contiguous ASCII font table for readable output
- direct pixel writes into the GOP framebuffer
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
- assumes a 32 bit linear framebuffer layout that works for common GOP modes
- no framebuffer format conversion layer yet
- panic path currently depends on console initialization succeeding

## Requirements

On Debian/Ubuntu-like systems you typically need:

```bash
sudo apt install gcc make nasm binutils qemu-system-x86 ovmf gnu-efi dosfstools mtools gdisk
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

This creates:
- `build/bootloader.so`
- `build/BOOTX64.EFI`
- `build/kernel.elf`
- `build/kernel.bin`
- `build/image/EFI/BOOT/BOOTX64.EFI`
- `build/image/kernel.elf`
- `build/esp.img`
- `build/bootdisk.img`
- `build/data.img`

`kernel.bin` is still produced as a convenience artifact for inspection, but the UEFI loader now boots from `kernel.elf`.
`esp.img` is a raw FAT32 ESP image that is convenient for QEMU. `bootdisk.img`
wraps that ESP in a proper GPT disk with an EFI System Partition, which is more
friendly to VirtualBox and real UEFI firmware.

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

This boots from the real FAT32 ESP image.

To boot the GPT disk image with the persistent data disk attached through AHCI:

```bash
make run-bootdisk
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
- `build/bootdisk.img` - GPT boot disk with a FAT32 EFI System Partition
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

This is now a clean milestone-1 starter: UEFI boot, ELF64 loading, boot info handoff, a C kernel main path, a C console path, visible kernel output, mostly-C descriptor-table setup, and a minimal PS/2 keyboard input path all work.

Current input path assumes classic QEMU PS/2 keyboard behavior and simple set-1 scancodes.
It now includes basic Shift and Caps Lock handling plus a simple visible text cursor during line input.

## Shell commands

The kernel console has a tiny command shell:
- `help` - show commands
- `bgcolor 0xRRGGBB` - set the background color
- `fgcolor 0xRRGGBB` - set the text color
- `clear` - clear the screen
- `echo text` - print text
- `info` - show boot/kernel info
- `mkdrive C:` - create a virtual `C:` drive
- `drives` - list virtual drives
- `C:` - switch to an existing virtual `C:` drive
- `blk` - list block devices
- `part` - list discovered partitions
- `mount C: rd0p1` - mount a partition at a drive letter
- `mounts` - list mounted filesystems
- `ahci` - show detected AHCI controllers and disks
- `ticks` - show PIT timer ticks
- `format C:` - format a mounted drive as `lainfs`
- `ls [C:]` - list `lainfs` entries in a table
- `cd name` / `cd ..` / `cd \` - change directory
- `pwd` - show the current drive and directory
- `mkdir name` - create a directory entry
- `rm name` / `del name` - delete a file or directory entry
- `rename old new` / `mv old new` - rename or move a file or directory entry
- `write name text` - write a text file to the current drive
- `cat name` - print a text file from the current drive
- `asm source.asm output.bin` - assemble a tiny x86_64 source file
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
`bits 64`, and `default rel` are accepted for source compatibility. Labels and
`db` data are supported.

The instruction subset is intentionally small but covers the common flat-binary
building blocks:
- registers: `rax`..`r15` and `eax`..`r15d`
- control: `nop`, `hlt`, `ret`, `ret imm16`, `leave`, `int imm8`, `iretq`,
  `syscall`, `cli`, `sti`
- data movement: `mov r64, imm64`, `mov r32, imm32`, `mov reg, reg`,
  `push r64`, `pop r64`
- arithmetic and logic: `add`, `sub`, `cmp`, `and`, `or`, `xor`, `test`,
  `inc`, `dec`
- flow: `call label`, `call r64`, `jmp label`, `jmp r64`, and rel32
  conditional jumps such as `je`, `jne`, `jl`, `jle`, `jg`, `jge`, `jb`,
  `jbe`, `ja`, and `jae`

Memory operands such as `[rbp-8]` are not implemented yet.

Example:

```nasm
global main

section .text
main:
    mov eax, 42
    ret
```

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
