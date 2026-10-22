#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only capture of the pinned USB; persistence tests use a separate local copy.

No device argument is accepted. --capture requires sudo; --test refuses root.
The physical device is only ever opened O_RDONLY and is ejected after capture.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace

from capture_usb_for_qemu import copy_readonly
from image_write_integration import digest, require
from physical_usb_integration import command, discover, open_readonly
import persistence_qemu_integration as persistence

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / 'build/ubuntu-iso'
WORK = ROOT / 'build/physical-persistence-test'
ISO = DATA / 'ubuntu-24.04.5-live-server-arm64.iso'
ISO_SHA = 'f12484464c7d4d73e2167e0a7dbda1e669f8fae9bcf778dc0944937034b320cb'
IDENTITY_KEYS = ('serial', 'stableId', 'vendor', 'model', 'capacityBytes', 'sectorBytes')


def expected_target():
    target = json.loads((DATA / 'physical-usb-target.json').read_text())
    previous = json.loads((DATA / 'physical-usb-replug-report.json').read_text())
    expected = {key: target[key] for key in IDENTITY_KEYS}
    require(previous.get('success') is True and previous.get('writeAttempted') is False
            and previous.get('expectedTarget') == expected,
            'pinned USB identity must match the previously qualified physical device')
    return expected


def capture():
    require(sys.platform == 'darwin' and os.geteuid() == 0,
            'run --capture with sudo from the normal Terminal')
    expected = expected_target()
    uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
    require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0,
            'normal sudo caller required for local capture ownership')
    WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
    require(not WORK.is_symlink(), 'capture directory cannot be a symlink')
    os.chown(WORK, int(uid), int(gid))
    media, record = WORK / 'usb-capture.img', WORK / 'capture-report.json'
    require(not media.exists() and not media.is_symlink()
            and not record.exists() and not record.is_symlink(),
            'existing captures or reports are never overwritten')
    args = SimpleNamespace(helper=ROOT / 'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                           serial=expected['serial'])
    device = discover(args, expected, by_serial=True)
    command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
    device = discover(args, expected, by_serial=True)
    fd = open_readonly(device)
    created = False
    try:
        local = os.open(media, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        created = True
        with os.fdopen(local, 'wb') as output:
            sha, _ = copy_readonly(fd, output, expected['capacityBytes'], 0)
            output.flush()
            os.fsync(output.fileno())
            os.fchown(output.fileno(), int(uid), int(gid))
            os.fchmod(output.fileno(), 0o400)
    except BaseException:
        if created:
            media.unlink(missing_ok=True)
        raise
    finally:
        os.close(fd)
    require(digest(persistence.ordinary(media)) == sha, 'whole local copy differs from USB read data')
    geometry = persistence.layout(media, expected['capacityBytes'])
    headers = persistence.finalized_counters(media, expected['capacityBytes'])
    device = discover(args, expected, by_serial=True)
    command(['/usr/sbin/diskutil', 'eject', device['devicePath']])
    report = {'success': True, 'physicalUsbOpenedReadOnly': True, 'ejected': True,
              'expectedTarget': expected, 'capturedBytes': expected['capacityBytes'],
              'wholeDiskSha256': sha, 'capture': str(media), 'geometry': geometry,
              'initialFilesystemHeader': headers}
    with record.open('x') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    os.chown(record, int(uid), int(gid))
    print('PASS read-only persistence USB capture; USB safely ejected.', flush=True)
    print('Next: run this script with --test, without sudo.', flush=True)


def checked_capture():
    record = json.loads(persistence.ordinary(WORK / 'capture-report.json').read_text())
    expected = expected_target()
    media = persistence.ordinary(WORK / 'usb-capture.img')
    require(record.get('success') is True and record.get('physicalUsbOpenedReadOnly') is True
            and record.get('ejected') is True and record.get('expectedTarget') == expected
            and record.get('capturedBytes') == expected['capacityBytes']
            and record.get('capture') == str(media)
            and media.stat().st_size == expected['capacityBytes']
            and digest(media) == record.get('wholeDiskSha256'),
            'physical USB capture provenance or whole-disk hash changed')
    require(persistence.layout(media, expected['capacityBytes']) == record.get('geometry'),
            'captured partition geometry changed')
    persistence.finalized_counters(media, expected['capacityBytes'])
    return media, record


def test_capture():
    require(os.geteuid() != 0, 'run --test without sudo; QEMU only receives ordinary local files')
    require(digest(persistence.ordinary(ISO)) == ISO_SHA, 'authenticated Ubuntu ISO changed')
    original, provenance = checked_capture()
    work = Path(tempfile.mkdtemp(prefix='run-', dir=WORK))
    report = {'success': False, 'physicalUsbOpenedForWriting': False,
              'physicalCapture': provenance, 'sourceIsoSha256': ISO_SHA}
    try:
        media = work / 'writable-test-copy.img'
        with original.open('rb') as source, media.open('xb') as target:
            shutil.copyfileobj(source, target, 8 * 1024**2)
        require(digest(media) == provenance['wholeDiskSha256'], 'writable test copy differs from USB capture')
        report['media'] = str(media)
        report.update(persistence.boot_twice(media, work, provenance['capturedBytes']))
        report['success'] = False
        report.update(persistence.check_filesystem(ISO, media, work))
        require(digest(original) == provenance['wholeDiskSha256'], 'pristine physical capture changed')
        report['pristinePhysicalCaptureUnchanged'] = True
        report['success'] = True
        print('PASS physical USB capture: persistence survived two independent boots; filesystem clean.', flush=True)
    except Exception as error:
        report['success'] = False
        report['error'] = str(error)
        raise
    finally:
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Report: ' + str(work / 'report.json'), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--capture', action='store_true')
    action.add_argument('--test', action='store_true')
    args = parser.parse_args()
    try:
        capture() if args.capture else test_capture()
        return 0
    except Exception as error:
        print('Physical persistence test refused/failed: ' + str(error), file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    sys.exit(main())
