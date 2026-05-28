#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mkdir -p build/zbrowser-smoke
make build/tools/zmod_link_host

scripts/zbuild-host.sh examples/zbrowser_module.zbuild build/zbrowser-smoke/zbrowser_module
scripts/zbuild-host.sh examples/zbrowser_netsurf.zbuild build/zbrowser-smoke/zbrowser_netsurf
scripts/zbuild-host.sh examples/zbrowser_css_repro.zbuild build/zbrowser-smoke/zbrowser_css_repro
scripts/zbuild-host.sh examples/zbcss_async.zbuild build/zbrowser-smoke/zbcss_async
scripts/zbuild-host.sh examples/zlang/selfhost_project/kernel.zbuild build/zbrowser-smoke/selfhost_project

check_output() {
  local path="$1"

  if [[ ! -s "$path" ]]; then
    printf 'zbrowser-compile-smoke: missing linked output %s\n' "$path" >&2
    exit 1
  fi
}

check_output "build/zbrowser-smoke/zbrowser_css_repro/zbrowser_css_repro.bin"
check_output "build/zbrowser-smoke/zbcss_async/zbcss_async.bin"
check_output "examples/zlang/selfhost_project/build/kernel.bin"
