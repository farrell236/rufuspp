#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manual macOS FAT32 USB capture/restore qualification; default is read-only.

Only --erase-authorized-kingston can format/write the pinned disposable USB.
Captures use the production backend; QEMU reads ordinary capture files only.
No VM or file-backed substitute for the physical target is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time

from image_write_integration import require
from physical_usb_integration import (command, open_readonly, read_exact,
                                      validate_diskutil, validate_identity)
from fat32_fixture_readback import fat32_geometry, verify_raw_fixture

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/capture-roundtrip"
FORMATS = (("raw", "raw", "capture.dd"),
           ("fixed-vhd", "vpc", "capture-fixed.vhd"),
           ("dynamic-vhd", "vpc", "capture-dynamic.vhd"),
           ("dynamic-vhdx", "vhdx", "capture.vhdx"))
CHUNK = 8 * 1024**2
SERIAL = "E82A448DA32EF39132D00923"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_path(folder, name):
    relative = PurePosixPath(name)
    require(bool(name) and not relative.is_absolute() and ".." not in relative.parts,
            "unsafe fixture path")
    current = folder
    for part in relative.parts:
        current /= part
        require(not current.is_symlink(), "unexpected fixture symlink: " + name)
    return current


def verify_fixture(folder, manifest):
    require(folder.is_dir() and not folder.is_symlink(), "fixture folder missing or linked")
    checked = total = 0
    for entry in manifest["files"]:
        path = checked_path(folder, entry["path"])
        require(path.is_file() and stat.S_ISREG(path.stat().st_mode),
                "missing fixture file: " + entry["path"])
        require(path.stat().st_size == entry["size"] and sha256(path) == entry["sha256"],
                "fixture content mismatch: " + entry["path"])
        checked += 1
        total += entry["size"]
    for name in manifest["directories"]:
        require(checked_path(folder, name).is_dir(), "missing fixture directory: " + name)
    require(checked > 0 and checked == manifest["fileCount"] and total == manifest["totalFileBytes"],
            "fixture manifest totals disagree")
    return {"filesChecked": checked, "fileBytesChecked": total,
            "directoriesChecked": len(manifest["directories"]), "allSha256Matched": True}


def discover(helper, expected):
    device = json.loads(command([helper, "--inspect-serial", expected["serial"]]))
    validate_identity(device, expected)
    info = plistlib.loads(command(["/usr/sbin/diskutil", "info", "-plist", device["devicePath"]]))
    validate_diskutil(info, device)
    return device


def identity_args(expected):
    return [expected["serial"], expected["stableId"], str(expected["capacityBytes"]),
            str(expected["sectorBytes"])]


def outside_usb(path, device):
    local = path if path.exists() else path.parent
    while not local.exists():
        local = local.parent
    require(not local.is_symlink(), "linked artifact storage")
    for mount in device["mountPoints"]:
        require(os.stat(local).st_dev != os.stat(mount).st_dev,
                "test artifacts must be outside the USB")


def compare_full_disk(fd, baseline, capacity):
    """Independent uncached physical read, exact bytes, including free space/tail."""
    require(baseline.is_file() and baseline.stat().st_size == capacity,
            "raw baseline size differs from USB capacity")
    digest = hashlib.sha256()
    with baseline.open("rb") as expected:
        for offset in range(0, capacity, CHUNK):
            count = min(CHUNK, capacity - offset)
            block = expected.read(count)
            require(len(block) == count, "short baseline read")
            actual = read_exact(fd, offset, count)
            if actual != block:
                first = next(i for i, pair in enumerate(zip(actual, block)) if pair[0] != pair[1])
                raise AssertionError("physical USB byte mismatch at offset " + str(offset + first))
            digest.update(actual)
            if offset % (256 * 1024**2) == 0 or offset + count == capacity:
                print("Independent disk readback: {}/{} bytes".format(offset + count, capacity), flush=True)
    return {"allDiskBytesMatched": True, "bytesCompared": capacity, "sha256": digest.hexdigest()}


def readback(helper, expected, baseline):
    device = discover(helper, expected)
    # Native I/O already unmounted the device, and the independent mount guard
    # keeps it unmounted. Do not issue redundant DA mutations after a write.
    require(not device["mountPoints"], "USB unexpectedly mounted before byte comparison")
    fd = open_readonly(device)
    try:
        result = compare_full_disk(fd, baseline, expected["capacityBytes"])
        require(not discover(helper, expected)["mountPoints"], "USB mounted during byte comparison")
        return result
    finally:
        os.close(fd)


def raw_fixture(helper, expected, manifest, absent=False):
    device = discover(helper, expected)
    require(not device["mountPoints"], "raw file inspection requires an unmounted USB")
    fd = open_readonly(device)
    try:
        layout = fat32_geometry(read_exact(fd, 0, 512), expected["capacityBytes"])
        result = verify_raw_fixture(lambda offset, count: read_exact(fd, offset, count),
                                    layout, manifest, absent)
        require(not discover(helper, expected)["mountPoints"], "USB mounted during file inspection")
        return result
    finally:
        os.close(fd)


class MountGuard:
    """A DA mount-approval callback, not a device claim or a raw I/O handle."""
    def __init__(self, run):
        self.run = run
        self.process = None
        stamp = str(time.time_ns())
        self.output = run.directory / ("mount-guard-" + stamp + ".jsonl")
        self.errors = run.directory / ("mount-guard-" + stamp + ".stderr")
        self.record = {"stdout": self.output.name, "stderr": self.errors.name, "active": False}

    def events(self):
        result = []
        if self.output.exists():
            for line in self.output.read_text().splitlines():
                try:
                    result.append(json.loads(line))
                except json.JSONDecodeError:
                    pass  # A concurrently flushed final line may be incomplete.
        return result

    def alive(self):
        require(self.process is not None and self.process.poll() is None,
                "host mount guard is no longer active")

    def wait_event(self, key):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            self.alive()
            if any(event.get(key) is True for event in self.events()):
                return
            time.sleep(0.05)
        raise AssertionError("mount guard did not confirm " + key)

    def start(self, wait_for_usb=False):
        device = None if wait_for_usb else discover(self.run.helper, self.run.expected)
        require(device is None or not device["mountPoints"], "unmount the USB before arming mount suppression")
        self.run.report.setdefault("mountGuards", []).append(self.record)
        try:
            with self.output.open("xb") as output, self.errors.open("xb") as errors:
                self.process = subprocess.Popen([str(self.run.helper), "--guard-host-mounts"]
                    + identity_args(self.run.expected), stdout=output, stderr=errors)
            self.wait_event("mountGuardReady")
            if wait_for_usb:
                self.record["armedBeforeReconnect"] = True
                self.run.report["currentOperation"] = "Mount suppression armed; waiting for Kingston reconnect"
                self.run.save()
                print("Mount suppression armed. Reconnect the Kingston now; waiting up to 10 minutes.", flush=True)
                deadline = time.monotonic() + 600
                while time.monotonic() < deadline:
                    self.alive()
                    try:
                        device = discover(self.run.helper, self.run.expected)
                    except AssertionError as error:
                        # IOKit may publish the whole disk before diskutil can
                        # query it. Retry absence/DA query errors, never an
                        # identity/geometry validation failure.
                        require("expected exactly one USB device matching" in str(error)
                                or str(error).startswith("/usr/sbin/diskutil failed:"), str(error))
                        self.record["lastReconnectQueryError"] = str(error)
                        time.sleep(0.5)
                        continue
                    require(not device["mountPoints"], "USB mounted on reconnect despite serial mount suppression")
                    if Path(device["devicePath"] + "s1").exists():
                        break
                    time.sleep(0.5)
                else:
                    raise AssertionError("timed out waiting for the pinned Kingston and its FAT32 partition")
            # Probe with a read-only mount request. A broken guard cannot turn
            # this qualification into an unintended writable host mount.
            probe = subprocess.run(["/usr/sbin/diskutil", "mount", "readOnly", device["devicePath"] + "s1"],
                                   capture_output=True, text=True, timeout=60)
            self.record["readOnlyProbeExitCode"] = probe.returncode
            self.record["probeOutput"] = probe.stdout + probe.stderr
            require(probe.returncode != 0, "mount guard failed to block the read-only mount probe")
            require("Rufus++ physical qualification is keeping this USB unmounted" in self.record["probeOutput"],
                    "mount probe failed for a reason other than the qualification guard")
            self.wait_event("mountDenied")
            require(not discover(self.run.helper, self.run.expected)["mountPoints"],
                    "USB mounted despite mount suppression")
            self.record.update(active=True, readOnlyProbeDenied=True, pid=self.process.pid)
            self.run.save()
            print("Host mount suppression active; read-only mount probe was denied.", flush=True)
            return self
        except BaseException:
            self.stop()
            raise

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()  # Only a mount-approval process; never a writer.
                self.process.wait(timeout=10)
        self.record["active"] = False
        self.run.save()


def checked_mount(helper, expected, readonly):
    device = discover(helper, expected)
    command(["/usr/sbin/diskutil", "unmountDisk", device["devicePath"]])
    device = discover(helper, expected)
    partition = device["devicePath"] + "s1"
    info = plistlib.loads(command(["/usr/sbin/diskutil", "info", "-plist", partition]))
    require(info.get("ParentWholeDisk") == Path(device["devicePath"]).name
            and info.get("FilesystemType") == "msdos" and info.get("VolumeName") == "RUFUS_TEST",
            "expected the sole FAT32 test partition on the pinned USB")
    args = ["/usr/sbin/diskutil", "mount"]
    if readonly:
        args.append("readOnly")
    command(args + [partition])
    # Query again after mount; never use a remembered /Volumes name as identity.
    device = discover(helper, expected)
    info = plistlib.loads(command(["/usr/sbin/diskutil", "info", "-plist", partition]))
    require(info.get("ParentWholeDisk") == Path(device["devicePath"]).name
            and info.get("WritableVolume") is (not readonly)
            and len(device["mountPoints"]) == 1
            and info.get("MountPoint") == device["mountPoints"][0],
            "mounted volume identity or read-only status changed")
    mount = Path(info["MountPoint"])
    require(mount.parent == Path("/Volumes") and os.path.ismount(mount) and not mount.is_symlink(),
            "invalid test volume mount")
    return mount


class Run:
    def __init__(self, directory, helper, expected, report):
        self.directory, self.helper, self.expected, self.report = directory, helper, expected, report
        self.mount_guard = None

    def save(self):
        temporary = self.directory / "report.json.tmp"
        temporary.write_text(json.dumps(self.report, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.directory / "report.json")

    def tool(self, label, args, json_output=True):
        self.report["currentOperation"] = label
        self.save()
        stamp = str(time.time_ns())
        out = self.directory / (stamp + ".stdout")
        err = self.directory / (stamp + ".stderr")
        self.report.setdefault("operations", []).append(
            {"label": label, "argv": [str(x) for x in args], "stdout": out.name, "stderr": err.name})
        self.save()
        print("\n" + label, flush=True)
        # Only stdout is buffered (small JSON). Progress remains visible in the
        # user's Terminal and a complete stderr log is retained on the Mac.
        with out.open("xb") as output, err.open("x", encoding="utf-8") as log:
            process = subprocess.Popen([str(x) for x in args], stdout=output, stderr=subprocess.PIPE,
                                       text=True, bufsize=1)
            try:
                for line in process.stderr:
                    print(line, end="", flush=True)
                    log.write(line)
                    log.flush()
                status = process.wait()
            except BaseException:
                process.terminate()
                process.wait(timeout=60)
                raise
        require(status == 0, label + " failed; see " + str(err))
        return json.loads(out.read_text()) if json_output else out.read_text()

    def helper_action(self, label, action, *values):
        if self.mount_guard is not None:
            self.mount_guard.alive()
        discover(self.helper, self.expected)
        result = self.tool(label, [self.helper, action] + list(values) + identity_args(self.expected))
        require(result.get("success") is True and result.get("cancelled") is False,
                label + " did not succeed")
        if self.mount_guard is not None:
            self.mount_guard.alive()
        return result

    def blank(self, label):
        stage = self.directory / ("blank-" + str(time.time_ns()) + ".img")
        result = self.helper_action(label, "--format-fat32-authorized", stage)
        require(result.get("verificationCompleted") is True, "blank format was not verified")
        return result

    def restore(self, name, capture):
        result = self.helper_action("Restore " + name, "--restore-capture-authorized", capture, name)
        require(result.get("verificationCompleted") is True
                and result.get("bytesWritten") == self.expected["capacityBytes"]
                and result.get("bytesVerified") == self.expected["capacityBytes"],
                "restore did not write and verify the full decoded disk")
        return result


def completed_native_restore(entry, capacity):
    result = entry.get("restore", {})
    return (entry.get("captureVerified") is True and entry.get("fixtureAbsentBeforeRestore") is True
            and result.get("success") is True and result.get("cancelled") is False
            and result.get("verificationCompleted") is True and result.get("bytesWritten") == capacity
            and result.get("bytesVerified") == capacity)


def pending_format(report):
    return next(((name, filename) for name, _, filename in FORMATS
                 if not report["roundtrips"].get(name, {}).get("success")), None)


def finish_verified_restore(run, baseline, manifest, name, filename):
    """Finish a saved native success using read-only opens; never retry a write."""
    entry = run.report["roundtrips"][name]
    require(completed_native_restore(entry, run.expected["capacityBytes"]),
            "read-only continuation requires a completed verified native restore")
    capture = run.directory / filename
    require(sha256(capture) == entry["captureSha256"], "captured image changed since the restore")
    require(sha256(baseline) == run.report["baselineSha256"], "raw baseline changed since previous run")
    run.mount_guard.alive()
    run.report["currentOperation"] = "Finish independent read-only checks for " + name
    run.save()
    entry["readback"] = readback(run.helper, run.expected, baseline)
    require(entry["readback"]["sha256"] == run.report["baselineSha256"], "restored disk hash mismatch")
    entry["files"] = raw_fixture(run.helper, run.expected, manifest)
    run.mount_guard.alive()
    require(sha256(capture) == entry["captureSha256"], "capture changed during read-only inspection")
    entry.update(success=True, continuedReadOnlyAfterVerifiedRestore=True,
                 restoreRepeatedDuringContinuation=False)
    run.save()
    print("PASS " + name + ": completed independent checks without repeating the write", flush=True)


def complete_readonly(run, baseline, manifest):
    pending = pending_format(run.report)
    require(pending is not None, "no pending restore needs read-only verification")
    finish_verified_restore(run, baseline, manifest, *pending)
    device = discover(run.helper, run.expected)
    require(not device["mountPoints"], "USB mounted before read-only completion")
    run.mount_guard.alive()
    command(["/usr/sbin/diskutil", "eject", device["devicePath"]])
    remaining = [name for name, _, _ in FORMATS
                 if not run.report["roundtrips"].get(name, {}).get("success")]
    run.report.update(ejected=True, success=not remaining,
                      currentOperation="read-only continuation complete; no further writes attempted",
                      readOnlyCompletion={"format": pending[0], "success": True,
                                          "usbWritten": False, "pendingFormats": remaining})
    run.save()
    print("Read-only continuation passed. USB safely ejected. Pending formats: "
          + (", ".join(remaining) or "none"), flush=True)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--erase-authorized-kingston", action="store_true")
    actions.add_argument("--verify-restored-usb", action="store_true",
                         help="finish only read-only checks of a saved verified restore, then eject and stop")
    parser.add_argument("--target-record", type=Path, default=WORK / "target-readonly.json")
    parser.add_argument("--fixture-record", type=Path, default=WORK / "fixture-ready.json")
    parser.add_argument("--helper", type=Path,
                        default=ROOT / "build/native-root/tests/rufus_plus_plus_physical_usb_helper")
    parser.add_argument("--resume", type=Path, help="resume evidence with explicit write or read-only action")
    parser.add_argument("--wait-for-usb", action="store_true",
                        help="arm mount suppression before reconnecting the pinned USB for an authorized resume")
    args = parser.parse_args(argv)
    run = None
    mount_guard = None
    try:
        require(sys.platform == "darwin", "this runner requires macOS")
        require(not args.resume or args.erase_authorized_kingston or args.verify_restored_usb,
                "resume requires an explicit write or read-only action")
        require(not args.verify_restored_usb or args.resume, "read-only continuation requires --resume")
        require(not args.wait_for_usb or (args.resume and (args.erase_authorized_kingston or args.verify_restored_usb)),
                "reconnect waiting requires an authorized resume")
        expected = json.loads(args.target_record.read_text())["expectedTarget"]
        require(expected.get("serial") == SERIAL and expected.get("vendor") == "Kingston"
                and expected.get("model") == "DataTraveler 3.0"
                and expected.get("capacityBytes") == 15472047104 and expected.get("sectorBytes") == 512,
                "this manual run is pinned to the authorized disposable Kingston")
        helper = args.helper.resolve(strict=True)
        device = None if args.wait_for_usb else discover(helper, expected)
        fixture = json.loads(args.fixture_record.read_text())
        payload, manifest_path = Path(fixture["payload"]), Path(fixture["manifest"])
        require(not payload.is_symlink() and not manifest_path.is_symlink(), "linked fixture record")
        manifest = json.loads(manifest_path.read_text())
        fixture_check = verify_fixture(payload, manifest)
        if device is not None:
            for path in (WORK, helper, payload, manifest_path):
                outside_usb(path, device)
        qemu_img, qemu_version = None, None
        if not args.verify_restored_usb:
            qemu_img = shutil.which("qemu-img", path="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin")
            require(qemu_img is not None, "qemu-img is required for independent container decoding")
            qemu_version = command([qemu_img, "--version"]).decode()
        print("Pinned target: " + json.dumps(device if device is not None else expected), flush=True)
        print("Fixture verified: " + json.dumps(fixture_check), flush=True)
        if not args.erase_authorized_kingston and not args.verify_restored_usb:
            print("Read-only preflight passed. No USB format, write, capture or mount attempted.", flush=True)
            return 0
        require(os.geteuid() == 0, "run with sudo in the user's Terminal; no raw device I/O attempted")
        require(os.environ.get("SUDO_UID", "").isdigit() and os.environ.get("SUDO_GID", "").isdigit()
                and int(os.environ["SUDO_UID"]) != 0, "sudo must identify the invoking normal user")
        if not args.verify_restored_usb:
            require(shutil.disk_usage(WORK).free > 3 * expected["capacityBytes"] + 512 * 1024**2,
                    "insufficient temporary space for capture, conversion and baseline")
        if args.resume:
            directory = args.resume.resolve(strict=True)
            require(directory.parent == WORK.resolve() and not args.resume.is_symlink(), "invalid resume folder")
            report = json.loads((directory / "report.json").read_text())
            require(report.get("expectedTarget") == expected and report.get("manifestSha256") == sha256(manifest_path),
                    "resume identity or fixture differs")
            require(report.get("baselineVerified") is True, "resume requires a verified raw baseline")
            if args.wait_for_usb or args.verify_restored_usb:
                pending = pending_format(report)
                require(pending is not None, "no pending capture/restore tests remain")
                entry = report["roundtrips"].get(pending[0], {})
                if args.verify_restored_usb:
                    require(completed_native_restore(entry, expected["capacityBytes"]),
                            "read-only continuation requires a completed native restore awaiting independent verification")
                if entry.get("captureVerified"):
                    capture = directory / pending[1]
                    require(sha256(capture) == entry["captureSha256"],
                            "captured image changed since the previous run")
            snapshot = directory / ("previous-report-" + str(time.time_ns()) + ".json")
            snapshot.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            report.setdefault("resumeHistory", []).append({"previousReport": snapshot.name,
                                                          "previousError": report.get("error")})
        else:
            directory = Path(tempfile.mkdtemp(prefix="run-", dir=WORK))
            # Let the invoking user and Codex inspect progress while sudo runs;
            # mkdtemp otherwise leaves the new evidence directory root-only.
            os.chown(directory, int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"]))
            report = {"success": False, "expectedTarget": expected, "fixture": fixture,
                      "manifestSha256": sha256(manifest_path), "fixtureCheck": fixture_check,
                      "qemuImgVersion": qemu_version, "roundtrips": {}, "baselineVerified": False,
                      "scope": "physical FAT32 capture/restore backends; no GUI or bootability qualification"}
        run = Run(directory, helper, expected, report)
        report["success"] = False
        report.pop("error", None)
        report.pop("ejected", None)
        run.save()
        print("Evidence directory: " + str(directory), flush=True)
        (WORK / "latest-run.txt").write_text(str(directory) + "\n")
        baseline = directory / "capture.dd"
        disk_baseline_known = False
        if args.wait_for_usb:
            require(sha256(baseline) == report["baselineSha256"], "raw baseline changed since previous run")
            mount_guard = MountGuard(run).start(wait_for_usb=True)
            run.mount_guard = mount_guard
            device = discover(helper, expected)
            for path in (WORK, helper, payload, manifest_path, directory):
                outside_usb(path, device)
        if not report["baselineVerified"]:
            report["initialFormat"] = run.blank("Prepare blank FAT32 test USB")
            mount = checked_mount(helper, expected, readonly=False)
            # macOS may add housekeeping files; only fixture files are asserted.
            for name in manifest["directories"]:
                checked_path(mount, name).mkdir(parents=True, exist_ok=True)
            for entry in manifest["files"]:
                source = checked_path(payload, entry["path"])
                target = checked_path(mount, entry["path"])
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            os.sync()
            report["originalFiles"] = verify_fixture(mount, manifest)
            device = discover(helper, expected)
            command(["/usr/sbin/diskutil", "unmountDisk", device["devicePath"]])
            mount_guard = MountGuard(run).start()
            run.mount_guard = mount_guard
            report["rawCapture"] = run.helper_action("Capture physical USB as DD", "--capture-authorized", "raw", baseline)
            require(report["rawCapture"]["bytesCaptured"] == expected["capacityBytes"], "partial raw capture")
            report["baselineReadback"] = readback(helper, expected, baseline)
            report["baselineSha256"] = report["baselineReadback"]["sha256"]
            report["baselineVerified"] = True
            report["baselineRawFiles"] = raw_fixture(helper, expected, manifest)
            disk_baseline_known = True
            run.save()
        else:
            require(sha256(baseline) == report["baselineSha256"], "raw baseline changed since previous run")
            if mount_guard is None:
                device = discover(helper, expected)
                if device["mountPoints"]:
                    command(["/usr/sbin/diskutil", "unmountDisk", device["devicePath"]])
                mount_guard = MountGuard(run).start()
                run.mount_guard = mount_guard
        if args.verify_restored_usb:
            # This returns before the only loop that can format, capture or
            # restore a disk. A saved native success never authorizes a rewrite.
            return complete_readonly(run, baseline, manifest)
        for name, qemu_format, filename in FORMATS:
            entry = report["roundtrips"].setdefault(name, {})
            if entry.get("success") is True:
                continue
            capture = directory / filename
            if name != "raw" and not entry.get("captureVerified"):
                if not disk_baseline_known:
                    # A resumed run may already have a verified pending capture
                    # ready to restore. Only reset the USB if a *new capture*
                    # actually needs the authenticated baseline on the device.
                    try:
                        report["resumeReadback"] = readback(helper, expected, baseline)
                    except AssertionError as error:
                        require(str(error).startswith("physical USB byte mismatch at offset "), str(error))
                        report["resumeRestore"] = run.restore("raw", baseline)
                        report["resumeReadback"] = readback(helper, expected, baseline)
                    disk_baseline_known = True
                require(not capture.exists(), "unverified capture exists; preserve it and diagnose before retry")
                require(shutil.disk_usage(directory).free > 2 * expected["capacityBytes"] + 512 * 1024**2,
                        "insufficient free space for conversion")
                entry["capture"] = run.helper_action("Capture physical USB as " + name,
                                                     "--capture-authorized", name, capture)
                require(entry["capture"]["bytesCaptured"] == expected["capacityBytes"], "partial container capture")
            if not entry.get("captureVerified"):
                entry["qemuInfo"] = json.loads(command([qemu_img, "info", "--output=json", "-f", qemu_format, capture]))
                require(entry["qemuInfo"]["virtual-size"] == expected["capacityBytes"],
                        "independent reader reports a different decoded disk size")
                if name != "raw":
                    entry["independentCompare"] = run.tool("Compare decoded " + name + " against DD baseline",
                        [qemu_img, "compare", "-p", "-f", "raw", "-F", qemu_format, baseline, capture], False)
                entry["captureSha256"] = sha256(capture)
                entry["captureVerified"] = True
                run.save()
            require(sha256(capture) == entry["captureSha256"], "captured image changed before restore")
            if completed_native_restore(entry, expected["capacityBytes"]):
                finish_verified_restore(run, baseline, manifest, name, filename)
                disk_baseline_known = True
                continue
            entry["blankBeforeRestore"] = run.blank("Remove filesystem contents before " + name + " restore")
            disk_baseline_known = False
            entry["blankRawFiles"] = raw_fixture(helper, expected, manifest, absent=True)
            entry["fixtureAbsentBeforeRestore"] = True
            entry["restore"] = run.restore(name, capture)
            entry["readback"] = readback(helper, expected, baseline)
            require(entry["readback"]["sha256"] == report["baselineSha256"], "restored disk hash mismatch")
            disk_baseline_known = True
            entry["files"] = raw_fixture(helper, expected, manifest)
            mount_guard.alive()
            require(sha256(capture) == entry["captureSha256"], "capture changed while restoring")
            entry["success"] = True
            run.save()
            print("PASS " + name + ": full disk bytes and all fixture files matched", flush=True)
        report["finalSourceFixtureCheck"] = verify_fixture(payload, manifest)
        device = discover(helper, expected)
        command(["/usr/sbin/diskutil", "eject", device["devicePath"]])
        report["ejected"] = True
        report["success"] = True
        report["currentOperation"] = "complete"
        run.save()
        print("PASS all four physical USB capture/restore roundtrips. USB safely ejected.", flush=True)
        print("Report: " + str(directory / "report.json"), flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        if run:
            run.report["error"] = str(error) or "interrupted"
            run.save()
        print("FAIL/REFUSED: " + (str(error) or "interrupted"), file=sys.stderr, flush=True)
        return 1
    finally:
        if mount_guard is not None:
            mount_guard.stop()
        # sudo-created evidence stays accessible to the invoking normal user.
        # Only this run's ordinary local artifacts are adjusted; never follow links.
        if run and os.geteuid() == 0:
            uid, gid = int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"])
            for parent, dirs, files in os.walk(run.directory, followlinks=False):
                for path in [Path(parent)] + [Path(parent) / x for x in dirs + files]:
                    if not path.is_symlink():
                        os.chown(path, uid, gid, follow_symlinks=False)


if __name__ == "__main__":
    sys.exit(main())
