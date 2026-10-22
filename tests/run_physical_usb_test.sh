#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned to the disposable UNTITLED USB explicitly authorized in this session.
# The production backend performs all writes; no dd or raw transport bypass.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
action=--erase-authorized
report_name=physical-usb-write-report.json
if [ "$#" -eq 1 ] && [ "$1" = "--verify-only" ]; then
  action=--verify-only
  report_name=physical-usb-replug-report.json
elif [ "$#" -ne 0 ]; then
  echo 'Only --verify-only is accepted; no device argument can be supplied.' >&2
  exit 1
fi
exec /usr/bin/python3 -B "$test_root/tests/physical_usb_integration.py" \
  "$action" \
  --helper "$test_root/build/native-root/tests/rufus_plus_plus_physical_usb_helper" \
  --iso "$test_root/build/ubuntu-iso/ubuntu-24.04.5-live-server-arm64.iso" \
  --sha256 f12484464c7d4d73e2167e0a7dbda1e669f8fae9bcf778dc0944937034b320cb \
  --target-record "$test_root/build/ubuntu-iso/physical-usb-target.json" \
  --report "$test_root/build/ubuntu-iso/$report_name"
