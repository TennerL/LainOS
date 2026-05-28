#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="build/browser-selfhost-c-stage"
smoke_img="build/zbrowser-c-selfhost.data.img"
manifest_path="scripts/browser-selfhost-c-files.txt"

make build/tools/lainfs_check_host build/tools/lainfs_seed reseed-data
scripts/prepare-browser-selfhost-c-workspace.sh "$stage_root" >/dev/null
cp build/data.img "$smoke_img"
build/tools/lainfs_seed "$smoke_img" "$stage_root/browser_c" >/dev/null

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

seeded_count=0
while IFS= read -r rel_path; do
  [[ -n "$rel_path" ]] || continue
  check_seeded_path "browser_c/$rel_path"
  seeded_count=$((seeded_count + 1))
done <"$manifest_path"

printf 'zbrowser-c-selfhost-stage-smoke: ok files=%u\n' "$seeded_count"
