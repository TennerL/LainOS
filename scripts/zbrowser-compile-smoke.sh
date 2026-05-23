#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mkdir -p build/zbrowser-smoke
make build/tools/zcc_host build/tools/zmod_link_host

build/tools/zcc_host examples/zbrowser_module.Z build/zbrowser-smoke/zbrowser_module.asm
build/tools/zmod_link_host \
  examples/zbrowser_html.Z build/zbrowser-smoke/zbrowser_html.zo \
  examples/zbrowser_module.Z build/zbrowser-smoke/zbrowser_module.zo
