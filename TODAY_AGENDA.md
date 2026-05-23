# Today Agenda

Date: 2026-05-23

## Current NetSurf Port Blocker

- Branch: `zbrowser-netsurf-port`
- Reproduced the real zbrowser compile failure with the internal object path, not `zcc-smoke`: `build/tools/zmod_link_host examples/zbrowser_html.Z /tmp/zbrowser_html.zo examples/zbrowser_module.Z /tmp/zbrowser_module.zo`
- Exact failing file/line before the fix: generated asm for `examples/zbrowser_module.Z`, `zobject_from_asm` status `-83626`, first visible failing asm line `80626` (`mov qword [zbrowser_image_buffer_addr], rax`)
- Root cause: `kernel/z/zobject.c` capped object relocations at `4096`, but `zbrowser_module` now needs `4118` relocations through the in-kernel assembler/object path
- Fix landed: raised `ZOBJECT_MAX_RELOCATIONS` to `16384` and added `scripts/zbrowser-compile-smoke.sh` so `scripts/agent-browser-check.sh` catches this end-to-end object/link path instead of only raw asm generation
- Automation now bootstraps NetSurf submodules and generated parser files before the browser build gate.
- `scripts/zbrowser-compile-smoke.sh` now passes: host `zcc_host` emits `build/zbrowser-smoke/zbrowser_module.asm`, `zmod_link_host` builds both `.zo` objects, and links them successfully.
- `scripts/agent-browser-check.sh` now passes `build/kernel.elf`, `zcc-smoke`, `lainfs-smoke`, and full image generation.
- Full image/QEMU verification needed PATH normalization because OpenClaw/Codex did not include `/usr/sbin`.
- After PATH normalization, `scripts/agent-browser-check.sh` also builds `boot.iso`, MBR disk image, and GPT disk image with `mkfs.fat`, `sgdisk`, `mtools`, and `xorriso`.
- Remaining blocker: there is still no reliable scripted way to boot LainOS and scrape the in-OS `autoexec`/`zinstall zbrowser_module` result. The current self-host path exists in `examples/autoexec`, but the repo lacks serial/log capture or a host-side LainFS result-file probe.
- Next concrete patch: add a bounded self-host smoke harness, most likely by either exposing boot/runtime logs over a host-capturable console path or by adding a tiny host reader for a result file written by a dedicated zbrowser self-host autoexec.

## Previous Agenda

Date: 2026-05-21

## Goal

Stabilize the OS foundation after the repaired filesystem/background-jobs merge, then start turning the recovered features into reliable kernel services.

## Priority 1: LainFS Safety

- [x] Add mount-time `fscheck` status reporting for writable LainFS drives.
- [x] Keep boot safe: warn only, no automatic repair yet.
- [x] Improve `fsrepair` diagnostics so failures clearly name the reason and entry id.
- [x] Verify with `make lainfs-smoke`.
- [ ] Verify on real PC with:
  - `fscheck S:`
  - `fsrepair S:`
  - `fscheck S:`

Acceptance:
- Clean filesystem reports `ok`.
- Known repairable smoke cases still repair.
- Bad/unrepairable cases report a precise reason without panic.

## Priority 2: Background Job Service

- [x] Clarify shell background jobs vs kernel task queue vs SMP work queue.
- [x] Make `tasks` show useful background job state.
- [x] Add task names/status where practical.
- [x] Make completed jobs release their task slots reliably.
- [x] Verify with `make`.
- [ ] Verify on real PC with:
  - `tasktest`
  - `tasktest &`
  - `tasks`
  - `wait`

Acceptance:
- `tasktest` completes.
- `tasks` shows pending/completed state clearly.
- No hangs on single-core or multi-core boot.

## Priority 3: Module Runtime Hardening

- [x] Prevent unloading a module while callbacks are running.
- [x] Improve `zmod` unresolved-symbol diagnostics.
- [x] Track loaded module metadata clearly in `zmods`.
- [x] Verify with `make`.
- [ ] Verify on real PC with:
  - `zmod hwdash`
  - `zmods`
  - `zunload hwdash`
  - `zmod zbrowser`
  - open/close/reload from desktop Modules

Acceptance:
- Existing modules install and load.
- Failed module loads explain the missing symbol or failure point.
- Closing/reloading desktop modules does not destabilize the shell.

## Priority 4: Boot Diagnostics Mode

- [x] Add a simple safe/debug boot path.
- [x] Safe mode should skip `autoexec`.
- [x] Debug mode should preserve verbose boot/driver information.
- [x] Verify with `make`.
- [ ] Verify on real PC with:
  - `bootmode safe`
  - reboot and confirm `autoexec` is skipped
  - `bootmode debug`
  - reboot and confirm debug mode banner appears
  - `bootmode normal`

Acceptance:
- Normal boot remains unchanged.
- Safe boot reaches shell without running startup scripts.

## Parking Lot

- Module-owned heap/resource tracking.
- `zmodtest` reload stress command for resident modules.
- Journaled or transactional LainFS metadata writes.
- Driver fault containment for USB/xHCI and e1000.
- Better desktop settings persistence UX.
