/* SPDX-License-Identifier: GPL-3.0-or-later */
// Test-only helper. Receives an inherited handle, never a raw-device pathname.
// The parent runner creates and proves ownership of the backing image.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "system_raw_io.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/standalone_media.hpp"

#if defined(_WIN32)
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace {
using namespace rufus::core;
using namespace rufus::backend::detail;
constexpr std::uint64_t kCapacity = 16U * 1024U * 1024U;
constexpr std::uint64_t kMaximumCapacity = 16ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kTail = 1024U * 1024U;

void check(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

BlockDeviceInfo descriptor(const std::filesystem::path& source, const std::uint64_t capacity = kCapacity) {
  BlockDeviceInfo target;
  // This descriptor is for the portable copy plan only. It does not override
  // discovery eligibility in the application or its privileged helper.
  target.stableId = "unit-test:owned-virtual-handle";
  target.devicePath = (source.parent_path() / "inherited-virtual-target").u8string();
  target.capacityBytes = capacity;
  target.logicalSectorSize = 512U;
  target.bus = DeviceBus::Virtual;
  target.removable = target.writable = target.wholeDevice = true;
  return target;
}

NativeHandle inheritedHandle(const char* value) {
  const auto number = std::stoull(value);
#if defined(_WIN32)
  return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number));
#else
  check(number >= 3U && number <= static_cast<unsigned int>(std::numeric_limits<int>::max()),
        "expected an inherited descriptor, not stdin/stdout/stderr");
  return static_cast<int>(number);
#endif
}

void checkGeometry(const NativeHandle handle, const std::uint64_t expectedCapacity) {
  check(expectedCapacity >= kCapacity && expectedCapacity <= kMaximumCapacity &&
            expectedCapacity % kTail == 0U, "bounded, MiB-aligned test capacity required");
  std::uint64_t capacity = 0U;
  std::uint32_t sector = 0U;
#if defined(_WIN32)
  alignas(DISK_GEOMETRY_EX) std::array<unsigned char, sizeof(DISK_GEOMETRY_EX) + 1024U> bytes{};
  DWORD returned = 0;
  check(DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                          bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr) != 0,
        "query inherited virtual-disk geometry");
  const auto* geometry = reinterpret_cast<const DISK_GEOMETRY_EX*>(bytes.data());
  check(geometry->DiskSize.QuadPart > 0, "positive capacity");
  capacity = static_cast<std::uint64_t>(geometry->DiskSize.QuadPart);
  sector = geometry->Geometry.BytesPerSector;
#else
  struct stat status {};
  check(fstat(handle, &status) == 0, "inspect inherited descriptor");
#if defined(__APPLE__)
  check(S_ISCHR(status.st_mode), "expected an inherited raw-disk character device");
  std::uint64_t blocks = 0U;
  check(ioctl(handle, DKIOCGETBLOCKSIZE, &sector) == 0 &&
            ioctl(handle, DKIOCGETBLOCKCOUNT, &blocks) == 0, "query virtual-disk geometry");
  check(sector != 0U && blocks <= std::numeric_limits<std::uint64_t>::max() / sector,
        "valid geometry");
  capacity = blocks * sector;
#else
  check(S_ISBLK(status.st_mode), "expected an inherited loop block device");
  int nativeSector = 0;
  check(ioctl(handle, BLKGETSIZE64, &capacity) == 0 &&
            ioctl(handle, BLKSSZGET, &nativeSector) == 0 && nativeSector > 0,
        "query virtual-disk geometry");
  sector = static_cast<std::uint32_t>(nativeSector);
#endif
#endif
  check(capacity == expectedCapacity && sector == 512U,
        "only the runner's exact capacity / 512-byte image is accepted");
}

void inspectIso(const std::filesystem::path& source) {
  check(std::filesystem::is_regular_file(source), "ISO source must be a regular file");
  const auto analysis = ImageAnalyzer{}.analyze(source);
  check(analysis.succeeded(), "analyze official ISO image");
  const auto& image = *analysis.image;
  check(image.format == ImageFormat::Iso && image.capabilities.rawWrite &&
            image.capabilities.iso9660 && image.bootable && image.capabilities.uefiBootable,
        "expected a raw-writable, bootable UEFI ISO9660 image");
  std::cout << "{\"format\":\"" << imageFormatName(image.format)
            << "\",\"architecture\":\"" << imageArchitectureName(image.architecture)
            << "\",\"partitionScheme\":\"" << partitionSchemeName(image.partitionScheme)
            << "\",\"sizeBytes\":" << image.sizeBytes
            << ",\"rawWrite\":true,\"uefiBootable\":true}\n";
}

void writeImage(const NativeHandle handle, const std::filesystem::path& source, const bool quick,
                 const std::uint64_t capacity) {
  checkGeometry(handle, capacity);
  const auto analysis = ImageAnalyzer{}.analyze(source);
  check(analysis.succeeded(), "analyze staged filesystem image");
  const auto plan = WritePlanBuilder{}.buildRawWriteWithVerification(
      *analysis.image, descriptor(source, capacity),
      analysis.image->deploymentSizeBytes() == capacity ? VerificationProfile::Full
                                                       : VerificationProfile::Standard,
      64U * 1024U, quick);
  check(plan.succeeded(), "build isolated transport test plan");
  SystemRawTarget target(handle, capacity, 512U);
  const auto result = RawImageWriter{}.write(*plan.plan, target);
  check(result.success && result.verificationCompleted, result.error);
  check(result.bytesWritten == analysis.image->sizeBytes, "complete source byte count");
  std::cout << "Native write, device flush and verification passed ("
            << result.bytesWritten << " image bytes)\n";
}

void verifyReopened(const NativeHandle handle, const std::filesystem::path& source, const bool quick,
                     const std::uint64_t capacity) {
  checkGeometry(handle, capacity);
  const auto sourceSize = std::filesystem::file_size(source);
  std::ifstream input(source, std::ios::binary);
  check(static_cast<bool>(input), "open expected image");
  SystemNativeIo io(handle);
  std::array<unsigned char, 64U * 1024U> actual{}, expected{};
  check(sourceSize <= capacity && sourceSize % 512U == 0U, "aligned source fits target");
  for (std::uint64_t offset = 0U; offset < capacity; offset += actual.size()) {
    expected.fill(0xa5U);
    if (offset < sourceSize) {
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(expected.size(), sourceSize - offset));
      input.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(count));
      check(input.gcount() == static_cast<std::streamsize>(count), "read expected source bytes");
    }
    if (quick && offset >= capacity - kTail) expected.fill(0U);
    std::string error;
    check(nativeTransfer(io, false, offset, actual.data(), actual.size(), error), error);
    check(actual == expected, "reattached device differs from expected image/gap/tail at " + std::to_string(offset));
  }
  std::cout << "Independent read-only reattach: all " << capacity << " bytes verified\n";
}
}  // namespace

int main(const int argc, const char* const argv[]) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--inspect-iso") {
      inspectIso(std::filesystem::u8path(argv[2]));
      return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--prepare") {
      const auto source = std::filesystem::u8path(argv[2]);
      const std::string mode = argv[3];
      check(mode == "quick" || mode == "full", "unknown test mode");
      check(!std::filesystem::exists(source), "never overwrite a source fixture");
      StandaloneFormatOptions options;
      options.quickFormat = mode == "quick";
      const auto staged = stageBlankFat16Media(descriptor(source), source, "RUFUSTEST", {}, {}, options);
      check(staged.success, staged.error);
      return 0;
    }
    if (argc == 5 || argc == 6) {
      const std::string action = argv[1], mode = argv[4];
      check(mode == "quick" || mode == "full" || (mode == "iso" && argc == 6), "unknown test mode");
      const auto capacity = argc == 6 ? std::stoull(argv[5]) : kCapacity;
      const auto handle = inheritedHandle(argv[2]);
      const auto source = std::filesystem::u8path(argv[3]);
      if (mode == "iso") inspectIso(source);
      if (action == "--write") writeImage(handle, source, mode == "quick", capacity);
      else if (action == "--verify") verifyReopened(handle, source, mode == "quick", capacity);
      else throw std::runtime_error("unknown helper action");
      return 0;
    }
    throw std::runtime_error("Use virtual_disk_integration.py; this helper accepts only inherited handles");
  } catch (const std::exception& error) {
    std::cerr << "Virtual-disk test failed: " << error.what() << '\n';
    return 1;
  }
}
