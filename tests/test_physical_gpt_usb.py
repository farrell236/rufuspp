#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare GPT media, then explicitly erase/test only the pinned real Kingston.

Prepared files are staging/recovery artifacts. Every boot/persistence exercise
and the final filesystem inspection use the physical USB, without an overlay.
"""
import json
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

from image_write_integration import digest, require
from physical_usb_integration import command, discover, open_readonly, read_exact
import persistence_disk_layout as disk_layout
import physical_persistence_qemu as capture
import persistence_qemu_integration as persistence
import test_physical_usb_persistence as hardware

ROOT = hardware.ROOT
WORK = ROOT/'build/physical-gpt-usb'
STAGER = ROOT/'build/native-root/tests/rufus_plus_plus_image_write_helper'
HELPER = ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper'


def gpt_profile():
    return SimpleNamespace(work=WORK, iso=capture.ISO, sha256=capture.ISO_SHA,
        scheme='GPT', firmware='UEFI', stage_flag='--stage-persistence-gpt',
        inspect_flag='--inspect-staged-gpt', write_flag='--write-staged-gpt-authorized',
        check_previous=None, guest_factory=None, check_transport=None)


def file_layout(path, capacity):
    with persistence.ordinary(path).open('rb') as stream:
        def read(offset, count):
            stream.seek(offset)
            return stream.read(count)
        return disk_layout.layout(read, capacity)


def physical_layout(fd, capacity):
    return disk_layout.layout(lambda offset, count: read_exact(fd, offset, count), capacity)


def acquire_unmounted_handles(args, expected):
    """A delayed Disk Arbitration mount can race the first post-write open."""
    for attempt in range(3):
        read_fd=write_fd=None
        try:
            device=discover(args,expected,by_serial=True)
            command(['/usr/sbin/diskutil','unmountDisk',device['devicePath']])
            device=discover(args,expected,by_serial=True)
            require(not device.get('mountPoints'),'USB volumes remain mounted')
            read_fd=open_readonly(device)
            write_fd=hardware.open_readwrite(device,read_fd)
            device=discover(args,expected,by_serial=True)
            require(not device.get('mountPoints'),'USB remounted while acquiring raw handles')
            return device,read_fd,write_fd
        except BaseException as error:
            for fd in (write_fd,read_fd):
                if fd is not None:os.close(fd)
            if not isinstance(error,OSError) or error.errno!=errno.EBUSY or attempt==2:raise
            print('macOS raw open was busy; revalidating identity and unmounting again.',flush=True)
            time.sleep(0.5)


def validate_ranges(ranges, capacity):
    require(type(ranges) is list and 0 < len(ranges) <= 128, 'invalid initialized-range count')
    end, total = 0, 0
    for extent in ranges:
        offset, size = extent['offset'], extent['size']
        require(type(offset) is int and type(size) is int and offset >= end and size > 0
                and offset % 512 == size % 512 == 0 and offset <= capacity
                and size <= capacity-offset, 'invalid initialized GPT range')
        end, total = offset+size, total+size
    require(ranges[0]['offset'] == 0 and ranges[0]['size'] >= 1024**2
            and end == capacity, 'initialized ranges must include both GPT copies')
    return total


def prepare(profile=None):
    profile = profile or gpt_profile()
    require(os.geteuid() != 0, 'prepare staging files without sudo')
    expected = capture.expected_target()
    require(digest(persistence.ordinary(profile.iso)) == profile.sha256, 'authenticated Ubuntu ISO changed')
    profile.work.mkdir(mode=0o700, parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=profile.work))
    staged = work/'prepared-media.img'
    print('Preparing {} + {} + FAT32 + 2 GiB persistence for the physical Kingston.'.format(
        profile.scheme, profile.firmware), flush=True)
    print('Preparation directory: '+str(work), flush=True)
    with (work/'stage.json').open('x') as output:
        subprocess.run([str(STAGER), profile.stage_flag, str(profile.iso), str(staged),
                        str(expected['capacityBytes']), str(persistence.PERSISTENCE)], stdout=output, check=True)
    stage = json.loads((work/'stage.json').read_text())
    require(stage.get('success') is True and staged.stat().st_size == expected['capacityBytes'],
            'GPT staging did not complete')
    geometry = file_layout(staged, expected['capacityBytes'])
    require(geometry['partitionScheme'] == profile.scheme, 'prepared partition scheme differs')
    require(profile.firmware != 'BIOS' or stage.get('biosBootable') is True,
            'production BIOS bootstrap was not staged')
    require(profile.firmware != 'BIOS+UEFI' or (stage.get('biosBootable') is True
            and stage.get('uefiBootable') is True and stage.get('targetSystem') == 'BIOS+UEFI'),
            'production stage must explicitly support both BIOS and UEFI')
    native = json.loads(command([HELPER, profile.inspect_flag, staged]))
    require(native.get('success') is True and native.get('capacityBytes') == expected['capacityBytes'],
            'native writer does not recognize the prepared source')
    planned = validate_ranges(stage['writeRanges'], expected['capacityBytes'])
    require(planned == stage['bytesToWrite'], 'staging byte counts disagree')
    report = {'prepared': True, 'physicalWritesStarted': False, 'expectedTarget': expected,
              'sourceIsoSha256': profile.sha256, 'staged': str(staged), 'stagedSha256': digest(staged),
              'targetSystem': profile.firmware, 'biosBootable': stage.get('biosBootable', False),
              'uefiBootable': stage.get('uefiBootable', False),
              'geometry': geometry, 'writeRanges': stage['writeRanges'], 'plannedWriteBytes': planned}
    (work/'prepared.json').write_text(json.dumps(report, indent=2)+'\n')
    (profile.work/'prepared.json').write_text(json.dumps({'session': str(work)})+'\n')
    print('Ready. No physical disk was opened or written.', flush=True)
    print('Prepared source SHA-256: '+report['stagedSha256'], flush=True)
    print('Planned native transfer: {}/{} bytes'.format(planned, expected['capacityBytes']), flush=True)
    return 0


class PhysicalUsbInspector(hardware.PhysicalGuest):
    def _configuration(self, media, variables, session):
        result = super()._configuration(media, variables, session)
        # Use USB's sector requests for the physical read-only checker, rather
        # than the raw VirtIO inspection path that failed on this macOS host.
        return [value.replace('virtio-blk-pci,drive=inspect-image,id=inspector',
                              'usb-storage,bus=xhci.0,drive=inspect-image,removable=off')
                for value in result]


def inspect_physical(fd, device, geometry, work, uid, gid, report):
    before = hardware.hash_region(fd, geometry['persistenceOffset'], geometry['persistenceBytes'],
                                  'Before direct physical filesystem inspection')
    guest = PhysicalUsbInspector(fd, work/'physical-fsck', inspect_only=True,
                                 device=device, run_as=(uid, gid))
    try:
        guest.shell()
        metadata = guest.metadata()
        node = next(item['inserted'] for item in metadata['query-block']
                    if item['inserted']['node-name'] == 'inspect-image')
        require(node['ro'] is True and node['drv'] == 'raw'
                and node['file'] == device['devicePath'].replace('/dev/disk', '/dev/rdisk', 1),
                'filesystem checker must read the physical USB directly, read-only')
        partition = '/dev/disk/by-uuid/'+geometry['persistenceUuid']
        output = guest.run('set -eu; p="$(readlink -f '+partition+')"; '
                           'test -b "$p"; test -z "$(findmnt -n -S "$p")"; '
                           'test "$(blockdev --getro "$p")" = 1; '
                           'blkid "$p"; e2fsck -f -n "$p"', timeout=120)
        (guest.session/'fsck.txt').write_text(output)
        persistence.require_clean_fsck(output)
        output = guest.run('set -euo pipefail; p="$(readlink -f '+partition+')"; '
            'test "$(debugfs -R \'cat /upper/home/rufuspp-test/rufuspp-persistence.txt\' '
            '"$p" 2>/dev/null | sha256sum | cut -d\' \' -f1)" = \''+report['userFileSha256']+'\'; '
            'test "$(debugfs -R \'cat /upper/etc/rufuspp-persistence-test.conf\' '
            '"$p" 2>/dev/null)" = \''+report['setting']+'\'; '
            'printf \'Physical persistence file and setting match.\\n\'')
        (guest.session/'readback-assertions.txt').write_text(output)
        guest.shutdown()
    finally:
        guest.close()
    require(hardware.hash_region(fd, geometry['persistenceOffset'], geometry['persistenceBytes'],
                                 'After direct physical filesystem inspection') == before,
            'read-only physical inspection changed persistence bytes')
    return {'filesystemCheckPassed': True, 'checkerUsesPhysicalUsb': True,
            'checkerPhysicalUsbReadOnly': True, 'inspectionPersistenceBytesUnchanged': True,
            'independentPhysicalFileAndSettingMatched': True, 'inspectionPersistenceSha256': before}


def qualify_written_media(read_fd, write_fd, device, geometry, work, uid, gid, profile, report, record):
    expected = report['expectedTarget']
    start = geometry['persistenceOffset']
    tail = start+geometry['persistenceBytes']
    before = hardware.hash_region(read_fd, 0, start, 'Physical boot-area baseline')
    report['bootAreaBaselineSha256'] = before
    tail_before = (hardware.hash_region(read_fd, tail, expected['capacityBytes']-tail,
                                       'Physical backup GPT baseline') if tail < expected['capacityBytes'] else None)
    (profile.check_transport or hardware.check_transport)(write_fd, work, device, (uid, gid))
    report['state'] = 'testing-physical-persistence'
    record.write_text(json.dumps(report, indent=2)+'\n')
    def after_shutdown():
        hardware.flush_physical(write_fd)
        require(physical_layout(read_fd, expected['capacityBytes']) == geometry,
                'physical partition layout changed during a boot')
    factory = profile.guest_factory or hardware.PhysicalGuest
    exercise_args = (geometry, work,
        lambda session: factory(write_fd, session, device=device, run_as=(uid, gid)),
        lambda: hardware.finalized_header(read_fd, geometry), after_shutdown)
    exercise = getattr(profile, 'exercise', None)
    report.update(exercise(*exercise_args) if exercise else persistence.exercise_persistence(*exercise_args, **(
        {'firmware': 'BIOS'} if profile.firmware == 'BIOS' else {})))
    report['success'] = False  # Boot success is not overall qualification success.
    report['state'] = 'checking-physical-filesystem-read-only'
    record.write_text(json.dumps(report, indent=2)+'\n')
    report.update(inspect_physical(read_fd, device, geometry, work, uid, gid, report))
    require(hardware.hash_region(read_fd, 0, start, 'Final physical boot-area verification') == before,
            'physical boot area changed during persistence testing')
    require(tail_before is None or hardware.hash_region(read_fd, tail, expected['capacityBytes']-tail,
                                 'Final physical backup GPT verification') == tail_before,
            'physical backup GPT region changed')
    report['bootAreaUnchanged'] = True
    if tail_before is not None: report['backupGptRegionUnchanged'] = True


def write_and_test(make_backup=True, profile=None):
    profile = profile or gpt_profile()
    read_fd, write_fd, work, record = None, None, None, None
    report = {'success': False, 'physicalWritesStarted': False, 'physicalDeviceAccess': True,
              'partitionScheme': profile.scheme, 'targetSystem': profile.firmware, 'overlayUsed': False,
              'backupSkippedByUser': not make_backup}
    try:
        require(sys.platform == 'darwin' and os.geteuid() == 0,
                'physical qualification requires sudo in the chat Terminal')
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0, 'normal sudo caller required')
        uid, gid = int(uid), int(gid)
        hardware.no_active_guest()
        work = Path(json.loads((profile.work/'prepared.json').read_text())['session'])
        require(work.resolve().parent == profile.work.resolve(), 'prepared session is outside its test directory')
        prepared = json.loads(persistence.ordinary(work/'prepared.json').read_text())
        expected = capture.expected_target()
        require(prepared.get('prepared') is True and prepared['expectedTarget'] == expected,
                'prepared physical identity changed')
        require(not (work/'report.json').exists(), 'this prepared physical run was already attempted')
        staged = persistence.ordinary(Path(prepared['staged']))
        require(staged.stat().st_size == expected['capacityBytes']
                and digest(staged) == prepared['stagedSha256'], 'prepared GPT source changed')
        native_source = json.loads(command([HELPER, profile.inspect_flag, staged]))
        require(native_source.get('success') is True
                and native_source.get('capacityBytes') == expected['capacityBytes'],
                'native GPT source preflight failed')
        require(digest(persistence.ordinary(profile.iso)) == profile.sha256, 'source ISO changed')
        require(profile.firmware != 'BIOS' or (prepared.get('biosBootable') is True
                and prepared.get('targetSystem') == 'BIOS'
                and prepared.get('sourceIsoSha256') == profile.sha256),
                'prepared BIOS qualification identity changed')
        require(profile.firmware != 'BIOS+UEFI' or (prepared.get('biosBootable') is True
                and prepared.get('uefiBootable') is True and prepared.get('targetSystem') == 'BIOS+UEFI'
                and prepared.get('sourceIsoSha256') == profile.sha256),
                'prepared dual-mode qualification identity changed')
        geometry = file_layout(staged, expected['capacityBytes'])
        require(geometry == prepared['geometry'] and geometry['partitionScheme'] == profile.scheme,
                'prepared GPT geometry changed')
        validate_ranges(prepared['writeRanges'], expected['capacityBytes'])
        args = SimpleNamespace(helper=HELPER, serial=expected['serial'])
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, expected, by_serial=True)
        require(not device.get('mountPoints'), 'USB volumes remain mounted')
        read_fd = open_readonly(device)
        current = physical_layout(read_fd, expected['capacityBytes'])
        if profile.check_previous is None:
            original = json.loads((capture.WORK/'capture-report.json').read_text())
            require(current['partitionScheme'] == 'MBR'
                    and current['persistenceUuid'] == original['geometry']['persistenceUuid'],
                    'current USB must match the previous MBR persistence qualification')
        else:
            report['previousMediaCheck'] = profile.check_previous(
                read_fd, device, current, work, uid, gid)
        report.update({'expectedTarget': expected, 'geometry': geometry, 'state': 'preflight-before-erase',
                       'prepared': str(work/'prepared.json'), 'previousGeometry': current})
        record = work/'report.json'
        record.write_text(json.dumps(report, indent=2)+'\n')
        os.chown(record, uid, gid)
        if make_backup:
            report['state'] = 'backing-up-current-usb'
            record.write_text(json.dumps(report, indent=2)+'\n')
            backup = work/'before-gpt-write.img'
            report.update({'backup': str(backup), 'backupSha256': hardware.backup_current_usb(
                read_fd, backup, expected['capacityBytes'])})
            os.chown(backup, uid, gid)
        else:
            print('Skipping backup as requested. Replacing the current Kingston contents.', flush=True)
        os.close(read_fd)
        read_fd = None
        report.update({'state': 'writing-physical-media', 'physicalWriteAttempted': True,
                       'physicalWritesStarted': None})
        record.write_text(json.dumps(report, indent=2)+'\n')
        launch = [str(HELPER), profile.write_flag, str(staged), expected['serial'],
                  expected['stableId'], str(expected['capacityBytes']), str(expected['sectorBytes'])]
        for extent in prepared['writeRanges']: launch += [str(extent['offset']), str(extent['size'])]
        with (work/'native-write.json').open('x') as output:
            result = subprocess.run(launch, stdout=output)
        native = json.loads((work/'native-write.json').read_text())
        report['nativeWrite'] = native
        report['physicalWritesStarted'] = native.get('destructiveWriteStarted') is True
        require(result.returncode == 0 and native.get('success') is True
                and native.get('verificationCompleted') is True,
                'native physical GPT write/verification did not complete')
        report['state'] = 'independent-physical-readback'
        record.write_text(json.dumps(report, indent=2)+'\n')
        device,read_fd,write_fd=acquire_unmounted_handles(args,expected)
        require(physical_layout(read_fd, expected['capacityBytes']) == geometry,
                'physical primary/backup GPT or filesystem identity differs from prepared media')
        with staged.open('rb') as source:
            for extent in prepared['writeRanges']:
                for done in range(0, extent['size'], 4*1024**2):
                    offset = extent['offset']+done
                    count = min(4*1024**2, extent['size']-done)
                    source.seek(offset)
                    require(read_exact(read_fd, offset, count) == source.read(count),
                            'independent physical initialized-range mismatch at '+str(offset))
        report['independentInitializedRangesMatched'] = True
        report['state'] = 'recording-physical-boot-area-baseline'
        record.write_text(json.dumps(report, indent=2)+'\n')
        qualify_written_media(read_fd, write_fd, device, geometry, work, uid, gid, profile, report, record)
        os.close(write_fd)
        write_fd = None
        os.close(read_fd)
        read_fd = None
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'eject', device['devicePath']])
        report.update({'success': True, 'state': 'complete', 'ejected': True})
        print('PASS physical {} + {}: native write/readback, {} persistence boots, direct read-only fsck.'.format(
            profile.scheme, profile.firmware, report['freshQemuProcesses']), flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['success'] = False
        report['error'] = str(error) or 'interrupted'
        print('Physical qualification stopped: '+report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        for fd in (write_fd, read_fd):
            if fd is not None: os.close(fd)
        if record is not None:
            record.write_text(json.dumps(report, indent=2)+'\n')
            print('Report: '+str(record), flush=True)


def main():
    if sys.argv[1:] == ['--prepare']:
        return prepare()
    if sys.argv[1:] == ['--erase-authorized-kingston', '--no-backup']:
        return write_and_test(make_backup=False)
    require(sys.argv[1:] == ['--erase-authorized-kingston'],
            'use --prepare or explicit --erase-authorized-kingston; no device arguments accepted')
    return write_and_test()


if __name__ == '__main__':
    sys.exit(main())
