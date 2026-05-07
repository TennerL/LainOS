# Self-Hosting Roadmap

Goal: make LainOS capable of developing itself from inside LainOS, with the
first major milestone being a kernel built locally by the in-OS Z toolchain.

## Current Checkpoint

The OS already has the first self-hosting rung:

- `.Z` source can be compiled in the kernel shell with `zc`, `zco`, and `zrun`.
- `.zo` objects can be linked with `zlink`.
- Multi-file projects can be built from a `target.zbuild` manifest with
  `zbuild target`.
- `ztest target` rebuilds and runs a target, then writes a test log.
- `zinstall target` rebuilds and copies the linked output into place.
- `.Z` programs can orchestrate other builds through `os_zbuild`,
  `os_ztest`, and `os_zinstall`.
- `examples/selfhost_project/` demonstrates a tiny source/include/build layout.
- The host build already compiles `kernel/z/zlink_probe.Z` into a NASM ELF object
  and links it into `kernel.elf`, proving that selected `.Z` code can live in
  the booted kernel.

That means LainOS can already build and run Z programs from inside itself. What
it cannot yet do is reproduce the bootable host-built kernel image.

## Try The Current Rung In The OS

Boot with a writable data disk:

```text
make run-bootdisk
```

The host build creates `build/data.img` with an MBR lainfs partition attached
to QEMU as the writable data disk. Fresh images are formatted and seeded with
the repo's `boot/`, `kernel/`, and `examples/` trees, so you can inspect and
move real files immediately instead of typing source into the shell by hand.
That is the preferred self-hosting workspace because it does not depend on
physical media:

```text
mount S: hd1p1
S:
ls
browse examples
```

Normal rebuilds preserve `build/data.img`. Run `make reseed-data` from the host
only when you intentionally want to reset it. To grow the writable workspace,
pass the desired image size when reseeding, for example
`make reseed-data DATA_SIZE_KB=131072`.

The useful smoke tests can run directly from the seeded examples directory:

```text
cd examples
ztest sysstat
zinstall sysstat
exec sysstat.bin
ztest zreport
zinstall zreport
exec zreport.bin
ztest zmake
zinstall zmake
exec zmake.bin
```

For the nested project layout:

```text
cd selfhost_project
ztest kernel
zinstall kernel
exec selfhost_project.bin
```

The `kernel` name in `examples/selfhost_project/kernel.zbuild` is intentionally
a tiny placeholder target. It exercises the shape of an in-OS kernel build
without pretending to be the real bootable kernel yet.

The kernel also creates a seeded live ramdisk for smoke tests. Without a
writable data disk it appears as `S:/examples`; when a real lainfs disk is
mounted as `S:`, the live examples are still available at `R:/examples`. That
workspace disappears when the VM reboots.

## Milestone 1: Make Z A Kernel Implementation Language

Keep the host linker in charge, but move small kernel services from C into `.Z`
one at a time.

Acceptance criteria:

- More files like `kernel/z/zlink_probe.Z` are compiled by the host build and
  linked into `kernel.elf`.
- `.Z` can express the needed kernel-facing declarations in shared headers.
- C callers and `.Z` functions agree on the x86_64 SysV ABI.
- Each migrated service has a host-build smoke test or an in-OS smoke command.

Good candidates:

- formatting helpers that do not allocate
- small math/string helpers
- status bar and dashboard helpers
- simple shell helpers that only touch exported kernel APIs

## Milestone 2: Emit Boot-Linkable Objects From Z

The in-OS `.zo` format is built for flat executables and resident modules. The
bootable kernel still needs ELF64 relocation/link behavior, or a boot path that
can load a different kernel image format.

Choose one path:

- teach the in-OS linker to emit ELF64 relocatable objects or an ELF64 kernel
- teach the bootloader to load a simpler native kernel image produced by
  `zbuild`
- keep a tiny host-built ELF loader kernel and make the real kernel a Z-built
  module image loaded from lainfs

Acceptance criteria:

- A Z-built artifact can be selected at boot.
- Symbols, data, BSS, and relocations are represented explicitly.
- The artifact has a stable entry convention and receives `boot_info`.
- A failed build cannot overwrite the last known-good boot image.

## Milestone 3: Build The Kernel From Inside LainOS

Add an in-OS kernel build project that mirrors the real kernel layout:

```text
kernel.zbuild
include/
src/
build/
```

Acceptance criteria:

- `ztest kernel` builds a non-booting smoke image and validates exported tests.
- `zinstall kernel next-kernel.bin` installs a boot candidate.
- The bootloader can pick `next-kernel.bin` or fall back to the previous kernel.
- Build logs are saved in lainfs and are readable after reboot.

## Milestone 4: Develop The OS On Itself

This is the practical daily-driver layer.

Needed pieces:

- a source tree copied or checked out into lainfs
- enough editor ergonomics for multi-file work
- larger files and more directory entries in lainfs
- path-aware shell commands and build errors
- a way to sync source changes back to the host during transition
- a boot menu or config file for last-good and next-kernel selection

The narrow next engineering move should be Milestone 1: migrate one tiny
kernel helper into `.Z`, link it into `kernel.elf`, and keep the C interface
unchanged. That builds confidence without needing the full bootable image
pipeline all at once.
