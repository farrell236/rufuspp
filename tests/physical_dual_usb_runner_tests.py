#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Memory/file-only guards for the manual physical cross-firmware test."""
import contextlib
import io
import json
import os
import errno
import struct
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import test_physical_dual_usb as dual
import fat32_readback


class DualGuards(unittest.TestCase):
    def test_continuation_never_dispatches_a_rewrite(self):
        with patch.object(dual.sys,'argv',['runner','--continue-written-usb']), \
             patch.object(dual,'continue_written',return_value=0) as continuation, \
             patch.object(dual.shared,'write_and_test') as write:
            self.assertEqual(dual.main(),0);continuation.assert_called_once_with();write.assert_not_called()

    def test_nonroot_continuation_never_discovers_a_device(self):
        with patch.object(dual.os,'geteuid',return_value=501),patch.object(dual.shared,'discover') as discover, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(dual.continue_written(),1);discover.assert_not_called()

    def test_busy_open_closes_handle_and_revalidates_identity_before_retry(self):
        device={'devicePath':'/dev/disk99','mountPoints':[]}
        with patch.object(dual.shared,'discover',return_value=device) as discover, \
             patch.object(dual.shared,'command') as command, \
             patch.object(dual.shared,'open_readonly',side_effect=[7,8]), \
             patch.object(dual.shared.hardware,'open_readwrite',side_effect=[OSError(errno.EBUSY,'busy'),9]), \
             patch.object(dual.shared.os,'close') as close,patch.object(dual.shared.time,'sleep'), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(dual.shared.acquire_unmounted_handles({},{}),(device,8,9))
            close.assert_called_once_with(7)
            self.assertEqual(discover.call_count,5);self.assertEqual(command.call_count,2)

    def test_non_busy_open_error_is_not_retried(self):
        device={'devicePath':'/dev/disk99','mountPoints':[]}
        with patch.object(dual.shared,'discover',return_value=device),patch.object(dual.shared,'command'), \
             patch.object(dual.shared,'open_readonly',return_value=7), \
             patch.object(dual.shared.hardware,'open_readwrite',side_effect=OSError(errno.EACCES,'denied')), \
             patch.object(dual.shared.os,'close') as close,patch.object(dual.shared.time,'sleep') as sleep:
            with self.assertRaises(PermissionError):dual.shared.acquire_unmounted_handles({},{})
            close.assert_called_once_with(7);sleep.assert_not_called()
    def test_missing_intent_or_arbitrary_target_never_dispatches(self):
        for args in [[],['--no-backup'],['--erase-authorized-kingston'],['/dev/disk8'],
                     ['--erase-authorized-kingston','--no-backup','/dev/disk8']]:
            with self.subTest(args=args),patch.object(dual.sys,'argv',['runner']+args), \
                 patch.object(dual,'profile') as profile,patch.object(dual.shared,'write_and_test') as write:
                with self.assertRaisesRegex(AssertionError,'explicit'):dual.main()
                profile.assert_not_called();write.assert_not_called()

    def test_authorized_action_uses_fixed_profile_without_backup(self):
        profile=SimpleNamespace()
        with patch.object(dual.sys,'argv',['runner','--erase-authorized-kingston','--no-backup']), \
             patch.object(dual,'profile',return_value=profile), \
             patch.object(dual.shared,'write_and_test',return_value=0) as write:
            self.assertEqual(dual.main(),0)
            write.assert_called_once_with(make_backup=False,profile=profile)

    def test_nonroot_refuses_before_physical_discovery(self):
        with patch.object(dual.os,'geteuid',return_value=501), \
             patch.object(dual.shared,'discover') as discover,patch.object(dual.shared,'open_readonly') as opened, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(dual.shared.write_and_test(False,SimpleNamespace(scheme='MBR',firmware='BIOS+UEFI')),1)
            discover.assert_not_called();opened.assert_not_called()

    def test_uefi_configuration_has_one_physical_media_disk_and_private_firmware(self):
        with tempfile.TemporaryFile() as file,patch.object(Path,'is_file',return_value=True):
            command=dual.uefi_configuration(file.fileno(),Path('/tmp/private-vars'),Path('/tmp/session'))
        nodes=[json.loads(command[i+1]) for i,v in enumerate(command) if v=='-blockdev']
        self.assertEqual(len(nodes),1)
        self.assertEqual(nodes[0]['file']['driver'],'host_device')
        self.assertFalse(nodes[0]['read-only'])
        self.assertEqual(command.count('-drive'),2)
        self.assertIn('file=/tmp/private-vars',command[command.index('-drive',command.index('-drive')+1)+1])
        self.assertIn('node-name=efi-code',command[command.index('-drive')+1])
        for forbidden in ['-snapshot','-kernel','-initrd','-virtfs','-netdev']:
            self.assertNotIn(forbidden,command)
        self.assertEqual(command[command.index('-nic')+1],'none')
        self.assertNotIn('secure',str(dual.EFI_CODE).lower())

    def test_readonly_and_writable_modes_cannot_be_confused(self):
        with tempfile.NamedTemporaryFile() as file,open(file.name,'rb') as readonly, \
             patch.object(Path,'is_file',return_value=True):
            with self.assertRaisesRegex(AssertionError,'access mode'):
                dual.uefi_configuration(readonly.fileno(),Path('/tmp/vars'),Path('/tmp/session'))
            with self.assertRaisesRegex(AssertionError,'access mode'):
                dual.uefi_configuration(file.fileno(),Path('/tmp/vars'),Path('/tmp/session'),True)
            command=dual.uefi_configuration(readonly.fileno(),Path('/tmp/vars'),Path('/tmp/session'),True)
            node=json.loads(command[command.index('-blockdev')+1])
            self.assertTrue(node['read-only']);self.assertTrue(node['file']['read-only'])

    def test_transport_refuses_wrong_device_extra_disk_or_running_cpus(self):
        device={'devicePath':'/dev/disk99'}
        disk={'node-name':'usb-image','drv':'raw','ro':False,'file':'/dev/rdisk99'}
        nodes=[{'inserted':disk}]+[{'inserted':{'node-name':name}} for name in ('efi-code','efi-vars')]
        good={'query-status':{'status':'prelaunch'},'query-block':nodes}
        dual.validate_transport(good,device,False,'UEFI')
        for replacement in [dict(good,**{'query-status':{'status':'running'}}),
                            dict(good,**{'query-block':nodes+[{'inserted':{'node-name':'other'}}]}),
                            dict(good,**{'query-block':[{'inserted':dict(disk,file='/tmp/copy.img')}]+nodes[1:]})]:
            with self.assertRaises(AssertionError):dual.validate_transport(replacement,device,False,'UEFI')
        with self.assertRaises(AssertionError):dual.validate_transport(good,device,True,'UEFI')

    def exercise(self,duplicate_ids=False,extra_node=False,missing_mount=False):
        instances=[]
        class Guest:
            def __init__(self,session):
                self.session=session;session.mkdir();self.phase=len(instances)+1;instances.append(self)
                self.transcript=b'' if missing_mount else b'mounted filesystem fixture r/w'
                self.scripts=[];self.powered_off=False;self.closed=False
            def shell(self):pass
            def metadata(self):
                nodes=[{'inserted':{'node-name':'usb-image','ro':False}}]
                if self.phase==2:
                    nodes += [{'inserted':{'node-name':name}} for name in ('efi-code','efi-vars')]
                if extra_node:nodes.append({'inserted':{'node-name':'other'}})
                return {'query-block':nodes}
            def run(self,script,**kwargs):
                self.scripts.append(script)
                return '\r\nBOOT_ID=00000000-0000-0000-0000-{:012d}\r\n'.format(1 if duplicate_ids else self.phase)
            def shutdown(self):self.powered_off=True
            def close(self):self.closed=True
        with tempfile.TemporaryDirectory() as folder,contextlib.redirect_stdout(io.StringIO()):
            result=dual.exercise_dual({'persistenceUuid':'fixture'},Path(folder),Guest,lambda:{'clean':True})
        return result,instances

    def test_three_fresh_boots_verify_updates_in_both_directions(self):
        result,guests=self.exercise()
        self.assertEqual(result['bootFirmwareSequence'],['BIOS','UEFI','BIOS'])
        self.assertEqual(len(set(result['bootIds'])),3)
        self.assertEqual(result['freshUefiVariableStores'],1)
        self.assertTrue(result['crossFirmwarePersistenceBothDirections'])
        self.assertTrue(all(g.powered_off and g.closed for g in guests))
        scripts=[next(s for s in g.scripts if 'BOOT_ID=' in s) for g in guests]
        self.assertIn('test ! -d /sys/firmware/efi',scripts[0])
        self.assertIn('test -d /sys/firmware/efi',scripts[1])
        self.assertIn('test ! -d /sys/firmware/efi',scripts[2])
        self.assertIn(result['biosCreatedFileSha256'],scripts[1])
        self.assertIn('>> /home/rufuspp-test/rufuspp-persistence.txt',scripts[1])
        self.assertIn(result['uefiUpdatedFileSha256'],scripts[2])
        self.assertIn(result['setting'],scripts[2])
        self.assertNotEqual(result['biosCreatedFileSha256'],result['uefiUpdatedFileSha256'])
        for script in scripts[1:]:self.assertIn('test ! -e /run/rufuspp-dual-volatile',script)

    def test_duplicate_boot_ids_cannot_prove_cross_firmware_persistence(self):
        with self.assertRaisesRegex(AssertionError,'independent'):self.exercise(duplicate_ids=True)

    def test_wrong_storage_or_unmounted_persistence_cannot_pass(self):
        with self.assertRaisesRegex(AssertionError,'selected firmware'):self.exercise(extra_node=True)
        with self.assertRaisesRegex(AssertionError,'did not mount'):self.exercise(missing_mount=True)

    def test_retention_hash_change_prevents_even_readonly_menu_preflight(self):
        result={'inspectionPersistenceSha256':'a'*64}
        with patch.object(dual,'completed_retention',return_value=(result,Path('/tmp/report'))), \
             patch.object(dual.shared.hardware,'hash_region',return_value='b'*64), \
             patch.object(dual,'UefiPhysicalGuest') as guest:
            with self.assertRaisesRegex(AssertionError,'changed after retention'):
                dual.verify_previous_bios(7,{}, {'persistenceOffset':512,'persistenceBytes':512},Path('/tmp'),501,20)
            guest.assert_not_called()

    def test_failed_or_different_retention_report_cannot_authorize_replacement(self):
        current={'persistenceUuid':'fixture'}
        previous={'expectedTarget':{'serial':'fixture'},'geometry':current,
                  'userFileSha256':'b'*64,'setting':'RUFUSPP_TEST_SETTING='+'c'*32}
        source=Path('/tmp/previous-qualified-report.json')
        good=dict(success=True,state='complete',expectedTarget=previous['expectedTarget'],geometry=current,
                  sourceQualification=str(source),physicalWritesAttempted=False,
                  savedFileSha256=previous['userFileSha256'],savedSetting=previous['setting'],
                  inspectionPersistenceSha256='a'*64)
        for key in ('physicalUsbOpenedReadOnly','powerCycleReportedByUser','filesystemCheckPassed',
                    'checkerUsesPhysicalUsb','checkerPhysicalUsbReadOnly',
                    'independentPhysicalFileAndSettingMatched','inspectionPersistenceBytesUnchanged'):
            good[key]=True
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);work=root/'run';work.mkdir()
            (root/'last-run.json').write_text(json.dumps({'session':str(work)}))
            record=work/'report.json'
            with patch.object(dual.retention,'WORK',root), \
                 patch.object(dual.retention,'completed_bios_profile',return_value=(previous,source)):
                record.write_text(json.dumps(good))
                self.assertEqual(dual.completed_retention(current)[0],good)
                for key,value in [('success',False),('error','failed'),('filesystemCheckPassed',False),
                                  ('physicalWritesAttempted',True),('inspectionPersistenceSha256','bad'),
                                  ('geometry',{'persistenceUuid':'other'}),('expectedTarget',{'serial':'other'}),
                                  ('savedFileSha256','d'*64),('sourceQualification','/tmp/different')]:
                    with self.subTest(key=key):
                        record.write_text(json.dumps(dict(good,**{key:value})))
                        with self.assertRaisesRegex(AssertionError,'matching read-only'):
                            dual.completed_retention(current)

    def test_terminal_decoder_uses_rendered_screen_and_supports_cursor_queries(self):
        try:pyte=dual.console_module()
        except ImportError:self.skipTest('manual UEFI UART runner requires local pyte 0.8.2')
        screen=pyte.Screen(80,25);replies=[]
        screen.write_process_input=replies.append
        stream=pyte.ByteStream(screen)
        stream.feed(b"\x1b[2J\x1b[5;1Hsetparams 'Try or Install Ubuntu Server'\r\n\r\n"
                    b' set gfxpayload=keep\r\n linux /casper/vmlinuz persistent ---\x1b[6n')
        self.assertEqual(dual.bios.editor_kernel_row('\n'.join(screen.display)),3)
        self.assertTrue(replies and replies[-1].startswith('\x1b['))


class IndependentFatReadback(unittest.TestCase):
    def fixture(self):
        image=bytearray(5632);base=512
        struct.pack_into('<H',image,base+11,512);image[base+13]=1
        struct.pack_into('<H',image,base+14,1);image[base+16]=1
        struct.pack_into('<I',image,base+36,1);struct.pack_into('<I',image,base+44,2)
        image[base+82:base+90]=b'FAT32   '
        for cluster in (2,3,4):struct.pack_into('<I',image,1024+cluster*4,0x0fffffff)
        image[1536:1547]=b'FILE    TXT';image[1547]=0x20
        struct.pack_into('<H',image,1536+26,3);struct.pack_into('<I',image,1536+28,3)
        image[2048:2051]=b'abc'
        return image

    def reader(self,image):
        def read(offset,count):
            self.assertEqual(offset%512,0);self.assertEqual(count%512,0)
            return bytes(image[offset:offset+count])
        return fat32_readback.Fat32(read,{'fatOffset':512,'fatBytes':5120})

    def test_all_original_files_match_with_independent_cluster_walks(self):
        image=self.fixture()
        result=fat32_readback.compare_files(self.reader(image),self.reader(bytearray(image)))
        self.assertTrue(result['independentStagedFilesMatched'])
        self.assertEqual(result['independentStagedFilesChecked'],1)
        self.assertEqual(result['independentStagedFileBytesChecked'],3)

    def test_extra_host_file_is_counted_without_hiding_original_mutation(self):
        image=self.fixture();written=bytearray(image)
        written[1568:1579]=b'MAC     TXT';written[1579]=0x20
        struct.pack_into('<H',written,1568+26,4);struct.pack_into('<I',written,1568+28,1)
        written[2560]=ord('x')
        result=fat32_readback.compare_files(self.reader(image),self.reader(written))
        self.assertEqual(result['hostAddedFiles'],1)
        written[2048]=ord('z')
        with self.assertRaisesRegex(AssertionError,'content differs'):
            fat32_readback.compare_files(self.reader(image),self.reader(written))

    def test_missing_file_changed_size_and_long_name_are_rejected(self):
        image=self.fixture()
        for mutate in [lambda b:b.__setitem__(1536,0),
                       lambda b:struct.pack_into('<I',b,1536+28,2),
                       lambda b:b.__setitem__(1536,ord('Z'))]:
            written=bytearray(image);mutate(written)
            with self.assertRaises(AssertionError):
                fat32_readback.compare_files(self.reader(image),self.reader(written))

    def test_bad_or_looping_cluster_chain_and_partition_overread_are_rejected(self):
        image=self.fixture()
        for next_cluster in (3,0,100):
            written=bytearray(image);struct.pack_into('<I',written,1024+3*4,next_cluster)
            with self.assertRaisesRegex(AssertionError,'FAT chain'):
                fat32_readback.compare_files(self.reader(image),self.reader(written))
        with self.assertRaisesRegex(AssertionError,'outside partition'):
            self.reader(image).read(0,512)

    def test_duplicate_directory_name_is_rejected(self):
        image=self.fixture();image[1568:1600]=image[1536:1568]
        with self.assertRaisesRegex(AssertionError,'duplicate'):
            self.reader(image).catalog()


if __name__=='__main__':
    unittest.main(verbosity=2)
