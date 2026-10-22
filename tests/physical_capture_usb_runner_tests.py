#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture-runner guards. No disk devices, mounts or privileged I/O are used."""
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch, Mock

import test_physical_capture_usb as runner
from fat32_fixture_readback import component, catalog


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.folder = Path(self.scratch.name)
        (self.folder / "empty").mkdir()
        (self.folder / "café-日本語-😀.txt").write_bytes(b"example")
        self.manifest = {"files": [{"path": "café-日本語-😀.txt", "size": 7,
                                     "sha256": hashlib.sha256(b"example").hexdigest()}],
                         "directories": ["empty"], "fileCount": 1, "totalFileBytes": 7}

    def test_unicode_file_and_empty_directory(self):
        self.assertTrue(runner.verify_fixture(self.folder, self.manifest)["allSha256Matched"])

    def test_one_changed_byte_is_detected(self):
        (self.folder / self.manifest["files"][0]["path"]).write_bytes(b"examplE")
        with self.assertRaisesRegex(AssertionError, "content mismatch"):
            runner.verify_fixture(self.folder, self.manifest)

    def test_missing_directory_is_detected(self):
        (self.folder / "empty").rmdir()
        with self.assertRaisesRegex(AssertionError, "missing fixture directory"):
            runner.verify_fixture(self.folder, self.manifest)

    def test_symlinks_and_manifest_traversal_are_refused(self):
        (self.folder / "link").symlink_to(self.folder, target_is_directory=True)
        for name in ("../outside", "/absolute", "link/inside", ""):
            with self.subTest(name=name), self.assertRaises(AssertionError):
                runner.checked_path(self.folder, name)

    def test_wrong_totals_are_detected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["totalFileBytes"] += 1
        with self.assertRaisesRegex(AssertionError, "totals"):
            runner.verify_fixture(self.folder, manifest)


class ByteComparisonTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.baseline = Path(self.scratch.name) / "baseline"
        self.observed = Path(self.scratch.name) / "readback"
        self.data = bytes(range(256)) * 2
        self.baseline.write_bytes(self.data)
        self.observed.write_bytes(self.data)

    def compare(self):
        fd = os.open(self.observed, os.O_RDONLY)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                return runner.compare_full_disk(fd, self.baseline, len(self.data))
        finally:
            os.close(fd)

    def test_complete_disk_hash(self):
        self.assertEqual(self.compare()["sha256"], hashlib.sha256(self.data).hexdigest())

    def test_last_byte_corruption_is_detected(self):
        self.observed.write_bytes(self.data[:-1] + b"x")
        with self.assertRaisesRegex(AssertionError, "offset 511"):
            self.compare()

    def test_short_usb_read_is_detected(self):
        self.observed.write_bytes(self.data[:-1])
        with self.assertRaisesRegex(AssertionError, "unexpected end"):
            self.compare()

    def test_wrong_baseline_size_is_detected(self):
        self.baseline.write_bytes(self.data[:-1])
        with self.assertRaisesRegex(AssertionError, "baseline size"):
            self.compare()

    def test_unmounted_readback_never_requests_another_unmount(self):
        device = {"mountPoints": []}
        with patch.object(runner, "discover", return_value=device), \
                patch.object(runner, "open_readonly", side_effect=lambda _: os.open(self.observed, os.O_RDONLY)), \
                patch.object(runner, "command", side_effect=AssertionError("unexpected diskutil mutation")), \
                contextlib.redirect_stdout(io.StringIO()):
            result = runner.readback(Path("helper"), {"capacityBytes": len(self.data)}, self.baseline)
        self.assertEqual(result["bytesCompared"], len(self.data))

    def test_mounted_target_is_refused_before_raw_open(self):
        with patch.object(runner, "discover", return_value={"mountPoints": ["/Volumes/test"]}), \
                patch.object(runner, "open_readonly") as opened, \
                self.assertRaisesRegex(AssertionError, "unexpectedly mounted"):
            runner.readback(Path("helper"), {}, self.baseline)
        opened.assert_not_called()


class MountTests(unittest.TestCase):
    def mount(self, readonly, writable):
        device = {"devicePath": "/dev/disk77", "mountPoints": ["/Volumes/RUFUS_TEST"]}
        info = {"ParentWholeDisk": "disk77", "FilesystemType": "msdos", "VolumeName": "RUFUS_TEST",
                "WritableVolume": writable, "MountPoint": "/Volumes/RUFUS_TEST"}
        def command(args):
            return plistlib.dumps(info) if "info" in args else b""
        with patch.object(runner, "discover", return_value=device), \
                patch.object(runner, "command", side_effect=command) as called, \
                patch.object(runner.os.path, "ismount", return_value=True), \
                patch.object(Path, "is_symlink", return_value=False):
            result = runner.checked_mount(Path("helper"), {}, readonly)
            return result, called.call_args_list

    def test_readonly_mount_is_explicit_and_verified(self):
        _, calls = self.mount(True, False)
        self.assertIn(["/usr/sbin/diskutil", "mount", "readOnly", "/dev/disk77s1"],
                      [call.args[0] for call in calls])

    def test_readonly_verification_refuses_writable_mount(self):
        with self.assertRaisesRegex(AssertionError, "read-only status"):
            self.mount(True, True)

    def test_initial_population_requires_writable_mount(self):
        with self.assertRaisesRegex(AssertionError, "read-only status"):
            self.mount(False, False)


class AuthorizationTests(unittest.TestCase):
    def test_default_preflight_never_constructs_write_runner(self):
        expected = {"serial": runner.SERIAL, "stableId": "pinned", "vendor": "Kingston",
                    "model": "DataTraveler 3.0", "capacityBytes": 15472047104, "sectorBytes": 512}
        fixture = {"payload": "/fixture/payload", "manifest": "/fixture/manifest"}
        records = iter([json.dumps({"expectedTarget": expected}), json.dumps(fixture), "{}"])
        with patch.object(Path, "read_text", side_effect=lambda: next(records)), \
                patch.object(Path, "resolve", return_value=Path("/helper")), \
                patch.object(Path, "is_symlink", return_value=False), \
                patch.object(runner, "discover", return_value={}), \
                patch.object(runner, "verify_fixture", return_value={}), \
                patch.object(runner, "outside_usb"), \
                patch.object(runner.shutil, "which", return_value="/qemu-img"), \
                patch.object(runner, "command", return_value=b"qemu version"), \
                patch.object(runner, "Run") as run, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runner.main([]), 0)
            run.assert_not_called()

    def test_resume_requires_explicit_authorization_before_discovery(self):
        with patch.object(runner, "discover") as discovered, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.main(["--resume", "/unused"]), 1)
            discovered.assert_not_called()

    def test_readonly_action_requires_saved_run_before_discovery(self):
        with patch.object(runner, "discover") as discovered, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.main(["--verify-restored-usb"]), 1)
            discovered.assert_not_called()

    def test_readonly_and_erase_actions_are_mutually_exclusive(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exit_status:
            runner.main(["--verify-restored-usb", "--erase-authorized-kingston"])
        self.assertEqual(exit_status.exception.code, 2)


class RestoreContinuationTests(unittest.TestCase):
    CAPACITY = 15472047104

    @classmethod
    def entry(cls):
        return {"captureVerified": True, "fixtureAbsentBeforeRestore": True,
                "restore": {"success": True, "cancelled": False, "verificationCompleted": True,
                            "bytesWritten": cls.CAPACITY, "bytesVerified": cls.CAPACITY}}

    def test_partial_or_unverified_restore_cannot_skip_write_control(self):
        entry = self.entry()
        self.assertTrue(runner.completed_native_restore(entry, self.CAPACITY))
        for field in ("captureVerified", "fixtureAbsentBeforeRestore"):
            changed = copy.deepcopy(entry)
            changed[field] = False
            self.assertFalse(runner.completed_native_restore(changed, self.CAPACITY))
        for field, value in (("success", False), ("cancelled", True), ("verificationCompleted", False),
                             ("bytesWritten", self.CAPACITY - 512), ("bytesVerified", self.CAPACITY - 512)):
            changed = copy.deepcopy(entry)
            changed["restore"][field] = value
            self.assertFalse(runner.completed_native_restore(changed, self.CAPACITY), field)
            del changed["restore"][field]
            self.assertFalse(runner.completed_native_restore(changed, self.CAPACITY), field)

    def invoke_readonly(self, mismatch=False):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            directory = work / "run-local"
            directory.mkdir()
            helper = work / "helper"
            helper.write_bytes(b"placeholder; never executed")
            payload = work / "payload"
            payload.mkdir()
            (payload / "saved.txt").write_bytes(b"fixture")
            manifest = {"files": [{"path": "saved.txt", "size": 7,
                                    "sha256": hashlib.sha256(b"fixture").hexdigest()}],
                        "directories": [], "fileCount": 1, "totalFileBytes": 7}
            manifest_path = work / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            fixture_path = work / "fixture.json"
            fixture_path.write_text(json.dumps({"payload": str(payload), "manifest": str(manifest_path)}))
            expected = {"serial": runner.SERIAL, "stableId": "pinned", "vendor": "Kingston",
                        "model": "DataTraveler 3.0", "capacityBytes": self.CAPACITY, "sectorBytes": 512}
            target_path = work / "target.json"
            target_path.write_text(json.dumps({"expectedTarget": expected}))
            baseline = directory / "capture.dd"
            capture = directory / "capture-fixed.vhd"
            baseline.write_bytes(b"authenticated baseline")
            capture.write_bytes(b"authenticated container")
            entry = self.entry()
            entry["captureSha256"] = runner.sha256(capture)
            report = {"expectedTarget": expected, "manifestSha256": runner.sha256(manifest_path),
                      "baselineVerified": True, "baselineSha256": runner.sha256(baseline),
                      "roundtrips": {"raw": {"success": True}, "fixed-vhd": entry}, "success": False}
            (directory / "report.json").write_text(json.dumps(report))
            guard = Mock()
            device = {"devicePath": "/dev/disk77", "mountPoints": []}
            digest = "wrong" if mismatch else report["baselineSha256"]
            with patch.object(runner, "WORK", work), \
                    patch.object(runner.os, "geteuid", return_value=0), \
                    patch.dict(runner.os.environ, {"SUDO_UID": "501", "SUDO_GID": "20"}), \
                    patch.object(runner.os, "chown"), \
                    patch.object(runner, "discover", return_value=device), \
                    patch.object(runner, "MountGuard") as guard_class, \
                    patch.object(runner.shutil, "which", side_effect=AssertionError("unexpected qemu dependency")), \
                    patch.object(runner.shutil, "disk_usage", side_effect=AssertionError("unexpected capture space check")), \
                    patch.object(runner.Run, "blank", side_effect=AssertionError("unexpected format")) as blank, \
                    patch.object(runner.Run, "restore", side_effect=AssertionError("unexpected restore")) as restore, \
                    patch.object(runner.Run, "helper_action", side_effect=AssertionError("unexpected capture/write")) as action, \
                    patch.object(runner, "readback", return_value={"sha256": digest}), \
                    patch.object(runner, "raw_fixture", return_value={"allSha256Matched": True}) as files, \
                    patch.object(runner, "command", return_value=b"") as command, \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                guard_class.return_value.start.return_value = guard
                status = runner.main(["--verify-restored-usb", "--resume", str(directory), "--wait-for-usb",
                                      "--target-record", str(target_path), "--fixture-record", str(fixture_path),
                                      "--helper", str(helper)])
                blank.assert_not_called()
                restore.assert_not_called()
                action.assert_not_called()
                guard_class.return_value.start.assert_called_once_with(wait_for_usb=True)
                guard.stop.assert_called_once()
                self.assertTrue(list(directory.glob("previous-report-*.json")))
                return status, json.loads((directory / "report.json").read_text()), command.call_args_list, files.called

    def test_readonly_action_finishes_one_case_and_stops_before_next_capture(self):
        status, report, commands, files_checked = self.invoke_readonly()
        self.assertEqual(status, 0)
        self.assertTrue(report["roundtrips"]["fixed-vhd"]["success"])
        self.assertFalse(report["roundtrips"]["fixed-vhd"]["restoreRepeatedDuringContinuation"])
        self.assertFalse(report["success"])
        self.assertEqual(report["readOnlyCompletion"]["pendingFormats"], ["dynamic-vhd", "dynamic-vhdx"])
        self.assertTrue(files_checked)
        self.assertEqual([call.args[0] for call in commands], [["/usr/sbin/diskutil", "eject", "/dev/disk77"]])

    def test_readback_failure_is_not_reported_as_a_pass_or_rewritten(self):
        status, report, commands, files_checked = self.invoke_readonly(mismatch=True)
        self.assertEqual(status, 1)
        self.assertNotIn("success", report["roundtrips"]["fixed-vhd"])
        self.assertFalse(report["success"])
        self.assertIn("restored disk hash mismatch", report["error"])
        self.assertFalse(files_checked)
        self.assertFalse(commands)


class PendingCaptureResumeTests(unittest.TestCase):
    def invoke(self, readonly):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            directory = work / "run-pending"
            directory.mkdir()
            payload = work / "payload"
            payload.mkdir()
            (payload / "file.txt").write_bytes(b"saved")
            manifest = {"files": [{"path": "file.txt", "size": 5,
                                    "sha256": hashlib.sha256(b"saved").hexdigest()}],
                        "directories": [], "fileCount": 1, "totalFileBytes": 5}
            manifest_path = work / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            fixture_path = work / "fixture.json"
            fixture_path.write_text(json.dumps({"payload": str(payload), "manifest": str(manifest_path)}))
            capacity = RestoreContinuationTests.CAPACITY
            expected = {"serial": runner.SERIAL, "stableId": "pinned", "vendor": "Kingston",
                        "model": "DataTraveler 3.0", "capacityBytes": capacity, "sectorBytes": 512}
            target_path = work / "target.json"
            target_path.write_text(json.dumps({"expectedTarget": expected}))
            helper = work / "helper"
            helper.write_bytes(b"never executed")
            baseline = directory / "capture.dd"
            baseline.write_bytes(b"authenticated baseline")
            complete = {name: {"success": True, "savedEvidence": name}
                        for name in ("raw", "fixed-vhd")}
            report = {"expectedTarget": expected, "manifestSha256": runner.sha256(manifest_path),
                      "baselineVerified": True, "baselineSha256": runner.sha256(baseline),
                      "roundtrips": copy.deepcopy(complete), "success": False, "ejected": True}
            (directory / "report.json").write_text(json.dumps(report))
            operations = []
            guard = Mock()
            def capture_action(run, label, action, name, capture):
                self.assertEqual(action, "--capture-authorized")
                self.assertIn(name, ("dynamic-vhd", "dynamic-vhdx"))
                operations.append(("capture", name))
                capture.write_bytes(("local test container " + name).encode())
                return {"success": True, "cancelled": False, "bytesCaptured": capacity}
            def blank(run, label):
                operations.append(("blank", label))
                return {"verificationCompleted": True}
            def restore(run, name, capture):
                self.assertTrue(capture.is_file())
                operations.append(("restore", name))
                return RestoreContinuationTests.entry()["restore"]
            def command(args):
                if str(args[0]) == "/qemu-img" and "--version" in args:
                    return b"qemu independent decoder"
                if str(args[0]) == "/qemu-img" and "info" in args:
                    return json.dumps({"virtual-size": capacity}).encode()
                self.assertEqual(args, ["/usr/sbin/diskutil", "eject", "/dev/disk77"])
                return b""
            with patch.object(runner, "WORK", work), \
                    patch.object(runner.os, "geteuid", return_value=0), \
                    patch.dict(runner.os.environ, {"SUDO_UID": "501", "SUDO_GID": "20"}), \
                    patch.object(runner.os, "chown"), \
                    patch.object(runner, "discover", return_value={"devicePath": "/dev/disk77", "mountPoints": []}), \
                    patch.object(runner, "MountGuard") as guard_class, \
                    patch.object(runner.shutil, "which", return_value="/qemu-img"), \
                    patch.object(runner.shutil, "disk_usage", return_value=SimpleNamespace(free=1024**4)), \
                    patch.object(runner.Run, "helper_action", capture_action), \
                    patch.object(runner.Run, "blank", blank), \
                    patch.object(runner.Run, "restore", restore), \
                    patch.object(runner.Run, "tool", return_value="Images are identical.") as compared, \
                    patch.object(runner, "readback", return_value={"sha256": report["baselineSha256"]}) as readback, \
                    patch.object(runner, "raw_fixture", return_value={"allSha256Matched": True}), \
                    patch.object(runner, "command", side_effect=command), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                guard_class.return_value.start.return_value = guard
                action = "--verify-restored-usb" if readonly else "--erase-authorized-kingston"
                status = runner.main([action, "--resume", str(directory), "--wait-for-usb",
                                      "--target-record", str(target_path), "--fixture-record", str(fixture_path),
                                      "--helper", str(helper)])
                result = json.loads((directory / "report.json").read_text())
                self.assertEqual({n: result["roundtrips"][n] for n in complete}, complete)
                if readonly:
                    guard_class.assert_not_called()
                    readback.assert_not_called()
                    compared.assert_not_called()
                else:
                    guard_class.return_value.start.assert_called_once_with(wait_for_usb=True)
                    guard.stop.assert_called_once()
                    self.assertEqual(compared.call_count, 2)
                    self.assertEqual(readback.call_count, 3)
                return status, result, operations

    def test_authorized_reconnect_runs_only_remaining_formats(self):
        status, report, operations = self.invoke(readonly=False)
        self.assertEqual(status, 0)
        self.assertTrue(report["success"])
        self.assertTrue(report["ejected"])
        self.assertEqual([(kind, name) for kind, name in operations if kind != "blank"],
                         [("capture", "dynamic-vhd"), ("restore", "dynamic-vhd"),
                          ("capture", "dynamic-vhdx"), ("restore", "dynamic-vhdx")])
        self.assertEqual(sum(kind == "blank" for kind, _ in operations), 2)

    def test_readonly_resume_cannot_turn_pending_capture_into_a_write(self):
        status, report, operations = self.invoke(readonly=True)
        self.assertEqual(status, 1)
        self.assertFalse(report["success"])
        self.assertFalse(operations)


class MountGuardTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        expected = {"serial": "test", "stableId": "stable", "capacityBytes": 15472047104, "sectorBytes": 512}
        self.run = SimpleNamespace(directory=Path(self.scratch.name), helper=Path("/test/helper"),
                                   expected=expected, report={}, save=Mock())
        self.device = {"devicePath": "/dev/disk77", "mountPoints": []}
        self.process = Mock(pid=123)
        self.process.poll.return_value = None
        self.process.terminate.side_effect = lambda: setattr(self.process.poll, "return_value", 0)
        self.guard = runner.MountGuard(self.run)
        self.guard.events = Mock(return_value=[{"mountGuardReady": True}, {"mountDenied": True}])

    def launch(self, probe_status, probe_output=None, wait_for_usb=False, discovery_errors=()):
        if probe_output is None:
            probe_output = 'Volume failed to mount: "Rufus++ physical qualification is keeping this USB unmounted"'
        order = []
        errors = iter(discovery_errors)
        def discover(*_):
            order.append("discover")
            error = next(errors, None)
            if error is not None:
                raise error
            return self.device
        def launch(*_, **kwargs):
            order.append("guard")
            return self.process
        with patch.object(runner, "discover", side_effect=discover), \
                patch.object(runner.time, "sleep"), \
                patch.object(Path, "exists", return_value=True), \
                patch.object(runner.subprocess, "Popen", side_effect=launch) as launched, \
                patch.object(runner.subprocess, "run", return_value=SimpleNamespace(
                    returncode=probe_status, stdout=probe_output, stderr="")) as probe, \
                contextlib.redirect_stdout(io.StringIO()):
            result = self.guard.start(wait_for_usb=wait_for_usb)
            self.assertIn("--guard-host-mounts", launched.call_args.args[0])
            self.assertEqual(probe.call_args.args[0],
                             ["/usr/sbin/diskutil", "mount", "readOnly", "/dev/disk77s1"])
            if wait_for_usb:
                self.assertEqual(order[0], "guard")
            return result

    def test_guard_requires_confirmed_readonly_mount_denial(self):
        self.launch(1)
        self.assertTrue(self.guard.record["readOnlyProbeDenied"])
        self.guard.stop()
        self.assertFalse(self.guard.record["active"])
        self.process.terminate.assert_called_once()

    def test_unexpected_successful_mount_stops_guard_and_refuses_test(self):
        with self.assertRaisesRegex(AssertionError, "failed to block"):
            self.launch(0)
        self.process.terminate.assert_called_once()

    def test_missing_denial_callback_stops_guard(self):
        self.guard.wait_event = Mock(side_effect=[None, AssertionError("missing mountDenied")])
        with self.assertRaisesRegex(AssertionError, "missing mountDenied"):
            self.launch(1)
        self.process.terminate.assert_called_once()

    def test_unrelated_mount_failure_cannot_qualify_guard(self):
        with self.assertRaisesRegex(AssertionError, "other than the qualification guard"):
            self.launch(1, "No such device")
        self.process.terminate.assert_called_once()

    def test_reconnect_wait_arms_guard_before_looking_for_device(self):
        self.launch(1, wait_for_usb=True)
        self.assertTrue(self.guard.record["armedBeforeReconnect"])
        self.guard.stop()

    def test_reconnect_retries_device_absence_and_pending_diskutil_publication(self):
        self.launch(1, wait_for_usb=True, discovery_errors=[
            AssertionError("expected exactly one USB device matching the selected mount/serial"),
            AssertionError("/usr/sbin/diskutil failed: device not yet published")])
        self.assertIn("not yet published", self.guard.record["lastReconnectQueryError"])
        self.guard.stop()

    def test_reconnect_identity_failure_is_not_retried(self):
        with self.assertRaisesRegex(AssertionError, "USB identity changed"):
            self.launch(1, wait_for_usb=True, discovery_errors=[AssertionError("USB identity changed: serial")])
        self.process.terminate.assert_called_once()

    def test_exited_guard_is_not_accepted(self):
        self.process.poll.return_value = 1
        with self.assertRaisesRegex(AssertionError, "no longer active"):
            self.launch(1)

    def test_already_mounted_device_is_refused_before_launch(self):
        self.device["mountPoints"] = ["/Volumes/test"]
        with patch.object(runner.subprocess, "Popen") as launched, \
                patch.object(runner, "discover", return_value=self.device), \
                self.assertRaisesRegex(AssertionError, "unmount the USB"):
            self.guard.start()
        launched.assert_not_called()


class FatLongNameTests(unittest.TestCase):
    # Golden directory records from our own generated Unicode fixture, rather
    # than a name encoder shared with the decoder under test.
    ENTRY = bytes.fromhex("434146e92d5f7e325458542000bf8849565d565d00008849565d01033b000000")
    SLOTS = [bytes.fromhex(value) for value in (
        "42780074000000ffffffff0f0074ffffffffffffffffffffffff0000ffffffff",
        "01630061006600e9002d000f0074e5652c679e8a2d003dd800de00002e007400")]

    def test_unicode_name_including_surrogate_pair(self):
        self.assertEqual(component(self.ENTRY, self.SLOTS), "café-日本語-😀.txt")

    def test_corrupt_long_name_checksum_is_refused(self):
        slots = [bytearray(slot) for slot in self.SLOTS]
        slots[0][13] ^= 1
        with self.assertRaisesRegex(AssertionError, "checksum"):
            component(self.ENTRY, slots)

    def test_reversed_long_name_sequence_is_refused(self):
        with self.assertRaisesRegex(AssertionError, "sequence"):
            component(self.ENTRY, list(reversed(self.SLOTS)))

    def test_short_name_lowercase_flags(self):
        entry = bytearray(32)
        entry[:11] = b"HELLO   TXT"
        entry[12] = 24
        self.assertEqual(component(entry, []), "hello.txt")

    def alias_catalog(self, names, hidden):
        entry = bytearray(32)
        entry[:11] = b"_FILE-~3TXT"
        entry[11] = 2 if hidden else 32
        raw = bytes(entry) * 2 + bytes(512 - 64)
        fat = SimpleNamespace(root=2, cluster_bytes=512, chain=lambda *args: [2],
                              blocks=lambda *args: [raw])
        with patch("fat32_fixture_readback.component", side_effect=names):
            return catalog(fat)

    def test_host_appledouble_alias_collision_is_recorded_without_losing_long_names(self):
        files, _, aliases = self.alias_catalog(["._file-000.txt", "._file-001.txt"], True)
        self.assertEqual(len(files), 2)
        self.assertEqual(len(aliases), 1)

    def test_non_host_alias_collision_is_refused(self):
        with self.assertRaisesRegex(AssertionError, "non-AppleDouble"):
            self.alias_catalog(["file-000.txt", "file-001.txt"], False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
