#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
output_root="${2:-$stage_root/browser_c_probe}"
manifest_path="$stage_root/browser_c_probe/queue.zbuild"
status_path="$output_root/browser_c_plan.status"
first_unit_path="$output_root/browser_c_plan.first"
run_log="$output_root/queue.host-run.log"

scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null

if [[ ! -f "$manifest_path" ]]; then
  printf 'zbrowser-c-queue-probe-smoke: missing staged manifest %s\n' "$manifest_path" >&2
  exit 1
fi

scripts/ztest-host.sh "$manifest_path" "$output_root" >/dev/null

expected_units="$(grep -c '^[^|][^|]*|' "$stage_root/browser_c/COMPILE_UNITS.txt")"

if [[ ! -f "$status_path" ]]; then
  printf 'zbrowser-c-queue-probe-smoke: missing status %s\n' "$status_path" >&2
  exit 1
fi
for probe_artifact in "$first_unit_path"; do
  if [[ ! -f "$probe_artifact" ]]; then
    printf 'zbrowser-c-queue-probe-smoke: missing artifact %s\n' "$probe_artifact" >&2
    exit 1
  fi
done
if ! rg -q '^browser_c plan ok$' "$status_path"; then
  printf 'zbrowser-c-queue-probe-smoke: unexpected status contents\n' >&2
  cat "$status_path" >&2
  exit 1
fi
if ! rg -q '^[^|]+\|[^|]+\.c\|[^|]+' "$first_unit_path"; then
  printf 'zbrowser-c-queue-probe-smoke: unexpected first unit format in %s\n' "$first_unit_path" >&2
  cat "$first_unit_path" >&2
  exit 1
fi
if ! rg -q 'kernel/include/freestanding' "$first_unit_path"; then
  printf 'zbrowser-c-queue-probe-smoke: missing freestanding include root in %s\n' "$first_unit_path" >&2
  cat "$first_unit_path" >&2
  exit 1
fi
if ! rg -q "^browser_c plan: ok units=${expected_units}$" "$run_log"; then
  printf 'zbrowser-c-queue-probe-smoke: missing success line in %s\n' "$run_log" >&2
  cat "$run_log" >&2
  exit 1
fi

printf 'zbrowser-c-queue-probe-smoke: ok units=%s\n' "$expected_units"
