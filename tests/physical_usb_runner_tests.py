#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-destructive guard tests; all data I/O uses private ordinary files."""
import contextlib
import hashlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import physical_usb_integration as runner


class IdentityTests(unittest.TestCase):
    def device(self):
        return {"serial": "test-serial", "stableId": "test-stable", "capacityBytes": 16 * 1024**3,
                "sectorBytes": 512, "vendor": "Test", "model": "Fixture",
                "bus": "USB", "wholeDevice": True, "systemDevice": False, "writable": True,
                "devicePath": "/dev/disk77"}

    def test_exact_identity_is_accepted(self):
        runner.validate_identity(self.device(), self.device())

    def test_replacement_or_missing_identity_is_refused(self):
        for field in ("serial", "stableId", "capacityBytes", "sectorBytes", "vendor", "model"):
            for missing in (True, False):
                with self.subTest(field=field, missing=missing):
                    device = self.device()
                    if missing:
                        del device[field]
                    else:
                        device[field] = "replacement"
                    with self.assertRaises(AssertionError):
                        runner.validate_identity(device, self.device())

    def test_system_partition_fixed_or_readonly_targets_are_refused(self):
        for field, value in (("bus", "NVMe"), ("wholeDevice", False),
                             ("systemDevice", True), ("writable", False)):
            with self.subTest(field=field):
                device = self.device()
                device[field] = value
                with self.assertRaises(AssertionError):
                    runner.validate_identity(device, self.device())

    def test_mount_partition_and_arbitrary_paths_are_refused(self):
        for path in ("/dev/disk77s1", "/dev/rdisk77", "/dev/disk77/../disk0", "/Volumes/UNTITLED"):
            with self.subTest(path=path):
                device = self.device()
                device["devicePath"] = path
                with self.assertRaises(AssertionError):
                    runner.validate_identity(device, self.device())

    def diskutil(self):
        return {"WholeDisk": True, "Internal": False, "BusProtocol": "USB",
                "DeviceNode": "/dev/disk77", "TotalSize": 16 * 1024**3, "DeviceBlockSize": 512}

    def test_independent_diskutil_identity_is_required(self):
        runner.validate_diskutil(self.diskutil(), self.device())
        for field in self.diskutil():
            with self.subTest(field=field):
                info = self.diskutil()
                del info[field]
                with self.assertRaises(AssertionError):
                    runner.validate_diskutil(info, self.device())

    def test_raw_readback_refuses_regular_file_before_open(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "file"
            path.write_bytes(b"fixture")
            with patch.object(runner.os, "open") as opened:
                with self.assertRaises(AssertionError):
                    runner.open_readonly({"devicePath": str(path)})
            opened.assert_not_called()


class ReadbackTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        directory = Path(self.scratch.name)
        self.source = directory / "source.iso"
        self.payload = bytes(range(256)) * 2
        self.source.write_bytes(self.payload)
        self.expected = hashlib.sha256(self.payload).hexdigest()
        self.capacity = 8 * runner.MIB
        self.target = directory / "target.img"
        with self.target.open("xb") as output:
            for _ in range(8):
                output.write(b"\xa5" * runner.MIB)
        with self.target.open("r+b") as output:
            output.write(self.payload)
            output.seek(self.capacity - runner.MIB)
            output.write(bytes(runner.MIB))
        self.fd = os.open(self.target, os.O_RDONLY)
        self.addCleanup(os.close, self.fd)
        self.before = runner.gap_samples(self.fd, len(self.payload), self.capacity)

    def verify(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return runner.verify_usb(self.fd, self.source, self.expected, self.capacity, self.before)

    def test_complete_image_tail_and_gap_controls_pass(self):
        result = self.verify()
        self.assertEqual(result["imageSha256"], self.expected)
        self.assertEqual(result["imageBytesCompared"], len(self.payload))
        self.assertTrue(result["gapControlsUnchanged"])

    def test_one_corrupt_image_byte_is_detected(self):
        with self.target.open("r+b") as output:
            output.seek(123)
            output.write(b"\xff")
        with self.assertRaisesRegex(AssertionError, "differs from ISO"):
            self.verify()

    def test_uncleared_tail_is_detected(self):
        with self.target.open("r+b") as output:
            output.seek(self.capacity - 1)
            output.write(b"\xa5")
        with self.assertRaisesRegex(AssertionError, "trailing metadata"):
            self.verify()

    def test_modified_gap_control_is_detected(self):
        with self.target.open("r+b") as output:
            output.seek(self.before[1]["offset"])
            output.write(b"\xff")
        with self.assertRaisesRegex(AssertionError, "gap control"):
            self.verify()

    def test_partial_reads_are_completed(self):
        actual_pread = os.pread
        with patch.object(runner.os, "pread", side_effect=lambda fd, count, offset:
                          actual_pread(fd, min(count, 7), offset)):
            self.assertEqual(runner.read_exact(self.fd, 0, len(self.payload)), self.payload)

    def test_truncated_readback_is_detected(self):
        with self.assertRaisesRegex(AssertionError, "unexpected end"):
            runner.read_exact(self.fd, self.capacity, 512)


if __name__ == "__main__":
    unittest.main(verbosity=2)
