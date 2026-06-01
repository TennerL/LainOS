#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

for tool in mkfs.fat mcopy mmd xorriso qemu-system-x86_64 timeout nc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Skipping zbrowser keyboard repro; missing host tool: %s\n' "$tool" >&2
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
  printf 'Skipping zbrowser keyboard repro; missing OVMF firmware files.\n' >&2
  exit 0
fi

timeout_seconds="${ZBROWSER_KEYBOARD_REPRO_TIMEOUT_SECONDS:-55}"
launch_page="${ZBROWSER_KEYBOARD_REPRO_PAGE:-zbinput.html}"
prompt_pattern="${ZBROWSER_KEYBOARD_REPRO_PROMPT_PATTERN:-S:\\mods>}"
load_done_pattern="${ZBROWSER_KEYBOARD_REPRO_LOAD_DONE_PATTERN:-zbrowser start-load sync-done}"
baseline_keys="${ZBROWSER_KEYBOARD_REPRO_BASELINE_KEYS:-k,b,b,e,f,o,r,e,ret}"
desktop_keys="${ZBROWSER_KEYBOARD_REPRO_DESKTOP_KEYS:-d,e,s,k,t,o,p,ret}"
zbrowser_keys="${ZBROWSER_KEYBOARD_REPRO_ZBROWSER_KEYS:-l,down,down}"
return_keys="${ZBROWSER_KEYBOARD_REPRO_RETURN_KEYS:-ctrl-w}"
post_keys="${ZBROWSER_KEYBOARD_REPRO_POST_KEYS:-k,b,a,f,t,e,r,ret}"
phase_delay="${ZBROWSER_KEYBOARD_REPRO_PHASE_DELAY_SECONDS:-1}"
post_close_delay="${ZBROWSER_KEYBOARD_REPRO_POST_CLOSE_DELAY_SECONDS:-2}"
smoke_img="build/zbrowser-keyboard-repro.data.img"
smoke_vars="build/OVMF_VARS.zbrowser-keyboard-repro.fd"
serial_log="build/zbrowser-keyboard-repro.serial.log"
monitor_socket="build/zbrowser-keyboard-repro.monitor.sock"
screenshot_before="build/zbrowser-keyboard-repro-before.png"
screenshot_during="build/zbrowser-keyboard-repro-during.png"
screenshot_after="build/zbrowser-keyboard-repro-after.png"
seed_root="build/zbrowser-keyboard-repro.seed"
host_build_root="build/zbrowser-keyboard-repro"
page_source="examples/${launch_page}"

mkdir -p "$host_build_root"
make build/tools/lainfs_check_host build/tools/lainfs_seed build/tools/zmod_link_host
make build/esp.img build/data.img

if [ ! -f "$page_source" ]; then
  printf 'zbrowser keyboard repro: missing page %s\n' "$page_source" >&2
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
printf 'legacy\n' >"$seed_root/mods/zbrowser.autostart"
cat >"$seed_root/autoexec" <<'EOF'
mkdir mods
cd mods
EOF
build/tools/lainfs_seed \
  "$smoke_img" \
  Makefile README.md SELFHOSTING.md boot kernel examples \
  "$seed_root/autoexec" \
  "$seed_root/mods" >/dev/null
: >"$serial_log"
rm -f "$monitor_socket" "$screenshot_before" "$screenshot_during" "$screenshot_after"

monitor_send_command() {
  local command="$1"

  if [ ! -S "$monitor_socket" ]; then
    return 1
  fi

  printf '%s\n' "$command" | nc -N -U "$monitor_socket" >/dev/null 2>&1
}

wait_for_log() {
  local pattern="$1"
  local wait_timeout="${2:-40}"
  local waited=0

  while [ "$waited" -lt "$wait_timeout" ]; do
    if grep -Fq "$pattern" "$serial_log" 2>/dev/null; then
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done

  return 1
}

send_keys_csv() {
  local keys_csv="$1"
  local key_name=
  local key_names=()

  IFS=',' read -r -a key_names <<<"$keys_csv"
  for key_name in "${key_names[@]}"; do
    key_name="${key_name#"${key_name%%[![:space:]]*}"}"
    key_name="${key_name%"${key_name##*[![:space:]]}"}"
    if [ -n "$key_name" ]; then
      monitor_send_command "sendkey $key_name" || true
      sleep 1
    fi
  done
}

capture_screendump() {
  local output="$1"
  local attempts="${2:-8}"
  local try=0

  while [ "$try" -lt "$attempts" ]; do
    monitor_send_command "screendump $output -f png" || true
    if [ -f "$output" ]; then
      return 0
    fi
    try=$((try + 1))
    sleep 1
  done

  return 1
}

run_repro_phases() {
  if ! wait_for_log "$prompt_pattern" 35; then
    printf 'zbrowser keyboard repro: timed out waiting for shell prompt pattern %s\n' "$prompt_pattern" >&2
    return 1
  fi

  send_keys_csv "$baseline_keys"
  sleep "$phase_delay"
  capture_screendump "$screenshot_before" 8 || true

  send_keys_csv "$desktop_keys"
  if ! wait_for_log "$load_done_pattern" 40; then
    printf 'zbrowser keyboard repro: timed out waiting for browser load pattern %s\n' "$load_done_pattern" >&2
    return 1
  fi

  sleep "$phase_delay"
  send_keys_csv "$zbrowser_keys"
  sleep "$phase_delay"
  capture_screendump "$screenshot_during" 8 || true

  send_keys_csv "$return_keys"
  sleep "$post_close_delay"
  send_keys_csv "$post_keys"
  sleep "$phase_delay"
  capture_screendump "$screenshot_after" 8 || true
}

run_repro_phases &
phase_pid=$!

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

wait "$phase_pid" || true

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  printf 'zbrowser keyboard repro: QEMU exited with status %s\n' "$qemu_status" >&2
  exit 1
fi

for ppm in "$screenshot_before" "$screenshot_during" "$screenshot_after"; do
  if [ -f "$ppm" ]; then
    printf 'zbrowser keyboard repro: framebuffer capture %s sha256=%s\n' \
      "$(basename "$ppm")" \
      "$(sha256sum "$ppm" | awk '{print $1}')"
  else
    printf 'zbrowser keyboard repro: framebuffer capture missing %s\n' "$(basename "$ppm")"
  fi
done

printf 'zbrowser keyboard repro: serial markers\n'
for marker in "kbbefore" "kbafter" "desktop: zbrowser autostart opened" "$load_done_pattern"; do
  if grep -Fq "$marker" "$serial_log" 2>/dev/null; then
    printf '  present: %s\n' "$marker"
  else
    printf '  missing: %s\n' "$marker"
  fi
done

printf 'zbrowser keyboard repro: serial log follows\n'
tail -n 260 "$serial_log" || true
