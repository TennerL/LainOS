#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
workspace_root="$stage_root/browser_c"
output_root="${2:-build/browser-selfhost-c-module-link}"
plan_path="$workspace_root/COMPILE_UNITS.txt"

if ! command -v cc >/dev/null 2>&1; then
  printf 'Skipping zbrowser C module link smoke; missing host tool: cc\n' >&2
  exit 0
fi

scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null
make build/tools/zelf_to_zobject build/tools/zmod_link_host >/dev/null

rm -rf "$output_root"
mkdir -p "$output_root"

bridge_line="$(awk -F'|' '$1 == "zbrowser_engine_bridge.o" { print; exit }' "$plan_path")"
if [[ -z "$bridge_line" ]]; then
  printf 'zbrowser-c-module-link-smoke: missing zbrowser_engine_bridge.o compile unit\n' >&2
  exit 1
fi

IFS='|' read -r object_name rel_source include_roots extra_flags <<<"$bridge_line"
source_path="$workspace_root/$rel_source"
elf_object="$output_root/$object_name"
zo_object="$output_root/${object_name%.o}.zo"
manifest_path="$output_root/zbrowser_c_engine_link_smoke.zbuild"

compile_cmd=(cc -c -ffreestanding -nostdinc -fno-pic -fno-PIE -mcmodel=large -mno-red-zone -mstackrealign -mincoming-stack-boundary=3 -fno-asynchronous-unwind-tables -fno-unwind-tables)
IFS=':' read -r -a include_array <<<"$include_roots"
for include_root in "${include_array[@]}"; do
  [[ -n "$include_root" ]] || continue
  compile_cmd+=(-I"$workspace_root/$include_root")
done
if [[ -n "${extra_flags:-}" ]]; then
  IFS=';' read -r -a extra_flag_array <<<"$extra_flags"
  for extra_flag in "${extra_flag_array[@]}"; do
    [[ -n "$extra_flag" ]] || continue
    if [[ "$extra_flag" == "-DZBROWSER_ENGINE_ENABLE_DOM" ]]; then
      continue
    fi
    compile_cmd+=("${extra_flag//\\\"/\"}")
  done
fi
compile_cmd+=("$source_path" -o "$elf_object")
"${compile_cmd[@]}"

build/tools/zelf_to_zobject "$elf_object" "$zo_object"

cat >"$manifest_path" <<EOF
include $repo_root/examples
module
$repo_root/examples/zbrowser_c_engine_link_smoke.Z zceng_smoke.zo
$repo_root/$zo_object zceng_bridge.zo
EOF

ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh "$manifest_path" "$output_root/zbuild" >/dev/null

printf 'zbrowser-c-module-link-smoke: ok %s\n' "$zo_object"
