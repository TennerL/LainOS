#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

for tool in mkfs.fat mcopy mmd xorriso qemu-system-x86_64 timeout; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Skipping zbrowser CSS panic repro; missing host tool: %s\n' "$tool" >&2
    exit 0
  fi
done

ovmf_code=
ovmf_vars=
for code_path in \
  /usr/share/OVMF/OVMF_CODE_4M.fd \
  /usr/share/edk2/x64/OVMF_CODE.4m.fd
do
  if [ -f "$code_path" ]; then
    ovmf_code="$code_path"
    break
  fi
done

for vars_path in \
  /usr/share/OVMF/OVMF_VARS_4M.fd \
  /usr/share/edk2/x64/OVMF_VARS.4m.fd
do
  if [ -f "$vars_path" ]; then
    ovmf_vars="$vars_path"
    break
  fi
done

if [ -z "$ovmf_code" ] || [ -z "$ovmf_vars" ]; then
  printf 'Skipping zbrowser CSS panic repro; missing OVMF firmware files.\n' >&2
  exit 0
fi

timeout_seconds="${ZBROWSER_CSS_REPRO_TIMEOUT_SECONDS:-90}"
smoke_img="build/zbrowser-css-repro.data.img"
smoke_vars="build/OVMF_VARS.zbrowser-css-repro.fd"
serial_log="build/zbrowser-css-repro.serial.log"
seed_root="build/zbrowser-css-repro.seed"

make build/tools/lainfs_check_host build/tools/lainfs_seed build/esp.img reseed-data
cp build/data.img "$smoke_img"
cp "$ovmf_vars" "$smoke_vars"
rm -rf "$seed_root"
mkdir -p "$seed_root"
cat >"$seed_root/autoexec" <<'EOF'
mkdir mods
cd mods
cp R:/examples/kernel_api.Z kernel_api.Z
cp R:/examples/zbrowser_css_repro.Z zbrowser_css_repro.Z
cp R:/examples/zbrowser_css_repro.zbuild zbrowser_css_repro.zbuild
cp R:/examples/zbrowser_smoke.html zbrowser_smoke.html
ztest zbrowser_css_repro
poweroff
EOF
build/tools/lainfs_seed "$smoke_img" Makefile README.md SELFHOSTING.md boot kernel examples "$seed_root/autoexec" >/dev/null
: >"$serial_log"

qemu_status=0
if ! timeout "${timeout_seconds}s" qemu-system-x86_64 \
  -m 256M \
  -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
  -drive if=pflash,format=raw,file="$smoke_vars" \
  -drive format=raw,file=build/esp.img,if=ide,index=0 \
  -drive format=raw,file="$smoke_img",if=ide,index=1 \
  -display none \
  -serial "file:$serial_log" \
  -monitor none
then
  qemu_status=$?
fi

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  printf 'zbrowser CSS panic repro: QEMU exited with status %s\n' "$qemu_status" >&2
  exit 1
fi

if build/tools/lainfs_check_host exists "$smoke_img" mods/zbrowser_css_repro.testlog; then
  printf 'zbrowser CSS panic repro: test log follows\n'
  build/tools/lainfs_check_host cat "$smoke_img" mods/zbrowser_css_repro.testlog
else
  printf 'zbrowser CSS panic repro: missing mods/zbrowser_css_repro.testlog\n' >&2
fi

printf 'zbrowser CSS panic repro: serial log follows\n'
tail -n 120 "$serial_log" || true
