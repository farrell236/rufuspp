#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Opt-in local fixture; never accepts a physical device or downloads anything.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec /usr/bin/python3 -B "$test_root/tests/ubuntu_iso_integration.py" \
  --iso "$test_root/build/ubuntu-iso/ubuntu-24.04.5-live-server-arm64.iso" \
  --sha256 f12484464c7d4d73e2167e0a7dbda1e669f8fae9bcf778dc0944937034b320cb \
  --helper "$test_root/build/native/tests/rufus_plus_plus_virtual_disk_helper" \
  --report "$test_root/build/ubuntu-iso/raw-virtual-write-report.json"
