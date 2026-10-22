# Windows NTFS legacy boot code

Data and reference installer from the GPL-licensed ms-sys implementation in
Rufus, pinned to commit `3e696ab19010d7459c31537684522c98ba6a84a9`.

Upstream: https://github.com/pbatard/rufus/tree/3e696ab19010d7459c31537684522c98ba6a84a9/src/ms-sys

The unmodified `br_ntfs_0x0.h` and `br_ntfs_0x54.h` tables install the BOOTMGR
NTFS volume bootstrap. `ntfs.c` retains the upstream GPL-2.0-or-later notice and
installation-offset reference (Copyright 2012 Pete Batard; based on Henrik
Carlqvist's 2009 work). SHA256.json records the downloaded source fingerprints.

Rufus++ uses these tables only for explicitly requested MBR Windows BIOS boot.
It preserves NTFS BPB bytes 0x0b..0x53, validates the partition geometry, updates
the backup boot sector, and verifies the complete modified boot area. The MBR
bootstrap is the existing GPL ms-sys Windows MBR data in windows_boot_code.hpp.

The source tables, reference and notices accompany distribution packages.
