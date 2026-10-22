#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run in the user's normal Terminal; Homebrew retains its normal sandbox.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(id -u)" -eq 0 ]; then
  echo 'Run this installer without sudo. Homebrew installs as your normal user.' >&2
  exit 1
fi
mkdir -p "$test_root/build/qemu-tools/cache" "$test_root/build/qemu-tools/logs" "$test_root/build/qemu-tools/tmp"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_CACHE="$test_root/build/qemu-tools/cache"
export HOMEBREW_LOGS="$test_root/build/qemu-tools/logs"
export HOMEBREW_TEMP="$test_root/build/qemu-tools/tmp"
exec /opt/homebrew/bin/brew install qemu
