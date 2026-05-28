#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source scripts/zbuild-host-lib.sh

usage() {
  printf 'usage: %s path/to/target.zbuild [output-dir]\n' "${0##*/}" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

manifest_input="$1"
manifest_name="$(basename "$manifest_input")"
target_name="${manifest_name%.zbuild}"
output_root="${2:-build/host-zbuild/$target_name}"

zbuild_host_prepare_manifest "$manifest_input" "$output_root" "$output_root"
zbuild_host_parse_manifest

make build/tools/zmod_link_host >/dev/null

if [[ $ZBUILD_LINK_OUTPUT -ne 0 ]]; then
  mkdir -p "$(dirname "$ZBUILD_EFFECTIVE_OUTPUT")"
  build/tools/zmod_link_host "${ZBUILD_INCLUDE_ARGS[@]}" --output "$ZBUILD_EFFECTIVE_OUTPUT" "${ZBUILD_LINK_ARGS[@]}"
else
  if [[ "${ZBUILD_HOST_MODULE_LINK:-0}" != 0 ]]; then
    build/tools/zmod_link_host "${ZBUILD_INCLUDE_ARGS[@]}" --module-link "${ZBUILD_LINK_ARGS[@]}"
  else
    build/tools/zmod_link_host "${ZBUILD_INCLUDE_ARGS[@]}" --objects-only "${ZBUILD_LINK_ARGS[@]}"
  fi
fi

if [[ $ZBUILD_LINK_OUTPUT -ne 0 ]]; then
  linked_size="$(wc -c <"$ZBUILD_EFFECTIVE_OUTPUT")"
  cat >"$ZBUILD_BUILD_LOG" <<EOF
target $ZBUILD_TARGET_NAME
objects $ZBUILD_SOURCE_COUNT
output $(basename "$ZBUILD_EFFECTIVE_OUTPUT")
bytes $linked_size
status ok
EOF
  printf '%s -> validated linked build with %d object(s), output=%s\n' "$ZBUILD_MANIFEST_NAME" "$ZBUILD_SOURCE_COUNT" "$ZBUILD_EFFECTIVE_OUTPUT"
else
  cat >"$ZBUILD_BUILD_LOG" <<EOF
target $ZBUILD_TARGET_NAME
objects $ZBUILD_SOURCE_COUNT
status module
EOF
  if [[ "${ZBUILD_HOST_MODULE_LINK:-0}" != 0 ]]; then
    printf '%s -> built and link-validated module object set with %d object(s)\n' "$ZBUILD_MANIFEST_NAME" "$ZBUILD_SOURCE_COUNT"
  else
    printf '%s -> built module object set with %d object(s)\n' "$ZBUILD_MANIFEST_NAME" "$ZBUILD_SOURCE_COUNT"
  fi
fi
