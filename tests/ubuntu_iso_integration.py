#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in official-ISO write test. Creates its own target; accepts no device path.

Download/signature verification is a separate preparation step. This runner
requires and rechecks the authenticated ISO's expected SHA-256 before writing.
No ISO is downloaded by CTest, and no physical drive can be supplied here.
"""
import argparse
import json
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile

from image_write_integration import digest, require, verify_bytes
from virtual_disk_integration import OwnedImage, Unavailable, run, validate_capacity

MIB = 1024 * 1024


def image_capacity(size):
    require(type(size) is int and size > 0 and size % 512 == 0,
            "ISO must have a positive, sector-aligned length")
    # Include untouched space beyond the ISO to prove we do not overwrite it.
    return validate_capacity(max(16 * MIB, ((size + MIB - 1) // MIB + 16) * MIB))


def iso_structure(path):
    # Read the written ISO9660 volume descriptor without mounting anything.
    with path.open("rb") as stream:
        stream.seek(16 * 2048)
        descriptor = stream.read(2048)
    require(descriptor[:7] == b"\x01CD001\x01", "written ISO9660 primary descriptor missing")
    return {"iso9660PrimaryDescriptor": True,
            "volumeLabel": descriptor[40:72].decode("ascii").rstrip()}


def archive_contents(path):
    # macOS bsdtar independently reads ISO9660/Rock Ridge. Do not substitute a
    # FAT checker for the ISO filesystem or change any image contents to boot.
    if sys.platform != "darwin":
        return {"archiveCheck": "not available on this host"}
    listing = run(["/usr/bin/tar", "-tf", str(path)]).decode().splitlines()
    files = {name.removeprefix("./").lower() for name in listing}
    required = {"efi/boot/bootaa64.efi", "boot/grub/grub.cfg", "casper/vmlinuz", "casper/initrd"}
    require(required <= files, "written Ubuntu ARM64 image is missing expected boot files")
    return {"archiveReader": "/usr/bin/tar (libarchive)", "bootFilesPresent": sorted(required)}


def exercise(args):
    require(re.fullmatch(r"[0-9a-fA-F]{64}", args.sha256) is not None, "invalid expected SHA-256")
    source = args.iso.resolve(strict=True)
    require(source.is_file() and not args.iso.is_symlink(), "source must be an ordinary ISO file")
    # Validate size before spending time hashing or allocating any target.
    capacity = image_capacity(source.stat().st_size)
    expected = args.sha256.lower()
    require(digest(source) == expected, "ISO SHA-256 does not match the authenticated manifest")
    helper = args.helper.resolve(strict=True)
    analysis = json.loads(run([str(helper), "--inspect-iso", str(source)]))
    require(analysis["sizeBytes"] == source.stat().st_size and analysis["rawWrite"], "ISO analysis mismatch")
    require(analysis["architecture"] == "ARM64", "this Ubuntu fixture expects the ARM64 installer")
    report = {"source": str(source), "expectedSha256": expected,
              "sourceBytes": source.stat().st_size, "targetBytes": capacity,
              "host": platform.platform(), "analysis": analysis,
              "mode": "image-file" if args.file_only else "raw-virtual-device",
              "bootTest": "not run"}
    print("Verified Ubuntu ISO SHA-256: " + expected, flush=True)
    print("Source {} bytes; private target {} bytes".format(source.stat().st_size, capacity), flush=True)
    directory = Path(tempfile.mkdtemp(prefix="rufus-owned-virtual-ubuntu-"))
    image = None
    wrote = False
    try:
        if args.file_only:
            require(args.file_helper is not None, "--file-helper is required with --file-only")
            destination = directory / "target.img"
            output = run([str(args.file_helper.resolve(strict=True)), "--write", str(source),
                          str(destination), str(capacity), "512", str(64 * 1024), "0"])
            report["writer"] = json.loads(output)
        else:
            if sys.platform not in ("darwin", "linux"):
                raise Unavailable("Ubuntu raw-ISO integration currently supports macOS and Linux")
            image = OwnedImage(directory, capacity)
            image.create()
            print("Private virtual-disk backing: " + str(image.backing), flush=True)
            image.attach()
            wrote = True
            report["writerOutput"] = image.invoke(helper, source, "iso")
            image.detach()
            image.attach(readonly=True)
            report["readOnlyReattachOutput"] = image.invoke(helper, source, "iso", verify=True)
            image.detach()
            destination = image.backing
        # Separate Python implementation, after closing the writer and (for a
        # raw device) detaching: exact whole-target comparison and image hash.
        report["readback"] = verify_bytes(source, destination, capacity, False)
        require(report["readback"]["actualImageSha256"] == expected, "written ISO SHA-256 mismatch")
        require(digest(source) == expected, "ISO source changed during testing")
        report["filesystem"] = iso_structure(destination)
        report["filesystem"].update(archive_contents(destination))
        print("Independent written-image SHA-256: " + report["readback"]["actualImageSha256"], flush=True)
        print("Every target byte, including the untouched trailing gap, matched", flush=True)
    except Unavailable as error:
        if wrote:
            raise RuntimeError("ISO validation became unavailable after a write attempt: " + str(error)) from error
        raise
    finally:
        try:
            if image is not None:
                image.detach()
        except Exception:
            print("Cleanup failed; retain backing directory: " + str(directory), file=sys.stderr)
            raise
        shutil.rmtree(directory)
    report["cleanupCompleted"] = True
    report["success"] = True
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--file-only", action="store_true")
    parser.add_argument("--file-helper", type=Path)
    args = parser.parse_args()
    args.report.unlink(missing_ok=True)
    try:
        report = exercise(args)
    except Exception as error:
        print("FAIL: " + str(error), file=sys.stderr)
        return 1
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS Ubuntu ISO " + report["mode"], flush=True)
    print("Report: " + str(args.report), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
