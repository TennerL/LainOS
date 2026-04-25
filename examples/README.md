`.Z` example programs for the kernel's tiny compiler.

Suggested flow in the kernel shell:

```text
mount C: sd0p1
C:
zc hello.Z hello.bin
exec hello.bin
zrun counter.Z
```

If your files are on a different drive or partition, mount and switch to that one first.
