#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

"${SCRIPT_DIR}/install-dependencies.sh"
"${SCRIPT_DIR}/clone-repos.sh"

