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
  ./OVMF_CODE.fd \
  /usr/share/OVMF/OVMF_CODE_4M.fd \
  /usr/share/edk2/x64/OVMF_CODE.4m.fd
do
  if [ -f "$code_path" ]; then
    ovmf_code="$code_path"
    break
  fi
done

for vars_path in \
  ./OVMF_VARS.fd \
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
qemu_log="build/zbrowser-selfhost.qemu.log"
seed_root="build/zbrowser-selfhost.seed"
data_seed_root="build/zbrowser-selfhost.data-seed"
browser_c_stage_root="build/browser-selfhost-c-stage"
restore_ramdisk_seed=0
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
poweroff_after_smoke="${ZBROWSER_SELFHOST_POWEROFF:-1}"

restore_default_ramdisk_seed() {
  if [ "$restore_ramdisk_seed" -eq 1 ] && [ -x build/tools/ramdisk_seed_gen ]; then
    make refresh-ramdisk >/dev/null || true
  fi
}
trap restore_default_ramdisk_seed EXIT

make build/tools/lainfs_check_host build/tools/lainfs_seed build/tools/ramdisk_seed_gen build/browser-c-engine/zbrowser_engine_module.zo reseed-data
ZBROWSER_SELFHOST_COMPACT_ASSETS=1 scripts/prepare-browser-selfhost-c-workspace.sh "$browser_c_stage_root" >/dev/null
rm -rf "$seed_root"
mkdir -p "$seed_root"
cat >"$seed_root/autoexec" <<'EOF'
mount S: hd1p1
mount S: hd0p1
mount S: sd0p1
blk
part
mounts
S:
mkdir mods
mkdir selfhost_project
cd selfhost_project
mkdir include
mkdir src
mkdir build
cd ..
mkdir kernel_z
cd kernel_z
mkdir install
cd ..
cd mods
cp R:/examples/kernel_api.Z kernel_api.Z
cp R:/examples/libc_api.Z libc_api.Z
cp R:/examples/mini_zlib.Z mini_zlib.Z
cp R:/examples/mini_zlib_api.Z mini_zlib_api.Z
cp R:/examples/browser_compat_stub.Z browser_compat_stub.Z
cp R:/examples/clib_port_smoke_module.Z clib_port_smoke_module.Z
cp R:/examples/clib_port_smoke_module.zbuild clib_port_smoke_module.zbuild
cp R:/examples/libc_smoke_module.Z libc_smoke_module.Z
cp R:/examples/libc_smoke_module.zbuild libc_smoke_module.zbuild
cp R:/examples/browser_selfhost_driver.Z browser_selfhost_driver.Z
cp R:/examples/browser_selfhost_driver.zbuild browser_selfhost_driver.zbuild
cp R:/examples/browser_selfhost_driver.zbuild bself.zbuild
cp R:/examples/zbrowser_module.Z zbrowser_module.Z
cp R:/examples/zbrowser_engine_module.zo zbrowser_engine_module.zo
cp R:/examples/zbrowser_netsurf.Z zbrowser_netsurf.Z
cp R:/examples/zbrowser_html.Z zbrowser_html.Z
cp R:/examples/zbrowser_html_api.Z zbrowser_html_api.Z
cp R:/examples/zbrowser_module.zbuild zbrowser_module.zbuild
cp R:/examples/zbrowser_netsurf.zbuild zbrowser_netsurf.zbuild
cp R:/examples/zbrowser_smoke.html zbrowser_smoke.html
cd ..
cd browser_c_probe
ztest kernel
zinstall kernel
exec browser_c_probe.bin
ztest queue
zinstall queue
exec browser_c_plan.bin
zcc ../browser_c/third_party/netsurf/src/libnsutils/src/base64.c src/selfhost_base64.zo
ztest first_unit
zcc ../browser_c/third_party/netsurf/src/libnsutils/src/time.c src/selfhost_time.zo
zcc ../browser_c/third_party/netsurf/src/libnsutils/src/unistd.c src/selfhost_unistd.zo
ztest tier1
zcc ../browser_c/third_party/netsurf/src/libparserutils/src/charset/encodings/utf8.c src/selfhost_utf8.zo
zcc ../browser_c/third_party/netsurf/src/libwapcaplet/src/libwapcaplet.c src/selfhost_lwc.zo
ztest tier2
cd ..
cd mods
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
zinstall clib_port_smoke_module
zinstall bself
exec browser_selfhost_driver.bin
cd ..
cd kernel_z
zinstall kernel_z_selfhost
cd ..
cd selfhost_project
ztest kernel
zinstall kernel
exec selfhost_project.bin
cd ..
cd browser_c_probe
zcc ../browser_c/third_party/netsurf/src/libhubbub/src/utils/errors.c src/selfhost_hubbub_errors.zo
zcc ../browser_c/third_party/netsurf/src/libhubbub/src/utils/string.c src/selfhost_hubbub_string.zo
zcc ../browser_c/third_party/netsurf/src/libhubbub/src/charset/detect.c src/selfhost_hubbub_detect.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/core/string.c src/selfhost_dom_string.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/utils/namespace.c src/selfhost_dom_namespace.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/core/implementation.c src/selfhost_dom_implementation.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/core/document.c src/selfhost_dom_document.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/core/nodelist.c src/selfhost_dom_nodelist.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/html/html_button_element.c src/selfhost_dom_html_button.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/html/html_input_element.c src/selfhost_dom_html_input.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/html/html_select_element.c src/selfhost_dom_html_select.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/html/html_script_element.c src/selfhost_dom_html_script.zo
zcc ../browser_c/third_party/netsurf/src/libdom/src/html/html_text_area_element.c src/selfhost_dom_html_textarea.zo
ztest tier3
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/bloom.c src/selfhost_bloom.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/url.c src/selfhost_url.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/utils.c src/selfhost_utils.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/useragent.c src/selfhost_useragent.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/desktop/mouse.c src/selfhost_mouse.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/nscolour.c src/selfhost_nscolour.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/utf8.c src/selfhost_netsurf_utf8.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/punycode.c src/selfhost_punycode.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/hashtable.c src/selfhost_hashtable.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/hashmap.c src/selfhost_hashmap.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/time.c src/selfhost_netsurf_time.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/primitives.c src/selfhost_http_primitives.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/generics.c src/selfhost_http_generics.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/parameter.c src/selfhost_http_parameter.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/content-type.c src/selfhost_http_content_type.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/content-disposition.c src/selfhost_http_cd.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/challenge.c src/selfhost_http_chal.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/www-authenticate.c src/selfhost_http_wa.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/cache-control.c src/selfhost_http_cc.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/http/strict-transport-security.c src/selfhost_http_sts.zo
zcc ../browser_c/third_party/netsurf/src/netsurf/utils/log.c src/selfhost_log.zo
ztest tier4
ztest http_chal
ztest http_wa
ztest http_cc
ztest http_sts
ztest log
EOF
if [ "$poweroff_after_smoke" = "1" ]; then
  printf 'poweroff\n' >>"$seed_root/autoexec"
else
  printf 'echo selfhost smoke complete\n' >>"$seed_root/autoexec"
fi
browser_c_seed_paths=(
  README.txt
  NEXT_C.txt
  COMPILE_UNITS.txt
  COMPILE_TIERS.txt
  COMPILE_TIER0.txt
  COMPILE_TIER1.txt
  COMPILE_TIER2.txt
  COMPILE_TIER3.txt
  COMPILE_TIER4.txt
  COMPILE_TIER5.txt
  kernel/include/freestanding/ctype.h
  kernel/include/freestanding/strings.h
  kernel/include/freestanding/sys/socket.h
  kernel/include/freestanding/unistd.h
  kernel/include/freestanding/sys/time.h
  kernel/include/freestanding/sys/utsname.h
  kernel/include/zlib.h
  third_party/netsurf/src/libnsutils/src/base64.c
  third_party/netsurf/src/libnsutils/src/time.c
  third_party/netsurf/src/libnsutils/src/unistd.c
  third_party/netsurf/src/libnsutils/include/nsutils/base64.h
  third_party/netsurf/src/libnsutils/include/nsutils/errors.h
  third_party/netsurf/src/libparserutils/src/charset/encodings/utf8.c
  third_party/netsurf/src/libparserutils/src/charset/encodings/utf8impl.h
  third_party/netsurf/src/libparserutils/include/parserutils/charset/utf8.h
  third_party/netsurf/src/libparserutils/include/parserutils/errors.h
  third_party/netsurf/src/libwapcaplet/src/libwapcaplet.c
  third_party/netsurf/src/libwapcaplet/include/libwapcaplet/libwapcaplet.h
  third_party/netsurf/src/libhubbub/src/utils/errors.c
  third_party/netsurf/src/libhubbub/src/utils/string.c
  third_party/netsurf/src/libhubbub/src/utils/string.h
  third_party/netsurf/src/libhubbub/src/charset/detect.c
  third_party/netsurf/src/libhubbub/src/charset/detect.h
  third_party/netsurf/src/libhubbub/include/hubbub/types.h
  third_party/netsurf/src/libhubbub/include/hubbub/errors.h
  third_party/netsurf/src/netsurf/utils/bloom.c
  third_party/netsurf/src/netsurf/utils/bloom.h
  third_party/netsurf/src/netsurf/utils/utils.h
  third_party/netsurf/src/netsurf/utils/corestrings.c
  third_party/netsurf/src/netsurf/utils/file.c
  third_party/netsurf/src/netsurf/utils/hashmap.c
  third_party/netsurf/src/netsurf/utils/hashtable.c
  third_party/netsurf/src/netsurf/utils/idna.c
  third_party/netsurf/src/netsurf/utils/idna_props.h
  third_party/netsurf/src/netsurf/utils/libdom.c
  third_party/netsurf/src/netsurf/utils/log.c
  third_party/netsurf/src/netsurf/utils/log.h
  third_party/netsurf/src/netsurf/utils/nscolour.c
  third_party/netsurf/src/netsurf/utils/nsoption.c
  third_party/netsurf/src/netsurf/utils/punycode.c
  third_party/netsurf/src/netsurf/utils/punycode.h
  third_party/netsurf/src/netsurf/utils/talloc.c
  third_party/netsurf/src/netsurf/utils/time.c
  third_party/netsurf/src/netsurf/utils/url.c
  third_party/netsurf/src/netsurf/utils/useragent.c
  third_party/netsurf/src/netsurf/utils/utf8.c
  third_party/netsurf/src/netsurf/utils/utils.c
  third_party/netsurf/src/netsurf/utils/utsname.h
  third_party/netsurf/src/netsurf/utils/http.h
  third_party/netsurf/src/netsurf/utils/http/cache-control.c
  third_party/netsurf/src/netsurf/utils/http/cache-control.h
  third_party/netsurf/src/netsurf/utils/http/challenge.c
  third_party/netsurf/src/netsurf/utils/http/challenge.h
  third_party/netsurf/src/netsurf/utils/http/challenge_internal.h
  third_party/netsurf/src/netsurf/utils/http/content-disposition.c
  third_party/netsurf/src/netsurf/utils/http/content-disposition.h
  third_party/netsurf/src/netsurf/utils/http/content-type.h
  third_party/netsurf/src/netsurf/utils/http/content-type.c
  third_party/netsurf/src/netsurf/utils/http/generics.c
  third_party/netsurf/src/netsurf/utils/http/parameter.h
  third_party/netsurf/src/netsurf/utils/http/parameter.c
  third_party/netsurf/src/netsurf/utils/http/primitives.c
  third_party/netsurf/src/netsurf/utils/http/primitives.h
  third_party/netsurf/src/netsurf/utils/http/response-codes.h
  third_party/netsurf/src/netsurf/utils/http/strict-transport-security.c
  third_party/netsurf/src/netsurf/utils/http/strict-transport-security.h
  third_party/netsurf/src/netsurf/utils/http/www-authenticate.c
  third_party/netsurf/src/netsurf/utils/http/www-authenticate.h
  third_party/netsurf/src/netsurf/utils/nsurl/nsurl.c
  third_party/netsurf/src/netsurf/utils/nsurl/parse.c
  third_party/netsurf/src/netsurf/utils/nsurl/private.h
  third_party/netsurf/src/netsurf/desktop/browser_private.h
  third_party/netsurf/src/netsurf/desktop/frame_types.h
  third_party/netsurf/src/netsurf/desktop/mouse.c
  third_party/netsurf/src/netsurf/desktop/plot_style.c
  third_party/netsurf/src/netsurf/desktop/search.c
  third_party/netsurf/src/netsurf/desktop/search.h
  third_party/netsurf/src/netsurf/desktop/scrollbar.c
  third_party/netsurf/src/netsurf/desktop/scrollbar.h
  third_party/netsurf/src/netsurf/desktop/system_colour.c
  third_party/netsurf/src/netsurf/desktop/version.c
  third_party/netsurf/src/netsurf/desktop/version.h
  third_party/netsurf/src/netsurf/content/fetch.h
  third_party/netsurf/src/netsurf/content/textsearch.h
  third_party/netsurf/src/netsurf/include/netsurf/browser_window.h
  third_party/netsurf/src/netsurf/include/netsurf/console.h
  third_party/netsurf/src/netsurf/include/netsurf/css.h
  third_party/netsurf/src/netsurf/include/netsurf/mouse.h
  third_party/netsurf/src/netsurf/include/netsurf/plotters.h
  third_party/netsurf/src/netsurf/include/netsurf/ssl_certs.h
  third_party/netsurf/src/netsurf/include/netsurf/utf8.h
  third_party/netsurf/src/netsurf/test/testament.h
  third_party/netsurf/src/libdom/include/dom/dom.h
  third_party/netsurf/src/libdom/include/dom/core/exceptions.h
  third_party/netsurf/src/libdom/include/dom/core/string.h
  third_party/netsurf/src/libdom/include/dom/bindings/hubbub/errors.h
  third_party/netsurf/src/libdom/include/dom/bindings/hubbub/parser.h
  third_party/netsurf/src/libdom/src/core/string.c
  third_party/netsurf/src/libdom/src/core/string.h
  third_party/netsurf/src/libdom/src/core/implementation.c
  third_party/netsurf/src/libdom/include/dom/core/implementation.h
  third_party/netsurf/src/libdom/src/core/document.c
  third_party/netsurf/src/libdom/src/core/document.h
  third_party/netsurf/src/libdom/include/dom/core/document.h
  third_party/netsurf/src/libdom/src/core/nodelist.c
  third_party/netsurf/src/libdom/src/core/nodelist.h
  third_party/netsurf/src/libdom/include/dom/core/nodelist.h
  third_party/netsurf/src/libdom/src/core/node.h
  third_party/netsurf/src/libdom/include/dom/core/node.h
  third_party/netsurf/src/libdom/src/html/html_button_element.c
  third_party/netsurf/src/libdom/src/html/html_button_element.h
  third_party/netsurf/src/libdom/src/html/html_document.h
  third_party/netsurf/src/libdom/include/dom/html/html_button_element.h
  third_party/netsurf/src/libdom/src/html/html_input_element.c
  third_party/netsurf/src/libdom/src/html/html_input_element.h
  third_party/netsurf/src/libdom/include/dom/html/html_input_element.h
  third_party/netsurf/src/libdom/src/html/html_select_element.c
  third_party/netsurf/src/libdom/src/html/html_select_element.h
  third_party/netsurf/src/libdom/include/dom/html/html_select_element.h
  third_party/netsurf/src/libdom/include/dom/html/html_options_collection.h
  third_party/netsurf/src/libdom/include/dom/html/html_option_element.h
  third_party/netsurf/src/libdom/src/html/html_script_element.c
  third_party/netsurf/src/libdom/src/html/html_script_element.h
  third_party/netsurf/src/libdom/include/dom/html/html_script_element.h
  third_party/netsurf/src/libdom/src/html/html_text_area_element.c
  third_party/netsurf/src/libdom/src/html/html_text_area_element.h
  third_party/netsurf/src/libdom/include/dom/html/html_text_area_element.h
  third_party/netsurf/src/libdom/include/dom/html/html_form_element.h
  third_party/netsurf/src/libdom/src/utils/namespace.c
  third_party/netsurf/src/libdom/src/utils/namespace.h
  third_party/netsurf/src/libdom/src/utils/validate.c
  third_party/netsurf/src/libdom/src/utils/validate.h
  third_party/netsurf/src/libdom/src/utils/character_valid.h
  third_party/netsurf/src/libhubbub/include/hubbub/errors.h
  third_party/netsurf/src/libcss/include/libcss/libcss.h
  third_party/netsurf/src/libparserutils/include/parserutils/charset/utf8.h
)
ramdisk_seed_args=(
  examples/kernel_api.Z=examples/kernel_api.Z
  examples/libc_api.Z=examples/libc_api.Z
  examples/mini_zlib.Z=examples/mini_zlib.Z
  examples/mini_zlib_api.Z=examples/mini_zlib_api.Z
  examples/browser_compat_stub.Z=examples/browser_compat_stub.Z
  examples/clib_port_smoke_module.Z=examples/clib_port_smoke_module.Z
  examples/clib_port_smoke_module.zbuild=examples/clib_port_smoke_module.zbuild
  examples/libc_smoke_module.Z=examples/libc_smoke_module.Z
  examples/libc_smoke_module.zbuild=examples/libc_smoke_module.zbuild
  examples/browser_selfhost_driver.Z=examples/browser_selfhost_driver.Z
  examples/browser_selfhost_driver.zbuild=examples/browser_selfhost_driver.zbuild
  examples/zbrowser_module.Z=examples/zbrowser_module.Z
  examples/zbrowser_netsurf.Z=examples/zbrowser_netsurf.Z
  examples/zbrowser_html.Z=examples/zbrowser_html.Z
  examples/zbrowser_html_api.Z=examples/zbrowser_html_api.Z
  examples/zbrowser_module.zbuild=examples/zbrowser_module.zbuild
  examples/zbrowser_netsurf.zbuild=examples/zbrowser_netsurf.zbuild
  examples/zbrowser_smoke.html=examples/zbrowser_smoke.html
  examples/zlang/selfhost_project/kernel.zbuild=examples/zlang/selfhost_project/kernel.zbuild
  examples/zlang/selfhost_project/include/kernel_api.Z=examples/zlang/selfhost_project/include/kernel_api.Z
  examples/zlang/selfhost_project/include/selfhost_once_leaf.Z=examples/zlang/selfhost_project/include/selfhost_once_leaf.Z
  examples/zlang/selfhost_project/include/selfhost_once_middle.Z=examples/zlang/selfhost_project/include/selfhost_once_middle.Z
  examples/zlang/selfhost_project/include/selfhost_once_root.Z=examples/zlang/selfhost_project/include/selfhost_once_root.Z
  examples/zlang/selfhost_project/src/selfhost_demo.Z=examples/zlang/selfhost_project/src/selfhost_demo.Z
)
ramdisk_seed_args+=("build/browser-c-engine/zbrowser_engine_module.zo=examples/zbrowser_engine_module.zo")
for seed_path in "${browser_c_seed_paths[@]}"; do
  ramdisk_seed_args+=("$browser_c_stage_root/browser_c/$seed_path=browser_c/$seed_path")
done
while IFS= read -r seed_path; do
  ramdisk_seed_args+=("$seed_path=${seed_path#"$browser_c_stage_root"/}")
done < <(find "$browser_c_stage_root/browser_c_probe" -type f | sort)
ramdisk_seed_args+=(
  kernel/z/kernel_z_selfhost.zbuild
  kernel/z/clock_math.Z
  kernel/z/status_math.Z
  kernel/z/zlink_probe.Z
  "$seed_root/autoexec=examples/autoexec"
)
build/tools/ramdisk_seed_gen build/ramdisk_seed.h "${ramdisk_seed_args[@]}" >/dev/null
restore_ramdisk_seed=1
make build/boot.iso
cp build/data.img "$smoke_img"
cp "$ovmf_vars" "$smoke_vars"
rm -rf "$data_seed_root"
mkdir -p "$data_seed_root"
for data_path in "${browser_c_seed_paths[@]}"; do
  seed_src="$browser_c_stage_root/browser_c/$data_path"
  seed_dst="$data_seed_root/browser_c/$data_path"
  mkdir -p "$(dirname "$seed_dst")"
  cp "$seed_src" "$seed_dst"
done
mkdir -p "$data_seed_root/browser_c_probe"
cp -R "$browser_c_stage_root/browser_c_probe/." "$data_seed_root/browser_c_probe/"
build/tools/lainfs_seed "$smoke_img" Makefile README.md SELFHOSTING.md "$data_seed_root/browser_c" "$data_seed_root/browser_c_probe" >/dev/null
: >"$serial_log"
: >"$qemu_log"

printf 'zbrowser self-host smoke: runtime=%s smp=%s mem=%s timeout=%ss\n' \
  "$qemu_runtime" "$qemu_smp" "$qemu_memory" "$timeout_seconds"
if [ "$qemu_runtime" != "kvm" ]; then
  printf 'zbrowser self-host smoke: running without KVM because %s\n' "$kvm_reason"
fi

qemu_start_epoch="$(date +%s)"
set +e
run_qemu_smoke() {
  timeout "${timeout_seconds}s" qemu-system-x86_64 \
  "${qemu_accel_args[@]}" \
  -smp "$qemu_smp" \
  -m "$qemu_memory" \
  -boot order=d,menu=on \
  -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
  -drive if=pflash,format=raw,file="$smoke_vars" \
  -cdrom build/boot.iso \
  -device ich9-ahci,id=ahci \
  -drive if=none,id=data,format=raw,file="$smoke_img" \
	  -device ide-hd,drive=data,bus=ahci.0 \
	  -display none \
	  -serial "file:$serial_log" \
	  -monitor none \
	  >>"$qemu_log" 2>&1
}

run_qemu_smoke
qemu_status=$?
if [ "$qemu_status" -ne 0 ] && [ "$qemu_runtime" = "kvm" ] &&
   grep -Eq 'failed to initialize kvm|KVM_CREATE_VM' "$qemu_log" 2>/dev/null; then
  printf 'zbrowser self-host smoke: KVM failed to initialize; retrying with TCG\n'
  qemu_runtime="tcg"
  qemu_accel_args=(-accel tcg,thread=multi -cpu max)
  qemu_memory="${ZBROWSER_SELFHOST_QEMU_MEMORY:-256M}"
  qemu_smp="${ZBROWSER_SELFHOST_QEMU_SMP:-1}"
  timeout_seconds="${ZBROWSER_SELFHOST_TIMEOUT_SECONDS:-420}"
  : >"$serial_log"
  : >"$qemu_log"
  run_qemu_smoke
  qemu_status=$?
fi
qemu_end_epoch="$(date +%s)"
set -e
qemu_elapsed="$((qemu_end_epoch - qemu_start_epoch))"

if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
  printf 'zbrowser self-host smoke: QEMU exited with status %s after %ss\n' "$qemu_status" "$qemu_elapsed" >&2
  tail -n 80 "$qemu_log" >&2 || true
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

check_serial_success() {
  local expected_units

  if ! grep -q 'created live ramdisk rd0p1 at R:' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected live R: ramdisk creation\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi
  if ! grep -q '  R: rd0p1 lainfs' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected R: mount in guest\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi
  if ! grep -q 'browser selfhost: ok' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected browser selfhost driver success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi
  if grep -q 'unsupported .Z syntax' "$serial_log"; then
    printf 'zbrowser self-host smoke: guest compiler reported unsupported .Z syntax\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi
  if ! grep -q 'browser_c probe: ok bytes=' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected browser C staged-source probe success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  expected_units="$(grep -c '^[^|][^|]*|' scripts/browser-selfhost-c-units.txt)"
  if ! grep -q "browser_c plan: ok units=${expected_units}" "$serial_log"; then
    printf 'zbrowser self-host smoke: expected browser C plan unit count output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c first unit: ok base64' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected first browser C guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c tier1: ok base64 time unistd' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected tier1 browser C guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c tier2: ok base64 time unistd utf8 lwc' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected tier2 browser C guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c tier3: ok hubbub detect dom_string namespace implementation document nodelist html_button html_input html_select html_script html_textarea' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected tier3 browser C guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c tier4: ok bloom url utils useragent mouse nscolour utf8 punycode hashtable hashmap time http generics parameter content_type content_disposition challenge www_authenticate cache_control strict_transport_security log' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected tier4 browser C guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c http challenge: ok' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected HTTP challenge guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c http wa: ok' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected HTTP WWW-Authenticate guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c http cc: ok' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected HTTP Cache-Control guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c http sts: ok' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected HTTP Strict-Transport-Security guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if ! grep -q 'browser_c log: ok' "$serial_log"; then
    printf 'zbrowser self-host smoke: expected NetSurf log guest-compiled unit success output\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi

  if [ "$(grep -c 'selfhost project 42' "$serial_log" || true)" -lt 2 ]; then
    printf 'zbrowser self-host smoke: expected selfhost project to print twice (ztest + exec)\n' >&2
    tail -n 120 "$serial_log" >&2 || true
    exit 1
  fi
}

if ! grep -q '  S: .* lainfs' "$serial_log"; then
  check_serial_success
  printf 'zbrowser self-host smoke: ok live-ramdisk path (timeout=%ss elapsed=%ss)\n' "$timeout_seconds" "$qemu_elapsed"
  exit 0
fi

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
check_file "browser_c/README.txt"
check_file "browser_c/NEXT_C.txt"
check_file "browser_c/COMPILE_UNITS.txt"
check_file "browser_c/COMPILE_TIERS.txt"
check_file "browser_c/COMPILE_TIER0.txt"
check_file "browser_c/COMPILE_TIER1.txt"
check_file "browser_c/COMPILE_TIER2.txt"
check_file "browser_c/COMPILE_TIER3.txt"
check_file "browser_c/COMPILE_TIER4.txt"
check_file "browser_c/COMPILE_TIER5.txt"
check_file "browser_c/kernel/include/freestanding/ctype.h"
check_file "browser_c/kernel/include/freestanding/strings.h"
check_file "browser_c/kernel/include/freestanding/sys/socket.h"
check_file "browser_c/kernel/include/freestanding/unistd.h"
check_file "browser_c/kernel/include/freestanding/sys/time.h"
check_file "browser_c/kernel/include/freestanding/sys/utsname.h"
check_file "browser_c/kernel/include/zlib.h"
check_file "browser_c/third_party/netsurf/src/libnsutils/src/base64.c"
check_file "browser_c/third_party/netsurf/src/libnsutils/src/time.c"
check_file "browser_c/third_party/netsurf/src/libnsutils/src/unistd.c"
check_file "browser_c/third_party/netsurf/src/libparserutils/src/charset/encodings/utf8.c"
check_file "browser_c/third_party/netsurf/src/libparserutils/src/charset/encodings/utf8impl.h"
check_file "browser_c/third_party/netsurf/src/libparserutils/include/parserutils/charset/utf8.h"
check_file "browser_c/third_party/netsurf/src/libparserutils/include/parserutils/errors.h"
check_file "browser_c/third_party/netsurf/src/libwapcaplet/src/libwapcaplet.c"
check_file "browser_c/third_party/netsurf/src/libwapcaplet/include/libwapcaplet/libwapcaplet.h"
check_file "browser_c/third_party/netsurf/src/libhubbub/src/utils/errors.c"
check_file "browser_c/third_party/netsurf/src/libhubbub/src/utils/string.c"
check_file "browser_c/third_party/netsurf/src/libhubbub/src/utils/string.h"
check_file "browser_c/third_party/netsurf/src/libhubbub/src/charset/detect.c"
check_file "browser_c/third_party/netsurf/src/libhubbub/src/charset/detect.h"
check_file "browser_c/third_party/netsurf/src/libhubbub/include/hubbub/types.h"
check_file "browser_c/third_party/netsurf/src/libhubbub/include/hubbub/errors.h"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/bloom.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/hashmap.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/hashtable.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/talloc.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/time.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/primitives.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/generics.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/parameter.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/content-type.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/content-disposition.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/challenge.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/www-authenticate.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/cache-control.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/http/strict-transport-security.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/log.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/log.h"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/url.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/useragent.c"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/utsname.h"
check_file "browser_c/third_party/netsurf/src/netsurf/desktop/plot_style.c"
check_file "browser_c/third_party/netsurf/src/netsurf/desktop/mouse.c"
check_file "browser_c/third_party/netsurf/src/netsurf/desktop/system_colour.c"
check_file "browser_c/third_party/netsurf/src/netsurf/desktop/version.h"
check_file "browser_c/third_party/netsurf/src/netsurf/include/netsurf/css.h"
check_file "browser_c/third_party/netsurf/src/netsurf/utils/utils.c"
check_file "browser_c/third_party/netsurf/src/netsurf/include/netsurf/ssl_certs.h"
check_file "browser_c_probe/browser_c_probe.status"
check_file "browser_c_probe/browser_c_probe.bin"
check_file "browser_c_probe/kernel.buildlog"
check_file "browser_c_probe/kernel.testlog"
check_file "browser_c_probe/browser_c_plan.bin"
check_file "browser_c_probe/browser_c_plan.status"
check_file "browser_c_probe/browser_c_plan.first"
check_file "browser_c_probe/queue.buildlog"
check_file "browser_c_probe/queue.testlog"
check_file "browser_c_probe/src/selfhost_base64.zo"
check_file "browser_c_probe/src/selfhost_time.zo"
check_file "browser_c_probe/src/selfhost_unistd.zo"
check_file "browser_c_probe/src/selfhost_utf8.zo"
check_file "browser_c_probe/src/selfhost_lwc.zo"
check_file "browser_c_probe/src/selfhost_hubbub_errors.zo"
check_file "browser_c_probe/src/selfhost_hubbub_string.zo"
check_file "browser_c_probe/src/selfhost_hubbub_detect.zo"
check_file "browser_c_probe/src/selfhost_dom_string.zo"
check_file "browser_c_probe/src/selfhost_dom_namespace.zo"
check_file "browser_c_probe/src/selfhost_dom_implementation.zo"
check_file "browser_c_probe/src/selfhost_dom_document.zo"
check_file "browser_c_probe/src/selfhost_dom_nodelist.zo"
check_file "browser_c_probe/src/selfhost_dom_html_button.zo"
check_file "browser_c_probe/src/selfhost_dom_html_input.zo"
check_file "browser_c_probe/src/selfhost_dom_html_select.zo"
check_file "browser_c_probe/src/selfhost_dom_html_script.zo"
check_file "browser_c_probe/src/selfhost_dom_html_textarea.zo"
check_file "browser_c_probe/src/selfhost_bloom.zo"
check_file "browser_c_probe/src/selfhost_url.zo"
check_file "browser_c_probe/src/selfhost_utils.zo"
check_file "browser_c_probe/src/selfhost_useragent.zo"
check_file "browser_c_probe/src/selfhost_mouse.zo"
check_file "browser_c_probe/src/selfhost_nscolour.zo"
check_file "browser_c_probe/src/selfhost_netsurf_utf8.zo"
check_file "browser_c_probe/src/selfhost_punycode.zo"
check_file "browser_c_probe/src/selfhost_hashtable.zo"
check_file "browser_c_probe/src/selfhost_hashmap.zo"
check_file "browser_c_probe/src/selfhost_netsurf_time.zo"
check_file "browser_c_probe/src/selfhost_http_primitives.zo"
check_file "browser_c_probe/src/selfhost_http_generics.zo"
check_file "browser_c_probe/src/selfhost_http_parameter.zo"
check_file "browser_c_probe/src/selfhost_http_content_type.zo"
check_file "browser_c_probe/src/selfhost_http_cd.zo"
check_file "browser_c_probe/src/selfhost_http_chal.zo"
check_file "browser_c_probe/src/selfhost_http_wa.zo"
check_file "browser_c_probe/src/selfhost_http_cc.zo"
check_file "browser_c_probe/src/selfhost_http_sts.zo"
check_file "browser_c_probe/src/selfhost_log.zo"
check_file "browser_c_probe/browser_c_first_unit.bin"
check_file "browser_c_probe/first_unit.buildlog"
check_file "browser_c_probe/first_unit.testlog"
check_file "browser_c_probe/browser_c_tier1.bin"
check_file "browser_c_probe/tier1.buildlog"
check_file "browser_c_probe/tier1.testlog"
check_file "browser_c_probe/browser_c_tier2.bin"
check_file "browser_c_probe/tier2.buildlog"
check_file "browser_c_probe/tier2.testlog"
check_file "browser_c_probe/browser_c_tier3.bin"
check_file "browser_c_probe/tier3.buildlog"
check_file "browser_c_probe/tier3.testlog"
check_file "browser_c_probe/browser_c_tier4.bin"
check_file "browser_c_probe/tier4.buildlog"
check_file "browser_c_probe/tier4.testlog"
check_file "browser_c_probe/http_chal.bin"
check_file "browser_c_probe/http_chal.buildlog"
check_file "browser_c_probe/http_chal.testlog"
check_file "browser_c_probe/http_wa.bin"
check_file "browser_c_probe/http_wa.buildlog"
check_file "browser_c_probe/http_wa.testlog"
check_file "browser_c_probe/http_cc.bin"
check_file "browser_c_probe/http_cc.buildlog"
check_file "browser_c_probe/http_cc.testlog"
check_file "browser_c_probe/http_sts.bin"
check_file "browser_c_probe/http_sts.buildlog"
check_file "browser_c_probe/http_sts.testlog"
check_file "browser_c_probe/log.bin"
check_file "browser_c_probe/log.buildlog"
check_file "browser_c_probe/log.testlog"

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
check_buildlog "mods/clib_port_smoke_module.buildlog" 3 "clib port smoke module"
check_buildlog "mods/zbrowser_module.buildlog" 3 "zbrowser module"
check_buildlog "mods/zbrowser_netsurf.buildlog" 2 "NetSurf module"
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
expected_browser_c_first="$(grep -m 1 '^[^|][^|]*|' scripts/browser-selfhost-c-units.txt)"
check_log_contains "browser_c_probe/browser_c_plan.first" "^${expected_browser_c_first}$" "browser C first unit"
check_log_contains "browser_c_probe/browser_c_plan.first" 'kernel/include/freestanding' "browser C first include roots"
check_log_contains "browser_c_probe/first_unit.buildlog" '^objects 2$' "browser C first unit build log"
check_log_contains "browser_c_probe/first_unit.buildlog" '^status ok$' "browser C first unit build log"
check_log_contains "browser_c_probe/first_unit.testlog" '^result 0$' "browser C first unit test log"
check_log_contains "browser_c_probe/first_unit.testlog" '^status ok$' "browser C first unit test log"
check_log_contains "browser_c_probe/tier1.buildlog" '^objects 4$' "browser C tier1 build log"
check_log_contains "browser_c_probe/tier1.buildlog" '^status ok$' "browser C tier1 build log"
check_log_contains "browser_c_probe/tier1.testlog" '^result 0$' "browser C tier1 test log"
check_log_contains "browser_c_probe/tier1.testlog" '^status ok$' "browser C tier1 test log"
check_log_contains "browser_c_probe/tier2.buildlog" '^objects 6$' "browser C tier2 build log"
check_log_contains "browser_c_probe/tier2.buildlog" '^status ok$' "browser C tier2 build log"
check_log_contains "browser_c_probe/tier2.testlog" '^result 0$' "browser C tier2 test log"
check_log_contains "browser_c_probe/tier2.testlog" '^status ok$' "browser C tier2 test log"
check_log_contains "browser_c_probe/tier3.buildlog" '^objects 16$' "browser C tier3 build log"
check_log_contains "browser_c_probe/tier3.buildlog" '^status ok$' "browser C tier3 build log"
check_log_contains "browser_c_probe/tier3.testlog" '^result 0$' "browser C tier3 test log"
check_log_contains "browser_c_probe/tier3.testlog" '^status ok$' "browser C tier3 test log"
check_log_contains "browser_c_probe/tier4.buildlog" '^objects 24$' "browser C tier4 build log"
check_log_contains "browser_c_probe/tier4.buildlog" '^status ok$' "browser C tier4 build log"
check_log_contains "browser_c_probe/tier4.testlog" '^result 0$' "browser C tier4 test log"
check_log_contains "browser_c_probe/tier4.testlog" '^status ok$' "browser C tier4 test log"
check_log_contains "browser_c_probe/http_chal.buildlog" '^objects 6$' "browser C HTTP challenge build log"
check_log_contains "browser_c_probe/http_chal.buildlog" '^status ok$' "browser C HTTP challenge build log"
check_log_contains "browser_c_probe/http_chal.testlog" '^result 0$' "browser C HTTP challenge test log"
check_log_contains "browser_c_probe/http_chal.testlog" '^status ok$' "browser C HTTP challenge test log"
check_log_contains "browser_c_probe/http_wa.buildlog" '^objects 7$' "browser C HTTP WWW-Authenticate build log"
check_log_contains "browser_c_probe/http_wa.buildlog" '^status ok$' "browser C HTTP WWW-Authenticate build log"
check_log_contains "browser_c_probe/http_wa.testlog" '^result 0$' "browser C HTTP WWW-Authenticate test log"
check_log_contains "browser_c_probe/http_wa.testlog" '^status ok$' "browser C HTTP WWW-Authenticate test log"
check_log_contains "browser_c_probe/http_cc.buildlog" '^objects 5$' "browser C HTTP Cache-Control build log"
check_log_contains "browser_c_probe/http_cc.buildlog" '^status ok$' "browser C HTTP Cache-Control build log"
check_log_contains "browser_c_probe/http_cc.testlog" '^result 0$' "browser C HTTP Cache-Control test log"
check_log_contains "browser_c_probe/http_cc.testlog" '^status ok$' "browser C HTTP Cache-Control test log"
check_log_contains "browser_c_probe/http_sts.buildlog" '^objects 5$' "browser C HTTP Strict-Transport-Security build log"
check_log_contains "browser_c_probe/http_sts.buildlog" '^status ok$' "browser C HTTP Strict-Transport-Security build log"
check_log_contains "browser_c_probe/http_sts.testlog" '^result 0$' "browser C HTTP Strict-Transport-Security test log"
check_log_contains "browser_c_probe/http_sts.testlog" '^status ok$' "browser C HTTP Strict-Transport-Security test log"
check_log_contains "browser_c_probe/log.buildlog" '^objects 2$' "browser C NetSurf log build log"
check_log_contains "browser_c_probe/log.buildlog" '^status ok$' "browser C NetSurf log build log"
check_log_contains "browser_c_probe/log.testlog" '^result 0$' "browser C NetSurf log test log"
check_log_contains "browser_c_probe/log.testlog" '^status ok$' "browser C NetSurf log test log"
check_log_contains "selfhost_project/build/kernel.buildlog" '^status ok$' "selfhost build log"
check_log_contains "selfhost_project/build/kernel.buildlog" '^objects 1$' "selfhost build log"
check_log_contains "selfhost_project/build/kernel.testlog" '^result 42$' "selfhost test log"
check_log_contains "selfhost_project/build/kernel.testlog" '^status ok$' "selfhost test log"
check_log_contains "selfhost_project/build/from_z_renamed.txt" 'created from selfhost_project' "selfhost output file"

check_serial_success

printf 'zbrowser self-host smoke: ok (timeout=%ss elapsed=%ss)\n' "$timeout_seconds" "$qemu_elapsed"
