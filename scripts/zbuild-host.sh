#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

usage() {
  printf 'usage: %s path/to/target.zbuild [output-dir]\n' "${0##*/}" >&2
}

fail_bad_directive() {
  local directive="$1"
  printf 'zbuild-host: bad %s directive in %s\n' "$directive" "$manifest_path" >&2
  exit 1
}

resolve_path() {
  local base_dir="$1"
  local path="$2"

  if [[ "$path" = /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s/%s\n' "$base_dir" "$path"
  fi
}

trim_line() {
  sed 's/\r$//; s/^[[:space:]]*//; s/[[:space:]]*$//'
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

manifest_input="$1"
manifest_dir="$(cd "$(dirname "$manifest_input")" && pwd)"
manifest_name="$(basename "$manifest_input")"
manifest_path="$manifest_dir/$manifest_name"
target_name="${manifest_name%.zbuild}"
output_root="${2:-build/host-zbuild/$target_name}"

if [[ "$manifest_name" == "$target_name" ]]; then
  printf 'zbuild-host: manifest must end with .zbuild: %s\n' "$manifest_input" >&2
  exit 2
fi
if [[ ! -f "$manifest_path" ]]; then
  printf 'zbuild-host: manifest not found: %s\n' "$manifest_path" >&2
  exit 1
fi

mkdir -p "$output_root"
make build/tools/zmod_link_host >/dev/null

source_dir="$manifest_dir"
build_dir="$(cd "$output_root" && pwd)"
linked_output=
effective_output=
link_output=1
source_count=0
declare -a include_args=("--include" "$manifest_dir")
declare -a link_args=()

while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
  line="$(printf '%s\n' "$raw_line" | trim_line)"
  if [[ -z "$line" || "$line" == \#* || "$line" == \;* || "$line" == //* ]]; then
    continue
  fi

  set -- $line
  directive="${1:-}"
  case "$directive" in
    src|source)
      if [[ $# -ne 2 ]]; then
        printf 'zbuild-host: bad %s directive in %s\n' "$directive" "$manifest_path" >&2
        exit 1
      fi
      source_dir="$(resolve_path "$manifest_dir" "$2")"
      ;;
    build)
      if [[ $# -ne 2 ]]; then
        printf 'zbuild-host: bad build directive in %s\n' "$manifest_path" >&2
        exit 1
      fi
      build_dir="$(resolve_path "$manifest_dir" "$2")"
      mkdir -p "$build_dir"
      ;;
    include)
      if [[ $# -ne 2 ]]; then
        printf 'zbuild-host: bad include directive in %s\n' "$manifest_path" >&2
        exit 1
      fi
      include_dir="$(resolve_path "$manifest_dir" "$2")"
      include_args+=("--include" "$include_dir")
      ;;
    output)
      if [[ $# -ne 2 ]]; then
        fail_bad_directive output
      fi
      linked_output="$2"
      ;;
    install|install-name)
      if [[ $# -ne 2 ]]; then
        fail_bad_directive "$directive"
      fi
      ;;
    test-return)
      if [[ $# -ne 2 || ! "$2" =~ ^[0-9]+$ ]]; then
        fail_bad_directive test-return
      fi
      ;;
    module|objects-only)
      if [[ $# -ne 1 ]]; then
        printf 'zbuild-host: bad %s directive in %s\n' "$directive" "$manifest_path" >&2
        exit 1
      fi
      link_output=0
      ;;
    *)
      if [[ $# -gt 2 ]]; then
        printf 'zbuild-host: too many fields in %s: %s\n' "$manifest_path" "$line" >&2
        exit 1
      fi
      source_name="$1"
      object_name="${2:-${source_name%.Z}.zo}"
      source_path="$(resolve_path "$source_dir" "$source_name")"
      object_path="$(resolve_path "$build_dir" "$object_name")"
      mkdir -p "$(dirname "$object_path")"
      link_args+=("$source_path" "$object_path")
      source_count=$((source_count + 1))
      ;;
  esac
done <"$manifest_path"

if [[ $source_count -eq 0 ]]; then
  printf 'zbuild-host: manifest has no sources: %s\n' "$manifest_path" >&2
  exit 1
fi

if [[ $link_output -ne 0 ]]; then
  if [[ -n "$linked_output" ]]; then
    effective_output="$build_dir/$linked_output"
  else
    effective_output="$build_dir/$target_name.bin"
  fi
  mkdir -p "$(dirname "$effective_output")"
  build/tools/zmod_link_host "${include_args[@]}" --output "$effective_output" "${link_args[@]}"
else
  build/tools/zmod_link_host "${include_args[@]}" "${link_args[@]}"
fi

if [[ $link_output -ne 0 ]]; then
  printf '%s -> validated linked build with %d object(s), output=%s\n' "$manifest_name" "$source_count" "$effective_output"
else
  printf '%s -> built module object set with %d object(s)\n' "$manifest_name" "$source_count"
fi
