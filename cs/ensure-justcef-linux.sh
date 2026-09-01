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

LOCK_PATH="${ZIP_PATH}.lock"
mkdir -p "$(dirname "$LOCK_PATH")"
exec 9>"$LOCK_PATH"

if command -v flock >/dev/null 2>&1; then
  flock 9
else
  LOCK_DIR="${LOCK_PATH}.d"
  while ! mkdir "$LOCK_DIR" 2>/dev/null; do sleep 0.1; done
  trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT INT TERM
fi

if [[ -f "$MARKER_VERSION" ]] \
  && [[ "$(cat "$MARKER_VERSION")" == "$VERSION" ]] \
  && [[ -f "$ZIP_PATH" ]] \
  && is_dir_nonempty "$EXTRACT_DIR"; then
  exit 0
fi

if [[ ! -f "$ZIP_PATH" ]]; then
  echo "JustCef: downloading $URL"
  tmp="$(mktemp "${ZIP_PATH}.tmp.XXXXXX")"
  curl -L --fail --retry 3 --retry-delay 2 -o "$tmp" "$URL"
  mv -f "$tmp" "$ZIP_PATH"
fi

rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"

if command -v unzip >/dev/null 2>&1; then
  unzip -q "$ZIP_PATH" -d "$EXTRACT_DIR"
elif command -v python3 >/dev/null 2>&1; then
  python3 -m zipfile -e "$ZIP_PATH" "$EXTRACT_DIR"
else
  echo "JustCef: need 'unzip' or 'python3' to extract zip" >&2
  exit 1
fi

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