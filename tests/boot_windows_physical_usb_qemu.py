#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Visible boot of the pinned Kingston's Windows media, physically read-only.

Run with sudo from the normal user's Terminal. The VM reads the physical USB;
only private firmware variables and a local copy-on-write overlay are writable.
QEMU drops root while paused and is checked before guest execution begins.
--bios selects SeaBIOS. MBR media needs the Windows MBR/NTFS bootstraps;
GPT/UEFI-only media remains an expected negative BIOS boot check.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import zlib

from image_write_integration import require
from physical_usb_integration import command, discover, open_readonly, read_exact
from boot_physical_usb_qemu import query, root_bootstrap, verify_privilege_drop

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / 'build/windows-usb-boot'
QEMU = Path('/opt/homebrew/bin/qemu-system-x86_64')
QEMU_IMG = Path('/opt/homebrew/bin/qemu-img')
CODE = Path('/opt/homebrew/share/qemu/edk2-x86_64-code.fd')
VARS = Path('/opt/homebrew/share/qemu/edk2-i386-vars.fd')
BIOS = Path('/opt/homebrew/share/qemu/bios-256k.bin')
EXPECTED = {'serial': 'E82A448DA32EF39132D00923',
            'stableId': 'macos:E82A448DA32EF39132D00923:15472047104',
            'vendor': 'Kingston', 'model': 'DataTraveler 3.0',
            'capacityBytes': 15472047104, 'sectorBytes': 512}


def metadata(fd):
    require(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY,
            'physical USB handle must be read-only')
    mbr = read_exact(fd, 0, 512)
    require(mbr[510:] == b'\x55\xaa', 'missing MBR signature')
    if mbr[450] == 0x07:
        first, sectors = struct.unpack_from('<II', mbr, 454)
        helper_first, helper_sectors = struct.unpack_from('<II', mbr, 470)
        total = EXPECTED['capacityBytes'] // 512
        require(mbr[446] == 0x80 and first == 2048 and sectors >= 65536
                and first + sectors <= total - 2048 and not any(mbr[478:510]),
                'unexpected Windows MBR partition bounds')
        if mbr[466] == 0xef:
            require(helper_first == first + sectors and helper_sectors == 2048
                    and helper_first + helper_sectors == total - 2048,
                    'unexpected MBR UEFI:NTFS helper bounds')
            helper = read_exact(fd, helper_first * 512, 512)
            require(helper[510:] == b'\x55\xaa', 'invalid UEFI:NTFS helper')
        else:
            require(not any(mbr[462:478]) and first + sectors == total - 2048,
                    'unexpected BIOS-only partition layout')
            helper_first, helper = None, b''
        boot = read_exact(fd, first * 512, 8192)
        backup = read_exact(fd, (first + sectors - 1) * 512, 512)
        require(boot[3:11] == b'NTFS    ' and boot[510:512] == b'\x55\xaa'
                and struct.unpack_from('<H', boot, 11)[0] == 512
                and struct.unpack_from('<I', boot, 28)[0] == first
                and struct.unpack_from('<Q', boot, 40)[0] == sectors - 1
                and backup == boot[:512], 'invalid NTFS boot geometry or backup')
        bios_code = (any(mbr[:440]) and boot[:3] == b'\xeb\x52\x90'
                     and boot[0x54:0x5a] == b'\xfa\x33\xc0\x8e\xd0\xbc'
                     and 'BOOTMGR'.encode('utf-16-le') in boot)
        return {'partitionScheme': 'MBR', 'ntfsFirstSector': first,
                'ntfsLastSector': first + sectors - 1, 'helperFirstSector': helper_first,
                'legacyMbrBootstrapPresent': any(mbr[:440]),
                'windowsBiosBootstrapPresent': bios_code,
                'metadataSha256': hashlib.sha256(mbr + boot + backup + helper).hexdigest()}
    header = read_exact(fd, 512, 512)
    require(mbr[510:] == b'\x55\xaa' and mbr[450] == 0xee
            and header[:8] == b'EFI PART', 'expected GPT Windows media')
    size, recorded = struct.unpack_from('<II', header, 12)
    require(92 <= size <= 512, 'invalid GPT header length')
    checked = bytearray(header[:size])
    checked[16:20] = bytes(4)
    require(zlib.crc32(checked) == recorded, 'GPT header checksum mismatch')
    table_lba, count, entry_bytes, array_crc = struct.unpack_from('<QIII', header, 72)
    require(table_lba == 2 and count == 128 and entry_bytes == 128,
            'unexpected Windows GPT entry geometry')
    entries = read_exact(fd, table_lba * 512, count * entry_bytes)
    require(zlib.crc32(entries) == array_crc, 'GPT partition checksum mismatch')
    first, last = struct.unpack_from('<QQ', entries, 32)
    helper_first, helper_last = struct.unpack_from('<QQ', entries, 128 + 32)
    require(first == 2048 and first <= last < helper_first <= helper_last
            and helper_last < EXPECTED['capacityBytes'] // 512 - 33
            and helper_last - helper_first + 1 == 2048,
            'unexpected NTFS/UEFI:NTFS partition bounds')
    boot = read_exact(fd, first * 512, 512)
    helper = read_exact(fd, helper_first * 512, 512)
    backup = read_exact(fd, EXPECTED['capacityBytes'] - 512, 512)
    require(boot[3:11] == b'NTFS    ' and boot[510:] == b'\x55\xaa'
            and struct.unpack_from('<I', boot, 28)[0] == first,
            'NTFS boot sector does not match the partition')
    require(helper[510:] == b'\x55\xaa' and backup[:8] == b'EFI PART',
            'UEFI helper or backup GPT is missing')
    return {'ntfsFirstSector': first, 'ntfsLastSector': last,
            'helperFirstSector': helper_first,
            'legacyMbrBootstrapPresent': any(mbr[:440]),
            'metadataSha256': hashlib.sha256(mbr + header + entries + boot + helper + backup).hexdigest()}


def configuration(fd, overlay, variables, session, qmp, bios=False):
    require(type(fd) is int and fd > 2, 'invalid verified USB descriptor')
    base = {'driver': 'raw', 'read-only': True,
            'file': {'driver': 'host_device', 'filename': '/dev/fd/{}'.format(fd),
                     'read-only': True, 'locking': 'off'}}
    media = {'driver': 'qcow2', 'node-name': 'usb-image', 'read-only': False,
             'file': {'driver': 'file', 'filename': str(overlay), 'read-only': False},
             'backing': base}
    # ASCII titles avoid Cocoa displaying UTF-8 separators as legacy glyphs.
    result = [str(QEMU), '-name', 'Rufus++ Windows 11 - physical Kingston USB',
            '-no-user-config', '-nodefaults', '-machine', 'q35',
            '-accel', 'tcg,thread=multi', '-cpu', 'max', '-m', '6144', '-smp', '4',
            '-drive', 'if=pflash,unit=0,format=raw,readonly=on,file=' + str(CODE),
            '-drive', 'if=pflash,unit=1,format=raw,file=' + str(variables),
            '-blockdev', json.dumps(media), '-device', 'qemu-xhci,id=xhci',
            '-device', 'usb-storage,bus=xhci.0,drive=usb-image,removable=on,bootindex=1',
            '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0',
            '-device', 'VGA', '-nic', 'none', '-boot', 'order=c,menu=on,strict=on',
            '-qmp', 'unix:{},server=on,wait=off'.format(qmp),
            '-serial', 'file:' + str(session / 'serial.log'), '-display', 'cocoa', '-S']
    if bios:
        # SeaBIOS receives the identical physical media through a fresh overlay.
        # No UEFI pflash or fallback firmware remains in this configuration.
        while '-drive' in result:
            index = result.index('-drive')
            del result[index:index+2]
        result[result.index('-machine')+1] = 'pc'
        result[result.index('-name')+1] = 'Rufus++ BIOS - physical Kingston USB'
        result += ['-bios', str(BIOS)]
    return result


def main():
    process, fd, record = None, None, None
    report = {'success': False, 'physicalUsbReadOnly': True, 'usesCaptureFile': False,
              'installationDiskAttached': False, 'networkAttached': False,
              'sharedFoldersAttached': False, 'expectedTarget': EXPECTED}
    try:
        require(sys.argv[1:] in ([], ['--bios']) and sys.platform == 'darwin' and os.geteuid() == 0,
                'run sudo /usr/bin/python3 -B tests/boot_windows_physical_usb_qemu.py [--bios]')
        bios = sys.argv[1:] == ['--bios']
        report['firmware'] = 'SeaBIOS' if bios else 'EDK2 UEFI'
        uid, gid = os.environ.get('SUDO_UID', ''), os.environ.get('SUDO_GID', '')
        require(uid.isdecimal() and gid.isdecimal() and int(uid) > 0,
                'normal sudo caller required')
        uid, gid = int(uid), int(gid)
        for path in ((QEMU, QEMU_IMG, BIOS) if bios else (QEMU, QEMU_IMG, CODE, VARS)):
            require(path.is_file(), 'QEMU dependency missing: ' + str(path))
        commands = subprocess.check_output(['/bin/ps', '-axo', 'comm='], text=True).splitlines()
        require(not any(Path(x.strip()).name in ('Rufus++', 'qemu-system-x86_64',
                                                'qemu-system-aarch64') for x in commands),
                'close Rufus++ and any existing QEMU session first')
        args = SimpleNamespace(helper=ROOT / 'build/native-root/tests/rufus_plus_plus_physical_usb_helper',
                               serial=EXPECTED['serial'])
        device = discover(args, EXPECTED, by_serial=True)
        command(['/usr/sbin/diskutil', 'unmountDisk', device['devicePath']])
        device = discover(args, EXPECTED, by_serial=True)
        fd = open_readonly(device)
        before = metadata(fd)
        report['windowsBootExpected'] = (before.get('windowsBiosBootstrapPresent', False)
                                         if bios else before['helperFirstSector'] is not None)
        WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
        require(not WORK.is_symlink(), 'session directory cannot be a symlink')
        os.chown(WORK, uid, gid)
        session = Path(tempfile.mkdtemp(prefix='run-', dir=WORK))
        os.chown(session, uid, gid)
        record = session / 'result.json'
        report.update({'state': 'preparing', 'target': device, 'metadataBefore': before,
                       'session': str(session), 'qemuUid': uid})
        (WORK / 'last-run.json').write_text(json.dumps({'session': str(session)}) + '\n')
        with tempfile.TemporaryDirectory(prefix='rufus-win-') as sockets:
            os.chown(sockets, uid, gid)
            qmp = Path(sockets) / 'qmp'
            overlay, variables = session / 'session-changes.qcow2', session / 'uefi-vars.fd'
            subprocess.run([str(QEMU_IMG), 'create', '-f', 'qcow2', str(overlay),
                            str(EXPECTED['capacityBytes'])], check=True)
            if not bios:
                shutil.copyfile(VARS, variables)
            for path in ((overlay,) if bios else (overlay, variables)):
                path.chmod(0o600)
                os.chown(path, uid, gid)
            launch = root_bootstrap(configuration(fd, overlay, variables, session, qmp, bios=bios),
                                    fd, device, uid, gid)
            report.update({'state': 'launching', 'qmpSocket': str(qmp), 'command': launch,
                           'overlay': str(overlay)})
            record.write_text(json.dumps(report, indent=2) + '\n')
            print('Opening a visible {} window from the physical Kingston.'.format(report['firmware']), flush=True)
            if not report['windowsBootExpected']:
                print('This media lacks the selected firmware bootstrap; Windows is not expected to boot.', flush=True)
            print('USB is opened read-only; guest writes go only to ' + str(overlay), flush=True)
            print('x64 emulation on this ARM Mac can take several minutes.', flush=True)
            print('Session: ' + str(session), flush=True)
            with (session / 'qemu.log').open('w') as log:
                process = subprocess.Popen(launch, stdout=log, stderr=subprocess.STDOUT)
                report['pid'] = process.pid
                deadline = time.monotonic() + 30
                while not qmp.exists():
                    require(process.poll() is None, 'QEMU exited; see ' + str(session / 'qemu.log'))
                    require(time.monotonic() < deadline, 'QEMU control socket timed out')
                    time.sleep(0.1)
                os.chown(qmp, uid, gid)
                qmp.chmod(0o600)
                status = query(qmp, ['query-status', 'query-block'])
                require(status['query-status']['status'] == 'prelaunch',
                        'guest must remain paused until checks pass')
                verify_privilege_drop(process.pid, fd, device, uid)
                require(metadata(fd) == before, 'physical metadata changed during startup')
                report.update({'qmpStartup': status, 'privilegeDropVerified': True})
                query(qmp, ['cont'])
                report['state'] = 'running'
                record.write_text(json.dumps(report, indent=2) + '\n')
                print('QEMU is running as your normal user; its window is visible.', flush=True)
                process.wait()
            require(process.returncode == 0, 'QEMU exited with an error; inspect qemu.log')
            require(metadata(fd) == before, 'physical boot metadata changed')
            report.update({'success': True, 'state': 'exited', 'exitCode': process.returncode,
                           'physicalMetadataUnchanged': True})
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        report['state'] = 'stopped'
        print('Windows USB boot stopped: ' + report['error'], file=sys.stderr, flush=True)
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
            record.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
