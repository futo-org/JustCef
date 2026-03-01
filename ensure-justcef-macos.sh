#!/usr/bin/env bash
set -euo pipefail

URL="${1:?url}"
ZIP_PATH="${2:?zip_path}"
EXTRACT_DIR="${3:?extract_dir}"
VERSION="${4:?version}"

MARKER_VERSION="${EXTRACT_DIR}/.justcef.version"
MARKER_URL="${EXTRACT_DIR}/.justcef.url"

is_dir_nonempty() {
  [[ -d "$1" ]] && find "$1" -mindepth 1 -print -quit | grep -q .
}

mkdir -p "$(dirname "$ZIP_PATH")" "$EXTRACT_DIR"

if [[ -f "$MARKER_VERSION" ]] && [[ "$(cat "$MARKER_VERSION")" == "$VERSION" ]] && is_dir_nonempty "$EXTRACT_DIR"; then
  exit 0
fi

if [[ ! -f "$ZIP_PATH" ]]; then
  echo "JustCef: downloading $URL"
  tmp="${ZIP_PATH}.tmp"
  rm -f "$tmp"
  curl -L --fail --retry 3 --retry-delay 2 -o "$tmp" "$URL"
  mv "$tmp" "$ZIP_PATH"
fi

rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"

# macOS: use ditto for zip extraction (permissions/symlinks/etc)
ditto -x -k "$ZIP_PATH" "$EXTRACT_DIR"

# Normalize: if zip contains a single top-level directory, flatten it
top_level_count="$(find "$EXTRACT_DIR" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')"
if [[ "$top_level_count" == "1" ]]; then
  only_entry="$(find "$EXTRACT_DIR" -mindepth 1 -maxdepth 1)"
  if [[ -d "$only_entry" ]]; then
    shopt -s dotglob
    tmpdir="${EXTRACT_DIR}.flatten"
    rm -rf "$tmpdir"
    mkdir -p "$tmpdir"
    mv "$only_entry"/* "$tmpdir"/
    rm -rf "$EXTRACT_DIR"
    mv "$tmpdir" "$EXTRACT_DIR"
    shopt -u dotglob
  fi
fi

printf "%s" "$VERSION" > "$MARKER_VERSION"
printf "%s" "$URL" > "$MARKER_URL"