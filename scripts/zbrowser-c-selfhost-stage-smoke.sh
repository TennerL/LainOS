#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="build/browser-selfhost-c-stage"
smoke_img="build/zbrowser-c-selfhost.data.img"
manifest_path="scripts/browser-selfhost-c-files.txt"

make build/tools/lainfs_check_host build/tools/lainfs_seed reseed-data
ZBROWSER_SELFHOST_COMPACT_ASSETS=1 scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null
cp build/data.img "$smoke_img"
build/tools/lainfs_seed "$smoke_img" "$stage_root/browser_c" "$stage_root/browser_c_probe" >/dev/null

check_seeded_path() {
  local fs_path="$1"

  if ! build/tools/lainfs_check_host exists "$smoke_img" "$fs_path"; then
    printf 'zbrowser-c-selfhost-stage-smoke: missing %s\n' "$fs_path" >&2
    build/tools/lainfs_check_host ls "$smoke_img" / >&2 || true
    build/tools/lainfs_check_host ls "$smoke_img" browser_c >&2 || true
    exit 1
  fi
}

check_seeded_path "browser_c/README.txt"
check_seeded_path "browser_c/NEXT_C.txt"
check_seeded_path "browser_c/FILES.txt"
check_seeded_path "browser_c/COMPILE_UNITS.txt"
check_seeded_path "browser_c/COMPILE_TIERS.txt"
for tier_index in 0 1 2 3 4 5; do
  check_seeded_path "browser_c/COMPILE_TIER${tier_index}.txt"
done
check_seeded_path "browser_c_probe/kernel.zbuild"
check_seeded_path "browser_c_probe/queue.zbuild"
check_seeded_path "browser_c_probe/first_unit.zbuild"
check_seeded_path "browser_c_probe/tier1.zbuild"
check_seeded_path "browser_c_probe/tier2.zbuild"
check_seeded_path "browser_c_probe/tier3.zbuild"
check_seeded_path "browser_c_probe/tier4.zbuild"
check_seeded_path "browser_c_probe/tier5.zbuild"
check_seeded_path "browser_c_probe/http_chal.zbuild"
check_seeded_path "browser_c_probe/http_wa.zbuild"
check_seeded_path "browser_c_probe/http_cc.zbuild"
check_seeded_path "browser_c_probe/http_sts.zbuild"
check_seeded_path "browser_c_probe/log.zbuild"
check_seeded_path "browser_c_probe/include/browser_c_probe_api.Z"
check_seeded_path "browser_c_probe/src/browser_c_probe.Z"
check_seeded_path "browser_c_probe/src/browser_c_plan.Z"
check_seeded_path "browser_c_probe/src/browser_c_first_unit_test.Z"
check_seeded_path "browser_c_probe/src/browser_c_tier1_test.Z"
check_seeded_path "browser_c_probe/src/browser_c_tier2_test.Z"
check_seeded_path "browser_c_probe/src/browser_c_tier3_test.Z"
check_seeded_path "browser_c_probe/src/browser_c_tier4_test.Z"
check_seeded_path "browser_c_probe/src/browser_c_tier5_test.Z"
check_seeded_path "browser_c_probe/src/http_chal_test.Z"
check_seeded_path "browser_c_probe/src/http_wa_test.Z"
check_seeded_path "browser_c_probe/src/http_cc_test.Z"
check_seeded_path "browser_c_probe/src/http_sts_test.Z"
check_seeded_path "browser_c_probe/src/log_test.Z"

seeded_count=0
while IFS= read -r rel_path; do
  [[ -n "$rel_path" ]] || continue
  check_seeded_path "browser_c/$rel_path"
  seeded_count=$((seeded_count + 1))
done <"$manifest_path"

printf 'zbrowser-c-selfhost-stage-smoke: ok files=%u probe=1\n' "$seeded_count"
