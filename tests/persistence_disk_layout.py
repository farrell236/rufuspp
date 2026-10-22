#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent bounded MBR/GPT parser for the pinned 512-byte-sector USB tests."""
import hashlib
import struct
import uuid
import zlib

from image_write_integration import require

PERSISTENCE = 2*1024**3
EFI_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
LINUX_TYPE = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le


def layout(read, capacity):
    require(type(capacity) is int and 4*1024**3 <= capacity <= 64*1024**3
            and capacity % 512 == 0, 'unexpected USB capacity')
    def bounded(offset, count):
        require(0 <= offset <= capacity and 0 < count <= capacity-offset
                and offset % 512 == count % 512 == 0, 'layout read exceeds media bounds')
        result = read(offset, count)
        require(len(result) == count, 'short layout read')
        return result
    mbr = bounded(0, 512)
    require(mbr[510:] == b'\x55\xaa', 'invalid MBR signature')
    fingerprint = hashlib.sha256(mbr)
    if mbr[450] == 0xee:
        blocks = capacity//512
        require(struct.unpack_from('<II', mbr, 454) == (1, min(blocks-1, 0xffffffff))
                and mbr[462:510] == bytes(48), 'invalid protective MBR')
        def header(lba, alternate, table_lba):
            raw = bounded(lba*512, 512)
            require(raw[:8] == b'EFI PART' and struct.unpack_from('<I', raw, 8)[0] == 0x10000
                    and struct.unpack_from('<I', raw, 12)[0] == 92
                    and struct.unpack_from('<I', raw, 20)[0] == 0, 'invalid GPT header')
            data = bytearray(raw[:92]); stored_crc = struct.unpack_from('<I', data, 16)[0]
            struct.pack_into('<I', data, 16, 0)
            require(zlib.crc32(data) == stored_crc, 'GPT header CRC mismatch')
            current, backup, first, last = struct.unpack_from('<QQQQ', raw, 24)
            table, count, size, crc = struct.unpack_from('<QIII', raw, 72)
            require((current, backup, table, count, size) == (lba, alternate, table_lba, 128, 128)
                    and 34 <= first <= last == blocks-34 and raw[56:72] != bytes(16),
                    'GPT geometry mismatch')
            entries = bounded(table*512, count*size)
            require(zlib.crc32(entries) == crc, 'GPT partition-array CRC mismatch')
            fingerprint.update(raw); fingerprint.update(entries)
            return raw[56:72], first, last, entries
        primary = header(1, blocks-1, 2)
        backup = header(blocks-1, 1, blocks-33)
        require(primary == backup, 'primary and backup GPT differ')
        entries = primary[3]
        require(entries[:16] == EFI_TYPE and entries[128:144] == LINUX_TYPE
                and entries[256:] == bytes(len(entries)-256), 'unexpected GPT partition types/count')
        unique = [entries[16:32], entries[144:160]]
        require(all(value != bytes(16) for value in unique) and unique[0] != unique[1],
                'GPT partition identities are invalid')
        fat_start, fat_last = struct.unpack_from('<QQ', entries, 32)
        start, last = struct.unpack_from('<QQ', entries, 160)
        require(primary[1] <= fat_start <= fat_last < start <= last == primary[2],
                'GPT partitions overlap or exceed usable space')
        fat_size, size, scheme = fat_last-fat_start+1, last-start+1, 'GPT'
    else:
        require(mbr[450] == 0x0c and mbr[466] == 0x83 and mbr[478:510] == bytes(32),
                'expected FAT32/persistence MBR partitions')
        fat_start, fat_size = struct.unpack_from('<II', mbr, 454)
        start, size = struct.unpack_from('<II', mbr, 470)
        require(start+size == capacity//512, 'MBR persistence end changed')
        scheme = 'MBR'
    require(fat_start >= 2048 and fat_size > 0 and fat_start+fat_size == start
            and PERSISTENCE <= size*512 < PERSISTENCE+2*1024**2,
            'unexpected FAT32/persistence bounds')
    boot = bounded(fat_start*512, 512)
    require(boot[510:] == b'\x55\xaa' and struct.unpack_from('<H', boot, 11)[0] == 512
            and boot[82:90] == b'FAT32   ', 'invalid FAT32 partition boot sector')
    superblock = bounded(start*512+1024, 1024)
    require(superblock[56:58] == b'\x53\xef'
            and superblock[120:136].rstrip(b'\0') == b'casper-rw'
            and struct.unpack_from('<I', superblock, 4)[0] == PERSISTENCE//4096
            and struct.unpack_from('<I', superblock, 24)[0] == 2,
            'unexpected Casper persistence filesystem')
    return {'partitionScheme': scheme, 'fatOffset': fat_start*512, 'fatBytes': fat_size*512,
            'persistenceOffset': start*512, 'persistenceBytes': size*512,
            'persistenceUuid': str(uuid.UUID(bytes=superblock[104:120])),
            'partitionLayoutSha256': fingerprint.hexdigest()}
