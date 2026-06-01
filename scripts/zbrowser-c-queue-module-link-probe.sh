#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
workspace_root="$stage_root/browser_c"
output_root="${2:-build/browser-selfhost-c-queue-module-link}"
plan_path="$workspace_root/COMPILE_UNITS.txt"
manifest_path="$output_root/zbrowser_c_queue_link_probe.zbuild"
link_log="$output_root/link.log"
undefined_path="$output_root/undefined-symbols.txt"
defined_path="$output_root/defined-symbols.txt"

if ! command -v cc >/dev/null 2>&1; then
  printf 'Skipping zbrowser C queue module link probe; missing host tool: cc\n' >&2
  exit 0
fi
if ! command -v nm >/dev/null 2>&1; then
  printf 'Skipping zbrowser C queue module link probe; missing host tool: nm\n' >&2
  exit 0
fi

if [[ "${ZBROWSER_C_QUEUE_REUSE:-0}" == 0 ]]; then
  scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null
fi
if [[ ! -d "$workspace_root/third_party/netsurf/src/libcss/src" ]]; then
  mkdir -p "$workspace_root/third_party/netsurf/src/libcss"
  cp -R third_party/netsurf/src/libcss/src "$workspace_root/third_party/netsurf/src/libcss/src"
fi
make build/tools/zelf_to_zobject build/tools/zmod_link_host >/dev/null

if [[ "${ZBROWSER_C_QUEUE_REUSE:-0}" == 0 ]]; then
  rm -rf "$output_root"
fi
mkdir -p "$output_root"

compile_one() {
  local object_name="$1"
  local rel_source="$2"
  local include_roots="$3"
  local extra_flags="${4:-}"
  local source_path="$workspace_root/$rel_source"
  local elf_object="$output_root/$object_name"
  local zo_object="$output_root/${object_name%.o}.zo"
  local compile_cmd

  if [[ "${ZBROWSER_C_QUEUE_REUSE:-0}" != 0 && -s "$elf_object" && -s "$zo_object" ]]; then
    return 0
  fi

  if [[ ! -f "$source_path" ]]; then
    printf 'zbrowser-c-queue-module-link-probe: missing source %s\n' "$rel_source" >&2
    exit 1
  fi

  compile_cmd=(cc -c -ffreestanding -nostdinc -fno-stack-protector -fno-pic -fno-PIE -mcmodel=large -mno-red-zone -mstackrealign -mincoming-stack-boundary=3 -fno-asynchronous-unwind-tables -fno-unwind-tables -D_ALIGNED= -DWITHOUT_ICONV_FILTER -include strings.h)

  IFS=':' read -r -a include_array <<<"$include_roots"
  for include_root in "${include_array[@]}"; do
    [[ -n "$include_root" ]] || continue
    compile_cmd+=(-I"$workspace_root/$include_root")
  done

  if [[ -n "$extra_flags" ]]; then
    IFS=';' read -r -a extra_flag_array <<<"$extra_flags"
    for extra_flag in "${extra_flag_array[@]}"; do
      [[ -n "$extra_flag" ]] || continue
      compile_cmd+=("${extra_flag//\\\"/\"}")
    done
  fi

  compile_cmd+=("$source_path" -o "$elf_object")
  "${compile_cmd[@]}"
  build/tools/zelf_to_zobject "$elf_object" "$zo_object" >/dev/null 2>&1
}

compiled_count=0
declare -A seen_objects=()
while IFS='|' read -r object_name rel_source include_roots extra_flags; do
  [[ -n "$object_name" ]] || continue
  compile_one "$object_name" "$rel_source" "$include_roots" "${extra_flags:-}"
  seen_objects["$rel_source"]=1
  compiled_count=$((compiled_count + 1))
done <"$plan_path"

discover_and_compile() {
  local root="$1"
  local include_roots="$2"
  local rel_source
  local object_name

  while IFS= read -r rel_source; do
    [[ -n "$rel_source" ]] || continue
    if [[ -n "${seen_objects[$rel_source]:-}" ]]; then
      continue
    fi
    object_name="auto_$(printf '%s' "$rel_source" | sed 's#[^A-Za-z0-9]#_#g').o"
    compile_one "$object_name" "$rel_source" "$include_roots" ""
    seen_objects["$rel_source"]=1
    printf '%s|%s\n' "$object_name" "$rel_source" >>"$output_root/AUTO_UNITS.txt"
    compiled_count=$((compiled_count + 1))
  done < <(find "$workspace_root/$root" -type f -name '*.c' |
    sed "s#^$workspace_root/##" |
    grep -v '/test/' |
    grep -v '/examples/' |
    grep -v '/perf/' |
    grep -v '/css_property_parser_gen\.c$' |
    sort)
}

: >"$output_root/AUTO_UNITS.txt"
discover_and_compile "third_party/netsurf/src/libdom/src" "third_party/netsurf/src/libdom/include:third_party/netsurf/src/libdom/src:third_party/netsurf/src/libwapcaplet/include:third_party/netsurf/src/libparserutils/include:kernel/include/freestanding"
discover_and_compile "third_party/netsurf/src/libparserutils/src" "third_party/netsurf/src/libparserutils/include:third_party/netsurf/src/libparserutils/src:kernel/include/freestanding"
discover_and_compile "third_party/netsurf/src/libcss/src" "third_party/netsurf/src/libcss/include:third_party/netsurf/src/libcss/src:third_party/netsurf/src/libparserutils/include:third_party/netsurf/src/libwapcaplet/include:kernel/include/freestanding"

{
  printf 'include %s/examples\n' "$repo_root"
  printf 'module\n'
  printf '%s/examples/zbrowser_c_engine_link_smoke.Z zceng_smoke.zo\n' "$repo_root"
  manifest_index=0
  while IFS='|' read -r object_name _rel_source _include_roots _extra_flags; do
    [[ -n "$object_name" ]] || continue
    manifest_index=$((manifest_index + 1))
    printf '%s/%s/%s zc%03u.zo\n' "$repo_root" "$output_root" "${object_name%.o}.zo" "$manifest_index"
  done <"$plan_path"
  while IFS='|' read -r object_name _rel_source; do
    [[ -n "$object_name" ]] || continue
    manifest_index=$((manifest_index + 1))
    printf '%s/%s/%s zc%03u.zo\n' "$repo_root" "$output_root" "${object_name%.o}.zo" "$manifest_index"
  done <"$output_root/AUTO_UNITS.txt"
} >"$manifest_path"

nm -g "$output_root"/*.o 2>/dev/null | awk '/ [TWDBR] / { print $3 }' | sort -u >"$defined_path"
nm -u "$output_root"/*.o 2>/dev/null | awk 'NF == 2 && $1 == "U" { print $2 }' | sort -u |
  comm -23 - "$defined_path" >"$undefined_path"

set +e
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh "$manifest_path" "$output_root/zbuild" >"$link_log" 2>&1
link_status=$?
set -e

if [[ $link_status -eq 0 ]]; then
  printf 'zbrowser-c-queue-module-link-probe: linked units=%u\n' "$compiled_count"
else
  printf 'zbrowser-c-queue-module-link-probe: converted units=%u link_status=%u unresolved=%u report=%s\n' \
    "$compiled_count" "$link_status" "$(wc -l <"$undefined_path")" "$undefined_path"
  printf 'first unresolved symbols:\n'
  head -n 40 "$undefined_path"
fi
