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

if [[ ! -f "$manifest_path" ]]; then
  printf 'prepare-browser-selfhost-c-workspace: missing manifest %s\n' "$manifest_path" >&2
  exit 1
fi
if [[ ! -f "$units_manifest_path" ]]; then
  printf 'prepare-browser-selfhost-c-workspace: missing units manifest %s\n' "$units_manifest_path" >&2
  exit 1
fi

rm -rf "$workspace_root" "$probe_root"
mkdir -p "$workspace_root"

cat >"$notes_path" <<'EOF'
browser_c is the first staged in-OS C selfhost slice for browser work.

It is intentionally small:
- libnsutils base64/time/unistd
- netsurf utils bloom/hashmap/hashtable/punycode/file/filepath/log/messages/talloc/time/url/useragent/utils

The goal is to remove the guest-side source staging blocker before the in-OS
C compiler lands. The tree preserves upstream-relative paths so future compile
commands can reuse the same include roots as the host build without depending
on host system headers.

COMPILE_UNITS.txt is the machine-readable first-pass browser-C build plan.
It is validated on the host by scripts/zbrowser-c-host-compile-smoke.sh with
-nostdinc plus repo-staged freestanding headers, and is intended to become the
first in-OS browser-C compile queue.

browser_c_probe/ is a tiny Z project that runs from the same staged workspace
and validates that the in-OS toolchain can consume that compile queue before a
guest C compiler exists.

hashtable.c intentionally uses the generic kernel zlib ABI header so the queue
exercises one concrete browser-C dependency without introducing browser-specific
kernel logic.
EOF

cp "$manifest_path" "$filelist_path"
cp "$units_manifest_path" "$compile_units_path"

while IFS= read -r rel_path; do
  src_path="$repo_root/$rel_path"
  dst_path="$workspace_root/$rel_path"

  [[ -n "$rel_path" ]] || continue

  if [[ ! -f "$src_path" ]]; then
    printf 'prepare-browser-selfhost-c-workspace: missing source %s\n' "$rel_path" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$dst_path")"
  cp "$src_path" "$dst_path"
done <"$manifest_path"

{
  cat <<'EOF'
When the in-OS C compiler lands, mirror these validated host-equivalent
commands from browser_c/ and write the objects under browser_c/build/:
EOF

  unit_index=1
  while IFS='|' read -r object_name rel_source include_roots; do
    include_flags=
    [[ -n "$object_name" ]] || continue

    IFS=':' read -r -a include_array <<< "$include_roots"
    for include_root in "${include_array[@]}"; do
      [[ -n "$include_root" ]] || continue
      include_flags="${include_flags} -I${include_root}"
    done

    printf '%u. cc -c -ffreestanding%s %s -o build/%s\n' \
      "$unit_index" "$include_flags" "$rel_source" "$object_name"
    unit_index=$((unit_index + 1))
  done <"$units_manifest_path"

  cat <<'EOF'

All units avoid generated parser tables and keep the first browser-C step
generic and small.
EOF
} >"$commands_path"

cp -R "$repo_root/examples/browser_c_probe" "$probe_root"

printf '%s\n' "$workspace_root"
