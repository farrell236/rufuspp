/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/backend/block_device_backend.hpp"

#include <fcntl.h>
// glibc's mount header must precede linux/fs.h so linux/libc-compat.h can
// suppress the kernel MS_* definitions that would collide with its enum.
#include <sys/mount.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rufus/core/image_analyzer.hpp"
#include "system_raw_io.hpp"
#include "raw_write_safety.hpp"

namespace rufus::backend {

namespace {

const std::filesystem::path kSysBlock{"/sys/class/block"};

class LinuxBlockDeviceBackend final : public BlockDeviceBackend {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "Linux sysfs";
  }

  [[nodiscard]] BackendCapabilities capabilities() const noexcept override {
    BackendCapabilities result;
    result.physicalDeviceDiscovery = true;
    result.readOnlyInspection = true;
    result.unmountVolumes = true;
    result.exclusiveAccess = true;
    result.rawWrite = true;
    result.flush = true;
    result.identityRevalidation = true;
    result.rawVerification = true;
    result.mediaCapture = true;
    result.badBlockTest = true;
    return result;
  }

  [[nodiscard]] PrivilegeStatus privilegeStatus() const override;

  [[nodiscard]] DeviceDiscoveryResult discover() const override;
  [[nodiscard]] RawWriteAvailability rawWriteAvailability(
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] core::RawWriteResult writeRaw(
      const core::RawWritePlan& plan,
      const core::RawWriteProgressCallback& onProgress = {},
      const core::RawWriteCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] RawWriteAvailability captureAvailability(
      const core::BlockDeviceInfo& target,
      core::MediaCaptureFormat format) const override;
  [[nodiscard]] core::MediaCaptureResult capture(
      const core::BlockDeviceInfo& target,
      const std::filesystem::path& destination,
      const core::MediaCaptureOptions& options,
      const core::MediaCaptureProgressCallback& onProgress = {},
      const core::MediaCaptureCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] RawWriteAvailability badBlockTestAvailability(
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] core::BadBlockTestResult testBadBlocks(
      const core::BlockDeviceInfo& target,
      const core::BadBlockTestOptions& options,
      const core::BadBlockTestProgressCallback& onProgress = {},
      const core::BadBlockTestCancelCallback& isCancelled = {}) const override;
};

class FileDescriptor final {
 public:
  explicit FileDescriptor(const int descriptor = -1) : descriptor_(descriptor) {}
  ~FileDescriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : descriptor_(other.descriptor_) {
    other.descriptor_ = -1;
  }
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0) {
        close(descriptor_);
      }
      descriptor_ = other.descriptor_;
      other.descriptor_ = -1;
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

 private:
  int descriptor_;
};

std::string errorMessage(const std::string_view action, const int error = errno) {
  return std::string(action) + ": " + std::strerror(error);
}

void report(const core::RawWriteProgressCallback& callback,
            const core::RawWriteStage stage, const std::uint64_t processed,
            const std::uint64_t total) {
  if (callback) {
    callback({stage, processed, total});
  }
}

bool cancelled(const core::RawWriteCancelCallback& callback) {
  return callback && callback();
}

using detail::positionalRead;
using LinuxRawTarget = detail::SystemRawTarget;

class LinuxRawSource final : public core::RawSourceIo {
 public:
  LinuxRawSource(const int descriptor, const std::uint64_t capacity)
      : descriptor_(descriptor), capacity_(capacity) {}

  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override {
    return capacity_;
  }

  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    return positionalRead(descriptor_, offset, data, size, error);
  }

 private:
  int descriptor_;
  std::uint64_t capacity_;
};

std::string trim(std::string value) {
  const auto notSpace = [](const unsigned char character) { return !std::isspace(character); };
  const auto first = std::find_if(value.begin(), value.end(), notSpace);
  const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
  return first < last ? std::string(first, last) : std::string{};
}

std::string decodeMountInfoPath(const std::string_view encoded) {
  std::string result;
  result.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    if (encoded[index] == '\\' && index + 3 < encoded.size() &&
        encoded[index + 1] >= '0' && encoded[index + 1] <= '7' &&
        encoded[index + 2] >= '0' && encoded[index + 2] <= '7' &&
        encoded[index + 3] >= '0' && encoded[index + 3] <= '7') {
      const unsigned int value =
          static_cast<unsigned int>(encoded[index + 1] - '0') * 64U +
          static_cast<unsigned int>(encoded[index + 2] - '0') * 8U +
          static_cast<unsigned int>(encoded[index + 3] - '0');
      result.push_back(static_cast<char>(value));
      index += 3;
    } else {
      result.push_back(encoded[index]);
    }
  }
  return result;
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::string value;
  std::getline(stream, value);
  return stream ? trim(value) : std::string{};
}

std::uint64_t readNumber(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::uint64_t value = 0;
  stream >> value;
  return stream ? value : 0;
}

bool hasVirtualPrefix(const std::string& name) {
  constexpr std::string_view prefixes[] = {"loop", "ram", "zram", "dm-", "md"};
  return std::any_of(std::begin(prefixes), std::end(prefixes),
                     [&name](const std::string_view prefix) {
                       return name.rfind(prefix, 0) == 0;
                     });
}

std::string blockNameFromDevicePath(const std::string& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  return error ? std::filesystem::path(path).filename().string()
               : canonical.filename().string();
}

std::set<std::string> underlyingWholeDevices(const std::string& name) {
  std::set<std::string> result;
  const auto sysPath = kSysBlock / name;
  std::error_code error;

  if (std::filesystem::exists(sysPath / "partition", error) && !error) {
    const auto canonical = std::filesystem::canonical(sysPath, error);
    if (!error) {
      result.insert(canonical.parent_path().filename().string());
    }
    return result;
  }

  const auto slaves = sysPath / "slaves";
  if (std::filesystem::is_directory(slaves, error) && !error) {
    for (const auto& slave : std::filesystem::directory_iterator(slaves, error)) {
      if (error) {
        break;
      }
      const auto nested = underlyingWholeDevices(slave.path().filename().string());
      result.insert(nested.begin(), nested.end());
    }
  }

  if (result.empty()) {
    result.insert(name);
  }
  return result;
}

struct MountState final {
  std::map<std::string, std::vector<std::string>> mountPoints;
  std::set<std::string> rootDevices;
};

MountState readMountState() {
  MountState result;
  std::ifstream stream("/proc/self/mountinfo");
  std::string line;
  while (std::getline(stream, line)) {
    std::istringstream fields(line);
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token) {
      tokens.push_back(token);
    }
    const auto separator = std::find(tokens.begin(), tokens.end(), "-");
    if (tokens.size() < 6 || separator == tokens.end() || separator + 2 >= tokens.end()) {
      continue;
    }

    const std::string mountPoint = decodeMountInfoPath(tokens[4]);
    const std::string source = decodeMountInfoPath(*(separator + 2));
    if (source.rfind("/dev/", 0) != 0) {
      continue;
    }

    const std::string blockName = blockNameFromDevicePath(source);
    for (const auto& whole : underlyingWholeDevices(blockName)) {
      result.mountPoints[whole].push_back(mountPoint);
      if (mountPoint == "/") {
        result.rootDevices.insert(whole);
      }
    }
  }
  return result;
}

std::string blockNameFromDeviceNumber(const dev_t device) {
  const std::filesystem::path sysPath =
      std::filesystem::path("/sys/dev/block") /
      (std::to_string(major(device)) + ':' + std::to_string(minor(device)));
  std::error_code error;
  const auto canonical = std::filesystem::canonical(sysPath, error);
  return error ? std::string{} : canonical.filename().string();
}

bool sourceUsesTarget(const int sourceDescriptor, const std::string& targetPath) {
  struct stat sourceStatus {};
  if (fstat(sourceDescriptor, &sourceStatus) != 0) {
    return true;
  }
  const std::string sourceBlock = blockNameFromDeviceNumber(sourceStatus.st_dev);
  if (sourceBlock.empty()) {
    return false;
  }
  const std::string targetBlock = blockNameFromDevicePath(targetPath);
  const auto sourceDevices = underlyingWholeDevices(sourceBlock);
  return sourceDevices.count(targetBlock) != 0;
}

bool isWholeBlockDevice(const std::string& path, std::string& reason) {
  struct stat status {};
  if (stat(path.c_str(), &status) != 0) {
    reason = errorMessage("Unable to inspect the target block device");
    return false;
  }
  if (!S_ISBLK(status.st_mode)) {
    reason = "The selected target is not a Linux block device";
    return false;
  }
  const std::string name = blockNameFromDevicePath(path);
  std::error_code error;
  if (name.empty() || std::filesystem::exists(kSysBlock / name / "partition", error)) {
    reason = "The selected target is not a whole Linux block device";
    return false;
  }
  return true;
}

bool unmountVolumes(std::vector<std::string> mountPoints, std::string& error) {
  std::sort(mountPoints.begin(), mountPoints.end(),
            [](const std::string& left, const std::string& right) {
              if (left.size() != right.size()) {
                return left.size() > right.size();
              }
              return left > right;
            });
  mountPoints.erase(std::unique(mountPoints.begin(), mountPoints.end()),
                    mountPoints.end());
  for (const auto& mountPoint : mountPoints) {
    if (umount2(mountPoint.c_str(), 0) != 0 && errno != EINVAL) {
      error = errorMessage("Unable to unmount " + mountPoint);
      return false;
    }
  }
  return true;
}

std::filesystem::path findCaptureExecutable(const std::string_view name) {
  constexpr std::array<std::string_view, 6> directories{
      "/usr/local/bin", "/usr/local/sbin", "/usr/bin", "/usr/sbin",
      "/bin", "/sbin"};
  for (const auto directory : directories) {
    const auto candidate = std::filesystem::path(directory) / name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path udfCaptureProvider() {
  for (const std::string_view name : {std::string_view("genisoimage"),
                                      std::string_view("mkisofs")}) {
    const auto executable = findCaptureExecutable(name);
    if (!executable.empty()) {
      return executable;
    }
  }
  return {};
}

struct CaptureCommandResult final {
  int exitCode{-1};
  bool cancelled{};
};

CaptureCommandResult runCaptureCommand(
    const std::filesystem::path& executable,
    std::vector<std::string> arguments,
    const core::MediaCaptureCancelCallback& isCancelled) {
  CaptureCommandResult result;
  if (executable.empty() || arguments.empty()) {
    return result;
  }
  const pid_t child = fork();
  if (child == 0) {
    static_cast<void>(setpgid(0, 0));
    const int nullOutput = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nullOutput >= 0) {
      static_cast<void>(dup2(nullOutput, STDOUT_FILENO));
      static_cast<void>(dup2(nullOutput, STDERR_FILENO));
      close(nullOutput);
    }
    std::vector<char*> native;
    native.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) {
      native.push_back(argument.data());
    }
    native.push_back(nullptr);
    execv(executable.c_str(), native.data());
    _exit(127);
  }
  if (child < 0) {
    return result;
  }
  static_cast<void>(setpgid(child, child));
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
      }
      return result;
    }
    if (waited < 0 && errno != EINTR) {
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      static_cast<void>(kill(-child, SIGTERM));
      static_cast<void>(kill(child, SIGTERM));
      for (unsigned int attempt = 0; attempt < 20U; ++attempt) {
        if (waitpid(child, &status, WNOHANG) == child) {
          return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      static_cast<void>(kill(-child, SIGKILL));
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

core::MediaCaptureResult captureMountedVolumeAsUdf(
    const LinuxBlockDeviceBackend& backend,
    const core::BlockDeviceInfo& selected,
    const std::filesystem::path& destination,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  if (selected.mountPoints.size() != 1U || udfCaptureProvider().empty()) {
    result.error =
        "UDF ISO capture requires one mounted source volume and genisoimage or mkisofs";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(destination, fileError) || fileError) {
    result.error = fileError
                       ? "Unable to inspect the UDF capture destination: " +
                             fileError.message()
                       : "The UDF capture destination already exists";
    return result;
  }
  const auto parent = destination.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : destination.parent_path();
  FileDescriptor destinationDirectory(
      fileError ? -1 : open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!destinationDirectory.valid() ||
      sourceUsesTarget(destinationDirectory.get(), selected.devicePath)) {
    result.error = "The UDF destination is unavailable or stored on the source device";
    return result;
  }
  const auto discovery = backend.discover();
  const auto observed = std::find_if(
      discovery.devices.begin(), discovery.devices.end(),
      [&selected](const core::BlockDeviceInfo& device) {
        return device.devicePath == selected.devicePath;
      });
  if (observed == discovery.devices.end() ||
      observed->mountPoints != selected.mountPoints) {
    result.error = "The mounted UDF source volume changed after selection";
    return result;
  }
  const core::SafetyPolicy policy;
  const auto identity = policy.validateIdentity(selected, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }
  auto partial = parent /
      (destination.stem().string() + ".rufus-plus-plus-capture-" +
       std::to_string(static_cast<unsigned long long>(getpid())) +
       destination.extension().string());
  if (std::filesystem::exists(partial, fileError) || fileError) {
    result.error = "Unable to allocate a private UDF capture path";
    return result;
  }
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
  };
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Capturing, 0U,
                selected.capacityBytes});
  }
  const auto executable = udfCaptureProvider();
  const auto command = runCaptureCommand(
      executable,
      {executable.filename().string(), "-udf", "-iso-level", "3", "-V",
       "RUFUSPP_CAPTURE", "-o", partial.string(), selected.mountPoints.front()},
      isCancelled);
  if (command.exitCode != 0) {
    result.cancelled = command.cancelled;
    result.error = command.cancelled ? "UDF ISO capture cancelled"
                                     : "The Linux UDF ISO provider failed";
    cleanup();
    return result;
  }
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Verifying, selected.capacityBytes,
                selected.capacityBytes});
  }
  const core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(partial);
  if (!analysis.succeeded() || analysis.image->format != core::ImageFormat::Iso ||
      !analysis.image->capabilities.udf ||
      !analysis.image->capabilities.isoExtraction) {
    result.error =
        "The generated UDF ISO failed structural and directory-tree validation";
    cleanup();
    return result;
  }
  const auto outputBytes = std::filesystem::file_size(partial, fileError);
  if (fileError || outputBytes == 0U) {
    result.error = "The Linux UDF provider produced no readable image";
    cleanup();
    return result;
  }
  std::filesystem::rename(partial, destination, fileError);
  if (fileError) {
    result.error = "Unable to commit the UDF ISO capture: " +
                   fileError.message();
    cleanup();
    return result;
  }
  result.success = true;
  result.bytesCaptured = selected.capacityBytes;
  result.outputSizeBytes = outputBytes;
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Complete, selected.capacityBytes,
                selected.capacityBytes});
  }
  return result;
}

core::DeviceBus detectBus(const std::string& name, const std::filesystem::path& sysPath) {
  std::error_code error;
  std::string hierarchy = std::filesystem::canonical(sysPath, error).string();
  std::transform(hierarchy.begin(), hierarchy.end(), hierarchy.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (hierarchy.find("/usb") != std::string::npos) {
    return core::DeviceBus::Usb;
  }
  if (name.rfind("mmcblk", 0) == 0) {
    return core::DeviceBus::Sd;
  }
  if (name.rfind("nvme", 0) == 0) {
    return core::DeviceBus::Nvme;
  }
  if (hierarchy.find("/ata") != std::string::npos) {
    return core::DeviceBus::Sata;
  }
  if (hierarchy.find("/virtual/") != std::string::npos) {
    return core::DeviceBus::Virtual;
  }
  return core::DeviceBus::Unknown;
}

DeviceDiscoveryResult LinuxBlockDeviceBackend::discover() const {
  DeviceDiscoveryResult result;
  std::error_code error;
  if (!std::filesystem::is_directory(kSysBlock, error) || error) {
    result.warnings.emplace_back("Linux sysfs block-device information is unavailable");
    return result;
  }

  const MountState mounts = readMountState();
  for (const auto& entry : std::filesystem::directory_iterator(kSysBlock, error)) {
    if (error) {
      result.warnings.emplace_back("Unable to enumerate every sysfs block device");
      break;
    }

    const std::string name = entry.path().filename().string();
    const auto sysPath = kSysBlock / name;
    if (hasVirtualPrefix(name) || std::filesystem::exists(sysPath / "partition", error)) {
      error.clear();
      continue;
    }

    core::BlockDeviceInfo device;
    device.devicePath = "/dev/" + name;
    device.vendor = readText(sysPath / "device/vendor");
    device.model = readText(sysPath / "device/model");
    device.serialNumber = readText(sysPath / "device/serial");
    if (device.serialNumber.empty()) {
      device.serialNumber = readText(sysPath / "wwid");
    }
    if (device.serialNumber.empty()) {
      device.serialNumber = readText(sysPath / "device/wwid");
    }
    device.displayName = trim(device.vendor + " " + device.model);
    if (device.displayName.empty()) {
      device.displayName = name;
    }
    device.capacityBytes = readNumber(sysPath / "size") * 512U;
    device.logicalSectorSize =
        static_cast<std::uint32_t>(readNumber(sysPath / "queue/logical_block_size"));
    device.bus = detectBus(name, sysPath);
    device.removable = readNumber(sysPath / "removable") != 0;
    device.ejectable = device.removable || device.bus == core::DeviceBus::Usb ||
                       device.bus == core::DeviceBus::Sd;
    device.writable = readNumber(sysPath / "ro") == 0;
    device.systemDevice = mounts.rootDevices.count(name) != 0;
    device.wholeDevice = true;
    if (const auto found = mounts.mountPoints.find(name); found != mounts.mountPoints.end()) {
      device.mountPoints = found->second;
    }
    device.stableId = "linux:" +
                      (device.serialNumber.empty() ? name : device.serialNumber) + ':' +
                      std::to_string(device.capacityBytes);
    result.devices.push_back(std::move(device));
  }

  std::sort(result.devices.begin(), result.devices.end(),
            [](const core::BlockDeviceInfo& left, const core::BlockDeviceInfo& right) {
              return left.devicePath < right.devicePath;
            });
  return result;
}

PrivilegeStatus LinuxBlockDeviceBackend::privilegeStatus() const {
  if (geteuid() == 0) {
    return {PrivilegeRoute::Elevated,
            "The complete application is running as root; guarded physical-device operations are enabled"};
  }
  return {PrivilegeRoute::Unprivileged,
          "The application is not running as root; launch it through a trusted administrator mechanism to enable physical-device operations"};
}

RawWriteAvailability LinuxBlockDeviceBackend::rawWriteAvailability(
    const core::BlockDeviceInfo& target) const {
  const auto eligibility = core::evaluateDeviceEligibility(target);
  if (eligibility != core::DeviceEligibility::Eligible) {
    return {false, false, "Target is not eligible: " +
                       std::string(core::deviceEligibilityName(eligibility))};
  }

  std::string reason;
  if (!isWholeBlockDevice(target.devicePath, reason)) {
    return {false, false, std::move(reason)};
  }
  if (geteuid() != 0) {
    return {false, false,
            "Administrative block-device access is required. Start Rufus++ through a trusted "
            "system administrator launcher; an integrated Linux helper is not installed yet."};
  }
  if (access(target.devicePath.c_str(), R_OK | W_OK) != 0) {
    return {false, false, errorMessage("The target cannot be opened for raw read/write access")};
  }
  return {true, false, "Administrative raw-device access is available"};
}

RawWriteAvailability LinuxBlockDeviceBackend::captureAvailability(
    const core::BlockDeviceInfo& target,
    const core::MediaCaptureFormat format) const {
  if (target.stableId.empty() || target.devicePath.empty() ||
      !target.wholeDevice || target.systemDevice || !target.removable) {
    return {false, false,
            "Capture requires an identified, whole, removable, non-system device"};
  }
  if (format == core::MediaCaptureFormat::Ffu) {
    return {false, false,
            "FFU capture requires the Windows DISM servicing provider"};
  }
  if (format == core::MediaCaptureFormat::UdfIso) {
    std::string reason;
    if (!isWholeBlockDevice(target.devicePath, reason)) {
      return {false, false, std::move(reason)};
    }
    if (target.mountPoints.size() != 1U) {
      return {false, false,
              "UDF ISO capture requires exactly one mounted source volume"};
    }
    if (access(target.mountPoints.front().c_str(), R_OK | X_OK) != 0) {
      return {false, false,
              errorMessage("The mounted UDF source volume is not readable")};
    }
    if (udfCaptureProvider().empty()) {
      return {false, false,
              "UDF ISO capture requires installed genisoimage or mkisofs tooling"};
    }
    return {true, false, "The Linux filesystem-aware UDF provider is available"};
  }
  constexpr std::uint64_t maximumVhdBytes = 2040ULL * 1024ULL * 1024ULL * 1024ULL;
  if ((format == core::MediaCaptureFormat::FixedVhd ||
       format == core::MediaCaptureFormat::DynamicVhd) &&
      target.capacityBytes > maximumVhdBytes) {
    return {false, false, "Classic VHD capture is limited to 2040 GiB"};
  }
  std::string reason;
  if (!isWholeBlockDevice(target.devicePath, reason)) {
    return {false, false, std::move(reason)};
  }
  if (geteuid() != 0) {
    return {false, false,
            "Administrative block-device access is required. Start Rufus++ through a trusted system administrator launcher."};
  }
  if (access(target.devicePath.c_str(), R_OK) != 0) {
    return {false, false,
            errorMessage("The source device cannot be opened for raw reading")};
  }
  return {true, false, "Administrative capture access is available"};
}

core::MediaCaptureResult LinuxBlockDeviceBackend::capture(
    const core::BlockDeviceInfo& selected,
    const std::filesystem::path& destination,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) const {
  core::MediaCaptureResult result;
  const auto ready = captureAvailability(selected, options.format);
  if (!ready.available) {
    result.error = ready.reason;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Capture cancelled before the device was opened";
    return result;
  }

  if (options.format == core::MediaCaptureFormat::UdfIso) {
    return captureMountedVolumeAsUdf(*this, selected, destination, onProgress,
                                     isCancelled);
  }

  std::error_code pathError;
  const auto parent = destination.parent_path().empty()
                          ? std::filesystem::current_path(pathError)
                          : destination.parent_path();
  if (pathError) {
    result.error = "Unable to resolve the capture destination directory: " +
                   pathError.message();
    return result;
  }
  FileDescriptor destinationDirectory(
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!destinationDirectory.valid()) {
    result.error = errorMessage("Unable to open the capture destination directory");
    return result;
  }
  if (sourceUsesTarget(destinationDirectory.get(), selected.devicePath)) {
    result.error = "The capture destination is stored on the device being captured";
    return result;
  }

  const DeviceDiscoveryResult beforeUnmount = discover();
  const auto observed = std::find_if(
      beforeUnmount.devices.begin(), beforeUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (observed == beforeUnmount.devices.end()) {
    result.error = "The selected capture device is no longer connected";
    return result;
  }
  const core::SafetyPolicy safetyPolicy;
  const auto identity = safetyPolicy.validateIdentity(selected, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }
  if (!unmountVolumes(observed->mountPoints, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Capture cancelled after unmounting the device";
    return result;
  }

  const DeviceDiscoveryResult afterUnmount = discover();
  const auto reobserved = std::find_if(
      afterUnmount.devices.begin(), afterUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (reobserved == afterUnmount.devices.end()) {
    result.error = "The capture device disappeared after unmounting";
    return result;
  }
  const auto finalIdentity = safetyPolicy.validateIdentity(selected, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  FileDescriptor sourceDescriptor(
      open(selected.devicePath.c_str(), O_RDONLY | O_EXCL | O_CLOEXEC));
  if (!sourceDescriptor.valid()) {
    result.error = errorMessage("Unable to acquire exclusive capture access");
    return result;
  }
  struct stat sourceStatus {};
  std::uint64_t capacity = 0;
  int sectorSize = 0;
  if (fstat(sourceDescriptor.get(), &sourceStatus) != 0 ||
      !S_ISBLK(sourceStatus.st_mode) ||
      ioctl(sourceDescriptor.get(), BLKGETSIZE64, &capacity) != 0 ||
      ioctl(sourceDescriptor.get(), BLKSSZGET, &sectorSize) != 0 ||
      capacity != selected.capacityBytes || sectorSize <= 0 ||
      static_cast<std::uint32_t>(sectorSize) != selected.logicalSectorSize) {
    result.error = "Capture-device identity or geometry changed before reading";
    return result;
  }

  LinuxRawSource source(sourceDescriptor.get(), capacity);
  const core::MediaCaptureWriter writer;
  return writer.capture(source, destination, options, onProgress, isCancelled);
}

RawWriteAvailability LinuxBlockDeviceBackend::badBlockTestAvailability(
    const core::BlockDeviceInfo& target) const {
  return rawWriteAvailability(target);
}

core::BadBlockTestResult LinuxBlockDeviceBackend::testBadBlocks(
    const core::BlockDeviceInfo& selected,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled) const {
  core::BadBlockTestResult result;
  const auto ready = badBlockTestAvailability(selected);
  if (!ready.available) {
    result.error = ready.reason;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled before the target was modified";
    return result;
  }

  const DeviceDiscoveryResult beforeUnmount = discover();
  const auto observed = std::find_if(
      beforeUnmount.devices.begin(), beforeUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (observed == beforeUnmount.devices.end()) {
    result.error = "The selected test device is no longer connected";
    return result;
  }
  const core::SafetyPolicy safetyPolicy;
  const auto identity = safetyPolicy.validateIdentity(selected, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }
  if (!unmountVolumes(observed->mountPoints, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled after unmounting the target";
    return result;
  }

  const DeviceDiscoveryResult afterUnmount = discover();
  const auto reobserved = std::find_if(
      afterUnmount.devices.begin(), afterUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (reobserved == afterUnmount.devices.end()) {
    result.error = "The test device disappeared after unmounting";
    return result;
  }
  const auto finalIdentity = safetyPolicy.validateIdentity(selected, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  FileDescriptor descriptor(
      open(selected.devicePath.c_str(), O_RDWR | O_EXCL | O_CLOEXEC));
  if (!descriptor.valid()) {
    result.error = errorMessage("Unable to acquire exclusive test-device access");
    return result;
  }
  struct stat status {};
  std::uint64_t capacity = 0;
  int sectorSize = 0;
  int readOnly = 1;
  if (fstat(descriptor.get(), &status) != 0 || !S_ISBLK(status.st_mode) ||
      ioctl(descriptor.get(), BLKGETSIZE64, &capacity) != 0 ||
      ioctl(descriptor.get(), BLKSSZGET, &sectorSize) != 0 ||
      ioctl(descriptor.get(), BLKROGET, &readOnly) != 0 || readOnly != 0 ||
      capacity != selected.capacityBytes || sectorSize <= 0 ||
      static_cast<std::uint32_t>(sectorSize) != selected.logicalSectorSize) {
    result.error = "Test-device identity, geometry, or writability changed before opening";
    return result;
  }
  LinuxRawTarget target(descriptor.get(), capacity,
                        static_cast<std::uint32_t>(sectorSize));
  const core::BadBlockTester tester;
  return tester.test(target, options, onProgress, isCancelled);
}

core::RawWriteResult LinuxBlockDeviceBackend::writeRaw(
    const core::RawWritePlan& plan,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) const {
  core::RawWriteResult result;
  const std::uint64_t total = plan.bytesToWrite();
  report(onProgress, core::RawWriteStage::Revalidating, 0, total);

  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled before the target was modified";
    return result;
  }
  if (geteuid() != 0) {
    result.error = "Administrative block-device access is required";
    return result;
  }
  const auto sourceSafety = core::WritePlanBuilder::validateSource(plan);
  if (!sourceSafety.safe()) {
    result.error = sourceSafety.issues.front().message;
    return result;
  }

  FileDescriptor sourceDescriptor(
      open(plan.image().path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!sourceDescriptor.valid()) {
    result.error = errorMessage("Unable to open the source image");
    return result;
  }
  struct stat sourceStatus {};
  if (fstat(sourceDescriptor.get(), &sourceStatus) != 0) {
    result.error = errorMessage("Unable to inspect the opened source image");
    return result;
  }
  if (!S_ISREG(sourceStatus.st_mode) || sourceStatus.st_size < 0 ||
      static_cast<std::uint64_t>(sourceStatus.st_size) != plan.image().sizeBytes) {
    result.error = "The opened source is not the regular image file that was analyzed";
    return result;
  }
  if (sourceUsesTarget(sourceDescriptor.get(), plan.target().devicePath)) {
    result.error = "The source image is stored on the selected target device";
    return result;
  }
  const std::filesystem::path descriptorPath =
      "/proc/self/fd/" + std::to_string(sourceDescriptor.get());
  auto openedSource = core::openRawImageSource(plan.image(), descriptorPath);
  if (!openedSource.succeeded()) {
    result.error = openedSource.error.empty()
                       ? "Unable to open the source image"
                       : std::move(openedSource.error);
    return result;
  }

  const DeviceDiscoveryResult beforeUnmount = discover();
  const auto observed = std::find_if(
      beforeUnmount.devices.begin(), beforeUnmount.devices.end(), [&plan](const auto& device) {
        return device.devicePath == plan.target().devicePath;
      });
  if (observed == beforeUnmount.devices.end()) {
    result.error = "The selected target is no longer connected";
    return result;
  }
  const auto targetSafety = detail::validateRawWriteTarget(plan, *observed);
  if (!targetSafety.safe()) {
    result.error = targetSafety.issues.front().message;
    return result;
  }

  report(onProgress, core::RawWriteStage::Claiming, 0, total);
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled before the target was modified";
    return result;
  }
  report(onProgress, core::RawWriteStage::Unmounting, 0, total);
  if (!unmountVolumes(observed->mountPoints, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled after unmounting but before modifying the target";
    return result;
  }

  const DeviceDiscoveryResult afterUnmount = discover();
  const auto reobserved = std::find_if(
      afterUnmount.devices.begin(), afterUnmount.devices.end(), [&plan](const auto& device) {
        return device.devicePath == plan.target().devicePath;
      });
  if (reobserved == afterUnmount.devices.end()) {
    result.error = "The target disappeared after its volumes were unmounted";
    return result;
  }
  const auto finalIdentity = detail::validateRawWriteTarget(plan, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  report(onProgress, core::RawWriteStage::Opening, 0, total);
  FileDescriptor targetDescriptor(
      open(plan.target().devicePath.c_str(), O_RDWR | O_EXCL | O_CLOEXEC));
  if (!targetDescriptor.valid()) {
    result.error = errorMessage("Unable to acquire exclusive raw-device access");
    return result;
  }
  struct stat targetStatus {};
  if (fstat(targetDescriptor.get(), &targetStatus) != 0 ||
      !S_ISBLK(targetStatus.st_mode)) {
    result.error = "The opened target is not a block device";
    return result;
  }

  std::uint64_t capacity = 0;
  int sectorSize = 0;
  int readOnly = 1;
  if (ioctl(targetDescriptor.get(), BLKGETSIZE64, &capacity) != 0 || capacity == 0) {
    result.error = errorMessage("Unable to read the raw-device capacity");
    return result;
  }
  if (ioctl(targetDescriptor.get(), BLKSSZGET, &sectorSize) != 0 || sectorSize <= 0) {
    result.error = errorMessage("Unable to read the raw-device sector size");
    return result;
  }
  if (ioctl(targetDescriptor.get(), BLKROGET, &readOnly) != 0) {
    result.error = errorMessage("Unable to query raw-device writability");
    return result;
  }
  if (readOnly != 0) {
    result.error = "The opened raw device reports that it is read-only";
    return result;
  }
  if (capacity != plan.target().capacityBytes ||
      static_cast<std::uint32_t>(sectorSize) != plan.target().logicalSectorSize) {
    result.error = "Raw-device geometry changed after the device was selected";
    return result;
  }

  LinuxRawTarget target(targetDescriptor.get(), capacity,
                        static_cast<std::uint32_t>(sectorSize));
  const core::RawImageWriter writer;
  return detail::finishNativeWrite(plan,
      [&](const auto& progress) {
        return writer.write(plan, *openedSource.source, target, progress, isCancelled);
      }, [&](std::string& error) {
        const auto safety = core::WritePlanBuilder::validateSource(plan);
        if (!safety.safe()) {
          error = safety.issues.front().message;
          return false;
        }
        if (ioctl(targetDescriptor.get(), BLKRRPART, 0) != 0) {
          error = errorMessage(
              "The image was written and verified, but Linux could not refresh the partition table");
          return false;
        }
        return true;
      }, onProgress);
}

}  // namespace

std::unique_ptr<BlockDeviceBackend> makeLinuxBlockDeviceBackend() {
  return std::make_unique<LinuxBlockDeviceBackend>();
}

}  // namespace rufus::backend
