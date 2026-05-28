#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source scripts/zbuild-host-lib.sh

usage() {
  printf 'usage: %s path/to/target.zbuild [build-dir]\n' "${0##*/}" >&2
}

remove_artifact() {
  local path="$1"

  if [[ -e "$path" ]]; then
    rm -f -- "$path"
    removed_count=$((removed_count + 1))
  fi
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

manifest_input="$1"
manifest_name="$(basename "$manifest_input")"
target_name="${manifest_name%.zbuild}"
build_root="${2:-build/host-zbuild/$target_name}"
test_log=
removed_count=0

zbuild_host_prepare_manifest "$manifest_input" "$build_root" "$build_root"
zbuild_host_parse_manifest

for object_path in "${ZBUILD_OBJECT_PATHS[@]}"; do
  remove_artifact "$object_path"
done

if [[ -n "$ZBUILD_EFFECTIVE_OUTPUT" ]]; then
  remove_artifact "$ZBUILD_EFFECTIVE_OUTPUT"
fi

remove_artifact "$ZBUILD_BUILD_LOG"
test_log="$ZBUILD_BUILD_DIR/$ZBUILD_TARGET_NAME.testlog"
remove_artifact "$test_log"

printf '%s -> removed %d artifact(s)\n' "$ZBUILD_MANIFEST_NAME" "$removed_count"
