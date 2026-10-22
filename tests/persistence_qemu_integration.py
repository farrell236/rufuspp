#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manual ARM64 persistence test using only new ordinary files and local QEMU.

No physical device, networking, shared folder, direct kernel, snapshot or
installation target is used. Two independent VM processes reuse one writable
USB image, with a fresh firmware variable store on each boot.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import time
import uuid

from image_write_integration import digest, require
import qemu_usb_boot as boot

ROOT = Path(__file__).resolve().parent.parent
CAPACITY = 8 * 1024**3
PERSISTENCE = 2 * 1024**3


def ordinary(path):
    require(path.is_file() and not path.is_symlink() and stat.S_ISREG(path.stat().st_mode)
            and not str(path.resolve()).startswith('/dev/'), 'only ordinary local files are accepted')
    return path.resolve()


def layout(media, capacity=CAPACITY):
    with ordinary(media).open('rb') as stream:
        mbr = stream.read(512)
        require(mbr[510:] == b'\x55\xaa' and mbr[450] == 0x0c and mbr[466] == 0x83,
                'expected FAT32 and Linux persistence MBR entries')
        fat_start, fat_size = struct.unpack_from('<II', mbr, 454)
        start, size = struct.unpack_from('<II', mbr, 470)
        stream.seek(start * 512 + 1024)
        superblock = stream.read(1024)
    require(type(capacity) is int and PERSISTENCE < capacity <= 64 * 1024**3
            and media.stat().st_size == capacity
            and PERSISTENCE <= size * 512 < PERSISTENCE + 2 * 1024**2
            and (start + size) * 512 == capacity
            and fat_start >= 2048 and fat_size > 0
            and (fat_start + fat_size) <= start, 'unexpected persistence geometry')
    require(superblock[56:58] == b'\x53\xef' and superblock[120:136].rstrip(b'\0') == b'casper-rw',
            'expected the production Casper ext2 filesystem')
    return {'fatOffset': fat_start * 512, 'fatBytes': fat_size * 512,
            'persistenceOffset': start * 512, 'persistenceBytes': size * 512,
            'persistenceUuid': str(uuid.UUID(bytes=superblock[104:120]))}


def region_hash(media, offset, size):
    sha = hashlib.sha256()
    with media.open('rb') as stream:
        stream.seek(offset)
        while size:
            data = stream.read(min(size, 8 * 1024**2))
            require(data, 'short image region')
            sha.update(data)
            size -= len(data)
    return sha.hexdigest()


def verify_range_write(source, media, ranges):
    """Compare every target byte; all skipped bytes must retain the dirty fill."""
    source, media = ordinary(source), ordinary(media)
    capacity = source.stat().st_size
    require(media.stat().st_size == capacity and 0 < capacity <= 16 * 1024**3,
            'range fixture geometry changed')
    end = total = 0
    require(0 < len(ranges) <= 65536, 'missing or excessive initialized ranges')
    for extent in ranges:
        offset, size = extent['offset'], extent['size']
        require(type(offset) is int and type(size) is int and size > 0
                and offset >= end and offset % 512 == 0 and size % 512 == 0
                and offset <= capacity and size <= capacity-offset,
                'invalid initialized range')
        end, total = offset+size, total+size
    expected_hash, actual_hash = hashlib.sha256(), hashlib.sha256()
    index = 0
    with source.open('rb') as original, media.open('rb') as written:
        for offset in range(0, capacity, 1024**2):
            count = min(1024**2, capacity-offset)
            expected = bytearray(b'\xa5' * count)
            while index < len(ranges) and ranges[index]['offset']+ranges[index]['size'] <= offset:
                index += 1
            cursor = index
            while cursor < len(ranges) and ranges[cursor]['offset'] < offset+count:
                extent = ranges[cursor]
                begin, end = max(offset, extent['offset']), min(offset+count, extent['offset']+extent['size'])
                original.seek(begin)
                data = original.read(end-begin)
                require(len(data) == end-begin, 'short initialized source range')
                expected[begin-offset:end-offset] = data
                cursor += 1
            actual = written.read(count)
            require(actual == expected, 'initialized bytes or untouched gap mismatch at '+str(offset))
            expected_hash.update(expected)
            actual_hash.update(actual)
    require(expected_hash.digest() == actual_hash.digest(), 'range target hash mismatch')
    return {'logicalDiskBytes':capacity, 'plannedWriteBytes':total,
            'skippedBytesPreserved':capacity-total, 'initializedRanges':len(ranges),
            'independentWholeTargetSha256':actual_hash.hexdigest()}

def finalized_counters(media, capacity=CAPACITY):
    geometry=layout(media, capacity)
    with media.open('rb') as stream:
        stream.seek(geometry['persistenceOffset']+1024)
        superblock=stream.read(1024)
        blocks=struct.unpack_from('<I',superblock,4)[0]
        per_group=struct.unpack_from('<I',superblock,32)[0]
        require(blocks==PERSISTENCE//4096 and per_group==32768,
                'unexpected ext2 accounting geometry')
        groups=(blocks+per_group-1)//per_group
        stream.seek(geometry['persistenceOffset']+4096)
        descriptors=stream.read(groups*32)
    require(struct.unpack_from('<H',superblock,58)[0]==1,
            'persistence filesystem was not finalized clean after poweroff')
    free_blocks=struct.unpack_from('<I',superblock,12)[0]
    free_inodes=struct.unpack_from('<I',superblock,16)[0]
    require(free_blocks==sum(struct.unpack_from('<H',descriptors,index*32+12)[0]
                            for index in range(groups))
            and free_inodes==sum(struct.unpack_from('<H',descriptors,index*32+14)[0]
                                 for index in range(groups)),
            'post-poweroff superblock counters disagree with group descriptors')
    return {'clean':True,'freeBlocks':free_blocks,'freeInodes':free_inodes}


class Guest:
    def _prepare_session(self, session, variables):
        pass

    def _configuration(self, media, variables, session):
        return boot.configuration(ordinary(media), variables, session)

    def _inherited_fds(self):
        return ()

    def _display_backend(self):
        return 'none'

    def __init__(self, media, session, readonly=False, inspect_media=None):
        self.session, self.process, self.console = session, None, None
        session.mkdir(mode=0o700)
        self.sockets = tempfile.TemporaryDirectory(prefix='rufus-persist-')
        self.socket_dir = Path(self.sockets.name)
        variables = session / 'uefi-vars.fd'
        shutil.copyfile(boot.VARS, variables)
        variables.chmod(0o600)
        self._prepare_session(session, variables)
        command = self._configuration(media, variables, session)
        if not readonly:
            # Model a USB stick with fixed inserted media. A swappable SCSI
            # tray lets casper-stop eject the backing disk before its COW
            # filesystem is unmounted, producing shutdown I/O errors.
            command=[value.replace('drive=usb-image,removable=on,',
                                   'drive=usb-image,removable=off,') for value in command]
        for index, value in enumerate(command):
            if value == '-blockdev':
                node = json.loads(command[index + 1])
                if node['node-name'] == 'usb-image':
                    node['read-only'] = node['file']['read-only'] = readonly
                    command[index + 1] = json.dumps(node)
        if inspect_media is not None:
            command += ['-blockdev', boot.block_node('inspect-image', ordinary(inspect_media), True),
                        '-device', 'virtio-blk-pci,drive=inspect-image,id=inspector']
        command[command.index('-display') + 1] = self._display_backend()
        command[command.index('-qmp') + 1] = 'unix:{},server=on,wait=off'.format(self.socket_dir/'qmp')
        command[command.index('-serial') + 1] = 'chardev:console'
        command += ['-chardev', 'socket,id=console,path={},server=on,wait=off,logfile={}'.format(
            self.socket_dir/'serial', session/'serial.log')]
        (session/'command.json').write_text(json.dumps(command, indent=2)+'\n')
        self.log = (session/'qemu.log').open('w')
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT,
                                        pass_fds=self._inherited_fds())
        try:
            self.console = socket.socket(socket.AF_UNIX)
            self.console.settimeout(1)
            deadline = time.monotonic()+20
            while True:
                require(self.process.poll() is None, 'QEMU exited; see '+str(session/'qemu.log'))
                try:
                    self.console.connect(str(self.socket_dir/'serial'))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    require(time.monotonic()<deadline, 'serial socket did not open')
                    time.sleep(0.1)
        except BaseException:
            self.close()
            raise
        self.transcript = bytearray()

    def metadata(self):
        replies={}
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(10)
            connection.connect(str(self.socket_dir/'qmp'))
            with connection.makefile('rwb') as channel:
                require('QMP' in json.loads(channel.readline()), 'invalid QMP greeting')
                for action in ('qmp_capabilities','query-status','query-block'):
                    channel.write((json.dumps({'execute':action})+'\n').encode())
                    channel.flush()
                    while True:
                        result=json.loads(channel.readline())
                        if 'return' in result or 'error' in result:
                            require('error' not in result, 'QMP query failed')
                            replies[action]=result['return']
                            break
        replies['pid']=self.process.pid
        (self.session/'qmp.json').write_text(json.dumps(replies,indent=2)+'\n')
        return replies

    def receive(self):
        try:
            data = self.console.recv(65536)
            self.transcript.extend(data)
            return data
        except socket.timeout:
            return b''

    def wait_for(self, pattern, timeout=240, start=0):
        deadline, update = time.monotonic()+timeout, time.monotonic()+30
        while True:
            result = re.search(pattern, bytes(self.transcript[start:]), re.DOTALL)
            if result:
                return result
            require(self.process.poll() is None, 'guest exited while waiting for '+str(pattern))
            require(time.monotonic()<deadline, 'guest timeout waiting for '+str(pattern))
            self.receive()
            if time.monotonic()>update:
                print('Waiting for '+self.session.name+' guest…', flush=True)
                update=time.monotonic()+30

    def shell(self):
        self.wait_for(rb'Continue in (?:rich|basic) mode')
        start=len(self.transcript)
        self.console.sendall(b'\x1a')  # Subiquity's documented Enter shell shortcut.
        self.wait_for(rb'root@[^\r\n]*[#$] ', timeout=30, start=start)

    def run(self, script, timeout=30):
        marker='RUFUS_RESULT_'+uuid.uuid4().hex
        start=len(self.transcript)
        # The line-anchored completion token excludes the terminal's echoed command.
        command="bash -c {} ; printf '\\n{}:%s\\n' $?\n".format(
            "'"+script.replace("'", "'\\''")+"'", marker)
        self.console.sendall(command.encode())
        result=self.wait_for(rb'\r?\n'+marker.encode()+rb':([0-9]+)\r?\n', timeout, start)
        output=bytes(self.transcript[start:]).decode(errors='replace')
        require(int(result.group(1))==0, 'guest command failed:\n'+output[-6000:])
        return output

    def shutdown(self):
        # Ubuntu provides this runtime flag for unattended live-media tests.
        # It suppresses only the removable-media confirmation, not normal sync/unmount.
        self.run("set -eu; grep -q /run/casper-no-prompt /sbin/casper-stop; "
                 "touch /run/casper-no-prompt; sync")
        self.console.sendall(b'systemctl poweroff\n')
        deadline=time.monotonic()+180
        start=len(self.transcript)
        acknowledged=False
        while self.process.poll() is None:
            require(time.monotonic()<deadline, 'clean guest poweroff timed out')
            self.receive()
            if not acknowledged and b'Please remove the installation medium' in self.transcript[start:]:
                self.console.sendall(b'\r')
                acknowledged=True
        require(self.process.returncode==0, 'QEMU did not exit cleanly')
        print(self.session.name+': guest powered off; QEMU process exited', flush=True)

    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.console is not None:
            self.console.close()
        self.log.close()
        self.sockets.cleanup()


def boot_twice(media, work, capacity=CAPACITY):
    geometry=layout(media, capacity)
    before=region_hash(media, geometry['fatOffset'], geometry['fatBytes'])
    result=exercise_persistence(geometry, work,
                                lambda session: Guest(media, session),
                                lambda: finalized_counters(media, capacity))
    require(region_hash(media, geometry['fatOffset'], geometry['fatBytes'])==before,
            'guest changed the boot FAT partition')
    result.update({'fatPartitionUnchanged':True, 'physicalDeviceAccess':False})
    return result


def exercise_persistence(geometry, work, guest_factory, finalized_header, after_shutdown=None, firmware='UEFI'):
    """The same guest assertions for file fixtures and explicitly authorized hardware."""
    require(firmware in ('UEFI','BIOS'), 'unsupported qualification firmware')
    token=uuid.uuid4().hex
    data='Rufus++ persistent user file '+token+'\n'
    sha=hashlib.sha256(data.encode()).hexdigest()
    setting='RUFUSPP_TEST_SETTING='+token
    common=("set -eu; "+("test ! -d /sys/firmware/efi; test \"$(uname -m)\" = x86_64; "
                        if firmware=='BIOS' else '')+"grep -qw persistent /proc/cmdline; "
            "test \"$(findmnt -n -o FSTYPE /)\" = overlay; "
            "findmnt -n -o OPTIONS / | grep -Fq upperdir=/cow/upper; "
            "test \"$(blkid -s UUID -o value /dev/sda2)\" = '"+geometry['persistenceUuid']+"'; "
            "findmnt /cow; findmnt /; lsblk -o NAME,FSTYPE,LABEL,UUID,MOUNTPOINTS; "
            "printf 'BOOT_ID='; cat /proc/sys/kernel/random/boot_id; ")
    outputs=[]
    headers=[]
    for phase in (1,2):
        print('Starting independent QEMU boot '+str(phase), flush=True)
        guest=guest_factory(work/('boot-'+str(phase)))
        try:
            guest.shell()
            metadata=guest.metadata()
            nodes=[item['inserted'] for item in metadata['query-block']]
            require(len(nodes)==(3 if firmware=='UEFI' else 1)
                    and next(node for node in nodes if node['node-name']=='usb-image')['ro'] is False,
                    'persistence guest must have one writable USB and its selected firmware only')
            if firmware=='BIOS':
                guest.run('dmesg', timeout=120)  # Collect kernel evidence even without a serial kernel console.
            require(('mounted filesystem '+geometry['persistenceUuid']+' r/w').encode()
                    in guest.transcript, 'kernel did not mount the expected persistence filesystem read-write')
            if phase==1:
                script=(common+"! id rufuspp-test >/dev/null 2>&1; "
                        "useradd --create-home --shell /bin/bash rufuspp-test; "
                        "test ! -e /home/rufuspp-test/rufuspp-persistence.txt; "
                        "test ! -e /etc/rufuspp-persistence-test.conf; "
                        "printf '"+data.replace('\n','\\n')+"' > /home/rufuspp-test/rufuspp-persistence.txt; "
                        "chown rufuspp-test:rufuspp-test /home/rufuspp-test/rufuspp-persistence.txt; "
                        "printf '%s\\n' '"+setting+"' > /etc/rufuspp-persistence-test.conf; "
                        "touch /run/rufuspp-volatile-control; sync; ")
            else:
                script=(common+"id rufuspp-test; "
                        "test ! -e /run/rufuspp-volatile-control; "
                        "test \"$(cat /etc/rufuspp-persistence-test.conf)\" = '"+setting+"'; ")
            script+=("printf '%s  %s\\n' '"+sha+"' /home/rufuspp-test/rufuspp-persistence.txt | sha256sum -c -; "
                     "runuser -u rufuspp-test -- test -r /home/rufuspp-test/rufuspp-persistence.txt; "
                     "cat /etc/rufuspp-persistence-test.conf; sync")
            output=guest.run(script)
            (guest.session/'assertions.txt').write_text(output)
            outputs.append(output)
            print('boot-'+str(phase)+': guest assertions passed', flush=True)
            guest.shutdown()
            if after_shutdown is not None:
                after_shutdown()
            header=finalized_header()
            (guest.session/'filesystem-header.json').write_text(json.dumps(header,indent=2)+'\n')
            headers.append(header)
        finally:
            guest.close()
    boot_ids=[re.search(r'\r?\nBOOT_ID=([0-9a-f-]{36})', output).group(1) for output in outputs]
    require(boot_ids[0]!=boot_ids[1], 'the two guest boots were not independent')
    return {'success':True,'geometry':geometry,'userFileSha256':sha,'setting':setting,
            'persistenceDataSurvived':True,
            'bootIds':boot_ids,'volatileControlAbsentOnSecondBoot':True,
            'guestRequestedPoweroffs':2,'qemuProcessExits':2,
            'freshQemuProcesses':2,
            'finalizedFilesystemHeaders':headers,
            'freshUefiVariableStores':2 if firmware=='UEFI' else 0,
            'firmware':firmware,
            'virtualUsbSwappableMedia':False,
            'networkAttached':False,'sharedFoldersAttached':False}


def require_clean_fsck(output):
    require(all(('Pass '+str(number)+':') in output for number in range(1,6)),
            'filesystem checker did not complete all five passes')
    require(re.search(r'\b(?:wrong|corrupt(?:ed|ion)?|inconsisten\w*|WARNING)\b|Fix\?|Clear\?|\? no',
                      output,re.IGNORECASE) is None,
            'filesystem checker reported an inconsistency despite its exit status')


def check_filesystem(iso, media, work):
    """Independent Ubuntu e2fsck on an unmounted, QEMU-enforced read-only disk."""
    before=digest(media)
    guest=Guest(iso,work/'fsck',readonly=True,inspect_media=media)
    try:
        guest.shell()
        metadata=guest.metadata()
        disk=next(item['inserted'] for item in metadata['query-block']
                  if item['inserted']['node-name']=='inspect-image')
        require(disk['ro'] is True, 'filesystem inspection image must be read-only')
        output=guest.run("set -eu; test -z \"$(findmnt -n -S /dev/vda2)\"; "
                         "blkid /dev/vda2; e2fsck -f -n /dev/vda2",timeout=90)
        (guest.session/'fsck.txt').write_text(output)
        require_clean_fsck(output)
        guest.shutdown()
    finally:
        guest.close()
    require(digest(media)==before,'read-only filesystem inspection changed the disk')
    return {'filesystemChecker':'Ubuntu e2fsck -f -n','filesystemCheckPassed':True,
            'checkerOutputValidated':True,
            'inspectionMediaUnchanged':True,'inspectionDiskReadOnly':True}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=Path,required=True)
    parser.add_argument('--sha256',required=True)
    parser.add_argument('--helper',type=Path,required=True)
    parser.add_argument('--prepared-dir',type=Path,
                        help='reuse a pristine helper-created fixture after full source/destination comparison')
    args=parser.parse_args()
    work=None
    report={'success':False}
    try:
        require(os.geteuid()!=0, 'this file-only test must run without sudo')
        iso,helper=ordinary(args.iso),ordinary(args.helper)
        require(re.fullmatch('[0-9a-f]{64}',args.sha256) and digest(iso)==args.sha256,
                'authenticated source ISO checksum mismatch')
        parent=ROOT/'build/persistence-test'
        parent.mkdir(parents=True,exist_ok=True)
        work=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
        fixture=args.prepared_dir.resolve() if args.prepared_dir else work
        staged,media=fixture/'staged-baseline.img',fixture/'disk-baseline.img'
        if not args.prepared_dir:
            with (fixture/'stage-baseline.json').open('x') as output:
                subprocess.run([str(helper),'--stage-persistence',str(iso),str(staged),str(CAPACITY),str(PERSISTENCE)],
                               stdout=output,check=True)
            stage=json.loads((fixture/'stage-baseline.json').read_text())
            ranges=stage.get('writeRanges',[])
            command=[str(helper),'--write-ranges' if ranges else '--write',str(staged),str(media),
                     str(CAPACITY),'512','1048576','0']
            for extent in ranges:
                command += [str(extent['offset']),str(extent['size'])]
            with (fixture/'write-baseline.json').open('x') as output:
                subprocess.run(command,stdout=output,check=True)
        stage=json.loads((fixture/'stage-baseline.json').read_text())
        write=json.loads((fixture/'write-baseline.json').read_text())
        require(stage.get('success') is True and write.get('success') is True,
                'successful production stage/write records required')
        ranges=stage.get('writeRanges',[])
        if ranges:
            report['rangeWrite']=verify_range_write(staged,media,ranges)
            require(write['bytesWritten'] == write['bytesVerified'] == write['nativeBytesWritten']
                    == write['nativeBytesRead'] == stage['bytesToWrite']
                    == report['rangeWrite']['plannedWriteBytes'], 'native transfer counts disagree')
            require(write['bytesWritten'] < CAPACITY//2, 'fixture did not avoid the full-device write')
            report['rangeWrite'].update({'nativeBytesWritten':write['nativeBytesWritten'],
                                         'nativeBytesRead':write['nativeBytesRead']})
        else:
            require(digest(ordinary(staged))==digest(ordinary(media)), 'pristine disk differs from staged source')
        report.update({'sourceIso':str(iso),'sourceIsoSha256':args.sha256,'media':str(media)})
        report.update(boot_twice(media,work))
        report['success']=False  # Do not retain a partial pass if independent fsck fails.
        report.update(check_filesystem(iso,media,work))
        report['success']=True
        print('PASS persistence: user file and setting survived complete shutdown and fresh VM boot',flush=True)
        return 0
    except Exception as error:
        report['error']=str(error)
        if report.get('persistenceDataSurvived'):
            report['filesystemCheckPassed']=False
        print('Persistence test failed: '+str(error),flush=True)
        return 1
    finally:
        if work is not None:
            (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
            print('Report: '+str(work/'report.json'),flush=True)


if __name__=='__main__':
    raise SystemExit(main())
