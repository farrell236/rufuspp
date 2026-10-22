#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(id -u)" -eq 0 ]; then
  echo 'Run without sudo. Only the read-only USB capture asks for elevation.' >&2
  exit 1
fi
if [ ! -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
  sh "$test_root/tests/install_qemu.sh"
fi
mkdir -p "$test_root/build/qemu-boot"
chmod 700 "$test_root/build/qemu-boot"
if [ ! -f "$test_root/build/qemu-boot/usb-readback.img" ]; then
  sudo /usr/bin/python3 -B "$test_root/tests/capture_usb_for_qemu.py"
fi
exec /usr/bin/python3 -B "$test_root/tests/qemu_usb_boot.py" --run
