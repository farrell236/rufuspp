#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only retention check of the pinned, previously qualified BIOS USB."""
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

from image_write_integration import digest, require
import test_physical_bios_usb as bios
import test_physical_gpt_usb as shared

ROOT=shared.ROOT
WORK=ROOT/'build/physical-bios-replug'


def completed_bios_profile():
    root=bios.WORK.resolve()
    work=Path(json.loads((root/'last-continuation.json').read_text())['session']).resolve()
    require(work.parent.parent==root, 'BIOS qualification is outside its expected directory')
    report=json.loads(shared.persistence.ordinary(work/'report.json').read_text())
    expected=shared.capture.expected_target()
    require(report.get('success') is True and report.get('state')=='complete'
            and not report.get('error') and report.get('expectedTarget')==expected
            and report.get('partitionScheme')=='MBR' and report.get('targetSystem')=='BIOS'
            and report.get('geometry',{}).get('partitionScheme')=='MBR'
            and all(report.get(key) is True for key in ('persistenceDataSurvived','filesystemCheckPassed',
                'checkerUsesPhysicalUsb','checkerPhysicalUsbReadOnly','independentPhysicalFileAndSettingMatched',
                'inspectionPersistenceBytesUnchanged','bootAreaUnchanged','ubuntuFileManifestsPassed','ejected')),
            'a completed matching physical BIOS qualification is required')
    return report,work/'report.json'


def check():
    fd=record=None
    report={'success':False,'physicalUsbOpenedReadOnly':True,'physicalWritesAttempted':False,
            'powerCycleReportedByUser':True,'ejected':False}
    try:
        require(sys.platform=='darwin' and os.geteuid()==0,'read-only replug check requires sudo in the chat Terminal')
        uid,gid=os.environ.get('SUDO_UID',''),os.environ.get('SUDO_GID','')
        require(uid.isdecimal() and gid.isdecimal() and int(uid)>0,'normal sudo caller required')
        uid,gid=int(uid),int(gid)
        shared.hardware.no_active_guest()
        previous,provenance=completed_bios_profile()
        require(digest(shared.persistence.ordinary(shared.capture.ISO))==shared.capture.ISO_SHA,
                'authenticated inspection ISO changed')
        WORK.mkdir(mode=0o700,parents=True,exist_ok=True);os.chown(WORK,uid,gid)
        work=Path(tempfile.mkdtemp(prefix='run-',dir=WORK));os.chown(work,uid,gid)
        record=work/'report.json'
        report.update(state='checking-reconnected-bios-usb',expectedTarget=previous['expectedTarget'],
                      geometry=previous['geometry'],sourceQualification=str(provenance))
        record.write_text(json.dumps(report,indent=2)+'\n');os.chown(record,uid,gid)
        (WORK/'last-run.json').write_text(json.dumps({'session':str(work)})+'\n')
        args=SimpleNamespace(helper=shared.HELPER,serial=previous['expectedTarget']['serial'])
        device=shared.discover(args,previous['expectedTarget'],by_serial=True)
        shared.command(['/usr/sbin/diskutil','unmountDisk',device['devicePath']])
        device=shared.discover(args,previous['expectedTarget'],by_serial=True)
        require(not device.get('mountPoints'),'USB volumes remain mounted')
        fd=shared.open_readonly(device)
        require(shared.physical_layout(fd,previous['expectedTarget']['capacityBytes'])==previous['geometry'],
                'reconnected BIOS USB layout or persistence UUID changed')
        report['finalizedFilesystemHeader']=shared.hardware.finalized_header(fd,previous['geometry'])
        report.update(shared.inspect_physical(fd,device,previous['geometry'],work,uid,gid,previous))
        report.update(success=True,state='complete',savedFileSha256=previous['userFileSha256'],
                      savedSetting=previous['setting'],keptConnectedForDualTest=True)
        print('PASS physical BIOS power-cycle retention: saved file/setting, clean fsck, unchanged inspection bytes.',flush=True)
        print('Kingston remains connected and unmounted for the dual-mode test.',flush=True)
        return 0
    except (Exception,KeyboardInterrupt) as error:
        report['success']=False;report['error']=str(error) or 'interrupted'
        print('BIOS replug check stopped: '+report['error'],file=sys.stderr,flush=True)
        return 1
    finally:
        if fd is not None:os.close(fd)
        if record is not None:
            record.write_text(json.dumps(report,indent=2)+'\n');print('Report: '+str(record),flush=True)


if __name__=='__main__':
    require(len(sys.argv)==1,'the read-only replug checker accepts no device or write arguments')
    sys.exit(check())
