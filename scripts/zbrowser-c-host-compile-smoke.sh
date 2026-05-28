#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
workspace_root="$stage_root/browser_c"
plan_path="$workspace_root/COMPILE_UNITS.txt"
output_root="${2:-build/browser-selfhost-c-host}"

if ! command -v cc >/dev/null 2>&1; then
  printf 'Skipping zbrowser C host compile smoke; missing host tool: cc\n' >&2
  exit 0
fi

scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null

if [[ ! -f "$plan_path" ]]; then
  printf 'zbrowser-c-host-compile-smoke: missing compile plan %s\n' "$plan_path" >&2
  exit 1
fi

rm -rf "$output_root"
mkdir -p "$output_root"

compiled_count=0
while IFS='|' read -r object_name rel_source include_roots; do
  compile_cmd=(cc -c -ffreestanding)
  source_path="$workspace_root/$rel_source"

  [[ -n "$object_name" ]] || continue

  if [[ ! -f "$source_path" ]]; then
    printf 'zbrowser-c-host-compile-smoke: missing staged source %s\n' "$rel_source" >&2
    exit 1
  fi

  IFS=':' read -r -a include_array <<< "$include_roots"
  for include_root in "${include_array[@]}"; do
    [[ -n "$include_root" ]] || continue
    compile_cmd+=(-I"$workspace_root/$include_root")
  done

  compile_cmd+=("$source_path" -o "$output_root/$object_name")
  "${compile_cmd[@]}"

  if [[ ! -s "$output_root/$object_name" ]]; then
    printf 'zbrowser-c-host-compile-smoke: missing compiled object %s\n' "$output_root/$object_name" >&2
    exit 1
  fi

  compiled_count=$((compiled_count + 1))
done <"$plan_path"

printf 'zbrowser-c-host-compile-smoke: ok units=%u\n' "$compiled_count"
