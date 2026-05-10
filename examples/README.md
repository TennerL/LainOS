`.Z` example programs for the kernel's tiny compiler.

Suggested flow in the kernel shell:

```text
mount C: sd0p1
C:
zc hello.Z hello.bin
exec hello.bin
zrun counter.Z
zrun defines.Z
zrun header_controls.Z
zrun hwinfo.Z
zrun function_pointers.Z
zrun typedefs.Z
zrun struct_values.Z
zrun struct_returns.Z
cp hello.Z hello-copy.Z
ztest gfxdemo
zinstall gfxdemo
exec gfxdemo.bin
ztest sysstat
zinstall sysstat
exec sysstat.bin
ztest zreport
zinstall zreport
exec zreport.bin
ztest zmake
zinstall zmake
exec zmake.bin
ztest hwinfo
zinstall hwinfo
exec hwinfo.bin
```

Resident dashboard module:

```text
zbuild hwdash_module
zmod hwdash_module.zo
zreload hwdash_module
zmods
```

Resident task manager module:

```text
zbuild taskmgr_module
zmod taskmgr_module.zo
```

Unlike `hwdash_module`, the task manager only draws while it is opened from the
desktop Modules window, so it behaves more like an app window than a desktop
overlay. The default `autoexec` preloads it so it appears in the desktop module
list immediately after boot.

In the desktop editor, open `hwdash_module.Z` or `hwdash_module.zbuild`, edit it,
then use Save, Build, Inst, Load, or Log. The output pane shows command output
and build logs. Load saves the editor buffer, installs the module object, and
reloads it as a resident module.
The Files window also recognizes `.Z`, `.zbuild`, `.zo`, `.bin`, and log files
and shows matching project actions when one is selected.
Inside the desktop editor, click the text area to move the cursor or drag the
scrollbar to move through the file.

If a loaded module exports `zmodule_tick`, the kernel calls it while the shell is
idle or waiting for keyboard input.

Resident mouse module:

```text
zbuild mouse_module
zmod mouse_module.zo
```

`mouse_module` calls the kernel `mouse_init` export, then keeps a quiet live
cursor updated from `zmodule_tick`. It restores the pixels underneath the cursor
instead of writing status text into the shell. Its `.zbuild` uses `module` so
the build produces only the `.zo` resident object, not an executable `.bin`.

Mouse example:

```text
ztest mousedemo
zinstall mousedemo
exec mousedemo.bin
```

`mousedemo` reads the kernel `mouse_*` exports declared in `mouse_api.Z`
and draws the current pointer position with the graphics API.

If your files are on a different drive or partition, mount and switch to that one first.
