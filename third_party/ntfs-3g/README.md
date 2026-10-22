# Bundled NTFS-3G

Upstream: https://github.com/tuxera/ntfs-3g
Release: 2026.9.28 (unmodified source archive)
Release notes: https://github.com/tuxera/ntfs-3g/releases/tag/2026.9.28
Source: https://download.tuxera.com/opensource/ntfs-3g_ntfsprogs-2026.9.28.tgz
SHA-256: `350d9415c59f7c3fa74e23985ad2d56423c6a168337368f672e2e362d8f295c8`
This matches the checksum published in the upstream release notes.

Rufus++ builds the static libntfs-3g library and a private formatter from
this archive on macOS and Linux. The FUSE driver, plugins, crypto tools and
macOS normalization extension are disabled. Nothing is installed into the
host system, and no Homebrew packages or filesystem extensions are required.

The application links the library statically. The formatter is distributed
as `rufus-mkntfs` beside the executable, never discovered through PATH.
The complete corresponding upstream source and licenses accompany packages.
The CMake build makes upstream's unconditional pkg-config requirement optional
when both FUSE and crypto are disabled. It changes only the extracted configure
script, retains the original archive, and needs no Autoconf/Automake installation.
Optional libhd/libuuid integrations are also disabled to avoid host dependencies.
The unused libintl header check is disabled too, preventing host gettext headers
from remapping `setlocale` to an unbundled library on Intel macOS.
The library/formatter source files carry their individual notices (principally
GPL-2.0-or-later); preserve COPYING and COPYING.LIB from the upstream archive.
