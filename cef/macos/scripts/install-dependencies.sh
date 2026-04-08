#!/bin/sh

WORK_ROOT="${WORK_ROOT:-$HOME/code}"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "This script must be run on macOS." >&2
  exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Xcode Command Line Tools are required. Run 'xcode-select --install' first." >&2
  exit 1
fi

if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "xcodebuild is missing. Install Xcode or the Command Line Tools first." >&2
  exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required for the local macOS builder setup." >&2
  echo "Install it from https://brew.sh and rerun this script." >&2
  exit 1
fi

brew update || exit 1
brew install git python@3.11 cmake ninja || exit 1

git config --global core.autocrlf false || exit 1
git config --global core.filemode false || exit 1
git config --global core.fscache true || exit 1
git config --global core.preloadindex true || exit 1

PYTHON_BIN=""
if command -v python3.11 >/dev/null 2>&1; then
  PYTHON_BIN="$(command -v python3.11)"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="$(command -v python3)"
else
  echo "No suitable Python 3 interpreter was found after Homebrew install." >&2
  exit 1
fi

"${PYTHON_BIN}" -m pip install --upgrade pip || exit 1
"${PYTHON_BIN}" -m pip install importlib_metadata || exit 1

mkdir -p "${WORK_ROOT}" || exit 1
mkdir -p "${WORK_ROOT}/chromium_git" || exit 1

