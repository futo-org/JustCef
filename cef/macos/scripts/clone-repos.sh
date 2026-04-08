#!/bin/sh
set -eu

WORK_ROOT="${WORK_ROOT:-$HOME/code}"
CHROMIUM_GIT_DIR="${CHROMIUM_GIT_DIR:-${WORK_ROOT}/chromium_git}"
DEPOT_TOOLS_DIR="${DEPOT_TOOLS_DIR:-${WORK_ROOT}/depot_tools}"

mkdir -p "${WORK_ROOT}"
mkdir -p "${CHROMIUM_GIT_DIR}"

if [ ! -d "${CHROMIUM_GIT_DIR}/cef/.git" ]; then
  git clone https://github.com/chromiumembedded/cef.git "${CHROMIUM_GIT_DIR}/cef"
fi

if [ ! -d "${CHROMIUM_GIT_DIR}/chromium/src/.git" ]; then
  mkdir -p "${CHROMIUM_GIT_DIR}/chromium"
  git clone https://github.com/chromium/chromium.git "${CHROMIUM_GIT_DIR}/chromium/src"
fi

if [ ! -d "${DEPOT_TOOLS_DIR}/.git" ]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "${DEPOT_TOOLS_DIR}"
fi

