# Changelog

## 0.2.1 — 2026-10-22

- Fix dynamic VHDX captures rejected by independent readers: reserve an empty,
  aligned log region and record its location in both headers.
- Add independent QEMU decoding checks for captured VHD/VHDX files and resumable
  physical USB capture/restore qualification.
- Record successful DD, fixed/dynamic VHD and VHDX physical USB data-integrity
  checks on macOS, including reconnect recoveries and the unresolved post-write
  disk-interface disappearance.

See the [full release notes](docs/release-notes/0.2.1.md) for validation and scope.

## 0.2.0 — 2026-10-21

- Bundle direct NTFS staging on macOS/Linux for standard Windows ISO media.
- Add Windows NTFS legacy BIOS and BIOS/UEFI dual-mode boot support.
- Optimize quick writes using initialized filesystem ranges.
- Fix persistence counters/shutdown consistency, FAT32 directory parents and
  trailing partition-metadata handling.
- Strengthen raw-write identity checks and failure/finalization reporting.
- Add visible stage/percentage/byte/elapsed progress and accurate NTFS preflight
  space estimates, deferred quick-write totals and receipt coverage.
- Expand automated writer tests and document physical USB boot/persistence tests.

See the [full release notes](docs/release-notes/0.2.0.md) for validation and scope.
