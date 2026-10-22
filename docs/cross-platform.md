# Cross-platform architecture

The original Win32 implementation and its Visual Studio, MinGW, and Autotools
builds have been retired from the working tree. They remain available in Git
history and in the upstream Rufus repository as behavioral references.

## Safety boundary

The Qt application discovers whole physical devices through native platform
backends and analyzes selected image files. Protected and fixed devices are
hidden by default; showing them is an inspection-only operation.

The default macOS backend contains an experimental DD transport behind a modern
`SMAppService` launch daemon. The signed app and helper mutually authenticate
their designated requirements over XPC. The protocol exposes only health,
write, progress, and cancellation messages: it has no shell-command facility.
Raw image operations pass an already-open source descriptor so the root daemon
does not become a general privileged file opener. Analysis and copying both use
that descriptor. The separate macOS-installer command accepts only an absolute
`.app` path because Apple's tool resolves payloads relative to its bundle. The
helper canonicalizes it, requires the exact
`Contents/Resources/createinstallmedia` location, verifies the complete bundle
against an Apple code-signing requirement before and after use, and permits no
caller-supplied executable or argument list.

For every write, the helper re-analyzes the source, checks that its descriptor
still identifies the same regular file, rediscovers and matches the selected
device, and rebuilds the write plan. The transport then claims the disk through
Disk Arbitration, unmounts the whole disk without forcing, rediscovers it again,
and validates block size, block count, and writability on the opened raw handle.
The shared engine writes aligned blocks, flushes both the descriptor and device
cache, verifies the result, and reports whether any destructive write occurred.
System-device detection follows the IOKit media graph so an APFS synthesized
root volume also protects each physical store that backs it.

The portable file-backed writer is test infrastructure. It explicitly rejects
device paths, refuses to overwrite an existing destination, writes through a
temporary file, supports cancellation, and can verify the result before making
it visible. It is not connected to the Start button.

Image analysis is non-destructive. The portable core walks ISO-9660/Joliet,
Rock Ridge, and ordinary UDF partition-map trees with explicit limits on
directory size, depth, metadata size, and entry count. It validates El Torito,
GPT, WIM, and supported container checksums before using those structures as
capability evidence. The Qt layer only renders that profile.

START remains disabled unless the selected operation has a complete portable
plan and a platform backend provides all of the following operations:

- enumerate physical, removable block devices;
- distinguish system and fixed disks from eligible targets;
- acquire exclusive access and unmount every target volume;
- read and verify device identity again immediately before writing;
- write, flush, cancel, and report progress without corrupting another device;
- notify the operating system after partition-table changes.

Native discovery is still unprivileged and read-only. A raw writer obtains its
own handle and repeats identity and safety validation immediately before the
first destructive operation; discovery metadata alone is never authorization.
The helper is bundled under `Contents/Resources`, and its launch-daemon plist is
bundled under `Contents/Library/LaunchDaemons`. It can be registered only when
`RUFUSPP_MACOS_TEAM_IDENTIFIER` is configured and the app and helper have valid
matching signatures. Ordinary ad-hoc development builds deliberately report
raw access as unavailable.

macOS also has an explicit development-only configuration,
`RUFUSPP_MACOS_UNSIGNED_ROOT_MODE=ON`. It omits the helper, launch-daemon plist,
ServiceManagement client, and signing steps from the build. The application can
still inspect media as a normal user, but each physical raw write, destructive
bad-block test, raw capture, and Apple installer operation checks for an actual
effective UID of zero. These operations therefore require launching the bundle
executable from Terminal with `sudo`. The same local transports retain target
identity, eligibility, geometry, exclusive-claim, flush, and verification
checks. Capture output ownership is restored from validated `SUDO_UID` and
`SUDO_GID` values. The bottom-right UI indicator labels an ordinary launch
`UNPRIVILEGED` and a root launch `ELEVATED`. A signed-helper build reports
`PRIVILEGED HELPER` only after its helper is registered and available; otherwise
it remains `UNPRIVILEGED`. Privilege state is deliberately omitted from the
window title. Unsigned-root mode is not suitable for distribution. Hardware
fault coverage and release signing remain outstanding.

Linux and Windows implement the same DD contract as direct elevated transports.
Linux uses non-forced `umount2` operations followed by an `O_EXCL` whole-block-
device handle, kernel geometry and read-only ioctls, `fsync`/block-cache
invalidation, verification, and `BLKRRPART`. Windows maps the source and every
target volume to physical disk numbers, locks each target volume with
`FSCTL_LOCK_VOLUME`, dismounts it, revalidates the PhysicalDrive number and
geometry, uses write-through I/O, flushes, verifies, and calls
`IOCTL_DISK_UPDATE_PROPERTIES`. Both open the source before changing the target
and refuse a source stored on that target.

For this first implementation, AUTHORIZE launches an elevated application
through UAC on Windows. Linux exposes raw writing only to an application that
was started through a trusted administrator launcher; it does not attempt to
smuggle display credentials through `pkexec`. Moving those destructive handles
into narrowly scoped, authenticated helper processes remains release-hardening
work; whole-process elevation is not the final privilege boundary.

The runtime indicator uses the same vocabulary on every host:
`UNPRIVILEGED` means no privileged write transport is active, `ELEVATED` means
the complete application process has root or Administrator access, and
`PRIVILEGED HELPER` means the GUI remains unprivileged while an authenticated
helper performs guarded disk operations. On Windows, AUTHORIZE opens a separate
UAC-elevated application process; it does not elevate the existing window in
place.

## Executable operation matrix

| Operation | macOS | Linux | Windows |
| --- | --- | --- | --- |
| Raw/DD and supported compressed images | Signed helper, or opt-in unsigned root process for development | Elevated process | Elevated process |
| Apple macOS installer application | Apple `createinstallmedia` through signed helper, or development root process | Unavailable | Unavailable |
| FAT32 ISO and standard Windows Setup | Portable stager + native raw writer | Portable stager + native raw writer | Portable stager + native raw writer |
| NTFS ISO (BIOS/UEFI) | Bundled direct NTFS library/formatter | Bundled direct NTFS library/formatter | DiskPart, elevated |
| Linux persistence | Portable FAT32/ext2 stager + native raw writer | Portable FAT32/ext2 stager + native raw writer | Portable FAT32/ext2 stager + native raw writer |
| Windows To Go | wimlib + host tools + BCD-SYS | wimlib + host tools + BCD-SYS, elevated | wimlib + DiskPart + BCDBoot, elevated |
| FFU | Analysis only | Analysis only | DISM `/Apply-Ffu`, elevated |
| Drive capture: DD/VHD/VHDX | Signed helper or development root process + portable converter | Elevated process + portable converter | Elevated process + portable converter |
| Drive capture: FFU | Unavailable | Unavailable | DISM `/Capture-Ffu`, elevated |
| Mounted-volume UDF capture | `hdiutil makehybrid` | `genisoimage`/`mkisofs` | user-installed `oscdimg.exe` |
| Standalone FAT16/FAT32/ext2 | Portable stager + native raw writer | Portable stager + native raw writer | Portable stager + native raw writer |
| Standalone host filesystems | exFAT/UDF; optional NTFS/ext3 | NTFS/exFAT/UDF/ext3 tools, elevated | NTFS/exFAT/ReFS through DiskPart |
| Blank boot media | GRUB2, Syslinux, Grub4DOS, ReactOS, FreeDOS/MS-DOS, UEFI | Same | Same |
| Runtime UEFI validation | Packaged offline app + portable manifest | Packaged offline app + portable manifest | Packaged offline app + portable manifest |
| Bad-block/fake-capacity test | Signed helper or development root process | Elevated process | Elevated process |

Each dependency-gated cell is exposed only after its complete provider reports
available. Missing dependencies produce an actionable reason and keep START
disabled; no row falls back to a similarly named but incomplete operation.

## ISO-mode deployment

The FAT32 ISO-mode strategy is shared by every host. The portable
core opens the analyzed ISO once for the complete stage, uses its validated
ISO-9660/Joliet or ordinary UDF tree, rejects missing
or unreadable extents and unsafe FAT names, and builds an aligned MBR or GPT
disk with a FAT32 partition. The immutable plan carries the selected partition
scheme, BIOS/UEFI target, filesystem, cluster size, and quick/full policy.
Invalid combinations are rejected before staging; GPT is UEFI-only and includes
CRC-checked primary and backup tables. It writes both FATs, the primary and backup FAT32 boot
records and FSInfo sectors, long and unique 8.3 names, directory chains, and
file chains. Every staged file is compared with its optical-image source before
the disk image can proceed.

The staged image is then passed through the same immutable raw-write plan used
by DD mode. Consequently macOS commits it through the authenticated XPC helper,
Linux through its exclusive block-device transport, and Windows through its
locked PhysicalDrive transport. Each OS therefore retains its native target
identity check, volume unmount, authorization, flush, read-back verification,
partition-table refresh, cancellation, and partial-write reporting; there are
no shell-driven format or mount commands in the UI process. The staging file is
removed after the native commit or after any recoverable failure. Quick MBR
plans also clear and verify the final MiB of the target so stale backup GPT
metadata cannot survive. Quick GPT and persistence stages retain a complete
logical source image but supply formatter-authored initialized write ranges.
The shared writer seeks directly to these offsets and verifies the transferred
bytes; unallocated gaps are not copied or read back. Every ext2 group's complete
inode table is included, even where it contains only zeroes. The final MiB is
copied from the new image to clear stale MBR-target GPT signatures or preserve
the new GPT backup tables. Full formats still overwrite and verify the complete
device; ordinary raw/DD images remain byte-for-byte dense copies. The macOS
signed helper receives typed, bounded ranges through protocol version 7 and
revalidates them with the same planner before opening the target.

For standard Windows Setup media, the same portable formatter also installs an
active Windows-compatible MBR and FAT32 BOOTMGR bootstrap when the target has
512-byte logical sectors. This supports BIOS-only and dual BIOS/UEFI Windows
install media without calling Windows formatting APIs. The bootstrap byte
sequences are a data-only adaptation of the GPL-licensed ms-sys assets retained
in Rufus Git history; no retired Win32 application code is reintroduced.

If `sources/install.wim` or `sources/install.esd` is too large for FAT32, the
planner requires wimlib 1.13.4 or later. The stager extracts that one entry to a
private directory and uses the runtime-loaded wimlib C API to create
`install.swm`, `install2.swm`, and subsequent parts. Solid ESD input is first
exported into an ordinary LZX WIM model so Windows Setup can consume the split
set. The stager then checks every part's size, WIM signature, GUID, part
number, and total-part count. The original `install.wim` is omitted and the
validated split set is copied into the FAT32 image. Cancellation reaches
wimlib's progress callback, all temporary WIM material is removed, and the
normal staged-file verification still runs before raw-device access begins.
The dynamic adapter uses native UTF-16 paths on Windows and UTF-8 paths on
macOS/Linux; the deployment algorithm above it is identical on all hosts.

The app searches for wimlib beside the executable and in safe platform library
locations. Distribution packages must bundle the library or declare it as a
dependency before large-WIM support can be considered available in that build.
For FAT32-overflow files or explicit NTFS selection, the planner selects an NTFS
layout only when the host staging provider is available. GPT remains UEFI-only.
For x86/x64 Windows installers with a root BOOTMGR file, MBR supports BIOS-only
and BIOS + UEFI: the stager installs Windows MBR code, marks NTFS active, and
installs pinned GPL ms-sys NTFS BOOTMGR code after the filesystem closes. BPB,
MFT locations and volume serial are preserved; the backup boot sector is updated
and both boot areas are read back. BIOS-only media has no helper partition.
UEFI and dual-mode media include the data-only GPL UEFI:NTFS 2.8 helper partition.
ARM and non-Windows NTFS BIOS requests are rejected. The implementation and
ordinary-file tests passed on macOS. On 2026-10-21, GUI-created MBR + BIOS/UEFI +
NTFS Windows 11 media on the physical Kingston reached Windows Setup under both
EDK2 UEFI and SeaBIOS in QEMU, using the same USB without rewriting between boots.
Both physical backing devices were read-only with private guest-write overlays.
BIOS-only layout, Linux/Windows host runtime and actual-machine firmware
qualification remain pending. On macOS and Linux the portable extractor streams
ISO files straight into the bundled, statically linked libntfs-3g 2026.9.28 library.
A private `rufus-mkntfs` beside the executable formats an ordinary partition
file with explicit sector size and starting LBA. It is never located via PATH.
No filesystem mount, loop device, macFUSE/FSKit extension, installed ntfs-3g,
or root access is needed for this staging step. The existing privileged device
writer still controls the final USB write. Windows retains its temporary fixed
VHD and native DiskPart path.

The library uses Windows-compatible case-insensitive name lookup, rejects file
collisions, writes empty/Unicode/large files, closes all file and volume metadata,
then reopens the filesystem read-only and byte-verifies every file against the
ISO (including generated unattended-setup content). The allocated-cluster bitmap
and backup NTFS boot sector determine the quick-format write ranges. The stager
copies and verifies those ranges into the final MBR/GPT image and includes both
the optional UEFI:NTFS helper and disk-tail metadata in the write map. A full format retains
dense zero-fill semantics. MBR retains its 32-bit sector limit; GPT includes both
table copies. Cancellation/failure deletes the incomplete output and private work
directory. GUI preflight and the backend free-space guard share the same
host-specific estimate: macOS/Linux budget two copies of the expanded image size with
10% headroom per copy plus a shared 256 MiB reserve for quick format, or twice
target capacity plus that reserve for full format. Windows budgets one native
staging image (110% of expanded image size for quick, target capacity for full) plus the
reserve. Arithmetic overflow blocks preflight. Quick NTFS write size is deferred
on macOS/Linux until allocated ranges are known; Windows retains the known dense
transfer size. Receipts keep the original preflight and actual outcome counts.

The pinned upstream source archive, published SHA-256, licensing notices and
build recipe live in `third_party/ntfs-3g` and accompany distribution packages.
CMake builds only the static library and formatter; no host software is installed.
This changes standard NTFS ISO deployment. Standalone NTFS formatting and Windows
To Go retain their separate host-tool dependencies.

A standard `/boot/grub/i386-pc` tree receives the same GPL GRUB 2.14 MBR/core-
image strategy used by Rufus, with the core image placed before the aligned
FAT32 partition. Common `/boot/grub2` and root `/grub` module trees are copied
to the bootstrap's expected `/boot/grub` path while preserving their original
layout. Syslinux-only legacy-BIOS layouts stay gated unless DD mode is
available. Non-512-byte-sector Windows and GRUB targets remain UEFI-only.

Linux persistence extends that portable ISO stage instead of adding a fourth
raw-device implementation. The planner applies the 256 MiB minimum and retains
110% of the extracted file-tree size for the FAT32 live partition. The stager
places a Linux filesystem partition at the end of the selected MBR or GPT disk, creates a sparse,
verified ext2 filesystem directly in the image, and selects `casper-rw` or the
Debian `persistence` plus `/persistence.conf` convention from ISO analysis.
Recognized GRUB/Syslinux configurations receive `persistent` or `persistence`
idempotently, and `MD5SUMS`/`md5sum.txt` entries are repaired for changed files.
No host mount, shell formatter, or retired Win32 formatting code is involved.
Casper media also receives a systemd shutdown helper inside the persistence
upper layer. It selects the uniquely matching generated filesystem UUID,
exposes its otherwise hidden backing mount, syncs it and remounts it read-only
after services stop. This lets Linux finalize its allocation counters during
normal shutdown. The stager verifies the helper's complete file/directory tree;
non-systemd shutdown behavior requires separate validation.
UEFI boot uses the extracted ISO bootloaders. Standard and common alternate
GRUB2 i386-pc trees are also bootstrapped for legacy BIOS. Syslinux-only and
truly distribution-specific loaders remain capability-gated because their
installed boot sector and `ldlinux` files must exactly match the media version.

## Standalone media and capture

Standalone formatting never asks a host formatter to touch the selected disk.
The portable FAT16/FAT32/ext2 implementations, or a capability-gated native
filesystem tool, construct a private MBR image first. Metadata is checked after
the image is detached, and only a successful image can enter the guarded raw-
write path used by ISO mode. macOS supplies exFAT and UDF formatters; Linux can
use installed NTFS, exFAT, UDF, and ext3 tools; Windows DiskPart supplies NTFS,
exFAT, and ReFS. An NTFS provider can also combine the data volume with the
embedded UEFI:NTFS helper partition.

Allocation-unit requests are validated at the portable/core boundary and
forwarded to capable host formatters. A full format expands the staged image
across the complete target and the guarded writer verifies that coverage;
quick FAT16/FAT32 formats retain the compact metadata-prefix path.

FreeDOS payloads are bundled under their original free-software license.
MS-DOS, Grub4DOS, ReactOS, and generic UEFI application payloads must be chosen
from local files by the user. GRUB2 and Syslinux 4.07 use version-matched
embedded GPL boot data; the Syslinux loader is patched against its actual FAT
sector allocation before staging. UEFI Shell 2.2 release executables are
embedded for x86-64, IA32, AArch64, ARM32, RISC-V 64, and LoongArch64 and can
be installed individually or together. The application checks their pinned
SHA-256 digest, PE architecture, and EFI application subsystem before use.
ARM32 is retained from its final supported upstream release. These Shell
executables are not Microsoft Secure Boot signed, so the preflight explicitly
warns that Secure Boot must be disabled. No bootloader or shell is downloaded.

Device capture uses guarded read-side transports and writes a new, verified
destination. DD and fixed/dynamic VHD plus dynamic VHDX are portable. FFU is
delegated only to Windows DISM. UDF capture is intentionally different: it
archives the one mounted source volume's file tree, not the source disk's
partition layout. The provider is `hdiutil makehybrid` on macOS,
`genisoimage`/`mkisofs` on Linux, or a user-supplied ADK `oscdimg.exe` beside
the Windows application.

The advanced destructive media test writes address-dependent patterns across
the advertised capacity, flushes, and reads every sector back for up to four
passes. Address dependence exposes damaged sectors and fake drives whose high
addresses alias real low-address flash cells. Counts and the first mismatching
byte offsets are retained in the failure message.

Runtime UEFI validation remains offline. For supported architectures the ISO
stager renames the original fallback loader, installs the packaged validator,
and creates a complete MD5 manifest. The option is exposed only for a UEFI-
bootable ISO-mode plan and is revalidated as part of staging.

## FFU deployment

FFU is an operating-system image container, not a raw byte stream. The portable
analyzer validates the security, image, manifest, store, descriptor, and payload
boundaries but never exposes the container itself as DD data. On Windows, the
native backend provides an FFU capability through DISM `/Apply-Ffu`: it locks
and revalidates the source file, rejects a source stored on the target, locks
and dismounts the target volumes, revalidates physical-drive identity, releases
its locks for DISM, supports cancellation, and refreshes disk properties after
success. Linux and macOS keep valid FFU inputs inspection-only because they do
not provide the Windows servicing stack.

Windows To Go has a separate staging path because it applies one WIM/ESD image
to NTFS rather than copying Windows Setup files. Its immutable plan retains the
selected edition, source timestamp, target identity, normalized label, and
generated unattended settings. The stager creates a sparse GPT disk image with
an EFI System Partition, Microsoft Reserved Partition, and NTFS Windows
partition; wimlib injects `Windows/Panther/unattend.xml` in memory and applies
the selected edition without modifying the ISO. Windows attaches the staging
file as a fixed VHD and uses DiskPart/BCDBoot. Linux and macOS use their native
image/loop attachment tools and BCD-SYS. Only after boot construction succeeds
does the existing native raw writer receive the staged image. Missing tools,
insufficient temporary space, unsupported sector geometry, or insufficient
privilege keep START disabled before destructive I/O.

## Intended layers

1. `rufus_plus_plus_portable_core`: operating-system-neutral value types, media analysis,
   partition planning, validation, and transformation rules.
2. `rufus_plus_plus_platform_backend`: native Windows, Linux, and macOS physical-device
   discovery plus guarded DD transports; macOS defaults to a signed-helper
   boundary and offers an explicit unsigned whole-process mode for development,
   while Linux/Windows currently use explicitly elevated direct transports.
3. `rufus_plus_plus`: presentation and user interaction. The UI consumes core models
   and backend interfaces but does not perform raw I/O directly.
4. Historical Rufus: consult Git history and upstream source for compatible
   behavior without introducing Win32 types into the portable architecture.

## Milestones

1. **Complete:** establish CMake, Qt, the portable core, and cross-platform CI.
2. **Complete for DD mode:** immutable write plans and a virtual-target raw-copy
   engine cover alignment, source changes, cancellation, flush failures,
   verification, and partial-write reporting.
3. **Implemented, awaiting hardware coverage:** read-only physical-device
   discovery for macOS (IOKit), Linux (sysfs), and Windows (storage interfaces).
4. **Implemented, awaiting release validation:** the macOS Disk Arbitration/raw-
   I/O transport and signed `SMAppService`/XPC helper boundary are implemented.
   An opt-in, visibly labelled unsigned-root build reuses the local operations
   for hardware development without packaging the helper. Developer ID signing,
   notarization, and hardware-backed fault coverage remain before treating it as
   release-ready.
5. **Implemented, awaiting release validation:** Linux and Windows raw
   transports use the same write plan and copy engine. Dedicated least-
   privilege helpers and removable-hardware fault coverage remain outstanding.
6. **Complete for common boot-media structures, sparse VHD disks, FFU analysis/application, and compressed DD images:**
   ISO-9660, Joliet, Rock Ridge, UDF type-1 partition maps, El Torito catalogs,
   MBR/GPT, WIM metadata, VHD/VHDX/FFU identification, Windows and Linux
   profiles, boot architecture, and large-file analysis are implemented. Gzip,
   Bzip2, single-image ZIP, LZMA-alone, XZ, and Zstandard sources receive a complete checksummed
   decompression preflight followed by streaming write and streaming verification
   through the shared raw engine. This also resolves gzip's exact expanded size
   without trusting its modulo-4-GiB trailer field. Uncommon UDF virtual,
   sparable, and metadata partition maps remain unsupported. Fixed and
   parentless dynamic VHD/VHDX payloads are exposed through bounded virtual-disk
   readers; differencing chains and active VHDX logs stay capability-gated.
   Structurally valid FFU images are applied by the Windows DISM provider and
   remain inspection-only on hosts without that servicing stack.
7. **Complete for selectable MBR/GPT FAT32 and dependency-gated NTFS ISO mode plus standard Windows Setup:** capability-driven
   Qt controls execute standard Windows installation and ISO file-copy choices
   through a portable, verified FAT32 staging engine and the native
   macOS/Linux/Windows raw writer. Standard Windows media includes BOOTMGR BIOS
   bootstrap support on 512-byte-sector targets and wimlib-backed `install.wim`
   and `install.esd` transformation. Partition scheme, compatible firmware
   target, filesystem, allocation unit, and quick/full format policy are part
   of the validated plan. Credential-free standard-installation
   Windows User Experience options are embedded as `autounattend.xml`. General
   UEFI ISO payloads that exceed FAT32 limits use NTFS plus UEFI:NTFS through
   the bundled macOS/Linux library/formatter or Windows DiskPart. Supported MBR
   Windows media also receives NTFS BOOTMGR BIOS boot code. The same physical
   dual-mode NTFS USB reached Setup under UEFI and BIOS QEMU firmware.
8. **Implemented, dependency-gated:** Windows To Go edition selection, Rufus-
   style Windows User Experience customization, unattended configuration,
   GPT/EFI/NTFS staging, WIM/ESD application, boot construction, cancellation,
   and guarded raw commit are wired for Windows, Linux, and macOS. Packaging
   wimlib, NTFS tooling, and BCD-SYS remains distribution work.
9. **Implemented for UEFI and common GRUB2 BIOS live media:** Linux persistence uses the portable
   MBR/GPT FAT32 ISO stage, an embedded ext2 formatter, Casper and Debian Live
   conventions, boot-configuration patching, checksum repair, validation,
   cancellation, and the existing native raw commit. Broader fixtures for
   distribution-specific ISO boot layouts remain follow-up work.
10. **Implemented, awaiting hardware coverage:** checksum calculation, portable
    DD/VHD/VHDX capture, Windows DISM FFU capture, provider-gated filesystem-
    aware UDF capture, FAT16/FAT32/ext2 and host-backed standalone formatting,
    FreeDOS/MS-DOS and blank bootloader installation, offline runtime UEFI
    validation, and multi-pass bad-block/fake-capacity testing are wired to the
    Qt controls. Physical removable-media fault testing remains outstanding.
11. **Implemented:** structured preflight reports and atomic JSON deployment
    receipts, Fast/Standard/Full verification policies, dependency health
    reporting, bounded read-only media inspection, operation sleep/stall guards,
    offline EFI hash/SBAT analysis with a verified, versioned Microsoft signed
    DBX baseline, user-supplied text/EFI DBX overlays, and a user-initiated,
    confirmation-gated signed-release update path; and optional read-only QEMU
    launch smoke testing. Full Authenticode chain
    policy, certificate revocations, Windows SVN policy, dedicated Linux and
    Windows privilege helpers, and hardware qualification remain hardening or
    release-engineering work rather than silently reported capabilities.
12. **Implemented on macOS, awaiting signed hardware coverage:** Apple-signed
    installer-application analysis, capability-driven Qt controls, target
    identity/capacity checks, optional full-device zero-fill/read verification,
    GPT/HFS+ preparation, and cancellation-aware `createinstallmedia` execution
    are routed through the mutually authenticated helper by default, or through
    the explicitly selected unsigned-root development transport. Installer
    downloads, live-macOS creation, and undocumented Apple tool switches are
    intentionally out of scope.

Windows-specific features such as ReFS and DISM/FFU operations remain optional
capabilities instead of being emulated on platforms that cannot provide them
safely.

The `windowsToGo` flag requires a structurally valid WIM header and XML image
metadata as well as Windows install and EFI boot files. The deployment preflight
also requires a 512-byte-sector target of at least 32 GiB, enough temporary
space, the complete host toolchain, and a guarded raw writer. Linux persistence
is host-tool independent but still requires compatible UEFI Syslinux/GRUB live
media and at least 256 MiB after the ISO sizing allowance.
