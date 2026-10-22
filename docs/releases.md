# Automated builds and releases

Rufus++ has two package-building entry workflows, **Build Development** and
**Publish Release**, under the repository's **Actions** tab. Both build the exact
commit selected in GitHub's branch menu,
run the complete test suite on every target, deploy the required Qt runtime,
and package four artifacts:

| Platform | Architecture | Package |
| --- | --- | --- |
| Windows | x86-64 | Portable ZIP |
| Linux | x86-64 | Portable tar.gz |
| macOS | Apple Silicon (ARM64) | DMG |
| macOS | Intel (x86-64) | DMG |

## Build Development

Choose **Actions → Build Development → Run workflow**. Development packages
are retained as workflow artifacts for 14 days and do not create a Git tag or
GitHub Release. `RelWithDebInfo` is the default; `Debug` can be selected when
diagnostics are more important than package size.

## Publish Release

Choose **Actions → Publish Release → Run workflow** from the default branch and
enter a version without a `v` prefix. The workflow rejects malformed or reused
versions, builds all four packages in CMake `Release` mode, and creates the tag
only after every build and test succeeds. It then creates a draft GitHub
Release, uploads the packages and SHA-256 checksums, and leaves publication as
a deliberate manual step.

Each release requires `docs/release-notes/<version>.md`. The workflow uses that
file as the release changelog, then appends the build commit and macOS package
notice. After checking all four packages, publish the draft to make the website's
download links and changelog pick it up automatically. The website includes
published prereleases and excludes drafts.

macOS/Linux packages built from this updated source include the statically linked
NTFS library, a private `rufus-mkntfs` next to the executable, and the bundled source/licenses. They do
not install macFUSE or a host NTFS driver. Windows retains native DiskPart for
standard NTFS ISO staging. Package builds need the host C compiler and `make`
on macOS/Linux in addition to the C++/Qt toolchain. Package CI runs the automated
suites on all four build targets; macOS field results do not establish physical
Linux or Windows host qualification.

The current macOS packages are clearly named `unsigned-root` and are intended
only for controlled development and hardware qualification. Their bottom-right
indicator displays `UNPRIVILEGED` when opened normally and `ELEVATED` when the
bundle executable is launched directly with `sudo`; only the latter enables
physical-device operations. They are not Developer ID signed or notarized. A
public production release still requires the signed-helper packaging and Apple
notarization workflow described in [cross-platform.md](cross-platform.md).
