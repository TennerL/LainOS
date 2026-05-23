# Today Agenda

Date: 2026-05-23

## Current NetSurf Port Blocker

- Branch: `zbrowser-netsurf-port`
- Latest 2026-05-23 22:10-22:20 Europe/Berlin timing pass:
  - Host compile gate still passes: `./scripts/zbrowser-compile-smoke.sh`
  - Default self-host smoke behavior in this cron environment is still the intended fast-fail skip:
    - command: `./scripts/zbrowser-selfhost-smoke.sh`
    - result: `Skipping zbrowser self-host smoke; KVM is unavailable (/dev/kvm is not accessible for uid=1000 gid=1000; groups=administrator adm cdrom sudo dip plugdev users). Set ZBROWSER_SELFHOST_ALLOW_TCG=1 to force slow TCG.`
  - Forced TCG timing probe still proves the in-OS zbrowser install path completes on this tree:
    - command: `ZBROWSER_SELFHOST_ALLOW_TCG=1 ./scripts/zbrowser-selfhost-smoke.sh`
    - result: `zbrowser self-host smoke: ok (timeout=420s elapsed=200s)`
  - Patch landed in this pass:
    - `scripts/zbrowser-selfhost-smoke.sh` now reports actual QEMU wall time on success and failure so future cron runs can compare `tcg` vs `kvm` runs numerically instead of only by timeout outcome
  - Current conclusion from this pass:
    - the cron/runtime blocker is firmly in the emulator profile difference, not in a reproducible self-host compiler deadlock on the current tree
    - use the measured `elapsed=200s` TCG result as the baseline until a KVM-capable cron environment is available
- Latest 2026-05-23 22:00-22:10 Europe/Berlin QEMU harness pass:
  - Host compile gate still passes: `./scripts/zbrowser-compile-smoke.sh`
  - In this cron environment, `./scripts/zbrowser-selfhost-smoke.sh` skips by default because `/dev/kvm` exists but is not usable by `administrator`; the exact reason is permission/group access, not missing QEMU support.
  - Forced TCG diagnosis now proves the self-host path is not inherently stuck:
    - command: `ZBROWSER_SELFHOST_ALLOW_TCG=1 ZBROWSER_SELFHOST_TIMEOUT_SECONDS=420 ./scripts/zbrowser-selfhost-smoke.sh`
    - result: `zbrowser self-host smoke: ok (timeout=420s)`
    - final serial progress: `zbuild: zbrowser_module.Z -> zbrowser_module.zo bytes=1163181` -> `zbuild: built 2 module object(s)` -> `zinstall: installed 2 module object(s)` -> `powering off...`
  - Exact harness/runtime difference versus Tenno's real PC clue:
    - this environment falls back to `runtime=tcg smp=1 mem=256M`
    - the fast path expects usable KVM and the script's KVM profile (`runtime=kvm smp=4 mem=2048M`)
  - Patch landed in this pass:
    - `scripts/zbrowser-selfhost-smoke.sh` now reports the concrete KVM unavailability reason and uses a larger default timeout when TCG is explicitly forced, while keeping the bounded 180s default for KVM runs
  - Current conclusion from this pass:
    - the missing `mods/zbrowser_module.buildlog` result from recent headless runs is explainable by the QEMU harness/runtime profile here
    - do not treat the 180s TCG timeout as evidence that `zinstall zbrowser_module` is intrinsically too slow on real hardware
- Latest 2026-05-23 21:10-21:20 Europe/Berlin bounded verification pass:
  - Host compile gate still passes: `./scripts/zbrowser-compile-smoke.sh`
  - Focused CSS-prep repro still passes: `./scripts/zbrowser-css-panic-repro.sh`
    - terminal lines: `css-trace style-prepare-precompute-done`, `css-repro style-result=4`, `ztest: result=0 expected=0 ok`
  - Async/SMP CSS-prep repro still passes: `./scripts/zbrowser-css-async-repro.sh`
    - terminal lines: `css-trace style-prepare-precompute-done`, `css-async style-result=4`, `css-async final-dom-status=127 render-dom-status=255 rewritten=2408 style-result=4`, `ztest: result=0 expected=0 ok`
  - In-OS compile gate still fails to finish within the bounded headless window: `./scripts/zbrowser-selfhost-smoke.sh`
    - exact failure: `zbrowser self-host smoke: missing mods/zbrowser_module.buildlog`
    - exact last serial progress: `zinstall zbrowser_module` -> `zclean: removed 0 artifact(s)` -> `zbuild: zbrowser_html.Z -> zbrowser_html.zo bytes=26236`
  - Current conclusion from this pass:
    - the urgent `preparing css` panic is not reproduced by the focused browser-open/CSS-prep probes on this tree
    - the actionable blocker for the next bounded patch is still the in-OS `zinstall zbrowser_module` stall after `zbrowser_html.zo` is built
- Latest 2026-05-23 CSS panic fix pass:
  - Focused host compile gate still passes: `./scripts/zbrowser-compile-smoke.sh`
  - Focused runtime repro now passes: `./scripts/zbrowser-css-panic-repro.sh`
  - Exact previously captured panic before the fix:
    - status path: `css-repro prepare-css` -> `css-trace precompute-select node=0 tag=html pos=0` -> `css-trace select-start node=0 tag=html pos=0`
    - exception: `KERNEL PANIC: CPU exception`
    - `vector=13 general protection fault error=0x0000000000000000`
    - `rip=0x0000000004163CAA`
    - symbol from `addr2line -e build/kernel.elf 0x0000000004163CAA -f -C`: `css__initial_clip`
  - Root cause for the panic: stale NetSurf/libcss objects in `build/` were still carrying SSE stack-local codegen (`movaps`/`xmm`) even though the current browser/kernel path expects no-SSE builds. The first `css_select_style()` call entered that stale `css__initial_clip()` body and faulted immediately.
  - Patch landed in this pass:
    - `Makefile`: keep NetSurf/browser bridge builds on `-mno-mmx -mno-sse -mno-sse2 -msoft-float` and add hash-named CFLAGS stamp dependencies so NetSurf/kernel objects rebuild when effective flags change
    - `kernel/arch/x86_64/interrupts.asm` + `kernel/core/cpu.c`: route exception stubs through `cpu_exception_handler()` so future browser faults report vector/RIP/CR2 instead of a blank `KERNEL PANIC:`
  - Verified fixed runtime outcome from the serial repro log:
    - CSS precompute advances through all 49 nodes
    - terminal lines: `css-trace style-prepare-precompute-done`, `css-repro style-result=4`, `ztest: result=0 expected=0 ok`
    - this clears the urgent `preparing css` kernel panic in the focused browser-open/CSS-prep path
  - Remaining verification gap for next pass:
    - rerun `make build/kernel.elf zcc-smoke lainfs-smoke`
    - rerun `./scripts/agent-browser-check.sh`
    - the focused panic repro is green, but the full automation gate was not carried through to completion inside this bounded pass
- Reproduced the real zbrowser compile failure with the internal object path, not `zcc-smoke`: `build/tools/zmod_link_host examples/zbrowser_html.Z /tmp/zbrowser_html.zo examples/zbrowser_module.Z /tmp/zbrowser_module.zo`
- Exact failing file/line before the fix: generated asm for `examples/zbrowser_module.Z`, `zobject_from_asm` status `-83626`, first visible failing asm line `80626` (`mov qword [zbrowser_image_buffer_addr], rax`)
- Root cause: `kernel/z/zobject.c` capped object relocations at `4096`, but `zbrowser_module` now needs `4118` relocations through the in-kernel assembler/object path
- Fix landed: raised `ZOBJECT_MAX_RELOCATIONS` to `16384` and added `scripts/zbrowser-compile-smoke.sh` so `scripts/agent-browser-check.sh` catches this end-to-end object/link path instead of only raw asm generation
- Automation now bootstraps NetSurf submodules and generated parser files before the browser build gate.
- `scripts/zbrowser-compile-smoke.sh` now passes: host `zcc_host` emits `build/zbrowser-smoke/zbrowser_module.asm`, `zmod_link_host` builds both `.zo` objects, and links them successfully.
- `scripts/agent-browser-check.sh` now passes `build/kernel.elf`, `zcc-smoke`, `lainfs-smoke`, and full image generation.
- Full image/QEMU verification needed PATH normalization because OpenClaw/Codex did not include `/usr/sbin`.
- After PATH normalization, `scripts/agent-browser-check.sh` also builds `boot.iso`, MBR disk image, and GPT disk image with `mkfs.fat`, `sgdisk`, `mtools`, and `xorriso`.
- Latest 2026-05-23 QEMU harness finding:
  - The cron environment has `/dev/kvm` and QEMU reports `kvm`, but the `administrator` user here is not in the `kvm` group, so `qemu-system-x86_64 -enable-kvm ...` fails with `Could not access KVM kernel module: Permission denied`.
  - That means the headless self-host smoke was running on 1 vCPU TCG emulation (`CPU cores online/detected: 1/1` in serial), which does not match Tenno's fast real-PC path and is a harness/runtime difference, not evidence that `zinstall zbrowser_module` is inherently slow on target hardware.
  - `scripts/zbrowser-selfhost-smoke.sh` now detects usable KVM explicitly, skips by default when only slow TCG is available, exposes the selected runtime/smp/memory in output, and preserves the real QEMU exit status instead of swallowing it through `if ! timeout ...`.
  - Manual override remains available for diagnosis via `ZBROWSER_SELFHOST_ALLOW_TCG=1`.
- New host-side probe landed: `tools/lainfs_check_host` now supports `exists`, `cat`, and `ls`, and `scripts/zbrowser-selfhost-smoke.sh` now seeds a temporary smoke image with a root `autoexec`, boots QEMU headlessly, and inspects the resulting LainFS image for `mods/zbrowser_*` artifacts.
- Host-side compile is still green after the smoke-harness changes:
  - `make build/tools/zcc_host zcc-smoke`
  - `scripts/zbrowser-compile-smoke.sh`
- Remaining blocker is now precise and reproducible in the self-host smoke path:
  - Command: `scripts/zbrowser-selfhost-smoke.sh`
  - Exact failure: after the QEMU timeout, `build/tools/lainfs_check_host exists build/zbrowser-selfhost.data.img mods/zbrowser_module.buildlog` fails, `mods/` does not exist, and only the seeded root files remain.
  - Reproduced on both boot attempts tried in the harness: ISO boot (`-cdrom build/boot.iso`) and direct ESP boot (`-drive format=raw,file=build/esp.img,if=ide,index=0`).
  - The temporary smoke image does contain a root `autoexec`, so this is no longer a missing-script issue; the guest is still not leaving any observable `mkdir mods`/`zinstall zbrowser_module` side effects before timeout.
  - Suspected root cause: the bounded headless boot path is not yet reaching or completing `shell_run_autoexec("autoexec")`, or there is no host-visible boot/runtime signal to prove where it stalls.
- Next concrete patch: make the self-host smoke observable earlier in boot, most likely by exposing boot/runtime output over a host-capturable serial/debug console or by writing a very early boot marker/result file to the data image before the full zbrowser build/install sequence.
- Latest 2026-05-23 pass on the CSS panic work:
  - Hardened `kernel/ui/weblayout.c` and `examples/zbrowser_module.Z` so `web_style_prepare_document()` now returns explicit failure states (`styles unavailable`, `style node limit hit`, `style rule limit hit`) and zbrowser no longer overwrites those statuses with `loaded http` / `loaded local file`.
  - Host gates stayed green after that patch:
    - `./scripts/zbrowser-compile-smoke.sh`
    - `make build/kernel.elf zcc-smoke lainfs-smoke`
  - Full verification still fails in `./scripts/agent-browser-check.sh` on the self-host leg, but the current failure is narrower than the earlier `mods/ missing` state:
    - After the timeout, `mods/` does exist and contains the copied zbrowser sources/manifests, but `mods/zbrowser_module.buildlog` is still missing.
    - Exact current failure text: `zbrowser self-host smoke: missing mods/zbrowser_module.buildlog`
    - Current observable state from `build/tools/lainfs_check_host ls build/zbrowser-selfhost.data.img mods`: `hwdash_module.*`, `taskmgr_module.*`, `filemgr_module.*`, `zbrowser_module.Z`, `zbrowser_html.Z`, `zbrowser_html_api.Z`, `zbrowser_module.zbuild`, `zbrowser_smoke.html`, `personalize_module.*`, `image_viewer*`
  - This means the guest is now getting far enough into `examples/autoexec` to populate `/mods`, but it is still not reaching a completed `zinstall zbrowser_module` result within the bounded headless run.
  - Next concrete patch from here: expose a host-capturable boot/runtime trace for the self-host smoke so the stall can be pinned to the exact `autoexec` command, then retry the browser-open panic repro after that runtime path is observable.
  - Latest 2026-05-23 CSS panic isolation pass:
    - New focused repro harness: `./scripts/zbrowser-css-panic-repro.sh`
    - Minimal in-OS repro target: `examples/zbrowser_css_repro.Z` + `examples/zbrowser_css_repro.zbuild`
    - Exact serial repro sequence:
      - `css-repro alloc`
      - `css-repro load`
      - `css-repro input-bytes=2494`
      - `css-repro rewrite-dom`
      - `css-repro dom-status=127 rewritten=2508`
      - `css-repro rewrite-render-dom`
      - `css-repro render-dom-status=255 rewritten=2408`
      - `css-repro prepare-css`
      - `KERNEL PANIC:`
    - This reproduces the crash without desktop/module clicks and proves the panic happens inside the CSS preparation path reached by `web_style_prepare_document()`, after NetSurf DOM rewrite/render rewrite, before any `zbrowser_css_repro.testlog` can be written.
    - Small hard bug fixed during the pass: `kernel/ui/weblayout.c:web_css_style_for_node()` was passing an uninitialized `css_media` struct into libcss; it is now zero-initialized and populated with basic screen defaults. This did not remove the panic.
    - Current suspected function chain:
      - `web_style_prepare_document()`
      - `web_css_precompute_styles()`
      - `web_css_style_for_node()`
      - `css_select_style(...)` or immediately after in computed-style application
    - Current suspected root cause: logic/adapter bug in the libcss selection bridge, not a realistic size/capacity limit. The repro page is only `2494` bytes before rewrite and the current guard statuses for node/rule saturation are not reached.
  - Latest 2026-05-23 CSS panic no-SSE follow-up:
    - Host compile gate still passes after the latest browser/runtime patch set:
      - `./scripts/zbrowser-compile-smoke.sh`
    - `kernel/ui/weblayout.c` no longer uses a designated-initializer local `css_unit_ctx`; it now `memset()`s the struct before filling viewport/font fields so the function prologue does not synthesize XMM zeroing before the first trace point.
    - `Makefile` now forces the browser bridge objects plus NetSurf/libcss/libdom/libhubbub/libparserutils/libwapcaplet builds through `-mno-mmx -mno-sse -mno-sse2 -msoft-float` instead of changing all kernel objects.
    - Static verification after rebuild:
      - `objdump -d build/kernel/ui/weblayout.o build/kernel/ui/netsurf_port.o build/third_party/netsurf/libcss/src/select/select.o | rg 'xmm|movap|movups|pxor|xorps|addss|mulss|cvt|ucomis|comis'`
      - result: no matches in the sampled browser/libcss objects, so the remaining panic is no longer explained by obvious SSE instructions in those rebuilt objects
    - Runtime verification is currently blocked by an active QEMU image lock during `./scripts/zbrowser-css-panic-repro.sh`:
      - exact failure: `qemu-system-x86_64: Failed to get "write" lock`
      - exact follow-up: `Is another process using the image [build/esp.img]?`
      - the repro script then reports `zbrowser CSS panic repro: missing mods/zbrowser_css_repro.testlog`
    - Next concrete patch from here:
      - rerun `./scripts/zbrowser-css-panic-repro.sh` once the image lock clears
      - capture the first post-`css-trace select-start` trace or panic text from the new no-SSE build
      - if the panic remains inside `css_select_style(...)`, inspect libcss callback/selection assumptions rather than compiler-generated SIMD
  - Latest 2026-05-23 self-host observability improvement:
    - `scripts/zbrowser-selfhost-smoke.sh` now captures a serial log and prints its tail on failure.
    - Latest bounded self-host stall point from `./scripts/agent-browser-check.sh`:
      - `zinstall zbrowser_module`
      - `zclean: removed 0 artifact(s)`
      - `zbuild: zbrowser_html.Z -> zbrowser_html.zo bytes=26236`
      - then no further progress before the QEMU timeout and missing `mods/zbrowser_module.buildlog`
    - This narrows the in-OS compiler/tooling blocker to the zbrowser multi-object install path after `zbrowser_html.zo` is built, before `zbrowser_module.zo`/module build completion is persisted.
  - Next concrete patch from here:
    - add a short libcss bridge trace around `web_css_precompute_styles()` / `web_css_style_for_node()` so the first crashing node and the last successful selector/computed-style step are captured in the serial log
    - then rerun `./scripts/zbrowser-css-panic-repro.sh`
    - after that, revisit the self-host `zinstall zbrowser_module` stall with the now-working serial capture if the CSS panic is resolved
  - Latest 2026-05-23 bounded compile-and-repro pass:
    - Host compile gate still passes:
      - `./scripts/zbrowser-compile-smoke.sh`
    - Full verification still fails in the in-OS compiler/tooling leg:
      - `./scripts/zbrowser-selfhost-smoke.sh`
      - `./scripts/agent-browser-check.sh`
    - Exact current self-host stall signature from the serial tail:
      - `zinstall zbrowser_module`
      - `zclean: removed 0 artifact(s)`
      - `zbuild: zbrowser_html.Z -> zbrowser_html.zo bytes=26236`
      - then no further progress before the QEMU timeout and missing `mods/zbrowser_module.buildlog`
    - Focused CSS preparation repros are currently green in this environment, so the reported browser-open panic is not reproduced by the smallest isolated paths tried in this pass:
      - `./scripts/zbrowser-css-panic-repro.sh`
      - terminal lines: `css-trace style-prepare-precompute-done`, `css-repro style-result=4`, `ztest: result=0 expected=0 ok`
      - new async/SMP probe: `./scripts/zbrowser-css-async-repro.sh`
      - terminal lines: `css-trace style-prepare-precompute-done`, `css-async style-result=4`, `css-async final-dom-status=127 render-dom-status=255 rewritten=2408 style-result=4`, `ztest: result=0 expected=0 ok`
    - Files added for the new focused async repro:
      - `examples/zbcss_async.Z`
      - `examples/zbcss_async.zbuild`
      - `scripts/zbrowser-css-async-repro.sh`
    - Current suspected root cause for the remaining blocker:
      - not a CSS selector/preparation crash on the smoke page, but an in-OS `zinstall zbrowser_module` stall after `zbrowser_html.zo` is produced and before the full module install/buildlog is persisted
    - Next concrete patch:
      - instrument the in-OS zbrowser install path so the serial/data-image trace proves whether the stall is in `zbuild` for `zbrowser_module.Z`, object link/writeback, or post-build module install
      - only return to the browser-open panic once that runtime path is observable again or the stall is cleared

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
