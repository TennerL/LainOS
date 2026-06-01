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

printf 'zbrowser-netsurf-full-package: objects=%u package=%s\n' "$index" "$package_root"
