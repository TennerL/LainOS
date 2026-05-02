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
```

Mouse example:

```text
ztest mousedemo
zinstall mousedemo
exec mousedemo.bin
```

`mousedemo` reads the kernel `mouse_*` exports declared in `mouse_api.Z`
and draws the current pointer position with the graphics API.

If your files are on a different drive or partition, mount and switch to that one first.
