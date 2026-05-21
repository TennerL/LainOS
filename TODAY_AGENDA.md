# Today Agenda

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

- Journaled or transactional LainFS metadata writes.
- Module-owned heap/resource tracking.
- Driver fault containment for USB/xHCI and e1000.
- Better desktop settings persistence UX.
