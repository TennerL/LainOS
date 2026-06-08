#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

stage_root="${1:-build/browser-selfhost-c-stage}"
queue_root="${2:-build/browser-selfhost-c-queue-module-link}"
package_root="${3:-build/zbrowser-netsurf-full}"
seed_individual_objects="${ZBROWSER_FULL_SEED_OBJECTS:-0}"
source_built_objects="${ZBROWSER_FULL_SOURCE_OBJECTS:-base64.o nsutils_time.o nsutils_unistd.o parserutils_utf8_core.o lwc_core.o dom_string_core.o dom_namespace_core.o dom_nodelist_core.o dom_implementation_core.o dom_document_core.o hubbub_errors_core.o hubbub_string_core.o hubbub_detect_core.o dom_html_button_core.o dom_html_input_core.o dom_html_select_core.o dom_html_script_core.o dom_html_textarea_core.o ns_css_internal.o ns_html_font.o ns_html_redraw_border.o bloom.o corestrings.o libdom.o bitmap.o mouse.o plot_style.o search.o searchweb.o scrollbar.o system_colour.o version.o hashmap.o hashtable.o punycode.o file.o filepath.o http_generics.o http_primitives.o http_parameter.o http_content_disposition.o http_content_type.o http_cache_control.o http_challenge.o http_strict_transport_security.o http_www_authenticate.o idna.o log.o messages.o nscolour.o nsoption.o ssl_certs.o talloc.o time.o url.o useragent.o utf8.o utils.o nsurl_core.o nsurl_parse.o auto_third_party_netsurf_src_libdom_src_core_attr_c.o auto_third_party_netsurf_src_libdom_src_core_cdatasection_c.o auto_third_party_netsurf_src_libdom_src_core_characterdata_c.o auto_third_party_netsurf_src_libdom_src_core_comment_c.o auto_third_party_netsurf_src_libdom_src_core_doc_fragment_c.o}"

scripts/zbrowser-c-queue-module-link-probe.sh "$stage_root" "$queue_root" >/dev/null

rm -rf "$package_root"
mkdir -p "$package_root"

cp examples/zbrowser_netsurf.Z "$package_root/zbrowser_netsurf.Z"

manifest="$package_root/zbrowser_netsurf.zbuild"
seed_args="$package_root/seed-args.txt"
copy_script="$package_root/autoexec-copy-lines.txt"

{
  printf 'include .\n'
  printf 'module\n'
} >"$manifest"
: >"$seed_args"
: >"$copy_script"

index=0
source_build_manifest_name() {
  local object_name="$1"
  local rel_source="$2"
  local wanted

  for wanted in $source_built_objects; do
    if [[ "$wanted" == "$object_name" ]]; then
      case "$object_name" in
        time.o) printf 'netsurf_time.c\n' ;;
        url.o) printf 'netsurf_url.c\n' ;;
        utf8.o) printf 'netsurf_utf8.c\n' ;;
        utils.o) printf 'netsurf_utils.c\n' ;;
        http_strict_transport_security.o) printf 'http_sts.c\n' ;;
        auto_third_party_netsurf_src_libdom_src_core_attr_c.o) printf 'dom_attr_core.c\n' ;;
        auto_third_party_netsurf_src_libdom_src_core_cdatasection_c.o) printf 'dom_cdata_core.c\n' ;;
        auto_third_party_netsurf_src_libdom_src_core_characterdata_c.o) printf 'dom_characterdata_core.c\n' ;;
        auto_third_party_netsurf_src_libdom_src_core_comment_c.o) printf 'dom_comment_core.c\n' ;;
        auto_third_party_netsurf_src_libdom_src_core_doc_fragment_c.o) printf 'dom_doc_fragment_core.c\n' ;;
        *) printf '%s.c\n' "${object_name%.o}" ;;
      esac
      return 0
    fi
  done

  return 1
}

add_object() {
  local source_object="$1"
  local manifest_source="${2:-}"
  local seed_source="${3:-}"
  local object_name

  index=$((index + 1))
  object_name="$(printf 'zc%03u.zo' "$index")"
  cp "$source_object" "$package_root/$object_name"
  if [[ -n "$manifest_source" ]]; then
    cp "$seed_source" "$package_root/$manifest_source"
    printf '%s %s\n' "$manifest_source" "$object_name" >>"$manifest"
    printf '%s/%s=examples/%s\n' "$package_root" "$manifest_source" "$manifest_source" >>"$seed_args"
    printf 'cp R:/examples/%s %s\n' "$manifest_source" "$manifest_source" >>"$copy_script"
  else
    printf '%s %s\n' "$object_name" "$object_name" >>"$manifest"
  fi
  if [[ "$seed_individual_objects" = "1" ]]; then
    printf '%s/%s=examples/%s\n' "$package_root" "$object_name" "$object_name" >>"$seed_args"
  fi
  printf 'cp R:/examples/%s %s\n' "$object_name" "$object_name" >>"$copy_script"
}

plan_path="$stage_root/browser_c/COMPILE_UNITS.txt"
while IFS='|' read -r object_name _rel_source _include_roots _extra_flags; do
  manifest_source=
  [[ -n "$object_name" ]] || continue
  if manifest_source="$(source_build_manifest_name "$object_name" "$_rel_source")"; then
    add_object "$queue_root/${object_name%.o}.zo" "$manifest_source" "$stage_root/browser_c/$_rel_source"
  else
    add_object "$queue_root/${object_name%.o}.zo"
  fi
done <"$plan_path"

if [[ -f "$queue_root/AUTO_UNITS.txt" ]]; then
  while IFS='|' read -r object_name _rel_source; do
    manifest_source=
    [[ -n "$object_name" ]] || continue
    if manifest_source="$(source_build_manifest_name "$object_name" "$_rel_source")"; then
      add_object "$queue_root/${object_name%.o}.zo" "$manifest_source" "$stage_root/browser_c/$_rel_source"
    else
      add_object "$queue_root/${object_name%.o}.zo"
    fi
  done <"$queue_root/AUTO_UNITS.txt"
fi

{
  printf 'zbrowser_netsurf.Z zbrowser_netsurf.zo\n'
} >>"$manifest"

{
  printf '%s/zbrowser_netsurf.Z=examples/zbrowser_netsurf.Z\n' "$package_root"
  printf '%s/zbrowser_netsurf.zbuild=examples/zbrowser_netsurf.zbuild\n' "$package_root"
} >>"$seed_args"

{
  printf 'cp R:/examples/zbrowser_netsurf.Z zbrowser_netsurf.Z\n'
  printf 'cp R:/examples/zbrowser_netsurf.zbuild zbrowser_netsurf.zbuild\n'
} >>"$copy_script"

ZBUILD_HOST_MODULE_LINK=1 scripts/zbuild-host.sh "$manifest" "$package_root/host-build" >/dev/null 2>&1
cp "$package_root/host-build/zbrowser_netsurf.zo" "$package_root/zbrowser_netsurf.zo"
python3 - "$manifest" "$package_root" "$package_root/zbrowser_netsurf.zpkg" <<'PY'
import os
import struct
import sys

manifest_path, package_root, output_path = sys.argv[1:4]
chunk_size = 3500000
objects = []
with open(manifest_path, "r", encoding="utf-8") as manifest:
    for raw_line in manifest:
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";") or line.startswith("//"):
            continue
        parts = line.split()
        if not parts:
            continue
        if parts[0] in {"include", "module", "objects-only", "src", "source", "build", "output", "install", "install-name"}:
            continue
        object_name = parts[1] if len(parts) > 1 else os.path.splitext(parts[0])[0] + ".zo"
        objects.append(object_name)

header_size = 12 + len(objects) * 40
offset = header_size
entries = []
for object_name in objects:
    object_path = os.path.join(package_root, object_name)
    size = os.path.getsize(object_path)
    entries.append((object_name, offset, size, object_path))
    offset += size

with open(output_path, "wb") as out:
    out.write(b"ZPKG1\0\0\0")
    out.write(struct.pack("<I", len(entries)))
    for object_name, object_offset, size, _path in entries:
        encoded = object_name.encode("ascii")
        if len(encoded) > 31:
            raise SystemExit(f"object name too long for zpkg: {object_name}")
        out.write(encoded + b"\0" * (32 - len(encoded)))
        out.write(struct.pack("<II", object_offset, size))
    for _object_name, _object_offset, _size, object_path in entries:
        with open(object_path, "rb") as obj:
            out.write(obj.read())

with open(output_path, "rb") as package:
    index = 0
    while True:
        data = package.read(chunk_size)
        if not data:
            break
        with open(os.path.join(package_root, f"zbrowser_netsurf.zp{index:02d}"), "wb") as chunk:
            chunk.write(data)
        index += 1
PY
for chunk in "$package_root"/zbrowser_netsurf.zp[0-9][0-9]; do
  [[ -f "$chunk" ]] || continue
  printf '%s=examples/%s\n' "$chunk" "$(basename "$chunk")" >>"$seed_args"
done
for chunk in "$package_root"/zbrowser_netsurf.zp[0-9][0-9]; do
  [[ -f "$chunk" ]] || continue
  printf 'cp R:/examples/%s %s\n' "$(basename "$chunk")" "$(basename "$chunk")" >>"$copy_script"
done

printf 'zbrowser-netsurf-full-package: objects=%u package=%s\n' "$index" "$package_root"
