#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare/launch an isolated ARM64 UEFI guest using a qualified USB capture.

The guest gets one read-only captured USB, private UEFI variables, no other
disks, no shared folders and no network. It never opens a physical device.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from image_write_integration import digest, require

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "qemu-boot"
QEMU = Path("/opt/homebrew/bin/qemu-system-aarch64")
CODE = Path("/opt/homebrew/share/qemu/edk2-aarch64-code.fd")
VARS = Path("/opt/homebrew/share/qemu/edk2-arm-vars.fd")


def block_node(name, path, readonly):
    require(path.is_file() and not str(path).startswith("/dev/"), "only ordinary image files are accepted")
    return json.dumps({"driver": "raw", "node-name": name, "read-only": readonly,
                       "file": {"driver": "file", "filename": str(path), "read-only": readonly}})


def configuration(media, variables, session, smoke=False):
    command = [str(QEMU), "-name", "Rufus++ Ubuntu USB boot test", "-no-user-config", "-nodefaults",
               "-machine", "virt,pflash0=efi-code,pflash1=efi-vars",
               "-accel", "tcg" if smoke else "hvf", "-cpu", "cortex-a72" if smoke else "host",
               "-m", "256" if smoke else "4096", "-smp", "1" if smoke else "4",
               "-blockdev", block_node("efi-code", CODE, True),
               "-blockdev", block_node("efi-vars", variables, False),
               "-blockdev", block_node("usb-image", media, True),
               "-device", "qemu-xhci,id=xhci",
               "-device", "usb-storage,bus=xhci.0,drive=usb-image,removable=on,bootindex=1",
               "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
               "-device", "virtio-gpu-pci", "-nic", "none", "-boot", "menu=on",
               "-qmp", "unix:{},server=on,wait=off".format(session / "qmp.sock"),
               "-serial", "file:" + str(session / "serial.log"),
               "-display", "none" if smoke else "cocoa"]
    if smoke:
        command.append("-S")  # Validate configuration paused; no claim of a guest boot.
    return command


def qmp_smoke(session, process):
    deadline = time.monotonic() + 15
    connection = socket.socket(socket.AF_UNIX)
    while True:
        require(process.poll() is None, "QEMU exited before its control socket became ready")
        try:
            connection.connect(str(session / "qmp.sock"))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            require(time.monotonic() < deadline, "QEMU control socket timed out")
            time.sleep(0.1)
    with connection, connection.makefile("rwb") as channel:
        require("QMP" in json.loads(channel.readline()), "invalid QEMU control greeting")
        for action in ("qmp_capabilities", "query-status", "quit"):
            channel.write((json.dumps({"execute": action}) + "\n").encode())
            channel.flush()
            while True:
                answer = json.loads(channel.readline())
                if "return" in answer or "error" in answer:
                    require("error" not in answer, "QEMU rejected " + action + ": " + str(answer))
                    if action == "query-status":
                        require(answer["return"]["status"] == "prelaunch", "smoke guest was not paused")
                    break


def checked_capture():
    media = WORK / "usb-readback.img"
    report = json.loads((WORK / "capture-report.json").read_text())
    previous = json.loads((ROOT / "build/ubuntu-iso/physical-usb-replug-report.json").read_text())
    require(report.get("success") is True and report.get("physicalUsbOpenedReadOnly") is True
            and report.get("expectedTarget") == previous.get("expectedTarget")
            and report.get("imageSha256") == previous.get("sourceSha256"), "capture provenance mismatch")
    require(media.is_file() and not media.is_symlink() and media.stat().st_size == report["capturedBytes"],
            "the whole-USB capture is missing or changed size")
    require(digest(media) == report["wholeDiskSha256"], "whole-USB capture SHA-256 mismatch")
    return media, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--run", action="store_true")
    action.add_argument("--smoke-test", action="store_true")
    args = parser.parse_args()
    process = None
    record = None
    report = {"success": False, "bootResult": "not assessed"}
    try:
        require(os.geteuid() != 0, "launch QEMU without sudo; root is only needed for read-only USB capture")
        require(QEMU.is_file() and CODE.is_file() and VARS.is_file(),
                "QEMU or ARM64 UEFI firmware is missing; run sh tests/install_qemu.sh")
        WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
        session = Path(tempfile.mkdtemp(prefix="run-", dir=WORK))
        variables = session / "uefi-vars.fd"
        shutil.copyfile(VARS, variables)
        variables.chmod(0o600)
        if args.smoke_test:
            media = session / "smoke-only.img"
            with media.open("xb") as output:
                output.truncate(16 * 1024 * 1024)
            capture = None
        else:
            print("Checking the complete USB capture before launching…", flush=True)
            media, capture = checked_capture()
        command = configuration(media, variables, session, args.smoke_test)
        report.update({"state": "launching", "command": command, "session": str(session),
                       "smokeTestOnly": args.smoke_test, "mediaReadOnly": True,
                       "networkAttached": False, "sharedFoldersAttached": False,
                       "qemuVersion": subprocess.check_output([str(QEMU), "--version"], text=True).splitlines()[0]})
        record = session / "result.json"
        (WORK / "last-run.json").write_text(json.dumps({"session": str(session)}) + "\n")
        with (session / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            report["pid"] = process.pid
            record.write_text(json.dumps(report, indent=2) + "\n")
            if args.smoke_test:
                qmp_smoke(session, process)
                process.wait(timeout=10)
            else:
                print("QEMU is opening. Stop at the Ubuntu installer welcome screen.", flush=True)
                print("The guest has no installation disk, shared folders or network. Close its window to stop.", flush=True)
                print("Session logs: " + str(session), flush=True)
                process.wait()
        require(process.returncode == 0, "QEMU failed; see " + str(session / "qemu.log"))
        if capture is not None:
            require(digest(media) == capture["wholeDiskSha256"], "captured media changed during QEMU use")
            report["mediaUnchanged"] = True
        report.update({"success": True, "state": "exited", "exitCode": process.returncode})
        if args.smoke_test:
            report["bootResult"] = "not run; paused configuration check only"
            print("PASS QEMU ARM64/UEFI/read-only USB configuration (paused, no guest boot)")
        return 0
    except Exception as error:
        report["error"] = str(error)
        print("QEMU preparation/launch failed: " + str(error), file=sys.stderr)
        return 1
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if record is not None:
            record.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
