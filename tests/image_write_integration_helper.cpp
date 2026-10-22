/* SPDX-License-Identifier: GPL-3.0-or-later */
// Test-only: writes exclusively created ordinary files, never device handles.
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "system_raw_io.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/iso_deployment.hpp"
#include "rufus/core/standalone_media.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace {
using namespace rufus::core;
using namespace rufus::backend::detail;
constexpr std::uint64_t kMaximumCapacity = 16ULL * 1024U * 1024U * 1024U;

void check(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::uint64_t number(const char* value) {
  const std::string text(value);
  check(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos,
        "expected an unsigned decimal number");
  return std::stoull(text);
}

void checkPath(const std::filesystem::path& path) {
  const auto text = std::filesystem::absolute(path).generic_u8string();
  check(text.rfind("/dev/", 0U) != 0U && text.rfind("//", 0U) != 0U &&
            text.find("/../") == std::string::npos,
        "device and namespace paths are forbidden");
  check(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
        "destination must not already exist");
  const auto parent = std::filesystem::canonical(path.parent_path());
  check(parent.generic_u8string().rfind("/dev", 0U) != 0U,
        "device directories are forbidden");
}

BlockDeviceInfo descriptor(const std::filesystem::path& path,
                           const std::uint64_t capacity, const std::uint32_t sector) {
  BlockDeviceInfo target;
  // Portable copy-plan metadata only; native discovery eligibility is untouched.
  target.stableId = "test:exclusive-regular-file";
  target.devicePath = path.u8string();
  target.capacityBytes = capacity;
  target.logicalSectorSize = sector;
  target.bus = DeviceBus::Virtual;
  target.removable = target.writable = target.wholeDevice = true;
  return target;
}

class NewFile final {
 public:
  explicit NewFile(const std::filesystem::path& path) {
    checkPath(path);
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    check(handle_ != INVALID_HANDLE_VALUE, "exclusive creation of regular target failed");
    if (GetFileType(handle_) != FILE_TYPE_DISK) {
      CloseHandle(handle_);
      throw std::runtime_error("target is not a disk file");
    }
#else
    handle_ = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(handle_ >= 0, "exclusive creation of regular target failed");
    struct stat status {};
    if (fstat(handle_, &status) != 0 || !S_ISREG(status.st_mode)) {
      close(handle_);
      throw std::runtime_error("target is not a regular file");
    }
#endif
  }
  ~NewFile() {
#if defined(_WIN32)
    CloseHandle(handle_);
#else
    close(handle_);
#endif
  }
  NewFile(const NewFile&) = delete;
  NewFile& operator=(const NewFile&) = delete;
  NativeHandle handle() const { return handle_; }
 private:
  NativeHandle handle_;
};

class RegularFileTarget final : public RawTargetIo {
 public:
  RegularFileTarget(const NativeHandle handle, const std::uint64_t capacity,
                     const std::uint32_t sector)
      : io_(handle), transfers_(io_, capacity, sector) {}
  std::uint64_t capacityBytes() const noexcept override { return transfers_.capacityBytes(); }
  std::uint32_t logicalSectorSize() const noexcept override { return transfers_.logicalSectorSize(); }
  bool writeAt(const std::uint64_t offset, const unsigned char* data, const std::size_t size,
               std::string& error) override {
    const bool success = transfers_.writeAt(offset, data, size, error);
    if (success) bytesWritten += size;
    return success;
  }
  bool readAt(const std::uint64_t offset, unsigned char* data, const std::size_t size,
              std::string& error) override {
    const bool success = transfers_.readAt(offset, data, size, error);
    if (success) bytesRead += size;
    return success;
  }
  bool flush(std::string& error) override {
    // Real fsync/FlushFileBuffers. Device-cache ioctls require a block device and
    // are intentionally covered by the separate opt-in virtual-device test.
    NativeIoResult result;
    do { result = io_.syncFile(); } while (!result.success && result.interrupted);
    error = result.error;
    if (result.success) ++flushCount;
    return result.success;
  }
  std::size_t flushCount{};
  std::uint64_t bytesWritten{}, bytesRead{};
 private:
  SystemNativeIo io_;
  NativeRawTarget transfers_;
};

void writeImage(const std::filesystem::path& source, const std::filesystem::path& destination,
                 const std::uint64_t capacity, const std::uint64_t sector,
                 const std::uint64_t blockSize, const bool clearTail,
                 std::vector<RawWriteRange> ranges = {}) {
  check(capacity > 0U && capacity <= kMaximumCapacity &&
            (sector == 512U || sector == 4096U) && capacity % sector == 0U,
        "test capacity/sector limit exceeded");
  check(blockSize >= sector && blockSize <= 1024U * 1024U && blockSize % sector == 0U,
        "invalid test block size");
  check(std::filesystem::is_regular_file(source), "source must be a regular image file");
  check(std::filesystem::file_size(source) <= capacity, "image exceeds target capacity");
  checkPath(destination);
  const auto analysis = ImageAnalyzer{}.analyze(source);
  check(analysis.succeeded(), "analyze source image");
  const auto plan = WritePlanBuilder{}.buildRawWriteWithVerification(
      *analysis.image, descriptor(destination, capacity, static_cast<std::uint32_t>(sector)),
      analysis.image->deploymentSizeBytes() == capacity && ranges.empty() ? VerificationProfile::Full
                                                       : VerificationProfile::Standard,
      static_cast<std::size_t>(blockSize), clearTail, std::move(ranges));
  check(plan.succeeded(), "build regular-file image test plan");
  NewFile file(destination);
  RegularFileTarget target(file.handle(), capacity, static_cast<std::uint32_t>(sector));
  std::array<unsigned char, 64U * 1024U> sentinel{};
  sentinel.fill(0xa5U);
  std::string error;
  for (std::uint64_t offset = 0U; offset < capacity; offset += sentinel.size()) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(sentinel.size(), capacity - offset));
    check(target.writeAt(offset, sentinel.data(), count, error), error);
  }
  check(target.flush(error), error);
  target.bytesWritten = target.bytesRead = 0U;
  const auto result = RawImageWriter{}.write(*plan.plan, target);
  check(result.success && result.verificationCompleted && result.destructiveWriteStarted, result.error);
  check(result.bytesWritten == plan.plan->bytesToWrite() &&
            target.bytesWritten == result.bytesWritten + (clearTail ? 1024U * 1024U : 0U) &&
            target.bytesRead == result.bytesVerified,
        "incomplete transfer or inconsistent actual native I/O count");
  check(target.flushCount >= 2U, "writer must perform a real file flush");
  std::cout << "{\"success\":true,\"bytesWritten\":" << result.bytesWritten
            << ",\"bytesVerified\":" << result.bytesVerified
            << ",\"nativeBytesWritten\":" << target.bytesWritten
            << ",\"nativeBytesRead\":" << target.bytesRead
            << ",\"verificationCompleted\":true,\"flushMode\":\"regular-file-fsync\"}\n";
}

void stagePersistence(const std::filesystem::path& source,
                      const std::filesystem::path& destination,
                      const std::uint64_t capacity, const std::uint64_t persistence,
                      const PartitionScheme scheme = PartitionScheme::Mbr,
                      const IsoTargetSystem targetSystem = IsoTargetSystem::Uefi) {
  check(capacity >= 2ULL * 1024U * 1024U * 1024U && capacity <= kMaximumCapacity &&
            capacity % 512U == 0U && persistence >= 256U * 1024U * 1024U &&
            persistence < capacity, "invalid bounded persistence fixture capacity");
  check(std::filesystem::is_regular_file(source), "ISO must be an ordinary source file");
  checkPath(destination);
  const auto analysis = ImageAnalyzer{}.analyze(source);
  check(analysis.succeeded(), "analyze persistence ISO");
  check(analysis.image->capabilities.linuxPersistence, "ISO does not advertise validated persistence");
  IsoDeploymentOptions options;
  options.targetSystem = targetSystem;
  options.partitionScheme = scheme;
  const auto planned = IsoDeploymentPlanner{createSystemWimSplitter(), options}.build(
      *analysis.image, descriptor(destination, capacity, 512U), "PERSIST_TEST",
      LinuxPersistenceOptions{persistence});
  if (!planned.succeeded()) {
    for (const auto& issue : planned.issues) std::cerr << issue.message << '\n';
    check(false, "persistence deployment planning failed");
  }
  check((targetSystem != IsoTargetSystem::Bios && targetSystem != IsoTargetSystem::BiosAndUefi) ||
            (planned.plan->biosBootable() &&
             planned.plan->legacyBiosBootstrap() == LegacyBiosBootstrap::Grub2),
        "BIOS qualification requires the production GRUB2 bootstrap");
  IsoDeploymentStage previous = IsoDeploymentStage::Planning;
  const auto staged = IsoImageStager{}.stage(*planned.plan, destination,
      [&](const IsoDeploymentProgress& progress) {
        if (progress.stage != previous) {
          std::cerr << isoDeploymentStageName(progress.stage) << '\n';
          previous = progress.stage;
        }
      });
  check(staged.success && staged.stagedImage.has_value(), staged.error);
  check(std::filesystem::file_size(destination) == capacity, "staged persistence image is incomplete");
  auto commitTarget = planned.plan->target();
  // The staging descriptor names its output file, not the eventual target.
  commitTarget.devicePath = destination.u8string() + ".commit-target";
  const auto commit = WritePlanBuilder{}.buildRawWriteWithVerification(
      *staged.stagedImage, commitTarget, VerificationProfile::Standard,
      4U * 1024U * 1024U, planned.plan->requiresTrailingMetadataClear(), staged.writeRanges);
  if (!commit.succeeded()) {
    for (const auto& issue : commit.issues) std::cerr << issue.message << '\n';
  }
  check(commit.succeeded() && !commit.plan->clearTargetTailMetadata(),
        "persistence must satisfy the GUI's native commit contract without tail clearing");
  std::cout << "{\"success\":true,\"capacityBytes\":" << capacity
            << ",\"persistenceBytes\":" << planned.plan->linuxPersistenceBytes()
            << ",\"filesExtracted\":" << staged.filesExtracted
            << ",\"biosBootable\":" << (planned.plan->biosBootable() ? "true" : "false")
            << ",\"uefiBootable\":" << (staged.stagedImage->capabilities.uefiBootable ? "true" : "false")
            << ",\"targetSystem\":\"" << (targetSystem == IsoTargetSystem::BiosAndUefi ? "BIOS+UEFI"
                                            : targetSystem == IsoTargetSystem::Bios ? "BIOS" : "UEFI") << '"'
            << ",\"style\":\"" << linuxPersistenceStyleName(planned.plan->linuxPersistenceStyle())
            << "\",\"bytesToWrite\":" << commit.plan->bytesToWrite()
            << ",\"writeRanges\":[";
  bool first = true;
  for (const auto& range : staged.writeRanges) {
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"offset\":" << range.offsetBytes << ",\"size\":" << range.sizeBytes << '}';
  }
  std::cout << "]}\n";
}
}  // namespace

int main(const int argc, const char* const argv[]) {
  try {
    if (argc >= 10 && std::string(argv[1]) == "--write-ranges") {
      check((argc - 8) % 2 == 0 && static_cast<std::size_t>((argc - 8) / 2) <= kMaximumRawWriteRanges,
            "expected bounded offset/size pairs");
      std::vector<RawWriteRange> ranges;
      for (int index = 8; index < argc; index += 2) {
        ranges.push_back({number(argv[index]), number(argv[index + 1])});
      }
      check(std::string(argv[7]) == "0", "range writes cannot request a second tail clearing operation");
      writeImage(std::filesystem::absolute(std::filesystem::u8path(argv[2])),
                 std::filesystem::absolute(std::filesystem::u8path(argv[3])),
                 number(argv[4]), number(argv[5]), number(argv[6]), false, std::move(ranges));
      return 0;
    }
    if (argc == 6 && (std::string(argv[1]) == "--stage-persistence" ||
                      std::string(argv[1]) == "--stage-persistence-gpt" ||
                      std::string(argv[1]) == "--stage-persistence-bios" ||
                      std::string(argv[1]) == "--stage-persistence-dual")) {
      stagePersistence(std::filesystem::absolute(std::filesystem::u8path(argv[2])),
                       std::filesystem::absolute(std::filesystem::u8path(argv[3])),
                       number(argv[4]), number(argv[5]),
                       std::string(argv[1]) == "--stage-persistence-gpt"
                           ? PartitionScheme::Gpt : PartitionScheme::Mbr,
                       std::string(argv[1]) == "--stage-persistence-bios"
                           ? IsoTargetSystem::Bios
                           : std::string(argv[1]) == "--stage-persistence-dual"
                               ? IsoTargetSystem::BiosAndUefi : IsoTargetSystem::Uefi);
      return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "--stage") {
      const std::string filesystem(argv[2]), mode(argv[3]);
      const auto capacity = number(argv[4]);
      const auto source = std::filesystem::absolute(std::filesystem::u8path(argv[5]));
      check(capacity <= 256U * 1024U * 1024U, "format-fixture capacity limit exceeded");
      check(mode == "quick" || mode == "full", "unknown format mode");
      check(filesystem == "fat16" || filesystem == "fat32", "unknown filesystem");
      checkPath(source);
      StandaloneFormatOptions options;
      options.quickFormat = mode == "quick";
      const auto target = descriptor(source, capacity, 512U);
      const auto staged = filesystem == "fat16"
          ? stageBlankFat16Media(target, source, "RUFUSTEST", {}, {}, options)
          : stageBlankFat32Media(target, source, "RUFUSTEST", {}, {}, options);
      check(staged.success, staged.error);
      return 0;
    }
    if (argc == 8 && std::string(argv[1]) == "--write") {
      check(std::string(argv[7]) == "0" || std::string(argv[7]) == "1", "invalid tail option");
      writeImage(std::filesystem::absolute(std::filesystem::u8path(argv[2])),
                 std::filesystem::absolute(std::filesystem::u8path(argv[3])),
                 number(argv[4]), number(argv[5]), number(argv[6]), std::string(argv[7]) == "1");
      return 0;
    }
    throw std::runtime_error("Use image_write_integration.py to create private test files");
  } catch (const std::exception& error) {
    std::cerr << "Image-file test failed: " << error.what() << '\n';
    return 1;
  }
}
