#!/usr/bin/env python3
"""Opt-in native transport tests. Creates its own images; accepts no device path.

77 means the host lacks virtual-disk privileges/services, never a test pass.
--require converts unavailable prerequisites into a failure for dedicated CI.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

CAPACITY = 16 * 1024 * 1024
MAX_CAPACITY = 16 * 1024 * 1024 * 1024


def validate_capacity(capacity):
    if (type(capacity) is not int or not CAPACITY <= capacity <= MAX_CAPACITY
            or capacity % (1024 * 1024)):
        raise ValueError("Owned virtual images must be 16 MiB..16 GiB, aligned to 1 MiB")
    return capacity


class Unavailable(RuntimeError):
    pass


def run(args, prerequisite=False, **kwargs):
    try:
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=120, **kwargs)
    except FileNotFoundError as error:
        if prerequisite:
            raise Unavailable("Required tool is unavailable: " + args[0]) from error
        raise
    if result.returncode:
        error = (result.stderr or result.stdout).decode(errors="replace").strip()
        raise (Unavailable if prerequisite else RuntimeError)(
            "{} failed: {}".format(args[0], error))
    return result.stdout


def whole_macos_device(attachment, images, backing):
    devices = [e.get("dev-entry", "") for e in attachment.get("system-entities", [])
               if re.fullmatch(r"/dev/disk[0-9]+", e.get("dev-entry", ""))]
    if len(devices) != 1:
        raise RuntimeError("Attach must return exactly one whole disk")
    matches = [image for image in images.get("images", [])
               if Path(image.get("image-path", "")).resolve() == backing.resolve()]
    if len(matches) != 1 or devices[0] not in [
            e.get("dev-entry") for e in matches[0].get("system-entities", [])]:
        raise RuntimeError("Refusing a device not mapped to this runner's backing image")
    return devices[0]


def check_windows_image(info, backing, capacity=CAPACITY):
    validate_capacity(capacity)
    if (Path(info.get("ImagePath", "")).resolve() != backing.resolve()
            or info.get("BusType") != 15 or info.get("IsBoot") is not False
            or info.get("IsSystem") is not False or info.get("Size") != capacity
            or type(info.get("Number")) is not int or info["Number"] < 0):
        raise RuntimeError("Refusing disk: expected this image's non-system file-backed virtual disk")
    return r"\\.\PhysicalDrive{}".format(info["Number"])


def ps_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def powershell(script, prerequisite=False):
    return run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                "$ErrorActionPreference='Stop'; [Console]::OutputEncoding="
                "[System.Text.UTF8Encoding]::new($false); " + script], prerequisite=prerequisite)


class OwnedImage:
    def __init__(self, directory, capacity=CAPACITY):
        self.directory = directory
        self.capacity = validate_capacity(capacity)
        self.backing = directory / ("target.vhd" if sys.platform == "win32" else "target.img")
        self.device = None
        self.attachment = None
        self.attach_attempted = False

    def create(self):
        if sys.platform == "win32":
            script = self.directory / "create.txt"
            script.write_text('create vdisk file="{}" maximum={} type=fixed\nexit\n'.format(
                self.backing, self.capacity // (1024 * 1024)))
            run(["diskpart.exe", "/s", str(script)], prerequisite=True)
            if not self.backing.exists() or self.backing.stat().st_size < self.capacity + 512:
                raise Unavailable("DiskPart could not create the private fixed VHD; administrator access is required")
        with self.backing.open("r+b" if self.backing.exists() else "xb") as output:
            for _ in range(self.capacity // (1024 * 1024)):
                output.write(b"\xa5" * (1024 * 1024))
            output.flush()
            os.fsync(output.fileno())

    def attach(self, readonly=False):
        if sys.platform == "darwin":
            args = ["hdiutil", "attach", "-nomount", "-noautofsck", "-imagekey",
                    "diskimage-class=CRawDiskImage", "-plist"]
            if readonly:
                args.append("-readonly")
            self.attach_attempted = True
            self.attachment = plistlib.loads(run(args + [str(self.backing)], prerequisite=True))
            # Record only a whole disk returned by this attach, for cleanup even
            # if the independent ownership check below fails.
            candidates = [e.get("dev-entry", "") for e in self.attachment.get("system-entities", [])
                          if re.fullmatch(r"/dev/disk[0-9]+", e.get("dev-entry", ""))]
            if len(candidates) == 1:
                self.device = candidates[0]
            self.check_owned()
        elif sys.platform.startswith("linux"):
            if os.geteuid() != 0:
                raise Unavailable("Loop-device integration requires root in a disposable test host")
            args = ["losetup", "--find", "--show", "--partscan"]
            if readonly:
                args.append("--read-only")
            self.attach_attempted = True
            device = run(args + [str(self.backing)], prerequisite=True).decode().strip()
            if not re.fullmatch(r"/dev/loop[0-9]+", device):
                raise RuntimeError("Unexpected loop-device allocation")
            self.device = device
            self.check_owned()
            if shutil.which("udevadm"):
                run(["udevadm", "settle", "--timeout=10"])
        elif sys.platform == "win32":
            access = "ReadOnly" if readonly else "ReadWrite"
            self.attach_attempted = True
            powershell("Mount-DiskImage -ImagePath {} -NoDriveLetter -Access {} | Out-Null".format(
                ps_string(self.backing), access), prerequisite=True)
            # Cleanup uses the exact image path, never a selected physical disk.
            self.device = "attached-owned-vhd"
            self.device = self.check_owned()
        else:
            raise Unavailable("Virtual-disk integration supports macOS, Linux and Windows")

    def check_owned(self):
        if sys.platform == "darwin":
            images = plistlib.loads(run(["hdiutil", "info", "-plist"]))
            device = whole_macos_device(self.attachment, images, self.backing)
            info = plistlib.loads(run(["diskutil", "info", "-plist", device]))
            if (info.get("WholeDisk") is not True or info.get("VirtualOrPhysical") != "Virtual"
                    or info.get("TotalSize") != self.capacity or info.get("DeviceNode") != device
                    or info.get("DeviceBlockSize") != 512):
                observed = {key: info.get(key) for key in (
                    "DeviceNode", "WholeDisk", "VirtualOrPhysical", "TotalSize", "DeviceBlockSize")}
                raise RuntimeError("Refusing anything except the allocated {} byte / 512-byte "
                                   "virtual whole disk; observed: {}".format(self.capacity, observed))
            return device
        if sys.platform.startswith("linux"):
            info = json.loads(run(["losetup", "--json", "--list", "--output", "NAME,BACK-FILE", self.device]))
            devices = info.get("loopdevices", [])
            if (len(devices) != 1 or devices[0].get("name") != self.device
                    or Path(devices[0].get("back-file", "")).resolve() != self.backing.resolve()):
                raise RuntimeError("Loop device does not belong to this backing image")
            return self.device
        info = json.loads(powershell(
            "$i=Get-DiskImage -ImagePath {}; $d=$i | Get-Disk; "
            "@{{ImagePath=$i.ImagePath;Number=$d.Number;Size=$d.Size;BusType=[int]$d.BusType;"
            "IsBoot=$d.IsBoot;IsSystem=$d.IsSystem}} | ConvertTo-Json -Compress".format(ps_string(self.backing))))
        return check_windows_image(info, self.backing, self.capacity)

    def invoke(self, helper, source, mode, verify=False):
        device = self.check_owned()  # Recheck immediately before obtaining a handle.
        action = "--verify" if verify else "--write"
        if sys.platform == "win32":
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                           ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.CreateFileW(device, 0x80000000 if verify else 0xc0000000,
                                        3, None, 3, 0x80000000, None)
            if handle == ctypes.c_void_p(-1).value:
                raise Unavailable("Cannot open the owned VHD: Windows error {}".format(ctypes.get_last_error()))
            try:
                os.set_handle_inheritable(handle, True)
                startup = subprocess.STARTUPINFO()
                startup.lpAttributeList = {"handle_list": [handle]}
                output = run([str(helper), action, str(handle), str(source), mode, str(self.capacity)],
                             startupinfo=startup, close_fds=True)
            finally:
                kernel.CloseHandle(handle)
        else:
            raw = device.replace("/dev/disk", "/dev/rdisk", 1) if sys.platform == "darwin" else device
            try:
                fd = os.open(raw, (os.O_RDONLY if verify else os.O_RDWR) | os.O_CLOEXEC)
            except PermissionError as error:
                raise Unavailable("Cannot open the owned virtual disk: " + str(error)) from error
            try:
                output = run([str(helper), action, str(fd), str(source), mode, str(self.capacity)], pass_fds=(fd,))
            finally:
                os.close(fd)
        print(output.decode().strip(), flush=True)
        return output.decode().strip()

    def detach(self):
        if not self.attach_attempted:
            return
        if sys.platform == "darwin":
            # A failed attach can still have allocated a device. Query by the
            # exact backing file rather than assuming no device was created.
            info = plistlib.loads(run(["hdiutil", "info", "-plist"]))
            matches = [i for i in info.get("images", [])
                       if Path(i.get("image-path", "")).resolve() == self.backing.resolve()]
            if matches:
                device = whole_macos_device(matches[0], info, self.backing)
                run(["hdiutil", "detach", device])
            remaining = plistlib.loads(run(["hdiutil", "info", "-plist"]))
            if any(Path(i.get("image-path", "")).resolve() == self.backing.resolve()
                   for i in remaining.get("images", [])):
                raise RuntimeError("Backing image is still attached after detach")
        elif sys.platform.startswith("linux"):
            args = ["losetup", "--json", "--list", "--associated", str(self.backing),
                    "--output", "NAME,BACK-FILE"]
            for device in json.loads(run(args)).get("loopdevices", []):
                path = device.get("name", "")
                if (not re.fullmatch(r"/dev/loop[0-9]+", path)
                        or Path(device.get("back-file", "")).resolve() != self.backing.resolve()):
                    raise RuntimeError("Refusing to detach an unowned loop device")
                run(["losetup", "--detach", path])
            if json.loads(run(args)).get("loopdevices", []):
                raise RuntimeError("Backing image is still attached after detach")
        else:
            path = ps_string(self.backing)
            powershell("if ((Get-DiskImage -ImagePath {}).Attached) {{ "
                       "Dismount-DiskImage -ImagePath {} }}; "
                       "if ((Get-DiskImage -ImagePath {}).Attached) {{ throw 'Image remains attached' }}".format(
                           path, path, path))
        self.device = None
        self.attach_attempted = False

    def filesystem_check(self):
        if sys.platform == "darwin":
            self.check_owned()
            partitions = [e.get("dev-entry", "") for e in self.attachment.get("system-entities", [])
                          if e.get("dev-entry", "") == self.device + "s1"]
            if len(partitions) != 1:
                raise RuntimeError("Reattached image has no expected MBR filesystem partition")
            print(run(["/sbin/fsck_msdos", "-n", partitions[0].replace("/dev/disk", "/dev/rdisk", 1)]).decode().strip())
        elif sys.platform.startswith("linux"):
            checker = shutil.which("fsck.fat")
            if not checker:
                raise Unavailable("dosfstools (fsck.fat) is required for filesystem verification")
            self.check_owned()
            print(run([checker, "-n", self.device + "p1"]).decode().strip())
        else:
            # Read-only recognition is independent of the portable formatter.
            info = powershell("Get-DiskImage -ImagePath {} | Get-Disk | Get-Partition | Get-Volume | "
                              "Select-Object FileSystem,FileSystemLabel | ConvertTo-Json -Compress".format(ps_string(self.backing)))
            volumes = json.loads(info)
            if isinstance(volumes, dict):
                volumes = [volumes]
            if not any(v.get("FileSystemLabel") == "RUFUSTEST" and v.get("FileSystem") in ("FAT", "FAT16")
                       for v in volumes):
                raise RuntimeError("Windows did not recognize the expected FAT16 filesystem")


def exercise(helper, mode):
    if sys.platform.startswith("linux") and not shutil.which("fsck.fat"):
        raise Unavailable("dosfstools (fsck.fat) is required for filesystem verification")
    directory = Path(tempfile.mkdtemp(prefix="rufus-owned-virtual-"))
    image = OwnedImage(directory)
    wrote = False
    try:
        source = directory / "source.img"
        run([str(helper), "--prepare", str(source), mode])
        image.create()
        print("Private virtual-disk backing: " + str(image.backing), flush=True)
        image.attach()
        image.invoke(helper, source, mode)
        wrote = True
        image.detach()
        image.attach(readonly=True)
        image.invoke(helper, source, mode, verify=True)
        image.filesystem_check()
        print("PASS {} FAT16 native virtual-disk transport".format(mode), flush=True)
    except Unavailable as error:
        if wrote:
            raise RuntimeError("Virtual-disk validation became unavailable after writing: " + str(error)) from error
        raise
    finally:
        try:
            image.detach()
        except Exception:
            print("Cleanup failed; backing image retained for recovery: " + str(directory), file=sys.stderr)
            raise
        shutil.rmtree(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--require", action="store_true", help="fail instead of skipping unavailable prerequisites")
    args = parser.parse_args()
    try:
        helper = args.helper.resolve(strict=True)
        for mode in ("quick", "full"):
            exercise(helper, mode)
        return 0
    except Unavailable as error:
        print("{}: {}".format("FAIL" if args.require else "SKIP", error), file=sys.stderr)
        return 1 if args.require else 77
    except Exception as error:
        print("FAIL: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
