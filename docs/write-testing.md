# Destructive-write validation

The automated suites establish specific software behavior. They do not qualify
the application for writing valuable physical media. Keep fault tests, real
image-file writes, virtual-device integration, and physical hardware qualification
as separate results.

## Current qualification snapshot

As of 2026-10-22, the local macOS arm64 development build passes **27/27 CTest
entries** with Python 3 and `qemu-img` installed. Coverage includes writer fault
injection, actual ordinary-file I/O, NTFS extraction/readback including files
over 4 GiB, initialized write ranges,
GUI progress, preflight/receipt estimates, independent capture-container decoding
and guarded runner behavior (including 38 physical capture-runner safety cases).
These entries contain multiple test cases; normal CTest does not run physical writes
or mount disposable volumes. A separate privileged-helper configuration builds
cleanly and its native safety, range-protocol and platform-backend checks pass;
signed installation/authorization/runtime writes remain untested.

Physical tests on the pinned Kingston cover Ubuntu DD readback, MBR/GPT
persistence, BIOS → UEFI → BIOS persistence and reconnect retention. Windows NTFS
media reached Windows Setup under GPT/UEFI and, after a user GUI rewrite, under
both UEFI and BIOS from the same MBR dual-mode USB. Both dual-mode Windows guests
exited normally and their final boot-metadata checks matched. Windows guest
writes were isolated in local overlays; the physical backing was read-only.

DD, fixed VHD, dynamic VHD and corrected VHDX physical capture/restore checks
also passed full-byte readback and all dummy-file checks. These results required
protected reconnects after some completed writes; the macOS disk-interface
disappearance remains unresolved and uninterrupted operation is not established.

These are scoped macOS/Kingston/QEMU results. Linux/Windows host runtime, other
USB hardware/sector sizes, actual-machine firmware, Secure Boot, a separate
NTFS BIOS-only layout, and a complete Windows installation remain unqualified.
The release workflows run automated suites on Linux, Windows and macOS; see
[GitHub Actions](https://github.com/farrell236/rufuspp/actions) for build results.
CI coverage does not establish physical-device qualification on those hosts.
Detailed field evidence and reproduction steps follow.

## Real image-file writes (no root required)

With Python 3 available, the default test build includes a real write integration
test. Run it after building:

```console
ctest --preset core-only -R '^native-file-image-write$' --verbose
```

The test writes seven newly created ordinary files through the production
`RawImageWriter`, native transfer/alignment implementation, and actual host OS
write calls. The only target adaptation is file flushing: it performs real
`fsync`/`FlushFileBuffers`, while device-specific cache ioctls remain in the
separate virtual-device test. No production bypass is introduced.

| Case | Target size | Independent checks after writer exit |
| --- | --- | --- |
| Patterned raw image, 512-byte sectors | 12 MiB | Partial final block, all image bytes and preserved gap |
| Patterned raw image, simulated 4096-byte sectors | 12 MiB | Sector alignment, partial final block, all image bytes and gap |
| Full FAT16 image with a known file | 16 MiB | MBR, FAT copies, file chain and contents, filesystem checker |
| Full FAT32 image with a known file | 128 MiB | MBR, FAT copies, backup boot sector, file contents, filesystem checker |
| Quick FAT32 format over stale data | 128 MiB | Format prefix, untouched data gap, zeroed final MiB, filesystem checker |
| Independently generated GPT image | 32 MiB | Protective MBR, both header CRCs, both array CRCs and partition bounds |
| GPT through initialized write ranges | 32 MiB | Only 3 MiB transferred/verified; both GPT copies valid; all 29 MiB of skipped data unchanged |

The C++ writer exits and closes its handle before Python opens the output.
Python compares **every target byte** against the source, preserved nonzero
sentinel data, and zeroed tail as appropriate. It computes expected and actual
whole-target SHA-256 values, checks source/control files remain unchanged, and
deliberately corrupts one byte to prove the independent verifier detects damage.
Existing targets and source-as-target are refused without modification.

All files live in a private temporary directory and are removed after testing.
The C++ helper exclusively creates its destination and rejects existing files
and raw-device namespaces. There is no mount operation or privilege requirement.
The per-case hashes, byte counts and check results are saved in
`build/core-only/tests/image-write-report.json` (or the chosen build directory).
A failed run removes any previous success report.

On macOS, `/sbin/fsck_msdos -n` checks the FAT partitions extracted from the
**written output**. Linux uses `fsck.fat -n` when installed. Other hosts still
perform full byte comparison and the independent structural/file-content
checks; an unavailable external checker is recorded explicitly in the report.

**Scope:** this proves image copying through real filesystem I/O, including
populated filesystems and partition tables. It does not prove raw block-device
opening, exclusive claims/unmounting, device-cache flushing, power-loss
durability, bootability, or physical 4096-byte-sector hardware behavior.

## Direct NTFS staging (macOS and Linux, no root required)

After building the native backends with the normal `dev` preset:

```console
ctest --preset dev -R '^direct-ntfs' --output-on-failure
```

These tests format newly created ordinary files with the application-bundled
formatter and populate them through the linked NTFS library. They never attach a
disk image, mount a filesystem, open a physical device or require FUSE. They cover
Unicode and empty files, MFT/directory-index growth, Windows name collisions,
link/device/path refusal, read-only reopen and file-size/readback verification,
MBR/GPT assembly, Windows NTFS BIOS and dual-mode bootstraps, preserved BPB/MFT
parameters, 512-byte through 64-KiB clusters, allocation write maps including the
entire bootstrap, backup boot sector and disk
tail, full-format semantics, existing-output refusal, and cancellation cleanup.
A separate case writes and reads every byte of a file larger than 4 GiB (needs
about 4.1 GiB free; timeout 300 seconds).

For a downloaded Windows ISO, the test executable also supports a staging-only
check with the production planner/extractor. The output must not already exist:

```console
build/dev/tests/rufus_plus_plus_ntfs_image_tests --stage-iso \
  /absolute/path/to/windows.iso /absolute/path/to/new-stage.img
```

This uses a 15,472,047,104-byte GPT/UEFI:NTFS layout in an ordinary output file and
compares every extracted file before assembling and verifying its allocated
ranges. It does not write a USB or establish firmware/Windows Setup bootability.
The physical Windows USB boot route below covers the GUI-created media.

NTFS preflight and the backend free-space guard use the same host-specific
staging estimate. On macOS/Linux, quick format budgets two copies of the expanded
ISO size plus 10% headroom per copy and a shared 256 MiB reserve; full format budgets
twice target capacity plus that reserve. Windows uses one copy and currently
writes the staged image densely, so its known write size remains target capacity.
Full-format Windows space budgeting likewise uses target capacity rather than
ISO size. Quick macOS/Linux NTFS write size is shown as "Calculated during
preparation", with the actual allocated-range total shown during writing.
Receipts preserve that preflight statement and record actual bytes in the outcome.

The `qt-preflight-sizing` check exercises the same sizing path used by START and
checks FAT32/NTFS quick/full reports, saved receipt contents and overflow blockers.
Platform tests cover the shared space calculation, upward rounding and overflow.
These checks do not prepare media or open a device.

### Physical Windows USB boot

On 2026-10-21 the user created GPT + UEFI + NTFS Windows 11 media through the
macOS GUI and reported successful writing/verification. The physical Kingston
then booted through UEFI:NTFS into Windows 11 Setup's populated language-settings
screen under x86_64 QEMU/EDK2. QMP confirmed the backing node was the actual raw
Kingston device, opened read-only. This establishes that particular media/boot
route; it does not establish installation success or actual-machine firmware
compatibility. Linux runtime coverage remains pending.

To repeat the visible boot of this pinned Kingston, close Rufus++ and other QEMU
sessions, keep the USB connected, and run from the normal user's Terminal:

```console
sudo /usr/bin/python3 -B tests/boot_windows_physical_usb_qemu.py
```

The launcher accepts no arbitrary device or physical-write arguments. It checks
serial, stable ID, geometry and MBR/GPT NTFS metadata, unmounts the USB, opens it
read-only, then launches QEMU paused. QEMU opens that verified physical path
before dropping root; its identity and unmounted target are checked before guest
execution begins. The visible guest has a private writable overlay and firmware
variables, no installation disk, network or shared folders. The USB itself is
never opened for writing. The launcher checks physical boot metadata again when
the guest closes. Evidence is saved under `build/windows-usb-boot/`, including
`boot-verification.json` for the observed Setup screen. A full independent USB
file-manifest comparison was not part of this boot observation.

Adding `--bios` selects a visible SeaBIOS session over the same read-only USB.
The earlier GPT/UEFI media was checked this way: SeaBIOS reached "Booting from
Hard Disk" but did not reach Windows Setup. Its protective MBR contained no
legacy bootstrap, and the boot metadata matched the successful UEFI run. This is
an expected unsupported route, recorded in `build/windows-usb-boot/bios-check.json`.
That negative result describes the earlier GPT media. The NTFS stager now
implements x86/x64 Windows **MBR + BIOS** and **MBR + BIOS/UEFI**. It installs the
Windows MBR/NTFS BOOTMGR bootstraps, preserves filesystem parameters and verifies
the primary/backup boot sectors. The BIOS-only layout omits UEFI:NTFS; dual-mode
retains it. Ordinary-file checks pass, including writing allocated ranges over
stale nonzero data. The downloaded Windows ISO staged successfully in both
profiles and all 977 source files were verified. Staging alone does not establish
BIOS bootability; the subsequent physical boot checks below provide that evidence.

On 2026-10-21 the user rewrote the Kingston through the GUI as MBR + BIOS/UEFI +
NTFS Windows 11 x64 media, reported successful completion and closed Rufus++.
The same connected USB then reached the populated Windows 11 Setup language
screen in two fresh, visible QEMU guests: EDK2 UEFI first, then SeaBIOS on a PC
machine with no UEFI pflash. QMP confirmed each host-device backing node was the
actual raw Kingston and read-only, with guest changes isolated in a fresh local
overlay. QEMU dropped root before guest execution. Both runs observed boot
metadata SHA-256 `09aa58b0cae88a903119bca7945df878516c80b8610ac8b11d343a55c8a11c29`.
Both guests subsequently exited normally with code 0, and both launchers'
final checks confirmed unchanged physical boot metadata. Evidence and
screenshots are recorded in `build/windows-usb-boot/dual-mode-uefi-verification.json`
and `dual-mode-bios-verification.json` (sessions `run-9cu5t8tc` and `run-it_pymm4`).
This validates both boot paths of this particular physical dual-mode USB in QEMU;
Windows installation, actual-machine firmware and a separate BIOS-only layout
without the helper remain untested. These boots did not independently compare
all files against the ISO; GUI write/verification completion was user-reported.

To repeat this field test, use the Windows x64 ISO, Standard Windows installation,
MBR, BIOS + UEFI, NTFS, Quick format and Standard post-write verification. After
the GUI finishes, close Rufus++ and boot the physical USB using the command above
with `--bios`; close QEMU before repeating without `--bios` for UEFI. The launcher
now validates MBR/NTFS geometry, bootstrap presence, the backup boot sector and
the optional helper, and keeps the physical backing read-only for both boots.
Four ordinary-file/command guards in `tests/windows_usb_boot_tests.py` exercise
those metadata and firmware paths without opening a device or starting QEMU.

Staging-only commands also accept `--stage-bios-iso` and `--stage-dual-iso` in
place of `--stage-iso`. These produce ordinary files without booting a VM or
writing physical media. The source tables and license/provenance accompany the
app in `third-party/windows-ntfs-boot`. Linux and native Windows runtime testing
remain pending.

## Automated native tests

```console
cmake --preset core-only -DRUFUSPP_STRICT_WARNINGS=ON
cmake --build --preset core-only --parallel
ctest --preset core-only -L writers --verbose
```

The native tests run without elevated privileges. They exercise the transfer
and flush implementation used by the Linux, macOS, and Windows backends, with
individual system-call results injected below that implementation. They cover
short reads and writes, interrupted calls, zero progress, errors after partial
I/O, offset and capacity boundaries, sector alignment, and file/device flush
ordering and failure. Real temporary-file handles additionally exercise the
host's OS read/write/flush APIs and permission failures.

Target revalidation tests cover replacement IDs, paths, capacity, sector size,
serial changes and disappearance, and devices becoming system disks, read-only,
partitions, or fixed non-USB/SD disks. The application uses the same validation
before and after unmounting. Finalization tests ensure that copy failures,
cancellation, and partition-refresh failures never publish `Complete`.

The Python runner tests check ownership validation, cleanup ordering, retention
of attached backing images on cleanup failure, and failure versus skip status.
Python 3 is optional for ordinary C++ tests and required for image-file and
virtual-device integration.

## Disposable virtual devices

This test is opt-in and creates only private 16 MiB images. The runner accepts
no physical-device argument. It uses `hdiutil` on macOS, loop devices on Linux,
and a fixed VHD through DiskPart/Storage PowerShell on Windows. It independently
checks that the allocated device maps to the exact backing image before opening
it, then gives the test helper an inherited handle. The helper accepts no raw
device pathname and also checks device type and geometry.

```console
cmake -S . -B build/virtual-disk -DCMAKE_BUILD_TYPE=Debug \
  -DRUFUSPP_BUILD_QT_APP=OFF -DRUFUSPP_BUILD_PLATFORM_BACKENDS=ON \
  -DRUFUSPP_STRICT_WARNINGS=ON -DRUFUSPP_RUN_VIRTUAL_DISK_TESTS=ON
cmake --build build/virtual-disk --parallel
ctest --test-dir build/virtual-disk -R '^native-virtual-disk$' --verbose
```

Linux requires loop-device privileges and `dosfstools`. Windows requires an
administrator session. macOS requires working DiskImages/DiskManagement services
and access to the newly allocated raw image device. Use a disposable test host
or the dedicated CI step for privileged execution.

For both quick and full FAT16 formats, the runner:

1. Stages a filesystem image with the application's portable formatter and
   initializes the private target with a nonzero sentinel pattern.
2. Writes it through the production native transport and portable copy engine,
   including real device flush calls and verification.
3. Closes the handle, detaches the image, and reattaches it read-only.
4. Independently reads all 16 MiB through a new handle, comparing the source
   bytes, preserved gap, and cleared trailing metadata as appropriate.
5. Runs the host's read-only FAT check (`fsck_msdos`/`fsck.fat`), or checks Windows
   filesystem recognition. Windows recognition is not a filesystem consistency
   check. Detaches only the device it created and removes the backing files.

If cleanup cannot detach the image, the runner fails and preserves the backing
directory for recovery. It never forces an unmount or deletes an attached
backing image. Missing privileges/services before writing produce exit 77,
reported by CTest as **Skipped**. A failure after a completed write remains a
failure. Dedicated CI uses `--require`, which turns missing prerequisites into
failures as well:

```console
python3 -B tests/virtual_disk_integration.py \
  --helper build/virtual-disk/tests/rufus_plus_plus_virtual_disk_helper --require
```

On Windows multi-configuration builds, use the helper under `tests/Debug/` and
its `.exe` suffix.

The manually dispatched **Native virtual-disk qualification** workflow builds
and runs these checks on Linux, macOS, and Windows hosted runners. It uses
`--require`; a runner without working virtual-disk support fails visibly. It
has not been dispatched from this local development session.

**Scope:** these are native transport integration tests. They exercise real OS
device handles, cache operations, detach/reattach, and filesystem recognition.
They do not exercise physical device discovery, the complete claim/unmount/lock
lifecycle, signed-helper authentication, USB controllers, power loss, or boot
firmware. Production eligibility rules for virtual/fixed disks are unchanged;
there is no application flag that permits test devices or bypasses safeguards.

## Official Ubuntu ISO (opt-in, large image)

The normal suites use small generated fixtures. `ubuntu_iso_integration.py`
additionally accepts an existing, checksum-verified **ARM64 Ubuntu installer
ISO** and always creates its own target. It accepts no device path. This test
is not run or downloaded automatically by CTest.

The local fixture is `ubuntu-24.04.5-live-server-arm64.iso`, downloaded from
[Canonical's release directory](https://cdimage.ubuntu.com/releases/24.04/release/).
Its authenticated SHA-256 is:

```text
f12484464c7d4d73e2167e0a7dbda1e669f8fae9bcf778dc0944937034b320cb
```

Preparation verified the manifest's detached signature using the Ubuntu CD
Image Signing Key (2012), fingerprint
`843938DF228D22F7B3742BC0D94AA3F0EFE21092`, pinned to
[Canonical's published fingerprint](https://ubuntu.com/tutorials/how-to-verify-ubuntu).
PGPy 0.6.0 verified the signature and rejected a deliberately modified manifest;
the downloaded ISO matched that manifest. This checks the pinned key's signature,
not a complete OpenPGP trust/revocation policy. Verification material and the
source record are retained locally in `build/ubuntu-iso/`; they are not repository
fixtures or runtime dependencies.

On the prepared local checkout, run the raw virtual test from an unrestricted
macOS Terminal (the agent's restricted command environment cannot attach disks):

```console
sudo sh tests/run_ubuntu_iso_test.sh
```

The wrapper pins the source path and checksum, requires the existing native
helper, and writes a report to `build/ubuntu-iso/raw-virtual-write-report.json`.
It neither downloads an ISO nor accepts a physical-drive argument. It verifies
the source checksum before target allocation, requires the analyzer to recognize
a bootable UEFI ARM64 ISO, and derives an MiB-aligned target size with at least
16 MiB of sentinel-filled extra space. All virtual images are bounded to
16 MiB..16 GiB. The inherited-handle helper independently checks the exact
requested capacity and 512-byte sectors; larger capacity does not bypass
ownership checks.

The raw run writes and verifies using the production native transport, flushes
file/device caches, closes/detaches, reattaches read-only, and compares every
target byte through a new raw handle. After detaching again, a separate Python
comparison checks the backing file against the complete ISO plus untouched gap,
computes the written ISO's SHA-256, and checks the ISO9660 descriptor. On macOS,
libarchive independently opens the written filesystem and checks that the EFI
loader, GRUB configuration, kernel and initrd are present. This is not a boot
test. The backing image is deleted only after successful detach; cleanup failure
retains it for recovery.

For the same ISO written to an ordinary file without root or disk attachment:

```console
python3 -B tests/ubuntu_iso_integration.py \
  --iso build/ubuntu-iso/ubuntu-24.04.5-live-server-arm64.iso \
  --sha256 f12484464c7d4d73e2167e0a7dbda1e669f8fae9bcf778dc0944937034b320cb \
  --helper build/native/tests/rufus_plus_plus_virtual_disk_helper \
  --file-helper build/native/tests/rufus_plus_plus_image_write_helper \
  --file-only --report build/ubuntu-iso/file-write-report.json
```

## Physical qualification procedure

### Prepared local USB test

The user explicitly authorized erasing the disposable `UNTITLED` volume for a
full image-write test. Read-only native discovery identified a Kingston
DataTraveler 3.0, 15,472,047,104 bytes with 512-byte sectors. Its exact serial,
stable identity, vendor/model, capacity, sector size and authorized mount are
pinned in the ignored local file `build/ubuntu-iso/physical-usb-target.json`.
The test is not bound to a transient disk number. Both the physical
write/readback run and subsequent unplug/reconnect read-only verification passed.

The preflight found that macOS discovery omitted this drive's USB serial when
the storage driver's Device Characteristics dictionary lacked it. Discovery
now reads the nearest actual USB device's serial as a fallback. It stops at
that device even if the serial is absent, so it never adopts a hub's serial.
Read-only rediscovery confirmed the USB's reported serial and stable identity.

The manual helper is built only with explicit
`RUFUSPP_BUILD_PHYSICAL_USB_TEST=ON` in macOS unsigned-root development mode.
It uses the **production backend's** discovery, eligibility checks, source and
target revalidation, exclusive claim, unmounting, raw opening, geometry checks,
native writes/flushes and verification. The signed-helper authentication route
and Qt UI remain separate qualification work. No physical write is registered
as a CTest test; only 12 guard/readback tests using private ordinary files run
automatically.

For this prepared local checkout, from an unrestricted Terminal:

```console
sudo sh tests/run_physical_usb_test.sh
```

This performs one normal write of the same authenticated Ubuntu ARM64 ISO,
replacing `UNTITLED` and its files. It cross-checks the snapshot against both
native discovery and `diskutil`, refuses system disks/partitions and changed
identities, and requires the source and report to be stored off the USB. It
does not run hardware fault-injection or destructive endurance loops.

After the production writer exits, a separate Python read-only handle checks
the opened raw device's type/geometry, disables host data caching, compares all
ISO bytes and SHA-256, checks the cleared final MiB, and compares three sampled
unwritten gap regions against their pre-write hashes. It safely ejects after
success. Keep the USB connected while the write/readback is running.

After a successful write/eject, physically unplug/reconnect that same USB and
run the prepared **read-only** reconnect check:

```console
sudo sh tests/run_physical_usb_test.sh --verify-only
```

Replug verification finds the USB by its pinned serial, requires the matching
successful write report and full identity/geometry, repeats image/tail/control
checks, and ejects. It never writes the USB or requires the old `UNTITLED` name.
Reports are retained in `build/ubuntu-iso/physical-usb-write-report.json` and
`physical-usb-replug-report.json`. Neither this image-write test nor byte
verification establishes bootability on a particular firmware.

### Wider hardware qualification

Physical writes require a drive explicitly designated by its owner as
disposable. Do not choose a target from its transient device number alone.
Use the application's normal guarded write path; do not use the transport test
helper with physical media. No physical-device write is part of automated CTest.

Prepare a record for each supported OS and drive/controller combination:

| Record | Required evidence |
| --- | --- |
| Host | OS version, application commit/build, privilege route |
| Target | Whole-device path, stable ID, model, serial, exact capacity, logical/physical sector size |
| Source | Image hash, expected layout, and a location on a different physical device |
| Control | A second non-target device with a recorded hash of its test data |
| Result | Write result, stages, error, bytes written/verified, independent readback and filesystem results |

Run and record each case separately:

- **Pre-write refusal:** system disk, read-only target, partition target, source
  stored on the target, insufficient capacity, missing privileges, claim/lock
  failure, and a busy volume that cannot be unmounted. Require no target writes.
- **Identity changes:** disconnect before writing, reconnect/replace while a
  selection is pending, and change eligible state between selection and opening.
  Require rejection of stale selections and re-selection by stable identity.
- **Normal writes:** raw images with MBR and GPT, quick formatting with stale
  tail metadata, and full-device writing. Include 512-byte and 4096-byte logical
  sectors where hardware is available, plus the supported USB/controller types.
- **Cancellation/faults:** cancel before the first write, during copying, and
  during verification. Disconnect only the disposable target during a write or
  verification. Require failure/cancellation, accurate possible-modification
  reporting, no false `Complete`, and release of locks/claims on every exit.
- **Media retention and isolation:** close, eject, physically disconnect/reconnect,
  and perform independent full byte comparison and partition/filesystem checks.
  Verify the control device's test data is unchanged. Boot representative media
  on the intended firmware separately from byte verification.

Never infer power-loss durability or wrong-device protection from a successful
immediate readback. Hardware fault qualification and independent review of
device discovery, opened-handle identity, source-on-target detection, privilege
boundaries, and lock/claim cleanup remain release requirements.

## QEMU boot preparation

The manual QEMU preparation is specific to this Apple Silicon host and the
Kingston USB already qualified below. It requires the local pinned identity and
successful physical reconnect report; it does not select a disk by number or
accept an arbitrary device argument. No capture or guest boot is part of CTest.

QEMU 11.1.2 and its ARM64 EDK II UEFI firmware are installed through Homebrew.
If needed on this host, install without `sudo` using
`sh tests/install_qemu.sh`. Homebrew retains its normal sandbox; installation
must run from the normal Terminal because its sandbox cannot start inside the
restricted agent command environment.

With the qualified USB connected, run from the repository Terminal **without
prefixing this command with sudo**:

```console
sh tests/prepare_qemu_boot.sh
```

The wrapper requests `sudo` only for the read-only physical capture. It checks
the pinned serial, stable identity and geometry through production discovery
and independently through `diskutil`, unmounts the USB, then opens the raw
device with `O_RDONLY`. It copies all **15,472,047,104 bytes** to a new ordinary
file, verifies the Ubuntu prefix against its authenticated source hash, checks
the cleared tail and preserved gap controls, and checks the full local copy's
SHA-256. It safely ejects the USB and leaves the capture at
`build/qemu-boot/usb-readback.img`, with owner-read-only permissions. Existing
captures are never overwritten. A later invocation checks the recorded full
capture hash and reuses it without requiring the physical USB.

QEMU runs as the normal user with macOS hardware acceleration, four virtual
CPUs, 4 GiB RAM, ARM64 UEFI, and the capture presented through an emulated USB
controller. Both the raw block node and underlying file node are read-only.
Each session gets a private writable copy of the UEFI variable store. There is
no other guest disk, networking or host folder sharing. This boots through the
USB's EFI loader rather than supplying a kernel directly or mounting the
original ISO as a virtual CD. Stop at the Ubuntu installer welcome screen and
close the QEMU window to finish. There is no installation target attached.

Session logs and the exact command are retained under `build/qemu-boot/run-*/`;
`last-run.json` identifies the most recent session. On exit the launcher checks
the capture's full hash again. A clean QEMU exit is recorded separately from
boot evidence: `bootResult` remains `not assessed` until the guest display or
serial output is inspected. Reaching the installer would establish bootability
under this virtual ARM64 firmware; booting on the intended physical machine
remains a separate test of that machine's firmware and hardware.

Nine file-only capture and command-safety cases run in
`qemu-boot-preparation-safety`. Real paused configuration probes have also
passed with both TCG emulation and HVF hardware acceleration, including UEFI
and a read-only virtual USB. They deliberately do not execute a guest boot.

## Live Linux persistence test

Media retaining its image after unplug/replug is distinct from a live operating
system saving session changes. The opt-in `persistence_qemu_integration.py`
tests the latter using private ordinary files and headless ARM64 QEMU. It runs
without `sudo` and never opens a physical disk. No guest execution or large ISO
download is part of CTest; sixteen file-only layout/cleanup/checker guards run in
`persistence-qemu-runner-safety`.

With QEMU installed, the authenticated local Ubuntu ISO and native test helper
can be used from the repository Terminal:

```console
python3 -B tests/persistence_qemu_integration.py \
  --iso build/ubuntu-iso/ubuntu-24.04.5-live-server-arm64.iso \
  --sha256 f12484464c7d4d73e2167e0a7dbda1e669f8fae9bcf778dc0944937034b320cb \
  --helper build/native/tests/rufus_plus_plus_image_write_helper
```

The helper uses production `ImageAnalyzer`, `IsoDeploymentPlanner` and
`IsoImageStager` to create an 8 GiB MBR image with FAT32 live files, patched boot
configuration and a 2 GiB ext2 `casper-rw` partition. Production `RawImageWriter`
and native file I/O then write and verify the initialized ranges to a new local
disk file prefilled with nonzero bytes. A separate comparison checks every
target byte: initialized ranges must match the staging image, and all skipped
regions must retain the nonzero fill. Native write/read counters must equal
the sum of the ranges, not the logical disk size. Earlier dense fixtures retain
the whole-file equality check for compatibility with saved reports.
The actual Ubuntu fixture's initrd was inspected: its Casper scripts accept
`casper-rw` as a fallback to `writable`; this compatibility is specific to the
tested release.

For Casper, the production stager now seeds
`upper/usr/lib/systemd/system-shutdown/90-rufuspp-persistence` in that filesystem.
Casper otherwise leaves its writable backing mount outside the live root's
visible mount tree. At [systemd's late shutdown stage](https://www.freedesktop.org/software/systemd/man/latest/systemd-poweroff.service.html), the helper locates only
the uniquely matching generated filesystem UUID, mounts a temporary alias,
syncs and remounts the filesystem read-only. It performs no repair. It runs
only for a persistent overlay live root; installed systems are excluded.
The formatter verifies the complete initial helper tree and its executable
mode. It also initializes the mount counters and write timestamp at their
correct superblock offsets.

The first VM boots normally through UEFI/USB/GRUB, checks the `persistent`
kernel option and expected filesystem UUID, creates a dedicated test user,
writes a file in that user's home and an application configuration under
`/etc`, then syncs and powers off. A second QEMU process uses the same disk
file with a fresh UEFI variable store. It must have a different kernel boot ID,
retain the user/file/configuration, match the file's SHA-256, and have lost a
control file under `/run`. Both guests must shut down cleanly. The virtual USB
uses fixed inserted media (`removable=off`, QEMU's default), so Ubuntu cannot
eject its backing file as a swappable SCSI tray during shutdown. The USB device
itself remains external virtual storage. Before shutdown,
the harness creates [Ubuntu's supported `/run/casper-no-prompt` flag](https://bugs.launchpad.net/bugs/1736797) to suppress
the installer-media confirmation; filesystem sync and normal shutdown still
run. This runtime flag is not part of the staged image. The FAT boot partition
must remain byte-identical. After each poweroff the host independently requires
the persistence superblock's clean flag and agreement between its free totals
and every group descriptor. These checks run before the next boot, so reboot
cannot conceal an incomplete shutdown. No snapshots, other guest storage,
networking or host shares are used for these two boots.

An independent third VM boots the original ISO read-only, with the tested disk
attached as an additional read-only inspection device. It requires the
persistence partition to be unmounted, runs Ubuntu's `e2fsck -f -n`, and checks
that the full inspection image hash is unchanged. It requires both a zero exit
status and a complete checker transcript without inconsistencies or repair
prompts: this e2fsck version can report incorrect free-space counters while
returning zero. Regression coverage explicitly rejects that result.
Per-run logs, exact commands,
guest assertions and the result report remain under
`build/persistence-test/run-*/`.

The test establishes the selected ISO's live persistence through clean virtual
shutdown and cold boot. It does not establish sudden-power-loss recovery,
desktop application compatibility, or persistence on a physical USB.

## Results from this development session

On 2026-10-20, the macOS native backend and privileged-helper targets built with
strict warnings. The native configuration passed all eleven non-virtual test
suites, including six real image-file writes with **328 MiB** independently
compared after closing/reopening. All expected/actual SHA-256 values matched;
both populated FAT images and the quick FAT32 image passed `fsck_msdos -n`.
Known payload contents and primary/backup GPT checksums also passed. The writer
tests and new image-file integration test passed AddressSanitizer and
UndefinedBehaviorSanitizer checks. The unsigned-root development configuration
also passed all eleven non-virtual suites after the USB serial discovery fix,
QEMU preparation and persistence runner additions.
Existing coverage includes
64 portable writer cases, 60 native I/O/safety cases, and 27 Python ownership,
cleanup, and lifecycle tests, plus 12 file-only physical-runner guard/readback
tests, nine file-only QEMU preparation tests and sixteen persistence fixture
guards alongside the existing
core/platform suites.
Virtual-disk attachment remains unavailable inside the restricted agent command
environment (`Device not configured` / DiskManagement unavailable), where CTest
reports **Skipped**. Running the same integration runner in the user's Terminal
with `sudo` exposed incorrect plist field names in its macOS guard. The runner
now checks `WholeDisk` and `TotalSize`, and additionally requires the exact owned
`DeviceNode` and 512-byte `DeviceBlockSize`. Regression tests cover valid metadata
and each missing or mismatched field.

After that correction, the user's Terminal run on the same macOS host passed
**both quick and full FAT16 raw virtual-disk cases**. The terminal output was
read directly through the app's terminal snapshot tool. Quick mode wrote
1,187,328 image bytes, cleared the trailing metadata area, and preserved the
sentinel gap; full mode wrote all 16,777,216 image bytes. Both cases passed the
real native write, file/device flush, and writer verification; closed/detached
and reattached read-only; then independently compared all 16 MiB and passed
`fsck_msdos -n`. Thus 32 MiB of raw virtual-device contents were independently
verified across the two cases. A separate `hdiutil info -plist` check confirmed
no test images remained attached, and both backing directories were removed.

This establishes successful macOS raw virtual transport integration. It does
not exercise the application's full discovery/claim/unmount/authentication path
or physical USB hardware. The separately authorized physical test described
above subsequently passed its initial write/readback run, as recorded below.

The official Ubuntu ARM64 ISO subsequently passed analysis and an ordinary-file
write/readback integration test: all **3,825,512,448 ISO bytes** matched the
authenticated source hash, and the entire **3,843,031,040-byte target** matched
the ISO plus preserved trailing sentinel data. ISO9660 recognition and expected
boot-file presence passed. This larger source also exercises offsets past the
signed 32-bit boundary.

The corresponding **Ubuntu raw virtual-device test also passed**, run with
`sudo sh tests/run_ubuntu_iso_test.sh` in the user's Terminal. Native writes,
file/device flushes and writer verification succeeded for all 3,825,512,448 ISO
bytes. After detach and read-only reattachment, the independent raw-handle check
matched every one of the 3,843,031,040 target bytes. A separate Python backing-file
comparison produced the authenticated source SHA-256 for the written ISO and
matched the complete target, including the preserved trailing gap. ISO9660
recognition and the expected EFI loader, GRUB configuration, kernel and initrd
checks passed. The structured result is retained locally in
`build/ubuntu-iso/raw-virtual-write-report.json`. The report and terminal output
were read directly; a further `hdiutil info -plist` check confirmed no test
attachments remained, and the temporary backing directory was absent.

The **physical Kingston USB write/readback also passed** through the production
macOS unsigned-root backend. The Terminal log showed exclusive claiming,
unmounting, raw opening, all 3,825,512,448 image bytes written, final-MiB metadata
clearing, native flushing, complete writer verification and `Complete`. The
structured writer result reported 3,826,561,024 verified bytes (ISO plus tail).
A separate Python raw handle with host data caching disabled independently
compared every ISO byte, matched the authenticated image SHA-256, verified the
zeroed final MiB and confirmed all three sampled unwritten gap controls were
unchanged. The test safely ejected the USB. Both Terminal output and
`build/ubuntu-iso/physical-usb-write-report.json` were read directly and confirmed
`success: true` and `ejected: true`.

macOS displayed an unreadable-disk dialog after the media layout changed; the
user was directed to Ignore, preserve the Ubuntu image, and wait for verification
and safe ejection. The completed byte comparison establishes that the USB image
matches the verified source despite the mount dialog.

After the user physically unplugged/reconnected the USB, the prepared
`--verify-only` run also **passed**. Native discovery matched the same pinned
serial, stable identity and full geometry. With no target writes attempted, the
new raw handle again compared all 3,825,512,448 ISO bytes and produced the same
authenticated SHA-256. The zeroed final MiB and all three pre-write gap controls
also matched. The replug result exactly matched the earlier readback record,
and the USB was safely ejected again. Both Terminal output and
`build/ubuntu-iso/physical-usb-replug-report.json` confirmed success, read-only
operation and ejection. This establishes byte retention through a normal safe-eject
and physical reconnect cycle on this particular USB/host combination; it is not
a sudden-power-loss test.

The **Ubuntu QEMU boot test subsequently passed through the installer welcome
screen**. The read-only physical capture copied all 15,472,047,104 USB bytes,
matched the authenticated Ubuntu prefix, verified the tail/gap controls and
safely ejected the physical USB. Its whole-disk SHA-256 is
`799a890bfe843892f52316ce5c436db12b24aa8b8110b977ea7d2df6cbefcf13`.
QEMU 11.1.2 then booted this captured USB through ARM64 EDK II UEFI and GRUB,
with HVF hardware acceleration. Serial output confirmed the Linux kernel,
write-protected USB discovery and live filesystem startup. Direct inspection
of a QMP screenshot confirmed the Ubuntu Welcome language selection screen.
The running VM's block metadata independently reported the captured media as
read-only, and a fresh complete capture hash still matched after reaching the
installer. No operating system installation was performed.

The capture report, serial output, screenshot and separate manual
`boot-evidence.json` assessment are retained locally under `build/qemu-boot/`.
The launcher records its exit status and final media hash separately when the
window closes. This establishes boot through the installer under this virtual
ARM64 firmware, using bytes captured from the physically written USB. Booting
the USB on the intended physical machine remains untested.

The production native discovery/claim/unmount/write/flush path and unsigned-root
privilege route have now been exercised on this USB; Qt interaction and signed
helper authentication remain separate qualification work. The downloaded source
ISO is retained for subsequent testing.

The **initial live persistence data check passed, but overall persistence
qualification initially failed**. Production staging and writing created the 8 GiB image and its 2 GiB
ext2 persistence partition. Two independent QEMU boots retained a newly created
user, the exact home-file SHA-256 and the `/etc` configuration. The kernel boot
IDs differed, the `/run` control file disappeared, and the FAT boot partition
remained unchanged. Both guests requested normal poweroff and their QEMU
processes exited.

However, independent read-only `e2fsck -f -n` reported inconsistent free block
and inode counts after those poweroffs. For the fixed-media run, its block
counts were 507587 versus 506964 counted; inode counts were 130134 versus 130120
counted. The filesystem remained marked unclean. e2fsck 1.47.0 returned zero
despite these messages; the runner now checks its transcript and correctly
reports this case as a failure. The complete result is in
`build/persistence-test/run-zynpop9r/report.json`, with the checker transcript in
`fsck/fsck.txt`. The failure logs and pristine source are retained locally; the
failed test image was subsequently used for shutdown prototyping.

A separate read-only check of the **unused staged filesystem passed all five
e2fsck passes without inconsistencies**, and its complete image hash was
unchanged by inspection. That result is in
`build/persistence-test/pristine-check/report.json`. This narrows the observed
counter inconsistency to live use/shutdown; it does not identify the responsible
component by itself. An earlier swappable-tray configuration also caused
shutdown I/O errors. Fixed inserted media removed those I/O errors, but the
allocation-counter inconsistency persisted. No repair was performed, and no
physical USB writes were made during persistence testing.

The **subsequent production fix passed the complete Ubuntu 24.04.5 ARM64
persistence test**. Casper staging now includes the UUID-pinned systemd shutdown
helper described above. The formatter initializes its additional directories,
inodes, allocation maps and group directory counts consistently, and verifies
their complete contents. It also corrects the superblock mount-counter and
write-timestamp field offsets. Regression tests reject damaged helper content
and incorrect group directory counts; sixteen file-only runner cases include
unclean post-poweroff state and stale counter totals.

A newly staged and natively written 8 GiB image retained the exact user-file
SHA-256, user account and `/etc` configuration through two independent VM
processes with fresh firmware stores and different kernel boot IDs. The
volatile `/run` control disappeared and the FAT partition remained unchanged.
Immediately after each poweroff, the host independently confirmed the clean
filesystem flag and exact agreement between superblock/group free totals.
A third VM inspected the unmounted persistence partition with both block/file
nodes read-only: all five `e2fsck -f -n` passes completed with no inconsistencies
or repair prompts, and the complete inspection image hash was unchanged.
All test VMs exited. The complete passing report, command lines, transcripts
and per-poweroff headers are retained in
`build/persistence-test/run-u3h_fhq3/`.

Both native macOS build configurations passed all eleven non-virtual suites
after the fix, and the backend staging tests passed AddressSanitizer and
UndefinedBehaviorSanitizer. This qualifies normal persistence shutdown/cold
boot for this ISO under the tested virtual ARM64 firmware. Physical USB
persistence, other distributions/init systems, and sudden power loss remain
separate tests. The physical USB was untouched and all changes remain local.

Linux and Windows native/virtual paths need their respective CI or test hosts;
local macOS results do not qualify them.

The first manual GUI persistence attempt exposed a separate commit-planning
bug: quick MBR/FAT32 mode requested trailing metadata clearing even though a
persistence stage already spans the complete target. The raw planner rejected
the overlapping request before the physical write. ISO staging and the GUI
now share `IsoDeploymentPlan::requiresTrailingMetadataClear()`: compact quick
MBR stages retain tail clearing, while persistence, GPT and full-format stages
write their own trailing region. The raw overlap guard remains enforced.
Regression tests build actual staged images and their native commit plans for
Casper, Debian Live, compact MBR, GPT and full-format media. The rebuilt
unsigned-root GUI and all sixteen local CTest suites passed. This fixes the
reported planning rejection; the GUI physical persistence write and its QEMU
boot still require the manual retry.

## Optimized quick-format commits

Quick FAT32 ISO stages now provide initialized ranges for GPT and persistence
layouts. The writer does not infer ranges by looking for zeroes: complete ext2
metadata prefixes, including every inode table's unused zero-filled entries,
must overwrite pre-existing data. Ranges are bounded, sector-aligned, ordered
and disjoint. Compressed/container inputs cannot use this path. The final
metadata region is copied from the staged image, retaining new GPT backups or
removing stale signatures without overwriting persistence metadata with zeroes.
Full format and normal DD/raw copying retain dense writes. Standard verification
compares every transferred byte; fast sampling includes every range boundary.
The progress total is the actual transfer sum. The signed macOS helper carries
and validates the same map through typed protocol version 7.

The optimized Ubuntu 24.04.5 ARM64 integration passed in
`build/persistence-test/run-2_is8y7r/`. Its 8 GiB destination was first filled
with nonzero bytes. Actual native I/O wrote and verified **3,865,463,808 bytes**
over 18 ranges. An independent comparison of all 8 GiB confirmed the correct
initialized contents and preservation of **4,724,470,784 skipped bytes**.
Two fresh UEFI/QEMU boots retained the test user, exact file SHA-256 and
configuration; the volatile control disappeared. Both poweroffs left clean
filesystem flags and consistent allocation totals. A third VM ran all five
`e2fsck -f -n` passes against the unmounted, read-only disk without errors, and
the disk hash remained unchanged by inspection. All three guests powered off
and their QEMU processes exited.

An additional ordinary staging file used the pinned Kingston geometry
(15,472,047,104 bytes, 512-byte sectors). Its valid plan transfers
**3,868,124,160 bytes**, about 3.60 GiB instead of 14.41 GiB, while the ext2
filesystem still has 2 GiB of capacity. The stage/plan record is retained under
`build/persistence-test/kingston-geometry-optimized/`; this was not a physical
USB write.

The unsigned-root GUI build passed all 18 CTest suites, including 61 portable
raw-writer cases, 25 file-writer cases, 60 native I/O/safety cases, typed XPC
range guards, 20 persistence fixture guards, and seven independent ordinary-file
write integrations. The default helper build passed all 12 non-virtual suites
and compiled the updated client/daemon. AddressSanitizer and
UndefinedBehaviorSanitizer passed the raw-writer and backend staging suites.
The elapsed-timer GUI test now waits for its actual asynchronous update instead
of assuming a fixed timer delivery deadline under load. No physical USB was
modified during this optimization qualification. Optimized physical writes,
actual hardware boots, and Linux/Windows native hosts remain separate checks.

### Capturing a GUI-created physical persistence USB

After the GUI reports completion, close Rufus++ and run from the repository
Terminal:

```console
sudo /usr/bin/python3 -B tests/physical_persistence_qemu.py --capture
```

This manual path accepts no device argument. It revalidates the previously
authorized Kingston serial, stable identity, model and geometry against the
successful physical reconnect record, unmounts it, and opens its raw handle
read-only. It copies all bytes to a new local file, verifies the complete local
SHA-256, checks the FAT32/Linux layout and clean ext2 allocation counters, then
ejects the USB. Existing captures are never overwritten. The original captured
file is made read-only and retained with its provenance report under
`build/physical-persistence-test/`.

Then run without sudo:

```console
/usr/bin/python3 -B tests/physical_persistence_qemu.py --test
```

The test verifies the capture hash, makes a separate writable ordinary-file
copy, and runs the two independent persistence boots and read-only filesystem
check described above. The physical USB is not attached to QEMU. A successful
run establishes boot and persistence behavior for bytes read from the GUI-written
USB; persistence writes during this test go to the local copy. It does not
establish physical USB write endurance or a real machine boot. Geometry checks
allow less than 2 MiB of partition padding around the 2 GiB filesystem, as used
by the actual Kingston capacity. The persistence runner safety suite now has
24 file-only cases, including physical geometry, sudo separation and changed
capture rejection; it never opens a device or starts a VM.

### GUI-written physical USB capture qualification — 2026-10-20

The user completed the optimized ISO-mode GUI write to the authorized Kingston
USB with 2048 MiB persistence. The read-only capture copied all 15,472,047,104
bytes, checked its clean initial allocation counters and safely ejected it.
The complete capture SHA-256 was
`d9c275ac26bbd0aaf2b72f462bba8215d29b70ff42f2027ca1e07a3462d6df9c`.

`physical_persistence_qemu.py --test` **passed**. A separate writable copy
booted through ARM64 UEFI/USB/GRUB twice, using independent QEMU processes and
fresh firmware variables. The expected `casper-rw` UUID was mounted read-write.
The test user, home file and `/etc` setting survived a full poweroff, the file's
SHA-256 matched on the second boot, and the `/run` control disappeared. Both
poweroffs left clean ext2 flags and matching free-space/inode counters; the
FAT boot partition remained byte-identical.

A third VM booted the authenticated original Ubuntu ISO and inspected the test
disk read-only and unmounted. All five `e2fsck -f -n` passes completed without
errors. Full hashes confirmed that inspection changed no test-disk bytes and
the pristine physical capture remained unchanged. All three guests powered off
normally and their QEMU processes exited. Reports, QMP block maps, serial logs
and assertions are retained in
`build/physical-persistence-test/run-8c3aqx0b/`.

This establishes boot and persistence behavior for bytes captured from the
GUI-written physical USB. The persistence operations occurred on its local
copy; this does not establish persistence writes on physical flash or booting
a real machine. No physical USB write was performed by the capture/test runner.

### Interactive boot directly from the physical USB

Reconnect the qualified Kingston, close Rufus++, and run in the normal repository
Terminal:

```console
sudo /usr/bin/python3 -B tests/boot_physical_usb_qemu.py
```

This launcher accepts no device or write arguments. It discovers and independently
checks the pinned serial and geometry, unmounts the USB, and opens it `O_RDONLY`.
It checks the captured persistence UUID. A root supervisor launches QEMU with
the verified raw path, a read-only `host_device` backing node and a local writable
qcow2 overlay. QEMU opens the raw device, drops root using `-run-with user=...`,
and remains paused until the supervisor verifies its real/effective user IDs
and revalidates the physical identity. Boot data comes directly from the connected USB; no captured disk
image is used. Guest changes go to `session-changes.qcow2`, not to the stick.
No network or shared folders are attached. The USB must stay connected while
QEMU runs. The Cocoa window shows the ARM64 Ubuntu Server live installer.

The launcher records QMP metadata, the command, serial output and exit status in
`build/direct-usb-demo/run-*/`. After a normal QEMU exit it checks the physical
MBR and persistence superblock again. Closing the window ends the demo; changes
are retained locally but the next invocation starts a fresh overlay.

Thirteen file-only capture/configuration guards passed, including read-only
backing construction, inherited descriptor validation and refusal of writable
physical handles. A paused QEMU configuration test passed using an inherited
read-only ordinary-file descriptor and writable qcow2 overlay; its source hash
remained unchanged. That paused check did not boot a guest or access the USB.

### Testing persistence writes on physical flash

The separate manual runner `test_physical_usb_persistence.py` tests actual
persistence writes on the previously authorized Kingston. Close QEMU and
Rufus++, keep that USB connected, and run from the repository Terminal:

```console
sudo /usr/bin/python3 -B tests/test_physical_usb_persistence.py --write-to-physical-usb
```

This command grants the guest write access to the physical USB. It accepts no
alternative device path. It verifies the serial, stable identity, model,
capacity, sector size, partition layout and persistence UUID against the
previous physical qualification. It refuses active QEMU/Rufus++ sessions and
mounted USB volumes after unmounting. It opens and independently validates
read-only and read/write raw handles. The root supervisor bootstraps QEMU's
raw-device open, then QEMU's built-in privilege drop changes it to the normal
user. Guest CPUs remain paused until both user IDs and the physical identity
are independently verified. A fresh, complete, read-only local backup is verified before any guest
starts; the backup is retained under `build/physical-usb-persistence/run-*/` and
is never restored automatically.

Two fresh QEMU processes boot directly from the verified physical raw path,
with no qcow2 overlay or captured disk attached. The test verifies
the live boot partition is mounted read-only in Linux, creates the test user,
home file and `/etc` setting, and checks their survival after normal poweroff
and a new boot. QEMU uses fixed inserted USB media and a fresh firmware variable
store each time. Native `fsync` and `DKIOCSYNCHRONIZECACHE` follow each poweroff;
the physical superblock clean flag and allocation counters must agree before
continuing. Assertion failures with an available guest shell still attempt a
normal Linux shutdown before emergency process cleanup.

After the second poweroff the write handle is closed. The supervisor reads the
current persistence partition into a new local file using its read-only handle.
It independently rereads and hashes the whole physical persistence partition,
requires an exact match to the local file, and compares every byte before the
persistence partition with the pre-test backup. The physical device is then
closed and safely ejected, and the supervisor drops root permanently.
A third VM boots the authenticated original Ubuntu ISO with only the captured
partition attached read-only and unmounted. `e2fsck -f -n` must pass all five
checks. Offline `debugfs` reads must independently match the saved home file's
SHA-256 and the exact `/etc` setting. The partition capture's full hash must
remain unchanged during inspection. A successful run leaves the test user/file/setting
on its persistence filesystem. It does not format or reinstall Ubuntu, and no
host shares or guest network are attached. This tests physical persistence
writes through QEMU; booting on physical machine firmware is a separate test.

The persistence runner safety suite has 56 file-only cases, including explicit
physical-write intent, native-handle access modes, ordinary-file rejection,
backup checks, native flush failures and normal failure shutdown. CTest never
invokes the physical runner or starts its guests.

On 2026-10-21 the new inherited-descriptor route passed an additional ordinary-file
qualification in `build/physical-usb-persistence/ordinary-fd-boot-sd_y24s4/`.
The test substituted the ordinary-file protocol for the physical-device protocol
and used a separate verified copy of the USB capture. QEMU inherited the actual
read/write descriptor with a raw disk node and no overlay. Both independent
Ubuntu boots retained the user, exact file and setting, and both shutdowns left
clean allocation counters. A third VM inherited only a read-only descriptor;
all five `e2fsck -f -n` passes succeeded. The complete persistence region stayed
unchanged during inspection and the entire boot area matched its baseline.
All three guests powered off and QEMU exited. This qualified the runner with
ordinary files; physical flash results must come from a subsequent manual run
of the explicit physical-write command above.

The first physical attempt, `run-ibaa48hf`, completed and verified its full
pre-test backup, then failed to initialize QEMU's raw-disk fdset handle with
`Failed to dup() given file descriptor`. The guest serial log was empty:
Ubuntu did not boot and the persistence exercise did not start. The captured
backup and failed report are retained.

An intermediate version used Darwin's `/dev/fd/<number>` path for inherited
handles instead of QEMU's fdset flag-duplication path. That version passed
paused QEMU checks with a character device (`/dev/null`) and an ordinary file;
an independently compared one-sector write reached that ordinary file directly
without an overlay. Evidence is in
`build/physical-usb-persistence/darwin-fd-check-oiwmx_jg/`.
The next physical attempt, `run-9s3aog88`, was rejected by macOS while reopening
the inherited raw handle after dropping root. Its paused check failed before
backup creation or guest execution. Ordinary character devices did not expose
this raw-disk permission constraint.

The current launchers let QEMU open only the verified raw path while privileged,
using [QEMU's built-in `-run-with user=...` privilege drop](https://www.qemu.org/docs/master/system/qemu-manpage.html).
Every physical guest starts with `-S`; the supervisor checks the paused state,
both process user IDs and the pinned physical identity before issuing `cont`.
The 56 persistence guards and 17 capture/boot guards pass, including refusal of
root guest execution and mismatched bootstrap device identities. The initial
paused transport check runs before backup creation.

The next physical run, `run-er7thfwn`, passed both direct USB persistence boots,
both normal shutdowns, native cache flushes, matching file/setting assertions,
distinct boot IDs and clean physical allocation counters. Its separate direct
read-only VirtIO inspection path produced guest I/O errors and timed out before
the checker ran. This result does not establish filesystem-check success.
The write/reboot evidence and verified pre-write backup remain retained.

The current runner therefore uses the verified physical partition readback
described above for the final independent filesystem and file-content checks.
To finish only these read-only checks after an already passed physical
write/reboot phase, use:

```console
sudo /usr/bin/python3 -B tests/test_physical_usb_persistence.py --verify-physical-usb
```

This mode requires the matching recorded physical identity, both successful
boot assertions and two clean poweroffs. It never opens a physical read/write
handle or starts a physical write guest. It retains a separate verification
report, captures and independently verifies the current on-device partition,
checks the immutable boot area, safely ejects the USB and runs the isolated
read-only checker after dropping root. It does not repeat the write phase.

The finishing run `verify-tnz17d4_` captured the current physical persistence
partition and matched it against a second independent physical read. Its strict
boot-area comparison then failed, so its overall report remains failed.
The separate file-only inspection `readback-check-pta2yb12` checked that same
captured partition: all five `e2fsck -f -n` passes succeeded, and offline reads
matched both the saved file SHA-256 and the exact setting. The capture remained
unchanged and the checker guest powered off normally. This establishes the
saved data and filesystem result, but does not establish boot-area integrity.

For a read-only diagnosis of that remaining difference, close QEMU/Rufus++ and
run:

```console
sudo /usr/bin/python3 -B tests/compare_physical_usb_boot_area.py
```

This diagnostic accepts no alternate device or baseline. It verifies the
recorded full pre-write backup, rediscovers the pinned Kingston, opens it only
read-only, compares every byte before the persistence partition, and ejects the
USB. Its separate `boot-area-diff-*/report.json` records both hashes, total
changed bytes/sectors, and up to 256 complete before/after sector samples.
It never repairs or rewrites the USB, and a difference is not counted as a
successful integrity check. File-only tests cover chunk boundaries, the final
byte, capped samples, baseline bounds, read-only access and evidence retention
when the strict finishing check fails.

The completed diagnostic `boot-area-diff-ml31jsix` found 11,360 changed bytes
in 52 sectors, with every changed sector retained. The MBR, partition boot
sectors and Ubuntu file data were unchanged. Differences were confined to
Spotlight/event metadata, the volume-label timestamp, and the corresponding
FAT allocation/FSInfo accounting. Those locations are consistent with macOS
metadata activity; the diagnostic does not establish which process wrote them.
The strict byte-for-byte boot-area check remains failed and is not relaxed.

An ordinary local FAT32 snapshot was reconstructed from the verified pre-test
backup plus every changed sector. Its complete boot-area SHA-256 matches the
current physical readback hash. Independent before/after file manifests match
all 568 Ubuntu files (3,814,891,704 content bytes) and 230 directories, excluding
only the two host-metadata roots `.Spotlight-V100` and `.fseventsd`. The exact
sector attribution, manifests and checker reports are retained in that
diagnostic directory. The USB was safely ejected after the physical comparison.

The additional read-only FAT check exposed a pre-existing formatter defect:
top-level directories encoded their `..` parent as root cluster 2 instead of
zero. The same bytes are present in the pre-write backup. Microsoft's
[FAT32 specification, page 25](https://cscie92.dce.harvard.edu/spring2024/Microsoft%20Extensible%20Firmware%20Initiative%20FAT32%20File%20System%20Specification%2C%20Version%201.03%2C%2020001206.pdf)
requires zero for a root parent. The ISO formatter now follows that rule while
preserving actual parent clusters for nested directories. Regression checks
inspect both MBR and GPT staged directory entries. The rebuilt app and all 18
CTest suites passed. A newly staged real Ubuntu ISO with 2 GiB persistence also
passed independent `fsck_msdos -f -n` without repair requests (exit 0), and its
capture hash stayed unchanged. Evidence is in `fat-parent-fix-1x1e0p_l`.

The physical MBR persistence file/setting and ext2 checks passed. That MBR
media predated the FAT32 parent-pointer fix. The later physical GPT qualification
below recreated the USB with the corrected formatter. No physical writes were
performed by the earlier finishing verification, diagnostic or local formatter checks.

### Interactive persistence on the physical USB

For a manual session whose changes stay on the Kingston itself, reconnect it,
close other QEMU/Rufus++ sessions, and run:

```console
sudo /usr/bin/python3 -B tests/boot_physical_usb_writable_qemu.py --write-to-physical-usb --gpt
```

`--gpt` selects the completed GPT qualification and checks both GPT copies and
its persistence UUID. Omit it only for the originally qualified MBR media;
that older identity will be refused on different media, including later BIOS
test media. This launcher does not yet provide an interactive BIOS session.

This separate launcher uses the same pinned identity, read/write handle checks,
paused startup and verified QEMU privilege drop as the physical test. It does
not format the USB or send automated commands to Ubuntu. The sole guest disk
is the physical USB, without an overlay, network or shared folders. It records
its session separately under `build/interactive-usb-persistence/run-*/`.
The original `boot_physical_usb_qemu.py` remains the read-only physical demo
whose changes are saved to a local overlay.

Use the live installer's shell for manual file changes. Shut down from inside
Ubuntu before closing QEMU, and acknowledge any remove-media prompt with Enter
while keeping the USB connected. Terminal Ctrl-C requests ACPI shutdown rather
than terminating a running writable guest. After QEMU exits, the launcher
flushes the physical device cache, checks the persistence clean flag/counters
and unchanged partition metadata, and safely ejects the stick. It does not automatically repair
a filesystem after an interrupted shutdown. The file-only guards include raw
writable configuration, explicit intent, privilege refusal and normal shutdown
on Terminal interruption.

### Physical GPT + UEFI qualification

The manual `test_physical_gpt_usb.py` runner prepares a GPT/FAT32 Ubuntu stage
with 2 GiB Casper persistence for the pinned Kingston's exact capacity. Staging
is a production image-preparation step; no image copy is booted or used as a
virtual test destination. `--prepare` runs without sudo and never opens a
physical disk. It validates primary/backup GPT headers and partition-array
CRCs independently, checks partition/filesystem bounds, records the complete
source hash and initialized write ranges, and checks native analyzer recognition.

After explicit erase authorization, run from the chat Terminal:

```console
sudo /usr/bin/python3 -B tests/test_physical_gpt_usb.py --erase-authorized-kingston --no-backup
```

`--no-backup` was explicitly requested for this test. Without that option the
runner makes a verified full read-only backup before overwriting. No alternate
physical device is accepted. The runner rediscovers the pinned USB and requires
the previously qualified MBR persistence UUID before replacing it. The native
physical helper uses the production guarded backend and Standard verification,
with the staged initialized ranges and no separate GPT tail clearing. A second
native read compares every initialized byte against the prepared source.

Two fresh QEMU guests then boot the physical raw USB without an overlay or
captured-disk attachment. They exercise the shared user/file/setting/reboot
assertions, with native cache flushes and clean-counter/GPT checks after each
normal poweroff. A third guest boots the authenticated inspection ISO and
attaches the physical USB read-only through USB storage, requires the persistence
partition to be unmounted and write-protected, runs `e2fsck -f -n`, and checks
the saved file/setting using read-only `debugfs`. No partition capture is booted
for inspection. Before/after physical hashes check inspection immutability,
the boot area and the trailing backup-GPT region. The stick is ejected on success.

Evidence is stored under `build/physical-gpt-usb/run-*/`. The initial prepared
run `run-uweklaae` contains the official Ubuntu ISO's 568 extracted files,
18 initialized ranges totalling 3,868,124,160 bytes, and both GPT copies for the
15,472,047,104-byte Kingston. Twenty-three corruption/refusal/reporting guard cases
pass, including both header/array CRCs, primary/backup disagreement, bounds,
explicit erase/no-backup intent, and refusal to overwrite historical reports.
The reporting regression injects a final-checker failure after successful boots
and requires the overall verdict to remain failed. No guard test launches a
guest or opens a physical device. The native GPT source preflight and five
relevant CTest suites passed.

The physical run completed successfully on 2026-10-21. Its `report.json` records:

- Native production write and verification of all 3,868,124,160 initialized
  bytes, followed by an independent physical readback matching every range.
- Valid matching primary/backup GPT copies and filesystem identities.
- Two fresh QEMU boots directly from the physical USB, with distinct boot IDs,
  normal shutdowns and clean filesystem counters. The user file and `/etc`
  setting survived; the volatile `/run` control did not survive the second boot.
- A direct physical, unmounted, read-only `e2fsck -f -n` completing all five
  passes cleanly, plus independent physical `debugfs` reads matching the file
  SHA-256 and setting. Hashes confirm the inspector changed no persistence bytes.
- Exact unchanged hashes for the entire 13,323,206,656-byte boot area and the
  16,896-byte backup-GPT region after the persistence exercise.
- Safe USB ejection. The run skipped the backup as explicitly requested.

The evidence is `build/physical-gpt-usb/run-uweklaae/report.json`, with native
results in `native-write.json`, guest serial/QMP logs under each boot session,
and `physical-fsck/fsck.txt` plus `physical-fsck/readback-assertions.txt`.
This qualifies the macOS production writer's GPT + UEFI + FAT32 + 2 GiB Casper
persistence route on this Kingston using QEMU's ARM64 UEFI firmware. Physical
computer firmware and native Linux/Windows writer routes remain untested here.

### Physical MBR + legacy BIOS qualification

The manual `test_physical_bios_usb.py` runner uses the same pinned Kingston and
guarded production writer. It accepts no arbitrary device or source argument.
The AMD64 Ubuntu Server ISO comes from
[Ubuntu's release directory](https://releases.ubuntu.com/24.04/).
Its signed checksum manifest was verified with the pinned Ubuntu image signing
key `843938DF228D22F7B3742BC0D94AA3F0EFE21092`, including rejection of a tampered
manifest. Source evidence is `build/ubuntu-iso/amd64-source-verification.json`;
the ISO SHA-256 is
`97f3d7ffb032c3eb3b23d2c8be9cc76e60c2c1f2c0146ba5ba9fe01cafae0fd8`.

Prepare without sudo, then run the explicitly authorized physical test:

```console
/usr/bin/python3 -B tests/test_physical_bios_usb.py --prepare
sudo /usr/bin/python3 -B tests/test_physical_bios_usb.py --erase-authorized-kingston --no-backup
```

The second command first checks the reconnected GPT media against its completed
qualification. Before any write it independently compares initialized boot
bytes and both GPT tables, checks clean ext2 counters, and reads the saved file
and setting directly from the read-only physical USB using the authenticated
ARM64 inspector. All five `e2fsck -f -n` passes must complete cleanly and the
inspection must leave persistence bytes unchanged. A failure prevents native
writer dispatch. The separate `gpt-replug-report.json` records these retention
checks, with the power cycle itself reported by the user.

The GPT retention checks passed on 2026-10-21 before the BIOS rewrite:
3,833,329,152 initialized boot/table bytes matched the original GPT stage, the
exact saved file SHA-256 and setting matched, all five physical filesystem
passes were clean, and the persistence bytes were unchanged by inspection.
Evidence is `build/physical-bios-usb/run-qy850nx8/gpt-replug-report.json` and
`gpt-retention/physical-fsck/` in that same run directory.

Only then does it replace the USB with a production MBR/FAT32 ISO stage and
2 GiB Casper persistence, using the actual device capacity. BIOS preparation
requires the production GRUB2 bootstrap, not merely an MBR signature. The
initial stage `run-qy850nx8` contains 942 extracted files and 18 initialized
ranges totaling 4,126,721,024 bytes; it has an active FAT32 partition, nonzero
MBR bootstrap and no stale backup GPT header. No stage or captured disk is
booted as the test destination.

Two fresh `qemu-system-x86_64` processes use SeaBIOS and TCG CPU emulation.
The actual physical USB is the sole guest disk, without an overlay, direct
kernel/initrd injection, networking or shared folders. QEMU opens the pinned
raw path then drops root with CPUs paused; execution starts only after the
normal-user identity is verified. The guest must report `x86_64` and have no
`/sys/firmware/efi`. Firmware-specific evidence records zero UEFI variable stores.

The runner observes the real GRUB text menu through QEMU's read-only VGA memory
inspection, edits the selected staged entry in memory to append only
`console=ttyS0,115200n8`, then boots that entry normally. Menu/editor text and
screenshots are retained. This follows the installer's
[serial-console requirement](https://canonical-subiquity.readthedocs-hosted.com/en/latest/tutorial/operate-server-installer.html)
without modifying the USB's boot configuration. Unexpected menu/editor content
stops the runner instead of attempting blind boot edits. TCG boots allow up to
20 minutes to reach the installer because this host uses a different CPU architecture.

The shared persistence assertions check a user file and `/etc` setting across
fresh boot IDs, a disappearing volatile control, normal shutdown and finalized
allocation counters. The finishing ARM64 inspector checks the physical USB
read-only, and a full boot-area hash must remain unchanged before safe ejection.
An overall pass requires the physical report to reach `state: complete`
with `success: true`. Sixteen memory/file-only BIOS guards passed; these include
firmware isolation, read-only-handle refusal, no explicit erase intent, refusal
before a write when GPT retention fails, bounded GRUB screen parsing, rejection
of host remounts before guest execution, and continuation refusal after an
unverified write.

The initial physical attempt wrote and natively verified all 4,126,721,024 bytes;
a second reader independently matched every initialized range. It then stopped
before any BIOS guest was created: macOS remounted `/Volumes/PERSIST_TES` during
the long boot-area baseline read and a subsequent writable raw open returned
`Resource busy`. This failed overall report is preserved unchanged. The harness
now acquires the writable whole-device handle before long reads and keeps it
open through the read-only guest inspector. The inspector's own physical block
node remains enforced read-only, with before/after hashes proving immutability.

For this specific verified write, continue without staging or rewriting:

```console
sudo /usr/bin/python3 -B tests/test_physical_bios_usb.py --continue-written-usb
```

The continuation requires the pinned identity, complete original native and
independent readback results, unchanged staged-source hash and persistence UUID,
and no earlier requested kernel boot. A prior menu-editor timeout can be retried
only with empty serial output and no completed console edit, kernel-boot request
or persistence assertions. It preserves the failed report and records
a fresh result under `run-qy850nx8/continue-*/report.json`. Because the host had
mounted the volume, each BIOS boot also compares `/cdrom/md5sum.txt` against its
SHA-256 from the authenticated production stage, then checks all its file
checksums inside Ubuntu. Host metadata additions are not treated as image-file
content failures; any Ubuntu-file mismatch fails the boot test. The continuation
still requires a byte-identical boot area throughout its own persistence exercise.

The first continuation `continue-ytinvxiw` successfully held the raw USB open,
passed its paused physical-transport check and reached the actual GRUB 2.12
menu through SeaBIOS. It stopped in the in-memory editor before requesting a
kernel boot: the harness initially counted from `set gfxpayload` instead of
GRUB's additional first `setparams` line. The captured screen exposed the row
error; the parser now validates and counts from `setparams`, and keypresses are
spaced more slowly. This failed result remains historical evidence, not a
successful Ubuntu BIOS boot.

The corrected continuation `continue-x4s_bsnd` completed successfully on
2026-10-21. Its `report.json` records `success: true`, `state: complete` and
`ejected: true`, with the verified original native write/readback linked through
`sourceWriteReport`. The continuation performed no image rewrite. Both fresh
SeaBIOS boots ran x86_64 Ubuntu directly from the physical USB without an
overlay, and runtime checks confirmed the absence of EFI firmware. Each boot
passed all 370 file checks in the authenticated staged manifest, including the
modified GRUB configurations, kernels and live-system payloads.

The saved user file SHA-256
`aa93c1693ed7e1aca1570769a64a9fad297c1908ca31df4f2354055488cefc53`
and the `/etc` setting survived distinct boot IDs. The volatile control was
absent on boot two. Both normal poweroffs left clean ext2 counters; the direct
read-only physical inspector then passed all five `e2fsck -f -n` passes and
matched the file and setting independently with `debugfs`. Full persistence
hashes were unchanged by inspection. The entire 13,323,206,656-byte boot area
matched its continuation baseline SHA-256
`0d9189ce0a84594f93ecbb27cd441b3fa6fa7500859e36b6ccfce36e6abc828d`
after both boots and inspection. Safe ejection completed the qualification.

The completed report is
`build/physical-bios-usb/run-qy850nx8/continue-x4s_bsnd/report.json`.
Its `boot-1/` and `boot-2/` folders retain GRUB screenshots and console edits,
serial/QMP records, all media-check output, persistence assertions and finalized
headers. `physical-fsck/` retains the physical read-only checker evidence.
This qualifies MBR + legacy BIOS + FAT32 + 2 GiB Casper persistence through the
macOS production writer on this Kingston with QEMU's SeaBIOS/x86 emulation.
Booting a physical PC and writing from native Linux/Windows hosts remain
separate tests. The USB now contains this BIOS test media; the earlier GPT
qualification remains historical evidence for that route.

### Physical dual-mode MBR + BIOS/UEFI

The user power-cycled the BIOS USB. The separate read-only check
`tests/check_physical_bios_replug.py` passed on 2026-10-21:
`build/physical-bios-replug/run-nafwl1x5/report.json`. It independently matched
the saved file and setting with `debugfs`, passed all five `e2fsck -f -n`
passes, and left the complete persistence hash unchanged. It deliberately
leaves the USB connected and unmounted for the following test.

`tests/test_physical_dual_usb.py` prepares MBR + BIOS/UEFI + FAT32 media with
2 GiB Casper persistence from the same authenticated Ubuntu AMD64 ISO. It
uses the production dual-mode stager and guarded native range writer. Only
the pinned Kingston is accepted, and no backup is made per the user's request.
Prepare as the ordinary user, then run the explicit physical action in Terminal:

```console
/usr/bin/python3 -B tests/test_physical_dual_usb.py --prepare
sudo /usr/bin/python3 -B tests/test_physical_dual_usb.py --erase-authorized-kingston --no-backup
```

The x86 UEFI runner uses non-Secure-Boot EDK2 firmware and its headless UART
console. Its terminal decoder is local `pyte==0.8.2` with `wcwidth==0.2.13`
under `build/console-tools/`, installed with `pip --target` (no global changes).
Only the terminal-decoder unit fixture skips when this manual-test dependency
is absent. The physical command itself refuses to proceed without it.

Before granting the native writer erase authorization, the runner binds the
current persistence hash and geometry to the completed power-cycle check. It
then opens the existing physical media read-only under UEFI, validates the
actual GRUB entry, adds a session-only serial-console option, and quits before
requesting a Linux boot. This validates firmware/menu controls before erasure;
it does not qualify the new dual-mode media.

After one native write and independent initialized-range readback, three fresh
QEMU processes boot the physical USB in order: BIOS, UEFI, BIOS. BIOS creates
a user file and `/etc` setting. UEFI verifies those, appends a distinct file
update and changes the setting. The final BIOS boot verifies UEFI's changes.
Distinct boot IDs and absent `/run` controls establish fresh boots. Each boot
checks the expected persistence UUID, read-write COW mount, overlay root,
firmware presence/absence and all Ubuntu checksums against the authenticated
staged manifest. Normal shutdowns must leave clean ext2 accounting.

The final independent inspector accesses the actual USB read-only for five
filesystem-check passes and `debugfs` verification of the final file and
setting. Full persistence hashes must be unchanged by inspection, and the
entire boot area must remain identical throughout the persistence exercise.
Only after these checks and safe ejection may the report pass. An ordinary
staging file is used by the native writer, but no guest boots a destination
image or overlay: its only media disk is the physical Kingston. Results are
written beneath `build/physical-dual-usb/run-*/`; the test is manual-only.

The first dual-mode write in `run-nqn83_0v` passed the read-only pre-erase
UEFI menu check, then natively wrote and verified all 4,126,721,024 bytes.
Before the next raw handle acquisition, Disk Arbitration mounted FAT32 as
`/Volumes/PERSIST_TES`; reopening read-write failed with `Resource busy`.
The failed original report remains unchanged and no persistence guest booted
in that attempt. The harness now retries only `EBUSY`, at most three times,
closing partial handles, revalidating pinned identity, and unmounting on each
attempt. Permission errors and identity changes are never retried.

Continue this verified write without replacing the image:

```console
sudo /usr/bin/python3 -B tests/test_physical_dual_usb.py --continue-written-usb
```

The continuation requires the original completed native verification, exact
prepared source hash/geometry, and no earlier requested guest kernel boot.
Because macOS has already mounted FAT32, it uses an independent bounded FAT32
reader to compare every original file's short path, long filename records,
size and SHA-256, including directory names. Additional host files are counted.
Bootstrap bytes before FAT32 and every initialized range outside FAT32 must
still match exactly. This is explicitly recorded as file and non-FAT metadata
verification, **not** an independent byte-identical comparison of every FAT32
metadata sector. Each of the three Ubuntu boots additionally verifies the
authenticated media manifest. The complete boot area must then remain
byte-identical from the continuation's baseline through final inspection.

`physical_dual_usb_runner_tests.py` has 21 memory/file-only guards covering
explicit authorization, no-rewrite continuation dispatch, identity/access
modes, bounded host-busy retries, retained retention evidence, independent
cross-firmware boots, UART screen decoding and corrupt/missing FAT files,
loops and duplicate names. No physical operation runs under CTest.

The continuation `run-nqn83_0v/continue-ojfevtu7` completed successfully on
2026-10-21. Its final `report.json` records `success: true`, `state: complete`,
`imageRewritePerformed: false` and `ejected: true`, with the original verified
write linked through `sourceWriteReport`. All 942 original staged files
(4,071,509,933 bytes) and their filenames matched independently. The comparison
also matched 35,860,480 initialized bytes outside FAT32 exactly. It counted
95 host-added files; it does not claim byte-identical FAT allocation metadata
after macOS's intervening mount.

Three independent boot IDs confirm BIOS -> UEFI -> BIOS. Each guest checked
the authenticated staged checksum manifest (370 Ubuntu file checks per boot).
The first BIOS boot created the persistent user file and setting. UEFI read
both, appended a distinct marker to the file and replaced the setting. The
third BIOS boot matched those UEFI changes. Volatile controls were absent on
each fresh boot. The final file SHA-256 is
`729e8a78a67dad24ce4a6978c7273be46534bcfa808dae217b59d6320f1bc28a`.
All three normal poweroffs left clean ext2 counters.

The independent read-only physical inspector passed all five `e2fsck -f -n`
passes and matched the final file and setting using `debugfs`. The complete
persistence hash was unchanged by inspection. The whole
13,323,206,656-byte boot area retained its baseline SHA-256
`ec6e6e8ad4eccc0f3a48a62f4ce78bb6ccdffe9e65aba52ca29ba9ad4a51231a`
through all three boots and inspection. Safe ejection completed the test.

The evidence is
`build/physical-dual-usb/run-nqn83_0v/continue-ojfevtu7/report.json`, with
`boot-1-bios/`, `boot-2-uefi/`, `boot-3-bios/` and `physical-fsck/` retaining
commands, firmware/menu evidence, serial logs, media checks, file/setting
assertions and filesystem checks. This qualifies the macOS production writer's
MBR + BIOS/UEFI + FAT32 + 2 GiB persistence route on this physical Kingston
using QEMU's SeaBIOS and non-Secure-Boot EDK2. No destination disk image or
overlay was used for these boots. Physical-PC firmware, Secure Boot and
native Linux/Windows writers remain separate qualifications.

### Physical dummy-file capture/restore qualification

Completed on 2026-10-22: DD, fixed VHD, dynamic VHD and dynamic VHDX capture and
restore data-integrity checks passed on the pinned physical Kingston using
macOS. Every restored disk matched all 15,472,047,104 baseline bytes, all 82
fixture files (3,686,568 bytes) and nine directories. The successful results
include read-only continuations after protected reconnects. The post-write
macOS disk-interface disappearance remains unexplained; these results do not
establish uninterrupted operation. The USB was safely ejected after verification.

#### Procedure and safeguards

`tests/test_physical_capture_usb.py` is a manual-only macOS runner for the
session's disposable Kingston. Its default invocation is read-only preflight.
Writes require `--erase-authorized-kingston` and matching pinned serial, stable
identity, vendor/model, exact capacity and sector size. Native discovery and
`diskutil` independently check the target. CTest registers only mocked safety
checks; it never invokes a physical write.

`/usr/bin/python3 -B tests/prepare_capture_fixture.py` creates the deterministic
local fixture, or verifies and reuses it when present. It never opens a device.
The fixture covers empty/binary files, boundary lengths, Unicode/long names and
nested/empty directories. The SHA-256 manifest is stored on the Mac in
`build/capture-roundtrip/fixture-ready.json`; the target record is
`build/capture-roundtrip/target-readonly.json`.

After authorization, run from the repository in an interactive Terminal:

```sh
sudo /usr/bin/caffeinate -i /usr/bin/python3 -B tests/test_physical_capture_usb.py --erase-authorized-kingston
```

The runner formats FAT32 through the production backend, copies the fixture,
then arms a temporary Disk Arbitration mount-approval guard for that USB. It
proves that an explicit read-only host mount request is denied. The guard never
claims or opens the device; production claims and identity checks still apply.
It matches identity again after reconnects, exits with its supervisor, and is
removed at completion.

For each format, the runner:

1. Captures through the production macOS backend and verifies the result.
2. Independently compares decoded VHD/VHDX against the DD baseline with
   `qemu-img`. QEMU opens only local files in this step.
3. Formats a blank control and proves the fixture is absent using raw FAT32 reads.
4. Restores through the production writer with full native verification.
5. Independently compares every physical disk byte using uncached reads and
   checks every fixture name, size, hash and directory through a bounded FAT32
   parser, without mounting the restored filesystem.

The fixture is small, but whole-device capture and restore still process all
15,472,047,104 bytes. Dynamic containers omit zero blocks rather than arbitrary
unallocated FAT32 clusters; conversion temporarily requires a raw intermediate.
`caffeinate` prevents idle sleep only for the command's lifetime.

Progress and evidence are retained in `build/capture-roundtrip/run-*/report.json`;
`latest-run.txt` names the last run. To resume an authorized write plan, repeat
the erase flag with `--resume /absolute/run-path`. Completed cases are skipped.
The runner authenticates the baseline and preserved containers, archives the
previous report, and refuses existing unverified captures. If native restore
already completed, it finishes independent checks without repeating that write.

To finish only independent checks of an already completed restore, eject and
stop before any further capture or write:

```sh
sudo /usr/bin/python3 -B tests/test_physical_capture_usb.py --verify-restored-usb --resume build/capture-roundtrip/run-n8g7zvpb --wait-for-usb
```

For a disconnected/ejected USB, start with `--wait-for-usb`, wait for
`Mount suppression armed. Reconnect the Kingston now`, then reconnect it. For a
connected, unmounted USB, omit that option. The read-only action cannot be
combined with the erase flag, authenticates the capture hashes, opens the USB
read-only, compares bytes and files, then ejects. A zero exit status completes
only this continuation; overall success remains false until every format has
passed. This is not permission to disconnect during an active write.

#### Results, failures and the VHDX fix

The final run is `build/capture-roundtrip/run-n8g7zvpb/report.json`, with
`success: true`, no pending formats and successful ejection. The independent
acceptance summary is `final-acceptance-20261022.json` beside it. Every format's
physical readback matched the baseline SHA-256:

`0f135e76de3f621b9186437d0a7cf69f3ff05943aedadf500f4c27e46943b98b`

| Capture | Container SHA-256 | Independent physical readback and fixture checks |
| --- | --- | --- |
| DD/raw | Same as baseline above | Passed |
| Fixed VHD | `58be09dba01fa23fa17e05e1800fd24116d21eb76b562a8b04b6507f986fa3b3` | Passed after protected reconnect |
| Dynamic VHD | `20177b9ee84f6adf99472c59cba46c8f7013e97e8ef7dba9f8b95670d97e3ab3` | Passed after protected reconnect |
| Corrected VHDX | `43f722a949e7d0509c4f947c2130f22bc86c7aa021fd42e3e45bed5b225e7ecb` | Passed after protected reconnect |

The first unguarded DD attempt failed byte identity: 8,094 bytes in 45 sectors
changed after native verification. A full read-only diagnostic attributed the
observed contents to Spotlight/FSEvents activity; all fixture files and FAT32
allocation counters remained correct. That attempt remains a failure. Temporary
mount suppression and direct raw file checks allowed the guarded rerun to pass.
`tests/inspect_capture_usb_readback.py` records diagnostic sector differences
without writing, mounting or ejecting the USB. The fixture parser also handles
recorded macOS AppleDouble short-alias collisions with distinct valid long names;
ordinary duplicate aliases remain errors.

Fixed/dynamic VHD and corrected VHDX restores passed full native verification,
then macOS removed the disk interface before the independent reader could open
it. The user confirmed that the USB stayed connected. Registry/log snapshots
still showed USB hardware and its storage driver, without a media child; driver
counters reported no read/write errors or retries. The cause is unconfirmed.
Protected reconnects completed the independent checks without repeating writes.
One read-only continuation was interrupted by closing Terminal and subsequently
restarted successfully. These recoveries do not establish host/USB stability.

The first VHDX capture passed Rufus++'s own decoder but QEMU rejected its zero
`LogOffset`/`LogLength` before any VHDX restore. See QEMU's
[VHDX log parser](https://raw.githubusercontent.com/qemu/qemu/master/block/vhdx-log.c)
and Microsoft's [header specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/39d641c2-093c-4d4a-8c9d-bd4b9fc2ff31).
An APFS diagnostic clone with corrected log fields/checksums decoded identically
to the baseline without changing payload blocks. The original rejected capture
remains preserved as `capture-invalid-log.vhdx`.

The production writer now reserves a non-overlapping, aligned 1 MiB empty log
after the BAT and records it in both checksummed headers. Native tests assert
its geometry. With `qemu-img` installed, `media-capture-interop` independently
decodes ordinary-file fixed VHD, dynamic VHD and VHDX fixtures, compares every
byte, and checks VHDX structure and the empty log. This regression failed before
the fix and passed afterward; all 27 local CTest entries passed. The corrected
writer then completed the fresh physical VHDX capture/restore recorded above.
Linux CI installs `qemu-utils` to run the independent decoder regression too.
VHD inputs explicitly select `force_size_calc=current_size`: older QEMU versions
otherwise use rounded CHS geometry for unfamiliar creator names. The test still
requires the declared size and every decoded byte to match the raw baseline.
These automated checks require neither a device, mount, root access nor a VM.

Earlier failed/interrupted reports, metadata diagnostics, host logs, the rejected
VHDX, diagnostic clone and before/after regression logs remain local under the
ignored build directory. The results qualify data integrity for this fixture,
device and host route after recovery. They do not establish bootability, all GUI
controls, other filesystems, large files, every ISO or another host's backend.
