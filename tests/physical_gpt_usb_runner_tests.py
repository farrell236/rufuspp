#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""In-memory GPT corruption cases and refusal guards. No guest or device opens."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import uuid
import zlib

import persistence_disk_layout as disk
import test_physical_gpt_usb as runner
import boot_physical_usb_qemu as direct
import boot_physical_usb_writable_qemu as interactive


class GptLayoutTests(unittest.TestCase):
    def setUp(self):
        self.capacity = 8*1024**3
        blocks = self.capacity//512
        self.start = ((blocks-33-disk.PERSISTENCE//512)//2048)*2048
        self.regions = {}
        mbr = bytearray(512); mbr[510:] = b'\x55\xaa'; mbr[450] = 0xee
        struct.pack_into('<II', mbr, 454, 1, blocks-1)
        self.regions[0] = mbr
        entries = bytearray(16384)
        for i,(kind,first,last) in enumerate([(disk.EFI_TYPE,2048,self.start-1),
                                               (disk.LINUX_TYPE,self.start,blocks-34)]):
            p=i*128; entries[p:p+16]=kind
            entries[p+16:p+32]=uuid.UUID(int=i+1).bytes_le
            struct.pack_into('<QQ', entries, p+32, first, last)
        for lba,other,table in [(1,blocks-1,2),(blocks-1,1,blocks-33)]:
            raw=bytearray(512)
            struct.pack_into('<8sIIIIQQQQ16sQIII',raw,0,b'EFI PART',0x10000,92,0,0,
                             lba,other,34,blocks-34,uuid.UUID(int=3).bytes_le,table,128,128,zlib.crc32(entries))
            self.regions[lba*512]=raw
            self.regions[table*512]=bytearray(entries)
            self.header_crc(lba*512)
        boot=bytearray(512); boot[510:]=b'\x55\xaa'; boot[82:90]=b'FAT32   '
        struct.pack_into('<H',boot,11,512)
        self.regions[2048*512]=boot
        sb=bytearray(1024); sb[56:58]=b'\x53\xef'; sb[120:129]=b'casper-rw'
        struct.pack_into('<I',sb,4,disk.PERSISTENCE//4096); struct.pack_into('<I',sb,24,2)
        sb[104:120]=uuid.UUID(int=4).bytes
        self.regions[self.start*512+1024]=sb

    def header_crc(self,offset):
        raw=self.regions[offset]; struct.pack_into('<I',raw,16,0)
        struct.pack_into('<I',raw,16,zlib.crc32(raw[:92]))

    def read(self,offset,count):
        self.assertGreaterEqual(offset,0); self.assertLessEqual(offset+count,self.capacity)
        data=bytearray(count)
        for position,raw in self.regions.items():
            lo,hi=max(offset,position),min(offset+count,position+len(raw))
            if lo<hi: data[lo-offset:hi-offset]=raw[lo-position:hi-position]
        return bytes(data)

    def test_valid_primary_and_backup_gpt_with_bounded_persistence(self):
        result=disk.layout(self.read,self.capacity)
        self.assertEqual(result['partitionScheme'],'GPT')
        self.assertEqual(result['persistenceOffset'],self.start*512)
        self.assertEqual(result['persistenceUuid'],str(uuid.UUID(int=4)))
        self.assertEqual(result['persistenceOffset']+result['persistenceBytes'],self.capacity-33*512)

    def test_primary_header_corruption_is_rejected(self):
        self.regions[512][56]^=1
        with self.assertRaisesRegex(AssertionError,'header CRC'): disk.layout(self.read,self.capacity)

    def test_backup_header_corruption_is_rejected(self):
        self.regions[self.capacity-512][56]^=1
        with self.assertRaisesRegex(AssertionError,'header CRC'): disk.layout(self.read,self.capacity)

    def test_primary_partition_array_corruption_is_rejected(self):
        self.regions[1024][16]^=1
        with self.assertRaisesRegex(AssertionError,'array CRC'): disk.layout(self.read,self.capacity)

    def test_backup_partition_array_corruption_is_rejected(self):
        self.regions[self.capacity-33*512][16]^=1
        with self.assertRaisesRegex(AssertionError,'array CRC'): disk.layout(self.read,self.capacity)

    def test_valid_but_different_backup_guid_is_rejected(self):
        self.regions[self.capacity-512][56]^=1; self.header_crc(self.capacity-512)
        with self.assertRaisesRegex(AssertionError,'GPT differ'): disk.layout(self.read,self.capacity)

    def test_recomputed_crc_cannot_hide_out_of_bounds_array(self):
        struct.pack_into('<Q',self.regions[512],72,self.capacity//512+1); self.header_crc(512)
        with self.assertRaisesRegex(AssertionError,'geometry'): disk.layout(self.read,self.capacity)

    def test_short_read_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,'short layout'): disk.layout(lambda o,n:b'',self.capacity)

    def test_wrong_filesystem_is_rejected(self):
        self.regions[self.start*512+1024][56]=0
        with self.assertRaisesRegex(AssertionError,'persistence filesystem'): disk.layout(self.read,self.capacity)

    def test_hybrid_protective_mbr_is_rejected(self):
        self.regions[0][466]=0x83
        with self.assertRaisesRegex(AssertionError,'protective MBR'): disk.layout(self.read,self.capacity)

    def test_interactive_identity_checks_both_gpt_copies(self):
        with patch.object(direct.fcntl,'fcntl',return_value=os.O_RDONLY), \
             patch.object(direct,'read_exact',side_effect=lambda fd,o,n:self.read(o,n)):
            result=direct.media_identity(7,self.capacity,str(uuid.UUID(int=4)))
            self.assertEqual(result['partitionLayoutSha256'],disk.layout(self.read,self.capacity)['partitionLayoutSha256'])
            self.regions[self.capacity-512][56]^=1
            with self.assertRaisesRegex(AssertionError,'header CRC'):
                direct.media_identity(7,self.capacity,str(uuid.UUID(int=4)))

    def test_interactive_identity_refuses_a_different_persistence_uuid(self):
        with patch.object(direct.fcntl,'fcntl',return_value=os.O_RDONLY), \
             patch.object(direct,'read_exact',side_effect=lambda fd,o,n:self.read(o,n)):
            with self.assertRaisesRegex(AssertionError,'identity differs'):
                direct.media_identity(7,self.capacity,str(uuid.UUID(int=5)))


class PhysicalGptGuards(unittest.TestCase):
    def test_manual_gpt_profile_requires_completed_physical_checks(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); parent=root/'build/physical-gpt-usb'
            session=parent/'run'; session.mkdir(parents=True)
            (parent/'prepared.json').write_text(json.dumps({'session':str(session)}))
            flags=('checkerUsesPhysicalUsb','checkerPhysicalUsbReadOnly','filesystemCheckPassed',
                   'inspectionPersistenceBytesUnchanged','independentPhysicalFileAndSettingMatched',
                   'bootAreaUnchanged','backupGptRegionUnchanged','ejected')
            expected={'serial':'fixture'}
            profile=dict.fromkeys(flags,True)
            profile.update(success=True,state='complete',partitionScheme='GPT',expectedTarget=expected)
            record=session/'report.json'
            with patch.object(interactive.direct,'ROOT',root):
                record.write_text(json.dumps(profile))
                self.assertEqual(interactive.qualified_profile(expected,gpt=True),profile)
                for changed in [{'state':'checking-physical-filesystem-read-only'},
                                {'success':False},{'error':'late failure'}]+[{key:False} for key in flags]:
                    with self.subTest(changed=changed):
                        record.write_text(json.dumps(dict(profile,**changed)))
                        with self.assertRaisesRegex(AssertionError,'completed physical GPT'):
                            interactive.qualified_profile(expected,gpt=True)

    def test_no_backup_does_not_grant_erase_authorization_by_itself(self):
        with patch.object(runner.sys,'argv',['runner','--no-backup']), \
             patch.object(runner,'write_and_test') as write:
            with self.assertRaisesRegex(AssertionError,'explicit'): runner.main()
            write.assert_not_called()

    def test_explicit_no_backup_flag_reaches_authorized_runner(self):
        with patch.object(runner.sys,'argv',['runner','--erase-authorized-kingston','--no-backup']), \
             patch.object(runner,'write_and_test',return_value=0) as write:
            self.assertEqual(runner.main(),0)
            write.assert_called_once_with(make_backup=False)

    def test_missing_erase_authorization_never_discovers_a_device(self):
        with patch.object(runner.sys,'argv',['runner']),patch.object(runner,'discover') as discover:
            with self.assertRaisesRegex(AssertionError,'explicit'): runner.main()
            discover.assert_not_called()

    def test_non_root_never_discovers_a_device(self):
        with patch.object(runner.os,'geteuid',return_value=501),patch.object(runner,'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.write_and_test(),1); discover.assert_not_called()

    def test_prepare_refuses_root_before_any_device_or_file_work(self):
        with patch.object(runner.os,'geteuid',return_value=0),patch.object(runner.capture,'expected_target') as target:
            with self.assertRaisesRegex(AssertionError,'without sudo'): runner.prepare()
            target.assert_not_called()

    def test_ranges_require_both_gpt_copies(self):
        with self.assertRaisesRegex(AssertionError,'both GPT'):
            runner.validate_ranges([{'offset':0,'size':1024**2}],8*1024**3)

    def test_ranges_reject_overlap_and_overflow(self):
        for ranges in [[{'offset':0,'size':2*1024**2},{'offset':1024**2,'size':1024**2}],
                       [{'offset':0,'size':9*1024**3}]]:
            with self.assertRaisesRegex(AssertionError,'invalid initialized'): runner.validate_ranges(ranges,8*1024**3)

    def test_valid_ranges_include_backup_at_actual_device_end(self):
        capacity=15472047104
        self.assertEqual(runner.validate_ranges([{'offset':0,'size':1024**2},
            {'offset':capacity-16896,'size':16896}],capacity),1024**2+16896)

    def test_retry_refuses_without_overwriting_previous_failed_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); work=root/'run'; work.mkdir()
            (root/'prepared.json').write_text(json.dumps({'session':str(work)}))
            (work/'prepared.json').write_text(json.dumps({'prepared':True,'expectedTarget':{}}))
            record=work/'report.json'; record.write_text('{"historical":true}\n')
            with patch.object(runner,'WORK',root),patch.object(runner.os,'geteuid',return_value=0), \
                 patch.dict(os.environ,{'SUDO_UID':'501','SUDO_GID':'20'}), \
                 patch.object(runner.hardware,'no_active_guest'), \
                 patch.object(runner.capture,'expected_target',return_value={}), \
                 patch.object(runner,'open_readonly') as opened, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.write_and_test(),1); opened.assert_not_called()
            self.assertEqual(record.read_text(),'{"historical":true}\n')

    def test_failed_final_inspection_cannot_inherit_successful_boot_verdict(self):
        # Only ordinary temporary files are opened. All device/guest operations
        # are replaced, so this covers report failure handling without a USB.
        with tempfile.TemporaryDirectory() as folder, contextlib.ExitStack() as mocks:
            root=Path(folder); work=root/'run'; work.mkdir()
            source=work/'source.img'; source.write_bytes(bytes(2048))
            expected={'capacityBytes':2048,'serial':'fixture','stableId':'fixture','sectorBytes':512}
            geometry={'partitionScheme':'GPT','persistenceOffset':512,
                      'persistenceBytes':1024,'persistenceUuid':'new'}
            (root/'prepared.json').write_text(json.dumps({'session':str(work)}))
            (work/'prepared.json').write_text(json.dumps({
                'prepared':True,'expectedTarget':expected,'staged':str(source),
                'stagedSha256':runner.capture.ISO_SHA,'geometry':geometry,
                'writeRanges':[{'offset':0,'size':2048}]}))
            previous=root/'previous'; previous.mkdir()
            (previous/'capture-report.json').write_text(json.dumps({'geometry':{'persistenceUuid':'old'}}))
            record=work/'report.json'
            def native_write(args,stdout):
                stdout.write(json.dumps({'success':True,'verificationCompleted':True,
                                         'destructiveWriteStarted':True}))
                return SimpleNamespace(returncode=0)
            def failed_inspection(*args):
                interim=json.loads(record.read_text())
                self.assertTrue(interim['persistenceDataSurvived'])
                self.assertFalse(interim['success'])
                raise OSError('final physical checker failed')
            replacements=[
                (runner,'WORK',root),(runner.os,'geteuid',lambda:0),
                (runner.os,'chown',lambda *args:None),
                (runner.capture,'WORK',previous),(runner.capture,'ISO',source),
                (runner.capture,'expected_target',lambda:expected),
                (runner.hardware,'no_active_guest',lambda:None),
                (runner,'digest',lambda path:runner.capture.ISO_SHA),
                (runner,'file_layout',lambda *args:geometry),
                (runner,'validate_ranges',lambda *args:2048),
                (runner,'command',lambda args:json.dumps({'success':True,'capacityBytes':2048}).encode()),
                (runner,'discover',lambda *args,**kwargs:{'devicePath':'/dev/fixture','mountPoints':[]}),
                (runner,'open_readonly',lambda device:os.open(source,os.O_RDONLY)),
                (runner.subprocess,'run',native_write),
                (runner.hardware,'hash_region',lambda *args:'baseline'),
                (runner.hardware,'open_readwrite',lambda *args:os.open(source,os.O_RDWR)),
                (runner.hardware,'check_transport',lambda *args:None),
                (runner.persistence,'exercise_persistence',lambda *args:{
                    'success':True,'persistenceDataSurvived':True}),
                (runner,'inspect_physical',failed_inspection)]
            for target,name,value in replacements:
                mocks.enter_context(patch.object(target,name,value))
            mocks.enter_context(patch.object(runner,'physical_layout',side_effect=[
                {'partitionScheme':'MBR','persistenceUuid':'old'},geometry]))
            mocks.enter_context(patch.dict(os.environ,{'SUDO_UID':'501','SUDO_GID':'20'}))
            with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.write_and_test(make_backup=False),1)
            result=json.loads(record.read_text())
            self.assertFalse(result['success'])
            self.assertTrue(result['persistenceDataSurvived'])
            self.assertEqual(result['error'],'final physical checker failed')
            self.assertNotEqual(result['state'],'complete')
            self.assertNotIn('ejected',result)


if __name__=='__main__':
    unittest.main(verbosity=2)
