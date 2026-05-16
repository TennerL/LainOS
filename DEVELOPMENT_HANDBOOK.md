# LainOS Development Handbook

This handbook is for people extending LainOS itself, resident `.zo` modules,
and small `.Z` applications. It assumes you have read the short overview in
`README.md`; this file is the practical "how to work without stepping on the
rakes" guide.

LainOS is still a hobby OS. Favor small, testable changes, preserve working
boot paths, and keep correctness ahead of cleverness.

## Daily Workflow

Build everything from the host:

```bash
make clean
make
```

Boot the normal QEMU image:

```bash
make run
```

Boot with a persistent writable disk:

```bash
make run-bootdisk
```

Inside the OS, a useful persistent workspace flow is:

```text
mount S: hd1p1
S:
ls
cd examples
```

Normal host rebuilds preserve `build/data.img`. Use `make reseed-data` only
when you intentionally want to reset the writable data disk.

For fast kernel compile checks, this is usually enough:

```bash
make build/kernel.elf
```

For image or bootloader changes, run the full `make`.

## Source Map

- `bootloader/main.c` loads `kernel.elf`, selects GOP mode, builds `boot_info_t`,
  exits boot services, and jumps to the kernel.
- `boot/shared/` contains the boot protocol shared by loader, kernel C, and ASM.
- `kernel/core/` owns kernel init, CPU/SMP, timer, DMA, heap, exports, and power.
- `kernel/arch/x86_64/` owns entry, interrupt stubs, AP startup, and CPU helpers.
- `kernel/drivers/` owns framebuffer graphics, console, input, PCI, storage,
  AHCI, USB, and networking experiments.
- `kernel/fs/` owns `lainfs`.
- `kernel/ui/` owns shell, editor, browser, and desktop.
- `kernel/z/` owns the assembler, `.Z` compiler, and `.zo` object/linker logic.
- `examples/` contains `.Z` apps, modules, build manifests, and `autoexec`.
- `tools/` contains host utilities for seeding images and compiling test inputs.

## Kernel Development Rules

Keep changes small and bootable. A kernel bug often looks like a black screen,
so avoid combining unrelated risks in one patch.

Prefer existing local patterns:

- Use `kmalloc`, `kzalloc`, and `kfree` for owned dynamic memory.
- Use `page_alloc` and `page_free` for whole-page allocations.
- Use `dma_alloc` only for DMA-visible device memory.
- Use `console_puts` and `console_kprintf*` for early diagnostics.
- Use existing `zero_memory`, `copy_*`, and text helpers in nearby files.
- Keep kernel code freestanding: no libc assumptions.

Be conservative with static buffers. Static state is fine for tiny permanent
tables and hardware state, but large scratch buffers should usually live on the
heap and have a clear owner.

When adding an exported kernel API:

1. Implement the C function in the owning subsystem.
2. Add a declaration to the relevant kernel header.
3. Export it in `kernel/core/kernel_exports.c`.
4. Add matching `extern` declarations to `examples/kernel_api.Z` and the
   mirrored `examples/zlang/.../kernel_api.Z` headers when Z code should use it.
5. Prefer simple integer and pointer signatures. Keep ABI shape obvious.

## Memory And Heap

Heap initialization happens after DMA setup. The allocator currently provides:

- page-range allocation
- small `kmalloc` size classes
- larger page-backed allocations
- `kzalloc`
- `kfree`
- heap statistics, fragmentation hints, and fault counters through shell command
  `heap`
- bounded allocation/failure stress diagnostics through shell command `heaptest`

Good practices:

- Check allocation failure. The OS often has enough memory in QEMU, but the
  code should still fail gracefully.
- Free memory when a window, resident module, or temporary browser/editor
  session closes.
- Avoid allocating from IRQ paths unless the subsystem is explicitly designed
  for it.
- Avoid keeping pointers into buffers that may be freed by another owner.
- Keep per-core CPU/SMP state in the core-state table rather than adding new
  loose per-core arrays.

Useful shell commands:

```text
heap
heaptest
info
cpus
```

## Desktop And Rendering

The desktop uses a lazily allocated backbuffer. Full redraws paint into that
backbuffer and flush damaged rectangles to the real framebuffer.

Important rule: content-only updates may use bounded dirty rectangles, but
geometry and overlay transitions should be conservative until there is a real
z-order-aware compositor.

Use full damage for:

- opening or restoring module app windows
- closing or minimizing any window
- drag/resize commits
- maximize/restore
- start-menu open, close, or selection
- any change that can expose another window underneath

Bounded damage is acceptable for:

- editor text area updates
- editor status-only/content-only redraws
- module tick content inside an already visible module viewport
- simple file/module panel content updates where no overlap relationship changes

Resident modules are drawn through a viewport. Inside `zmodule_redraw`, module
code sees `gfx_width()` and `gfx_height()` for its own content area, not the
whole screen. A module should repaint its entire content area when asked to
redraw. Incremental module drawing belongs in `zmodule_tick`, but even then the
module should be able to reconstruct a full frame from `zmodule_redraw`.

If window content disappears until the window is moved, suspect a dirty-rect
that was too narrow. Promote that transition to `desktop_damage_full()` first;
only optimize back down after the compositor can prove the exposed regions.

## Shell And Filesystem

The shell is the integration hub. It owns:

- commands and command dispatch
- mounted drive/session state
- in-OS assembler/compiler/linker command flow
- resident module load/unload/reload commands
- project commands such as `zbuild`, `ztest`, and `zinstall`

`lainfs` is intentionally simple. Current limits matter:

- small directory tables
- contiguous file allocation
- limited maximum file size
- no journaling

When developing filesystem features, preserve old images where possible, or add
clear format-version handling. The shell should print specific errors for disk
full, directory full, malformed paths, and missing files.

Delete behavior is intentionally conservative: `rm`, the console browser, and
the desktop Files window delete files and empty directories, while non-empty
directories are refused until recursive delete has explicit UI/confirmation.

## Z Language Development

`.Z` is a small C-like language compiled inside the kernel. It is meant for
small apps, modules, demos, tests, and eventually selected kernel-adjacent code.

Common commands:

```text
zrun examples/zlang/hello.Z
zc source.Z output.bin
zco source.Z output.zo
zlink source.zo output.bin
exec output.bin
zasm source.Z output.asm
```

Project commands use `.zbuild` manifests:

```text
zbuild taskmgr_module
ztest sysstat
zinstall sysstat
```

A minimal module manifest looks like:

```text
include .
module
taskmgr_module.Z taskmgr_module.zo
```

Use `#include "kernel_api.Z"` for kernel APIs in examples. Keep API declarations
in sync with `kernel/core/kernel_exports.c`.

## Resident Modules

Resident modules are linked `.zo` images kept in kernel memory. They can export:

- `zmodule_tick`
- `zmodule_redraw`
- `zmodule_unload`

Lifecycle:

```text
zbuild taskmgr_module
zmod taskmgr_module.zo
zmods
zreload taskmgr_module
zunload taskmgr_module
```

Desktop workflow:

```text
zbuild taskmgr_module
zmod taskmgr_module.zo
desktop
```

The `.Z` file manager module is the primary desktop file UI. Keep file actions
there first; the built-in desktop Files window is fallback UI.

Then open the module from the Modules window or Start menu.

The `zbrowser_module` example is the smallest useful web-surface: it renders
basic tags from `index.html`, or it fetches a page when `browser.url` contains
an `http://host[:port]/path` or `https://host[:port]/path` URL. HTTPS uses the
in-kernel TLS client with SNI, CMOS RTC time, and a small built-in CA anchor
set for certificate validation. Run `net dhcp` first when a DHCP server is
available, or set `net ip` and `net dns` manually. A quick loop is:

```text
write browser.url http://10.0.2.2:8000/index.html
zbuild zbrowser_module
zmod zbrowser_module.zo
desktop
```

Module rendering guidelines:

- `zmodule_redraw` should clear and repaint the full viewport.
- `zmodule_tick` should be cheap. If it changes visible state, update only what
  changed, but keep `zmodule_redraw` complete.
- Do not assume a fixed window size. Read `gfx_width()` and `gfx_height()`.
- Keep persistent state in globals.
- Release owned resources in `zmodule_unload` when applicable.
- Avoid busy loops. The desktop already ticks modules periodically.

## Adding A Z App Or Module

1. Create `examples/my_app.Z` or `examples/my_module.Z`.
2. Include `kernel_api.Z` if you need kernel APIs.
3. Add a `.zbuild` file beside it.
4. Build in the OS with `zbuild my_app` or `zbuild my_module`.
5. For apps, install and run:

```text
zinstall my_app
exec my_app.bin
```

6. For modules, load and inspect:

```text
zmod my_module.zo
zmods
desktop
```

7. Add the file to `examples/autoexec` only if it is useful enough to preload
   on most boots.

## Debugging Checklist

Black screen or boot hang:

- Rebuild `build/kernel.elf`.
- Check whether the failure happens before or after `console ready` boot stage.
- Add short `boot_stage("name")` markers in `kernel/core/main.c` for init bugs.
- Keep changes small enough to bisect manually.

Window content disappears:

- Check whether the transition changes geometry, visibility, z-order, or overlay
  state.
- If yes, use `desktop_damage_full()` before `desktop_redraw_all()`.
- If no, verify the dirty rectangle fully covers changed pixels.
- For modules, verify `zmodule_redraw` repaints the full viewport.

Module loads but does not appear:

- Run `zmods`.
- Check exported symbol names.
- Make sure the module was built with `module` in its `.zbuild`.
- Open it from Desktop's Modules window or Start menu.

Compiler or build failure:

- Try `zasm source.Z output.asm` and inspect generated assembly.
- Run `zclean target`, then `zbuild target`.
- Check include paths in `.zbuild`.
- Keep generated output under current file-size limits.

Filesystem surprise:

- Run `mounts`, `pwd`, and `ls`.
- Verify the active drive with `S:` or `C:`.
- Remember that `R:`/ramdisk contents disappear after reboot.
- Do not format a persistent disk unless you intend to erase it.

## Testing Expectations

Before finishing a kernel-facing change, at minimum run:

```bash
make build/kernel.elf
```

For boot, storage, or image layout changes, run:

```bash
make
make run-bootdisk
```

Useful in-OS smoke tests:

```text
heap
cpus
smp
cd examples
ztest sysstat
ztest zreport
zbuild taskmgr_module
zmod taskmgr_module.zo
desktop
```

For desktop rendering changes, manually check:

- open and close each built-in window
- open two module windows
- open and close the Start menu over a module window
- drag and resize windows
- maximize and restore windows
- type in the editor
- run `smp` while `taskmgr_module` is open

## Good Engineering Habits

- Prefer obvious control flow over clever compression.
- Keep APIs narrow and stable.
- Use explicit capacities with buffers.
- Preserve old behavior unless the change intentionally replaces it.
- Keep README-level docs current when user-facing workflows change.
- Record known incompleteness honestly. It helps the next developer choose the
  right level of caution.

## Current Sharp Edges

- No process isolation or userspace.
- No preemptive scheduler.
- No full z-order-aware compositor.
- Early USB/xHCI paths.
- Early AHCI/network paths.
- `lainfs` is intentionally tiny and contiguous.
- Module ABI is experimental.
- The OS identity-maps through firmware-provided page tables.

These are not reasons to avoid working on the OS. They are just the boundaries
you should design within.
