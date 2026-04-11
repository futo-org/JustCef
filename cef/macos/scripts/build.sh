#!/bin/sh
set -eu

usage() {
  echo "Usage: $0 [x64] [arm64] [all]" >&2
  exit 1
}

resolve_python() {
  if [ -n "${PYTHON_BIN:-}" ]; then
    printf '%s\n' "${PYTHON_BIN}"
    return 0
  fi

  if command -v python3.11 >/dev/null 2>&1; then
    command -v python3.11
    return 0
  fi

  if command -v python3 >/dev/null 2>&1; then
    command -v python3
    return 0
  fi

  echo "python3.11 or python3 is required." >&2
  exit 1
}

REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
CEF_BRANCH=$(tr -d '\r\n' < "${REPO_ROOT}/cef.branch")
WORK_ROOT="${WORK_ROOT:-$HOME/code}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-${WORK_ROOT}/chromium_git}"
DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-${WORK_ROOT}/depot_tools}"
PYTHON="$(resolve_python)"

export GN_DEFINES="${GN_DEFINES:-is_official_build=true proprietary_codecs=true ffmpeg_branding=Chrome symbol_level=1 is_cfi=false use_thin_lto=false is_component_ffmpeg=false enable_widevine=true enable_printing=true enable_cdm_host_verification=true angle_enable_vulkan_validation_layers=false dawn_enable_vulkan_validation_layers=false dawn_use_built_dxc=false}"
export CEF_USE_GN=1
export CEF_ARCHIVE_FORMAT=tar.bz2
export CEF_CUSTOM_PATCH_SCRIPT="${REPO_ROOT}/patches/apply_cef_patches.py"

check_prereqs() {
  if [ ! -d "${DOWNLOAD_DIR}/cef/.git" ]; then
    echo "Missing CEF checkout at ${DOWNLOAD_DIR}/cef. Run setup.sh first." >&2
    exit 1
  fi

  if [ ! -d "${DOWNLOAD_DIR}/chromium/src/.git" ]; then
    echo "Missing Chromium checkout at ${DOWNLOAD_DIR}/chromium/src. Run setup.sh first." >&2
    exit 1
  fi

  if [ ! -d "${DEPOT_TOOLS_DIR}/.git" ]; then
    echo "Missing depot_tools checkout at ${DEPOT_TOOLS_DIR}. Run setup.sh first." >&2
    exit 1
  fi
}

run_build() {
  arch="$1"

  case "$arch" in
    x64)
      arch_arg="--x64-build"
      ;;
    arm64)
      arch_arg="--arm64-build"
      ;;
    *)
      usage
      ;;
  esac

  echo "==> Starting macOS ${arch} build for branch ${CEF_BRANCH}"
  "${PYTHON}" "${REPO_ROOT}/automate/automate-git.py" \
    --branch="${CEF_BRANCH}" \
    --download-dir="${DOWNLOAD_DIR}" \
    --depot-tools-dir="${DEPOT_TOOLS_DIR}" \
    --minimal-distrib-only \
    --force-clean \
    --force-build \
    --with-pgo-profiles \
    --no-debug-build \
    "${arch_arg}"
}

build_x64=0
build_arm64=0

check_prereqs

if [ "$#" -eq 0 ]; then
  build_x64=1
  build_arm64=1
fi

for arg in "$@"; do
  case "$arg" in
    all)
      build_x64=1
      build_arm64=1
      ;;
    x64)
      build_x64=1
      ;;
    arm64)
      build_arm64=1
      ;;
    *)
      usage
      ;;
  esac
done

if [ "$build_x64" -eq 1 ]; then
  run_build x64
fi

if [ "$build_arm64" -eq 1 ]; then
  run_build arm64
fi

