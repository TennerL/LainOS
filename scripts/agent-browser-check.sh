#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

scripts/bootstrap-browser-deps.sh
scripts/zbrowser-compile-smoke.sh
make build/kernel.elf zcc-smoke lainfs-smoke

missing_tools=()
for tool in mkfs.fat mcopy mmd sgdisk xorriso qemu-system-x86_64; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    missing_tools+=("$tool")
  fi
done

if [ "${#missing_tools[@]}" -ne 0 ]; then
  printf 'Skipping full image/QEMU checks; missing host tools: %s\n' "${missing_tools[*]}" >&2
  printf 'Install the README dependencies, especially dosfstools, mtools, gdisk, xorriso, qemu-system-x86, and ovmf.\n' >&2
  exit 0
fi

make
scripts/zbrowser-selfhost-smoke.sh
