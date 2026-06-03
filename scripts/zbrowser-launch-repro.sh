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
serve_http="${ZBROWSER_LAUNCH_REPRO_SERVE_HTTP:-0}"
serve_http_port="${ZBROWSER_LAUNCH_REPRO_HTTP_PORT:-28080}"
launch_url="${ZBROWSER_LAUNCH_REPRO_URL:-}"
send_keys="${ZBROWSER_LAUNCH_REPRO_SENDKEYS:-}"
mouse_clicks="${ZBROWSER_LAUNCH_REPRO_MOUSE_CLICKS:-}"
send_keys_delay="${ZBROWSER_LAUNCH_REPRO_SENDKEYS_DELAY_SECONDS:-1}"
mouse_clicks_delay="${ZBROWSER_LAUNCH_REPRO_MOUSE_CLICKS_DELAY_SECONDS:-1}"
post_keys_capture_delay="${ZBROWSER_LAUNCH_REPRO_POST_KEYS_CAPTURE_DELAY_SECONDS:-2}"
idle_capture_delay="${ZBROWSER_LAUNCH_REPRO_IDLE_CAPTURE_DELAY_SECONDS:-6}"
capture_trigger_pattern="${ZBROWSER_LAUNCH_REPRO_CAPTURE_TRIGGER_PATTERN:-phase page}"
idle_trigger_pattern="${ZBROWSER_LAUNCH_REPRO_IDLE_TRIGGER_PATTERN:-zbrowser redraw-summary page}"
send_keys_trigger_pattern="${ZBROWSER_LAUNCH_REPRO_SENDKEYS_TRIGGER_PATTERN:-zbrowser renderer-status}"
browser_mode="${ZBROWSER_LAUNCH_REPRO_BROWSER:-netsurf}"
require_html_redraw="${ZBROWSER_LAUNCH_REPRO_REQUIRE_HTML_REDRAW:-1}"
require_frontend_smoke="${ZBROWSER_LAUNCH_REPRO_REQUIRE_FRONTEND_SMOKE:-}"
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
min_rendered_css="${ZBROWSER_LAUNCH_REPRO_MIN_CSS:-}"
min_rendered_rects="${ZBROWSER_LAUNCH_REPRO_MIN_RECTS:-}"
min_rendered_lines="${ZBROWSER_LAUNCH_REPRO_MIN_LINES:-}"
min_rendered_text="${ZBROWSER_LAUNCH_REPRO_MIN_TEXT:-}"
min_rendered_paths="${ZBROWSER_LAUNCH_REPRO_MIN_PATHS:-}"
min_rendered_polygons="${ZBROWSER_LAUNCH_REPRO_MIN_POLYGONS:-}"
min_rendered_discs="${ZBROWSER_LAUNCH_REPRO_MIN_DISCS:-}"
min_rendered_arcs="${ZBROWSER_LAUNCH_REPRO_MIN_ARCS:-}"
min_image_decodes="${ZBROWSER_LAUNCH_REPRO_MIN_IMAGE_DECODES:-}"
min_image_fallbacks="${ZBROWSER_LAUNCH_REPRO_MIN_IMAGE_FALLBACKS:-}"
min_image_errors="${ZBROWSER_LAUNCH_REPRO_MIN_IMAGE_ERRORS:-}"
min_bitmap_render_successes="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAP_RENDER_SUCCESSES:-}"
min_bitmap_renders="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAP_RENDERS:-}"
max_bitmap_render_errors="${ZBROWSER_LAUNCH_REPRO_MAX_BITMAP_RENDER_ERRORS:-}"
min_http_fetches="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_FETCHES:-0}"
min_http_successes="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_SUCCESSES:-0}"
min_type_rejects="${ZBROWSER_LAUNCH_REPRO_MIN_TYPE_REJECTS:-0}"
min_css_misses="${ZBROWSER_LAUNCH_REPRO_MIN_CSS_MISSES:-0}"
min_object_misses="${ZBROWSER_LAUNCH_REPRO_MIN_OBJECT_MISSES:-0}"
min_form_keys="${ZBROWSER_LAUNCH_REPRO_MIN_FORM_KEYS:-0}"
min_form_mouse="${ZBROWSER_LAUNCH_REPRO_MIN_FORM_MOUSE:-0}"
min_form_nav_creates="${ZBROWSER_LAUNCH_REPRO_MIN_FORM_NAV_CREATES:-0}"
min_form_nav_consumes="${ZBROWSER_LAUNCH_REPRO_MIN_FORM_NAV_CONSUMES:-0}"
min_js_execs="${ZBROWSER_LAUNCH_REPRO_MIN_JS_EXECS:-0}"
min_js_successes="${ZBROWSER_LAUNCH_REPRO_MIN_JS_SUCCESSES:-0}"
require_serial_fragment="${ZBROWSER_LAUNCH_REPRO_REQUIRE_SERIAL_FRAGMENT:-}"
require_pending_url_fragment="${ZBROWSER_LAUNCH_REPRO_REQUIRE_PENDING_URL_FRAGMENT:-}"
require_http_status="${ZBROWSER_LAUNCH_REPRO_REQUIRE_HTTP_STATUS:-}"
require_style_sample="${ZBROWSER_LAUNCH_REPRO_REQUIRE_STYLE_SAMPLE:-}"
if [ "$serve_http" = "1" ] && [ -z "$launch_url" ]; then
  launch_page="${ZBROWSER_LAUNCH_REPRO_PAGE:-zbrowser_http_smoke.html}"
  launch_url="http://10.0.2.2:${serve_http_port}/${launch_page}"
fi
if [ -z "$require_frontend_smoke" ]; then
  if [ "$browser_mode" = "netsurf" ]; then
    require_frontend_smoke=1
  else
    require_frontend_smoke=0
  fi
fi
if [ -z "$require_style_sample" ]; then
  if [ "$launch_page" = "zbrowser_border_box.html" ]; then
    require_style_sample=1
  else
    require_style_sample=0
  fi
fi
if [ "$launch_page" = "zbrowser_css_external.html" ]; then
  min_rendered_css="${min_rendered_css:-3}"
  min_rendered_rects="${min_rendered_rects:-10}"
  min_rendered_lines="${min_rendered_lines:-1}"
  min_rendered_text="${min_rendered_text:-7}"
fi
if [ "$launch_page" = "zbrowser_shape_primitives.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-4}"
  min_rendered_text="${min_rendered_text:-8}"
fi
if [ "$launch_page" = "zbrowser_radio_disc.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-4}"
  min_rendered_text="${min_rendered_text:-3}"
  min_rendered_discs="${min_rendered_discs:-2}"
  min_rendered_arcs="${min_rendered_arcs:-4}"
fi
if [ "$launch_page" = "zbrowser_border_join.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-3}"
  min_rendered_text="${min_rendered_text:-3}"
  min_rendered_polygons="${min_rendered_polygons:-2}"
fi
if [ "$launch_page" = "zbrowser_visual_png.html" ]; then
  min_rendered_css="${min_rendered_css:-3}"
  min_rendered_rects="${min_rendered_rects:-6}"
  min_rendered_text="${min_rendered_text:-1}"
  min_rendered_bitmaps="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAPS:-1}"
  min_rendered_objects="${ZBROWSER_LAUNCH_REPRO_MIN_OBJECTS:-1}"
  min_image_decodes="${min_image_decodes:-1}"
fi
if [ "$launch_page" = "zbrowser_bg_slice.html" ]; then
  min_rendered_css="${min_rendered_css:-3}"
  min_rendered_rects="${min_rendered_rects:-4}"
  min_rendered_text="${min_rendered_text:-2}"
  min_rendered_bitmaps="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAPS:-1}"
  min_rendered_objects="${ZBROWSER_LAUNCH_REPRO_MIN_OBJECTS:-1}"
  min_image_decodes="${min_image_decodes:-1}"
fi
if [ "$launch_page" = "zbrowser_bad_image.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-4}"
  min_rendered_text="${min_rendered_text:-1}"
  min_rendered_bitmaps="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAPS:-1}"
  min_rendered_objects="${ZBROWSER_LAUNCH_REPRO_MIN_OBJECTS:-1}"
  min_image_fallbacks="${min_image_fallbacks:-1}"
fi
if [ "$launch_page" = "zbrowser_http_smoke.html" ]; then
  min_rendered_css="${min_rendered_css:-3}"
  min_rendered_rects="${min_rendered_rects:-10}"
  min_rendered_lines="${min_rendered_lines:-1}"
  min_rendered_text="${min_rendered_text:-7}"
  min_http_fetches="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_FETCHES:-2}"
  min_http_successes="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_SUCCESSES:-2}"
  require_http_status="${require_http_status:-200}"
fi
if [ "$launch_page" = "zbrowser_remote_bg_http.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-3}"
  min_rendered_text="${min_rendered_text:-2}"
  min_rendered_bitmaps="${ZBROWSER_LAUNCH_REPRO_MIN_BITMAPS:-1}"
  min_image_decodes="${min_image_decodes:-1}"
  min_http_fetches="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_FETCHES:-1}"
  min_http_successes="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_SUCCESSES:-1}"
  require_http_status="${require_http_status:-200}"
fi
if [ "$launch_page" = "zbrowser_bad_css_type.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-4}"
  min_rendered_text="${min_rendered_text:-2}"
  min_http_fetches="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_FETCHES:-1}"
  min_http_successes="${ZBROWSER_LAUNCH_REPRO_MIN_HTTP_SUCCESSES:-1}"
  min_css_misses="${ZBROWSER_LAUNCH_REPRO_MIN_CSS_MISSES:-1}"
  min_object_misses="${ZBROWSER_LAUNCH_REPRO_MIN_OBJECT_MISSES:-1}"
  require_http_status="${require_http_status:-200}"
fi
if [ "$launch_page" = "zbrowser_form_smoke.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-10}"
  min_rendered_text="${min_rendered_text:-5}"
fi
if [ "$launch_page" = "zbrowser_js_baseline.html" ]; then
  min_rendered_css="${min_rendered_css:-1}"
  min_rendered_rects="${min_rendered_rects:-4}"
  min_rendered_text="${min_rendered_text:-2}"
  min_js_execs="${ZBROWSER_LAUNCH_REPRO_MIN_JS_EXECS:-1}"
  min_js_successes="${ZBROWSER_LAUNCH_REPRO_MIN_JS_SUCCESSES:-1}"
fi
min_rendered_css="${min_rendered_css:-0}"
min_rendered_rects="${min_rendered_rects:-0}"
min_rendered_lines="${min_rendered_lines:-0}"
min_rendered_text="${min_rendered_text:-0}"
min_rendered_paths="${min_rendered_paths:-0}"
min_rendered_polygons="${min_rendered_polygons:-0}"
min_rendered_discs="${min_rendered_discs:-0}"
min_rendered_arcs="${min_rendered_arcs:-0}"
min_image_decodes="${min_image_decodes:-0}"
min_image_fallbacks="${min_image_fallbacks:-0}"
min_image_errors="${min_image_errors:-0}"
min_bitmap_render_successes="${min_bitmap_render_successes:-0}"
min_bitmap_renders="${min_bitmap_renders:-0}"
max_bitmap_render_errors="${max_bitmap_render_errors:-999999}"
qemu_memory="${ZBROWSER_LAUNCH_REPRO_MEMORY:-2048M}"
qemu_smp="${ZBROWSER_LAUNCH_REPRO_SMP:-2}"
qemu_accel="${ZBROWSER_LAUNCH_REPRO_ACCEL:-}"
qemu_accel_auto=0
kvm_reason="not checked"
kvm_preflight="${ZBROWSER_LAUNCH_REPRO_KVM_PREFLIGHT:-1}"
kvm_preflight_log="build/zbrowser-launch-repro.kvm-preflight.log"

qemu_kvm_preflight_ok() {
  local status=0

  if [ "$kvm_preflight" != "1" ]; then
    return 0
  fi
  mkdir -p "$(dirname "$kvm_preflight_log")"
  set +e
  timeout 4s qemu-system-x86_64 \
    -accel kvm \
    -machine q35 \
    -nodefaults \
    -display none \
    -serial none \
    -monitor none \
    -S >/dev/null 2>"$kvm_preflight_log"
  status=$?
  set -e
  if [ "$status" -eq 124 ]; then
    return 0
  fi
  if [ -s "$kvm_preflight_log" ]; then
    kvm_reason="$(tr '\n' ' ' <"$kvm_preflight_log" | sed 's/[[:space:]][[:space:]]*/ /g')"
  else
    kvm_reason="KVM preflight exited with status $status"
  fi
  return 1
}

if [ -z "$qemu_accel" ]; then
  qemu_accel_auto=1
  if [ ! -e /dev/kvm ]; then
    kvm_reason="/dev/kvm is missing"
    qemu_accel="tcg"
  elif [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    kvm_reason="/dev/kvm is not accessible for uid=$(id -u) gid=$(id -g); groups=$(id -Gn)"
    qemu_accel="tcg"
  elif qemu_kvm_preflight_ok; then
    kvm_reason="available"
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
if [ -z "${ZBROWSER_LAUNCH_REPRO_MEMORY:-}" ] && [ "$qemu_accel" = "tcg" ]; then
  qemu_memory="768M"
fi
if [ -z "${ZBROWSER_LAUNCH_REPRO_SMP:-}" ] && [ "$qemu_accel" = "tcg" ]; then
  qemu_smp="1"
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
http_root="build/zbrowser-launch-repro.http-root"
http_server_pid=
monitor_client=
restore_ramdisk_seed=0

if command -v nc >/dev/null 2>&1; then
  monitor_client="nc"
elif command -v ncat >/dev/null 2>&1; then
  monitor_client="ncat"
elif command -v python3 >/dev/null 2>&1; then
  monitor_client="python3"
fi

cleanup_repro() {
  if [ -n "$http_server_pid" ]; then
    kill "$http_server_pid" >/dev/null 2>&1 || true
    wait "$http_server_pid" >/dev/null 2>&1 || true
  fi
  if [ "$restore_ramdisk_seed" -eq 1 ] && [ -x build/tools/ramdisk_seed_gen ]; then
    make refresh-ramdisk >/dev/null || true
  fi
}
trap cleanup_repro EXIT

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
if [ "$serve_http" = "1" ] && ! command -v python3 >/dev/null 2>&1; then
  printf 'Skipping zbrowser launch repro; HTTP mode needs python3 for host server.\n' >&2
  exit 0
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
if [ "$launch_page" = "zbrowser_form_smoke.html" ] ||
   [ "$launch_page" = "zbrowser_form_empty.html" ]; then
  cp examples/zbrowser_form_*.html "$seed_root/mods/"
fi
if [ -d examples/styles ]; then
  cp -R examples/styles "$seed_root/mods/styles"
fi
if [ -f third_party/netsurf/src/netsurf/resources/netsurf.png ]; then
  mkdir -p "$seed_root/mods/styles"
  cp third_party/netsurf/src/netsurf/resources/netsurf.png "$seed_root/mods/styles/zbrowser_netsurf.png"
fi
if [ "$serve_http" = "1" ]; then
  rm -rf "$http_root"
  mkdir -p "$http_root"
  cp "$page_source" "$http_root/$launch_page"
  if [ "$launch_page" = "zbrowser_form_smoke.html" ] ||
     [ "$launch_page" = "zbrowser_form_empty.html" ]; then
    cp examples/zbrowser_form_*.html "$http_root/"
  fi
  if [ -d examples/styles ]; then
    cp -R examples/styles "$http_root/styles"
  fi
  if [ -f third_party/netsurf/src/netsurf/resources/netsurf.png ]; then
    mkdir -p "$http_root/styles"
    cp third_party/netsurf/src/netsurf/resources/netsurf.png "$http_root/styles/zbrowser_netsurf.png"
  fi
  python3 -m http.server "$serve_http_port" --bind 127.0.0.1 --directory "$http_root" \
    >"$host_build_root/http-server.log" 2>&1 &
  http_server_pid=$!
  sleep 1
  if ! kill -0 "$http_server_pid" >/dev/null 2>&1; then
    printf 'zbrowser launch repro: HTTP server failed to start on port %s\n' "$serve_http_port" >&2
    cat "$host_build_root/http-server.log" >&2 || true
    exit 1
  fi
fi
printf '%s\n' "${launch_url:-$launch_page}" >"$seed_root/mods/browser.url"
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
rm -f build/kernel/ui/shell.o build/kernel.elf build/kernel.bin build/esp.img build/image/kernel.elf
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

send_mouse_clicks_after_log() {
  local pattern="$1"
  local clicks_spec="$2"
  local settle_delay="${3:-0}"
  local wait_timeout="${4:-35}"
  (
    local waited=0
    local click_spec=
    local click_items=()
    local click_x=
    local click_y=

    while [ "$waited" -lt "$wait_timeout" ]; do
      if grep -q "$pattern" "$serial_log" 2>/dev/null; then
        if [ "$settle_delay" -gt 0 ]; then
          sleep "$settle_delay"
        fi
        IFS=';' read -r -a click_items <<<"$clicks_spec"
        for click_spec in "${click_items[@]}"; do
          click_spec="${click_spec#"${click_spec%%[![:space:]]*}"}"
          click_spec="${click_spec%"${click_spec##*[![:space:]]}"}"
          if [ -z "$click_spec" ]; then
            continue
          fi
          click_x="${click_spec%%,*}"
          click_y="${click_spec#*,}"
          if [ -z "$click_x" ] || [ "$click_y" = "$click_spec" ] || [ -z "$click_y" ]; then
            continue
          fi
          monitor_send_command "mouse_move -10000 -10000" || true
          sleep 1
          monitor_send_command "mouse_move $click_x $click_y" || true
          sleep 1
          monitor_send_command "mouse_button 1" || true
          sleep 1
          monitor_send_command "mouse_button 0" || true
          sleep 1
        done
        if [ -z "$send_keys" ]; then
          capture_screendump "$post_keys_capture_delay" "$screenshot_2" 8
        fi
        exit 0
      fi
      sleep 1
      waited=$((waited + 1))
    done
    if [ -z "$send_keys" ]; then
      capture_screendump "$post_keys_capture_delay" "$screenshot_2" 8
    fi
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
  fi
  if [ -n "$mouse_clicks" ]; then
    send_mouse_clicks_after_log "$send_keys_trigger_pattern" "$mouse_clicks" "$mouse_clicks_delay" 40
  fi
  if [ -z "$send_keys" ] && [ -z "$mouse_clicks" ]; then
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
  printf 'zbrowser launch repro: runtime=%s smp=%s mem=%s timeout=%ss\n' \
    "$qemu_accel" "$qemu_smp" "$qemu_memory" "$timeout_seconds"
  if [ "$qemu_accel" != "kvm" ]; then
    printf 'zbrowser launch repro: running without KVM because %s\n' "$kvm_reason"
  fi
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
  if [ -z "${ZBROWSER_LAUNCH_REPRO_MEMORY:-}" ]; then
    qemu_memory="768M"
  fi
  if [ -z "${ZBROWSER_LAUNCH_REPRO_SMP:-}" ]; then
    qemu_smp="1"
  fi
  kvm_reason="KVM launch failed after preflight"
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

if grep -q 'zbrowser frontend-status' "$serial_log"; then
  printf 'zbrowser launch repro: frontend status lines follow\n'
  grep -A3 'zbrowser frontend-status' "$serial_log" | tail -n 32 || true
elif [ "$require_frontend_smoke" = "1" ]; then
  printf 'zbrowser launch repro: missing frontend status marker\n' >&2
  tail -n 220 "$serial_log" || true
  exit 1
fi
if [ "$require_frontend_smoke" = "1" ] &&
   ! grep -q 'detail .*smoke127' "$serial_log"; then
  printf 'zbrowser launch repro: frontend smoke did not prove bitmap render: expected smoke127\n' >&2
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
  if [ "$min_rendered_css" -gt 0 ] ||
     [ "$min_rendered_rects" -gt 0 ] ||
     [ "$min_rendered_lines" -gt 0 ] ||
     [ "$min_rendered_text" -gt 0 ] ||
     [ "$min_rendered_paths" -gt 0 ] ||
     [ "$min_rendered_polygons" -gt 0 ] ||
     [ "$min_rendered_discs" -gt 0 ] ||
     [ "$min_rendered_arcs" -gt 0 ] ||
     [ "$min_image_decodes" -gt 0 ] ||
     [ "$min_image_fallbacks" -gt 0 ] ||
     [ "$min_image_errors" -gt 0 ] ||
     [ "$min_bitmap_render_successes" -gt 0 ] ||
     [ "$min_bitmap_renders" -gt 0 ] ||
     [ "$max_bitmap_render_errors" -lt 999999 ] ||
     [ "$min_http_fetches" -gt 0 ] ||
     [ "$min_http_successes" -gt 0 ] ||
     [ "$min_type_rejects" -gt 0 ] ||
     [ "$min_css_misses" -gt 0 ] ||
     [ "$min_object_misses" -gt 0 ]; then
    rendered_status="$(grep 'status NS rendered' "$serial_log" | tail -n 1)"
    rendered_css="$(sed -n 's/.* css\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_css_total="$(sed -n 's/.* css[0-9][0-9]*\/\([0-9][0-9]*\) .*/\1/p' <<<"$rendered_status")"
    rendered_rects="$(sed -n 's/.* rect\([0-9][0-9]*\) .*/\1/p' <<<"$rendered_status")"
    rendered_lines="$(sed -n 's/.* line\([0-9][0-9]*\) .*/\1/p' <<<"$rendered_status")"
    rendered_text="$(sed -n 's/.* txt\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_paths="$(sed -n 's/.* path\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_polygons="$(sed -n 's/.* poly\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_discs="$(sed -n 's/.* disc\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_arcs="$(sed -n 's/.* arc\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_image_decodes="$(sed -n 's/.* img\([0-9][0-9]*\) fb[0-9][0-9]* ie[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_image_fallbacks="$(sed -n 's/.* img[0-9][0-9]* fb\([0-9][0-9]*\) ie[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_image_errors="$(sed -n 's/.* img[0-9][0-9]* fb[0-9][0-9]* ie\([0-9][0-9]*\) .*/\1/p' <<<"$rendered_status")"
    rendered_bitmap_render_successes="$(sed -n 's/.* br\([0-9][0-9]*\)\/[0-9][0-9]*\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_bitmap_renders="$(sed -n 's/.* br[0-9][0-9]*\/\([0-9][0-9]*\)\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_bitmap_render_errors="$(sed -n 's/.* br[0-9][0-9]*\/[0-9][0-9]*\/\([0-9][0-9]*\) .*/\1/p' <<<"$rendered_status")"
    rendered_http_successes="$(sed -n 's/.* http\([0-9][0-9]*\)\/[0-9][0-9]* fail[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_http_fetches="$(sed -n 's/.* http[0-9][0-9]*\/\([0-9][0-9]*\) fail[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_type_rejects="$(sed -n 's/.* tr\([0-9][0-9]*\) .*/\1/p' <<<"$rendered_status")"
    rendered_objects="$(sed -n 's/.* obj\([0-9][0-9]*\)\/[0-9][0-9]* http[0-9][0-9]*\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_object_total="$(sed -n 's/.* obj[0-9][0-9]*\/\([0-9][0-9]*\) http[0-9][0-9]*\/[0-9][0-9]* .*/\1/p' <<<"$rendered_status")"
    rendered_css="${rendered_css:-0}"
    rendered_css_total="${rendered_css_total:-0}"
    rendered_rects="${rendered_rects:-0}"
    rendered_lines="${rendered_lines:-0}"
    rendered_text="${rendered_text:-0}"
    rendered_paths="${rendered_paths:-0}"
    rendered_polygons="${rendered_polygons:-0}"
    rendered_discs="${rendered_discs:-0}"
    rendered_arcs="${rendered_arcs:-0}"
    rendered_image_decodes="${rendered_image_decodes:-0}"
    rendered_image_fallbacks="${rendered_image_fallbacks:-0}"
    rendered_image_errors="${rendered_image_errors:-0}"
    rendered_bitmap_render_successes="${rendered_bitmap_render_successes:-0}"
    rendered_bitmap_renders="${rendered_bitmap_renders:-0}"
    rendered_bitmap_render_errors="${rendered_bitmap_render_errors:-0}"
    rendered_http_successes="${rendered_http_successes:-0}"
    rendered_http_fetches="${rendered_http_fetches:-0}"
    rendered_type_rejects="${rendered_type_rejects:-0}"
    rendered_objects="${rendered_objects:-0}"
    rendered_object_total="${rendered_object_total:-0}"
    rendered_css_misses=$((rendered_css_total > rendered_css ? rendered_css_total - rendered_css : 0))
    rendered_object_misses=$((rendered_object_total > rendered_objects ? rendered_object_total - rendered_objects : 0))
    if [ "$rendered_css" -lt "$min_rendered_css" ] ||
       [ "$rendered_rects" -lt "$min_rendered_rects" ] ||
       [ "$rendered_lines" -lt "$min_rendered_lines" ] ||
       [ "$rendered_text" -lt "$min_rendered_text" ] ||
       [ "$rendered_paths" -lt "$min_rendered_paths" ] ||
       [ "$rendered_polygons" -lt "$min_rendered_polygons" ] ||
       [ "$rendered_discs" -lt "$min_rendered_discs" ] ||
       [ "$rendered_arcs" -lt "$min_rendered_arcs" ] ||
       [ "$rendered_image_decodes" -lt "$min_image_decodes" ] ||
       [ "$rendered_image_fallbacks" -lt "$min_image_fallbacks" ] ||
       [ "$rendered_image_errors" -lt "$min_image_errors" ] ||
       [ "$rendered_bitmap_render_successes" -lt "$min_bitmap_render_successes" ] ||
       [ "$rendered_bitmap_renders" -lt "$min_bitmap_renders" ] ||
       [ "$rendered_bitmap_render_errors" -gt "$max_bitmap_render_errors" ] ||
       [ "$rendered_http_fetches" -lt "$min_http_fetches" ] ||
       [ "$rendered_http_successes" -lt "$min_http_successes" ] ||
       [ "$rendered_type_rejects" -lt "$min_type_rejects" ] ||
       [ "$rendered_css_misses" -lt "$min_css_misses" ] ||
       [ "$rendered_object_misses" -lt "$min_object_misses" ]; then
      printf 'zbrowser launch repro: rendered layout counters too low: %s\n' "$rendered_status" >&2
      printf 'zbrowser launch repro: minimums css>=%s rect>=%s line>=%s text>=%s path>=%s poly>=%s disc>=%s arc>=%s img>=%s img-fallback>=%s img-error>=%s bitmap-render-ok>=%s bitmap-render>=%s bitmap-render-error<=%s http-fetch>=%s http-ok>=%s type-reject>=%s css-miss>=%s obj-miss>=%s\n' \
        "$min_rendered_css" "$min_rendered_rects" "$min_rendered_lines" "$min_rendered_text" \
        "$min_rendered_paths" "$min_rendered_polygons" "$min_rendered_discs" "$min_rendered_arcs" \
        "$min_image_decodes" "$min_image_fallbacks" "$min_image_errors" \
        "$min_bitmap_render_successes" "$min_bitmap_renders" "$max_bitmap_render_errors" \
        "$min_http_fetches" "$min_http_successes" "$min_type_rejects" \
        "$min_css_misses" "$min_object_misses" >&2
      tail -n 220 "$serial_log" || true
      exit 1
    fi
  fi
if [ -n "$require_http_status" ]; then
    load_status="$(grep 'zbrowser stage load-done status loaded' "$serial_log" | tail -n 1)"
    load_http_status="$(sed -n 's/.* http \([0-9][0-9]*\).*/\1/p' <<<"$load_status")"
    if [ "$load_http_status" != "$require_http_status" ]; then
      printf 'zbrowser launch repro: HTTP load status expected %s got %s\n' \
        "$require_http_status" "${load_http_status:-missing}" >&2
      tail -n 220 "$serial_log" || true
      exit 1
  fi
fi
max_status_pair_field() {
  local pattern="$1"
  local index="$2"
  local max_value=0
  local value=

  while IFS= read -r value; do
    value="${value:-0}"
    if [ "$value" -gt "$max_value" ]; then
      max_value="$value"
    fi
  done < <(sed -n "s/.* $pattern\\([0-9][0-9]*\\)\\/\\([0-9][0-9]*\\).*/\\${index}/p" "$serial_log")
  printf '%s\n' "$max_value"
}
max_status_quad_field() {
  local pattern="$1"
  local index="$2"
  local max_value=0
  local value=

  while IFS= read -r value; do
    value="${value:-0}"
    if [ "$value" -gt "$max_value" ]; then
      max_value="$value"
    fi
  done < <(sed -n "s/.* $pattern\\([0-9][0-9]*\\)\\/\\([0-9][0-9]*\\)\\/\\([0-9][0-9]*\\)\\/\\([0-9][0-9]*\\).*/\\${index}/p" "$serial_log")
  printf '%s\n' "$max_value"
}
if [ "$min_form_keys" -gt 0 ] ||
   [ "$min_form_mouse" -gt 0 ] ||
   [ "$min_form_nav_creates" -gt 0 ] ||
   [ "$min_form_nav_consumes" -gt 0 ]; then
  form_keys="$(max_status_quad_field "frm" 1)"
  form_mouse="$(max_status_quad_field "frm" 2)"
  form_nav_creates="$(max_status_quad_field "frm" 3)"
  form_nav_consumes="$(max_status_quad_field "frm" 4)"
  if [ "$form_keys" -lt "$min_form_keys" ] ||
     [ "$form_mouse" -lt "$min_form_mouse" ] ||
     [ "$form_nav_creates" -lt "$min_form_nav_creates" ] ||
     [ "$form_nav_consumes" -lt "$min_form_nav_consumes" ]; then
    printf 'zbrowser launch repro: form counters too low: frm key=%s mouse=%s nav=%s consume=%s\n' \
      "$form_keys" "$form_mouse" "$form_nav_creates" "$form_nav_consumes" >&2
    tail -n 220 "$serial_log" || true
    exit 1
  fi
fi
if [ "$min_js_execs" -gt 0 ] || [ "$min_js_successes" -gt 0 ]; then
  js_successes="$(max_status_pair_field "jse" 1)"
  js_execs="$(max_status_pair_field "jse" 2)"
  if [ "$js_execs" -lt "$min_js_execs" ] ||
     [ "$js_successes" -lt "$min_js_successes" ]; then
    printf 'zbrowser launch repro: javascript counters too low: jse %s/%s\n' \
      "$js_successes" "$js_execs" >&2
    tail -n 220 "$serial_log" || true
    exit 1
  fi
fi
if [ -n "$require_serial_fragment" ] &&
   ! grep -Fq "$require_serial_fragment" "$serial_log"; then
  printf 'zbrowser launch repro: missing serial fragment: %s\n' "$require_serial_fragment" >&2
  tail -n 220 "$serial_log" || true
  exit 1
fi
if [ -n "$require_pending_url_fragment" ] &&
   ! grep -F "zbrowser pending-navigation" "$serial_log" | grep -Fq "$require_pending_url_fragment"; then
  printf 'zbrowser launch repro: missing pending navigation fragment: %s\n' "$require_pending_url_fragment" >&2
  tail -n 220 "$serial_log" || true
  exit 1
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
