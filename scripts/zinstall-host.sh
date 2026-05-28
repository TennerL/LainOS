#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source scripts/zbuild-host-lib.sh

usage() {
  printf 'usage: %s path/to/target.zbuild [install-root]\n' "${0##*/}" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

manifest_input="$1"
manifest_name="$(basename "$manifest_input")"
target_name="${manifest_name%.zbuild}"
if [[ $# -eq 2 ]]; then
  install_root="$2"
  build_root="$install_root/.host-build"
else
  build_root="build/host-zinstall/$target_name/build"
  install_root="build/host-zinstall/$target_name/install"
fi

ZBUILD_HOST_FORCE_BUILD_ROOT=1
zbuild_host_prepare_manifest "$manifest_input" "$build_root" "$install_root"
zbuild_host_parse_manifest

ZBUILD_HOST_FORCE_BUILD_ROOT=1 scripts/zbuild-host.sh "$ZBUILD_MANIFEST_PATH" "$build_root" >/dev/null

mkdir -p "$ZBUILD_INSTALL_DIR"

if [[ $ZBUILD_OBJECTS_ONLY -ne 0 && $ZBUILD_SOURCE_COUNT -ne 1 ]]; then
  installed_count=0
  for i in "${!ZBUILD_OBJECT_PATHS[@]}"; do
    object_path="${ZBUILD_OBJECT_PATHS[$i]}"
    object_name="${ZBUILD_OBJECT_NAMES[$i]}"
    install_path="$ZBUILD_INSTALL_DIR/$object_name"

    if [[ ! -s "$object_path" ]]; then
      printf 'zinstall-host: build did not produce %s\n' "$object_name" >&2
      exit 1
    fi
    mkdir -p "$(dirname "$install_path")"
    cp "$object_path" "$install_path"
    installed_count=$((installed_count + 1))
  done

  printf '%s -> installed %d module object(s) into %s\n' "$ZBUILD_MANIFEST_NAME" "$installed_count" "$ZBUILD_INSTALL_DIR"
  exit 0
fi

if [[ -z "$ZBUILD_EFFECTIVE_OUTPUT" || ! -s "$ZBUILD_EFFECTIVE_OUTPUT" ]]; then
  printf 'zinstall-host: build did not produce installable output for %s\n' "$ZBUILD_MANIFEST_PATH" >&2
  exit 1
fi

install_path="$ZBUILD_INSTALL_DIR/$ZBUILD_INSTALL_NAME"
mkdir -p "$(dirname "$install_path")"
cp "$ZBUILD_EFFECTIVE_OUTPUT" "$install_path"

printf '%s -> installed %s as %s\n' "$ZBUILD_MANIFEST_NAME" "$(basename "$ZBUILD_EFFECTIVE_OUTPUT")" "$install_path"
