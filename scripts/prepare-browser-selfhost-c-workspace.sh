#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

manifest_path="scripts/browser-selfhost-c-files.txt"
stage_root="${1:-build/browser-selfhost-c-stage}"
workspace_root="$stage_root/browser_c"
notes_path="$workspace_root/README.txt"
commands_path="$workspace_root/NEXT_C.txt"
filelist_path="$workspace_root/FILES.txt"

if [[ ! -f "$manifest_path" ]]; then
  printf 'prepare-browser-selfhost-c-workspace: missing manifest %s\n' "$manifest_path" >&2
  exit 1
fi

rm -rf "$workspace_root"
mkdir -p "$workspace_root"

cat >"$notes_path" <<'EOF'
browser_c is the first staged in-OS C selfhost slice for browser work.

It is intentionally small:
- libnsutils base64
- netsurf utils bloom

The goal is to remove the guest-side source staging blocker before the in-OS
C compiler lands. The tree preserves upstream-relative paths so future compile
commands can reuse the same include roots as the host build.
EOF

cat >"$commands_path" <<'EOF'
When an in-OS C compiler exists, start with these compile units:
1. third_party/netsurf/src/libnsutils/src/base64.c
   include root: third_party/netsurf/src/libnsutils/include
2. third_party/netsurf/src/netsurf/utils/bloom.c
   include roots:
   - third_party/netsurf/src/netsurf
   - third_party/netsurf/src/netsurf/include

Both units avoid generated parser tables and keep the first browser-C step
generic and small.
EOF

cp "$manifest_path" "$filelist_path"

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

printf '%s\n' "$workspace_root"
