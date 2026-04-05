#!/bin/sh
set -eu

usage() {
  echo "Usage: $0 [x64] [arm64] [all]" >&2
  exit 1
}

REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
CEF_BRANCH=$(tr -d '\r\n' < "${REPO_ROOT}/cef.branch")

export GN_DEFINES="is_official_build=true proprietary_codecs=true ffmpeg_branding=Chrome use_sysroot=true symbol_level=1 is_cfi=false use_thin_lto=false is_component_ffmpeg=false enable_widevine=true enable_printing=true enable_cdm_host_verification=false angle_enable_vulkan_validation_layers=false dawn_enable_vulkan_validation_layers=false dawn_use_built_dxc=false"
export CEF_USE_GN=1
export CEF_ARCHIVE_FORMAT=tar.bz2
export CEF_CUSTOM_PATCH_SCRIPT="${REPO_ROOT}/patches/apply_cef_patches.py"

check_swap() {
  current_swap_kib=$(awk 'NR > 1 { sum += $3 } END { print sum + 0 }' /proc/swaps)
  required_swap_kib=$((32 * 1024 * 1024))
  # mkswap reserves a small amount of metadata, so a 32 GiB swapfile will
  # report slightly less usable swap than its nominal file size.
  minimum_active_swap_kib=$((required_swap_kib - 1024))

  if [ "$current_swap_kib" -lt "$minimum_active_swap_kib" ]; then
    current_swap_mib=$((current_swap_kib / 1024))
    echo "At least 32 GiB of active swap is required. Current active swap: ${current_swap_mib} MiB." >&2
    exit 1
  fi
}

run_build() {
  arch="$1"

  case "$arch" in
    x64)
      unset CEF_INSTALL_SYSROOT || true
      arch_arg="--x64-build"
      ;;
    arm64)
      export CEF_INSTALL_SYSROOT=arm64
      arch_arg="--arm64-build"
      ;;
    *)
      usage
      ;;
  esac

  echo "==> Starting ${arch} build for branch ${CEF_BRANCH}"
  python3 "${REPO_ROOT}/automate/automate-git.py" \
    --branch="${CEF_BRANCH}" \
    --download-dir=/home/ubuntu/code/chromium_git \
    --depot-tools-dir=/home/ubuntu/code/depot_tools \
    --minimal-distrib-only \
    --build-target=cefsimple \
    --force-clean \
    --force-build \
    --with-pgo-profiles \
    --no-debug-build \
    "${arch_arg}"
}

build_x64=0
build_arm64=0

check_swap

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
