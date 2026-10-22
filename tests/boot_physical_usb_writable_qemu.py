#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Interactive persistence demo on the pinned Kingston; explicitly writable.

No formatting or automated guest commands. QEMU opens only the verified USB,
drops root while paused, then lets the user operate the guest normally.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

from image_write_integration import require
from physical_usb_integration import command, discover, open_readonly, read_exact
import boot_physical_usb_qemu as direct
import physical_persistence_qemu as physical
import test_physical_usb_persistence as hardware
import qemu_usb_boot as boot

WORK = direct.ROOT/'build/interactive-usb-persistence'


def qualified_profile(expected, gpt=False):
    if gpt:
        parent = direct.ROOT/'build/physical-gpt-usb'
        session = Path(json.loads((parent/'prepared.json').read_text())['session'])
        require(session.resolve().parent == parent.resolve(), 'GPT profile is outside its qualification directory')
        profile = json.loads(hardware.persistence.ordinary(session/'report.json').read_text())
        require(profile.get('success') is True and profile.get('state') == 'complete'
                and not profile.get('error') and profile.get('partitionScheme') == 'GPT'
                and all(profile.get(key) is True for key in (
                    'checkerUsesPhysicalUsb', 'checkerPhysicalUsbReadOnly', 'filesystemCheckPassed',
                    'inspectionPersistenceBytesUnchanged', 'independentPhysicalFileAndSettingMatched',
                    'bootAreaUnchanged', 'backupGptRegionUnchanged', 'ejected')),
                'a completed physical GPT qualification is required')
    else:
        profile = json.loads((physical.WORK/'capture-report.json').read_text())
    require(profile.get('success') is True and profile.get('expectedTarget') == expected,
            'matching qualified physical USB profile required')
    return profile


def configuration(fd, variables, session, qmp):
    media = hardware.physical_node(fd, 'usb-image', False)
    result = boot.configuration(physical.ISO, variables, session)
    for index, value in enumerate(result):
        if value == '-blockdev' and json.loads(result[index+1])['node-name'] == 'usb-image':
            result[index+1] = json.dumps(media)
    result = [value.replace('drive=usb-image,removable=on,',
                            'drive=usb-image,removable=off,') for value in result]
    result[result.index('-name')+1] = 'Rufus++ Kingston USB — changes saved to USB'
    result[result.index('-qmp')+1] = 'unix:{},server=on,wait=off'.format(qmp)
    return result


def wait_until_exit(process, qmp, request_shutdown=False):
    """Never terminate a writable guest to dismiss a Terminal interrupt."""
    def poweroff():
        print('Requesting normal guest shutdown. If prompted, press Enter in QEMU; keep USB connected.',
              flush=True)
        try:
            direct.query(qmp, ['system_powerdown'])
        except Exception as error:
            print('Please shut down inside the QEMU window: '+str(error), flush=True)
    if request_shutdown and process.poll() is None:
        poweroff()
    while process.poll() is None:
        try:
            process.wait()
        except KeyboardInterrupt:
            poweroff()


def main():
    read_fd, write_fd, process, record = None, None, None, None
    qmp, guest_started = None, False
    report = {'success': False, 'physicalUsbWritable': True, 'overlayUsed': False,
              'networkAttached': False, 'sharedFoldersAttached': False}
    try:
        gpt = sys.argv[1:] == ['--write-to-physical-usb', '--gpt']
        require(sys.argv[1:] == ['--write-to-physical-usb'] or gpt,
                'explicit --write-to-physical-usb is required')
        require(sys.platform == 'darwin' and os.geteuid() == 0,
                'run this launcher with sudo in the chat Terminal')
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0, 'normal sudo caller required')
        uid, gid = int(uid), int(gid)
        hardware.no_active_guest()
        expected = physical.expected_target()
        provenance = qualified_profile(expected, gpt)
        geometry = provenance['geometry']
        args = SimpleNamespace(helper=direct.ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                               serial=expected['serial'])
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, expected, by_serial=True)
        require(not device.get('mountPoints'), 'USB volumes remain mounted')
        read_fd = open_readonly(device)
        before = direct.media_identity(read_fd, expected['capacityBytes'], geometry['persistenceUuid'])
        hardware.finalized_header(read_fd, geometry)
        write_fd = hardware.open_readwrite(device, read_fd)
        WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chown(WORK, uid, gid)
        session = Path(tempfile.mkdtemp(prefix='run-', dir=WORK))
        os.chown(session, uid, gid)
        record = session/'result.json'
        report.update({'state': 'launching', 'expectedTarget': expected, 'geometry': geometry,
                       'qemuUid': uid, 'physicalMetadataBefore': before})
        record.write_text(json.dumps(report, indent=2)+'\n')
        os.chown(record, uid, gid)
        (WORK/'last-run.json').write_text(json.dumps({'session': str(session)})+'\n')
        variables = session/'uefi-vars.fd'
        shutil.copyfile(boot.VARS, variables)
        variables.chmod(0o600)
        os.chown(variables, uid, gid)
        with tempfile.TemporaryDirectory(prefix='rufus-usb-live-') as sockets:
            qmp = Path(sockets)/'qmp'
            launch = direct.root_bootstrap(configuration(write_fd, variables, session, qmp),
                                           write_fd, device, uid, gid)
            report.update({'command': launch, 'qmpSocket': str(qmp)})
            with (session/'qemu.log').open('w') as log:
                # QEMU must not receive Terminal Ctrl-C before Linux can shut down.
                process = subprocess.Popen(launch, stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                report['pid'] = process.pid
                deadline = time.monotonic()+30
                while not qmp.exists():
                    require(process.poll() is None, 'QEMU exited; see '+str(session/'qemu.log'))
                    require(time.monotonic()<deadline, 'QEMU control socket did not open')
                    time.sleep(0.1)
                metadata = direct.query(qmp, ['query-status', 'query-block'])
                report['qmp'] = metadata
                require(metadata['query-status']['status'] == 'prelaunch', 'guest must start paused')
                disks = [item['inserted'] for item in metadata['query-block']]
                usb = next(item for item in disks if item['node-name'] == 'usb-image')
                require(len(disks) == 3 and usb['drv'] == 'raw' and usb['ro'] is False,
                        'guest must have only the writable raw USB and its firmware')
                direct.verify_privilege_drop(process.pid, write_fd, device, uid)
                guest_started = True  # A lost QMP reply must still trigger normal shutdown.
                direct.query(qmp, ['cont'])
                report['state'] = 'running'
                record.write_text(json.dumps(report, indent=2)+'\n')
                print('QEMU is booting directly from Kingston. Changes save to the USB.', flush=True)
                print('Keep it connected. Shut down inside Ubuntu before closing QEMU.', flush=True)
                print('Session: '+str(session), flush=True)
                wait_until_exit(process, qmp)
            require(process.returncode == 0, 'QEMU exited with an error; inspect qemu.log')
        hardware.flush_physical(write_fd)
        report['finalizedFilesystemHeader'] = hardware.finalized_header(read_fd, geometry)
        require(hashlib.sha256(read_exact(read_fd, 0, 512)).hexdigest() == before['mbrSha256'],
                'USB partition table changed during the session')
        if gpt:
            after = direct.media_identity(read_fd, expected['capacityBytes'], geometry['persistenceUuid'])
            require(after['partitionLayoutSha256'] == before['partitionLayoutSha256'],
                    'GPT partition tables changed during the session')
        os.close(write_fd)
        write_fd = None
        os.close(read_fd)
        read_fd = None
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'eject', device['devicePath']])
        report.update({'success': True, 'state': 'complete', 'ejected': True})
        print('Ubuntu stopped cleanly. USB cache flushed; Kingston safely ejected.', flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        print('Interactive USB demo stopped: '+report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        if process is not None and process.poll() is None:
            if guest_started:
                wait_until_exit(process, qmp, request_shutdown=True)
            else:
                # -S kept CPUs paused throughout startup validation.
                try:
                    direct.query(qmp, ['quit'])
                    process.wait(timeout=10)
                except Exception:
                    process.terminate()
                    process.wait(timeout=10)
        for fd in (write_fd, read_fd):
            if fd is not None: os.close(fd)
        if record is not None:
            record.write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    sys.exit(main())
