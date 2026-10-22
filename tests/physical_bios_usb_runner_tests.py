#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""BIOS runner safety checks using memory and ordinary temporary files only."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import test_physical_bios_usb as bios


class BiosGuards(unittest.TestCase):
    def test_continuation_requires_sudo_before_discovering_or_opening_usb(self):
        with patch.object(bios.os,'geteuid',return_value=501), \
             patch.object(bios.shared,'discover') as discover,patch.object(bios.shared,'open_readonly') as opened, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(bios.continue_written(),1)
            discover.assert_not_called();opened.assert_not_called()

    def test_continuation_flag_does_not_dispatch_an_image_rewrite(self):
        with patch.object(bios.sys,'argv',['runner','--continue-written-usb']), \
             patch.object(bios,'continue_written',return_value=0) as continuation, \
             patch.object(bios.shared,'write_and_test') as write:
            self.assertEqual(bios.main(),0);continuation.assert_called_once_with();write.assert_not_called()

    def test_continuation_refuses_failed_write_and_preserves_original_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);work=root/'run';work.mkdir()
            (root/'prepared.json').write_text(json.dumps({'session':str(work)}))
            (work/'prepared.json').write_text('{}')
            original={'expectedTarget':{},'partitionScheme':'MBR','targetSystem':'BIOS','success':False,
                      'error':'Resource busy','independentInitializedRangesMatched':True,
                      'previousMediaCheck':{'success':True},'nativeWrite':{'success':False}}
            record=work/'report.json';record.write_text(json.dumps(original));before=record.read_bytes()
            with patch.object(bios,'WORK',root),patch.object(bios.os,'geteuid',return_value=0), \
                 patch.dict(os.environ,{'SUDO_UID':'501','SUDO_GID':'20'}), \
                 patch.object(bios.hardware,'no_active_guest'),patch.object(bios,'profile'), \
                 patch.object(bios.shared.capture,'expected_target',return_value={}), \
                 patch.object(bios.shared,'open_readonly') as opened, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(bios.continue_written(),1);opened.assert_not_called()
            self.assertEqual(record.read_bytes(),before)

    def test_host_remount_prevents_guest_execution_before_raw_path_check(self):
        device={'devicePath':'/dev/fixture','serial':'fixture'}
        with patch.object(bios.direct.subprocess,'run',return_value=SimpleNamespace(stdout='501 501')), \
             patch.object(bios.direct,'discover',return_value=dict(device,mountPoints=['/Volumes/fixture'])), \
             patch.object(bios.direct.os,'lstat') as path:
            with self.assertRaisesRegex(AssertionError,'remounted by the host'):
                bios.direct.verify_privilege_drop(12,7,device,501)
            path.assert_not_called()

    def firmware_exercise(self, extra_nodes=False):
        geometry={'persistenceUuid':'fixture-uuid'}
        instances=[]
        class Guest:
            def __init__(self,session):
                self.session=session; session.mkdir()
                self.phase=len(instances)+1; instances.append(self)
                self.transcript=b'mounted filesystem fixture-uuid r/w'
                self.scripts=[]; self.powered_off=False
            def shell(self): pass
            def metadata(self):
                nodes=[{'inserted':{'node-name':'usb-image','ro':False}}]
                if extra_nodes:
                    nodes += [{'inserted':{'node-name':name,'ro':True}} for name in ('efi-code','efi-vars')]
                return {'query-block':nodes}
            def run(self,script,**kwargs):
                self.scripts.append(script)
                return '\r\nBOOT_ID=00000000-0000-0000-0000-{:012d}\r\n'.format(self.phase)
            def shutdown(self): self.powered_off=True
            def close(self): pass
        with tempfile.TemporaryDirectory() as folder,contextlib.redirect_stdout(io.StringIO()):
            result=bios.shared.persistence.exercise_persistence(geometry,Path(folder),Guest,
                lambda:{'clean':True},firmware='BIOS')
        return result,instances

    def test_bios_exercise_checks_runtime_firmware_and_records_no_uefi_stores(self):
        result,guests=self.firmware_exercise()
        self.assertEqual(result['firmware'],'BIOS')
        self.assertEqual(result['freshUefiVariableStores'],0)
        self.assertEqual(len(set(result['bootIds'])),2)
        self.assertTrue(all(guest.powered_off for guest in guests))
        for guest in guests:
            script=next(script for script in guest.scripts if 'BOOT_ID=' in script)
            self.assertIn('test ! -d /sys/firmware/efi',script)
            self.assertIn('uname -m',script)

    def test_bios_exercise_rejects_a_guest_with_uefi_block_nodes(self):
        with self.assertRaisesRegex(AssertionError,'selected firmware'):
            self.firmware_exercise(extra_nodes=True)

    def test_no_backup_or_device_argument_does_not_authorize_writing(self):
        for args in [[],['--no-backup'],['/dev/disk8'],['--erase-authorized-kingston']]:
            with self.subTest(args=args),patch.object(bios.sys,'argv',['runner']+args), \
                 patch.object(bios,'profile') as profile,patch.object(bios.shared,'write_and_test') as write:
                with self.assertRaisesRegex(AssertionError,'explicit'): bios.main()
                write.assert_not_called(); profile.assert_not_called()

    def test_explicit_authorized_action_uses_only_the_pinned_bios_profile(self):
        profile=SimpleNamespace()
        with patch.object(bios.sys,'argv',['runner','--erase-authorized-kingston','--no-backup']), \
             patch.object(bios,'profile',return_value=profile), \
             patch.object(bios.shared,'write_and_test',return_value=0) as write:
            self.assertEqual(bios.main(),0)
            write.assert_called_once_with(make_backup=False,profile=profile)

    def test_non_root_cannot_discover_or_open_a_physical_device(self):
        profile=SimpleNamespace(scheme='MBR',firmware='BIOS')
        with patch.object(bios.shared.os,'geteuid',return_value=501), \
             patch.object(bios.shared,'discover') as discover, \
             patch.object(bios.shared,'open_readonly') as opened, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(bios.shared.write_and_test(make_backup=False,profile=profile),1)
            discover.assert_not_called(); opened.assert_not_called()

    def test_bios_configuration_has_only_physical_usb_and_no_uefi_or_overlay(self):
        with tempfile.TemporaryFile() as source,patch.object(Path,'is_file',return_value=True):
            command=bios.configuration(source.fileno(),Path('/tmp/session'))
        nodes=[json.loads(command[i+1]) for i,value in enumerate(command) if value=='-blockdev']
        self.assertEqual(len(nodes),1); node=nodes[0]
        self.assertEqual(node['driver'],'raw'); self.assertFalse(node['read-only'])
        self.assertEqual(node['file']['driver'],'host_device')
        self.assertTrue(node['file']['filename'].startswith('/dev/fd/'))
        self.assertEqual(command[command.index('-bios')+1],str(bios.BIOS))
        self.assertEqual(command[command.index('-accel')+1],'tcg,thread=multi')
        self.assertEqual(command[command.index('-nic')+1],'none')
        self.assertTrue(any('removable=off' in value for value in command))
        for forbidden in ['-kernel','-initrd','-append','-snapshot','-drive','-pflash','-virtfs','-fsdev']:
            self.assertNotIn(forbidden,command)

    def test_readonly_descriptor_cannot_be_used_for_writable_bios_guest(self):
        with tempfile.TemporaryDirectory() as folder,patch.object(Path,'is_file',return_value=True):
            p=Path(folder)/'file'; p.write_bytes(b'fixture')
            with p.open('rb') as source:
                with self.assertRaisesRegex(AssertionError,'access mode'):
                    bios.configuration(source.fileno(),Path(folder))

    def test_vga_decoder_reads_characters_without_attributes_or_addresses(self):
        text='Try or Install Ubuntu Server'.ljust(2000)
        words=[0x0700 | ord(char) for char in text]
        dump='\n'.join('0x{:08x}: '.format(0xb8000+i*2)+' '.join(
            '0x{:04x}'.format(value) for value in words[i:i+8]) for i in range(0,2000,8))
        result=bios.decode_vga(dump)
        self.assertEqual(len(result.splitlines()),25)
        self.assertTrue(result.startswith('Try or Install Ubuntu Server'))

    def test_vga_decoder_rejects_truncated_or_invalid_memory(self):
        for dump in ['', '0xb8000: 0x0741', '0xb8000: '+('0x10000 '*2000)]:
            with self.assertRaisesRegex(AssertionError,'VGA'): bios.decode_vga(dump)

    def test_editor_requires_the_actual_persistent_kernel_entry(self):
        text="Instructions\n setparams 'Try or Install Ubuntu Server'\n\n set gfxpayload=keep\n linux /casper/vmlinuz persistent ---\n initrd /casper/initrd"
        self.assertEqual(bios.editor_kernel_row(text),3)
        for changed in [text.replace(' persistent',''),text.replace('/casper/vmlinuz','/casper/hwe-vmlinuz'),
                        text+'\nlinux /casper/vmlinuz persistent ---', 'grub>']:
            with self.assertRaisesRegex(AssertionError,'refusing blind'): bios.editor_kernel_row(changed)

    def test_different_reconnected_gpt_identity_stops_before_reads_or_guest(self):
        with patch.object(bios.interactive,'qualified_profile',return_value={'geometry':{'id':'qualified'}}), \
             patch.object(bios.shared.capture,'expected_target',return_value={}), \
             patch.object(bios,'read_exact') as read,patch.object(bios.shared,'inspect_physical') as guest:
            with self.assertRaisesRegex(AssertionError,'reconnected USB differs'):
                bios.verify_previous_gpt(7,{}, {'id':'different'},Path('/tmp/unused'),501,20)
            read.assert_not_called(); guest.assert_not_called()

    def test_failed_retention_check_prevents_native_writer_dispatch(self):
        # A full control-flow refusal with a tiny ordinary source; no raw opens.
        with tempfile.TemporaryDirectory() as folder,contextlib.ExitStack() as mocks:
            root=Path(folder); work=root/'run'; work.mkdir()
            source=work/'source'; source.write_bytes(bytes(2048))
            expected={'serial':'fixture','stableId':'fixture','capacityBytes':2048,'sectorBytes':512}
            geometry={'partitionScheme':'MBR'}
            (root/'prepared.json').write_text(json.dumps({'session':str(work)}))
            (work/'prepared.json').write_text(json.dumps({'prepared':True,'expectedTarget':expected,
                'staged':str(source),'stagedSha256':bios.ISO_SHA,'geometry':geometry,
                'biosBootable':True,'targetSystem':'BIOS','sourceIsoSha256':bios.ISO_SHA,'writeRanges':[]}))
            def failed_retention(*args): raise AssertionError('saved data did not survive')
            profile=SimpleNamespace(work=root,iso=source,sha256=bios.ISO_SHA,scheme='MBR',firmware='BIOS',
                                    inspect_flag='--inspect-staged-mbr',check_previous=failed_retention)
            replacements=[(bios.shared.os,'geteuid',lambda:0),
                (bios.shared.hardware,'no_active_guest',lambda:None),
                (bios.shared.capture,'expected_target',lambda:expected),
                (bios.shared,'digest',lambda path:bios.ISO_SHA),
                (bios.shared,'file_layout',lambda *args:geometry),
                (bios.shared,'validate_ranges',lambda *args:2048),
                (bios.shared,'command',lambda args:json.dumps({'success':True,'capacityBytes':2048}).encode()),
                (bios.shared,'discover',lambda *args,**kwargs:{'devicePath':'/dev/fixture','mountPoints':[]}),
                (bios.shared,'open_readonly',lambda device:os.open(source,os.O_RDONLY)),
                (bios.shared,'physical_layout',lambda *args:{'partitionScheme':'GPT'})]
            for target,name,value in replacements:mocks.enter_context(patch.object(target,name,value))
            mocks.enter_context(patch.dict(os.environ,{'SUDO_UID':'501','SUDO_GID':'20'}))
            writer=mocks.enter_context(patch.object(bios.shared.subprocess,'run'))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(bios.shared.write_and_test(make_backup=False,profile=profile),1)
            writer.assert_not_called()
            self.assertFalse((work/'native-write.json').exists())


if __name__=='__main__':
    unittest.main(verbosity=2)
