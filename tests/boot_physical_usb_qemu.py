#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot only the pinned physical Kingston, read-only, with a local writable overlay.

Run with sudo in the normal Terminal. QEMU opens the verified USB O_RDONLY,
then drops root and stays paused until its normal user identity is verified.
No device argument or physical-write mode is accepted.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import uuid

from image_write_integration import require
from physical_usb_integration import command, discover, open_readonly, read_exact
import physical_persistence_qemu as physical
import qemu_usb_boot as boot
import persistence_disk_layout as disk_layout

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / 'build/direct-usb-demo'
QEMU_IMG = Path('/opt/homebrew/bin/qemu-img')


def configuration(fd, overlay, variables, session, qmp, protocol='host_device'):
    require(type(fd) is int and fd > 2, 'invalid inherited USB descriptor')
    require(protocol in ('host_device', 'file'), 'unsupported backing protocol')
    base = {'driver': 'raw', 'read-only': True,
            'file': {'driver': protocol, 'filename': '/dev/fd/{}'.format(fd),
                     'read-only': True, 'locking': 'off'}}
    media = {'driver': 'qcow2', 'node-name': 'usb-image', 'read-only': False,
             'file': {'driver': 'file', 'filename': str(overlay), 'read-only': False},
             'backing': base}
    result = boot.configuration(overlay, variables, session)
    for index, value in enumerate(result):
        if value == '-blockdev' and json.loads(result[index+1])['node-name'] == 'usb-image':
            result[index+1] = json.dumps(media)
    result[result.index('-name')+1] = 'Rufus++ direct Kingston USB — changes saved locally'
    result[result.index('-qmp')+1] = 'unix:{},server=on,wait=off'.format(qmp)
    result = [value.replace('drive=usb-image,removable=on,',
                            'drive=usb-image,removable=off,') for value in result]
    return result


def media_identity(fd, capacity, expected_uuid):
    require(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY,
            'physical USB handle must be read-only')
    mbr = read_exact(fd, 0, 512)
    if mbr[450] == 0xee:
        geometry = disk_layout.layout(lambda offset, count: read_exact(fd, offset, count), capacity)
        require(geometry['persistenceUuid'] == expected_uuid,
                'physical persistence identity differs from the qualified GPT write')
        superblock = read_exact(fd, geometry['persistenceOffset']+1024, 1024)
        return {'mbrSha256': hashlib.sha256(mbr).hexdigest(),
                'persistenceSuperblockSha256': hashlib.sha256(superblock).hexdigest(),
                'partitionLayoutSha256': geometry['partitionLayoutSha256']}
    require(mbr[510:] == b'\x55\xaa' and mbr[450] == 0x0c and mbr[466] == 0x83,
            'expected Ubuntu FAT32 and persistence partitions')
    start, count = struct.unpack_from('<II', mbr, 470)
    require((start+count)*512 == capacity and count*512 >= physical.persistence.PERSISTENCE,
            'physical persistence geometry changed')
    superblock = read_exact(fd, start*512+1024, 1024)
    require(superblock[56:58] == b'\x53\xef'
            and superblock[120:136].rstrip(b'\0') == b'casper-rw'
            and str(uuid.UUID(bytes=superblock[104:120])) == expected_uuid,
            'physical persistence identity differs from the qualified GUI write')
    return {'mbrSha256': hashlib.sha256(mbr).hexdigest(),
            'persistenceSuperblockSha256': hashlib.sha256(superblock).hexdigest()}


def query(qmp, actions):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(str(qmp))
        with connection.makefile('rwb') as channel:
            require('QMP' in json.loads(channel.readline()), 'invalid QMP greeting')
            replies = {}
            for action in ['qmp_capabilities'] + list(actions):
                channel.write((json.dumps({'execute': action})+'\n').encode())
                channel.flush()
                while True:
                    result = json.loads(channel.readline())
                    if 'return' in result or 'error' in result:
                        require('error' not in result, 'QMP command failed: '+str(result))
                        replies[action] = result['return']
                        break
            return replies


def root_bootstrap(command_line, fd, device, uid, gid):
    """Open the validated raw path as root, then QEMU drops privileges while paused."""
    require(os.geteuid() == 0 and type(uid) is int and uid > 0
            and type(gid) is int and gid >= 0, 'root bootstrap requires a normal destination user')
    raw_path = device['devicePath'].replace('/dev/disk', '/dev/rdisk', 1)
    inspected, opened = os.lstat(raw_path), os.fstat(fd)
    import stat
    require(stat.S_ISCHR(inspected.st_mode) and stat.S_ISCHR(opened.st_mode)
            and inspected.st_rdev == opened.st_rdev,
            'bootstrap raw path does not match the verified physical handle')
    result = list(command_line)
    replaced = 0
    def replace(node):
        nonlocal replaced
        if isinstance(node, dict):
            if node.get('driver') == 'host_device' and node.get('filename') == '/dev/fd/{}'.format(fd):
                node['filename'] = raw_path
                replaced += 1
            for value in node.values():
                replace(value)
        elif isinstance(node, list):
            for value in node:
                replace(value)
    for index, value in enumerate(result):
        if value == '-blockdev':
            node = json.loads(result[index+1])
            replace(node)
            result[index+1] = json.dumps(node)
    require(replaced == 1, 'bootstrap must attach exactly one verified physical raw node')
    require('-run-with' not in result, 'duplicate privilege drop configuration')
    result += ['-run-with', 'user={}:{}'.format(uid, gid)]
    if '-S' not in result:
        result.append('-S')
    return result


def verify_privilege_drop(pid, fd, device, uid):
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'ruid=,uid='],
                            capture_output=True, text=True, check=True)
    require(result.stdout.split() == [str(uid), str(uid)],
            'QEMU must drop both real and effective root before guest execution')
    args = SimpleNamespace(helper=ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                           serial=device['serial'])
    current = discover(args, device, by_serial=True)
    require(current['devicePath'] == device['devicePath'], 'physical device path changed during startup')
    require(not current.get('mountPoints'), 'physical USB was remounted by the host before guest execution')
    raw_path = current['devicePath'].replace('/dev/disk', '/dev/rdisk', 1)
    require(os.lstat(raw_path).st_rdev == os.fstat(fd).st_rdev,
            'physical handle changed during QEMU startup')


def main():
    fd, process, record, report = None, None, None, {'success': False}
    try:
        require(len(sys.argv) == 1, 'this launcher accepts no device or write arguments')
        require(sys.platform == 'darwin' and os.geteuid() == 0,
                'run sudo /usr/bin/python3 -B tests/boot_physical_usb_qemu.py in the normal Terminal')
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0,
                'normal sudo caller identity required')
        uid, gid = int(uid), int(gid)
        os.umask(0o022)
        expected = physical.expected_target()
        captured = json.loads((physical.WORK/'capture-report.json').read_text())
        require(captured.get('success') is True and captured.get('expectedTarget') == expected,
                'a qualified physical persistence capture is required')
        args = SimpleNamespace(helper=ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                               serial=expected['serial'])
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, expected, by_serial=True)
        fd = open_readonly(device)
        before = media_identity(fd, expected['capacityBytes'], captured['geometry']['persistenceUuid'])
        WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chown(WORK, uid, gid)
        session = Path(tempfile.mkdtemp(prefix='run-', dir=WORK))
        os.chown(session, uid, gid)
        with tempfile.TemporaryDirectory(prefix='rufus-direct-') as sockets:
            qmp = Path(sockets)/'qmp'
            overlay, variables = session/'session-changes.qcow2', session/'uefi-vars.fd'
            subprocess.run([str(QEMU_IMG), 'create', '-f', 'qcow2', str(overlay),
                            str(expected['capacityBytes'])], check=True)
            overlay.chmod(0o600)
            os.chown(overlay, uid, gid)
            shutil.copyfile(boot.VARS, variables)
            variables.chmod(0o600)
            os.chown(variables, uid, gid)
            launch = configuration(fd, overlay, variables, session, qmp)
            launch = root_bootstrap(launch, fd, device, uid, gid)
            report.update({'state': 'launching', 'physicalUsbReadOnly': True,
                           'usesCaptureFile': False, 'changesStoredLocally': True,
                           'expectedTarget': expected, 'target': device, 'qemuUid': uid,
                           'overlay': str(overlay), 'qmpSocket': str(qmp),
                           'command': launch, 'physicalMetadataBefore': before})
            record = session/'result.json'
            (WORK/'last-run.json').write_text(json.dumps({'session': str(session)})+'\n')
            record.write_text(json.dumps(report, indent=2)+'\n')
            print('Opening QEMU: boot reads come directly from the Kingston USB.', flush=True)
            print('Keep the USB connected. Guest changes go only to '+str(overlay), flush=True)
            print('Session: '+str(session), flush=True)
            with (session/'qemu.log').open('w') as log:
                process = subprocess.Popen(launch, stdout=log, stderr=subprocess.STDOUT)
                report['pid'] = process.pid
                deadline = time.monotonic()+30
                while not qmp.exists():
                    require(process.poll() is None, 'QEMU exited; see '+str(session/'qemu.log'))
                    require(time.monotonic()<deadline, 'QEMU control socket did not open')
                    time.sleep(0.1)
                report['qmp'] = query(qmp, ['query-status', 'query-block'])
                require(report['qmp']['query-status']['status'] == 'prelaunch',
                        'direct demo must remain paused until startup checks pass')
                verify_privilege_drop(process.pid, fd, device, uid)
                query(qmp, ['cont'])
                report['state'] = 'running'
                record.write_text(json.dumps(report, indent=2)+'\n')
                process.wait()
            require(process.returncode == 0, 'QEMU exited with an error; inspect qemu.log')
            require(media_identity(fd, expected['capacityBytes'], captured['geometry']['persistenceUuid']) == before,
                    'physical partition metadata changed during the demo')
            report.update({'success': True, 'state': 'exited', 'exitCode': process.returncode,
                           'physicalMetadataUnchanged': True})
            print('QEMU closed. Physical USB metadata is unchanged.', flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        print('Direct USB demo stopped: '+report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if fd is not None:
            os.close(fd)
        if record is not None:
            record.write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    sys.exit(main())
