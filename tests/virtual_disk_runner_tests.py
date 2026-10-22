#!/usr/bin/env python3
"""Read-only tests of the virtual-device runner's ownership checks."""
from pathlib import Path
import shutil
import plistlib
import json
import tempfile
import contextlib
import io
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import virtual_disk_integration as runner
from virtual_disk_integration import CAPACITY, whole_macos_device, check_windows_image
import ubuntu_iso_integration as iso_runner


class OwnershipTests(unittest.TestCase):
    backing = Path("/unit-test-only/rufus-owned.img")

    def mac(self, device="/dev/disk77", path=None):
        attachment = {"system-entities": [{"dev-entry": device}]}
        images = {"images": [{"image-path": str(path or self.backing),
                              "system-entities": [{"dev-entry": device}]}]}
        return attachment, images

    def test_mac_requires_exact_backing(self):
        attachment, images = self.mac(path=Path("/unit-test-only/someone-elses.img"))
        with self.assertRaises(RuntimeError):
            whole_macos_device(attachment, images, self.backing)

    def test_mac_refuses_partition_or_arbitrary_path(self):
        for path in ("/dev/disk77s1", "/dev/rdisk77", "/tmp/disk77", "/dev/disk", "/dev/disk77/../disk0"):
            with self.subTest(path=path):
                attachment, images = self.mac(path)
                with self.assertRaises(RuntimeError):
                    whole_macos_device(attachment, images, self.backing)

    def test_mac_requires_one_new_whole_disk(self):
        attachment, images = self.mac()
        attachment["system-entities"].append({"dev-entry": "/dev/disk0"})
        with self.assertRaises(RuntimeError):
            whole_macos_device(attachment, images, self.backing)

    def test_mac_refuses_mismatched_device_mapping(self):
        attachment, images = self.mac()
        images["images"][0]["system-entities"] = [{"dev-entry": "/dev/disk0"}]
        with self.assertRaises(RuntimeError):
            whole_macos_device(attachment, images, self.backing)

    def test_mac_accepts_owned_mapping(self):
        attachment, images = self.mac()
        self.assertEqual(whole_macos_device(attachment, images, self.backing), "/dev/disk77")

    def mac_disk_info(self):
        # diskutil info -plist uses WholeDisk/TotalSize, not Whole/DiskSize.
        return {"DeviceNode": "/dev/disk77", "WholeDisk": True,
                "VirtualOrPhysical": "Virtual", "TotalSize": CAPACITY,
                "DeviceBlockSize": 512}

    def check_mac_info(self, info, capacity=CAPACITY):
        attachment, images = self.mac()
        image = runner.OwnedImage(self.backing.parent, capacity)
        image.backing = self.backing
        image.attachment = attachment
        with patch.object(runner.sys, "platform", "darwin"), \
                patch.object(runner, "run", side_effect=[plistlib.dumps(images), plistlib.dumps(info)]):
            return image.check_owned()

    def test_mac_accepts_diskutil_whole_disk_geometry(self):
        self.assertEqual(self.check_mac_info(self.mac_disk_info()), "/dev/disk77")

    def test_mac_rejects_each_missing_or_changed_diskutil_identity_field(self):
        for key, bad in (("DeviceNode", "/dev/disk0"), ("WholeDisk", False),
                         ("VirtualOrPhysical", "Physical"), ("TotalSize", CAPACITY + 512),
                         ("DeviceBlockSize", 4096)):
            for missing in (False, True):
                with self.subTest(key=key, missing=missing):
                    info = self.mac_disk_info()
                    if missing:
                        del info[key]
                    else:
                        info[key] = bad
                    with self.assertRaises(RuntimeError):
                        self.check_mac_info(info)

    def test_mac_requires_the_exact_larger_iso_capacity(self):
        info = self.mac_disk_info()
        capacity = 4 * 1024 * 1024 * 1024
        info["TotalSize"] = capacity
        self.assertEqual(self.check_mac_info(info, capacity), "/dev/disk77")
        with self.assertRaises(RuntimeError):
            self.check_mac_info(info)

    def windows(self):
        return {"ImagePath": str(self.backing), "Number": 77, "Size": CAPACITY,
                "BusType": 15, "IsBoot": False, "IsSystem": False}

    def test_windows_accepts_only_file_backed_non_system_disk(self):
        self.assertEqual(check_windows_image(self.windows(), self.backing), r"\\.\PhysicalDrive77")

    def test_windows_rejects_each_missing_or_changed_identity_field(self):
        for key, bad in (("ImagePath", "/unit-test-only/foreign.vhd"), ("BusType", 7),
                         ("IsBoot", True), ("IsSystem", True), ("Size", CAPACITY + 512),
                         ("Number", -1), ("Number", "0"), ("Number", True)):
            for missing in (False, True):
                with self.subTest(key=key, bad=bad, missing=missing):
                    info = self.windows()
                    if missing:
                        del info[key]
                    else:
                        info[key] = bad
                    with self.assertRaises(RuntimeError):
                        check_windows_image(info, self.backing)

    def test_windows_requires_the_exact_larger_iso_capacity(self):
        capacity = 4 * 1024 * 1024 * 1024
        info = self.windows()
        info["Size"] = capacity
        self.assertEqual(check_windows_image(info, self.backing, capacity), r"\\.\PhysicalDrive77")
        with self.assertRaises(RuntimeError):
            check_windows_image(info, self.backing)


class IsoSafetyTests(unittest.TestCase):
    def test_rejects_unbounded_or_unaligned_virtual_capacity(self):
        for capacity in (0, -1, True, "16777216", CAPACITY - 512,
                         CAPACITY + 512, runner.MAX_CAPACITY + 1024 * 1024):
            with self.subTest(capacity=capacity), self.assertRaises(ValueError):
                runner.OwnedImage(Path("/unit-only"), capacity)

    def test_iso_size_must_be_positive_and_sector_aligned(self):
        for size in (0, -512, 511, True, "512"):
            with self.subTest(size=size), self.assertRaises(AssertionError):
                iso_runner.image_capacity(size)

    def test_iso_larger_than_four_gib_has_bounded_aligned_trailing_gap(self):
        size = 4 * 1024 * 1024 * 1024 + 2048
        capacity = iso_runner.image_capacity(size)
        self.assertEqual(capacity % (1024 * 1024), 0)
        self.assertGreaterEqual(capacity - size, 16 * 1024 * 1024)
        self.assertLessEqual(capacity, runner.MAX_CAPACITY)
        with self.assertRaises(ValueError):
            iso_runner.image_capacity(runner.MAX_CAPACITY)

    def test_checksum_mismatch_refuses_before_target_allocation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.iso"
            source.write_bytes(bytes(512))
            args = SimpleNamespace(iso=source, sha256="0" * 64)
            with patch.object(iso_runner.tempfile, "mkdtemp") as allocate:
                with self.assertRaisesRegex(AssertionError, "SHA-256"):
                    iso_runner.exercise(args)
            allocate.assert_not_called()

    def test_corrupt_iso_descriptor_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "corrupt.iso"
            source.write_bytes(bytes(18 * 2048))
            with self.assertRaisesRegex(AssertionError, "descriptor"):
                iso_runner.iso_structure(source)


class LifecycleTests(unittest.TestCase):
    def exercise(self, fail=None):
        calls = []
        directories = []

        class Image:
            def __init__(self, directory):
                directories.append(directory)
                self.backing = directory / "target.img"

            def create(self):
                calls.append("create")

            def attach(self, readonly=False):
                calls.append("attach-read" if readonly else "attach-write")
                if fail == "attach":
                    raise runner.Unavailable("no virtual-disk service")

            def invoke(self, helper, source, mode, verify=False):
                calls.append("verify" if verify else "write")
                if fail == "write" and not verify:
                    raise RuntimeError("write failure")
                if fail == "verify" and verify:
                    raise runner.Unavailable("read handle unavailable")

            def detach(self):
                calls.append("detach")
                if fail == "cleanup":
                    raise RuntimeError("detach failure")

            def filesystem_check(self):
                calls.append("fsck")

        error = None
        with patch.object(runner, "OwnedImage", Image), patch.object(runner, "run", return_value=b""), \
                patch.object(runner.shutil, "which", return_value="test-only-fsck"), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            try:
                runner.exercise(Path("not-executed-helper"), "quick")
            except RuntimeError as caught:
                error = caught
        return calls, directories[0], error

    def test_close_detach_and_readonly_reattach_order(self):
        calls, directory, error = self.exercise()
        self.assertIsNone(error)
        self.assertEqual(calls, ["create", "attach-write", "write", "detach", "attach-read", "verify", "fsck", "detach"])
        self.assertFalse(directory.exists())

    def test_write_failure_stops_and_cleans_up(self):
        calls, directory, error = self.exercise("write")
        self.assertIsInstance(error, RuntimeError)
        self.assertEqual(calls, ["create", "attach-write", "write", "detach"])
        self.assertFalse(directory.exists())

    def test_unavailable_attach_is_skippable_before_writes(self):
        calls, directory, error = self.exercise("attach")
        self.assertIsInstance(error, runner.Unavailable)
        self.assertNotIn("write", calls)
        self.assertFalse(directory.exists())

    def test_unavailable_readback_after_write_is_failure_not_skip(self):
        calls, directory, error = self.exercise("verify")
        self.assertIsInstance(error, RuntimeError)
        self.assertNotIsInstance(error, runner.Unavailable)
        self.assertEqual(calls[-1], "detach")
        self.assertFalse(directory.exists())

    def test_failed_detach_preserves_backing_for_recovery(self):
        calls, directory, error = self.exercise("cleanup")
        try:
            self.assertIsInstance(error, RuntimeError)
            self.assertTrue(directory.exists())
            self.assertNotIn("verify", calls)
        finally:
            shutil.rmtree(directory)


class CleanupProofTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="rufus-cleanup-unit-")
        self.addCleanup(self.scratch.cleanup)
        self.image = runner.OwnedImage(Path(self.scratch.name))
        self.image.attach_attempted = True

    def mac_info(self, device="/dev/disk77"):
        return {"images": [{"image-path": str(self.image.backing),
                             "system-entities": [{"dev-entry": device}]}]}

    def test_failed_attach_still_recovers_and_detaches_owned_mac_device(self):
        responses = [plistlib.dumps(self.mac_info()), b"", plistlib.dumps({"images": []})]
        with patch.object(runner.sys, "platform", "darwin"), \
                patch.object(runner, "run", side_effect=responses) as commands:
            self.image.detach()
        self.assertEqual(commands.call_args_list[1].args[0], ["hdiutil", "detach", "/dev/disk77"])
        self.assertFalse(self.image.attach_attempted)

    def test_mac_cleanup_ignores_foreign_devices(self):
        foreign = {"images": [{"image-path": "/unit-only/foreign.img",
                                "system-entities": [{"dev-entry": "/dev/disk0"}]}]}
        with patch.object(runner.sys, "platform", "darwin"), \
                patch.object(runner, "run", return_value=plistlib.dumps(foreign)) as commands:
            self.image.detach()
        self.assertTrue(all(call.args[0] == ["hdiutil", "info", "-plist"]
                            for call in commands.call_args_list))

    def test_mac_still_attached_is_not_successful_cleanup(self):
        info = plistlib.dumps(self.mac_info())
        with patch.object(runner.sys, "platform", "darwin"), \
                patch.object(runner, "run", side_effect=[info, b"", info]):
            with self.assertRaises(RuntimeError):
                self.image.detach()
        self.assertTrue(self.image.attach_attempted)

    def test_linux_cleanup_recovers_loop_after_failed_attach(self):
        info = {"loopdevices": [{"name": "/dev/loop77", "back-file": str(self.image.backing)}]}
        with patch.object(runner.sys, "platform", "linux"), \
                patch.object(runner, "run", side_effect=[json.dumps(info).encode(), b"", b'{"loopdevices": []}']) as commands:
            self.image.detach()
        self.assertEqual(commands.call_args_list[1].args[0], ["losetup", "--detach", "/dev/loop77"])
        self.assertFalse(self.image.attach_attempted)

    def test_linux_never_detaches_forged_non_loop_path(self):
        info = {"loopdevices": [{"name": "/dev/sda", "back-file": str(self.image.backing)}]}
        with patch.object(runner.sys, "platform", "linux"), \
                patch.object(runner, "run", return_value=json.dumps(info).encode()) as commands:
            with self.assertRaises(RuntimeError):
                self.image.detach()
        self.assertEqual(commands.call_count, 1)
        self.assertTrue(self.image.attach_attempted)

    def test_windows_cleanup_uses_owned_image_path_only(self):
        self.image.device = r"\\.\PhysicalDrive0"
        with patch.object(runner.sys, "platform", "win32"), \
                patch.object(runner, "powershell", return_value=b"") as command:
            self.image.detach()
        self.assertIn(runner.ps_string(self.image.backing), command.call_args.args[0])
        self.assertNotIn("PhysicalDrive", command.call_args.args[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
