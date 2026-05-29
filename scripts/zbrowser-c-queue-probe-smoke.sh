#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
output_root="${2:-build/browser-selfhost-c-probe-host}"
manifest_path="$stage_root/browser_c_probe/kernel.zbuild"
status_path="$output_root/browser_c_probe.status"
run_log="$output_root/kernel.host-run.log"

scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null

if [[ ! -f "$manifest_path" ]]; then
  printf 'zbrowser-c-queue-probe-smoke: missing staged manifest %s\n' "$manifest_path" >&2
  exit 1
fi

scripts/ztest-host.sh "$manifest_path" "$output_root" >/dev/null

if [[ ! -f "$status_path" ]]; then
  printf 'zbrowser-c-queue-probe-smoke: missing status %s\n' "$status_path" >&2
  exit 1
fi
if ! rg -q '^browser_c probe ok$' "$status_path"; then
  printf 'zbrowser-c-queue-probe-smoke: unexpected status contents\n' >&2
  cat "$status_path" >&2
  exit 1
fi
if ! rg -q '^browser_c probe: ok units=17$' "$run_log"; then
  printf 'zbrowser-c-queue-probe-smoke: missing success line in %s\n' "$run_log" >&2
  cat "$run_log" >&2
  exit 1
fi

printf 'zbrowser-c-queue-probe-smoke: ok units=17\n'
