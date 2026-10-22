#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Manual, explicitly authorized persistence writes to the pinned Kingston USB.

Two fresh QEMU boots write directly to the physical USB, without an overlay or
captured disk. A third VM checks a verified, read-only partition capture after
the USB is ejected. --verify-physical-usb performs only the finishing readback
checks. This script never formats a disk or restores a backup automatically,
and is not a CTest.
"""
import array
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from image_write_integration import digest, require
from physical_usb_integration import command, discover, open_readonly, read_exact
import boot_physical_usb_qemu as direct
import persistence_qemu_integration as persistence
import physical_persistence_qemu as capture
import qemu_usb_boot as boot

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / 'build/physical-usb-persistence'


def no_active_guest():
    running = subprocess.run(['ps', '-axo', 'comm='], capture_output=True, text=True, check=True)
    require(not any('qemu-system' in line or 'Rufus++' in line
                    for line in running.stdout.splitlines()),
            'close QEMU and Rufus++ before the physical persistence test')


def open_readwrite(device, readonly_fd):
    """Open only the already discovered raw device; independently check its handle."""
    path = device['devicePath'].replace('/dev/disk', '/dev/rdisk', 1)
    expected = os.fstat(readonly_fd)
    require(stat.S_ISCHR(expected.st_mode), 'verified read-only raw device required')
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        actual = os.fstat(fd)
        require(stat.S_ISCHR(actual.st_mode) and actual.st_rdev == expected.st_rdev,
                'read/write handle does not match the pinned raw USB')
        sector, blocks = array.array('I', [0]), array.array('Q', [0])
        fcntl.ioctl(fd, 0x40046418, sector, True)
        fcntl.ioctl(fd, 0x40086419, blocks, True)
        require(sector[0] == device['sectorBytes']
                and blocks[0]*sector[0] == device['capacityBytes'],
                'read/write raw USB geometry changed')
        fcntl.fcntl(fd, 48, 1)  # Darwin F_NOCACHE.
        return fd
    except BaseException:
        os.close(fd)
        raise


def physical_node(fd, name, readonly):
    require(type(fd) is int and fd > 2, 'invalid inherited physical descriptor')
    mode = fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE
    require(mode == (os.O_RDONLY if readonly else os.O_RDWR),
            'physical descriptor access mode does not match the requested guest')
    return {'driver': 'raw', 'node-name': name, 'read-only': readonly,
            'file': {'driver': 'host_device', 'filename': '/dev/fd/{}'.format(fd),
                     'read-only': readonly, 'locking': 'off',
                     'cache': {'direct': False, 'no-flush': False}}}


class PhysicalGuest(persistence.Guest):
    def __init__(self, fd, session, inspect_only=False, paused=False, device=None, run_as=None):
        self.physical_fd, self.inspect_only = fd, inspect_only
        self.paused = paused
        self.bootstrap_device, self.run_as = device, run_as
        self.shell_ready = False
        # Check access mode before starting a process or touching a session.
        physical_node(fd, 'inspect-image' if inspect_only else 'usb-image', inspect_only)
        super().__init__(capture.ISO, session, readonly=inspect_only)
        if device is not None:
            try:
                metadata = self.metadata()
                require(metadata['query-status']['status'] == 'prelaunch',
                        'physical guest must remain paused until startup checks pass')
                direct.verify_privilege_drop(self.process.pid, fd, device, run_as[0])
                for name in ('serial.log', 'qemu.log', 'command.json', 'qmp.json'):
                    path = session/name
                    if path.exists(): os.chown(path, *run_as)
            except BaseException:
                self.close()
                raise

    def _prepare_session(self, session, variables):
        if self.bootstrap_device is not None:
            os.chown(session, *self.run_as)
            os.chown(variables, *self.run_as)

    def _configuration(self, media, variables, session):
        result = super()._configuration(media, variables, session)
        node = physical_node(self.physical_fd,
                             'inspect-image' if self.inspect_only else 'usb-image', self.inspect_only)
        if self.inspect_only:
            result += ['-blockdev', json.dumps(node), '-device',
                       'virtio-blk-pci,drive=inspect-image,id=inspector']
        else:
            for index, value in enumerate(result):
                if value == '-blockdev' and json.loads(result[index+1])['node-name'] == 'usb-image':
                    result[index+1] = json.dumps(node)
        result[result.index('-name')+1] = ('Rufus++ physical USB filesystem check' if self.inspect_only
                                         else 'Rufus++ persistence test — writes to Kingston USB')
        # File-only qualifications can inherit descriptors. Physical runs
        # bootstrap the validated raw path before QEMU's own privilege drop.
        result += (['-S'] if self.paused else [])
        if self.bootstrap_device is not None:
            result = direct.root_bootstrap(result, self.physical_fd, self.bootstrap_device, *self.run_as)
        return result

    def _inherited_fds(self):
        return () if self.bootstrap_device is not None else (self.physical_fd,)

    def _display_backend(self):
        return 'none' if self.inspect_only or self.paused else 'cocoa'

    def shell(self):
        if self.bootstrap_device is not None:
            direct.verify_privilege_drop(self.process.pid, self.physical_fd,
                                         self.bootstrap_device, self.run_as[0])
            direct.query(self.socket_dir/'qmp', ['cont'])
        super().shell()
        self.shell_ready = True
        if not self.inspect_only:
            self.run('set -eu; test "$(findmnt -n -o SOURCE /cdrom)" = /dev/sda1; '
                     'findmnt -n -o OPTIONS /cdrom | grep -Eq "(^|,)ro(,|$)"')

    def close(self):
        # If an assertion fails after the shell is available, still try normal
        # Linux shutdown before the inherited emergency process cleanup.
        if self.process is not None and self.process.poll() is None and self.shell_ready:
            try:
                self.shutdown()
            except Exception as error:
                print('Normal shutdown after failure also failed: '+str(error), file=sys.stderr, flush=True)
        super().close()


def hash_region(fd, offset, size, label):
    require(offset >= 0 and size > 0 and offset % 512 == size % 512 == 0,
            'invalid bounded physical read region')
    sha = hashlib.sha256()
    for done in range(0, size, 8 * 1024**2):
        sha.update(read_exact(fd, offset+done, min(8 * 1024**2, size-done)))
        if done % (512 * 1024**2) == 0:
            print('{}: {}/{} bytes read'.format(label, min(done+8*1024**2, size), size), flush=True)
    return sha.hexdigest()


def backup_current_usb(fd, destination, capacity):
    require(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY,
            'backup must use the read-only physical handle')
    with destination.open('xb') as output:
        sha, _ = capture.copy_readonly(fd, output, capacity, 0)
        output.flush()
        os.fsync(output.fileno())
        os.fchmod(output.fileno(), 0o400)
    require(digest(persistence.ordinary(destination)) == sha,
            'fresh local backup differs from the physical USB read data')
    return sha


def finalized_header(fd, geometry):
    start = geometry['persistenceOffset']
    superblock = read_exact(fd, start+1024, 1024)
    blocks, per_group = struct.unpack_from('<I', superblock, 4)[0], struct.unpack_from('<I', superblock, 32)[0]
    require(superblock[56:58] == b'\x53\xef' and blocks == persistence.PERSISTENCE//4096
            and per_group == 32768 and struct.unpack_from('<I', superblock, 24)[0] == 2,
            'physical ext2 accounting geometry changed')
    require(struct.unpack_from('<H', superblock, 58)[0] == 1,
            'physical persistence filesystem is not clean after shutdown')
    groups = (blocks+per_group-1)//per_group
    descriptors = read_exact(fd, start+4096, ((groups*32+511)//512)*512)
    free_blocks, free_inodes = struct.unpack_from('<II', superblock, 12)
    require(free_blocks == sum(struct.unpack_from('<H', descriptors, i*32+12)[0] for i in range(groups))
            and free_inodes == sum(struct.unpack_from('<H', descriptors, i*32+14)[0] for i in range(groups)),
            'physical free-space/inode counters disagree with group descriptors')
    return {'clean': True, 'freeBlocks': free_blocks, 'freeInodes': free_inodes}


def flush_physical(fd):
    require(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDWR,
            'physical cache flush requires the authorized write handle')
    os.fsync(fd)
    fcntl.ioctl(fd, 0x20006416)  # Darwin DKIOCSYNCHRONIZECACHE = _IO('d', 22).


def inspect_physical(fd, geometry, work, device=None, run_as=None):
    before = hash_region(fd, geometry['persistenceOffset'], geometry['persistenceBytes'],
                         'Before read-only filesystem inspection')
    guest = PhysicalGuest(fd, work/'fsck', inspect_only=True, device=device, run_as=run_as)
    try:
        guest.shell()
        metadata = guest.metadata()
        disk = next(item['inserted'] for item in metadata['query-block']
                    if item['inserted']['node-name'] == 'inspect-image')
        require(disk['ro'] is True and disk['drv'] == 'raw',
                'physical filesystem inspector must use a read-only raw disk')
        output = guest.run('set -eu; test -z "$(findmnt -n -S /dev/vda2)"; '
                           'blkid /dev/vda2; e2fsck -f -n /dev/vda2', timeout=120)
        (guest.session/'fsck.txt').write_text(output)
        persistence.require_clean_fsck(output)
        guest.shutdown()
    finally:
        guest.close()
    require(hash_region(fd, geometry['persistenceOffset'], geometry['persistenceBytes'],
                        'After read-only filesystem inspection') == before,
            'read-only inspection changed physical persistence bytes')
    return {'filesystemCheckPassed': True, 'filesystemChecker': 'Ubuntu e2fsck -f -n',
            'inspectionPhysicalHandleReadOnly': True, 'inspectionPersistenceBytesUnchanged': True}


def check_transport(fd, work, device, run_as):
    """Validate the actual raw handle with guest CPUs paused, before making a backup."""
    guest = PhysicalGuest(fd, work/'transport-check', paused=True, device=device, run_as=run_as)
    try:
        metadata = guest.metadata()
        disk = next(item['inserted'] for item in metadata['query-block']
                    if item['inserted']['node-name'] == 'usb-image')
        require(metadata['query-status']['status'] == 'prelaunch'
                and disk['drv'] == 'raw' and disk['ro'] is False,
                'physical transport check did not remain paused with a raw disk')
        direct.query(guest.socket_dir/'qmp', ['quit'])
        require(guest.process.wait(timeout=10) == 0, 'paused transport check did not exit normally')
    finally:
        guest.close()
    print('PASS physical descriptor transport check (guest CPUs paused; no guest writes).', flush=True)


def capture_post_write(fd, geometry, work, report, uid, gid):
    """Read current physical bytes; the checker later receives only an ordinary file."""
    require(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY,
            'post-write capture requires the physical read-only handle')
    snapshot = work/'persistence-readback.img'
    sha = hashlib.sha256()
    start, size = geometry['persistenceOffset'], geometry['persistenceBytes']
    require(start > 0 and size > 0 and start % 512 == size % 512 == 0,
            'invalid physical partition capture bounds')
    with snapshot.open('xb') as output:
        for done in range(0, size, 8*1024**2):
            data = read_exact(fd, start+done, min(8*1024**2, size-done))
            require(output.write(data) == len(data), 'short persistence capture write')
            sha.update(data)
        output.flush()
        os.fsync(output.fileno())
        os.fchown(output.fileno(), uid, gid)
        os.fchmod(output.fileno(), 0o400)
    checksum = sha.hexdigest()
    require(digest(snapshot) == checksum, 'local persistence capture differs from physical read data')
    require(hash_region(fd, start, size, 'Independent physical persistence readback') == checksum,
            'physical persistence changed during read-only capture')
    header = finalized_header(fd, geometry)
    report.update({'partitionReadback': str(snapshot), 'partitionReadbackSha256': checksum,
            'physicalReadbackIndependentlyMatched': True, 'postWriteFilesystemHeader': header})
    report['bootAreaAfterSha256'] = hash_region(fd, 0, start, 'Physical boot area verification')
    report['bootAreaUnchanged'] = report['bootAreaAfterSha256'] == report['bootAreaBeforeSha256']
    require(report['bootAreaUnchanged'],
            'physical boot area differs from its pre-write backup')
    return {'bootAreaUnchanged': True, 'fatPartitionUnchanged': True}


def inspect_partition_capture(snapshot, work, expected_sha, expected_setting):
    require(re.fullmatch('[0-9a-f]{64}', expected_sha) is not None
            and re.fullmatch('RUFUSPP_TEST_SETTING=[0-9a-f]{32}', expected_setting) is not None,
            'invalid saved persistence test fingerprints')
    before = digest(persistence.ordinary(snapshot))
    guest = persistence.Guest(capture.ISO, work/'readback-fsck', readonly=True, inspect_media=snapshot)
    try:
        guest.shell()
        metadata = guest.metadata()
        disk = next(item['inserted'] for item in metadata['query-block']
                    if item['inserted']['node-name'] == 'inspect-image')
        require(disk['ro'] is True and disk.get('file') == str(snapshot),
                'checker must use only the read-only captured partition')
        output = guest.run('set -eu; test -z "$(findmnt -n -S /dev/vda)"; '
                           'blkid /dev/vda; e2fsck -f -n /dev/vda', timeout=120)
        (guest.session/'fsck.txt').write_text(output)
        persistence.require_clean_fsck(output)
        output = guest.run('set -euo pipefail; '
            'test "$(debugfs -R \'cat /upper/home/rufuspp-test/rufuspp-persistence.txt\' '
            '/dev/vda 2>/dev/null | sha256sum | cut -d\' \' -f1)" = \''+expected_sha+'\'; '
            'test "$(debugfs -R \'cat /upper/etc/rufuspp-persistence-test.conf\' '
            '/dev/vda 2>/dev/null)" = \''+expected_setting+'\'; '
            'printf \'Physical readback file and setting match.\\n\'')
        (guest.session/'readback-assertions.txt').write_text(output)
        guest.shutdown()
    finally:
        guest.close()
    require(digest(snapshot) == before, 'read-only checker changed the partition capture')
    return {'filesystemCheckPassed': True, 'filesystemChecker': 'Ubuntu e2fsck -f -n on physical readback',
            'checkerMediaReadOnly': True, 'checkerMediaUnchanged': True,
            'physicalReadbackUserFileMatched': True, 'physicalReadbackSettingMatched': True}


def drop_supervisor(uid, gid):
    os.initgroups(pwd.getpwuid(uid).pw_name, gid)
    os.setgid(gid)
    os.setuid(uid)
    require(os.getuid() == os.geteuid() == uid != 0, 'supervisor privilege drop failed')


def verify_only():
    """Complete readback of an already passed physical write/reboot phase; no RW open."""
    fd, work = None, None
    report = {'success': False, 'mode': 'read-only-post-write-verification', 'newPhysicalWrites': False}
    try:
        require(sys.platform == 'darwin' and os.geteuid() == 0,
                'run --verify-physical-usb with sudo for read-only USB access')
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0, 'normal sudo caller required')
        uid, gid = int(uid), int(gid)
        os.umask(0o022)
        no_active_guest()
        previous_work = Path(json.loads((WORK/'last-run.json').read_text())['session'])
        previous_path = previous_work/'report.json'
        previous = json.loads(persistence.ordinary(previous_path).read_text())
        if previous.get('physicalWriteReport'):
            previous_path = Path(previous['physicalWriteReport'])
            previous = json.loads(persistence.ordinary(previous_path).read_text())
        expected = capture.expected_target()
        require(previous.get('expectedTarget') == expected and previous.get('persistenceDataSurvived') is True
                and previous.get('physicalDeviceAccess') is True and previous.get('overlayUsed') is False
                and len(previous.get('finalizedFilesystemHeaders', [])) == 2
                and all(header.get('clean') is True for header in previous['finalizedFilesystemHeaders']),
                'a successful physical write and two clean poweroffs are required')
        require(digest(persistence.ordinary(capture.ISO)) == capture.ISO_SHA, 'inspection ISO changed')
        geometry = previous['geometry']
        args = SimpleNamespace(helper=ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                               serial=expected['serial'])
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, expected, by_serial=True)
        require(not device.get('mountPoints'), 'USB volumes remain mounted')
        fd = open_readonly(device)
        direct.media_identity(fd, expected['capacityBytes'], geometry['persistenceUuid'])
        finalized_header(fd, geometry)
        work = Path(tempfile.mkdtemp(prefix='verify-', dir=WORK))
        os.chown(work, uid, gid)
        report.update({'physicalWriteReport': str(previous_path), 'expectedTarget': expected,
                       'geometry': geometry, 'persistenceDataSurvived': True,
                       'userFileSha256': previous['userFileSha256'], 'setting': previous['setting'],
                       'bootAreaBeforeSha256': previous['bootAreaBeforeSha256'],
                       'state': 'capturing-physical-readback'})
        record = work/'report.json'
        record.write_text(json.dumps(report, indent=2)+'\n')
        os.chown(record, uid, gid)
        (WORK/'last-run.json').write_text(json.dumps({'session': str(work)})+'\n')
        report.update(capture_post_write(fd, geometry, work, report, uid, gid))
        os.close(fd)
        fd = None
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'eject', device['devicePath']])
        report.update({'ejected': True, 'state': 'checking-captured-partition'})
        record.write_text(json.dumps(report, indent=2)+'\n')
        print('Kingston safely ejected. Checking the captured physical partition read-only.', flush=True)
        drop_supervisor(uid, gid)
        report.update(inspect_partition_capture(Path(report['partitionReadback']), work,
                                               report['userFileSha256'], report['setting']))
        report.update({'success': True, 'state': 'complete'})
        print('PASS physical persistence: two USB boots and independent readback file/setting/fsck verification.', flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        print('Read-only physical verification refused/failed: '+report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        if fd is not None: os.close(fd)
        if work is not None:
            (work/'report.json').write_text(json.dumps(report, indent=2)+'\n')
            print('Report: '+str(work/'report.json'), flush=True)


def main():
    if sys.argv[1:] == ['--verify-physical-usb']:
        return verify_only()
    read_fd, write_fd, work = None, None, None
    report = {'success': False, 'writeGuestStarted': False, 'physicalDeviceAccess': True,
              'networkAttached': False, 'sharedFoldersAttached': False, 'overlayUsed': False}
    try:
        require(sys.argv[1:] == ['--write-to-physical-usb'],
                'run sudo /usr/bin/python3 -B tests/test_physical_usb_persistence.py --write-to-physical-usb')
        require(sys.platform == 'darwin' and os.geteuid() == 0,
                'sudo in the normal Terminal is required to open the physical USB')
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0, 'normal sudo caller required')
        uid, gid = int(uid), int(gid)
        os.umask(0o022)  # Artifacts live inside private, user-owned 0700 directories.
        no_active_guest()
        original, provenance = capture.checked_capture()
        require(digest(persistence.ordinary(capture.ISO)) == capture.ISO_SHA,
                'authenticated inspection ISO changed')
        expected, geometry = provenance['expectedTarget'], provenance['geometry']
        args = SimpleNamespace(helper=ROOT/'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                               serial=expected['serial'])
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, expected, by_serial=True)
        require(not device.get('mountPoints'), 'physical USB still has mounted volumes')
        read_fd = open_readonly(device)
        identity = direct.media_identity(read_fd, expected['capacityBytes'], geometry['persistenceUuid'])
        finalized_header(read_fd, geometry)
        write_fd = open_readwrite(device, read_fd)
        discover(args, expected, by_serial=True)
        # The supervisor retains root to bootstrap each QEMU raw-device open.
        # QEMU uses -run-with to drop root and stays paused until verified.
        WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chown(WORK, uid, gid)
        work = Path(tempfile.mkdtemp(prefix='run-', dir=WORK))
        os.chown(work, uid, gid)
        (WORK/'last-run.json').write_text(json.dumps({'session': str(work)})+'\n')
        report.update({'state': 'checking-transport', 'expectedTarget': expected, 'target': device,
                       'geometry': geometry, 'qemuUid': uid, 'supervisorUid': os.geteuid(),
                       'qemuDropsRootBeforeGuestExecution': True, 'backup': str(original),
                       'qualifiedCaptureSha256': provenance['wholeDiskSha256'], 'physicalIdentity': identity})
        record = work/'report.json'
        record.write_text(json.dumps(report, indent=2)+'\n')
        os.chown(record, uid, gid)
        check_transport(write_fd, work, device, (uid, gid))
        report.update({'state': 'creating-backup', 'pausedTransportCheckPassed': True})
        record.write_text(json.dumps(report, indent=2)+'\n')
        backup = work/'before-physical-test.img'
        backup_sha = backup_current_usb(read_fd, backup, expected['capacityBytes'])
        os.chown(backup, uid, gid)
        require(direct.media_identity(read_fd, expected['capacityBytes'], geometry['persistenceUuid']) == identity,
                'physical identity changed while creating the backup')
        boot_hash = persistence.region_hash(backup, 0, geometry['persistenceOffset'])
        report.update({'state': 'writing-persistence', 'baselineMatchesBackup': True,
                       'writeGuestStarted': True, 'bootAreaBeforeSha256': boot_hash,
                       'backup': str(backup), 'backupSha256': backup_sha})
        record.write_text(json.dumps(report, indent=2)+'\n')
        print('Starting physical persistence test. Keep the Kingston connected.', flush=True)
        def after_shutdown():
            flush_physical(write_fd)
            require(read_exact(read_fd, 0, 512) == read_exact(write_fd, 0, 512),
                    'physical handles no longer agree')
            require(hashlib.sha256(read_exact(read_fd, 0, 512)).hexdigest() == identity['mbrSha256'],
                    'physical partition table changed')
        report.update(persistence.exercise_persistence(
            geometry, work, lambda session: PhysicalGuest(write_fd, session, device=device, run_as=(uid, gid)),
            lambda: finalized_header(read_fd, geometry), after_shutdown))
        report.update({'success': False, 'state': 'checking-filesystem',
                       'physicalDeviceAccess': True})
        os.close(write_fd)
        write_fd = None  # Inspector cannot inherit a physical write handle.
        record.write_text(json.dumps(report, indent=2)+'\n')
        report.update(capture_post_write(read_fd, geometry, work, report, uid, gid))
        os.close(read_fd)
        read_fd = None
        device = discover(args, expected, by_serial=True)
        command(['/usr/sbin/diskutil', 'eject', device['devicePath']])
        report['ejected'] = True
        print('Kingston USB safely ejected.', flush=True)
        report['state'] = 'checking-captured-partition'
        record.write_text(json.dumps(report, indent=2)+'\n')
        drop_supervisor(uid, gid)
        report.update(inspect_partition_capture(Path(report['partitionReadback']), work,
                                               report['userFileSha256'], report['setting']))
        report.update({'success': True, 'state': 'complete'})
        print('PASS: physical USB persistence and independent readback file/setting/fsck verification.', flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['success'] = False
        report['error'] = str(error) or 'interrupted'
        print('Physical persistence test refused/failed: '+report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        for fd in (write_fd, read_fd):
            if fd is not None:
                os.close(fd)
        if work is not None:
            (work/'report.json').write_text(json.dumps(report, indent=2)+'\n')
            print('Report: '+str(work/'report.json'), flush=True)


if __name__ == '__main__':
    sys.exit(main())
