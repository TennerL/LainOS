#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source scripts/zbuild-host-lib.sh

mkdir -p build/zbrowser-smoke
mkdir -p build/zbrowser-smoke/all-manifests
mkdir -p build/zbrowser-smoke/all-installs
mkdir -p build/zbrowser-smoke/all-ztests
make build/tools/zmod_link_host

ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/zbrowser_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/zbrowser_netsurf.zbuild build/zbrowser-smoke/zbrowser_netsurf
scripts/zbuild-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/zbrowser_css_repro
scripts/zbuild-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/zbcss_async
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/image_viewer.zbuild build/zbrowser-smoke/image_viewer
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/clib_port_smoke_module.zbuild build/zbrowser-smoke/clib_port_smoke_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/libc_smoke_module.zbuild build/zbrowser-smoke/libc_smoke_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/filemgr_module.zbuild build/zbrowser-smoke/filemgr_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/taskmgr_module.zbuild build/zbrowser-smoke/taskmgr_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/personalize_module.zbuild build/zbrowser-smoke/personalize_module
scripts/zbuild-host.sh examples/jpg_decode_demo.zbuild build/zbrowser-smoke/jpg_decode_demo
scripts/zbuild-host.sh examples/jpg_decoder.zbuild build/zbrowser-smoke/jpg_decoder
scripts/zbuild-host.sh examples/zlang/install_dir_project/kernel.zbuild build/zbrowser-smoke/install_dir_project
scripts/zbuild-host.sh examples/zlang/selfhost_project/kernel.zbuild build/zbrowser-smoke/selfhost_project
scripts/zbuild-host.sh examples/zlang/manifest_cwd_project/kernel.zbuild build/zbrowser-smoke/manifest_cwd_project
scripts/zbuild-host.sh examples/zlang/default_output_project/kernel.zbuild build/zbrowser-smoke/default_output_project
scripts/zbuild-host.sh examples/zlang/output_name_project/kernel.zbuild build/zbrowser-smoke/output_name_project
scripts/zbuild-host.sh examples/zlang/glob_literal_project/kernel.zbuild build/zbrowser-smoke/glob_literal_project
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/install/zbrowser_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/zbrowser_netsurf.zbuild build/zbrowser-smoke/install/zbrowser_netsurf
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/image_viewer.zbuild build/zbrowser-smoke/install/image_viewer
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/clib_port_smoke_module.zbuild build/zbrowser-smoke/install/clib_port_smoke_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/libc_smoke_module.zbuild build/zbrowser-smoke/install/libc_smoke_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/filemgr_module.zbuild build/zbrowser-smoke/install/filemgr_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/taskmgr_module.zbuild build/zbrowser-smoke/install/taskmgr_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/personalize_module.zbuild build/zbrowser-smoke/install/personalize_module
scripts/zinstall-host.sh examples/jpg_decode_demo.zbuild build/zbrowser-smoke/install/jpg_decode_demo
scripts/zinstall-host.sh examples/zlang/install_dir_project/kernel.zbuild build/zbrowser-smoke/install/install_dir_project
scripts/zbuild-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/zclean-linked
printf 'status ok\n' > build/zbrowser-smoke/zclean-linked/zbcss_async.testlog
scripts/zclean-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/zclean-linked
scripts/zbuild-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/zclean-module
printf 'status ok\n' > build/zbrowser-smoke/zclean-module/zbrowser_module.testlog
scripts/zclean-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/zclean-module
scripts/ztest-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/zclean-ztest
scripts/zclean-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/zclean-ztest

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

invalid_object_count_log="build/zbrowser-smoke/invalid_object_count.log"
if scripts/zbuild-host.sh examples/zlang/invalid_object_count.zbuild build/zbrowser-smoke/invalid_object_count >"$invalid_object_count_log" 2>&1; then
  printf 'zbrowser-compile-smoke: too-many-objects manifest unexpectedly succeeded\n' >&2
  cat "$invalid_object_count_log" >&2
  exit 1
fi
if ! rg -q 'too many objects' "$invalid_object_count_log"; then
  printf 'zbrowser-compile-smoke: too-many-objects manifest failed for an unexpected reason\n' >&2
  cat "$invalid_object_count_log" >&2
  exit 1
fi

invalid_install_name_log="build/zbrowser-smoke/invalid_install_name.log"
if scripts/zbuild-host.sh examples/zlang/invalid_install_name.zbuild build/zbrowser-smoke/invalid_install_name >"$invalid_install_name_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid install-name manifest unexpectedly succeeded\n' >&2
  cat "$invalid_install_name_log" >&2
  exit 1
fi
if ! rg -q 'bad install-name directive' "$invalid_install_name_log"; then
  printf 'zbrowser-compile-smoke: invalid install-name manifest failed for an unexpected reason\n' >&2
  cat "$invalid_install_name_log" >&2
  exit 1
fi

invalid_src_dir_log="build/zbrowser-smoke/invalid_src_dir.log"
if scripts/zbuild-host.sh examples/zlang/invalid_src_dir.zbuild build/zbrowser-smoke/invalid_src_dir >"$invalid_src_dir_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid src-dir manifest unexpectedly succeeded\n' >&2
  cat "$invalid_src_dir_log" >&2
  exit 1
fi
if ! rg -q 'bad src directive' "$invalid_src_dir_log"; then
  printf 'zbrowser-compile-smoke: invalid src-dir manifest failed for an unexpected reason\n' >&2
  cat "$invalid_src_dir_log" >&2
  exit 1
fi

invalid_build_dir_log="build/zbrowser-smoke/invalid_build_dir.log"
if scripts/zbuild-host.sh examples/zlang/invalid_build_dir.zbuild build/zbrowser-smoke/invalid_build_dir >"$invalid_build_dir_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid build-dir manifest unexpectedly succeeded\n' >&2
  cat "$invalid_build_dir_log" >&2
  exit 1
fi
if ! rg -q 'bad build directive' "$invalid_build_dir_log"; then
  printf 'zbrowser-compile-smoke: invalid build-dir manifest failed for an unexpected reason\n' >&2
  cat "$invalid_build_dir_log" >&2
  exit 1
fi

invalid_include_dir_log="build/zbrowser-smoke/invalid_include_dir.log"
if scripts/zbuild-host.sh examples/zlang/invalid_include_dir.zbuild build/zbrowser-smoke/invalid_include_dir >"$invalid_include_dir_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid include-dir manifest unexpectedly succeeded\n' >&2
  cat "$invalid_include_dir_log" >&2
  exit 1
fi
if ! rg -q 'bad include directive' "$invalid_include_dir_log"; then
  printf 'zbrowser-compile-smoke: invalid include-dir manifest failed for an unexpected reason\n' >&2
  cat "$invalid_include_dir_log" >&2
  exit 1
fi

invalid_include_count_log="build/zbrowser-smoke/invalid_include_count.log"
if scripts/zbuild-host.sh examples/zlang/invalid_include_count.zbuild build/zbrowser-smoke/invalid_include_count >"$invalid_include_count_log" 2>&1; then
  printf 'zbrowser-compile-smoke: too-many-include manifest unexpectedly succeeded\n' >&2
  cat "$invalid_include_count_log" >&2
  exit 1
fi
if ! rg -q 'bad include directive' "$invalid_include_count_log"; then
  printf 'zbrowser-compile-smoke: too-many-include manifest failed for an unexpected reason\n' >&2
  cat "$invalid_include_count_log" >&2
  exit 1
fi

mkdir -p build/zbrowser-smoke/install-root
invalid_install_dir_log="build/zbrowser-smoke/invalid_install_dir.log"
if scripts/zinstall-host.sh examples/zlang/invalid_install_dir.zbuild build/zbrowser-smoke/install-root >"$invalid_install_dir_log" 2>&1; then
  printf 'zbrowser-compile-smoke: invalid install-dir manifest unexpectedly succeeded\n' >&2
  cat "$invalid_install_dir_log" >&2
  exit 1
fi
if ! rg -q 'bad install directive' "$invalid_install_dir_log"; then
  printf 'zbrowser-compile-smoke: invalid install-dir manifest failed for an unexpected reason\n' >&2
  cat "$invalid_install_dir_log" >&2
  exit 1
fi

invalid_install_escape_log="build/zbrowser-smoke/invalid_install_escape.log"
if scripts/zbuild-host.sh examples/zlang/invalid_install_escape.zbuild build/zbrowser-smoke/invalid_install_escape >"$invalid_install_escape_log" 2>&1; then
  printf 'zbrowser-compile-smoke: install-escape manifest unexpectedly succeeded\n' >&2
  cat "$invalid_install_escape_log" >&2
  exit 1
fi
if ! rg -q 'bad install directive' "$invalid_install_escape_log"; then
  printf 'zbrowser-compile-smoke: install-escape manifest failed for an unexpected reason\n' >&2
  cat "$invalid_install_escape_log" >&2
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

check_testlog() {
  local path="$1"
  local result_line="$2"
  local expected_line="$3"

  if [[ ! -s "$path" ]]; then
    printf 'zbrowser-compile-smoke: missing test log %s\n' "$path" >&2
    exit 1
  fi
  if ! rg -qx "$result_line" "$path"; then
    printf 'zbrowser-compile-smoke: unexpected test result in %s\n' "$path" >&2
    cat "$path" >&2
    exit 1
  fi
  if [[ -n "$expected_line" ]] && ! rg -qx "$expected_line" "$path"; then
    printf 'zbrowser-compile-smoke: unexpected expected-return in %s\n' "$path" >&2
    cat "$path" >&2
    exit 1
  fi
  if ! rg -qx 'status ok' "$path"; then
    printf 'zbrowser-compile-smoke: test did not finish ok in %s\n' "$path" >&2
    cat "$path" >&2
    exit 1
  fi
}

manifest_is_known_negative() {
  local manifest="$1"

  case "$manifest" in
    examples/zlang/invalid_test_return.zbuild|examples/zlang/invalid_output_name.zbuild|examples/zlang/invalid_object_name.zbuild|examples/zlang/invalid_object_count.zbuild|examples/zlang/invalid_install_name.zbuild|examples/zlang/invalid_src_dir.zbuild|examples/zlang/invalid_build_dir.zbuild|examples/zlang/invalid_include_dir.zbuild|examples/zlang/invalid_include_count.zbuild|examples/zlang/invalid_install_dir.zbuild|examples/zlang/invalid_install_escape.zbuild)
      return 0
      ;;
  esac

  return 1
}

manifest_skips_module_validation() {
  local manifest="$1"

  case "$manifest" in
    examples/jpg_decoder.zbuild)
      return 0
      ;;
  esac

  return 1
}

manifest_needs_module_validation() {
  local manifest="$1"
  local build_root="$2"
  local install_root="$3"

  if manifest_skips_module_validation "$manifest"; then
    return 1
  fi

  zbuild_host_prepare_manifest "$manifest" "$build_root" "$install_root"
  zbuild_host_parse_manifest
  [[ $ZBUILD_OBJECTS_ONLY -ne 0 ]]
}

check_manifest_validation_line() {
  local manifest="$1"
  local build_root="$2"
  local install_root="$3"

  zbuild_host_prepare_manifest "$manifest" "$build_root" "$install_root"
  zbuild_host_parse_manifest
  check_buildlog_line "$ZBUILD_BUILD_LOG" "validation module-link"
}

ztest_manifest_list() {
  local manifest=
  local discovery_root="build/zbrowser-smoke/discovery"

  mkdir -p "$discovery_root"

  while IFS= read -r manifest; do
    [[ -n "$manifest" ]] || continue
    if manifest_is_known_negative "$manifest"; then
      continue
    fi

    zbuild_host_prepare_manifest "$manifest" "$discovery_root" "$discovery_root"
    zbuild_host_parse_manifest
    if [[ $ZBUILD_HAS_EXPECTED_RETURN -ne 0 ]]; then
      printf '%s\n' "$manifest"
    fi
  done < <(find examples -name '*.zbuild' -type f | sort)
}

run_manifest_ztest_probe() {
  local manifest="$1"
  local probe_rel=
  local probe_dir=

  probe_rel="${manifest#examples/}"
  probe_dir="build/zbrowser-smoke/all-ztests/${probe_rel%.zbuild}"
  mkdir -p "$probe_dir"
  if ! scripts/ztest-host.sh "$manifest" "$probe_dir" >"$probe_dir/probe.log" 2>&1; then
    printf 'zbrowser-compile-smoke: broad ztest probe failed for %s\n' "$manifest" >&2
    cat "$probe_dir/probe.log" >&2
    exit 1
  fi
}

check_manifest_testlog() {
  local manifest="$1"
  local probe_rel=
  local probe_dir=
  local test_log=

  probe_rel="${manifest#examples/}"
  probe_dir="build/zbrowser-smoke/all-ztests/${probe_rel%.zbuild}"

  zbuild_host_prepare_manifest "$manifest" "$probe_dir" "$probe_dir"
  zbuild_host_parse_manifest

  if [[ $ZBUILD_HAS_EXPECTED_RETURN -eq 0 ]]; then
    printf 'zbrowser-compile-smoke: test manifest missing expected return %s\n' "$manifest" >&2
    exit 1
  fi

  test_log="$ZBUILD_BUILD_DIR/$ZBUILD_TARGET_NAME.testlog"
  check_testlog "$test_log" "result $ZBUILD_EXPECTED_RETURN" "expected $ZBUILD_EXPECTED_RETURN"
}

check_installed_manifest() {
  local manifest="$1"
  local install_root="$2"
  local i=

  zbuild_host_prepare_manifest "$manifest" "$install_root/build" "$install_root"
  zbuild_host_parse_manifest

  if [[ $ZBUILD_OBJECTS_ONLY -ne 0 && $ZBUILD_SOURCE_COUNT -ne 1 ]]; then
    for i in "${!ZBUILD_OBJECT_NAMES[@]}"; do
      check_output "$ZBUILD_INSTALL_DIR/${ZBUILD_OBJECT_NAMES[$i]}"
    done
    return
  fi

  check_output "$ZBUILD_INSTALL_DIR/$ZBUILD_INSTALL_NAME"
}

check_output "build/zbrowser-smoke/zbrowser_css_repro/zbrowser_css_repro.bin"
check_output "build/zbrowser-smoke/zbcss_async/zbcss_async.bin"
check_output "build/zbrowser-smoke/jpg_decode_demo/jpg_decode_demo.bin"
check_output "build/zbrowser-smoke/install/zbrowser_module/zbrowser_html.zo"
check_output "build/zbrowser-smoke/install/zbrowser_module/zbrowser_module.zo"
check_output "build/zbrowser-smoke/install/zbrowser_netsurf/zbrowser_netsurf.zo"
check_output "build/zbrowser-smoke/install/image_viewer/image_viewer.zo"
check_output "build/zbrowser-smoke/install/clib_port_smoke_module/clib_port_smoke_module.zo"
check_output "build/zbrowser-smoke/install/clib_port_smoke_module/mini_zlib.zo"
check_output "build/zbrowser-smoke/install/libc_smoke_module/libc_smoke_module.zo"
check_output "build/zbrowser-smoke/install/filemgr_module/filemgr_module.zo"
check_output "build/zbrowser-smoke/install/taskmgr_module/taskmgr_module.zo"
check_output "build/zbrowser-smoke/install/personalize_module/personalize_module.zo"
check_output "build/zbrowser-smoke/install/jpg_decode_demo/jpg_decode_demo.bin"
check_output "build/zbrowser-smoke/install/install_dir_project/apps/browser/install_dir_project.bin"
check_output "build/zbrowser-smoke/install/install_dir_project/.host-build/install_dir.bin"
check_output "build/zbrowser-smoke/install_dir_project/install_dir.bin"
check_output "examples/zlang/selfhost_project/build/kernel.bin"
check_output "examples/zlang/manifest_cwd_project/build/manifest_cwd.bin"
check_output "examples/zlang/default_output_project/build/kernel.bin"
check_output "examples/zlang/output_name_project/build/custom_named_output.bin"
check_output "examples/zlang/glob_literal_project/build/glob_literal.bin"
check_output "build/zbrowser-smoke/jpg_decoder/jpg_decoder.zo"
check_buildlog "build/zbrowser-smoke/zbrowser_module/zbrowser_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/zbrowser_netsurf/zbrowser_netsurf.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/zbrowser_css_repro/zbrowser_css_repro.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/zbcss_async/zbcss_async.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/image_viewer/image_viewer.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/clib_port_smoke_module/clib_port_smoke_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/libc_smoke_module/libc_smoke_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/filemgr_module/filemgr_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/taskmgr_module/taskmgr_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/personalize_module/personalize_module.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/jpg_decode_demo/jpg_decode_demo.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/jpg_decoder/jpg_decoder.buildlog" "status module"
check_buildlog "build/zbrowser-smoke/install_dir_project/kernel.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/install/install_dir_project/.host-build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/selfhost_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/manifest_cwd_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/default_output_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/output_name_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/glob_literal_project/build/kernel.buildlog" "status ok"
check_buildlog_line "build/zbrowser-smoke/zbrowser_module/zbrowser_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/zbrowser_netsurf/zbrowser_netsurf.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/image_viewer/image_viewer.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/clib_port_smoke_module/clib_port_smoke_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/libc_smoke_module/libc_smoke_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/filemgr_module/filemgr_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/taskmgr_module/taskmgr_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/personalize_module/personalize_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/zbrowser_module/.host-build/zbrowser_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/zbrowser_netsurf/.host-build/zbrowser_netsurf.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/image_viewer/.host-build/image_viewer.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/clib_port_smoke_module/.host-build/clib_port_smoke_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/libc_smoke_module/.host-build/libc_smoke_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/filemgr_module/.host-build/filemgr_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/taskmgr_module/.host-build/taskmgr_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/personalize_module/.host-build/personalize_module.buildlog" "validation module-link"
check_buildlog_line "examples/zlang/output_name_project/build/kernel.buildlog" "output custom_named_output.bin"
check_buildlog_line "examples/zlang/glob_literal_project/build/kernel.buildlog" "output glob_literal.bin"
check_buildlog_line "build/zbrowser-smoke/install_dir_project/kernel.buildlog" "output install_dir.bin"
if [[ -e build/zbrowser-smoke/zclean-linked/zbcss_async.bin ||
      -e build/zbrowser-smoke/zclean-linked/zbcss_async.zo ||
      -e build/zbrowser-smoke/zclean-linked/zbcss_async.buildlog ||
      -e build/zbrowser-smoke/zclean-linked/zbcss_async.testlog ]]; then
  printf 'zbrowser-compile-smoke: zclean-host left linked-build artifacts behind\n' >&2
  exit 1
fi
if [[ -e build/zbrowser-smoke/zclean-module/zbrowser_html.zo ||
      -e build/zbrowser-smoke/zclean-module/zbrowser_module.zo ||
      -e build/zbrowser-smoke/zclean-module/zbrowser_module.buildlog ||
      -e build/zbrowser-smoke/zclean-module/zbrowser_module.testlog ]]; then
  printf 'zbrowser-compile-smoke: zclean-host left module-build artifacts behind\n' >&2
  exit 1
fi
if [[ -e build/zbrowser-smoke/zclean-ztest/zbrowser_css_repro.bin ||
      -e build/zbrowser-smoke/zclean-ztest/zbrowser_css_repro.zo ||
      -e build/zbrowser-smoke/zclean-ztest/zbrowser_css_repro.buildlog ||
      -e build/zbrowser-smoke/zclean-ztest/zbrowser_css_repro.testlog ||
      -e build/zbrowser-smoke/zclean-ztest/zbrowser_css_repro.host-run.log ]]; then
  printf 'zbrowser-compile-smoke: zclean-host left ztest artifacts behind\n' >&2
  exit 1
fi

while IFS= read -r manifest; do
  [[ -n "$manifest" ]] || continue
  run_manifest_ztest_probe "$manifest"
done < <(ztest_manifest_list)

while IFS= read -r manifest; do
  if manifest_is_known_negative "$manifest"; then
    continue
  fi

  probe_rel="${manifest#examples/}"
  probe_dir="build/zbrowser-smoke/all-manifests/${probe_rel%.zbuild}"
  mkdir -p "$probe_dir"
  if manifest_needs_module_validation "$manifest" "$probe_dir" "$probe_dir"; then
    if ! ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh "$manifest" "$probe_dir" >"$probe_dir/probe.log" 2>&1; then
      printf 'zbrowser-compile-smoke: broad manifest probe failed for %s\n' "$manifest" >&2
      cat "$probe_dir/probe.log" >&2
      exit 1
    fi
    check_manifest_validation_line "$manifest" "$probe_dir" "$probe_dir"
  else
    if ! scripts/zbuild-host.sh "$manifest" "$probe_dir" >"$probe_dir/probe.log" 2>&1; then
      printf 'zbrowser-compile-smoke: broad manifest probe failed for %s\n' "$manifest" >&2
      cat "$probe_dir/probe.log" >&2
      exit 1
    fi
  fi
done < <(find examples -name '*.zbuild' -type f | sort)

while IFS= read -r manifest; do
  if manifest_is_known_negative "$manifest"; then
    continue
  fi

  install_rel="${manifest#examples/}"
  install_root="build/zbrowser-smoke/all-installs/${install_rel%.zbuild}"
  mkdir -p "$install_root"
  if manifest_needs_module_validation "$manifest" "$install_root/.host-build" "$install_root"; then
    if ! ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh "$manifest" "$install_root" >"$install_root/install.log" 2>&1; then
      printf 'zbrowser-compile-smoke: broad install probe failed for %s\n' "$manifest" >&2
      cat "$install_root/install.log" >&2
      exit 1
    fi
    check_manifest_validation_line "$manifest" "$install_root/.host-build" "$install_root"
  else
    if ! scripts/zinstall-host.sh "$manifest" "$install_root" >"$install_root/install.log" 2>&1; then
      printf 'zbrowser-compile-smoke: broad install probe failed for %s\n' "$manifest" >&2
      cat "$install_root/install.log" >&2
      exit 1
    fi
  fi
  check_installed_manifest "$manifest" "$install_root"
done < <(find examples -name '*.zbuild' -type f | sort)

while IFS= read -r manifest; do
  [[ -n "$manifest" ]] || continue
  check_manifest_testlog "$manifest"
done < <(ztest_manifest_list)

check_output "build/zbrowser-smoke/all-installs/zlang/install_dir_project/kernel/.host-build/install_dir.bin"
check_buildlog "build/zbrowser-smoke/all-installs/zlang/install_dir_project/kernel/.host-build/kernel.buildlog" "status ok"
check_output "build/zbrowser-smoke/all-ztests/zlang/install_dir_project/kernel/install_dir.bin"
check_buildlog "build/zbrowser-smoke/all-ztests/zlang/install_dir_project/kernel/kernel.buildlog" "status ok"
check_output "build/zbrowser-smoke/all-ztests/zlang/zmake/.host-build/sysstat.bin"
check_output "build/zbrowser-smoke/all-ztests/zlang/zmake/.host-build/zreport.bin"
check_output "build/zbrowser-smoke/all-ztests/zlang/zmake/last.testlog"
check_testlog "build/zbrowser-smoke/all-ztests/zlang/zmake/sysstat.testlog" "result 7" "expected 7"
check_testlog "build/zbrowser-smoke/all-ztests/zlang/zmake/zreport.testlog" "result 13" "expected 13"

if ! cmp -s \
    "build/zbrowser-smoke/all-ztests/zlang/zmake/last.testlog" \
    "build/zbrowser-smoke/all-ztests/zlang/zmake/zreport.testlog"; then
  printf 'zbrowser-compile-smoke: zmake did not copy zreport.testlog to last.testlog\n' >&2
  exit 1
fi
