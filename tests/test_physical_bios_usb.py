#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manual MBR/BIOS test of the pinned physical Kingston, without disk overlays.

First verify the previous GPT media after the user's physical power cycle.
Only then replace it using the production ISO stager and native guarded writer.
QEMU emulates x86 on this ARM Mac; its sole disk is the actual USB.
"""
import json
import hashlib
import os
from pathlib import Path
import re
import socket
import sys
import struct
import tempfile
import time
from types import SimpleNamespace

from image_write_integration import digest, require
from physical_usb_integration import read_exact
import boot_physical_usb_qemu as direct
import boot_physical_usb_writable_qemu as interactive
import test_physical_gpt_usb as shared
import test_physical_usb_persistence as hardware

ROOT = shared.ROOT
WORK = ROOT/'build/physical-bios-usb'
ISO = ROOT/'build/ubuntu-iso/ubuntu-24.04.5-live-server-amd64.iso'
ISO_SHA = '97f3d7ffb032c3eb3b23d2c8be9cc76e60c2c1f2c0146ba5ba9fe01cafae0fd8'
QEMU = Path('/opt/homebrew/bin/qemu-system-x86_64')
BIOS = Path('/opt/homebrew/share/qemu/bios-256k.bin')


def staged_manifest_sha256():
    """Read the small root MD5SUM.TXT from the authenticated ordinary stage."""
    work=Path(json.loads((WORK/'prepared.json').read_text())['session'])
    require(work.resolve().parent==WORK.resolve(), 'BIOS stage is outside its preparation directory')
    prepared=json.loads(shared.persistence.ordinary(work/'prepared.json').read_text())
    source=shared.persistence.ordinary(Path(prepared['staged']))
    return manifest_sha256_from_stage(source, prepared['geometry'])


def manifest_sha256_from_stage(source, geometry):
    source=shared.persistence.ordinary(source)
    base=geometry['fatOffset']; limit=base+geometry['fatBytes']
    with source.open('rb') as stream:
        def read(offset,count):
            require(base <= offset and 0 < count <= limit-offset, 'FAT manifest read out of bounds')
            stream.seek(offset); data=stream.read(count)
            require(len(data)==count, 'short FAT manifest read'); return data
        boot=read(base,512)
        require(boot[82:90]==b'FAT32   ' and struct.unpack_from('<H',boot,11)[0]==512,
                'expected the staged FAT32 source')
        cluster_bytes=boot[13]*512
        require(512 <= cluster_bytes <= 65536 and cluster_bytes & (cluster_bytes-1)==0,
                'invalid manifest cluster size')
        fat=base+struct.unpack_from('<H',boot,14)[0]*512
        data=fat+boot[16]*struct.unpack_from('<I',boot,36)[0]*512
        def chain(cluster):
            seen=set()
            while cluster < 0x0ffffff8:
                require(2 <= cluster and cluster not in seen and len(seen)*cluster_bytes < 1024**2,
                        'invalid or excessive manifest chain')
                seen.add(cluster)
                yield read(data+(cluster-2)*cluster_bytes,cluster_bytes)
                cluster=struct.unpack('<I',read(fat+cluster*4,4))[0] & 0x0fffffff
        for block in chain(struct.unpack_from('<I',boot,44)[0]):
            for i in range(0,len(block),32):
                entry=block[i:i+32]
                if entry[:11]==b'MD5SUM  TXT' and entry[11] & 0x18 == 0:
                    size=struct.unpack_from('<I',entry,28)[0]
                    require(0 < size <= 1024**2,'invalid staged checksum manifest size')
                    cluster=(struct.unpack_from('<H',entry,20)[0]<<16)|struct.unpack_from('<H',entry,26)[0]
                    content=b''.join(chain(cluster))
                    require(len(content)>=size,'truncated checksum manifest')
                    return hashlib.sha256(content[:size]).hexdigest()
    raise AssertionError('staged Ubuntu checksum manifest missing')


def qmp(socket_path, action, arguments):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10); connection.connect(str(socket_path))
        with connection.makefile('rwb') as stream:
            require('QMP' in json.loads(stream.readline()), 'invalid BIOS QMP greeting')
            for command in [{'execute':'qmp_capabilities'}, {'execute':action,'arguments':arguments}]:
                stream.write((json.dumps(command)+'\n').encode()); stream.flush()
                while True:
                    reply=json.loads(stream.readline())
                    if 'return' in reply or 'error' in reply:
                        require('error' not in reply, 'BIOS monitor operation failed: '+str(reply))
                        break
            return reply['return']


def decode_vga(dump):
    words=[]
    for line in dump.splitlines():
        if ':' in line:
            words.extend(int(word,16) for word in re.findall(r'0x[0-9a-fA-F]+',line.split(':',1)[1]))
    require(len(words)==2000 and all(word <= 0xffff for word in words), 'short VGA text-memory read')
    text=''.join(chr(word & 0xff) for word in words)
    return '\n'.join(text[start:start+80] for start in range(0,2000,80))


def editor_kernel_row(text):
    lines=text.splitlines()
    starts=[i for i,line in enumerate(lines) if "setparams 'Try or Install Ubuntu Server'" in line]
    kernels=[i for i,line in enumerate(lines) if re.search(r'\blinux\s+/casper/vmlinuz\b',line)]
    require(len(starts)==len(kernels)==1 and 0 < kernels[0]-starts[0] <= 6
            and 'persistent' in lines[kernels[0]], 'unexpected GRUB entry; refusing blind boot edits')
    return kernels[0]-starts[0]


def configuration(fd, session):
    require(QEMU.is_file() and BIOS.is_file(), 'x86 QEMU and SeaBIOS must be installed')
    node = hardware.physical_node(fd, 'usb-image', False)
    return [str(QEMU), '-name', 'Rufus++ physical Kingston MBR BIOS persistence test',
        '-no-user-config', '-nodefaults', '-machine', 'pc', '-bios', str(BIOS),
        '-accel', 'tcg,thread=multi', '-cpu', 'max', '-m', '4096', '-smp', '4',
        '-blockdev', json.dumps(node), '-device', 'qemu-xhci,id=xhci',
        '-device', 'usb-storage,bus=xhci.0,drive=usb-image,removable=off,bootindex=1',
        '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0',
        '-device', 'VGA', '-nic', 'none', '-boot', 'order=c,strict=on',
        '-qmp', 'unix:{},server=on,wait=off'.format(session/'qmp.sock'),
        '-serial', 'file:'+str(session/'serial.log'), '-display', 'cocoa']


class BiosPhysicalGuest(hardware.PhysicalGuest):
    def media_manifest_sha256(self):
        return staged_manifest_sha256()

    def check_media_files(self):
        checked=self.run('set -eu; cd /cdrom; printf "%s  %s\\n" "'+self.media_manifest_sha256()+
                        '" md5sum.txt | sha256sum -c -; md5sum -c md5sum.txt',timeout=600)
        (self.session/'media-check.txt').write_text(checked)

    def monitor(self, action, arguments):
        return qmp(self.socket_dir/'qmp', action, arguments)

    def vga_text(self):
        return decode_vga(self.monitor('human-monitor-command', {'command-line':'xp /2000hx 0xb8000'}))

    def press(self, *keys):
        self.monitor('send-key', {'keys':[{'type':'qcode','data':key} for key in keys], 'hold-time':100})
        time.sleep(0.3)

    def wait_vga(self, predicate, timeout=120):
        deadline=time.monotonic()+timeout
        while True:
            require(self.process.poll() is None, 'BIOS guest exited before its boot menu')
            text=self.vga_text()
            (self.session/'current-vga-text.txt').write_text(text)
            if predicate(text): return text
            if time.monotonic() >= deadline:
                (self.session/'last-vga-text.txt').write_text(text)
                raise AssertionError('BIOS GRUB text screen timed out; inspect last-vga-text.txt')
            time.sleep(0.25)

    def shell(self):
        direct.verify_privilege_drop(self.process.pid,self.physical_fd,self.bootstrap_device,self.run_as[0])
        direct.query(self.socket_dir/'qmp',['cont'])
        # Add only a session console option to the actual staged menu entry.
        # The MBR/core, menu, kernel and initrd still load from the physical USB;
        # neither boot configuration on the USB nor the stager is modified.
        menu=self.wait_vga(lambda text:'Try or Install Ubuntu Server' in text)
        self.monitor('screendump', {'filename':str(self.session/'bios-grub-menu.ppm')})
        self.press('e')
        editor=self.wait_vga(lambda text:'set gfxpayload=keep' in text and '/casper/vmlinuz' in text)
        (self.session/'bios-editor-before.txt').write_text(editor)
        row=editor_kernel_row(editor)
        for _ in range(25): self.press('up')
        self.press('home')
        for _ in range(row): self.press('down')
        self.press('end')
        for char in ' console=ttyS0,115200n8':
            if char==' ': self.press('spc')
            elif char=='=': self.press('equal')
            elif char==',': self.press('comma')
            elif char.isupper(): self.press('shift',char.lower())
            else: self.press(char)
        edited=self.wait_vga(lambda text:any('/casper/vmlinuz' in line and
            'console=ttyS0,115200n8' in line for line in text.splitlines()))
        require('persistent' in edited, 'session console edit lost the staged persistence option')
        self.monitor('screendump', {'filename':str(self.session/'bios-grub-edited.ppm')})
        (self.session/'bios-console-edit.json').write_text(json.dumps({
            'menu':menu,'entryBefore':editor,'entryAfter':edited,
            'sessionOnlyKernelOption':'console=ttyS0,115200n8','usbBootConfigurationEdited':False},indent=2)+'\n')
        (self.session/'kernel-boot-requested.json').write_text('{"requestedByRunner":true}\n')
        self.press('ctrl','x')
        shared.persistence.Guest.shell(self)
        self.shell_ready=True
        self.run('set -eu; test ! -d /sys/firmware/efi; test "$(uname -m)" = x86_64; '
                 'test "$(findmnt -n -o SOURCE /cdrom)" = /dev/sda1; '
                 'findmnt -n -o OPTIONS /cdrom | grep -Eq "(^|,)ro(,|$)"')
        self.check_media_files()

    def _configuration(self, media, variables, session):
        result = configuration(self.physical_fd, session)
        if self.bootstrap_device is not None:
            result = direct.root_bootstrap(result, self.physical_fd,
                self.bootstrap_device, *self.run_as)
        elif self.paused:
            result.append('-S')
        return result

    def wait_for(self, pattern, timeout=240, start=0):
        # Cross-architecture TCG boots are much slower than ARM/HVF boots.
        return super().wait_for(pattern, 1200 if timeout == 240 else timeout, start)


def check_transport(fd, work, device, run_as):
    guest = BiosPhysicalGuest(fd, work/'bios-transport-check', paused=True,
                              device=device, run_as=run_as)
    try:
        metadata = guest.metadata()
        nodes = [item['inserted'] for item in metadata['query-block']]
        require(metadata['query-status']['status'] == 'prelaunch' and len(nodes) == 1
                and nodes[0]['node-name'] == 'usb-image' and nodes[0]['ro'] is False
                and nodes[0]['drv'] == 'raw'
                and nodes[0]['file'] == device['devicePath'].replace('/dev/disk','/dev/rdisk',1),
                'BIOS transport must have only the pinned physical USB with CPUs paused')
    finally:
        guest.close()


def verify_previous_gpt(fd, device, current, work, uid, gid):
    """Read-only retention checks before any authorization is passed to the writer."""
    previous = interactive.qualified_profile(shared.capture.expected_target(), gpt=True)
    require(current == previous['geometry'], 'reconnected USB differs from the qualified GPT media')
    require(digest(shared.persistence.ordinary(shared.capture.ISO)) == shared.capture.ISO_SHA,
            'authenticated ARM64 inspection ISO changed')
    prepared = json.loads(shared.persistence.ordinary(Path(previous['prepared'])).read_text())
    staged = shared.persistence.ordinary(Path(prepared['staged']))
    require(digest(staged) == prepared['stagedSha256'], 'previous GPT stage changed')
    shared.validate_ranges(prepared['writeRanges'], previous['expectedTarget']['capacityBytes'])
    start = current['persistenceOffset']; tail = start+current['persistenceBytes']
    compared = 0
    print('Checking GPT boot files and partition tables after the reported USB power cycle.', flush=True)
    with staged.open('rb') as original:
        for extent in prepared['writeRanges']:
            for lo, hi in [(extent['offset'], min(extent['offset']+extent['size'], start)),
                           (max(extent['offset'], tail), extent['offset']+extent['size'])]:
                for offset in range(lo, hi, 4*1024**2):
                    count = min(4*1024**2, hi-offset)
                    original.seek(offset)
                    require(read_exact(fd, offset, count) == original.read(count),
                            'GPT boot-data retention mismatch at '+str(offset))
                    compared += count
                    if compared % (512*1024**2) == 0:
                        print('GPT retained boot bytes checked: '+str(compared), flush=True)
    header = hardware.finalized_header(fd, current)
    inspection = work/'gpt-retention'
    inspection.mkdir(mode=0o700); os.chown(inspection, uid, gid)
    result = shared.inspect_physical(fd, device, current, inspection, uid, gid, previous)
    result.update(success=True, state='complete', physicalUsbReadOnly=True,
        powerCycleReportedByUser=True, bootInitializedBytesMatched=compared,
        geometry=current, expectedTarget=previous['expectedTarget'],
        savedFileSha256=previous['userFileSha256'], savedSetting=previous['setting'],
        finalizedFilesystemHeader=header)
    record = work/'gpt-replug-report.json'
    record.write_text(json.dumps(result, indent=2)+'\n'); os.chown(record,uid,gid)
    print('PASS GPT power-cycle retention: boot bytes, saved file/setting and physical read-only fsck.', flush=True)
    return result


def profile():
    source = json.loads((ROOT/'build/ubuntu-iso/amd64-source-verification.json').read_text())
    require(source.get('manifestSignatureVerified') is True
            and source.get('signingKeyFingerprint') == '843938DF228D22F7B3742BC0D94AA3F0EFE21092'
            and source.get('tamperedManifestRejected') is True and source.get('sha256') == ISO_SHA
            and source.get('iso') == ISO.name, 'authenticated AMD64 ISO provenance is required')
    return SimpleNamespace(work=WORK, iso=ISO, sha256=ISO_SHA, scheme='MBR', firmware='BIOS',
        stage_flag='--stage-persistence-bios', inspect_flag='--inspect-staged-mbr',
        write_flag='--write-staged-mbr-bios-authorized', check_previous=verify_previous_gpt,
        guest_factory=BiosPhysicalGuest, check_transport=check_transport)


def continue_written():
    """Continue only the verified run that stopped before its first BIOS guest."""
    read_fd=write_fd=record=None
    report={'success':False,'imageRewritePerformed':False,'physicalDeviceAccess':True,
            'partitionScheme':'MBR','targetSystem':'BIOS','overlayUsed':False}
    try:
        require(sys.platform=='darwin' and os.geteuid()==0, 'continuation requires sudo in the chat Terminal')
        uid,gid=os.environ.get('SUDO_UID',''),os.environ.get('SUDO_GID','')
        require(uid.isdecimal() and gid.isdecimal() and int(uid)>0,'normal sudo caller required')
        uid,gid=int(uid),int(gid)
        hardware.no_active_guest(); setup=profile()
        parent=Path(json.loads((WORK/'prepared.json').read_text())['session'])
        require(parent.resolve().parent==WORK.resolve(),'BIOS preparation is outside its test directory')
        original=json.loads(shared.persistence.ordinary(parent/'report.json').read_text())
        prepared=json.loads(shared.persistence.ordinary(parent/'prepared.json').read_text())
        expected=shared.capture.expected_target()
        native=original.get('nativeWrite',{})
        require(original.get('expectedTarget')==expected and original.get('partitionScheme')=='MBR'
                and original.get('targetSystem')=='BIOS' and original.get('success') is False
                and 'Resource busy' in original.get('error','')
                and original.get('independentInitializedRangesMatched') is True
                and original.get('previousMediaCheck',{}).get('success') is True
                and native.get('success') is True and native.get('verificationCompleted') is True,
                'continuation requires the verified BIOS write stopped by the host remount')
        require(not (parent/'boot-1').exists(), 'a BIOS boot was already attempted in the write run')
        for earlier in parent.glob('continue-*'):
            if not (earlier/'boot-1').exists(): continue
            saved=json.loads(shared.persistence.ordinary(earlier/'report.json').read_text())
            serial=earlier/'boot-1/serial.log'
            require(saved.get('success') is False and 'GRUB text screen timed out' in saved.get('error','')
                    and serial.is_file() and serial.stat().st_size==0
                    and not (earlier/'boot-1/bios-console-edit.json').exists()
                    and not (earlier/'boot-1/kernel-boot-requested.json').exists()
                    and not (earlier/'boot-1/assertions.txt').exists(),
                    'an earlier continuation requested a kernel boot; preserve its evidence for review')
        source=shared.persistence.ordinary(Path(prepared['staged']))
        require(source.stat().st_size==expected['capacityBytes']
                and digest(source)==prepared['stagedSha256'] and digest(setup.iso)==setup.sha256,
                'authenticated BIOS source changed')
        planned=shared.validate_ranges(prepared['writeRanges'],expected['capacityBytes'])
        require(native['bytesWritten']==native['bytesVerified']==planned,'incomplete earlier native transfer')
        geometry=shared.file_layout(source,expected['capacityBytes'])
        require(geometry==prepared['geometry']==original['geometry'],'prepared BIOS identity changed')
        work=Path(tempfile.mkdtemp(prefix='continue-',dir=parent)); os.chown(work,uid,gid)
        report.update({key:original[key] for key in ['expectedTarget','geometry','nativeWrite',
            'previousMediaCheck','backupSkippedByUser','independentInitializedRangesMatched']})
        report.update(state='acquiring-verified-written-usb',sourceWriteReport=str(parent/'report.json'),
                      physicalWritesStarted=True,hostRemountObservedAfterWriteVerification=True)
        record=work/'report.json'; record.write_text(json.dumps(report,indent=2)+'\n'); os.chown(record,uid,gid)
        (WORK/'last-continuation.json').write_text(json.dumps({'session':str(work)})+'\n')
        args=SimpleNamespace(helper=shared.HELPER,serial=expected['serial'])
        device=shared.discover(args,expected,by_serial=True)
        shared.command(['/usr/sbin/diskutil','unmountDisk',device['devicePath']])
        device=shared.discover(args,expected,by_serial=True)
        require(not device.get('mountPoints'),'USB volumes remain mounted')
        read_fd=shared.open_readonly(device)
        write_fd=hardware.open_readwrite(device,read_fd)
        require(shared.physical_layout(read_fd,expected['capacityBytes'])==geometry,
                'physical BIOS layout or persistence UUID changed')
        hardware.finalized_header(read_fd,geometry)
        report['state']='recording-physical-boot-area-baseline'
        record.write_text(json.dumps(report,indent=2)+'\n')
        shared.qualify_written_media(read_fd,write_fd,device,geometry,work,uid,gid,setup,report,record)
        require(all((work/('boot-'+str(phase))/'media-check.txt').is_file() for phase in (1,2)),
                'both BIOS boots must independently check the authenticated Ubuntu file manifest')
        report['ubuntuFileManifestsPassed']=True
        os.close(write_fd);write_fd=None
        os.close(read_fd);read_fd=None
        device=shared.discover(args,expected,by_serial=True)
        shared.command(['/usr/sbin/diskutil','eject',device['devicePath']])
        report.update(success=True,state='complete',ejected=True)
        print('PASS physical MBR + BIOS continuation: two persistence boots and direct read-only fsck.',flush=True)
        return 0
    except (Exception,KeyboardInterrupt) as error:
        report['success']=False;report['error']=str(error) or 'interrupted'
        print('Physical BIOS continuation stopped: '+report['error'],file=sys.stderr,flush=True)
        return 1
    finally:
        for fd in (write_fd,read_fd):
            if fd is not None:os.close(fd)
        if record is not None:
            record.write_text(json.dumps(report,indent=2)+'\n');print('Report: '+str(record),flush=True)


def main():
    if sys.argv[1:] == ['--prepare']:
        return shared.prepare(profile())
    if sys.argv[1:]==['--continue-written-usb']:
        return continue_written()
    require(sys.argv[1:] == ['--erase-authorized-kingston','--no-backup'],
            'use --prepare or explicit --erase-authorized-kingston --no-backup')
    return shared.write_and_test(make_backup=False, profile=profile())


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (Exception, KeyboardInterrupt) as error:
        print('Physical BIOS test refused/failed: '+str(error), file=sys.stderr, flush=True)
        sys.exit(1)
