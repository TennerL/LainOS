#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source scripts/zbuild-host-lib.sh

mkdir -p build/zbrowser-smoke
mkdir -p build/zbrowser-smoke/all-manifests
mkdir -p build/zbrowser-smoke/all-installs
make build/tools/zmod_link_host

ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/zbrowser_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh examples/zbrowser_netsurf.zbuild build/zbrowser-smoke/zbrowser_netsurf
scripts/zbuild-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/zbrowser_css_repro
scripts/zbuild-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/zbcss_async
scripts/zbuild-host.sh examples/image_viewer.zbuild build/zbrowser-smoke/image_viewer
scripts/zbuild-host.sh examples/jpg_decode_demo.zbuild build/zbrowser-smoke/jpg_decode_demo
scripts/zbuild-host.sh examples/jpg_decoder.zbuild build/zbrowser-smoke/jpg_decoder
scripts/zbuild-host.sh examples/zlang/selfhost_project/kernel.zbuild build/zbrowser-smoke/selfhost_project
scripts/zbuild-host.sh examples/zlang/manifest_cwd_project/kernel.zbuild build/zbrowser-smoke/manifest_cwd_project
scripts/zbuild-host.sh examples/zlang/default_output_project/kernel.zbuild build/zbrowser-smoke/default_output_project
scripts/zbuild-host.sh examples/zlang/output_name_project/kernel.zbuild build/zbrowser-smoke/output_name_project
scripts/zbuild-host.sh examples/zlang/glob_literal_project/kernel.zbuild build/zbrowser-smoke/glob_literal_project
scripts/ztest-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/ztest-zbrowser-css-repro
scripts/ztest-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/ztest-zbcss-async
scripts/ztest-host.sh examples/zlang/sysstat.zbuild build/zbrowser-smoke/ztest-sysstat
scripts/ztest-host.sh examples/zlang/hwinfo.zbuild build/zbrowser-smoke/ztest-hwinfo
scripts/ztest-host.sh examples/zlang/gfxdemo.zbuild build/zbrowser-smoke/ztest-gfxdemo
scripts/ztest-host.sh examples/zlang/mousedemo.zbuild build/zbrowser-smoke/ztest-mousedemo
scripts/ztest-host.sh examples/zlang/selfhost.zbuild build/zbrowser-smoke/ztest-selfhost
scripts/ztest-host.sh examples/zlang/zmake.zbuild build/zbrowser-smoke/ztest-zmake
scripts/ztest-host.sh examples/zlang/zreport.zbuild build/zbrowser-smoke/ztest-zreport
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/install/zbrowser_module
ZBUILD_HOST_MODULE_LINK=1 scripts/zinstall-host.sh examples/zbrowser_netsurf.zbuild build/zbrowser-smoke/install/zbrowser_netsurf
scripts/zinstall-host.sh examples/jpg_decode_demo.zbuild build/zbrowser-smoke/install/jpg_decode_demo
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
    examples/zlang/invalid_test_return.zbuild|examples/zlang/invalid_output_name.zbuild|examples/zlang/invalid_object_name.zbuild|examples/zlang/invalid_object_count.zbuild|examples/zlang/invalid_install_name.zbuild|examples/zlang/invalid_src_dir.zbuild|examples/zlang/invalid_build_dir.zbuild|examples/zlang/invalid_include_dir.zbuild|examples/zlang/invalid_include_count.zbuild|examples/zlang/invalid_install_dir.zbuild)
      return 0
      ;;
  esac

  return 1
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
check_output "build/zbrowser-smoke/install/jpg_decode_demo/jpg_decode_demo.bin"
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
check_buildlog "build/zbrowser-smoke/jpg_decode_demo/jpg_decode_demo.buildlog" "status ok"
check_buildlog "build/zbrowser-smoke/jpg_decoder/jpg_decoder.buildlog" "status module"
check_buildlog "examples/zlang/selfhost_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/manifest_cwd_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/default_output_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/output_name_project/build/kernel.buildlog" "status ok"
check_buildlog "examples/zlang/glob_literal_project/build/kernel.buildlog" "status ok"
check_buildlog_line "build/zbrowser-smoke/zbrowser_module/zbrowser_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/zbrowser_netsurf/zbrowser_netsurf.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/zbrowser_module/.host-build/zbrowser_module.buildlog" "validation module-link"
check_buildlog_line "build/zbrowser-smoke/install/zbrowser_netsurf/.host-build/zbrowser_netsurf.buildlog" "validation module-link"
check_testlog "build/zbrowser-smoke/ztest-zbrowser-css-repro/zbrowser_css_repro.testlog" "result 0" "expected 0"
check_testlog "build/zbrowser-smoke/ztest-zbcss-async/zbcss_async.testlog" "result 0" "expected 0"
check_testlog "build/zbrowser-smoke/ztest-sysstat/sysstat.testlog" "result 7" "expected 7"
check_testlog "build/zbrowser-smoke/ztest-hwinfo/hwinfo.testlog" "result 0" "expected 0"
check_testlog "build/zbrowser-smoke/ztest-gfxdemo/gfxdemo.testlog" "result 9" "expected 9"
check_testlog "build/zbrowser-smoke/ztest-mousedemo/mousedemo.testlog" "result 0" "expected 0"
check_testlog "build/zbrowser-smoke/ztest-selfhost/selfhost.testlog" "result 41" "expected 41"
check_testlog "build/zbrowser-smoke/ztest-zmake/zmake.testlog" "result 0" "expected 0"
check_testlog "build/zbrowser-smoke/ztest-zreport/zreport.testlog" "result 13" "expected 13"
check_buildlog_line "examples/zlang/output_name_project/build/kernel.buildlog" "output custom_named_output.bin"
check_buildlog_line "examples/zlang/glob_literal_project/build/kernel.buildlog" "output glob_literal.bin"
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
  if manifest_is_known_negative "$manifest"; then
    continue
  fi

  probe_rel="${manifest#examples/}"
  probe_dir="build/zbrowser-smoke/all-manifests/${probe_rel%.zbuild}"
  mkdir -p "$probe_dir"
  if ! scripts/zbuild-host.sh "$manifest" "$probe_dir" >"$probe_dir/probe.log" 2>&1; then
    printf 'zbrowser-compile-smoke: broad manifest probe failed for %s\n' "$manifest" >&2
    cat "$probe_dir/probe.log" >&2
    exit 1
  fi
done < <(find examples -name '*.zbuild' -type f | sort)

while IFS= read -r manifest; do
  if manifest_is_known_negative "$manifest"; then
    continue
  fi

  install_rel="${manifest#examples/}"
  install_root="build/zbrowser-smoke/all-installs/${install_rel%.zbuild}"
  mkdir -p "$install_root"
  if ! scripts/zinstall-host.sh "$manifest" "$install_root" >"$install_root/install.log" 2>&1; then
    printf 'zbrowser-compile-smoke: broad install probe failed for %s\n' "$manifest" >&2
    cat "$install_root/install.log" >&2
    exit 1
  fi
  check_installed_manifest "$manifest" "$install_root"
done < <(find examples -name '*.zbuild' -type f | sort)
