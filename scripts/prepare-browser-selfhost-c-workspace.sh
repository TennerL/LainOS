#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

manifest_path="scripts/browser-selfhost-c-files.txt"
units_manifest_path="scripts/browser-selfhost-c-units.txt"
stage_root="${1:-build/browser-selfhost-c-stage}"
workspace_root="$stage_root/browser_c"
probe_root="$stage_root/browser_c_probe"
notes_path="$workspace_root/README.txt"
commands_path="$workspace_root/NEXT_C.txt"
filelist_path="$workspace_root/FILES.txt"
compile_units_path="$workspace_root/COMPILE_UNITS.txt"
tier_status_path="$workspace_root/COMPILE_TIERS.txt"

if [[ ! -f "$manifest_path" ]]; then
  printf 'prepare-browser-selfhost-c-workspace: missing manifest %s\n' "$manifest_path" >&2
  exit 1
fi
if [[ ! -f "$units_manifest_path" ]]; then
  printf 'prepare-browser-selfhost-c-workspace: missing units manifest %s\n' "$units_manifest_path" >&2
  exit 1
fi

if grep -qx 'build/netsurf_resource_css.h' "$manifest_path"; then
  make build/netsurf_resource_css.h >/dev/null
fi
if grep -qx 'build/dejavu_sans_ttf.h' "$manifest_path"; then
  make build/dejavu_sans_ttf.h >/dev/null
fi

rm -rf "$workspace_root" "$probe_root"
mkdir -p "$workspace_root"

cat >"$notes_path" <<'EOF'
browser_c is the first staged in-OS C selfhost slice for browser work.

It is intentionally small:
- libnsutils base64/time/unistd
- libdom public headers plus hubbub binding install headers and the
  matching libhubbub error header needed by staged browser-side DOM helpers
- libcss public include surface needed by staged browser-side colour headers
- netsurf desktop bitmap/mouse/plot_style/search/searchweb/scrollbar/system_colour
  /version plus the generic browser/content/css-facing headers those sources
  consume, including the checked-in testament placeholder that desktop/version.c
  includes
- netsurf utils bloom/corestrings/libdom/hashmap/hashtable/http/{cache-control,challenge,content-disposition,content-type,generics,parameter,primitives,strict-transport-security,www-authenticate}/punycode/file/filepath/idna/log/messages/nscolour/nsoption/nsurl/{nsurl,parse}/ssl_certs/talloc/time/url/useragent/utf8/utils

The goal is to remove the guest-side source staging blocker before the in-OS
C compiler lands. The tree preserves upstream-relative paths so future compile
commands can reuse the same include roots as the host build without depending
on host system headers.

COMPILE_UNITS.txt is the machine-readable first-pass browser-C build plan.
Each line is object|source|include-roots with an optional fourth field for
semicolon-delimited extra compiler flags when a real upstream unit needs them.
It is validated on the host by scripts/zbrowser-c-host-compile-smoke.sh with
-nostdinc plus repo-staged freestanding headers, and is intended to become the
first in-OS browser-C compile queue.

COMPILE_TIER0.txt through COMPILE_TIER5.txt split that same queue into the
self-hosting gradient. COMPILE_TIERS.txt records the current state of each
tier. For now every tier is host-built and guest-linked; no tier is marked
guest-compiled until the in-OS C compiler emits its first browser object.

browser_c_probe/ is a tiny Z project that runs from the same staged workspace
and validates that the in-OS toolchain can consume that compile queue before a
guest C compiler exists.

hashtable.c intentionally uses the generic kernel zlib ABI header so the queue
exercises one concrete browser-C dependency without introducing browser-specific
kernel logic.
EOF

cp "$manifest_path" "$filelist_path"
cp "$units_manifest_path" "$compile_units_path"

classify_compile_tier() {
  local rel_source="$1"

  case "$rel_source" in
    examples/browser_c_probe/src/*)
      printf '0'
      ;;
    third_party/netsurf/src/libnsutils/*)
      printf '1'
      ;;
    third_party/netsurf/src/libwapcaplet/*|third_party/netsurf/src/libparserutils/*)
      printf '2'
      ;;
    third_party/netsurf/src/libcss/*|third_party/netsurf/src/libdom/*|third_party/netsurf/src/libhubbub/*)
      printf '3'
      ;;
    third_party/netsurf/src/netsurf/content/handlers/html/*|third_party/netsurf/src/netsurf/content/handlers/css/*)
      printf '5'
      ;;
    third_party/netsurf/src/netsurf/*)
      printf '4'
      ;;
    *)
      printf '4'
      ;;
  esac
}

for tier_index in 0 1 2 3 4 5; do
  : >"$workspace_root/COMPILE_TIER${tier_index}.txt"
done

while IFS='|' read -r object_name rel_source include_roots extra_flags; do
  [[ -n "$object_name" ]] || continue
  tier_index="$(classify_compile_tier "$rel_source")"
  if [[ -n "${extra_flags:-}" ]]; then
    printf '%s|%s|%s|%s\n' "$object_name" "$rel_source" "$include_roots" "$extra_flags"
  else
    printf '%s|%s|%s\n' "$object_name" "$rel_source" "$include_roots"
  fi >>"$workspace_root/COMPILE_TIER${tier_index}.txt"
done <"$units_manifest_path"

cat >"$tier_status_path" <<'EOF'
tier0|libc/compiler smoke C files|host-built|guest-linked
tier1|libnsutils and simple utility leaf files|host-built|guest-linked
tier2|libwapcaplet and parserutils leaf files|host-built|guest-linked
tier3|libcss/libdom/hubbub parser leaf files|host-built|guest-linked
tier4|NetSurf utility/desktop/content support files|host-built|guest-linked
tier5|NetSurf CSS, HTML, layout, form, and redraw files|host-built|guest-linked
EOF

while IFS= read -r rel_path; do
  src_path="$repo_root/$rel_path"
  dst_path="$workspace_root/$rel_path"

  [[ -n "$rel_path" ]] || continue

  if [[ -f "$src_path" ]]; then
    mkdir -p "$(dirname "$dst_path")"
    cp "$src_path" "$dst_path"
    continue
  fi

  if [[ -d "$src_path" ]]; then
    mkdir -p "$(dirname "$dst_path")"
    mkdir -p "$dst_path"
    cp -R "$src_path"/. "$dst_path"/.
    continue
  fi

  if [[ ! -e "$src_path" ]]; then
    printf 'prepare-browser-selfhost-c-workspace: missing source %s\n' "$rel_path" >&2
    exit 1
  fi
done <"$manifest_path"

if [[ "${ZBROWSER_SELFHOST_COMPACT_ASSETS:-0}" != 0 ]]; then
  mkdir -p "$workspace_root/build"
  cat >"$workspace_root/build/dejavu_sans_ttf.h" <<'EOF'
#ifndef BUILD_DEJAVU_SANS_TTF_H
#define BUILD_DEJAVU_SANS_TTF_H
#include <stdint.h>
static const uint8_t dejavu_sans_ttf[] = { 0 };
static const uint32_t dejavu_sans_ttf_len = 0;
#endif
EOF
fi

mkdir -p "$workspace_root/third_party/netsurf/src/libdom/include/dom/bindings/hubbub"
cp "$workspace_root/third_party/netsurf/src/libdom/bindings/hubbub/parser.h" \
  "$workspace_root/third_party/netsurf/src/libdom/include/dom/bindings/hubbub/parser.h"
cp "$workspace_root/third_party/netsurf/src/libdom/bindings/hubbub/errors.h" \
  "$workspace_root/third_party/netsurf/src/libdom/include/dom/bindings/hubbub/errors.h"

{
  cat <<'EOF'
When the in-OS C compiler lands, mirror these validated host-equivalent
commands from browser_c/ and write the objects under browser_c/build/:
EOF

  unit_index=1
  while IFS='|' read -r object_name rel_source include_roots extra_flags; do
    include_flags=
    flag_text=
    [[ -n "$object_name" ]] || continue

    IFS=':' read -r -a include_array <<< "$include_roots"
    for include_root in "${include_array[@]}"; do
      [[ -n "$include_root" ]] || continue
      include_flags="${include_flags} -I${include_root}"
    done

    if [[ -n "$extra_flags" ]]; then
      IFS=';' read -r -a extra_flag_array <<< "$extra_flags"
      for extra_flag in "${extra_flag_array[@]}"; do
        [[ -n "$extra_flag" ]] || continue
        extra_flag="${extra_flag//\\\"/\"}"
        flag_text="${flag_text} ${extra_flag}"
      done
    fi

    printf '%u. cc -c -ffreestanding -mno-red-zone%s%s %s -o build/%s\n' \
      "$unit_index" "$include_flags" "$flag_text" "$rel_source" "$object_name"
    unit_index=$((unit_index + 1))
  done <"$units_manifest_path"

  cat <<'EOF'

All units avoid parser-generator outputs and keep the first browser-C step
generic and small, even where upstream ships a checked-in data table such as
utils/idna_props.h.

The same commands are also available as COMPILE_TIER0.txt through
COMPILE_TIER5.txt, with the current host/guest status tracked in
COMPILE_TIERS.txt.
EOF
} >"$commands_path"

cp -R "$repo_root/examples/browser_c_probe" "$probe_root"

printf '%s\n' "$workspace_root"
