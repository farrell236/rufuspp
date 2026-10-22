#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent capture-container decoding; ordinary private files, no devices/VMs."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def command(args):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(result.stderr or result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--qemu-img", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-capture-interop-") as temporary:
        directory = Path(temporary) / "captures"
        command([args.helper, "--capture-fixtures", directory])
        baseline = directory / "captured.img"
        for filename, fmt in (("captured-fixed.vhd", "vpc"),
                              ("captured-dynamic.vhd", "vpc"),
                              ("captured-dynamic.vhdx", "vhdx")):
            capture = directory / filename
            info = json.loads(command([args.qemu_img, "info", "--output=json", "-f", fmt, capture]))
            assert info["virtual-size"] == baseline.stat().st_size
            command([args.qemu_img, "compare", "-f", "raw", "-F", fmt, baseline, capture])
        vhdx = directory / "captured-dynamic.vhdx"
        with vhdx.open("rb") as stream:
            for offset in (65536, 131072):
                stream.seek(offset + 68)
                length, start = struct.unpack("<IQ", stream.read(12))
                assert length >= 1024**2 and length % 1024**2 == 0
                assert start >= 1024**2 and start % 1024**2 == 0
                assert start + length <= vhdx.stat().st_size
                stream.seek(start)
                assert stream.read(length) == bytes(length), "new capture log must be empty"
        command([args.qemu_img, "check", "-f", "vhdx", vhdx])
        print("PASS: QEMU independently decoded all capture formats byte-for-byte; VHDX log checks passed")


if __name__ == "__main__":
    main()
