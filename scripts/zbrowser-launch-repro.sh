#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

for tool in mkfs.fat mcopy mmd xorriso qemu-system-x86_64 timeout; do
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

timeout_seconds="${ZBROWSER_LAUNCH_REPRO_TIMEOUT_SECONDS:-}"
launch_page="${ZBROWSER_LAUNCH_REPRO_PAGE:-zbrowser_border_box.html}"
send_keys="${ZBROWSER_LAUNCH_REPRO_SENDKEYS:-}"
send_keys_delay="${ZBROWSER_LAUNCH_REPRO_SENDKEYS_DELAY_SECONDS:-1}"
post_keys_capture_delay="${ZBROWSER_LAUNCH_REPRO_POST_KEYS_CAPTURE_DELAY_SECONDS:-2}"
idle_capture_delay="${ZBROWSER_LAUNCH_REPRO_IDLE_CAPTURE_DELAY_SECONDS:-6}"
capture_trigger_pattern="${ZBROWSER_LAUNCH_REPRO_CAPTURE_TRIGGER_PATTERN:-phase page}"
idle_trigger_pattern="${ZBROWSER_LAUNCH_REPRO_IDLE_TRIGGER_PATTERN:-zbrowser redraw-summary page}"
send_keys_trigger_pattern="${ZBROWSER_LAUNCH_REPRO_SENDKEYS_TRIGGER_PATTERN:-zbrowser renderer-status}"
browser_mode="${ZBROWSER_LAUNCH_REPRO_BROWSER:-netsurf}"
require_html_redraw="${ZBROWSER_LAUNCH_REPRO_REQUIRE_HTML_REDRAW:-1}"
auto_quit="${ZBROWSER_LAUNCH_REPRO_AUTO_QUIT:-1}"
auto_quit_delay="${ZBROWSER_LAUNCH_REPRO_AUTO_QUIT_DELAY_SECONDS:-7}"
idle_max_full="${ZBROWSER_LAUNCH_REPRO_IDLE_MAX_FULL:-3}"
idle_max_chrome="${ZBROWSER_LAUNCH_REPRO_IDLE_MAX_CHROME:-4}"
idle_max_page="${ZBROWSER_LAUNCH_REPRO_IDLE_MAX_PAGE:-2}"
idle_max_poll_full="${ZBROWSER_LAUNCH_REPRO_IDLE_MAX_POLL_FULL:-1}"
idle_max_poll_chrome="${ZBROWSER_LAUNCH_REPRO_IDLE_MAX_POLL_CHROME:-4}"
idle_max_render_fail="${ZBROWSER_LAUNCH_REPRO_IDLE_MAX_RENDER_FAIL:-0}"
min_rendered_bitmaps="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAPS:-0}"
min_rendered_objects="${ZBROWSER_LAUNCH_REPRO_MIN_OBJECTS:-0}"
require_style_sample="${ZBROWSER_LAUNCH_REPRO_REQUIRE_STYLE_SAMPLE:-}"
if [ -z "$require_style_sample" ]; then
  if [ "$launch_page" = "zbrowser_border_box.html" ]; then
    require_style_sample=1
  else
    require_style_sample=0
  fi
fi
qemu_memory="${ZBROWSER_LAUNCH_REPRO_MEMORY:-2048M}"
qemu_smp="${ZBROWSER_LAUNCH_REPRO_SMP:-2}"
qemu_accel="${ZBROWSER_LAUNCH_REPRO_ACCEL:-}"
qemu_accel_auto=0
if [ -z "$qemu_accel" ]; then
  qemu_accel_auto=1
  if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    qemu_accel="kvm"
  else
    qemu_accel="tcg"
  fi
fi
if [ -z "$timeout_seconds" ]; then
  if [ "$qemu_accel" = "tcg" ]; then
    timeout_seconds=900
  else
    timeout_seconds=300
  fi
fi
smoke_img="build/zbrowser-launch-repro.data.img"
smoke_vars="build/OVMF_VARS.zbrowser-launch-repro.fd"
serial_log="build/zbrowser-launch-repro.serial.log"
monitor_socket="build/zbrowser-launch-repro.monitor.sock"
screenshot_1="build/zbrowser-launch-repro-1.ppm"
screenshot_2="build/zbrowser-launch-repro-2.ppm"
seed_root="build/zbrowser-launch-repro.seed"
host_build_root="build/zbrowser-launch-repro"
page_source="examples/${launch_page}"
monitor_client=
restore_ramdisk_seed=0

if command -v nc >/dev/null 2>&1; then
  monitor_client="nc"
elif command -v ncat >/dev/null 2>&1; then
  monitor_client="ncat"
elif command -v python3 >/dev/null 2>&1; then
  monitor_client="python3"
fi

restore_default_ramdisk_seed() {
  if [ "$restore_ramdisk_seed" -eq 1 ] && [ -x build/tools/ramdisk_seed_gen ]; then
    make refresh-ramdisk >/dev/null || true
  fi
}
trap restore_default_ramdisk_seed EXIT

mkdir -p "$host_build_root"
make build/tools/lainfs_check_host build/tools/lainfs_seed build/tools/ramdisk_seed_gen build/tools/zmod_link_host build/zbrowser-netsurf-full/.stamp

if [ ! -f build/data.img ]; then
  printf 'zbrowser launch repro: missing build/data.img; run a normal build first.\n' >&2
  exit 1
fi

if [ ! -f "$page_source" ]; then
  printf 'zbrowser launch repro: missing page %s\n' "$page_source" >&2
  exit 1
fi

if [ "$browser_mode" = "legacy" ]; then
  build/tools/zmod_link_host \
    examples/zbrowser_html.Z "$host_build_root/zbrowser_html.zo" \
    examples/zbrowser_module.Z "$host_build_root/zbrowser_module.zo"
fi

cp build/data.img "$smoke_img"
cp "$ovmf_vars" "$smoke_vars"
rm -rf "$seed_root"
mkdir -p "$seed_root/mods"
if [ "$browser_mode" = "legacy" ]; then
  cp "$host_build_root/zbrowser_html.zo" "$seed_root/mods/zbrowser_html.zo"
  cp "$host_build_root/zbrowser_module.zo" "$seed_root/mods/zbrowser_module.zo"
fi
find build/zbrowser-netsurf-full -maxdepth 1 \( -name '*.zo' -o -name '*.Z' -o -name '*.zbuild' -o -name '*.zp[0-9][0-9]' \) \
  -exec cp {} "$seed_root/mods/" \;
mkdir -p "$(dirname "$seed_root/mods/$launch_page")"
cp "$page_source" "$seed_root/mods/$launch_page"
if [ -d examples/styles ]; then
  cp -R examples/styles "$seed_root/mods/styles"
fi
printf '%s\n' "$launch_page" >"$seed_root/mods/browser.url"
printf '%s\n' "$browser_mode" >"$seed_root/mods/zbrowser.autostart"
cat >"$seed_root/autoexec" <<'EOF'
mkdir mods
cd mods
cat zbrowser.autostart
cat browser.url
desktop
EOF

ramdisk_seed_args=(
  "$seed_root/autoexec=examples/autoexec"
)
while IFS= read -r seed_path; do
  ramdisk_seed_args+=("$seed_path=mods/$(basename "$seed_path")")
done < <(find "$seed_root/mods" -maxdepth 1 -type f | sort)
if [ -d "$seed_root/mods/styles" ]; then
  while IFS= read -r seed_path; do
    ramdisk_seed_args+=("$seed_path=mods/${seed_path#"$seed_root/mods/"}")
  done < <(find "$seed_root/mods/styles" -type f | sort)
fi
build/tools/ramdisk_seed_gen build/ramdisk_seed.h "${ramdisk_seed_args[@]}" >/dev/null
restore_ramdisk_seed=1
make build/esp.img
: >"$serial_log"
rm -f "$monitor_socket" "$screenshot_1" "$screenshot_2"

monitor_send_command() {
  local command="$1"

  if [ -z "$monitor_client" ]; then
    return 1
  fi
  if [ ! -S "$monitor_socket" ]; then
    return 1
  fi

  if [ "$monitor_client" = "nc" ]; then
    printf '%s\n' "$command" | nc -N -U "$monitor_socket" >/dev/null 2>&1
  elif [ "$monitor_client" = "ncat" ]; then
    printf '%s\n' "$command" | ncat -U "$monitor_socket" >/dev/null 2>&1
  else
    python3 - "$monitor_socket" "$command" <<'PY' >/dev/null 2>&1
import socket
import sys

sock_path, command = sys.argv[1], sys.argv[2]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    sock.settimeout(2.0)
    sock.connect(sock_path)
    try:
        sock.recv(4096)
    except TimeoutError:
        pass
    sock.sendall((command + "\n").encode("ascii"))
finally:
    sock.close()
PY
  fi
}

capture_screendump() {
  local delay="$1"
  local output="$2"
  local attempts="${3:-6}"
  local try=0
  (
    sleep "$delay"
    while [ "$try" -lt "$attempts" ]; do
      monitor_send_command "screendump $output" || true
      if [ -f "$output" ]; then
        exit 0
      fi
      try=$((try + 1))
      sleep 2
    done
  ) &
}

capture_screendump_after_log() {
  local pattern="$1"
  local output="$2"
  local settle_delay="${3:-0}"
  local wait_timeout="${4:-35}"
  local attempts="${5:-6}"
  (
    local waited=0
    while [ "$waited" -lt "$wait_timeout" ]; do
      if grep -q "$pattern" "$serial_log" 2>/dev/null; then
        if [ "$settle_delay" -gt 0 ]; then
          sleep "$settle_delay"
        fi
        capture_screendump 0 "$output" "$attempts"
        exit 0
      fi
      sleep 1
      waited=$((waited + 1))
    done
    capture_screendump 0 "$output" "$attempts"
  ) &
}

send_keys_after_log() {
  local pattern="$1"
  local keys_csv="$2"
  local settle_delay="${3:-0}"
  local wait_timeout="${4:-35}"
  (
    local waited=0
    local key_name=
    local key_names=()

    while [ "$waited" -lt "$wait_timeout" ]; do
      if grep -q "$pattern" "$serial_log" 2>/dev/null; then
        if [ "$settle_delay" -gt 0 ]; then
          sleep "$settle_delay"
        fi
        IFS=',' read -r -a key_names <<<"$keys_csv"
        for key_name in "${key_names[@]}"; do
          key_name="${key_name#"${key_name%%[![:space:]]*}"}"
          key_name="${key_name%"${key_name##*[![:space:]]}"}"
          if [ -n "$key_name" ]; then
            monitor_send_command "sendkey $key_name" || true
            sleep 1
          fi
        done
        capture_screendump "$post_keys_capture_delay" "$screenshot_2" 8
        exit 0
      fi
      sleep 1
      waited=$((waited + 1))
    done
    capture_screendump "$post_keys_capture_delay" "$screenshot_2" 8
  ) &
}

quit_after_log() {
  local pattern="$1"
  local settle_delay="${2:-10}"
  local wait_timeout="${3:-45}"
  (
    local waited=0
    while [ "$waited" -lt "$wait_timeout" ]; do
      if grep -q "$pattern" "$serial_log" 2>/dev/null; then
        sleep "$settle_delay"
        monitor_send_command "quit" || true
        exit 0
      fi
      sleep 1
      waited=$((waited + 1))
    done
  ) &
}

start_repro_watchers() {
  capture_screendump_after_log "$capture_trigger_pattern" "$screenshot_1" 1 35 8
  if [ -n "$send_keys" ]; then
    send_keys_after_log "$send_keys_trigger_pattern" "$send_keys" "$send_keys_delay" 40
  else
    capture_screendump_after_log "$idle_trigger_pattern" "$screenshot_2" "$idle_capture_delay" 60 8
  fi
  if [ "$auto_quit" = "1" ]; then
    quit_after_log "$idle_trigger_pattern" "$auto_quit_delay" "$timeout_seconds"
  fi
}

run_qemu_once() {
  : >"$serial_log"
  rm -f "$monitor_socket" "$screenshot_1" "$screenshot_2"
  start_repro_watchers
  timeout "${timeout_seconds}s" qemu-system-x86_64 \
    -accel "$qemu_accel" \
    -smp "$qemu_smp" \
    -m "$qemu_memory" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
    -drive if=pflash,format=raw,file="$smoke_vars" \
    -drive format=raw,file=build/esp.img,if=ide,index=0 \
    -drive format=raw,file="$smoke_img",if=ide,index=1 \
    -display none \
    -serial "file:$serial_log" \
    -monitor "unix:$monitor_socket,server,nowait"
}

qemu_status=0
set +e
run_qemu_once
qemu_status=$?
if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ] &&
   [ "$qemu_accel_auto" -eq 1 ] && [ "$qemu_accel" = "kvm" ]; then
  printf 'zbrowser launch repro: auto-selected KVM failed; retrying with TCG\n' >&2
  qemu_accel="tcg"
  if [ -z "${ZBROWSER_LAUNCH_REPRO_TIMEOUT_SECONDS:-}" ]; then
    timeout_seconds=900
  fi
  run_qemu_once
  qemu_status=$?
fi
set -e

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

if grep -q 'zbrowser renderer-status' "$serial_log"; then
  printf 'zbrowser launch repro: renderer status lines follow\n'
  grep -A10 'zbrowser renderer-status' "$serial_log" | tail -n 80 || true
else
  if grep -q 'zbrowser_netsurf module loaded' "$serial_log"; then
    printf 'zbrowser launch repro: module loaded but renderer status did not arrive before timeout\n' >&2
  elif grep -q 'desktop: zbrowser autostart requested' "$serial_log"; then
    printf 'zbrowser launch repro: autostart requested but module did not finish loading before timeout\n' >&2
  else
    printf 'zbrowser launch repro: missing renderer status marker\n' >&2
  fi
  printf 'zbrowser launch repro: serial log follows\n'
  tail -n 220 "$serial_log" || true
  exit 1
fi

if grep -q 'zbrowser visual-sample' "$serial_log"; then
  printf 'zbrowser launch repro: visual sample lines follow\n'
  grep 'zbrowser visual-sample' "$serial_log" | tail -n 10 || true
fi

if grep -q 'zbrowser redraw-summary' "$serial_log"; then
  printf 'zbrowser launch repro: redraw summary lines follow\n'
  grep 'zbrowser redraw-summary' "$serial_log" | tail -n 10 || true
else
  printf 'zbrowser launch repro: missing redraw summary marker\n' >&2
  tail -n 220 "$serial_log" || true
  exit 1
fi

extract_visual_sample_value() {
  local line="$1"
  local key="$2"

  awk -v key="$key" '{
    for (i = 1; i < NF; ++i) {
      if ($i == key) {
        print $(i + 1)
        exit
      }
    }
  }' <<<"$line"
}

require_visual_sample_value() {
  local line="$1"
  local key="$2"
  local expected="$3"
  local actual

  actual="$(extract_visual_sample_value "$line" "$key")"
  if [ "$actual" != "$expected" ]; then
    printf 'zbrowser launch repro: visual sample %s expected %s got %s\n' \
      "$key" "$expected" "${actual:-missing}" >&2
    tail -n 220 "$serial_log" || true
    exit 1
  fi
}

renderer_path=unknown
renderer_failed=0

if grep -q 'raw-fallback\|raw fallback' "$serial_log"; then
  renderer_path=raw-fallback
  renderer_failed=1
elif grep -q 'NS dom fallback\|DOM painter fallback\|dom-fallback' "$serial_log"; then
  renderer_path=dom-fallback
  renderer_failed=1
elif grep -q 'NS html redraw failed\|NetSurf HTML layout failed\|html-redraw-failed' "$serial_log"; then
  renderer_path=html-redraw-failed
  renderer_failed=1
elif grep -q 'status NS rendered\|NS html redraw' "$serial_log"; then
  renderer_path=html-redraw
fi

if [ "$renderer_failed" -ne 0 ]; then
  printf 'zbrowser launch repro: renderer path=%s\n' "$renderer_path" >&2
  tail -n 220 "$serial_log" || true
  exit 1
fi
if [ "$renderer_path" = "html-redraw" ]; then
  if ! grep -q 'zbrowser visual-sample' "$serial_log"; then
    printf 'zbrowser launch repro: missing visual sample after html-redraw\n' >&2
    tail -n 220 "$serial_log" || true
    exit 1
  fi
  if [ "$require_style_sample" = "1" ]; then
    visual_sample="$(grep 'zbrowser visual-sample' "$serial_log" | tail -n 1)"
    require_visual_sample_value "$visual_sample" "body" "0000000000F7F3EA"
    require_visual_sample_value "$visual_sample" "frame-border" "00000000002F4858"
    require_visual_sample_value "$visual_sample" "frame-fill" "0000000000FFFDF8"
    require_visual_sample_value "$visual_sample" "heading-text" "0000000000355C7D"
    require_visual_sample_value "$visual_sample" "badge-border" "00000000008D3B12"
    require_visual_sample_value "$visual_sample" "badge-fill" "0000000000FFE8D6"
  fi
  idle_summary="$(grep 'zbrowser redraw-summary' "$serial_log" | tail -n 1)"
  idle_full="$(awk -v key="full" '{value=""; for (i=1; i<NF; ++i) if ($i == key) value=$(i + 1); print value}' <<<"$idle_summary")"
  idle_chrome="$(awk -v key="chrome" '{value=""; for (i=1; i<NF; ++i) if ($i == key) value=$(i + 1); print value}' <<<"$idle_summary")"
  idle_page="$(awk -v key="page" '{value=""; for (i=1; i<NF; ++i) if ($i == key) value=$(i + 1); print value}' <<<"$idle_summary")"
  idle_poll_full="$(awk -v key="poll-full" '{value=""; for (i=1; i<NF; ++i) if ($i == key) value=$(i + 1); print value}' <<<"$idle_summary")"
  idle_poll_chrome="$(awk -v key="poll-chrome" '{value=""; for (i=1; i<NF; ++i) if ($i == key) value=$(i + 1); print value}' <<<"$idle_summary")"
  idle_render_fail="$(awk -v key="render-fail" '{value=""; for (i=1; i<NF; ++i) if ($i == key) value=$(i + 1); print value}' <<<"$idle_summary")"
  if [ -z "$idle_full" ] || [ -z "$idle_chrome" ] ||
     [ -z "$idle_page" ] || [ -z "$idle_poll_full" ] ||
     [ -z "$idle_poll_chrome" ] || [ -z "$idle_render_fail" ]; then
    printf 'zbrowser launch repro: could not parse redraw summary: %s\n' "$idle_summary" >&2
    tail -n 220 "$serial_log" || true
    exit 1
  fi
  if [ "$idle_full" -gt "$idle_max_full" ] ||
     [ "$idle_chrome" -gt "$idle_max_chrome" ] ||
     [ "$idle_page" -gt "$idle_max_page" ] ||
     [ "$idle_poll_full" -gt "$idle_max_poll_full" ] ||
     [ "$idle_poll_chrome" -gt "$idle_max_poll_chrome" ] ||
     [ "$idle_render_fail" -gt "$idle_max_render_fail" ]; then
    printf 'zbrowser launch repro: redraw counters too high after idle wait: %s\n' "$idle_summary" >&2
    printf 'zbrowser launch repro: limits full<=%s chrome<=%s page<=%s poll-full<=%s poll-chrome<=%s render-fail<=%s\n' \
      "$idle_max_full" "$idle_max_chrome" "$idle_max_page" "$idle_max_poll_full" \
      "$idle_max_poll_chrome" "$idle_max_render_fail" >&2
    tail -n 220 "$serial_log" || true
    exit 1
  fi
  if [ "$min_rendered_bitmaps" -gt 0 ] || [ "$min_rendered_objects" -gt 0 ]; then
    rendered_status="$(grep 'status NS rendered' "$serial_log" | tail -n 1)"
    rendered_bitmaps="$(sed -n 's/.* bmp\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_objects="$(sed -n 's/.* obj\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_bitmaps="${rendered_bitmaps:-0}"
    rendered_objects="${rendered_objects:-0}"
    if [ "$rendered_bitmaps" -lt "$min_rendered_bitmaps" ] ||
       [ "$rendered_objects" -lt "$min_rendered_objects" ]; then
      printf 'zbrowser launch repro: rendered counters too low: %s\n' "$rendered_status" >&2
      printf 'zbrowser launch repro: minimums bmp>=%s obj>=%s\n' \
        "$min_rendered_bitmaps" "$min_rendered_objects" >&2
      tail -n 220 "$serial_log" || true
      exit 1
    fi
  fi
  printf 'zbrowser launch repro: renderer path=%s\n' "$renderer_path"
elif [ "$require_html_redraw" = "1" ]; then
  printf 'zbrowser launch repro: renderer path=%s; expected html-redraw\n' "$renderer_path" >&2
  tail -n 220 "$serial_log" || true
  exit 1
else
  printf 'zbrowser launch repro: renderer path=%s\n' "$renderer_path"
fi

printf 'zbrowser launch repro: serial log follows\n'
tail -n 220 "$serial_log" || true
