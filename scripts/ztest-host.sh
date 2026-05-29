#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source scripts/zbuild-host-lib.sh

usage() {
  printf 'usage: %s path/to/target.zbuild [build-dir]\n' "${0##*/}" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

manifest_input="$1"
manifest_name="$(basename "$manifest_input")"
target_name="${manifest_name%.zbuild}"
build_root="${2:-build/host-ztest/$target_name}"
run_log=
result_line=
result=
status_word=
test_log=

ZBUILD_HOST_FORCE_BUILD_ROOT=1
zbuild_host_prepare_manifest "$manifest_input" "$build_root" "$build_root"
zbuild_host_parse_manifest

if [[ $ZBUILD_OBJECTS_ONLY -ne 0 ]]; then
  printf 'ztest-host: module targets do not produce executables: %s\n' "$ZBUILD_MANIFEST_PATH" >&2
  exit 1
fi

make build/tools/zmod_link_host >/dev/null

ZBUILD_HOST_FORCE_BUILD_ROOT=1 scripts/zclean-host.sh "$manifest_input" "$build_root" >/dev/null
ZBUILD_HOST_FORCE_BUILD_ROOT=1 scripts/zbuild-host.sh "$manifest_input" "$build_root" >/dev/null

run_log="$ZBUILD_BUILD_DIR/$ZBUILD_TARGET_NAME.host-run.log"
test_log="$ZBUILD_BUILD_DIR/$ZBUILD_TARGET_NAME.testlog"

run_args=("$repo_root/build/tools/zmod_link_host")
run_args+=("${ZBUILD_INCLUDE_ARGS[@]}")
run_args+=(--run)
if [[ $ZBUILD_HAS_EXPECTED_RETURN -ne 0 ]]; then
  run_args+=(--expect-return "$ZBUILD_EXPECTED_RETURN")
fi
run_args+=("${ZBUILD_LINK_ARGS[@]}")

ZMOD_HOST_REPO_ROOT="$repo_root" \
ZMOD_HOST_RUN_ROOT="$ZBUILD_BUILD_DIR" \
ZMOD_HOST_MANIFEST_DIR="$ZBUILD_MANIFEST_DIR" \
  "${run_args[@]}" >"$run_log" 2>&1

result_line="$(tail -n 1 "$run_log")"
result="$(printf '%s\n' "$result_line" | sed -n 's/^run result=\([0-9][0-9]*\).*/\1/p')"
status_word="$(printf '%s\n' "$result_line" | sed -n 's/^run result=[0-9][0-9]*\( expected=[0-9][0-9]*\)\{0,1\} status=\([a-z][a-z]*\)$/\2/p')"

if [[ -z "$result" || "$status_word" != "ok" ]]; then
  printf 'ztest-host: unexpected run result for %s\n' "$ZBUILD_MANIFEST_PATH" >&2
  cat "$run_log" >&2
  exit 1
fi

{
  printf 'target %s\n' "$ZBUILD_TARGET_NAME"
  printf 'output %s\n' "$(basename "$ZBUILD_EFFECTIVE_OUTPUT")"
  printf 'result %s\n' "$result"
  if [[ $ZBUILD_HAS_EXPECTED_RETURN -ne 0 ]]; then
    printf 'expected %s\n' "$ZBUILD_EXPECTED_RETURN"
  fi
  printf 'status ok\n'
} >"$test_log"

printf '%s -> validated host test run result=%s\n' "$ZBUILD_MANIFEST_NAME" "$result"
