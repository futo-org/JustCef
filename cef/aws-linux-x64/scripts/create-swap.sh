#!/bin/bash
set -euo pipefail

SWAPSIZE_GIB=32
SWAPSIZE_BYTES=$((SWAPSIZE_GIB * 1024 * 1024 * 1024))
SWAPFILE="/swapfile"
FSTAB_ENTRY="$SWAPFILE none swap sw 0 0"

active_swapfile() {
  swapon --noheadings --show=NAME | grep -Fxq "$SWAPFILE"
}

current_size=0
if [ -f "$SWAPFILE" ]; then
  current_size=$(stat -c%s "$SWAPFILE")
fi

if [ -f "$SWAPFILE" ] && [ "$current_size" -lt "$SWAPSIZE_BYTES" ]; then
  if active_swapfile; then
    swapoff "$SWAPFILE"
  fi
  rm -f "$SWAPFILE"
fi

if [ ! -f "$SWAPFILE" ]; then
  if ! fallocate -l "${SWAPSIZE_GIB}G" "$SWAPFILE"; then
    dd if=/dev/zero of="$SWAPFILE" bs=1M count=$((SWAPSIZE_GIB * 1024)) status=progress
  fi
  chmod 600 "$SWAPFILE"
  mkswap "$SWAPFILE"
fi

if ! active_swapfile; then
  swapon "$SWAPFILE"
fi

if ! grep -Fqx "$FSTAB_ENTRY" /etc/fstab; then
  echo "$FSTAB_ENTRY" >> /etc/fstab
fi
