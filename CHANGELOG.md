# Changelog

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
