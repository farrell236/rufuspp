#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No VM/device access: private-file capture tests and command safety checks."""
import contextlib
import hashlib
import io
import json
import os
import stat
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import capture_usb_for_qemu as capture
import qemu_usb_boot as boot
import boot_physical_usb_qemu as direct


class CaptureTests(unittest.TestCase):
    def test_full_disk_and_partial_image_hashes_match_independently(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ordinary.img"
            data = bytes(range(256)) * (3 * 1024 * 1024 // 256 + 2)
            path.write_bytes(data)
            fd = os.open(path, os.O_RDONLY)
            try:
                output = io.BytesIO()
                image_size = 1024 * 1024 + 512
                with contextlib.redirect_stdout(io.StringIO()):
                    disk_sha, image_sha = capture.copy_readonly(fd, output, len(data), image_size)
                self.assertEqual(output.getvalue(), data)
                self.assertEqual(disk_sha, hashlib.sha256(data).hexdigest())
                self.assertEqual(image_sha, hashlib.sha256(data[:image_size]).hexdigest())
            finally:
                os.close(fd)

    def test_short_local_output_write_is_refused(self):
        class ShortOutput:
            def write(self, _):
                return 0
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ordinary.img"
            path.write_bytes(bytes(512))
            with path.open("rb") as source:
                with self.assertRaisesRegex(AssertionError, "short local"):
                    capture.copy_readonly(source.fileno(), ShortOutput(), 512, 512)

    def test_truncated_usb_read_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ordinary.img"
            path.write_bytes(bytes(512))
            with path.open("rb") as source:
                with self.assertRaisesRegex(AssertionError, "unexpected end"):
                    capture.copy_readonly(source.fileno(), io.BytesIO(), 1024, 512)


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.media, self.code, self.variables = [self.root / name for name in ("usb.img", "code.fd", "vars.fd")]
        for path in (self.media, self.code, self.variables):
            path.write_bytes(bytes(512))
        # Every command-only test uses private firmware, including the direct
        # USB launcher paths. No Homebrew/QEMU installation is needed in CI.
        firmware = patch.object(boot, "CODE", self.code)
        firmware.start()
        self.addCleanup(firmware.stop)

    def command(self, smoke=False):
        with patch.object(boot, "CODE", self.code):
            return boot.configuration(self.media, self.variables, self.root, smoke)

    def test_only_captured_usb_is_readonly_and_only_private_nvram_is_writable(self):
        command = self.command()
        nodes = [json.loads(command[i + 1]) for i, value in enumerate(command) if value == "-blockdev"]
        self.assertEqual(len(nodes), 3)
        self.assertEqual([node["node-name"] for node in nodes if not node["read-only"]], ["efi-vars"])
        usb = next(node for node in nodes if node["node-name"] == "usb-image")
        self.assertTrue(usb["read-only"])
        self.assertTrue(usb["file"]["read-only"])
        self.assertEqual(usb["file"]["filename"], str(self.media))

    def test_guest_has_no_network_shared_folders_or_host_devices(self):
        command = self.command()
        self.assertIn("-no-user-config", command)
        self.assertIn("-nodefaults", command)
        self.assertEqual(command[command.index("-nic") + 1], "none")
        self.assertFalse(any(value in command for value in ("-virtfs", "-fsdev", "-cdrom", "-kernel")))
        self.assertFalse(any("usb-host" in value or '"/dev/' in value for value in command))

    def test_boots_as_usb_storage_through_uefi(self):
        command = self.command()
        self.assertIn("virt,pflash0=efi-code,pflash1=efi-vars", command)
        self.assertIn("usb-storage,bus=xhci.0,drive=usb-image,removable=on,bootindex=1", command)
        self.assertEqual(command[command.index("-accel") + 1], "hvf")

    def test_smoke_configuration_is_headless_and_paused(self):
        command = self.command(True)
        self.assertIn("-S", command)
        self.assertEqual(command[command.index("-display") + 1], "none")

    def test_image_paths_are_json_escaped(self):
        media = self.root / 'comma,quote".img'
        media.write_bytes(bytes(512))
        self.assertEqual(json.loads(boot.block_node("media", media, True))["file"]["filename"], str(media))

    def test_non_file_media_is_refused(self):
        with self.assertRaises(AssertionError):
            boot.block_node("media", Path("/dev/null"), True)

    def test_direct_usb_backing_is_readonly_with_only_local_overlay_writable(self):
        command=direct.configuration(7,self.media,self.variables,self.root,self.root/'qmp')
        nodes=[json.loads(command[i+1]) for i,value in enumerate(command) if value=='-blockdev']
        usb=next(node for node in nodes if node['node-name']=='usb-image')
        self.assertEqual(usb['driver'],'qcow2')
        self.assertFalse(usb['read-only'])
        self.assertEqual(usb['file']['filename'],str(self.media))
        self.assertTrue(usb['backing']['read-only'])
        self.assertTrue(usb['backing']['file']['read-only'])
        self.assertEqual(usb['backing']['file']['driver'],'host_device')
        self.assertEqual(usb['backing']['file']['filename'],'/dev/fd/7')
        self.assertNotIn('-add-fd',command)
        self.assertEqual(command[command.index('-nic')+1],'none')
        self.assertNotIn('-snapshot',command)

    def test_direct_usb_rejects_stdio_or_invalid_descriptor(self):
        for fd in (0,1,2,-1,'7'):
            with self.subTest(fd=fd),self.assertRaises(AssertionError):
                direct.configuration(fd,self.media,self.variables,self.root,self.root/'qmp')

    def test_direct_usb_refuses_a_writable_physical_handle(self):
        with self.media.open('r+b') as stream:
            with self.assertRaisesRegex(AssertionError,'read-only'):
                direct.media_identity(stream.fileno(),512,'unused')

    def test_direct_usb_refuses_non_root_before_discovery(self):
        with patch.object(direct.os,'geteuid',return_value=501), \
             patch.object(direct.sys,'argv',['boot_physical_usb_qemu.py']), \
             patch.object(direct,'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(direct.main(),1)
            discover.assert_not_called()

    def test_root_bootstrap_opens_only_verified_raw_node_and_keeps_guest_paused(self):
        device={'devicePath':'/dev/disk123'}
        inspected=MagicMock(st_mode=stat.S_IFCHR,st_rdev=123)
        command=direct.configuration(7,self.media,self.variables,self.root,self.root/'qmp')
        with patch.object(direct.os,'geteuid',return_value=0), \
             patch.object(direct.os,'lstat',return_value=inspected), \
             patch.object(direct.os,'fstat',return_value=inspected):
            bootstrap=direct.root_bootstrap(command,7,device,501,20)
        self.assertIn('-S',bootstrap)
        self.assertEqual(bootstrap[bootstrap.index('-run-with')+1],'user=501:20')
        nodes=[json.loads(bootstrap[i+1]) for i,value in enumerate(bootstrap) if value=='-blockdev']
        usb=next(node for node in nodes if node['node-name']=='usb-image')
        self.assertEqual(usb['backing']['file']['filename'],'/dev/rdisk123')
        self.assertTrue(usb['backing']['file']['read-only'])
        self.assertNotIn('-run-with',command)

    def test_root_bootstrap_refuses_a_changed_raw_device(self):
        command=direct.configuration(7,self.media,self.variables,self.root,self.root/'qmp')
        with patch.object(direct.os,'geteuid',return_value=0), \
             patch.object(direct.os,'lstat',return_value=MagicMock(st_mode=stat.S_IFCHR,st_rdev=123)), \
             patch.object(direct.os,'fstat',return_value=MagicMock(st_mode=stat.S_IFCHR,st_rdev=456)):
            with self.assertRaisesRegex(AssertionError,'verified physical handle'):
                direct.root_bootstrap(command,7,{'devicePath':'/dev/disk123'},501,20)

    def test_root_bootstrap_refuses_to_run_guest_as_root(self):
        with patch.object(direct.os,'geteuid',return_value=0):
            with self.assertRaisesRegex(AssertionError,'normal destination user'):
                direct.root_bootstrap([],7,{'devicePath':'/dev/disk123'},0,0)

    def test_guest_uid_check_rejects_root_before_any_device_or_guest_operations(self):
        result=MagicMock(stdout='0 0\n')
        with patch.object(direct.subprocess,'run',return_value=result), \
             patch.object(direct,'discover') as discover:
            with self.assertRaisesRegex(AssertionError,'drop both real and effective root'):
                direct.verify_privilege_drop(123,7,{'devicePath':'/dev/disk123'},501)
            discover.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
