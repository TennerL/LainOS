#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

for tool in mkfs.fat mcopy mmd xorriso qemu-system-x86_64 timeout; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Skipping zbrowser self-host smoke; missing host tool: %s\n' "$tool" >&2
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
  printf 'Skipping zbrowser self-host smoke; missing OVMF firmware files.\n' >&2
  exit 0
fi

smoke_img="build/zbrowser-selfhost.data.img"
smoke_vars="build/OVMF_VARS.zbrowser-selfhost.fd"
serial_log="build/zbrowser-selfhost.serial.log"
seed_root="build/zbrowser-selfhost.seed"
allow_tcg="${ZBROWSER_SELFHOST_ALLOW_TCG:-0}"
qemu_runtime="tcg"
qemu_accel_args=()
default_memory="256M"
default_smp="1"
kvm_reason="QEMU binary does not advertise KVM acceleration"

if qemu-system-x86_64 -accel help 2>/dev/null | grep -qx 'kvm'; then
  if [ ! -e /dev/kvm ]; then
    kvm_reason="/dev/kvm is missing"
  elif [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    kvm_reason="/dev/kvm is not accessible for uid=$(id -u) gid=$(id -g); groups=$(id -Gn)"
  else
    qemu_runtime="kvm"
    qemu_accel_args=(-enable-kvm -cpu host)
    default_memory="2048M"
    default_smp="4"
  fi
fi

if [ "$qemu_runtime" != "kvm" ] && [ "$allow_tcg" != "1" ]; then
  printf 'Skipping zbrowser self-host smoke; KVM is unavailable (%s). Set ZBROWSER_SELFHOST_ALLOW_TCG=1 to force slow TCG.\n' "$kvm_reason" >&2
  exit 0
fi

if [ -n "${ZBROWSER_SELFHOST_TIMEOUT_SECONDS:-}" ]; then
  timeout_seconds="${ZBROWSER_SELFHOST_TIMEOUT_SECONDS}"
elif [ "$qemu_runtime" = "kvm" ]; then
  timeout_seconds=180
else
  timeout_seconds=420
fi

qemu_memory="${ZBROWSER_SELFHOST_QEMU_MEMORY:-$default_memory}"
qemu_smp="${ZBROWSER_SELFHOST_QEMU_SMP:-$default_smp}"

make build/tools/lainfs_check_host build/tools/lainfs_seed build/esp.img reseed-data
cp build/data.img "$smoke_img"
cp "$ovmf_vars" "$smoke_vars"
rm -rf "$seed_root"
mkdir -p "$seed_root"
cat >"$seed_root/autoexec" <<'EOF'
mkdir mods
cd mods
cp R:/examples/kernel_api.Z kernel_api.Z
cp R:/examples/zbrowser_module.Z zbrowser_module.Z
cp R:/examples/zbrowser_html.Z zbrowser_html.Z
cp R:/examples/zbrowser_html_api.Z zbrowser_html_api.Z
cp R:/examples/zbrowser_module.zbuild zbrowser_module.zbuild
cp R:/examples/zbrowser_smoke.html zbrowser_smoke.html
zinstall zbrowser_module
poweroff
EOF
build/tools/lainfs_seed "$smoke_img" Makefile README.md SELFHOSTING.md boot kernel examples "$seed_root/autoexec" >/dev/null
: >"$serial_log"

printf 'zbrowser self-host smoke: runtime=%s smp=%s mem=%s timeout=%ss\n' \
  "$qemu_runtime" "$qemu_smp" "$qemu_memory" "$timeout_seconds"
if [ "$qemu_runtime" != "kvm" ]; then
  printf 'zbrowser self-host smoke: running without KVM because %s\n' "$kvm_reason"
fi

set +e
timeout "${timeout_seconds}s" qemu-system-x86_64 \
  "${qemu_accel_args[@]}" \
  -smp "$qemu_smp" \
  -m "$qemu_memory" \
  -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
  -drive if=pflash,format=raw,file="$smoke_vars" \
  -drive format=raw,file=build/esp.img,if=ide,index=0 \
  -drive format=raw,file="$smoke_img",if=ide,index=1 \
  -display none \
  -serial "file:$serial_log" \
  -monitor none
qemu_status=$?
set -e

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  printf 'zbrowser self-host smoke: QEMU exited with status %s\n' "$qemu_status" >&2
  exit 1
fi

check_file() {
  local fs_path="$1"
  if ! build/tools/lainfs_check_host exists "$smoke_img" "$fs_path"; then
    printf 'zbrowser self-host smoke: missing %s\n' "$fs_path" >&2
    printf 'zbrowser self-host smoke: root listing follows\n' >&2
    build/tools/lainfs_check_host ls "$smoke_img" / >&2 || true
    printf 'zbrowser self-host smoke: mods listing follows\n' >&2
    build/tools/lainfs_check_host ls "$smoke_img" mods >&2 || true
    printf 'zbrowser self-host smoke: serial log tail follows\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi
}

check_file "mods/zbrowser_module.buildlog"
check_file "mods/zbrowser_html.zo"
check_file "mods/zbrowser_module.zo"

build_log="$(build/tools/lainfs_check_host cat "$smoke_img" mods/zbrowser_module.buildlog)"
if ! printf '%s\n' "$build_log" | grep -q '^status module$'; then
  printf 'zbrowser self-host smoke: unexpected build log contents\n%s\n' "$build_log" >&2
  exit 1
fi
if ! printf '%s\n' "$build_log" | grep -q '^objects 2$'; then
  printf 'zbrowser self-host smoke: unexpected object count\n%s\n' "$build_log" >&2
  exit 1
fi

printf 'zbrowser self-host smoke: ok (timeout=%ss)\n' "$timeout_seconds"
