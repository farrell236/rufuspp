#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manual-only physical USB qualification; default action is read-only preflight.

An erase requires --erase-authorized and a pinned mount, serial, stable ID,
model, vendor and geometry. CTest never invokes the destructive path. Native
writes run only through the application's production guarded backend.
"""
import argparse
import array
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
import sys

from image_write_integration import digest, require

MIB = 1024 * 1024
SAMPLE = 64 * 1024


def command(args):
    result = subprocess.run([str(x) for x in args], capture_output=True, timeout=60)
    require(result.returncode == 0, "{} failed: {}".format(args[0], result.stderr.decode(errors="replace")))
    return result.stdout


def validate_identity(device, expected):
    require(device.get("bus") == "USB" and device.get("wholeDevice") is True
            and device.get("systemDevice") is False and device.get("writable") is True,
            "refusing a non-USB, system, partition or read-only target")
    for key in ("serial", "stableId", "capacityBytes", "sectorBytes", "vendor", "model"):
        require(key in expected and device.get(key) == expected[key], "USB identity changed: " + key)
    require(bool(expected["serial"]) and bool(expected["stableId"]), "missing pinned USB identity")
    require(re.fullmatch(r"/dev/disk[0-9]+", device.get("devicePath", "")) is not None,
            "not a whole macOS disk path")


def validate_diskutil(info, device):
    require(info.get("WholeDisk") is True and info.get("Internal") is False
            and info.get("BusProtocol") == "USB" and info.get("DeviceNode") == device["devicePath"]
            and info.get("TotalSize") == device["capacityBytes"]
            and info.get("DeviceBlockSize") == device["sectorBytes"],
            "diskutil does not independently confirm the selected external whole USB")


def discover(args, expected, by_serial=False):
    action, value = ("--inspect-serial", args.serial) if by_serial else ("--inspect-mounted", args.mount)
    device = json.loads(command([args.helper, action, value]))
    validate_identity(device, expected)
    import plistlib
    validate_diskutil(plistlib.loads(command(["/usr/sbin/diskutil", "info", "-plist", device["devicePath"]])), device)
    if not by_serial:
        require(args.mount in device.get("mountPoints", []) and os.path.ismount(args.mount),
                "the authorized volume is no longer mounted on this device")
        info = plistlib.loads(command(["/usr/sbin/diskutil", "info", "-plist", args.mount]))
        require(info.get("ParentWholeDisk") == Path(device["devicePath"]).name,
                "selected mount does not belong to the authorized whole disk")
    return device


def read_exact(fd, offset, count):
    data = bytearray()
    while len(data) < count:
        block = os.pread(fd, count - len(data), offset + len(data))
        require(bool(block), "unexpected end of physical USB readback")
        data.extend(block)
    return bytes(data)


def open_readonly(device):
    path = device["devicePath"].replace("/dev/disk", "/dev/rdisk", 1)
    before = os.lstat(path)
    require(stat.S_ISCHR(before.st_mode), "expected a raw disk character device")
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        opened = os.fstat(fd)
        require(stat.S_ISCHR(opened.st_mode) and opened.st_rdev == before.st_rdev,
                "raw handle does not match inspected device")
        sector, blocks = array.array("I", [0]), array.array("Q", [0])
        # Darwin _IOR('d', 24, uint32_t), _IOR('d', 25, uint64_t), from sys/disk.h.
        fcntl.ioctl(fd, 0x40046418, sector, True)
        fcntl.ioctl(fd, 0x40086419, blocks, True)
        require(sector[0] == device["sectorBytes"] and blocks[0] * sector[0] == device["capacityBytes"],
                "opened raw USB geometry changed")
        fcntl.fcntl(fd, 48, 1)  # Darwin F_NOCACHE; readback avoids the host data cache.
        return fd
    except BaseException:
        os.close(fd)
        raise


def gap_samples(fd, image_size, capacity):
    begin = ((image_size + MIB - 1) // MIB + 1) * MIB
    end = capacity - MIB
    require(end - begin >= 3 * SAMPLE, "insufficient space for independent gap controls")
    offsets = (begin, ((begin + end) // 2 // 512) * 512, end - SAMPLE)
    return [{"offset": offset, "sha256": hashlib.sha256(read_exact(fd, offset, SAMPLE)).hexdigest()}
            for offset in offsets]


def verify_usb(fd, source, expected_sha, capacity, before_samples=None):
    actual_hash = hashlib.sha256()
    size = source.stat().st_size
    with source.open("rb") as original:
        for offset in range(0, size, MIB):
            expected = original.read(min(MIB, size - offset))
            actual = read_exact(fd, offset, len(expected))
            require(actual == expected, "physical USB differs from ISO at block " + str(offset))
            actual_hash.update(actual)
            if offset % (128 * MIB) == 0:
                print("Independent USB readback: {}/{} bytes".format(offset + len(actual), size), flush=True)
    require(actual_hash.hexdigest() == expected_sha, "physical USB image SHA-256 mismatch")
    require(read_exact(fd, capacity - MIB, MIB) == bytes(MIB), "trailing metadata was not cleared")
    after = gap_samples(fd, size, capacity)
    if before_samples is not None:
        require(before_samples == after, "an unwritten gap control changed")
    return {"imageBytesCompared": size, "imageSha256": actual_hash.hexdigest(),
            "zeroTailBytesVerified": MIB, "gapSamples": after,
            "gapControlsUnchanged": before_samples is not None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--target-record", type=Path, required=True,
                        help="local authorized identity snapshot; never select by disk number alone")
    parser.add_argument("--report", type=Path, required=True)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--erase-authorized", action="store_true")
    action.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    report = {"host": platform.platform(), "success": False,
              "writeAttempted": False, "bootTest": "not run",
              "mode": "replug-read-only" if args.verify_only else "physical-write"}
    try:
        require(sys.platform == "darwin", "this qualification runner requires macOS")
        target = json.loads(args.target_record.read_text(encoding="utf-8"))
        expected = {key: target[key] for key in (
            "serial", "stableId", "vendor", "model", "capacityBytes", "sectorBytes")}
        args.mount, args.serial, args.stable_id = target["mount"], target["serial"], target["stableId"]
        args.capacity, args.sector = target["capacityBytes"], target["sectorBytes"]
        require(type(args.capacity) is int and 4 * 1024**3 <= args.capacity <= 64 * 1024**3
                and args.sector == 512 and Path(args.mount).parent == Path("/Volumes"),
                "invalid authorized USB snapshot")
        report["expectedTarget"] = expected
        args.helper = args.helper.resolve(strict=True)
        source = args.iso.resolve(strict=True)
        require(source.is_file() and not args.iso.is_symlink(), "ISO must be an ordinary source file")
        require(re.fullmatch(r"[0-9a-f]{64}", args.sha256) is not None, "invalid pinned SHA-256")
        require(digest(source) == args.sha256, "source ISO checksum mismatch")
        require(source.stat().st_size % args.sector == 0 and
                source.stat().st_size < args.capacity - 2 * MIB, "ISO must fit with safe trailing space")
        device = discover(args, expected, by_serial=args.verify_only)
        report["target"] = device
        report["sourceSha256"] = args.sha256
        report["sourceBytes"] = source.stat().st_size
        print("Pinned physical target: " + json.dumps(device), flush=True)
        if not args.erase_authorized and not args.verify_only:
            print("Read-only preflight passed. No write was authorized or attempted.")
            return 0
        require(os.geteuid() == 0, "run the prepared command with sudo; no target writes attempted")
        args.report.parent.mkdir(parents=True, exist_ok=True)
        report_path = args.report.resolve()
        if not args.verify_only:
            require(os.stat(source).st_dev != os.stat(args.mount).st_dev and
                    os.stat(report_path.parent).st_dev != os.stat(args.mount).st_dev,
                    "source and report must be stored outside the USB")
            fd = open_readonly(device)
            try:
                before = gap_samples(fd, source.stat().st_size, args.capacity)
            finally:
                os.close(fd)
            # Recheck immediately before passing the pinned identity to the
            # backend, which independently revalidates before/after unmounting.
            device = discover(args, expected)
            report["writeAttempted"] = True
            report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            writer = subprocess.run([str(args.helper), "--write-authorized", str(source), args.mount,
                                     args.stable_id, args.serial, str(args.capacity), str(args.sector)],
                                    stdout=subprocess.PIPE, text=True)
            if writer.stdout.strip():
                report["writer"] = json.loads(writer.stdout)
            require(writer.returncode == 0 and report.get("writer", {}).get("success") is True,
                    "production backend write failed; see writer result/progress")
            require(report["writer"].get("verificationCompleted") is True
                    and report["writer"].get("cancelled") is False
                    and report["writer"].get("bytesWritten") == source.stat().st_size
                    and report["writer"].get("bytesVerified") == source.stat().st_size + MIB,
                    "production writer did not report the complete image and cleared-tail verification")
        else:
            # A replug check can validate gap controls against the earlier run.
            previous = report_path.parent / "physical-usb-write-report.json"
            recorded = json.loads(previous.read_text(encoding="utf-8"))
            require(recorded.get("success") is True and recorded.get("expectedTarget") == expected
                    and recorded.get("sourceSha256") == args.sha256,
                    "replug verification requires the successful matching write report")
            before = recorded["readback"]["gapSamples"]
        device = discover(args, expected, by_serial=True)
        command(["/usr/sbin/diskutil", "unmountDisk", device["devicePath"]])
        device = discover(args, expected, by_serial=True)
        fd = open_readonly(device)
        try:
            report["readback"] = verify_usb(fd, source, args.sha256, args.capacity, before)
        finally:
            os.close(fd)
        require(digest(source) == args.sha256, "source changed during physical qualification")
        device = discover(args, expected, by_serial=True)
        command(["/usr/sbin/diskutil", "eject", device["devicePath"]])
        report["ejected"] = True
        report["success"] = True
        print("PASS physical USB " + ("replug readback" if args.verify_only else "guarded write/readback"), flush=True)
        print("Written ISO SHA-256: " + report["readback"]["imageSha256"], flush=True)
        print("USB safely ejected. Report: " + str(args.report), flush=True)
        return 0
    except Exception as error:
        report["error"] = str(error)
        print("FAIL/REFUSED: " + str(error), file=sys.stderr, flush=True)
        return 1
    finally:
        # Always preserve the result of an attempted destructive operation.
        if report["writeAttempted"] or args.verify_only:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
