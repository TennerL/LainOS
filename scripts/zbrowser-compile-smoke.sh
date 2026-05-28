#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mkdir -p build/zbrowser-smoke
mkdir -p build/zbrowser-smoke/all-manifests
make build/tools/zmod_link_host

scripts/zbuild-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/zbrowser_module
scripts/zbuild-host.sh examples/zbrowser_netsurf.zbuild build/zbrowser-smoke/zbrowser_netsurf
scripts/zbuild-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/zbrowser_css_repro
scripts/zbuild-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/zbcss_async
scripts/zbuild-host.sh examples/image_viewer.zbuild build/zbrowser-smoke/image_viewer
scripts/zbuild-host.sh examples/jpg_decode_demo.zbuild build/zbrowser-smoke/jpg_decode_demo
scripts/zbuild-host.sh examples/jpg_decoder.zbuild build/zbrowser-smoke/jpg_decoder
scripts/zbuild-host.sh examples/zlang/selfhost_project/kernel.zbuild build/zbrowser-smoke/selfhost_project
scripts/zbuild-host.sh examples/zlang/manifest_cwd_project/kernel.zbuild build/zbrowser-smoke/manifest_cwd_project
scripts/zbuild-host.sh examples/zlang/default_output_project/kernel.zbuild build/zbrowser-smoke/default_output_project
scripts/zbuild-host.sh examples/zlang/output_name_project/kernel.zbuild build/zbrowser-smoke/output_name_project

invalid_log="build/zbrowser-smoke/invalid_test_return.log"
if scripts/zbuild-host.sh examples/zlang/invalid_test_return.zbuild build/zbrowser-smoke/invalid_test_return >"$invalid_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid test-return manifest unexpectedly succeeded\n' >&2
  cat "$invalid_log" >&2
  exit 1
fi
if ! rg -q 'bad test-return directive' "$invalid_log"; then
  printf 'zbrowser-compile-smoke: invalid test-return manifest failed for an unexpected reason\n' >&2
  cat "$invalid_log" >&2
  exit 1
fi

invalid_output_log="build/zbrowser-smoke/invalid_output_name.log"
if scripts/zbuild-host.sh examples/zlang/invalid_output_name.zbuild build/zbrowser-smoke/invalid_output_name >"$invalid_output_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid output manifest unexpectedly succeeded\n' >&2
  cat "$invalid_output_log" >&2
  exit 1
fi
if ! rg -q 'bad output directive' "$invalid_output_log"; then
  printf 'zbrowser-compile-smoke: invalid output manifest failed for an unexpected reason\n' >&2
  cat "$invalid_output_log" >&2
  exit 1
fi

invalid_object_log="build/zbrowser-smoke/invalid_object_name.log"
if scripts/zbuild-host.sh examples/zlang/invalid_object_name.zbuild build/zbrowser-smoke/invalid_object_name >"$invalid_object_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid object manifest unexpectedly succeeded\n' >&2
  cat "$invalid_object_log" >&2
  exit 1
fi
if ! rg -q 'bad object name' "$invalid_object_log"; then
  printf 'zbrowser-compile-smoke: invalid object manifest failed for an unexpected reason\n' >&2
  cat "$invalid_object_log" >&2
  exit 1
fi

check_output() {
  local path="$1"

  if [[ ! -s "$path" ]]; then
    printf 'zbrowser-compile-smoke: missing linked output %s\n' "$path" >&2
    exit 1
  fi
}

check_buildlog() {
  local path="$1"
  local status_line="$2"

  if [[ ! -s "$path" ]]; then
    printf 'zbrowser-compile-smoke: missing build log %s\n' "$path" >&2
    exit 1
  fi
  if ! rg -qx "$status_line" "$path"; then
    printf 'zbrowser-compile-smoke: unexpected build log status in %s\n' "$path" >&2
    cat "$path" >&2
    exit 1
  fi
}

check_buildlog_line() {
  local path="$1"
  local expected="$2"

  if ! rg -qx "$expected" "$path"; then
    printf 'zbrowser-compile-smoke: missing build log line %s in %s\n' "$expected" "$path" >&2
    cat "$path" >&2
    exit 1
  fi
}

check_output "build/zbrowser-smoke/zbrowser_css_repro/zbrowser_css_repro.bin"
check_output "build/zbrowser-smoke/zbcss_async/zbcss_async.bin"
check_output "build/zbrowser-smoke/jpg_decode_demo/jpg_decode_demo.bin"
check_output "examples/zlang/selfhost_project/build/kernel.bin"
check_output "examples/zlang/manifest_cwd_project/build/manifest_cwd.bin"
check_output "examples/zlang/default_output_project/build/kernel.bin"
check_output "examples/zlang/output_name_project/build/custom_named_output.bin"
check_output "build/zbrowser-smoke/jpg_decoder/jpg_decoder.zo"
check_buildlog "build/zbrowser-smoke/zbrowser_module/zbrowser_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/zbrowser_netsurf/zbrowser_netsurf.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/zbrowser_css_repro/zbrowser_css_repro.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/zbcss_async/zbcss_async.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/image_viewer/image_viewer.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/jpg_decode_demo/jpg_decode_demo.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/jpg_decoder/jpg_decoder.buildlog" "status module"
check_buildlog "examples/zlang/selfhost_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/manifest_cwd_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/default_output_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/output_name_project/build/kernel.buildlog" "status ok"
check_buildlog_line "examples/zlang/output_name_project/build/kernel.buildlog" "output custom_named_output.bin"

while IFS= read -r manifest; do
  case "$manifest" in
    examples/zlang/invalid_test_return.zbuild|examples/zlang/invalid_output_name.zbuild|examples/zlang/invalid_object_name.zbuild)
      continue
      ;;
  esac

  probe_rel="${manifest#examples/}"
  probe_dir="build/zbrowser-smoke/all-manifests/${probe_rel%.zbuild}"
  mkdir -p "$probe_dir"
  if ! scripts/zbuild-host.sh "$manifest" "$probe_dir" >"$probe_dir/probe.log" 2>&1; then
    printf 'zbrowser-compile-smoke: broad manifest probe failed for %s\n' "$manifest" >&2
    cat "$probe_dir/probe.log" >&2
    exit 1
  fi
done < <(find examples -name '*.zbuild' -type f | sort)
