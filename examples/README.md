`.Z` example programs for the kernel's tiny compiler.

Suggested flow in the kernel shell:

```text
mount C: sd0p1
C:
zc hello.Z hello.bin
exec hello.bin
zrun counter.Z
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

Mouse examples are staged for the PS/2 mouse driver milestone:

```text
ztest mousedemo
zinstall mousedemo
exec mousedemo.bin
```

Those require the kernel to export the `mouse_*` API declared in
`mouse_api.Z`.

If your files are on a different drive or partition, mount and switch to that one first.
