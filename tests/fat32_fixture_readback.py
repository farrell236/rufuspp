# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent read-only manifest checks using FAT32 long filenames.

macOS may create AppleDouble files whose long names differ but whose short
aliases collide. Record those host aliases without weakening the strict
short-name catalog used for boot-media qualification in fat32_readback.py.
"""
import struct
import unicodedata

from fat32_readback import Fat32
from image_write_integration import require


def fat32_geometry(mbr, capacity):
    require(len(mbr) == 512 and mbr[510:] == b"\x55\xaa", "invalid MBR")
    entries = [mbr[446 + i * 16:462 + i * 16] for i in range(4)]
    used = [entry for entry in entries if entry[4] != 0]
    require(len(used) == 1 and used[0][4] in (0x0b, 0x0c), "expected one FAT32 partition")
    offset, sectors = struct.unpack_from("<II", used[0], 8)
    base, size = offset * 512, sectors * 512
    require(base >= 1024**2 and size > 32 * 1024**2 and size <= capacity - base,
            "FAT32 partition exceeds physical bounds")
    return {"fatOffset": base, "fatBytes": size}


def component(entry, pending):
    short = entry[:11]
    if not pending:
        stem = short[:8].rstrip(b" ").decode("cp437")
        extension = short[8:].rstrip(b" ").decode("cp437")
        if entry[12] & 8:
            stem = stem.lower()
        if entry[12] & 16:
            extension = extension.lower()
        return stem + ("." + extension if extension else "")
    count = pending[0][0] & 31
    require(0 < count <= 20 and len(pending) == count
            and [slot[0] for slot in pending] == [count | 64] + list(range(count - 1, 0, -1)),
            "invalid FAT long-name sequence")
    checksum = 0
    for value in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 255
    require(all(slot[11] == 15 and slot[12] == 0 and slot[13] == checksum
                and slot[26:28] == b"\0\0" for slot in pending), "invalid FAT long-name checksum/type")
    raw = b"".join(slot[1:11] + slot[14:26] + slot[28:32] for slot in reversed(pending))
    units = [raw[index:index + 2] for index in range(0, len(raw), 2)]
    if b"\0\0" in units:
        end = units.index(b"\0\0")
        require(all(unit == b"\xff\xff" for unit in units[end + 1:]), "invalid FAT name padding")
        units = units[:end]
    value = b"".join(units).decode("utf-16le")
    require(0 < len(value.encode("utf-16le")) <= 510 and "/" not in value and "\\" not in value,
            "invalid FAT long filename")
    return value


def catalog(fat):
    files, directories, visited, aliases = {}, set(), set(), []

    def walk(cluster, path):
        require(cluster not in visited and len(visited) < 10000 and len(path) < 32,
                "cyclic or excessive FAT directory tree")
        visited.add(cluster)
        chain = fat.chain(cluster, 16 * 1024**2)
        pending, names, shorts = [], set(), {}
        for block in fat.blocks(chain, len(chain) * fat.cluster_bytes):
            for offset in range(0, len(block), 32):
                entry = block[offset:offset + 32]
                if entry[0] == 0:
                    return
                if entry[0] == 0xe5:
                    pending = []
                    continue
                if entry[11] == 15:
                    pending.append(entry)
                    require(len(pending) <= 20, "excessive FAT long-name slots")
                    continue
                if entry[11] & 8 or entry[:11] in (b".          ", b"..         "):
                    pending = []
                    continue
                name = unicodedata.normalize("NFC", component(entry, pending))
                pending = []
                require(name not in (".", "..") and name not in names, "duplicate FAT long filename")
                names.add(name)
                short = entry[:11]
                if short in shorts:
                    old_name, old_attr = shorts[short]
                    require(old_name.startswith("._") and name.startswith("._")
                            and old_attr & 2 and entry[11] & 2,
                            "duplicate non-AppleDouble FAT short alias")
                    aliases.append({"directory": "/".join(path), "first": old_name, "second": name,
                                    "shortNameHex": short.hex()})
                else:
                    shorts[short] = (name, entry[11])
                key = "/".join(path + (name,))
                child = (struct.unpack_from("<H", entry, 20)[0] << 16) | struct.unpack_from("<H", entry, 26)[0]
                if entry[11] & 16:
                    directories.add(key)
                    walk(child, path + (name,))
                else:
                    require(len(files) < 100000, "excessive FAT file count")
                    files[key] = {"cluster": child, "size": struct.unpack_from("<I", entry, 28)[0]}
    walk(fat.root, ())
    return files, directories, aliases


def verify_raw_fixture(read, layout, manifest, absent=False):
    fat = Fat32(read, layout)
    files, directories, aliases = catalog(fat)
    checked = total = 0
    for entry in manifest["files"]:
        name = unicodedata.normalize("NFC", entry["path"])
        if absent:
            require(name not in files, "fixture remained visible after blank format: " + name)
            continue
        require(name in files, "fixture file missing from raw FAT32: " + name)
        found = files[name]
        require(found["size"] == entry["size"] and fat.checksum(found) == entry["sha256"],
                "raw FAT32 fixture content mismatch: " + name)
        checked += 1
        total += found["size"]
    if not absent:
        require(all(unicodedata.normalize("NFC", name) in directories for name in manifest["directories"]),
                "fixture directory missing from raw FAT32")
        require(checked == manifest["fileCount"] and total == manifest["totalFileBytes"],
                "raw FAT32 manifest totals disagree")
    return {"fixtureAbsent": absent, "filesChecked": checked, "fileBytesChecked": total,
            "directoriesChecked": 0 if absent else len(manifest["directories"]),
            "allSha256Matched": not absent, "hostAppleDoubleAliasCollisions": aliases,
            "hostFilesystemMountUsed": False}
