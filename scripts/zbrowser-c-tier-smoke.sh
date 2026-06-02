#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
workspace_root="$stage_root/browser_c"
all_units_path="$workspace_root/COMPILE_UNITS.txt"
status_path="$workspace_root/COMPILE_TIERS.txt"

scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null

if [[ ! -f "$all_units_path" ]]; then
  printf 'zbrowser-c-tier-smoke: missing compile queue %s\n' "$all_units_path" >&2
  exit 1
fi
if [[ ! -f "$status_path" ]]; then
  printf 'zbrowser-c-tier-smoke: missing tier status %s\n' "$status_path" >&2
  exit 1
fi

tmp_all="$(mktemp)"
tmp_tiers="$(mktemp)"
trap 'rm -f "$tmp_all" "$tmp_tiers"' EXIT

sort "$all_units_path" >"$tmp_all"
: >"$tmp_tiers"

total_count=0
for tier_index in 0 1 2 3 4 5; do
  tier_path="$workspace_root/COMPILE_TIER${tier_index}.txt"

  if [[ ! -f "$tier_path" ]]; then
    printf 'zbrowser-c-tier-smoke: missing tier manifest %s\n' "$tier_path" >&2
    exit 1
  fi

  tier_count="$(grep -c '^[^|][^|]*|' "$tier_path" || true)"
  if [[ "$tier_count" -eq 0 ]]; then
    printf 'zbrowser-c-tier-smoke: empty tier %u\n' "$tier_index" >&2
    exit 1
  fi

  while IFS='|' read -r object_name rel_source _include_roots _extra_flags; do
    [[ -n "$object_name" ]] || continue
    if [[ ! -f "$workspace_root/$rel_source" ]]; then
      printf 'zbrowser-c-tier-smoke: missing tier%u source %s\n' "$tier_index" "$rel_source" >&2
      exit 1
    fi
  done <"$tier_path"

  sort "$tier_path" >>"$tmp_tiers"
  total_count=$((total_count + tier_count))
done

sort -o "$tmp_tiers" "$tmp_tiers"
if ! cmp -s "$tmp_all" "$tmp_tiers"; then
  printf 'zbrowser-c-tier-smoke: tier manifests do not match COMPILE_UNITS.txt\n' >&2
  diff -u "$tmp_all" "$tmp_tiers" >&2 || true
  exit 1
fi

status_count="$(grep -c '^tier[0-5]|' "$status_path" || true)"
if [[ "$status_count" -ne 6 ]]; then
  printf 'zbrowser-c-tier-smoke: expected 6 tier status rows, got %s\n' "$status_count" >&2
  cat "$status_path" >&2
  exit 1
fi
if grep -v '^tier[0-5]|[^|][^|]*|host-built|guest-linked$' "$status_path" >/dev/null; then
  printf 'zbrowser-c-tier-smoke: unexpected tier status row\n' >&2
  cat "$status_path" >&2
  exit 1
fi

printf 'zbrowser-c-tier-smoke: ok tiers=6 units=%u status=host-built/guest-linked\n' "$total_count"
