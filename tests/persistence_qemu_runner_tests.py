#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""File-only guards for the optional persistence guest test; never starts QEMU."""
from pathlib import Path
import contextlib
import io
import os
import struct
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import uuid

import persistence_qemu_integration as runner
import physical_persistence_qemu as physical
import test_physical_usb_persistence as hardware
import compare_physical_usb_boot_area as boot_comparison
import boot_physical_usb_writable_qemu as interactive


class FixtureGuards(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.media=Path(self.scratch.name)/'disk.img'
        self.mbr=bytearray(512)
        self.mbr[510:]=b'\x55\xaa'
        self.mbr[450],self.mbr[466]=0x0c,0x83
        self.start=(runner.CAPACITY-runner.PERSISTENCE)//512
        struct.pack_into('<II',self.mbr,454,2048,self.start-2048)
        struct.pack_into('<II',self.mbr,470,self.start,runner.PERSISTENCE//512)
        self.superblock=bytearray(1024)
        self.superblock[56:58]=b'\x53\xef'
        struct.pack_into('<I',self.superblock,4,runner.PERSISTENCE//4096)
        struct.pack_into('<I',self.superblock,32,32768)
        struct.pack_into('<H',self.superblock,58,1)
        self.identity=uuid.uuid4()
        self.superblock[104:120]=self.identity.bytes
        self.superblock[120:129]=b'casper-rw'
        self.save()

    def save(self):
        with self.media.open('wb') as output:
            output.truncate(runner.CAPACITY)
            output.seek(0)
            output.write(self.mbr)
            output.seek(self.start*512+1024)
            output.write(self.superblock)

    def test_valid_layout_preserves_independent_filesystem_identity(self):
        info=runner.layout(self.media)
        self.assertEqual(info['persistenceUuid'],str(self.identity))
        self.assertEqual(info['persistenceBytes'],runner.PERSISTENCE)

    def test_device_is_rejected(self):
        with self.assertRaises(AssertionError):
            runner.ordinary(Path('/dev/null'))

    def test_symlink_is_rejected(self):
        alias=self.media.with_name('alias.img')
        alias.symlink_to(self.media)
        with self.assertRaises(AssertionError):
            runner.layout(alias)

    def test_truncated_disk_is_rejected(self):
        with self.media.open('r+b') as output:
            output.truncate(runner.CAPACITY-512)
        with self.assertRaisesRegex(AssertionError,'geometry'):
            runner.layout(self.media)

    def test_overlapping_partitions_are_rejected(self):
        struct.pack_into('<I',self.mbr,458,self.start)
        self.save()
        with self.assertRaisesRegex(AssertionError,'geometry'):
            runner.layout(self.media)

    def test_physical_geometry_allows_bounded_partition_padding(self):
        capacity = 15472047104
        partition_bytes = runner.PERSISTENCE + 1356800
        start = (capacity - partition_bytes) // 512
        struct.pack_into('<II', self.mbr, 454, 2048, start-2048)
        struct.pack_into('<II', self.mbr, 470, start, partition_bytes//512)
        with self.media.open('wb') as output:
            output.truncate(capacity)
            output.write(self.mbr)
            output.seek(start*512+1024)
            output.write(self.superblock)
        self.assertEqual(runner.layout(self.media, capacity)['persistenceBytes'], partition_bytes)
        self.assertTrue(runner.finalized_counters(self.media, capacity)['clean'])
        with self.assertRaisesRegex(AssertionError, 'geometry'):
            runner.layout(self.media)

    def test_wrong_partition_type_is_rejected(self):
        self.mbr[466]=0x07
        self.save()
        with self.assertRaisesRegex(AssertionError,'MBR'):
            runner.layout(self.media)

    def test_wrong_filesystem_magic_is_rejected(self):
        self.superblock[56:58]=b'\x00\x00'
        self.save()
        with self.assertRaisesRegex(AssertionError,'filesystem'):
            runner.layout(self.media)

    def test_wrong_filesystem_label_is_rejected(self):
        self.superblock[120:129]=b'unrelated'
        self.save()
        with self.assertRaisesRegex(AssertionError,'filesystem'):
            runner.layout(self.media)

    def test_serial_access_failure_stops_the_owned_vm(self):
        code=self.media.with_name('code.fd')
        code.write_bytes(bytes(512))
        process=MagicMock()
        process.poll.return_value=None
        connection=MagicMock()
        connection.connect.side_effect=PermissionError('local socket denied')
        with patch.object(runner.boot,'CODE',code), patch.object(runner.boot,'VARS',code), \
             patch.object(runner.subprocess,'Popen',return_value=process), \
             patch.object(runner.socket,'socket',return_value=connection):
            with self.assertRaises(PermissionError):
                runner.Guest(self.media,self.media.with_name('session'))
        process.terminate.assert_called_once()
        process.wait.assert_called_once()
        connection.close.assert_called_once()

    def test_clean_post_poweroff_counters_are_accepted(self):
        self.assertTrue(runner.finalized_counters(self.media)['clean'])

    def test_unclean_post_poweroff_state_is_rejected(self):
        struct.pack_into('<H',self.superblock,58,0)
        self.save()
        with self.assertRaisesRegex(AssertionError,'finalized clean'):
            runner.finalized_counters(self.media)

    def test_stale_free_block_total_is_rejected(self):
        struct.pack_into('<I',self.superblock,12,1)
        self.save()
        with self.assertRaisesRegex(AssertionError,'disagree'):
            runner.finalized_counters(self.media)

    def test_stale_free_inode_total_is_rejected(self):
        struct.pack_into('<I',self.superblock,16,1)
        self.save()
        with self.assertRaisesRegex(AssertionError,'disagree'):
            runner.finalized_counters(self.media)


class CheckerOutputGuards(unittest.TestCase):
    def clean_output(self):
        return '\n'.join('Pass '+str(number)+': Checking metadata' for number in range(1,6))

    def test_all_clean_checker_passes_are_accepted(self):
        runner.require_clean_fsck(self.clean_output()+'\ncasper-rw: 100/1000 files, 200/2000 blocks\n')

    def test_incorrect_counts_are_rejected_even_with_zero_exit(self):
        with self.assertRaisesRegex(AssertionError,'inconsistency'):
            runner.require_clean_fsck(self.clean_output()+'\nFree blocks count wrong (507585, counted=505405).\nFix? no\n')

    def test_incomplete_checker_run_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,'five passes'):
            runner.require_clean_fsck('Pass 1: Checking inodes\n')


class RangeReadbackGuards(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.source=Path(self.scratch.name)/'source.img'
        self.media=self.source.with_name('written.img')
        self.source.write_bytes(bytes(512)+b'\x6e'*7680)
        self.ranges=[{'offset':0,'size':512},{'offset':7168,'size':1024}]
        expected=bytearray(b'\xa5'*8192)
        expected[:512]=bytes(512)
        expected[7168:]=b'\x6e'*1024
        self.media.write_bytes(expected)

    def test_nonzero_gaps_and_zero_initialized_metadata_are_checked(self):
        result=runner.verify_range_write(self.source,self.media,self.ranges)
        self.assertEqual(result['plannedWriteBytes'],1536)
        self.assertEqual(result['skippedBytesPreserved'],6656)

    def test_skipped_gap_modification_is_detected(self):
        with self.media.open('r+b') as stream:
            stream.seek(2048)
            stream.write(bytes(512))
        with self.assertRaisesRegex(AssertionError,'mismatch'):
            runner.verify_range_write(self.source,self.media,self.ranges)

    def test_failure_to_write_zero_metadata_is_detected(self):
        with self.media.open('r+b') as stream:
            stream.write(b'\xa5'*512)
        with self.assertRaisesRegex(AssertionError,'mismatch'):
            runner.verify_range_write(self.source,self.media,self.ranges)

    def test_invalid_ranges_are_refused(self):
        for ranges in ([],[{'offset':0,'size':0}],[{'offset':8192,'size':512}],
                       [{'offset':0,'size':1024},{'offset':512,'size':512}]):
            with self.subTest(ranges=ranges),self.assertRaises(AssertionError):
                runner.verify_range_write(self.source,self.media,ranges)


class PhysicalCaptureGuards(unittest.TestCase):
    def test_capture_requires_sudo_before_any_device_discovery(self):
        with patch.object(physical.os, 'geteuid', return_value=501), \
             patch.object(physical, 'discover') as discover:
            with self.assertRaisesRegex(AssertionError, 'sudo'):
                physical.capture()
            discover.assert_not_called()

    def test_qemu_test_refuses_root_before_reading_capture(self):
        with patch.object(physical.os, 'geteuid', return_value=0), \
             patch.object(physical, 'checked_capture') as checked:
            with self.assertRaisesRegex(AssertionError, 'without sudo'):
                physical.test_capture()
            checked.assert_not_called()

    def test_changed_capture_hash_is_rejected_before_guest_boot(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            media = work / 'usb-capture.img'
            media.write_bytes(bytes(512))
            expected = {'capacityBytes': 512, 'serial': 'test'}
            report = {'success': True, 'physicalUsbOpenedReadOnly': True,
                      'ejected': True, 'expectedTarget': expected, 'capturedBytes': 512,
                      'capture': str(media), 'wholeDiskSha256': 'old'}
            (work / 'capture-report.json').write_text(json.dumps(report))
            with patch.object(physical, 'WORK', work), \
                 patch.object(physical, 'expected_target', return_value=expected), \
                 patch.object(physical.persistence, 'layout') as layout:
                with self.assertRaisesRegex(AssertionError, 'provenance'):
                    physical.checked_capture()
                layout.assert_not_called()


class PhysicalWriteGuards(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root=Path(self.scratch.name)
        self.media=self.root/'ordinary.img'
        data=bytearray(8192)
        struct.pack_into('<I',data,1024+4,runner.PERSISTENCE//4096)
        struct.pack_into('<I',data,1024+24,2)
        struct.pack_into('<I',data,1024+32,32768)
        struct.pack_into('<H',data,1024+58,1)
        data[1024+56:1024+58]=b'\x53\xef'
        self.media.write_bytes(data)
        self.code=self.root/'code.fd'
        self.code.write_bytes(bytes(512))
        firmware=patch.object(runner.boot,'CODE',self.code)
        firmware.start()
        self.addCleanup(firmware.stop)

    def test_missing_explicit_physical_write_argument_refuses_before_discovery(self):
        with patch.object(hardware.sys,'argv',['test_physical_usb_persistence.py']), \
             patch.object(hardware,'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(hardware.main(),1)
            discover.assert_not_called()

    def test_non_root_refuses_before_discovery(self):
        with patch.object(hardware.sys,'argv',['runner','--write-to-physical-usb']), \
             patch.object(hardware.os,'geteuid',return_value=501), \
             patch.object(hardware,'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(hardware.main(),1)
            discover.assert_not_called()

    def test_interactive_launcher_requires_explicit_write_flag(self):
        with patch.object(interactive.sys, 'argv', ['runner']), \
             patch.object(interactive, 'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(interactive.main(), 1)
            discover.assert_not_called()

    def test_interactive_launcher_refuses_non_root_before_discovery(self):
        with patch.object(interactive.sys, 'argv', ['runner', '--write-to-physical-usb']), \
             patch.object(interactive.os, 'geteuid', return_value=501), \
             patch.object(interactive, 'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(interactive.main(), 1)
            discover.assert_not_called()

    def test_interactive_guest_uses_raw_usb_without_overlay_or_other_disks(self):
        with self.media.open('r+b') as source, \
             patch.object(interactive.physical, 'ISO', self.media), \
             patch.object(interactive.boot, 'CODE', self.media):
            command = interactive.configuration(source.fileno(), self.media, self.root, self.root/'qmp')
        nodes = [hardware.json.loads(command[i+1]) for i,value in enumerate(command) if value=='-blockdev']
        self.assertEqual(len(nodes), 3)
        usb = next(node for node in nodes if node['node-name']=='usb-image')
        self.assertEqual(usb['driver'], 'raw')
        self.assertFalse(usb['read-only'])
        self.assertFalse(usb['file']['read-only'])
        self.assertNotIn('backing', usb)
        self.assertIn('removable=off', next(value for value in command if value.startswith('usb-storage,')))
        self.assertEqual(command[command.index('-nic')+1], 'none')
        self.assertFalse(any(value in command for value in ['-snapshot','-virtfs','-fsdev','-cdrom']))

    def test_interactive_guest_rejects_readonly_handle(self):
        with self.media.open('rb') as source:
            with self.assertRaisesRegex(AssertionError, 'access mode'):
                interactive.configuration(source.fileno(), self.media, self.root, self.root/'qmp')

    def test_interactive_interrupt_requests_shutdown_without_terminating_guest(self):
        process = MagicMock()
        process.poll.side_effect = [None, None, 0]
        process.wait.side_effect = [KeyboardInterrupt(), 0]
        with patch.object(interactive.direct, 'query') as query, \
             contextlib.redirect_stdout(io.StringIO()):
            interactive.wait_until_exit(process, self.root/'qmp')
        query.assert_called_once_with(self.root/'qmp', ['system_powerdown'])
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_regular_file_cannot_be_opened_as_physical_write_target(self):
        with self.media.open('rb') as source:
            with self.assertRaisesRegex(AssertionError,'raw device'):
                hardware.open_readwrite({'devicePath':'/dev/disk123'},source.fileno())

    def test_direct_write_node_has_no_overlay_and_requires_actual_rw_handle(self):
        with self.media.open('r+b') as source:
            node=hardware.physical_node(source.fileno(),'usb-image',False)
            self.assertEqual(node['driver'],'raw')
            self.assertEqual(node['file']['driver'],'host_device')
            self.assertEqual(node['file']['filename'],'/dev/fd/{}'.format(source.fileno()))
            self.assertFalse(node['read-only'])
            self.assertFalse(node['file']['read-only'])
            self.assertFalse(node['file']['cache']['no-flush'])
            self.assertNotIn('backing',node)
            with self.assertRaisesRegex(AssertionError,'access mode'):
                hardware.physical_node(source.fileno(),'inspect-image',True)

    def test_readonly_handle_cannot_run_a_write_guest(self):
        with self.media.open('rb') as source:
            with patch.object(hardware.subprocess,'Popen') as launch:
                with self.assertRaisesRegex(AssertionError,'access mode'):
                    hardware.PhysicalGuest(source.fileno(),self.root/'session')
                launch.assert_not_called()

    def test_writable_handle_cannot_run_readonly_inspection(self):
        with self.media.open('r+b') as source:
            with patch.object(hardware.subprocess,'Popen') as launch:
                with self.assertRaisesRegex(AssertionError,'access mode'):
                    hardware.PhysicalGuest(source.fileno(),self.root/'session',inspect_only=True)
                launch.assert_not_called()

    def test_transport_check_pauses_cpus_and_uses_native_descriptor_path(self):
        with self.media.open('r+b') as source:
            guest=hardware.PhysicalGuest.__new__(hardware.PhysicalGuest)
            guest.physical_fd=source.fileno()
            guest.inspect_only=False
            guest.paused=True
            guest.bootstrap_device=None
            guest.run_as=None
            command=guest._configuration(self.media,self.media,self.root)
            self.assertIn('-S',command)
            self.assertNotIn('-add-fd',command)
            self.assertEqual(guest._display_backend(),'none')
            self.assertEqual(guest._inherited_fds(),(source.fileno(),))

    def test_fresh_backup_matches_read_data_and_is_readonly(self):
        destination=self.root/'backup.img'
        with self.media.open('rb') as source, contextlib.redirect_stdout(io.StringIO()):
            sha=hardware.backup_current_usb(source.fileno(),destination,self.media.stat().st_size)
        self.assertEqual(sha,hardware.digest(self.media))
        self.assertEqual(destination.read_bytes(),self.media.read_bytes())
        self.assertEqual(destination.stat().st_mode & 0o777,0o400)

    def test_backup_cannot_overwrite_existing_file(self):
        destination=self.root/'existing.img'
        destination.write_bytes(b'keep')
        with self.media.open('rb') as source:
            with self.assertRaises(FileExistsError):
                hardware.backup_current_usb(source.fileno(),destination,8192)
        self.assertEqual(destination.read_bytes(),b'keep')

    def test_backup_requires_readonly_input_handle(self):
        with self.media.open('r+b') as source:
            with self.assertRaisesRegex(AssertionError,'read-only'):
                hardware.backup_current_usb(source.fileno(),self.root/'backup.img',8192)

    def test_post_shutdown_header_checks_counters_and_clean_flag(self):
        with self.media.open('rb') as source:
            self.assertTrue(hardware.finalized_header(source.fileno(),{'persistenceOffset':0})['clean'])
        with self.media.open('r+b') as output:
            output.seek(1024+12)
            output.write(struct.pack('<I',1))
        with self.media.open('rb') as source:
            with self.assertRaisesRegex(AssertionError,'counters'):
                hardware.finalized_header(source.fileno(),{'persistenceOffset':0})

    def test_physical_flush_requires_fsync_before_device_cache_command(self):
        events=[]
        with self.media.open('r+b') as source, \
             patch.object(hardware.os,'fsync',side_effect=lambda fd:events.append('fsync')), \
             patch.object(hardware.fcntl,'ioctl',side_effect=lambda fd,op:events.append(op)):
            hardware.flush_physical(source.fileno())
        self.assertEqual(events,['fsync',0x20006416])

    def test_failed_fsync_prevents_successful_flush(self):
        with self.media.open('r+b') as source, \
             patch.object(hardware.os,'fsync',side_effect=OSError('flush failed')), \
             patch.object(hardware.fcntl,'ioctl') as ioctl:
            with self.assertRaisesRegex(OSError,'flush failed'):
                hardware.flush_physical(source.fileno())
            ioctl.assert_not_called()

    def test_active_qemu_refuses_another_physical_test(self):
        result=MagicMock(stdout='/opt/homebrew/bin/qemu-system-aarch64\n')
        with patch.object(hardware.subprocess,'run',return_value=result):
            with self.assertRaisesRegex(AssertionError,'close QEMU'):
                hardware.no_active_guest()

    def test_physical_failure_cleanup_attempts_normal_shutdown(self):
        guest=hardware.PhysicalGuest.__new__(hardware.PhysicalGuest)
        guest.process=MagicMock()
        guest.process.poll.return_value=None
        guest.shell_ready=True
        guest.shutdown=MagicMock()
        with patch.object(runner.Guest,'close') as cleanup:
            guest.close()
        guest.shutdown.assert_called_once()
        cleanup.assert_called_once()

    def test_shutdown_failure_is_reported_and_owned_process_cleanup_still_runs(self):
        guest=hardware.PhysicalGuest.__new__(hardware.PhysicalGuest)
        guest.process=MagicMock()
        guest.process.poll.return_value=None
        guest.shell_ready=True
        guest.shutdown=MagicMock(side_effect=OSError('shutdown failed'))
        with patch.object(runner.Guest,'close') as cleanup, \
             contextlib.redirect_stderr(io.StringIO()) as errors:
            guest.close()
        self.assertIn('shutdown failed',errors.getvalue())
        cleanup.assert_called_once()

    def test_readonly_finishing_mode_refuses_non_root_before_opening_any_device(self):
        with patch.object(hardware.sys,'argv',['runner','--verify-physical-usb']), \
             patch.object(hardware.os,'geteuid',return_value=501), \
             patch.object(hardware,'open_readwrite') as writable, \
             patch.object(hardware,'open_readonly') as readonly, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(hardware.main(),1)
            writable.assert_not_called()
            readonly.assert_not_called()

    def test_post_write_capture_refuses_a_writable_source_handle(self):
        with self.media.open('r+b') as source:
            with self.assertRaisesRegex(AssertionError,'read-only handle'):
                hardware.capture_post_write(source.fileno(),{},self.root,{},os.getuid(),os.getgid())

    def test_boot_mismatch_retains_completed_readback_evidence(self):
        report = {'bootAreaBeforeSha256': 'a'*64}
        snapshot_sha = hardware.hashlib.sha256(bytes(512)).hexdigest()
        with self.media.open('rb') as source, \
             patch.object(hardware, 'finalized_header', return_value={'clean': True}), \
             patch.object(hardware, 'hash_region', side_effect=[snapshot_sha, 'b'*64]):
            with self.assertRaisesRegex(AssertionError, 'boot area differs'):
                hardware.capture_post_write(source.fileno(),
                    {'persistenceOffset': 512, 'persistenceBytes': 512}, self.root,
                    report, os.getuid(), os.getgid())
        self.assertTrue(report['physicalReadbackIndependentlyMatched'])
        self.assertEqual(report['partitionReadbackSha256'], snapshot_sha)
        self.assertEqual(report['bootAreaAfterSha256'], 'b'*64)
        self.assertFalse(report['bootAreaUnchanged'])

    def test_boot_comparison_refuses_non_root_before_discovery(self):
        with patch.object(boot_comparison.sys, 'argv', ['runner']), \
             patch.object(boot_comparison.os, 'geteuid', return_value=501), \
             patch.object(boot_comparison, 'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(boot_comparison.main(), 1)
            discover.assert_not_called()

    def test_boot_comparison_requires_readonly_source(self):
        with self.media.open('r+b') as source:
            with self.assertRaisesRegex(AssertionError, 'read-only'):
                boot_comparison.compare_region(source.fileno(), self.media, 512)

    def test_boot_comparison_requires_complete_bounded_baseline(self):
        with self.media.open('rb') as source:
            for size, message in ((511, 'complete sectors'), (16384, 'truncated')):
                with self.assertRaisesRegex(AssertionError, message):
                    boot_comparison.compare_region(source.fileno(), self.media, size)

    def test_boot_comparison_identical_bytes_match_without_changes(self):
        with self.media.open('rb') as source, contextlib.redirect_stdout(io.StringIO()):
            report = boot_comparison.compare_region(source.fileno(), self.media, 8192)
        self.assertTrue(report['bootAreaUnchanged'])
        self.assertEqual(report['baselineBootAreaSha256'], hardware.digest(self.media))
        self.assertEqual(report['currentBootAreaSha256'], hardware.digest(self.media))
        self.assertEqual(report['changedBytes'], 0)

    def test_boot_comparison_retains_changes_across_chunk_and_end_boundaries(self):
        size = 8*1024**2+512
        baseline, current = self.root/'baseline.img', self.root/'current.img'
        baseline.write_bytes(bytes(size))
        data = bytearray(size)
        for position in (8*1024**2-1, 8*1024**2, size-1): data[position] = 42
        current.write_bytes(data)
        with current.open('rb') as source, contextlib.redirect_stdout(io.StringIO()):
            report = boot_comparison.compare_region(source.fileno(), baseline, size)
        self.assertFalse(report['bootAreaUnchanged'])
        self.assertEqual(report['changedBytes'], 3)
        self.assertEqual(report['changedSectors'], 2)
        self.assertEqual([item['offset'] for item in report['sectorDifferences']],
                         [8*1024**2-512, 8*1024**2])
        self.assertEqual(current.read_bytes(), data)
        self.assertEqual(baseline.read_bytes(), bytes(size))

    def test_boot_comparison_counts_all_differences_when_samples_are_capped(self):
        size = 257*512
        baseline, current = self.root/'baseline.img', self.root/'current.img'
        baseline.write_bytes(bytes(size))
        current.write_bytes(bytes([1])*size)
        with current.open('rb') as source, contextlib.redirect_stdout(io.StringIO()):
            report = boot_comparison.compare_region(source.fileno(), baseline, size)
        self.assertEqual(report['changedBytes'], size)
        self.assertEqual(report['changedSectors'], 257)
        self.assertEqual(len(report['sectorDifferences']), 256)
        self.assertTrue(report['sectorSamplesTruncated'])

    def test_offline_checker_refuses_invalid_fingerprints_before_guest_launch(self):
        with patch.object(hardware.persistence,'Guest') as launch:
            for sha,setting in (('invalid','RUFUSPP_TEST_SETTING='+'a'*32),
                                 ('a'*64,'bad-setting')):
                with self.assertRaisesRegex(AssertionError,'fingerprints'):
                    hardware.inspect_partition_capture(self.media,self.root,sha,setting)
            launch.assert_not_called()

    def test_offline_checker_refuses_device_paths(self):
        with patch.object(hardware.persistence,'Guest') as launch:
            with self.assertRaises(AssertionError):
                hardware.inspect_partition_capture(Path('/dev/null'),self.root,'a'*64,
                                                     'RUFUSPP_TEST_SETTING='+'a'*32)
            launch.assert_not_called()


if __name__=='__main__':
    unittest.main(verbosity=2)
