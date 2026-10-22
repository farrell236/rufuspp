#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manual physical Kingston MBR BIOS/UEFI cross-firmware persistence test.

The native production writer commits one dual-mode preparation. Three fresh
guests boot BIOS -> UEFI -> BIOS using only the real USB as their media disk.
No destination disk image, copy-on-write overlay, network or shared folder.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time
from types import SimpleNamespace
import uuid

from image_write_integration import require
import check_physical_bios_replug as retention
import test_physical_bios_usb as bios
import test_physical_gpt_usb as shared
import fat32_readback

ROOT=shared.ROOT
WORK=ROOT/'build/physical-dual-usb'
EFI_CODE=Path('/opt/homebrew/share/qemu/edk2-x86_64-code.fd')
EFI_VARS=Path('/opt/homebrew/share/qemu/edk2-i386-vars.fd')


def console_module():
    # Pure-Python, locally installed terminal decoder; no global installation.
    sys.path.insert(0,str(ROOT/'build/console-tools'))
    import pyte
    return pyte


def staged_manifest_sha256():
    work=Path(json.loads((WORK/'prepared.json').read_text())['session'])
    require(work.resolve().parent==WORK.resolve(),'dual stage is outside its preparation directory')
    prepared=json.loads(shared.persistence.ordinary(work/'prepared.json').read_text())
    require(prepared.get('targetSystem')=='BIOS+UEFI' and prepared.get('biosBootable') is True
            and prepared.get('uefiBootable') is True,'both staged boot paths are required')
    return bios.manifest_sha256_from_stage(Path(prepared['staged']),prepared['geometry'])


class DualBiosGuest(bios.BiosPhysicalGuest):
    def media_manifest_sha256(self):
        return staged_manifest_sha256()


def uefi_configuration(fd,variables,session,readonly=False):
    require(bios.QEMU.is_file() and EFI_CODE.is_file() and EFI_VARS.is_file(),
            'x86 QEMU and non-Secure-Boot EDK2 firmware must be installed')
    node=shared.hardware.physical_node(fd,'usb-image',readonly)
    return [str(bios.QEMU),'-name','Rufus++ physical Kingston dual-mode UEFI test',
        '-no-user-config','-nodefaults','-machine','q35','-accel','tcg,thread=multi',
        '-cpu','max','-m','4096','-smp','4',
        '-drive','if=pflash,unit=0,format=raw,readonly=on,file='+str(EFI_CODE)+',node-name=efi-code',
        '-drive','if=pflash,unit=1,format=raw,file='+str(variables)+',node-name=efi-vars',
        '-blockdev',json.dumps(node),'-device','qemu-xhci,id=xhci',
        '-device','usb-storage,bus=xhci.0,drive=usb-image,removable=off,bootindex=1',
        '-nic','none','-boot','order=c,strict=on',
        '-qmp','unix:{},server=on,wait=off'.format(session/'qmp.sock'),
        '-serial','file:'+str(session/'serial.log'),'-display','none']


class UefiPhysicalGuest(DualBiosGuest):
    """Use the firmware's UART console and validate the rendered GRUB entry."""
    def __init__(self,*args,**kwargs):
        pyte=console_module()
        self.screen=pyte.Screen(80,25)
        self.screen.write_process_input=lambda value:self.console.sendall(value.encode())
        self.terminal=pyte.ByteStream(self.screen)
        super().__init__(*args,**kwargs)

    def _prepare_session(self,session,variables):
        shutil.copyfile(EFI_VARS,variables)
        shared.hardware.PhysicalGuest._prepare_session(self,session,variables)

    def _configuration(self,media,variables,session):
        result=uefi_configuration(self.physical_fd,variables,session,self.inspect_only)
        if self.bootstrap_device is not None:
            result=bios.direct.root_bootstrap(result,self.physical_fd,self.bootstrap_device,*self.run_as)
        elif self.paused:
            result.append('-S')
        return result

    def _display_backend(self):
        return 'none'

    def receive(self):
        data=super().receive()
        self.terminal.feed(data)
        return data

    def screen_text(self):
        return '\n'.join(self.screen.display)

    def wait_screen(self,predicate,timeout=180):
        deadline=time.monotonic()+timeout
        update=time.monotonic()+30
        while True:
            text=self.screen_text()
            (self.session/'current-uefi-screen.txt').write_text(text)
            if predicate(text):return text
            require(self.process.poll() is None,'UEFI guest exited before its boot entry')
            require(time.monotonic()<deadline,'UEFI console screen timed out; inspect current-uefi-screen.txt')
            self.receive()
            if time.monotonic()>update:
                print('Waiting for '+self.session.name+' UEFI console…',flush=True)
                update=time.monotonic()+30

    def key(self,data):
        self.console.sendall(data)
        time.sleep(0.2)
        previous=self.console.gettimeout()
        try:
            self.console.settimeout(0.1)
            self.receive()
        finally:self.console.settimeout(previous)

    def prepare_boot_entry(self):
        bios.direct.verify_privilege_drop(self.process.pid,self.physical_fd,self.bootstrap_device,self.run_as[0])
        bios.direct.query(self.socket_dir/'qmp',['cont'])
        menu=self.wait_screen(lambda text:'Try or Install Ubuntu Server' in text)
        (self.session/'uefi-grub-menu.txt').write_text(menu)
        self.key(b'e')
        editor=self.wait_screen(lambda text:'set gfxpayload=keep' in text and '/casper/vmlinuz' in text)
        (self.session/'uefi-editor-before.txt').write_text(editor)
        row=bios.editor_kernel_row(editor)
        # GRUB Emacs bindings work over UART; the inspected entry determines
        # the kernel row. Deliberate pacing avoids dropped firmware keystrokes.
        for _ in range(25):self.key(b'\x10')  # C-p, previous line
        self.key(b'\x01')  # C-a, start of line
        for _ in range(row):self.key(b'\x0e')  # C-n
        self.key(b'\x05')  # C-e, end of line
        for char in b' console=ttyS0,115200n8':self.key(bytes([char]))
        edited=self.wait_screen(lambda text:any('/casper/vmlinuz' in line and
            'console=ttyS0,115200n8' in line for line in text.splitlines()))
        bios.editor_kernel_row(edited)  # Requires the persistence option still on the kernel line.
        (self.session/'uefi-console-edit.json').write_text(json.dumps({
            'entryBefore':editor,'entryAfter':edited,'kernelRow':row,
            'sessionOnlyKernelOption':'console=ttyS0,115200n8',
            'usbBootConfigurationEdited':False},indent=2)+'\n')

    def shell(self):
        self.prepare_boot_entry()
        require(not self.inspect_only,'read-only menu preflight cannot request a Linux boot')
        (self.session/'kernel-boot-requested.json').write_text('{"requestedByRunner":true}\n')
        self.key(b'\x18')  # C-x, boot inspected entry
        shared.persistence.Guest.shell(self)
        self.shell_ready=True
        self.run('set -eu; test -d /sys/firmware/efi; test "$(uname -m)" = x86_64; '
                 'test "$(findmnt -n -o SOURCE /cdrom)" = /dev/sda1; '
                 'findmnt -n -o OPTIONS /cdrom | grep -Eq "(^|,)ro(,|$)"')
        self.check_media_files()


def guest_factory(fd,session,**kwargs):
    cls=UefiPhysicalGuest if session.name.endswith('-uefi') else DualBiosGuest
    return cls(fd,session,**kwargs)


def validate_transport(metadata,device,readonly,firmware):
    nodes=[item['inserted'] for item in metadata['query-block']]
    disks=[node for node in nodes if node['node-name']=='usb-image']
    require(metadata['query-status']['status']=='prelaunch'
            and len(nodes)==(3 if firmware=='UEFI' else 1) and len(disks)==1,
            'transport must contain only physical USB and selected firmware with CPUs paused')
    disk=disks[0]
    require(disk['ro'] is readonly and disk['drv']=='raw'
            and disk['file']==device['devicePath'].replace('/dev/disk','/dev/rdisk',1),
            'transport differs from the pinned physical USB or requested access mode')
    require(firmware!='UEFI' or {node['node-name'] for node in nodes}=={'usb-image','efi-code','efi-vars'},
            'unexpected UEFI storage nodes')


def check_transport(fd,work,device,run_as):
    for firmware,cls in [('BIOS',DualBiosGuest),('UEFI',UefiPhysicalGuest)]:
        guest=cls(fd,work/('transport-'+firmware.lower()),paused=True,device=device,run_as=run_as)
        try:
            validate_transport(guest.metadata(),device,False,firmware)
            bios.direct.query(guest.socket_dir/'qmp',['quit'])
            require(guest.process.wait(timeout=10)==0,'paused transport did not exit normally')
        finally:guest.close()
    print('PASS paused physical USB transport checks for BIOS and UEFI.',flush=True)


def completed_retention(current):
    previous,source=retention.completed_bios_profile()
    work=Path(json.loads((retention.WORK/'last-run.json').read_text())['session']).resolve()
    require(work.parent==retention.WORK.resolve(),'retention report outside its expected directory')
    result=json.loads(shared.persistence.ordinary(work/'report.json').read_text())
    require(result.get('success') is True and result.get('state')=='complete' and not result.get('error')
            and result.get('expectedTarget')==previous['expectedTarget']
            and result.get('geometry')==current==previous['geometry']
            and result.get('sourceQualification')==str(source)
            and result.get('physicalWritesAttempted') is False
            and result.get('savedFileSha256')==previous['userFileSha256']
            and result.get('savedSetting')==previous['setting']
            and all(result.get(key) is True for key in ('physicalUsbOpenedReadOnly','powerCycleReportedByUser',
                'filesystemCheckPassed','checkerUsesPhysicalUsb','checkerPhysicalUsbReadOnly',
                'independentPhysicalFileAndSettingMatched','inspectionPersistenceBytesUnchanged'))
            and re.fullmatch('[0-9a-f]{64}',result.get('inspectionPersistenceSha256','')),
            'completed matching read-only BIOS power-cycle retention check required')
    return result,work/'report.json'


def verify_previous_bios(fd,device,current,work,uid,gid):
    result,source=completed_retention(current)
    before=shared.hardware.hash_region(fd,current['persistenceOffset'],current['persistenceBytes'],
                                       'Binding power-cycle retention to connected physical USB')
    require(before==result['inspectionPersistenceSha256'],'physical persistence changed after retention check')
    shared.hardware.finalized_header(fd,current)
    # Validate the new UART firmware/GRUB control path on the current USB,
    # read-only and before any native writer receives erase authorization.
    guest=UefiPhysicalGuest(fd,work/'uefi-menu-before-erase',inspect_only=True,paused=True,
                            device=device,run_as=(uid,gid))
    try:
        validate_transport(guest.metadata(),device,True,'UEFI')
        guest.prepare_boot_entry()
        bios.direct.query(guest.socket_dir/'qmp',['quit'])
        require(guest.process.wait(timeout=10)==0,'read-only UEFI preflight did not exit normally')
    finally:guest.close()
    require(not (guest.session/'kernel-boot-requested.json').exists(),
            'pre-erase menu check must not boot a guest kernel')
    require(shared.hardware.hash_region(fd,current['persistenceOffset'],current['persistenceBytes'],
            'Read-only UEFI preflight persistence verification')==before,
            'pre-erase read-only check changed persistence')
    return {'success':True,'retentionReport':str(source),'powerCycleRetentionMatched':True,
            'physicalUsbReadOnly':True,'uefiMenuPreflightPassed':True,'guestKernelBootRequested':False,
            'inspectionPersistenceBytesUnchanged':True}


def exercise_dual(geometry,work,factory,finalized_header,after_shutdown=None):
    token1,token2=uuid.uuid4().hex,uuid.uuid4().hex
    data1='Rufus++ BIOS persistent user file '+token1+'\n'
    data2=data1+'Rufus++ UEFI persistent update '+token2+'\n'
    shas=[hashlib.sha256(data.encode()).hexdigest() for data in (data1,data2)]
    settings=['RUFUSPP_TEST_SETTING='+token for token in (token1,token2)]
    outputs=[];headers=[]
    for phase,firmware in enumerate(('BIOS','UEFI','BIOS'),1):
        print('Starting direct physical USB boot {} ({})'.format(phase,firmware),flush=True)
        guest=factory(work/('boot-{}-{}'.format(phase,firmware.lower())))
        try:
            guest.shell()
            nodes=[item['inserted'] for item in guest.metadata()['query-block']]
            require(len(nodes)==(3 if firmware=='UEFI' else 1)
                    and {node['node-name'] for node in nodes}==(
                        {'usb-image','efi-code','efi-vars'} if firmware=='UEFI' else {'usb-image'})
                    and next(node for node in nodes if node['node-name']=='usb-image')['ro'] is False,
                    'cross-firmware guest must use the writable physical USB and selected firmware only')
            guest.run('dmesg',timeout=120)
            require(('mounted filesystem '+geometry['persistenceUuid']+' r/w').encode() in guest.transcript,
                    'kernel did not mount the expected physical persistence filesystem read-write')
            script=('set -eu; test '+('' if firmware=='UEFI' else '! ')+
                '-d /sys/firmware/efi; test "$(uname -m)" = x86_64; grep -qw persistent /proc/cmdline; '
                'test "$(findmnt -n -o FSTYPE /)" = overlay; '
                'findmnt -n -o OPTIONS / | grep -Fq upperdir=/cow/upper; '
                'test "$(blkid -s UUID -o value /dev/sda2)" = "'+geometry['persistenceUuid']+'"; '
                'findmnt /cow; findmnt /; printf "BOOT_ID="; cat /proc/sys/kernel/random/boot_id; ')
            if phase==1:
                script+=('! id rufuspp-test >/dev/null 2>&1; useradd --create-home --shell /bin/bash rufuspp-test; '
                    'test ! -e /home/rufuspp-test/rufuspp-persistence.txt; '
                    'test ! -e /etc/rufuspp-persistence-test.conf; '
                    "printf '"+data1.replace('\n','\\n')+"' > /home/rufuspp-test/rufuspp-persistence.txt; "
                    'chown rufuspp-test:rufuspp-test /home/rufuspp-test/rufuspp-persistence.txt; '
                    "printf '%s\\n' '"+settings[0]+"' > /etc/rufuspp-persistence-test.conf; "
                    'touch /run/rufuspp-dual-volatile; ')
            else:
                index=0 if phase==2 else 1
                script+=('id rufuspp-test; test ! -e /run/rufuspp-dual-volatile; '
                    "test \"$(cat /etc/rufuspp-persistence-test.conf)\" = '"+settings[index]+"'; "
                    "printf '%s  %s\\n' '"+shas[index]+"' /home/rufuspp-test/rufuspp-persistence.txt | sha256sum -c -; ")
                if phase==2:
                    script+=("printf 'Rufus++ UEFI persistent update "+token2+"\\n' >> /home/rufuspp-test/rufuspp-persistence.txt; "
                        "printf '%s\\n' '"+settings[1]+"' > /etc/rufuspp-persistence-test.conf; "
                        'touch /run/rufuspp-dual-volatile; ')
            index=0 if phase==1 else 1
            script+=("printf '%s  %s\\n' '"+shas[index]+"' /home/rufuspp-test/rufuspp-persistence.txt | sha256sum -c -; "
                'runuser -u rufuspp-test -- test -r /home/rufuspp-test/rufuspp-persistence.txt; '
                'cat /etc/rufuspp-persistence-test.conf; sync')
            output=guest.run(script);outputs.append(output)
            (guest.session/'assertions.txt').write_text(output)
            print(guest.session.name+': file/setting assertions passed',flush=True)
            guest.shutdown()
            if after_shutdown is not None:after_shutdown()
            header=finalized_header();headers.append(header)
            (guest.session/'filesystem-header.json').write_text(json.dumps(header,indent=2)+'\n')
        finally:guest.close()
    boot_ids=[]
    for output in outputs:
        match=re.search(r'\r?\nBOOT_ID=([0-9a-f-]{36})',output)
        require(match is not None,'guest boot ID missing');boot_ids.append(match.group(1))
    require(len(set(boot_ids))==3,'cross-firmware test requires three independent boots')
    return {'success':True,'geometry':geometry,'userFileSha256':shas[1],'setting':settings[1],
        'persistenceDataSurvived':True,'crossFirmwarePersistenceBothDirections':True,
        'biosCreatedFileSha256':shas[0],'uefiUpdatedFileSha256':shas[1],
        'bootIds':boot_ids,'bootFirmwareSequence':['BIOS','UEFI','BIOS'],
        'volatileControlsAbsentOnFreshBoots':True,'guestRequestedPoweroffs':3,'qemuProcessExits':3,
        'freshQemuProcesses':3,'freshUefiVariableStores':1,'finalizedFilesystemHeaders':headers,
        'firmware':'BIOS+UEFI','ubuntuFileManifestsPassed':True,'ubuntuManifestCheckBoots':3,
        'virtualUsbSwappableMedia':False,'networkAttached':False,'sharedFoldersAttached':False}


def profile():
    bios.profile()  # Require the previously authenticated signed AMD64 source.
    console_module()
    return SimpleNamespace(work=WORK,iso=bios.ISO,sha256=bios.ISO_SHA,scheme='MBR',firmware='BIOS+UEFI',
        stage_flag='--stage-persistence-dual',inspect_flag='--inspect-staged-mbr',
        write_flag='--write-staged-mbr-dual-authorized',check_previous=verify_previous_bios,
        guest_factory=guest_factory,check_transport=check_transport,exercise=exercise_dual)


def continue_written():
    """Resume the verified write stopped by a host remount; never rewrite it."""
    read_fd=write_fd=record=None
    report={'success':False,'imageRewritePerformed':False,'physicalDeviceAccess':True,
            'partitionScheme':'MBR','targetSystem':'BIOS+UEFI','overlayUsed':False}
    try:
        require(sys.platform=='darwin' and os.geteuid()==0,'continuation requires sudo in the chat Terminal')
        uid,gid=os.environ.get('SUDO_UID',''),os.environ.get('SUDO_GID','')
        require(uid.isdecimal() and gid.isdecimal() and int(uid)>0,'normal sudo caller required')
        uid,gid=int(uid),int(gid)
        shared.hardware.no_active_guest();setup=profile()
        parent=Path(json.loads((WORK/'prepared.json').read_text())['session']).resolve()
        require(parent.parent==WORK.resolve(),'dual preparation outside its expected directory')
        original=json.loads(shared.persistence.ordinary(parent/'report.json').read_text())
        prepared=json.loads(shared.persistence.ordinary(parent/'prepared.json').read_text())
        expected=shared.capture.expected_target();native=original.get('nativeWrite',{})
        require(original.get('expectedTarget')==expected and original.get('success') is False
                and original.get('targetSystem')=='BIOS+UEFI' and original.get('partitionScheme')=='MBR'
                and original.get('state')=='independent-physical-readback'
                and 'Resource busy' in original.get('error','')
                and original.get('previousMediaCheck',{}).get('success') is True
                and native.get('success') is True and native.get('verificationCompleted') is True,
                'continuation requires the verified dual-mode write stopped by macOS remount')
        require(not list(parent.glob('boot-*')),'a guest boot was already attempted in the write run')
        for earlier in parent.glob('continue-*'):
            require(not list(earlier.glob('boot-*/kernel-boot-requested.json'))
                    and not list(earlier.glob('boot-*/assertions.txt')),
                    'an earlier continuation requested a kernel boot; preserve its evidence for review')
        require(prepared.get('prepared') is True and prepared.get('expectedTarget')==expected
                and prepared.get('targetSystem')=='BIOS+UEFI' and prepared.get('biosBootable') is True
                and prepared.get('uefiBootable') is True and prepared.get('sourceIsoSha256')==setup.sha256,
                'prepared dual-mode qualification changed')
        source=shared.persistence.ordinary(Path(prepared['staged']))
        require(source.stat().st_size==expected['capacityBytes']
                and shared.digest(source)==prepared['stagedSha256']
                and shared.digest(shared.persistence.ordinary(setup.iso))==setup.sha256,
                'authenticated dual-mode source changed')
        planned=shared.validate_ranges(prepared['writeRanges'],expected['capacityBytes'])
        require(native['bytesWritten']==native['bytesVerified']==planned,'earlier native transfer incomplete')
        geometry=shared.file_layout(source,expected['capacityBytes'])
        require(geometry==prepared['geometry']==original['geometry'],'prepared dual-mode geometry changed')
        work=Path(tempfile.mkdtemp(prefix='continue-',dir=parent));os.chown(work,uid,gid)
        report.update({key:original[key] for key in ('expectedTarget','geometry','nativeWrite',
                                                    'previousMediaCheck','backupSkippedByUser')})
        report.update(state='acquiring-verified-written-usb',sourceWriteReport=str(parent/'report.json'),
                      physicalWritesStarted=True,hostRemountObservedAfterWriteVerification=True)
        record=work/'report.json';record.write_text(json.dumps(report,indent=2)+'\n');os.chown(record,uid,gid)
        (WORK/'last-continuation.json').write_text(json.dumps({'session':str(work)})+'\n')
        args=SimpleNamespace(helper=shared.HELPER,serial=expected['serial'])
        device,read_fd,write_fd=shared.acquire_unmounted_handles(args,expected)
        require(shared.physical_layout(read_fd,expected['capacityBytes'])==geometry,
                'physical dual-mode layout or persistence UUID changed')
        shared.hardware.finalized_header(read_fd,geometry)
        report['state']='independent-physical-file-and-metadata-readback'
        record.write_text(json.dumps(report,indent=2)+'\n')
        # FAT32 may already contain macOS allocation/status/timestamp changes.
        # Compare every original file and name through independent FAT walks;
        # require exact bytes for bootstrap and all non-FAT initialized ranges.
        with source.open('rb') as stream:
            def read_source(offset,count):
                stream.seek(offset);return stream.read(count)
            report.update(fat32_readback.compare_files(fat32_readback.Fat32(read_source,geometry),
                fat32_readback.Fat32(lambda offset,count:shared.read_exact(read_fd,offset,count),geometry)))
            compared=0;fat_start=geometry['fatOffset'];fat_end=fat_start+geometry['fatBytes']
            for extent in prepared['writeRanges']:
                lo,hi=extent['offset'],extent['offset']+extent['size']
                for start,end in [(lo,min(hi,fat_start)),(max(lo,fat_end),hi)]:
                    for offset in range(start,end,4*1024**2):
                        count=min(4*1024**2,end-offset)
                        require(read_source(offset,count)==shared.read_exact(read_fd,offset,count),
                                'initialized non-FAT metadata mismatch at '+str(offset))
                        compared+=count
        require(report['independentStagedFilesChecked']>0,'no staged files checked')
        report.update(independentNonFatInitializedBytesMatched=True,independentNonFatBytesCompared=compared,
                      independentInitializedRangesMatched=False,
                      independentReadbackMode='all-original-FAT-files-and-non-FAT-initialized-ranges',
                      state='recording-physical-boot-area-baseline')
        record.write_text(json.dumps(report,indent=2)+'\n')
        shared.qualify_written_media(read_fd,write_fd,device,geometry,work,uid,gid,setup,report,record)
        require(all((work/name/'media-check.txt').is_file() for name in ('boot-1-bios','boot-2-uefi','boot-3-bios')),
                'all three firmware boots must check the authenticated Ubuntu manifest')
        os.close(write_fd);write_fd=None;os.close(read_fd);read_fd=None
        device=shared.discover(args,expected,by_serial=True)
        shared.command(['/usr/sbin/diskutil','eject',device['devicePath']])
        report.update(success=True,state='complete',ejected=True)
        print('PASS physical dual-mode continuation: BIOS -> UEFI -> BIOS, direct persistence and read-only fsck.',flush=True)
        return 0
    except (Exception,KeyboardInterrupt) as error:
        report['success']=False;report['error']=str(error) or 'interrupted'
        print('Physical dual-mode continuation stopped: '+report['error'],file=sys.stderr,flush=True)
        return 1
    finally:
        for fd in (write_fd,read_fd):
            if fd is not None:os.close(fd)
        if record is not None:
            record.write_text(json.dumps(report,indent=2)+'\n');print('Report: '+str(record),flush=True)


def main():
    if sys.argv[1:]==['--prepare']:return shared.prepare(profile())
    if sys.argv[1:]==['--continue-written-usb']:return continue_written()
    require(sys.argv[1:]==['--erase-authorized-kingston','--no-backup'],
            'use --prepare or explicit --erase-authorized-kingston --no-backup; no device arguments accepted')
    return shared.write_and_test(make_backup=False,profile=profile())


if __name__=='__main__':
    sys.exit(main())
