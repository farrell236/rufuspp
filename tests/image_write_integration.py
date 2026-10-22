#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real image-file writes, verified after the C++ writer process has exited.

No mounting, raw devices, or privileges. Sources/targets live in a private temp
directory. Byte expectations and GPT fixtures are independent of the C++ writer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

MIB = 1024 * 1024
CHUNK = 64 * 1024
SENTINEL = b"\xa5" * CHUNK


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=120)
    require(result.returncode == 0, "{} failed:\n{}{}".format(command[0], result.stdout, result.stderr))
    return result.stdout


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            result.update(block)
    return result.hexdigest()


def pattern(path, size):
    # A different 512-byte pattern at every sector catches misplaced writes.
    with path.open("xb") as stream:
        for offset in range(0, size, CHUNK):
            block = b"".join(
                hashlib.sha256(struct.pack("<Q", sector)).digest() * 16
                for sector in range(offset // 512, (offset + CHUNK) // 512))
            stream.write(block[:min(CHUNK, size - offset)])


def verify_bytes(source, destination, capacity, clear_tail):
    source_size = source.stat().st_size
    require(destination.stat().st_size == capacity, "wrong destination length")
    expected_hash, actual_hash, image_hash = hashlib.sha256(), hashlib.sha256(), hashlib.sha256()
    with source.open("rb") as original, destination.open("rb") as written:
        for offset in range(0, capacity, CHUNK):
            count = min(CHUNK, capacity - offset)
            data = original.read(min(count, max(0, source_size - offset)))
            expected = data + SENTINEL[:count - len(data)]
            if clear_tail:
                zero_start = max(0, capacity - MIB - offset)
                if zero_start < count:
                    expected = expected[:zero_start] + bytes(count - zero_start)
            actual = written.read(count)
            require(actual == expected, "byte mismatch at block starting {}".format(offset))
            expected_hash.update(expected)
            actual_hash.update(actual)
            image_hash.update(actual[:len(data)])
    require(expected_hash.digest() == actual_hash.digest(), "whole-target SHA-256 mismatch")
    return {"verifiedBytes": capacity, "sourceBytes": source_size,
            "sourceSha256": digest(source), "expectedTargetSha256": expected_hash.hexdigest(),
            "actualTargetSha256": actual_hash.hexdigest(), "actualImageSha256": image_hash.hexdigest()}


def gpt_image(path, capacity):
    pattern(path, capacity)
    sectors = capacity // 512
    entries = bytearray(128 * 128)
    entries[:16] = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le
    entries[16:32] = uuid.UUID("8363f061-6815-448a-a119-0b385a36d718").bytes_le
    struct.pack_into("<QQQ", entries, 32, 2048, sectors - 2049, 0)
    name = "RUFUS WRITE TEST".encode("utf-16le")
    entries[56:56 + len(name)] = name
    array_crc = zlib.crc32(entries)

    def header(current, backup, array_lba):
        data = bytearray(struct.pack(
            "<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0,
            current, backup, 34, sectors - 34,
            uuid.UUID("936cf03e-2d8f-46fd-89c1-8316016c0bb6").bytes_le,
            array_lba, 128, 128, array_crc))
        struct.pack_into("<I", data, 16, zlib.crc32(data))
        return data + bytes(512 - len(data))

    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xee,
                               b"\xff\xff\xff", 1, sectors - 1)
    mbr[510:512] = b"\x55\xaa"
    with path.open("r+b") as stream:
        for offset, data in [(0, mbr), (512, header(1, sectors - 1, 2)),
                             (1024, entries), ((sectors - 33) * 512, entries),
                             ((sectors - 1) * 512, header(sectors - 1, 1, sectors - 33))]:
            stream.seek(offset)
            stream.write(data)


def check_gpt(path):
    sectors = path.stat().st_size // 512
    with path.open("rb") as stream:
        mbr = stream.read(512)
        require(mbr[510:512] == b"\x55\xaa" and mbr[450] == 0xee, "protective MBR missing")
        arrays = []
        for lba, backup in [(1, sectors - 1), (sectors - 1, 1)]:
            stream.seek(lba * 512)
            block = bytearray(stream.read(512))
            fields = struct.unpack_from("<8sIIIIQQQQ16sQIII", block)
            signature, revision, length, crc, reserved, current, other = fields[:7]
            require(signature == b"EFI PART" and revision == 0x10000 and length == 92,
                    "invalid GPT header")
            require(reserved == 0 and current == lba and other == backup, "GPT location mismatch")
            struct.pack_into("<I", block, 16, 0)
            require(zlib.crc32(block[:length]) == crc, "GPT header CRC mismatch")
            first, last, _, array_lba, count, entry_size, array_crc = fields[7:]
            require(first == 34 and last == sectors - 34 and count == 128 and entry_size == 128,
                    "invalid GPT array geometry")
            require(array_lba == (2 if lba == 1 else sectors - 33), "GPT array location")
            stream.seek(array_lba * 512)
            entries = stream.read(count * entry_size)
            require(zlib.crc32(entries) == array_crc, "GPT partition-array CRC mismatch")
            begin, end = struct.unpack_from("<QQ", entries, 32)
            require(first <= begin <= end <= last, "GPT partition outside usable range")
            arrays.append(entries)
        require(arrays[0] == arrays[1], "primary and backup GPT arrays differ")
    return "primary/backup header and partition-array CRCs valid"


def payload_bytes():
    return bytes(range(256)) * 513 + b"Rufus++ known file contents\n"


def populate_fat(path, filesystem):
    """Independently add a multi-cluster file to the full-format source fixture."""
    payload = payload_bytes()
    with path.open("r+b") as stream:
        stream.seek(454)
        start = struct.unpack("<I", stream.read(4))[0]
        stream.seek(start * 512)
        boot = stream.read(512)
        spc = boot[13]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        root_entries = struct.unpack_from("<H", boot, 17)[0]
        spf = struct.unpack_from("<H" if filesystem == "fat16" else "<I", boot,
                                 22 if filesystem == "fat16" else 36)[0]
        root_sectors = (root_entries * 32 + 511) // 512
        fat_start = start + reserved
        data_start = fat_start + 2 * spf + root_sectors
        cluster_bytes = spc * 512
        first_cluster = 2 if filesystem == "fat16" else 3
        clusters = (len(payload) + cluster_bytes - 1) // cluster_bytes
        width = 2 if filesystem == "fat16" else 4
        fmt = "<H" if width == 2 else "<I"
        for copy in range(2):
            for index in range(clusters):
                cluster = first_cluster + index
                following = cluster + 1 if index + 1 < clusters else (0xffff if width == 2 else 0x0fffffff)
                stream.seek((fat_start + copy * spf) * 512 + cluster * width)
                stream.write(struct.pack(fmt, following))
        directory = (fat_start + 2 * spf) * 512 if width == 2 else data_start * 512
        # The production formatter uses the first directory entry for the label.
        for slot in range(16):
            stream.seek(directory + slot * 32)
            if stream.read(1) in (b"\x00", b"\xe5"):
                break
        else:
            raise AssertionError("no free root directory slot")
        entry = bytearray(32)
        entry[:11] = b"PAYLOAD BIN"
        entry[11] = 0x20
        struct.pack_into("<H", entry, 24, 0x5c21)  # 2026-01-01, valid FAT date
        struct.pack_into("<H", entry, 26, first_cluster)
        struct.pack_into("<I", entry, 28, len(payload))
        stream.seek(directory + slot * 32)
        stream.write(entry)
        stream.seek((data_start + (first_cluster - 2) * spc) * 512)
        stream.write(payload)
        if width == 4:
            fsinfo = struct.unpack_from("<H", boot, 48)[0]
            backup_boot = struct.unpack_from("<H", boot, 50)[0]
            for sector in (fsinfo, backup_boot + fsinfo):
                stream.seek((start + sector) * 512 + 488)
                free = struct.unpack("<I", stream.read(4))[0]
                stream.seek((start + sector) * 512 + 488)
                stream.write(struct.pack("<II", free - clusters, first_cluster + clusters))


def check_fat(path, filesystem, work):
    with path.open("rb") as stream:
        mbr = stream.read(512)
        require(mbr[510:512] == b"\x55\xaa", "MBR signature missing")
        start, sectors = struct.unpack_from("<II", mbr, 454)
        require(start == 2048 and (start + sectors) * 512 <= path.stat().st_size,
                "invalid FAT partition bounds")
        stream.seek(start * 512)
        boot = stream.read(512)
        bps = struct.unpack_from("<H", boot, 11)[0]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fat_count = boot[16]
        spf = struct.unpack_from("<H" if filesystem == "fat16" else "<I", boot,
                                 22 if filesystem == "fat16" else 36)[0]
        require(bps == 512 and fat_count == 2 and spf > 0 and boot[510:512] == b"\x55\xaa",
                "invalid FAT boot sector")
        fats = []
        for copy in range(fat_count):
            stream.seek((start + reserved + copy * spf) * bps)
            fats.append(stream.read(spf * bps))
        require(fats[0] == fats[1], "FAT copies differ")
        if filesystem == "fat32":
            backup = struct.unpack_from("<H", boot, 50)[0]
            stream.seek((start + backup) * bps)
            require(stream.read(512) == boot, "FAT32 backup boot sector differs")
        payload_hash = None
        if work.name.endswith("-full"):
            root_entries = struct.unpack_from("<H", boot, 17)[0]
            root_sectors = (root_entries * 32 + bps - 1) // bps
            data_start = start + reserved + fat_count * spf + root_sectors
            directory = start + reserved + fat_count * spf if filesystem == "fat16" else data_start
            stream.seek(directory * bps)
            entries = stream.read(512)
            found = [entries[i:i + 32] for i in range(0, 512, 32)
                     if entries[i:i + 11] == b"PAYLOAD BIN"]
            require(len(found) == 1, "known payload file missing from written directory")
            entry = found[0]
            cluster = struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            contents = bytearray()
            visited = set()
            width, fmt = (2, "<H") if filesystem == "fat16" else (4, "<I")
            while cluster < (0xfff8 if width == 2 else 0x0ffffff8):
                require(cluster >= 2 and cluster not in visited, "invalid/cyclic payload FAT chain")
                visited.add(cluster)
                stream.seek((data_start + (cluster - 2) * boot[13]) * bps)
                contents.extend(stream.read(boot[13] * bps))
                cluster = struct.unpack_from(fmt, fats[0], cluster * width)[0]
                if width == 4:
                    cluster &= 0x0fffffff
            require(bytes(contents[:size]) == payload_bytes() and size == len(payload_bytes()),
                    "known file contents differ after native write")
            payload_hash = hashlib.sha256(contents[:size]).hexdigest()
        partition = work / "filesystem.img"
        stream.seek(start * bps)
        remaining = sectors * bps
        with partition.open("xb") as output:
            while remaining:
                data = stream.read(min(CHUNK, remaining))
                require(data, "truncated FAT partition")
                output.write(data)
                remaining -= len(data)
    checker = "/sbin/fsck_msdos" if platform.system() == "Darwin" else shutil.which("fsck.fat")
    if checker and Path(checker).is_file():
        output = run([checker, "-n", partition])
        result = {"checker": str(checker), "exitCode": 0, "output": output}
    else:
        result = {"checker": None, "status": "unavailable; BPB, FAT copies and backup boot checked"}
    if payload_hash:
        result["payloadSha256"] = payload_hash
    partition.unlink()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    helper = args.helper.resolve()
    # Never leave a previous successful report behind after a failing run.
    args.report.unlink(missing_ok=True)
    report = {"scope": "ordinary image files; real native writes and file flush; no raw devices",
              "host": platform.platform(), "cases": []}
    cases = [("raw-512", 12 * MIB, 512, 4096, False, None, 7 * MIB + 512),
             ("raw-4096", 12 * MIB, 4096, CHUNK, False, None, 9 * MIB + 4096),
             ("fat16-full", 16 * MIB, 512, CHUNK, False, "fat16", None),
             ("fat32-full", 128 * MIB, 512, CHUNK, False, "fat32", None),
             ("fat32-quick-clear-tail", 128 * MIB, 512, CHUNK, True, "fat32", None),
             ("gpt-full", 32 * MIB, 512, CHUNK, False, "gpt", None)]
    with tempfile.TemporaryDirectory(prefix="rufuspp-image-write-") as temporary:
        root = Path(temporary)
        control = root / "untouched-control.bin"
        pattern(control, CHUNK)
        control_hash = digest(control)
        for name, capacity, sector, block, tail, filesystem, source_size in cases:
            work = root / name
            work.mkdir()
            source, target = work / "source.img", work / "written.img"
            if filesystem in ("fat16", "fat32"):
                run([helper, "--stage", filesystem, "quick" if tail else "full", capacity, source])
                if not tail:
                    populate_fat(source, filesystem)
            elif filesystem == "gpt":
                gpt_image(source, capacity)
            else:
                pattern(source, source_size)
            source_hash = digest(source)
            writer = json.loads(run([helper, "--write", source, target, capacity, sector, block, int(tail)]))
            require(writer["success"] and writer["verificationCompleted"], "writer did not verify")
            result = {"name": name, "sectorBytes": sector, "blockBytes": block,
                      "clearTail": tail, "writer": writer}
            result.update(verify_bytes(source, target, capacity, tail))
            require(writer["bytesWritten"] == result["sourceBytes"], "writer byte count mismatch")
            require(writer["bytesVerified"] == result["sourceBytes"] + (MIB if tail else 0),
                    "writer verification count mismatch")
            require(source_hash == digest(source), "source changed during write")
            if filesystem in ("fat16", "fat32"):
                result["filesystem"] = check_fat(target, filesystem, work)
            elif filesystem == "gpt":
                result["partitionTable"] = check_gpt(target)
            # Prove the independent verifier actually detects a corrupted write.
            if name == "raw-512":
                with target.open("r+b") as stream:
                    stream.seek(12345)
                    original = stream.read(1)
                    stream.seek(12345)
                    stream.write(bytes([original[0] ^ 1]))
                detected = False
                try:
                    verify_bytes(source, target, capacity, tail)
                except AssertionError:
                    detected = True
                require(detected, "independent verifier failed to detect corruption")
                with target.open("r+b") as stream:
                    stream.seek(12345)
                    stream.write(original)
                    stream.flush()
                    os.fsync(stream.fileno())
                verify_bytes(source, target, capacity, tail)
                result["corruptionDetected"] = True
                # Existing destination and source-as-destination must be refused.
                for protected in (target, source):
                    before = digest(protected)
                    refused = subprocess.run([str(helper), "--write", str(source), str(protected),
                                              str(capacity), str(sector), str(block), "0"],
                                             capture_output=True, text=True, timeout=30)
                    require(refused.returncode != 0 and "must not already exist" in refused.stderr,
                            "existing destination was not refused before writing")
                    require(before == digest(protected), "refused destination changed")
                result["existingDestinationRefused"] = True
            require(digest(control) == control_hash, "non-target control changed")
            report["cases"].append(result)
            print("PASS {}: {} bytes independently verified, SHA-256 {}".format(
                name, capacity, result["actualTargetSha256"]), flush=True)
        # A ranged native write must retain both GPT copies at their true disk
        # offsets while leaving the intervening, deliberately dirty gap alone.
        work = root / "gpt-initialized-ranges"
        work.mkdir()
        source, target = work / "source.img", work / "written.img"
        capacity = 32 * MIB
        gpt_image(source, capacity)
        original = source.read_bytes()
        expected = original[:2 * MIB] + b"\xa5" * (29 * MIB) + original[-MIB:]
        writer = json.loads(run([helper, "--write-ranges", source, target, capacity,
                                 512, CHUNK, 0, 0, 2 * MIB, 31 * MIB, MIB]))
        require(writer["success"] and writer["verificationCompleted"]
                and writer["bytesWritten"] == writer["bytesVerified"]
                == writer["nativeBytesWritten"] == writer["nativeBytesRead"] == 3 * MIB,
                "GPT ranges must transfer/verify only the initialized data")
        require(target.read_bytes() == expected, "ranged GPT bytes or untouched gap differ")
        require(source.read_bytes() == original, "ranged GPT source changed")
        result = {"name":"gpt-initialized-ranges", "writer":writer,
                  "partitionTable":check_gpt(target), "verifiedBytes":capacity,
                  "gapBytesPreserved":29 * MIB,
                  "actualTargetSha256":digest(target)}
        report["cases"].append(result)
        require(digest(control) == control_hash, "ranged write changed non-target control")
        print("PASS gpt-initialized-ranges: 3 MiB written; 32 MiB independently compared", flush=True)
        report["controlSha256"] = control_hash
        report["success"] = True
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Report: {}".format(args.report))


if __name__ == "__main__":
    main()
