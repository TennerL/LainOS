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
cp R:/examples/libc_api.Z libc_api.Z
cp R:/examples/mini_zlib.Z mini_zlib.Z
cp R:/examples/mini_zlib_api.Z mini_zlib_api.Z
cp R:/examples/clib_port_smoke_module.Z clib_port_smoke_module.Z
cp R:/examples/clib_port_smoke_module.zbuild clib_port_smoke_module.zbuild
cp R:/examples/libc_smoke_module.Z libc_smoke_module.Z
cp R:/examples/libc_smoke_module.zbuild libc_smoke_module.zbuild
cp R:/examples/browser_selfhost_driver.Z browser_selfhost_driver.Z
cp R:/examples/browser_selfhost_driver.zbuild browser_selfhost_driver.zbuild
cp R:/examples/zbrowser_module.Z zbrowser_module.Z
cp R:/examples/zbrowser_netsurf.Z zbrowser_netsurf.Z
cp R:/examples/zbrowser_html.Z zbrowser_html.Z
cp R:/examples/zbrowser_html_api.Z zbrowser_html_api.Z
cp R:/examples/zbrowser_module.zbuild zbrowser_module.zbuild
cp R:/examples/zbrowser_netsurf.zbuild zbrowser_netsurf.zbuild
cp R:/examples/zbrowser_smoke.html zbrowser_smoke.html
mkdir ../selfhost_project
mkdir ../selfhost_project/include
mkdir ../selfhost_project/src
mkdir ../kernel_z
cp R:/examples/zlang/selfhost_project/kernel.zbuild ../selfhost_project/kernel.zbuild
cp R:/examples/zlang/selfhost_project/include/kernel_api.Z ../selfhost_project/include/kernel_api.Z
cp R:/examples/zlang/selfhost_project/include/selfhost_once_leaf.Z ../selfhost_project/include/selfhost_once_leaf.Z
cp R:/examples/zlang/selfhost_project/include/selfhost_once_middle.Z ../selfhost_project/include/selfhost_once_middle.Z
cp R:/examples/zlang/selfhost_project/include/selfhost_once_root.Z ../selfhost_project/include/selfhost_once_root.Z
cp R:/examples/zlang/selfhost_project/src/selfhost_demo.Z ../selfhost_project/src/selfhost_demo.Z
cp R:/kernel/z/kernel_z_selfhost.zbuild ../kernel_z/kernel_z_selfhost.zbuild
cp R:/kernel/z/clock_math.Z ../kernel_z/clock_math.Z
cp R:/kernel/z/status_math.Z ../kernel_z/status_math.Z
cp R:/kernel/z/zlink_probe.Z ../kernel_z/zlink_probe.Z
zinstall browser_selfhost_driver
exec browser_selfhost_driver.bin
cd ../kernel_z
zinstall kernel_z_selfhost
cd ../selfhost_project
ztest kernel
zinstall kernel
exec selfhost_project.bin
poweroff
EOF
build/tools/lainfs_seed "$smoke_img" Makefile README.md SELFHOSTING.md boot kernel examples "$seed_root/autoexec" >/dev/null
: >"$serial_log"

printf 'zbrowser self-host smoke: runtime=%s smp=%s mem=%s timeout=%ss\n' \
  "$qemu_runtime" "$qemu_smp" "$qemu_memory" "$timeout_seconds"
if [ "$qemu_runtime" != "kvm" ]; then
  printf 'zbrowser self-host smoke: running without KVM because %s\n' "$kvm_reason"
fi

qemu_start_epoch="$(date +%s)"
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
qemu_end_epoch="$(date +%s)"
set -e
qemu_elapsed="$((qemu_end_epoch - qemu_start_epoch))"

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  printf 'zbrowser self-host smoke: QEMU exited with status %s after %ss\n' "$qemu_status" "$qemu_elapsed" >&2
  exit 1
fi

check_file() {
  local fs_path="$1"
  if ! build/tools/lainfs_check_host exists "$smoke_img" "$fs_path"; then
    printf 'zbrowser self-host smoke: missing %s after %ss\n' "$fs_path" "$qemu_elapsed" >&2
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
check_file "mods/zbrowser_netsurf.buildlog"
check_file "mods/zbrowser_netsurf.zo"
check_file "mods/browser_selfhost_driver.bin"
check_file "mods/browser_selfhost.status"
check_file "mods/libc_smoke_module.buildlog"
check_file "mods/libc_smoke_module.zo"
check_file "mods/clib_port_smoke_module.buildlog"
check_file "mods/clib_port_smoke_module.zo"
check_file "mods/mini_zlib.zo"
check_file "kernel_z/kernel_z_selfhost.buildlog"
check_file "kernel_z/install/clock_math.zo"
check_file "kernel_z/install/status_math.zo"
check_file "kernel_z/install/zlink_probe.zo"
check_file "selfhost_project/build/kernel.bin"
check_file "selfhost_project/build/kernel.buildlog"
check_file "selfhost_project/build/kernel.testlog"
check_file "selfhost_project/build/selfhost_demo.zo"
check_file "selfhost_project/build/from_z_renamed.txt"
check_file "selfhost_project/selfhost_project.bin"

check_buildlog() {
  local fs_path="$1"
  local expected_objects="$2"
  local label="$3"
  local build_log

  build_log="$(build/tools/lainfs_check_host cat "$smoke_img" "$fs_path")"
  if ! printf '%s\n' "$build_log" | grep -q '^status module$'; then
    printf 'zbrowser self-host smoke: unexpected %s build log contents\n%s\n' "$label" "$build_log" >&2
    exit 1
  fi
  if ! printf '%s\n' "$build_log" | grep -q "^objects ${expected_objects}\$"; then
    printf 'zbrowser self-host smoke: unexpected %s object count\n%s\n' "$label" "$build_log" >&2
    exit 1
  fi
}

check_buildlog "mods/libc_smoke_module.buildlog" 1 "libc smoke module"
check_buildlog "mods/clib_port_smoke_module.buildlog" 2 "clib port smoke module"
check_buildlog "mods/zbrowser_module.buildlog" 2 "zbrowser module"
check_buildlog "mods/zbrowser_netsurf.buildlog" 1 "NetSurf module"
check_buildlog "kernel_z/kernel_z_selfhost.buildlog" 3 "kernel Z selfhost slice"

check_log_contains() {
  local fs_path="$1"
  local pattern="$2"
  local label="$3"
  local log_text

  log_text="$(build/tools/lainfs_check_host cat "$smoke_img" "$fs_path")"
  if ! printf '%s\n' "$log_text" | grep -q "$pattern"; then
    printf 'zbrowser self-host smoke: unexpected %s contents\n%s\n' "$label" "$log_text" >&2
    exit 1
  fi
}

check_log_contains "mods/browser_selfhost.status" 'browser selfhost install ok' "browser selfhost status"
check_log_contains "selfhost_project/build/kernel.buildlog" '^status ok$' "selfhost build log"
check_log_contains "selfhost_project/build/kernel.buildlog" '^objects 1$' "selfhost build log"
check_log_contains "selfhost_project/build/kernel.testlog" '^result 42$' "selfhost test log"
check_log_contains "selfhost_project/build/kernel.testlog" '^status ok$' "selfhost test log"
check_log_contains "selfhost_project/build/from_z_renamed.txt" 'created from selfhost_project' "selfhost output file"

if ! grep -q 'browser selfhost: ok' "$serial_log"; then
  printf 'zbrowser self-host smoke: expected browser selfhost driver success output\n' >&2
  tail -n 120 "$serial_log" >&2 || true
  exit 1
fi

if [ "$(grep -c 'selfhost project 42' "$serial_log" || true)" -lt 2 ]; then
  printf 'zbrowser self-host smoke: expected selfhost project to print twice (ztest + exec)\n' >&2
  tail -n 120 "$serial_log" >&2 || true
  exit 1
fi

printf 'zbrowser self-host smoke: ok (timeout=%ss elapsed=%ss)\n' "$timeout_seconds" "$qemu_elapsed"
