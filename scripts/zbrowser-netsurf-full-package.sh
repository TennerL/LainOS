#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
queue_root="${2:-build/browser-selfhost-c-queue-module-link}"
package_root="${3:-build/zbrowser-netsurf-full}"

scripts/zbrowser-c-queue-module-link-probe.sh "$stage_root" "$queue_root" >/dev/null

rm -rf "$package_root"
mkdir -p "$package_root"

cp examples/zbrowser_netsurf.Z "$package_root/zbrowser_netsurf.Z"

manifest="$package_root/zbrowser_netsurf.zbuild"
seed_args="$package_root/seed-args.txt"
copy_script="$package_root/autoexec-copy-lines.txt"

{
  printf 'include .\n'
  printf 'module\n'
} >"$manifest"
: >"$seed_args"
: >"$copy_script"

index=0
add_object() {
  local source_object="$1"
  local object_name

  index=$((index + 1))
  object_name="$(printf 'zc%03u.zo' "$index")"
  cp "$source_object" "$package_root/$object_name"
  printf '%s %s\n' "$object_name" "$object_name" >>"$manifest"
  printf '%s/%s=examples/%s\n' "$package_root" "$object_name" "$object_name" >>"$seed_args"
  printf 'cp R:/examples/%s %s\n' "$object_name" "$object_name" >>"$copy_script"
}

plan_path="$stage_root/browser_c/COMPILE_UNITS.txt"
while IFS='|' read -r object_name _rel_source _include_roots _extra_flags; do
  [[ -n "$object_name" ]] || continue
  add_object "$queue_root/${object_name%.o}.zo"
done <"$plan_path"

if [[ -f "$queue_root/AUTO_UNITS.txt" ]]; then
  while IFS='|' read -r object_name _rel_source; do
    [[ -n "$object_name" ]] || continue
    add_object "$queue_root/${object_name%.o}.zo"
  done <"$queue_root/AUTO_UNITS.txt"
fi

{
  printf 'zbrowser_netsurf.Z zbrowser_netsurf.zo\n'
} >>"$manifest"

{
  printf '%s/zbrowser_netsurf.Z=examples/zbrowser_netsurf.Z\n' "$package_root"
  printf '%s/zbrowser_netsurf.zbuild=examples/zbrowser_netsurf.zbuild\n' "$package_root"
} >>"$seed_args"

{
  printf 'cp R:/examples/zbrowser_netsurf.Z zbrowser_netsurf.Z\n'
  printf 'cp R:/examples/zbrowser_netsurf.zbuild zbrowser_netsurf.zbuild\n'
} >>"$copy_script"

ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh "$manifest" "$package_root/host-build" >/dev/null 2>&1
cp "$package_root/host-build/zbrowser_netsurf.zo" "$package_root/zbrowser_netsurf.zo"
printf '%s/zbrowser_netsurf.zo=examples/zbrowser_netsurf.zo\n' "$package_root" >>"$seed_args"
python3 - "$manifest" "$package_root" "$package_root/zbrowser_netsurf.zpkg" <<'PY'
import os
import struct
import sys

manifest_path, package_root, output_path = sys.argv[1:4]
chunk_size = 3500000
objects = []
with open(manifest_path, "r", encoding="utf-8") as manifest:
    for raw_line in manifest:
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";") or line.startswith("//"):
            continue
        parts = line.split()
        if not parts:
            continue
        if parts[0] in {"include", "module", "objects-only", "src", "source", "build", "output", "install", "install-name"}:
            continue
        object_name = parts[1] if len(parts) > 1 else os.path.splitext(parts[0])[0] + ".zo"
        objects.append(object_name)

header_size = 12 + len(objects) * 40
offset = header_size
entries = []
for object_name in objects:
    object_path = os.path.join(package_root, object_name)
    size = os.path.getsize(object_path)
    entries.append((object_name, offset, size, object_path))
    offset += size

with open(output_path, "wb") as out:
    out.write(b"ZPKG1\0\0\0")
    out.write(struct.pack("<I", len(entries)))
    for object_name, object_offset, size, _path in entries:
        encoded = object_name.encode("ascii")
        if len(encoded) > 31:
            raise SystemExit(f"object name too long for zpkg: {object_name}")
        out.write(encoded + b"\0" * (32 - len(encoded)))
        out.write(struct.pack("<II", object_offset, size))
    for _object_name, _object_offset, _size, object_path in entries:
        with open(object_path, "rb") as obj:
            out.write(obj.read())

with open(output_path, "rb") as package:
    index = 0
    while True:
        data = package.read(chunk_size)
        if not data:
            break
        with open(os.path.join(package_root, f"zbrowser_netsurf.zp{index:02d}"), "wb") as chunk:
            chunk.write(data)
        index += 1
PY
for chunk in "$package_root"/zbrowser_netsurf.zp[0-9][0-9]; do
  [[ -f "$chunk" ]] || continue
  printf '%s=examples/%s\n' "$chunk" "$(basename "$chunk")" >>"$seed_args"
done
for chunk in "$package_root"/zbrowser_netsurf.zp[0-9][0-9]; do
  [[ -f "$chunk" ]] || continue
  printf 'cp R:/examples/%s %s\n' "$(basename "$chunk")" "$(basename "$chunk")" >>"$copy_script"
done

printf 'zbrowser-netsurf-full-package: objects=%u package=%s\n' "$index" "$package_root"
