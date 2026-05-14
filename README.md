# LainOS

LainOS is a small UEFI x86_64 operating-system project. It now boots through a
GNU-EFI loader, enters a mostly-C kernel, brings up framebuffer graphics,
storage, a tiny filesystem, a desktop shell, resident modules, and an in-kernel
`.Z` toolchain.

This is still an experimental hobby OS, but it has moved beyond the original
"print text from a kernel" milestone. The current direction is: self-hosting
tools, small graphical applications, and incremental kernel infrastructure.

For day-to-day development practices, module/app workflows, debugging checklists,
and desktop rendering rules, see `DEVELOPMENT_HANDBOOK.md`.

## Current Highlights

- UEFI `BOOTX64.EFI` loader written in C with ELF64 kernel loading.
- Shared boot protocol in `boot/shared/bootinfo.h` and `boot/shared/bootinfo.inc`.
- 64-bit x86_64 kernel loaded at a fixed physical address by the UEFI loader.
- GOP framebuffer console and graphics primitives.
- Desktop mode with windows, terminal, file browser, editor, module windows,
  mouse interaction, and a backbuffered redraw path.
- SMP CPU discovery and startup through ACPI MADT, xAPIC INIT/SIPI, AP
  trampoline, and a small multicore work queue.
- Per-core CPU activity accounting for task-manager-style modules.
- PIT/PIC timer and IRQ stubs.
- PS/2 keyboard and mouse input, plus early USB/xHCI probing paths.
- PCI scan, legacy ATA/IDE, AHCI block devices, E1000 detection, and simple
  networking experiments.
- `lainfs`, a deliberately tiny writable filesystem used by the OS tools.
- In-kernel shell, editor, browser, assembler, `.Z` compiler, object linker,
  module loader, and project build commands.
- Example `.Z` apps and modules in `examples/`, including desktop task manager,
  hardware dashboard, mouse demo, file manager, JPEG demo, and self-hosting
  build examples.

## Project Layout

- `bootloader/main.c` - GNU-EFI loader, ELF loader, GOP setup, boot info setup.
- `boot/shared/` - shared boot protocol declarations for C and NASM.
- `kernel/arch/x86_64/` - entry point, interrupt stubs, AP trampoline, CPU helpers.
- `kernel/core/` - kernel main path, CPU/SMP, timer, DMA pool, power, exports.
- `kernel/drivers/` - graphics, console, keyboard, mouse, PCI, AHCI, USB, net.
- `kernel/fs/` - `lainfs` implementation.
- `kernel/ui/` - shell, desktop, editor, browser.
- `kernel/z/` - assembler, `.Z` compiler, `.zo` object support.
- `kernel/include/` - kernel headers and exported APIs.
- `examples/` - `.Z` examples, resident modules, manifests, and `autoexec`.
- `tools/` - host-side helpers for seeding images and compiling test artifacts.
- `archive/kernel-old/` - old assembly prototypes kept for reference.
- `Makefile` - image, ISO, PXE, and QEMU workflows.

## Requirements

On Debian/Ubuntu-like systems:

```bash
sudo apt install gcc make nasm binutils qemu-system-x86 ovmf gnu-efi dosfstools mtools gdisk xorriso
```

If your GNU-EFI paths differ, override them:

```bash
make EFI_INC=/usr/include/efi \
     EFI_LIBDIR=/usr/lib \
     EFI_CRT0=/usr/lib/crt0-efi-x86_64.o \
     EFI_LDS=/usr/lib/elf_x86_64_efi.lds
```

Inspect the configured EFI paths with:

```bash
make print-efi-config
```

## Build

```bash
make clean
make
```

The full build creates:

- `build/BOOTX64.EFI`
- `build/kernel.elf`
- `build/kernel.bin`
- `build/esp.img`
- `build/bootdisk.img`
- `build/bootdisk-gpt.img`
- `build/data.img`
- `build/boot.iso`

`kernel.elf` is the real boot artifact. `kernel.bin` is still produced for
inspection and experiments.

To request a GOP mode at build time:

```bash
make BOOT_RES_WIDTH=1024 BOOT_RES_HEIGHT=768
```

From inside the OS, after a lainfs drive is mounted, this writes a next-boot
resolution request:

```text
resolution 1024 768
```

The loader searches lainfs devices for `bootres.cfg` and applies the requested
mode if firmware exposes it.

## Run

Default QEMU boot from the FAT32 ESP image:

```bash
make run
```

Other useful targets:

```bash
make run-ahci
make run-usb
make run-net
make run-bootdisk
make run-bootdisk-gpt
make run-iso
```

`run.sh` is also available and currently boots the ISO with `-smp 4`, the data
image, and an E1000 NIC using local `OVMF_CODE.fd` / `OVMF_VARS.fd` files.

Verify the EFI binary:

```bash
make inspect-efi
```

## Boot Flow

1. UEFI starts `BOOTX64.EFI`.
2. The loader optionally starts `\EFI\BOOT\drivers\ntfs_x64.efi`.
3. The loader scans UEFI-visible filesystems for `\kernel.elf`.
4. It parses the ELF64 program headers.
5. It reserves and loads all `PT_LOAD` segments.
6. It gathers the UEFI memory map, GOP framebuffer mode, and ACPI RSDP pointer.
7. It allocates and fills `boot_info_t`.
8. It retries `GetMemoryMap` / `ExitBootServices` until the map key is stable.
9. It jumps to the ELF entry point with `boot_info_t *` in `RDI`.
10. The kernel initializes graphics, console, CPU tables, SMP, timer, DMA,
    input, storage, USB, networking, shell state, and autoexec.

Kernel entry convention:

```c
void kernel_main(boot_info_t *info);
```

Current `boot_info_t` fields:

- `magic`
- `kernel_base`
- `kernel_size`
- `memory_map`
- `memory_map_size`
- `memory_map_descriptor_size`
- `memory_map_descriptor_version`
- `framebuffer_base`
- `framebuffer_width`
- `framebuffer_height`
- `framebuffer_pixels_per_scanline`
- `framebuffer_format`
- `rsdp`

Magic value:

```c
0x4C41494E424F4F54ULL
```

## Kernel Subsystems

### CPU And SMP

The kernel parses ACPI MADT CPU topology and records local APIC IDs. It starts
secondary xAPIC-addressable CPUs with a real-mode AP trampoline copied to
`0x8000`, then brings them into long mode using the BSP page tables.

Secondary CPUs load the shared GDT/IDT, enable their LAPIC, enter an idle loop,
and wake through an SMP IPI vector. A fixed-size SMP work queue lets the kernel
submit function-pointer jobs to online APs:

- `smp_submit_work(fn, arg)`
- `smp_wait_work(id)`
- `smp_work_done(id)`
- `smp_pending_work_count()`

The shell command `smp` submits visible test work. CPU usage accounting records
busy ticks for AP-executed work, so `taskmgr_module` can show activity on
secondary CPU rows.

This is not a preemptive scheduler yet. There are no per-core run queues,
kernel threads, TSS/IST setup, APIC timer interrupts, or userspace processes.
Basic per-core state tracks LAPIC ID, online state, and busy ticks.

### Timer And Interrupts

The kernel uses the legacy PIT at `250 Hz`, remaps the PIC to IRQ vectors
starting at 32, and installs IRQ0 for timer ticks. The IDT also contains CPU
exception stubs, PS/2 mouse IRQ handling, and the SMP IPI vector.

Useful shell command:

```text
ticks
```

### Graphics And Desktop Rendering

The graphics driver assumes a 32-bit linear framebuffer and supports common UEFI
RGB/BGR formats. It provides pixels, rectangles, lines, full clears, and a
vertical-gradient helper.

Current rendering improvements:

- Large rectangle fills write framebuffer rows directly.
- Large fills and gradients are split across online CPUs with the SMP queue.
- Desktop full redraws render into a lazily allocated backbuffer.
- Full desktop redraws flush from the backbuffer to the real framebuffer in a
  multicore copy pass.

This hides most visible "paint the scene live" redraw artifacts. Desktop also
tracks dirty rectangles for window geometry, taskbar/menu changes, editor
content, file/module actions, and module app ticks so redraws can flush bounded
regions from the backbuffer instead of copying the whole screen. This is still
an incremental damage model, not a full z-order-aware compositor.

### Console And Input

The console uses an 8x8 bitmap font with line wrapping, scrolling, decimal/hex
printing, a tiny `kprintf` subset, and panic-screen support.

Keyboard input supports basic PS/2 set-1 scancodes, Shift, Caps Lock, and
layout switching:

```text
keymap us
keymap de
```

`Ctrl+W` opens a two-pane console split and switches focus. `Ctrl+E` closes the
right pane.

### Storage And Filesystems

The block/storage stack can expose:

- `rd0` seeded RAM disk
- legacy ATA/IDE disk as `hd*`
- AHCI disks as `sd*`
- MBR/GPT partitions as `hd1p1`, `sd0p1`, and similar names

`lainfs` is the current native filesystem. It is intentionally tiny: small
directory tables, contiguous file data, and a 64 KiB file cap. It supports
directories, parent links, scoped `ls`, `cd`, `pwd`, move/rename, copy, and
delete operations.

The build creates `build/data.img`, attaches it in QEMU, and keeps it persistent
across normal rebuilds.

Typical persistent-disk flow:

```text
blk
part
mount C: hd1p1
format C:
write hello hello-from-disk
ls
cat hello
```

After reboot:

```text
mount C: hd1p1
C:
ls
cat hello
```

### USB, PCI, AHCI, And Network

USB support is still conservative. The `usb` command lists PCI USB host
controllers, and subcommands can probe or start xHCI paths:

```text
usb
usb scan 0
usb init 0
usb handoff 0
usb start 0
usb enum 0
```

AHCI is early but integrated with the block-device API. E1000 and basic network
diagnostics exist for experimentation.

## Shell Commands

Core commands:

- `help`
- `clear`
- `echo text`
- `info`
- `cpus`
- `smp`
- `ticks`
- `reboot`
- `poweroff` / `shutdown`
- `bgcolor 0xRRGGBB`
- `fgcolor 0xRRGGBB`
- `resolution [width height]`
- `keymap us|de`

Storage and files:

- `mkdrive C:`
- `drives`
- `C:`
- `blk`
- `part`
- `mount C: rd0p1`
- `mounts`
- `format C:`
- `format hd1p1`
- `format hd1`
- `ls [C:]`
- `cd name`
- `cd ..`
- `cd \`
- `pwd`
- `mkdir name`
- `rm name` / `del name`
- `rename old new` / `mv old new`
- `cp source dest`
- `write name text`
- `cat name`
- `browse [path-or-drive:]`

Hardware and UI:

- `ahci`
- `usb [subcommand]`
- `net`
- `wget http://IP/path output`
- `desktop`

Toolchain:

- `asm source.asm output.bin`
- `exec file.bin`
- `zc source.Z output.bin`
- `zco source.Z output.zo`
- `zlink input.zo [more.zo ...] output.bin`
- `zmod input.zo [more.zo ...]`
- `zunload module`
- `zreload module`
- `zmods`
- `zrun source.Z`
- `zasm source.Z [output.asm]`
- `zbuild target`
- `zclean target`
- `ztest target`
- `zinstall target [dest.bin]`

## Desktop Mode

Start it with:

```text
desktop
```

Exit with `Esc`, `Ctrl+Q`, or by holding both mouse buttons.

Desktop features:

- Terminal window running the real shell.
- Start menu and taskbar.
- File browser window.
- Modules window and module app windows.
- Native `.Z` editor with Save, Build, Inst, Load, and Log buttons.
- Mouse drag/resize with lightweight outline previews.
- Up to six open module app windows.
- Viewport clipping/translation for resident module drawing.
- Backbuffered full redraws with multicore flushes.

Useful workflow:

```text
zbuild taskmgr_module
zmod taskmgr_module.zo
desktop
```

Open the task manager module from the Modules list or Start menu, then run
`smp` from the terminal to see secondary-core activity.

## In-Kernel Toolchain

### Tiny Assembler

`asm` accepts a small NASM-like subset and emits flat binaries runnable with
`exec`. It supports labels, data directives, basic control flow, arithmetic,
loads/stores, calls, and references to exported kernel symbols.

### Tiny `.Z` Compiler

`.Z` is a small C-like language compiled inside the kernel. It supports:

- scalar integer types including fixed-width signed/unsigned forms
- globals and file-local statics
- functions, prototypes, function pointers, and indirect calls
- local arrays and simple multidimensional local arrays
- structs, nested fields, pointer-to-struct access, and mixed-width fields
- pointers, address-of, dereference, and scaled pointer arithmetic
- `if`, `else`, `while`, `for`, `do while`, `switch`, `break`, `continue`
- numeric `#define`, include guards, `#include`, and `#pragma once`
- comments
- calls to selected kernel APIs

Important commands:

```text
zrun examples/zlang/hello.Z
zco source.Z source.zo
zlink source.zo source.bin
exec source.bin
zbuild sysstat
ztest sysstat
zinstall sysstat
```

`.zo` objects carry section records, exports, externs, entry offsets, and
relocations. `zlink` lays out `.text`, `.data`, and `.bss`, resolves symbols,
and emits flat executables. `zmod` uses the same linker in memory, resolves
against kernel exports and resident modules, calls the module initializer, and
keeps the module image resident.

Resident modules can export:

- `zmodule_tick`
- `zmodule_unload`
- `zmodule_redraw`

Examples:

```text
zbuild hwdash_module
zmod hwdash_module.zo

zbuild taskmgr_module
zmod taskmgr_module.zo

zbuild filemgr_module
zmod filemgr_module.zo

zbuild zbrowser_module
zmod zbrowser_module.zo
```

The example `autoexec` preloads useful modules and can enter desktop mode.

The tiny Z browser module renders basic HTML from `index.html` or from a
plain HTTP URL stored in `browser.url`. Networking currently expects numeric
IPv4 HTTP URLs, for example:

```text
write browser.url http://10.0.2.2:8000/index.html
zbuild zbrowser_module
zmod zbrowser_module.zo
desktop
```

## Kernel Export Surface

The kernel export table exposes selected symbols to assembler, `.Z`, linked
objects, and resident modules. Current groups include:

- console output: `puts`, `put_hex64`, `put_dec64`
- timer: `ticks`
- memory status: `mem_total_kb`, `mem_free_kb`, `mem_used_kb`
- CPU status: `cpu_count`, `cpu_usage`
- SMP work queue: `smp_submit_work`, `smp_work_done`, `smp_wait_work`,
  `smp_pending_work_count`
- graphics: `put_pixel`, `gfx_width`, `gfx_height`, `gfx_pitch`,
  `gfx_format`, `gfx_fill_rect`, `gfx_draw_rect`, `gfx_draw_line`,
  `gfx_clear`, viewport helpers
- mouse helpers
- screen text helpers
- status bar
- filesystem/project APIs such as `os_write_file`, `os_read_file`,
  `os_http_get`, `os_zbuild`, `os_ztest`, `os_zinstall`, `os_zmod`,
  `os_zunload`, and `os_zreload`
- desktop helper: `os_open_editor`

See `kernel/core/kernel_exports.c` for the authoritative list.

## VirtualBox

Build raw images:

```bash
make
```

Convert to VDI:

```bash
VBoxManage convertfromraw build/bootdisk.img build/bootdisk.vdi --format VDI
VBoxManage convertfromraw build/data.img build/data.vdi --format VDI
```

Create a 64-bit VM:

- Enable EFI.
- Use 256 MiB RAM or more.
- Attach `bootdisk.vdi` and `data.vdi` to a SATA controller.
- Boot from hard disk.

Inside the OS:

```text
ahci
blk
part
mount C: sd0p1
format C:
write hello hello-from-virtualbox
ls
cat hello
```

After reboot, mount again but do not format.

## Optional UEFI NTFS Driver

The bootloader can start an optional UEFI NTFS driver before looking for
`kernel.elf`:

```bash
make NTFS_DRIVER=/path/to/ntfs_x64.efi
```

The driver is copied to:

```text
\EFI\BOOT\drivers\ntfs_x64.efi
```

This only affects bootloader-time filesystem visibility. The kernel does not
yet implement NTFS.

## Caveats

Things that are intentionally incomplete:

- no higher-half kernel
- no custom page table setup beyond using the firmware identity mappings
- no preemptive scheduler or kernel threads
- no per-core timers or APIC timer
- no TSS/IST
- no userspace/process isolation
- no dynamic kernel module ABI beyond the `.Z` resident module experiment
- no NTFS driver inside the kernel
- heap is active: page-range allocation, `kmalloc`/`kfree` classes, basic
  canary/double-free diagnostics, and the largest driver/UI scratch buffers
  are on heap
- limited GOP pixel-format support
- tiny `lainfs` limits and contiguous file allocation
- early USB/xHCI enumeration
- early AHCI and network paths
- no full z-order-aware window compositor yet

## Good Next Work

Here are high-leverage next steps, roughly ordered by payoff:

1. Heap hardening.
   Add allocation failure tests, leak checks for long desktop sessions, and
   stronger fragmentation reports under repeated module/browser use.

2. Per-core data and APIC timer.
   Calibrate and enable LAPIC timer interrupts, then use the existing per-core
   state for local tick accounting. This sets up a future scheduler cleanly.

3. Cooperative kernel tasks.
   Build a small task abstraction on top of the SMP executor before jumping to
   preemption. Let background jobs such as builds, file copies, and redraw
   preparation run off the BSP.

4. Dirty filesystem cache.
   Cache directory blocks and file data in memory, then flush deliberately.
   This would make editor/build workflows faster and reduce repeated disk reads.

5. Expand `lainfs`.
   Lift file count/file size limits, support non-contiguous extents, and add
   more robust metadata validation.

6. Desktop window damage model.
   Extend the current dirty-rectangle path into a z-order-aware compositor so
   moving a window redraws only exposed areas and the moved window itself.

7. Safer resident modules.
   Add dependency-aware unload, module ownership for resources, and better
   failure isolation when a module hook misbehaves.

9. USB input path.
   Make USB keyboard/tablet input first-class, not just experimental xHCI
   probing.

10. Self-hosting milestone.
    Keep moving small C/ASM helpers into `.Z` where it makes sense, then use
    `zbuild` to build more of the demo userland from inside the OS.

## Why No Long-Mode Switch?

UEFI x86_64 already enters the loader in 64-bit long mode. The BIOS-style
real-mode to protected-mode to long-mode path is only needed for legacy BIOS
boot. The AP trampoline still performs a real-mode startup path because that is
how x86 secondary CPUs begin after SIPI.
