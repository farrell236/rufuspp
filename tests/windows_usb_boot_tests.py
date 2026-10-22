#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ordinary-file metadata tests; never launch QEMU or open a device."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import boot_windows_physical_usb_qemu as boot


class WindowsUsbBootTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'ordinary-test-file'
        self.capacity = 128 * 1024 * 1024
        self.expected = patch.dict(boot.EXPECTED, capacityBytes=self.capacity)
        self.expected.start()
        self.addCleanup(self.expected.stop)

    def media(self, bios, uefi):
        first = 2048
        sectors = self.capacity // 512 - first - 2048 - (2048 if uefi else 0)
        mbr = bytearray(512)
        if bios:
            mbr[:3] = b'\x33\xc0\x8e'
        mbr[446], mbr[450], mbr[510:] = 0x80, 7, b'\x55\xaa'
        struct.pack_into('<II', mbr, 454, first, sectors)
        if uefi:
            mbr[466] = 0xef
            struct.pack_into('<II', mbr, 470, first + sectors, 2048)
        pbr = bytearray(8192)
        pbr[:11] = b'\xeb\x52\x90NTFS    '
        pbr[510:512] = b'\x55\xaa'
        struct.pack_into('<H', pbr, 11, 512)
        struct.pack_into('<I', pbr, 28, first)
        struct.pack_into('<Q', pbr, 40, sectors - 1)
        if bios:
            pbr[0x54:0x5a] = b'\xfa\x33\xc0\x8e\xd0\xbc'
            pbr[600:614] = 'BOOTMGR'.encode('utf-16-le')
        with self.path.open('wb') as file:
            file.truncate(self.capacity)
            file.write(mbr)
            file.seek(first * 512)
            file.write(pbr)
            file.seek((first + sectors - 1) * 512)
            file.write(pbr[:512])
            if uefi:
                file.seek((first + sectors) * 512 + 510)
                file.write(b'\x55\xaa')
        return first, sectors

    def metadata(self):
        with self.path.open('rb') as file:
            return boot.metadata(file.fileno())

    def test_bios_uefi_and_dual_profiles(self):
        for bios, uefi in ((True, False), (True, True), (False, True)):
            with self.subTest(bios=bios, uefi=uefi):
                first, sectors = self.media(bios, uefi)
                result = self.metadata()
                self.assertEqual(result['windowsBiosBootstrapPresent'], bios)
                self.assertEqual(result['helperFirstSector'], first + sectors if uefi else None)
                self.assertEqual(result['ntfsLastSector'], first + sectors - 1)
                self.assertEqual(result['metadataSha256'], self.metadata()['metadataSha256'])

    def test_metadata_corruption_is_refused(self):
        for offset in (450, 446, 466, 454, 2048 * 512 + 28):
            with self.subTest(offset=offset):
                self.media(True, True)
                with self.path.open('r+b') as file:
                    file.seek(offset)
                    file.write(b'\xff')
                with self.assertRaises(AssertionError):
                    self.metadata()
        first, sectors = self.media(True, False)
        with self.path.open('r+b') as file:
            file.seek((first + sectors - 1) * 512)
            file.write(b'!')
        with self.assertRaisesRegex(AssertionError, 'backup'):
            self.metadata()

    def test_write_descriptor_is_refused(self):
        self.media(True, True)
        with self.path.open('r+b') as file:
            with self.assertRaisesRegex(AssertionError, 'read-only'):
                boot.metadata(file.fileno())

    def test_firmware_commands_keep_physical_backing_readonly(self):
        root = Path(self.temporary.name)
        for bios in (True, False):
            args = boot.configuration(7, root/'overlay', root/'vars', root, root/'qmp', bios=bios)
            node = json.loads(args[args.index('-blockdev') + 1])
            self.assertTrue(node['backing']['read-only'])
            self.assertTrue(node['backing']['file']['read-only'])
            self.assertEqual(node['backing']['file']['filename'], '/dev/fd/7')
            self.assertIn('-S', args)
            self.assertEqual(args[args.index('-nic') + 1], 'none')
            self.assertEqual('-bios' in args, bios)
            self.assertEqual('-drive' in args, not bios)


if __name__ == '__main__':
    unittest.main()
