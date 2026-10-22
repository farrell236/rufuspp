#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only diagnostic of a stopped physical capture/restore test.

Never writes, mounts or ejects the USB. Refuses an already mounted device.
Records every changed sector (bounded to 1,024) and compares the FAT32 tree
independently with the authenticated DD baseline, including file hashes.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

from fat32_readback import Fat32, compare_files
from fat32_fixture_readback import fat32_geometry as geometry
from image_write_integration import require
from physical_usb_integration import open_readonly, read_exact
from test_physical_capture_usb import CHUNK, ROOT, WORK, discover, outside_usb, sha256


def allocation_info(fat):
    boot = fat.read(fat.base, 512)
    fsinfo_sector = struct.unpack_from("<H", boot, 48)[0]
    info = fat.read(fat.base + fsinfo_sector * 512, 512)
    require(info[:4] == b"RRaA" and info[484:488] == b"rrAa" and info[510:] == b"\x55\xaa",
            "invalid FSInfo signatures")
    free = sum(struct.unpack_from("<I", fat.table, index * 4)[0] & 0x0fffffff == 0
               for index in range(2, fat.clusters + 2))
    reported_free, next_free = struct.unpack_from("<II", info, 488)
    return {"actualFreeClusters": free, "fsinfoFreeClusters": reported_free,
            "fsinfoNextFreeCluster": next_free,
            "freeCounterConsistent": reported_free in (free, 0xffffffff)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path)
    args = parser.parse_args(argv)
    result = {"success": False, "usbOpenedReadOnly": True, "usbWritesAttempted": False,
              "mountsAttempted": False, "changedSectors": [], "differingBytes": 0}
    destination = None
    try:
        require(sys.platform == "darwin" and os.geteuid() == 0, "run with sudo in your Terminal")
        run = (args.run or Path((WORK / "latest-run.txt").read_text().strip())).resolve(strict=True)
        require(run.parent == WORK.resolve() and not (args.run and args.run.is_symlink()), "invalid run folder")
        record = json.loads((run / "report.json").read_text())
        expected = record["expectedTarget"]
        # Also validate against the original authorization, independent of the
        # mutable test report. No disk-number-only selection is accepted.
        require(expected == json.loads((WORK / "target-readonly.json").read_text())["expectedTarget"],
                "run target differs from the authorized identity")
        helper = ROOT / "build/native-root/tests/rufus_plus_plus_physical_usb_helper"
        device = discover(helper, expected)
        require(not device["mountPoints"], "USB must already be unmounted; diagnostic will not unmount it")
        outside_usb(run, device)
        baseline = run / "capture.dd"
        require(record.get("baselineVerified") is True and sha256(baseline) == record["baselineSha256"],
                "raw baseline is not authenticated")
        destination = run / ("readback-diagnostic-" + str(time.time_ns()) + ".json")
        result["target"] = device
        fd = open_readonly(device)
        try:
            digest = hashlib.sha256()
            with baseline.open("rb") as stream:
                for offset in range(0, expected["capacityBytes"], CHUNK):
                    count = min(CHUNK, expected["capacityBytes"] - offset)
                    before = stream.read(count)
                    require(len(before) == count, "short baseline read")
                    after = read_exact(fd, offset, count)
                    digest.update(after)
                    if before != after:
                        for within in range(0, count, 512):
                            old, new = before[within:within + 512], after[within:within + 512]
                            if old == new:
                                continue
                            require(len(result["changedSectors"]) < 1024, "more than 1,024 sectors differ; diagnostic bounded")
                            changed = [index for index, pair in enumerate(zip(old, new)) if pair[0] != pair[1]]
                            result["differingBytes"] += len(changed)
                            result["changedSectors"].append({"offset": offset + within,
                                "changedOffsetsWithinSector": changed,
                                "baselineBase64": base64.b64encode(old).decode("ascii"),
                                "observedBase64": base64.b64encode(new).decode("ascii")})
                    if offset % (256 * 1024**2) == 0 or offset + count == expected["capacityBytes"]:
                        print("Read-only diagnostic: {}/{} bytes, {} changed sectors".format(
                            offset + count, expected["capacityBytes"], len(result["changedSectors"])), flush=True)
            result["physicalSha256"] = digest.hexdigest()
            result["baselineSha256"] = record["baselineSha256"]
            result["allBytesCompared"] = expected["capacityBytes"]
            with baseline.open("rb") as original:
                def read_baseline(offset, count):
                    original.seek(offset)
                    return original.read(count)
                layout = geometry(read_baseline(0, 512), expected["capacityBytes"])
                require(geometry(read_exact(fd, 0, 512), expected["capacityBytes"]) == layout,
                        "physical FAT32 partition geometry changed")
                old_fat = Fat32(read_baseline, layout)
                new_fat = Fat32(lambda offset, count: read_exact(fd, offset, count), layout)
                result["baselineAllocation"] = allocation_info(old_fat)
                result["physicalAllocation"] = allocation_info(new_fat)
                try:
                    result["independentFiles"] = compare_files(old_fat, new_fat)
                except AssertionError as error:
                    # Preserve the complete byte/sector evidence even when the
                    # strict directory parser rejects an existing host-created
                    # entry. This is a diagnostic, never a passing field test.
                    result["independentFileCheckError"] = str(error)
            require(not discover(helper, expected)["mountPoints"], "USB mounted during inspection")
            result["success"] = True
        finally:
            os.close(fd)
        print(json.dumps({key: value for key, value in result.items() if key != "changedSectors"}, indent=2), flush=True)
        print("Changed sectors: " + str([item["offset"] for item in result["changedSectors"]]), flush=True)
        return 0
    except Exception as error:
        result["error"] = str(error)
        print("FAIL/REFUSED: " + str(error), file=sys.stderr, flush=True)
        return 1
    finally:
        if destination:
            destination.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            if os.environ.get("SUDO_UID", "").isdigit() and os.environ.get("SUDO_GID", "").isdigit():
                os.chown(destination, int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"]))
            print("Diagnostic report: " + str(destination), flush=True)


if __name__ == "__main__":
    sys.exit(main())
