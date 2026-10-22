#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only diagnosis of boot-area differences on the pinned test Kingston.

No device or baseline arguments are accepted. Only the recorded, verified
pre-write backup and previously authorized physical identity are used.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

from image_write_integration import digest, require
from physical_usb_integration import command, discover, open_readonly, read_exact
import test_physical_usb_persistence as hardware


def compare_region(fd, baseline, size):
    require(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY,
            'comparison requires a read-only source handle')
    require(type(size) is int and size > 0 and size % 512 == 0,
            'comparison must cover complete sectors')
    baseline = hardware.persistence.ordinary(baseline)
    require(baseline.stat().st_size >= size, 'baseline is truncated')
    before_sha, after_sha = hashlib.sha256(), hashlib.sha256()
    differences, sectors, changed_bytes = [], 0, 0
    with baseline.open('rb') as original:
        for offset in range(0, size, 8*1024**2):
            count = min(8*1024**2, size-offset)
            before, after = original.read(count), read_exact(fd, offset, count)
            require(len(before) == count, 'short baseline read')
            before_sha.update(before)
            after_sha.update(after)
            if before != after:
                for position in range(0, count, 512):
                    old, new = before[position:position+512], after[position:position+512]
                    if old != new:
                        sectors += 1
                        changed_bytes += sum(a != b for a, b in zip(old, new))
                        if len(differences) < 256:
                            differences.append({'offset': offset+position,
                                                'beforeHex': old.hex(), 'afterHex': new.hex()})
            if offset % (512*1024**2) == 0:
                print('Read-only boot comparison: {}/{} bytes'.format(offset+count, size), flush=True)
    return {'comparedBytes': size, 'baselineBootAreaSha256': before_sha.hexdigest(),
            'currentBootAreaSha256': after_sha.hexdigest(), 'bootAreaUnchanged': sectors == 0,
            'changedSectors': sectors, 'changedBytes': changed_bytes,
            'sectorDifferences': differences, 'sectorSamplesTruncated': sectors > len(differences)}


def main():
    fd, work = None, None
    report = {'diagnosticCompleted': False, 'physicalUsbReadOnly': True, 'newPhysicalWrites': False}
    try:
        require(len(sys.argv) == 1 and sys.platform == 'darwin' and os.geteuid() == 0,
                'run sudo /usr/bin/python3 -B tests/compare_physical_usb_boot_area.py')
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0, 'normal sudo caller required')
        uid, gid = int(uid), int(gid)
        hardware.no_active_guest()
        latest = Path(json.loads((hardware.WORK/'last-run.json').read_text())['session'])/'report.json'
        previous = json.loads(hardware.persistence.ordinary(latest).read_text())
        source = Path(previous.get('physicalWriteReport', str(latest)))
        previous = json.loads(hardware.persistence.ordinary(source).read_text())
        expected = hardware.capture.expected_target()
        require(previous.get('expectedTarget') == expected
                and previous.get('persistenceDataSurvived') is True
                and previous.get('physicalDeviceAccess') is True and previous.get('overlayUsed') is False,
                'matching physical persistence write evidence required')
        baseline = hardware.persistence.ordinary(Path(previous['backup']))
        require(baseline.stat().st_size == expected['capacityBytes']
                and digest(baseline) == previous['backupSha256'], 'verified pre-write backup changed')
        geometry = previous['geometry']
        args = SimpleNamespace(helper=hardware.ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                               serial=expected['serial'])
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, expected, by_serial=True)
        require(not device.get('mountPoints'), 'USB volumes remain mounted')
        fd = open_readonly(device)
        hardware.direct.media_identity(fd, expected['capacityBytes'], geometry['persistenceUuid'])
        work = Path(tempfile.mkdtemp(prefix='boot-area-diff-', dir=hardware.WORK))
        os.chown(work, uid, gid)
        report.update({'physicalWriteReport': str(source), 'expectedTarget': expected,
                       'baseline': str(baseline), 'geometry': geometry})
        report.update(compare_region(fd, baseline, geometry['persistenceOffset']))
        require(report['baselineBootAreaSha256'] == previous['bootAreaBeforeSha256'],
                'compared baseline does not match pre-write boot-area hash')
        os.close(fd)
        fd = None
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'eject', device['devicePath']])
        report.update({'diagnosticCompleted': True, 'ejected': True})
        print('Comparison complete: {} changed bytes in {} sectors. USB safely ejected.'.format(
            report['changedBytes'], report['changedSectors']), flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        print('Read-only boot comparison stopped: '+report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        if fd is not None: os.close(fd)
        if work is not None:
            record = work/'report.json'
            record.write_text(json.dumps(report, indent=2)+'\n')
            os.chown(record, uid, gid)
            print('Report: '+str(record), flush=True)


if __name__ == '__main__':
    sys.exit(main())
