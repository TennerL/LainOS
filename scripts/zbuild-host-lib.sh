zbuild_host_fail_bad_directive() {
  local directive="$1"
  printf 'zbuild-host: bad %s directive in %s\n' "$directive" "$ZBUILD_MANIFEST_PATH" >&2
  exit 1
}

zbuild_host_valid_lainfs_name() {
  local name="$1"
  local len="${#name}"

  if [[ "$len" -eq 0 || "$len" -gt 30 ]]; then
    return 1
  fi
  if [[ "$name" == *"/"* || "$name" == *"\\"* || "$name" == *":"* || "$name" == *$'\t'* || "$name" == *" "* ]]; then
    return 1
  fi
  return 0
}

zbuild_host_resolve_path() {
  local base_dir="$1"
  local path="$2"

  if [[ "$path" = /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s/%s\n' "$base_dir" "$path"
  fi
}

zbuild_host_resolve_dir() {
  local base_dir="$1"
  local path="$2"
  local directive="$3"
  local resolved=

  resolved="$(zbuild_host_resolve_path "$base_dir" "$path")"
  if [[ ! -d "$resolved" ]]; then
    zbuild_host_fail_bad_directive "$directive"
  fi
  if ! resolved="$(cd "$resolved" && pwd)"; then
    zbuild_host_fail_bad_directive "$directive"
  fi

  printf '%s\n' "$resolved"
}

zbuild_host_trim_line() {
  sed 's/\r$//; s/^[[:space:]]*//; s/[[:space:]]*$//'
}

zbuild_host_prepare_manifest() {
  local manifest_input="$1"
  local initial_build_root="$2"
  local initial_install_root="$3"

  ZBUILD_MANIFEST_INPUT="$manifest_input"
  ZBUILD_MANIFEST_DIR="$(cd "$(dirname "$manifest_input")" && pwd)"
  ZBUILD_MANIFEST_NAME="$(basename "$manifest_input")"
  ZBUILD_MANIFEST_PATH="$ZBUILD_MANIFEST_DIR/$ZBUILD_MANIFEST_NAME"
  ZBUILD_TARGET_NAME="${ZBUILD_MANIFEST_NAME%.zbuild}"

  if [[ "$ZBUILD_MANIFEST_NAME" == "$ZBUILD_TARGET_NAME" ]]; then
    printf 'zbuild-host: manifest must end with .zbuild: %s\n' "$manifest_input" >&2
    exit 2
  fi
  if [[ ! -f "$ZBUILD_MANIFEST_PATH" ]]; then
    printf 'zbuild-host: manifest not found: %s\n' "$ZBUILD_MANIFEST_PATH" >&2
    exit 1
  fi

  mkdir -p "$initial_build_root" "$initial_install_root"

  ZBUILD_SOURCE_DIR="$ZBUILD_MANIFEST_DIR"
  ZBUILD_BUILD_DIR="$(cd "$initial_build_root" && pwd)"
  ZBUILD_INSTALL_ROOT="$(cd "$initial_install_root" && pwd)"
  ZBUILD_INSTALL_DIR="$ZBUILD_INSTALL_ROOT"
  ZBUILD_LINKED_OUTPUT=
  ZBUILD_OUTPUT_NAME=
  ZBUILD_EFFECTIVE_OUTPUT=
  ZBUILD_BUILD_LOG=
  ZBUILD_INSTALL_NAME=
  ZBUILD_LINK_OUTPUT=1
  ZBUILD_OBJECTS_ONLY=0
  ZBUILD_EXPECTED_RETURN=
  ZBUILD_HAS_EXPECTED_RETURN=0
  ZBUILD_SOURCE_COUNT=0
  declare -ga ZBUILD_INCLUDE_ARGS=("--include" "$ZBUILD_MANIFEST_DIR")
  declare -ga ZBUILD_LINK_ARGS=()
  declare -ga ZBUILD_OBJECT_NAMES=()
  declare -ga ZBUILD_OBJECT_PATHS=()
}

zbuild_host_parse_manifest() {
  local raw_line=
  local line=
  local field_count=
  local directive=
  local include_dir=
  local source_name=
  local object_name=
  local source_path=
  local object_path=

  while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    fields=()
    line="$(printf '%s\n' "$raw_line" | zbuild_host_trim_line)"
    if [[ -z "$line" || "$line" == \#* || "$line" == \;* || "$line" == //* ]]; then
      continue
    fi

    read -r -a fields <<<"$line"
    field_count="${#fields[@]}"
    directive="${fields[0]:-}"
    case "$directive" in
      src|source)
        if [[ $field_count -ne 2 ]]; then
          printf 'zbuild-host: bad %s directive in %s\n' "$directive" "$ZBUILD_MANIFEST_PATH" >&2
          exit 1
        fi
        ZBUILD_SOURCE_DIR="$(zbuild_host_resolve_dir "$ZBUILD_MANIFEST_DIR" "${fields[1]}" "$directive")"
        ;;
      build)
        if [[ $field_count -ne 2 ]]; then
          printf 'zbuild-host: bad build directive in %s\n' "$ZBUILD_MANIFEST_PATH" >&2
          exit 1
        fi
        ZBUILD_BUILD_DIR="$(zbuild_host_resolve_dir "$ZBUILD_MANIFEST_DIR" "${fields[1]}" "build")"
        ;;
      include)
        if [[ $field_count -ne 2 ]]; then
          printf 'zbuild-host: bad include directive in %s\n' "$ZBUILD_MANIFEST_PATH" >&2
          exit 1
        fi
        include_dir="$(zbuild_host_resolve_dir "$ZBUILD_MANIFEST_DIR" "${fields[1]}" "include")"
        ZBUILD_INCLUDE_ARGS+=("--include" "$include_dir")
        ;;
      output)
        if [[ $field_count -ne 2 ]]; then
          zbuild_host_fail_bad_directive output
        fi
        if ! zbuild_host_valid_lainfs_name "${fields[1]}"; then
          zbuild_host_fail_bad_directive output
        fi
        ZBUILD_LINKED_OUTPUT="${fields[1]}"
        ;;
      install)
        if [[ $field_count -ne 2 ]]; then
          zbuild_host_fail_bad_directive install
        fi
        ZBUILD_INSTALL_DIR="$(zbuild_host_resolve_dir "$ZBUILD_INSTALL_ROOT" "${fields[1]}" "install")"
        ;;
      install-name)
        if [[ $field_count -ne 2 ]]; then
          zbuild_host_fail_bad_directive install-name
        fi
        if ! zbuild_host_valid_lainfs_name "${fields[1]}"; then
          zbuild_host_fail_bad_directive install-name
        fi
        ZBUILD_INSTALL_NAME="${fields[1]}"
        ;;
      test-return)
        if [[ $field_count -ne 2 || ! "${fields[1]}" =~ ^[0-9]+$ ]]; then
          zbuild_host_fail_bad_directive test-return
        fi
        ZBUILD_EXPECTED_RETURN="${fields[1]}"
        ZBUILD_HAS_EXPECTED_RETURN=1
        ;;
      module|objects-only)
        if [[ $field_count -ne 1 ]]; then
          printf 'zbuild-host: bad %s directive in %s\n' "$directive" "$ZBUILD_MANIFEST_PATH" >&2
          exit 1
        fi
        ZBUILD_LINK_OUTPUT=0
        ;;
      *)
        if [[ $field_count -gt 2 ]]; then
          printf 'zbuild-host: too many fields in %s: %s\n' "$ZBUILD_MANIFEST_PATH" "$line" >&2
          exit 1
        fi
        source_name="${fields[0]}"
        object_name="${fields[1]:-${source_name%.Z}.zo}"
        if ! zbuild_host_valid_lainfs_name "$object_name"; then
          printf 'zbuild-host: bad object name in %s: %s\n' "$ZBUILD_MANIFEST_PATH" "$object_name" >&2
          exit 1
        fi
        source_path="$(zbuild_host_resolve_path "$ZBUILD_SOURCE_DIR" "$source_name")"
        object_path="$(zbuild_host_resolve_path "$ZBUILD_BUILD_DIR" "$object_name")"
        mkdir -p "$(dirname "$object_path")"
        ZBUILD_LINK_ARGS+=("$source_path" "$object_path")
        ZBUILD_OBJECT_NAMES+=("$object_name")
        ZBUILD_OBJECT_PATHS+=("$object_path")
        ZBUILD_SOURCE_COUNT=$((ZBUILD_SOURCE_COUNT + 1))
        ;;
    esac
  done <"$ZBUILD_MANIFEST_PATH"

  if [[ $ZBUILD_SOURCE_COUNT -eq 0 ]]; then
    printf 'zbuild-host: manifest has no sources: %s\n' "$ZBUILD_MANIFEST_PATH" >&2
    exit 1
  fi

  if [[ $ZBUILD_LINK_OUTPUT -ne 0 ]]; then
    if [[ -n "$ZBUILD_LINKED_OUTPUT" ]]; then
      ZBUILD_OUTPUT_NAME="$ZBUILD_LINKED_OUTPUT"
    else
      ZBUILD_OUTPUT_NAME="$ZBUILD_TARGET_NAME.bin"
    fi
    ZBUILD_EFFECTIVE_OUTPUT="$ZBUILD_BUILD_DIR/$ZBUILD_OUTPUT_NAME"
  elif [[ $ZBUILD_SOURCE_COUNT -eq 1 ]]; then
    ZBUILD_OUTPUT_NAME="${ZBUILD_OBJECT_NAMES[0]}"
    ZBUILD_EFFECTIVE_OUTPUT="${ZBUILD_OBJECT_PATHS[0]}"
  fi

  if [[ -z "$ZBUILD_INSTALL_NAME" && -n "$ZBUILD_OUTPUT_NAME" ]]; then
    ZBUILD_INSTALL_NAME="$ZBUILD_OUTPUT_NAME"
  fi

  ZBUILD_OBJECTS_ONLY=$((ZBUILD_LINK_OUTPUT == 0 ? 1 : 0))
  ZBUILD_BUILD_LOG="$ZBUILD_BUILD_DIR/$ZBUILD_TARGET_NAME.buildlog"
}
