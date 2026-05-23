#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

for tool in mkfs.fat mcopy mmd xorriso qemu-system-x86_64 timeout nc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Skipping zbrowser launch repro; missing host tool: %s\n' "$tool" >&2
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
  printf 'Skipping zbrowser launch repro; missing OVMF firmware files.\n' >&2
  exit 0
fi

timeout_seconds="${ZBROWSER_LAUNCH_REPRO_TIMEOUT_SECONDS:-45}"
launch_page="${ZBROWSER_LAUNCH_REPRO_PAGE:-zbrowser_smoke.html}"
smoke_img="build/zbrowser-launch-repro.data.img"
smoke_vars="build/OVMF_VARS.zbrowser-launch-repro.fd"
serial_log="build/zbrowser-launch-repro.serial.log"
monitor_socket="build/zbrowser-launch-repro.monitor.sock"
screenshot_1="build/zbrowser-launch-repro-1.ppm"
screenshot_2="build/zbrowser-launch-repro-2.ppm"
seed_root="build/zbrowser-launch-repro.seed"
host_build_root="build/zbrowser-launch-repro"
page_source="examples/${launch_page}"

mkdir -p "$host_build_root"
make build/tools/lainfs_check_host build/tools/lainfs_seed build/tools/zmod_link_host

if [ ! -f build/esp.img ] || [ ! -f build/data.img ]; then
  printf 'zbrowser launch repro: missing build/esp.img or build/data.img; run a normal build first.\n' >&2
  exit 1
fi

if [ ! -f "$page_source" ]; then
  printf 'zbrowser launch repro: missing page %s\n' "$page_source" >&2
  exit 1
fi

build/tools/zmod_link_host \
  examples/zbrowser_html.Z "$host_build_root/zbrowser_html.zo" \
  examples/zbrowser_module.Z "$host_build_root/zbrowser_module.zo"

cp build/data.img "$smoke_img"
cp "$ovmf_vars" "$smoke_vars"
rm -rf "$seed_root"
mkdir -p "$seed_root/mods"
cp "$host_build_root/zbrowser_html.zo" "$seed_root/mods/zbrowser_html.zo"
cp "$host_build_root/zbrowser_module.zo" "$seed_root/mods/zbrowser_module.zo"
mkdir -p "$(dirname "$seed_root/mods/$launch_page")"
cp "$page_source" "$seed_root/mods/$launch_page"
if [ -d examples/styles ]; then
  cp -R examples/styles "$seed_root/mods/styles"
fi
printf '%s\n' "$launch_page" >"$seed_root/mods/browser.url"
: >"$seed_root/mods/zbrowser.autostart"
cat >"$seed_root/autoexec" <<'EOF'
mkdir mods
cd mods
desktop
EOF
build/tools/lainfs_seed \
  "$smoke_img" \
  Makefile README.md SELFHOSTING.md boot kernel examples \
  "$seed_root/autoexec" \
  "$seed_root/mods" >/dev/null
: >"$serial_log"
rm -f "$monitor_socket" "$screenshot_1" "$screenshot_2"

capture_screendump() {
  local delay="$1"
  local output="$2"
  local attempts="${3:-6}"
  local try=0
  (
    sleep "$delay"
    while [ "$try" -lt "$attempts" ]; do
      if [ -S "$monitor_socket" ]; then
        printf 'screendump %s\n' "$output" | nc -U "$monitor_socket" >/dev/null 2>&1 || true
      fi
      if [ -f "$output" ]; then
        exit 0
      fi
      try=$((try + 1))
      sleep 2
    done
  ) &
}

capture_screendump 10 "$screenshot_1"
capture_screendump 20 "$screenshot_2"

qemu_status=0
if ! timeout "${timeout_seconds}s" qemu-system-x86_64 \
  -m 256M \
  -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
  -drive if=pflash,format=raw,file="$smoke_vars" \
  -drive format=raw,file=build/esp.img,if=ide,index=0 \
  -drive format=raw,file="$smoke_img",if=ide,index=1 \
  -display none \
  -serial "file:$serial_log" \
  -monitor "unix:$monitor_socket,server,nowait"
then
  qemu_status=$?
fi

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  printf 'zbrowser launch repro: QEMU exited with status %s\n' "$qemu_status" >&2
  exit 1
fi

for ppm in "$screenshot_1" "$screenshot_2"; do
  if [ -f "$ppm" ]; then
    printf 'zbrowser launch repro: framebuffer capture %s sha256=%s\n' \
      "$(basename "$ppm")" \
      "$(sha256sum "$ppm" | awk '{print $1}')"
  else
    printf 'zbrowser launch repro: framebuffer capture missing %s\n' "$(basename "$ppm")"
  fi
done

printf 'zbrowser launch repro: serial log follows\n'
tail -n 220 "$serial_log" || true
