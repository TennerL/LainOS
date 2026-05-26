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
  hardware dashboard, mouse demo, file manager, image viewer, and self-hosting
  build examples.

## Project Layout

- `bootloader/main.c` - GNU-EFI loader, ELF loader, GOP setup, boot info setup.
- `boot/shared/` - shared boot protocol declarations for C and NASM.
- `kernel/arch/x86_64/` - entry point, interrupt stubs, AP trampoline, CPU helpers.
- `kernel/core/` - kernel main path, CPU/SMP, timer, DMA pool, power, exports.
- `kernel/drivers/` - graphics, image decode, console, keyboard, mouse, PCI,
  AHCI, USB, net.
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

On macOS with Homebrew, install GNU-EFI plus GNU binutils and prefer the GNU
tool names:

```bash
brew install gnu-efi x86_64-elf-binutils nasm qemu mtools dosfstools gdisk xorriso
make CC="clang --target=x86_64-unknown-elf" \
     LD=x86_64-elf-ld \
     OBJCOPY=x86_64-elf-objcopy
```

If you have `x86_64-elf-gcc`, the Makefile will prefer it automatically for
kernel and EFI objects while still using the native compiler for host tools.

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
scripts/bootstrap-browser-deps.sh
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

The QEMU run targets also expose a matching high-resolution GOP-friendly VGA
device by default. Override the virtual display and loader request together with:

```bash
make QEMU_VIDEO_WIDTH=1920 QEMU_VIDEO_HEIGHT=1080 run
```

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

`run.sh` is also available and boots the ISO with the data image, an E1000 NIC,
local `OVMF_CODE.fd` / `OVMF_VARS.fd` files, and a 1280x720 framebuffer request
by default. Override it with:

```bash
FB_WIDTH=1920 FB_HEIGHT=1080 ./run.sh
```

When building in a Linux container and running QEMU from macOS, build first in
the container:

```bash
make BOOT_RES_WIDTH=1920 BOOT_RES_HEIGHT=1080 all
```

Then run from macOS:

```bash
RUN_BUILD=0 FB_WIDTH=1920 FB_HEIGHT=1080 ./run.sh
```

On Intel macOS, `run.sh` automatically asks QEMU for `hvf` acceleration. On
Apple Silicon, an x86_64 guest falls back to TCG translation, so it will be far
slower than a real x86_64 PC. For that case, lower the framebuffer size first:

```bash
RUN_BUILD=0 FB_WIDTH=1280 FB_HEIGHT=720 QEMU_SMP=4 ./run.sh
```

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

There is also a small cooperative kernel task layer on top of the SMP executor.
It submits work to secondary CPUs when possible and falls back to local polling
from the BSP:

- `kernel_task_submit(fn, arg)`
- `kernel_task_poll()`
- `kernel_task_wait(id)`
- `kernel_task_done(id)`
- `kernel_task_pending_count()`

The shell command `smp` submits visible test work. CPU usage accounting uses
cycle-based idle/busy sampling on the bootstrap CPU and records busy ticks for
AP-executed work, so `taskmgr_module` can show foreground activity and
secondary CPU queue work without charging whole PIT ticks for tiny redraws.

This is not a preemptive scheduler yet. There are no per-core run queues,
kernel threads, TSS/IST setup, or userspace processes. Basic per-core state
tracks LAPIC ID, online state, busy ticks, and local timer ticks.

### Timer And Interrupts

The kernel uses the legacy PIT at `250 Hz`, remaps the PIC to IRQ vectors
starting at 32, and installs IRQ0 for global timer ticks. After the PIT is
running, it calibrates the local APIC timer, enables a periodic LAPIC timer
interrupt on online cores, and records per-core local timer ticks. The IDT also
contains CPU exception stubs, PS/2 mouse IRQ handling, the LAPIC timer vector,
and the SMP IPI vector.

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
layout switching. Mouse input supports PS/2 and USB motion/buttons plus wheel
deltas; the desktop editor and console file browser consume wheel scrolling.

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
directory tables, contiguous file data, and a 512 KiB file cap. It supports
directories, parent links, scoped `ls`, `cd`, `pwd`, move/rename, copy, and
delete operations.

`lainfs` keeps a write-back cache for the active directory table and a small
data-sector cache for repeated file reads/writes. Use `fsflush` to flush dirty
cached blocks and print cache counters; `reboot` and `poweroff` also flush the
cache before leaving the OS.

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

AHCI is early but integrated with the block-device API. E1000 networking can
use DHCP, ARP, DNS, and plain HTTP fetches.

## Shell Commands

Core commands:

- `help`
- `clear`
- `echo text`
- `info`
- `cpus`
- `smp`
- `tasks`
- `tasktest`
- `ticks`
- `date`
- `reboot`
- `poweroff` / `shutdown`
- `heap`
- `heaptest [soak cycles]`
- `bgcolor 0xRRGGBB`
- `fgcolor 0xRRGGBB`
- `reg [list|get key|set key value]`
- `theme [list|lain|midnight|olive|plum]`
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
- `rm path` / `del path`
- `rename old new` / `mv old new`
- `cp source dest`
- `write name text`
- `cat name`
- `fsflush [drive:|all]`
- `browse [path-or-drive:]`

In the console file browser, use `D` to delete the selected file or empty
directory. Right-click an entry for the same delete action.

Hardware and UI:

- `ahci`
- `usb [subcommand]`
- `net`
- `net dhcp`
- `net resolve host`
- `wget http://host/path output`
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
- File browser delete support from the `Del` button or right-click menu.
- Modules window and module app windows.
- Native `.Z` editor with Save, Build, Inst, Load, and Log buttons.
- Registry-backed desktop themes via the `theme` and `reg` shell commands.
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
zmod zbrowser_module

zbuild libc_smoke_module
zmod libc_smoke_module.zo

zbuild clib_port_smoke_module
zmod mini_zlib.zo clib_port_smoke_module.zo
```

The example `autoexec` preloads useful modules and can enter desktop mode.
The file manager module supports toolbar deletion and a right-click Delete menu
for files and empty directories. It can open JPEG and PNG files in the image
viewer; decoding is provided by the kernel image service, so the viewer does
not need a resident decoder module.

The tiny Z browser module renders basic HTML from local files such as
`index.html`, plain HTTP URLs, HTTPS URLs, and Wikipedia article URLs via
Wikipedia's smaller mobile HTML endpoint.
For example:

```text
write index.html "<h1>Hello from Z Browser</h1><p>Local HTML is safe.</p>"
zbuild zbrowser_module
zmod zbrowser_module
desktop
```

## Kernel Export Surface

The kernel export table exposes selected symbols to assembler, `.Z`, linked
objects, and resident modules. Current groups include:

- console output: `puts`, `put_hex64`, `put_dec64`
- timer/clock: `ticks`, `clock_unix_time`, `clock_get_rtc_time`
- memory status: `mem_total_kb`, `mem_free_kb`, `mem_used_kb`
- libc compatibility: `malloc`, `calloc`, `realloc`, `free`, `memcpy`,
  `memset`, `memmove`, `memcmp`, `strlen`, `strcpy`, `strncpy`, `strcat`,
  `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strdup`, and
  `errno_location`
- CPU status: `cpu_count`, `cpu_usage`
- SMP work queue: `smp_submit_work`, `smp_work_done`, `smp_wait_work`,
  `smp_pending_work_count`
- graphics: `put_pixel`, `gfx_width`, `gfx_height`, `gfx_pitch`,
  `gfx_format`, `gfx_fill_rect`, `gfx_draw_rect`, `gfx_draw_line`,
  `gfx_clear`, viewport helpers
- image decode: `image_probe`, `image_decode_rgb24`,
  `image_decode_to_screen`, plus compatibility `jpg_*` aliases
- mouse helpers
- screen text helpers
- status bar
- filesystem/project APIs such as `os_write_file`, `os_read_file`,
  `os_http_get`, `os_zbuild`, `os_ztest`, `os_zinstall`, `os_zmod`,
  `os_zunload`, and `os_zreload`
- desktop helper: `os_open_editor`

See `kernel/core/kernel_exports.c` for the authoritative list.

`examples/libc_api.Z` declares the libc-compatible module ABI. It is the
starting point for porting small C libraries into `.zo` modules before taking
on larger browser dependencies. `examples/mini_zlib.Z` is a tiny zlib-style
library object used by `clib_port_smoke_module` to prove the multi-object
module pattern for reusable C-library ports.
`examples/zbrowser_html.Z` is the first browser library object; it owns HTML
tag/entity scanning while `zbrowser_module.Z` owns browser UI, fetching, and
rendering. `zmod zbrowser_module` reads the object-only zbuild manifest and
loads both objects in order. The next browser milestone is a kernel-owned
fetch/cache service with stronger cancellation, progress reporting, and cached
image fetches.

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
- no TSS/IST
- no userspace/process isolation
- no dynamic kernel module ABI beyond the `.Z` resident module experiment
- no NTFS driver inside the kernel
- heap is active: page-range allocation, `kmalloc`/`kfree` classes, basic
  canary/double-free diagnostics, fragmentation/failure reporting, bounded
  `heaptest` and `heaptest soak` diagnostics, and the largest driver/UI scratch
  buffers are on heap
- limited GOP pixel-format support
- tiny `lainfs` limits and contiguous file allocation
- early USB/xHCI enumeration
- early AHCI and network paths
- no full z-order-aware window compositor yet

## Good Next Work

Here are high-leverage next steps, roughly ordered by payoff:

1. Heap hardening.
   Add long desktop session leak checks that sample heap counters around real
   interactive shell, editor, browser, and module workflows.

2. Background kernel jobs.
   Move real shell and desktop jobs such as builds, file copies, and redraw
   preparation onto the cooperative task layer so long work can progress off
   the BSP.

3. Expand `lainfs`.
   Lift file count/file size limits, support non-contiguous extents, and add
   more robust metadata validation.

4. Desktop window damage model.
   Extend the current dirty-rectangle path into a z-order-aware compositor so
   moving a window redraws only exposed areas and the moved window itself.

5. Safer resident modules.
   Add dependency-aware unload, module ownership for resources, and better
   failure isolation when a module hook misbehaves.

6. USB input path.
   Make USB keyboard/tablet input first-class, not just experimental xHCI
   probing.

7. Self-hosting milestone.
   Keep moving small C/ASM helpers into `.Z` where it makes sense, then use
   `zbuild` to build more of the demo userland from inside the OS.

## Why No Long-Mode Switch?

UEFI x86_64 already enters the loader in 64-bit long mode. The BIOS-style
real-mode to protected-mode to long-mode path is only needed for legacy BIOS
boot. The AP trampoline still performs a real-mode startup path because that is
how x86 secondary CPUs begin after SIPI.
