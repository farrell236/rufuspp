# Rufus++

Rufus++ is a cross-platform boot-media utility for Windows, Linux, and macOS.
It combines a portable C++17 core with a Qt 6 interface and native device
backends for each operating system.

It can analyze, create, format, verify, and capture bootable media, including
Windows installation and Windows To Go media, persistent Linux live media,
raw and compressed disk images, and native macOS installer applications.

> [!WARNING]
> This project is a development prototype, not a released replacement for
> Rufus. Its destructive operations have not completed platform-specific
> hardware qualification. Do not use it with media containing important data.

This is an unofficial project derived from the GPL-licensed
[Rufus project](https://github.com/pbatard/rufus). Pete Batard and Akeo
Consulting do not publish or support this branch.

## Build and test

You will need:

- CMake 3.24 or later
- A C++17 compiler
- A C compiler and `make` on macOS/Linux for the bundled NTFS components
- Qt 6.5 or later with Widgets and Network
- Development packages for zlib, libbz2, liblzma, and libzstd

Configure, build, and test the Qt application:

```console
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

If Qt is installed in a non-system prefix:

```console
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/Qt
```

To build only the portable core:

```console
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

To run the writer tests after building:

```console
ctest --preset core-only -L writers --verbose
```

These tests exercise `RawImageWriter` with in-memory source and target transports,
and `FileImageWriter` with isolated temporary files. They cover exact copying,
verification profiles, trailing metadata clearing, source and target changes,
injected I/O failures, cancellation, progress reporting, and temporary-file
cleanup. They do not open physical devices or require elevated privileges.
With Python 3 available, they also write real FAT16, FAT32, GPT and patterned
images through native OS file I/O, close/reopen them, and independently compare
every byte and SHA-256. The FAT fixtures include known file contents; macOS and
Linux can additionally run read-only filesystem checks. These file-based tests
need no root access. Run just this integration test with:

```console
ctest --preset core-only -R '^native-file-image-write$' --verbose
```

The cross-platform CI workflow runs these automated writer tests on Linux,
Windows and macOS. Physical-device tests remain separate, opt-in operations.

Standard Windows ISO staging on macOS/Linux bundles its NTFS library and
private formatter; it does not require macFUSE or a host ntfs-3g installation.
MBR Windows media supports BIOS and BIOS + UEFI, while GPT is UEFI-only.
Standalone NTFS formatting and Windows To Go retain separate host dependencies.

With Python 3 and `qemu-img` installed, the latest local macOS build passes 27
CTest entries. The independent capture test decodes VHD/VHDX through QEMU and
compares every byte with a raw fixture; it uses ordinary files and requires no
root access or VM. Linux CI installs `qemu-utils` to run this check too.

Physical Kingston USB checks cover Ubuntu writing/persistence, Windows NTFS boot
to Setup under QEMU UEFI and BIOS firmware, and full-byte DD/VHD/VHDX
capture/restore round trips. The capture results include reconnect recoveries;
macOS disk-interface disappearance after some completed writes remains
unresolved. These results do not qualify other host backends, Secure Boot,
actual PC firmware, or a complete Windows installation. See the
[current qualification snapshot](docs/write-testing.md#current-qualification-snapshot).

Native I/O fault tests, opt-in disposable virtual-disk integration, and the
physical-device qualification procedure are documented in
[Destructive-write validation](docs/write-testing.md). A skipped virtual-disk
test is not evidence of a successful device write.

An opt-in test also writes a checksum-verified official Ubuntu ARM64 ISO and
checks the complete readback and boot-file contents. Its preparation, local
runner and limits are described in the same validation document; it does not
download multi-gigabyte fixtures during normal tests.

For this host's qualified physical USB, a manual ARM64 QEMU launcher can capture
the USB read-only, safely eject it, and boot the verified local copy as read-only
virtual USB media. See [QEMU boot preparation](docs/write-testing.md#qemu-boot-preparation).

An additional manual QEMU test creates persistent live media through the
production ISO stager and writer, checks saved user data and configuration
across independent VM boots, and runs a read-only Linux filesystem check. See
[Live Linux persistence test](docs/write-testing.md#live-linux-persistence-test).

The resulting Qt executable is placed under `build/dev/src/qt`. On macOS it is
the `Rufus++.app` bundle.

Manual development packages and tagged draft releases can be built for Windows
x86-64, Linux x86-64, macOS ARM64, and macOS x86-64 from the repository's
**Actions** tab. See [Automated builds and releases](docs/releases.md).

By default, an unsigned macOS development build can inspect media but cannot
write to a physical disk. Write-enabled release builds require a matching Apple
Team ID and code-signing identity for the app and privileged helper.

For local removable-media development only, an alternate whole-process root
mode can be compiled without the helper or code signing:

```console
cmake --preset dev -DRUFUSPP_MACOS_UNSIGNED_ROOT_MODE=ON
cmake --build --preset dev
sudo "build/dev/src/qt/Rufus++.app/Contents/MacOS/Rufus++"
```

The active privilege route is shown only by the bottom-right status indicator,
log, and diagnostics. A normal launch is labelled `UNPRIVILEGED`; a `sudo`
launch is labelled `ELEVATED`. A production macOS build displays
`PRIVILEGED HELPER` only while its signed helper is registered and available.
The backend checks the effective UID again at every physical-device entry point
and keeps per-operation target revalidation, but the complete Qt process runs
as root when unsigned-root mode is enabled.
It is strictly a development and hardware-testing option, not a release
security boundary. Set the option back to `OFF` before configuring a signed
build; it cannot be combined with the Team ID or signing-identity settings.
See the platform documentation below for details.

## Documentation

- [User guide](docs/documentation.md)
- [Features and runtime requirements](docs/features.md)
- [Architecture, safety model, and platform backends](docs/cross-platform.md)
- [Automated builds and releases](docs/releases.md)
- [Changelog](CHANGELOG.md)
- [Write tests and field qualification](docs/write-testing.md)

## License and attribution

This project is a new cross-platform reimplementation inspired by Rufus. The
reimplementation was developed with assistance from OpenAI’s GPT-5.6 Sol model
using the Extra High reasoning setting. AI-assisted development does not
replace independent code review, security assessment, or field testing on
supported platforms and physical removable media.

The project is distributed under the GNU General Public License version 3 or,
at your option, any later version. See [LICENSE.txt](LICENSE.txt).

Rufus and its original source code are copyright Pete Batard and contributors.
New cross-platform code is copyright its respective contributors. Third-party
assets retain their original licences and provenance under `res/` and
`third_party/`. Bundled NTFS source and licensing accompany distribution packages.
