#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture only the already-qualified USB, with a read-only device handle.

No device argument is accepted. The local authorized target and successful
replug report must agree before any device is opened. Only a new local ordinary
file is written; the physical USB is never opened for writing.
"""
import hashlib
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

from image_write_integration import digest, require
from physical_usb_integration import command, discover, gap_samples, open_readonly, read_exact, MIB

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "build" / "ubuntu-iso"
DESTINATION = ROOT / "build" / "qemu-boot"


def copy_readonly(fd, output, capacity, image_size):
    disk_hash, image_hash = hashlib.sha256(), hashlib.sha256()
    for offset in range(0, capacity, MIB):
        data = read_exact(fd, offset, min(MIB, capacity - offset))
        require(output.write(data) == len(data), "short local capture-file write")
        disk_hash.update(data)
        if offset < image_size:
            image_hash.update(data[:min(len(data), image_size - offset)])
        if offset % (256 * MIB) == 0:
            print("Read-only USB capture: {}/{} bytes".format(offset + len(data), capacity), flush=True)
    return disk_hash.hexdigest(), image_hash.hexdigest()


def main():
    partial = DESTINATION / "usb-readback.img.partial"
    final = DESTINATION / "usb-readback.img"
    created = False
    try:
        require(sys.platform == "darwin" and os.geteuid() == 0,
                "run this read-only capture with sudo from the normal Terminal")
        target = json.loads((DATA / "physical-usb-target.json").read_text())
        previous = json.loads((DATA / "physical-usb-replug-report.json").read_text())
        require(previous.get("success") is True and previous.get("writeAttempted") is False,
                "a successful read-only physical reconnect check is required first")
        expected = previous["expectedTarget"]
        require(all(target.get(key) == value for key, value in expected.items()),
                "authorized target does not match the qualified USB")
        require(not final.exists() and not final.is_symlink(),
                "capture already exists; use the launcher or explicitly remove the old local capture")
        DESTINATION.mkdir(mode=0o700, parents=True, exist_ok=True)
        args = SimpleNamespace(helper=ROOT / "build/native-root/tests/rufus_plus_plus_physical_usb_helper",
                               serial=expected["serial"], mount=target["mount"])
        device = discover(args, expected, by_serial=True)
        command(["/usr/sbin/diskutil", "unmountDisk", device["devicePath"]])
        device = discover(args, expected, by_serial=True)
        fd = open_readonly(device)
        try:
            local = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            created = True
            with os.fdopen(local, "wb") as output:
                disk_sha, image_sha = copy_readonly(fd, output, expected["capacityBytes"], previous["sourceBytes"])
                require(image_sha == previous["sourceSha256"], "captured Ubuntu image checksum mismatch")
                require(read_exact(fd, expected["capacityBytes"] - MIB, MIB) == bytes(MIB),
                        "USB tail no longer matches the qualified write")
                require(gap_samples(fd, previous["sourceBytes"], expected["capacityBytes"]) ==
                        previous["readback"]["gapSamples"], "USB gap controls changed")
                output.flush()
                os.fsync(output.fileno())
                uid, gid = os.environ.get("SUDO_UID", ""), os.environ.get("SUDO_GID", "")
                require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0,
                        "normal sudo caller identity required for capture ownership")
                os.fchown(output.fileno(), int(uid), int(gid))
                os.fchmod(output.fileno(), 0o400)
        finally:
            os.close(fd)
        require(partial.stat().st_size == expected["capacityBytes"] and digest(partial) == disk_sha,
                "local whole-disk capture did not match the read data")
        # Atomic no-overwrite publication, even if another local capture appears.
        os.link(partial, final)
        partial.unlink()
        created = False
        device = discover(args, expected, by_serial=True)
        command(["/usr/sbin/diskutil", "eject", device["devicePath"]])
        report = {"success": True, "physicalUsbOpenedReadOnly": True, "ejected": True,
                  "expectedTarget": expected, "capturedBytes": expected["capacityBytes"],
                  "imageBytes": previous["sourceBytes"], "imageSha256": image_sha,
                  "wholeDiskSha256": disk_sha, "capture": str(final)}
        record = DESTINATION / "capture-report.json"
        record.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        os.chown(record, int(uid), int(gid))
        print("PASS read-only physical USB capture; Ubuntu SHA-256: " + image_sha, flush=True)
        print("USB safely ejected. QEMU will only use: " + str(final), flush=True)
        return 0
    except Exception as error:
        print("Capture refused/failed: " + str(error), file=sys.stderr, flush=True)
        return 1
    finally:
        if created:
            partial.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
