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

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <signal.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "rufus/core/raw_image_writer.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "system_raw_io.hpp"
#include "raw_write_safety.hpp"
#include "rufus/core/safety_policy.hpp"
#include "rufus/core/write_plan.hpp"
#if !RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
#include "macos_privileged_helper_client.hpp"
#endif
#include "macos_installer_operations.hpp"
#include "macos_raw_device_operations.hpp"

namespace rufus::backend {

namespace {

class MacOSBlockDeviceBackend final : public BlockDeviceBackend {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
    return "macOS IOKit (unsigned root mode)";
#else
    return "macOS IOKit";
#endif
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
    result.macOsInstallerCreation = true;
    return result;
  }

  [[nodiscard]] PrivilegeStatus privilegeStatus() const override;

  [[nodiscard]] DeviceDiscoveryResult discover() const override;
  [[nodiscard]] RawWriteAvailability rawWriteAvailability(
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] RawWriteAvailability requestRawWriteAuthorization() const override;
  [[nodiscard]] core::MacOsInstallerAnalysisResult
  analyzeMacOsInstallerApplication(
      const std::filesystem::path& applicationPath) const override;
  [[nodiscard]] RawWriteAvailability macOsInstallerAvailability(
      const core::MacOsInstallerInfo& installer,
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] core::RawWriteResult createMacOsInstaller(
      const core::MacOsInstallerInfo& installer,
      const core::BlockDeviceInfo& target, bool fullWipe,
      const core::MacOsInstallerProgressCallback& onProgress = {},
      const core::MacOsInstallerCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] core::RawWriteResult writeRaw(
      const core::RawWritePlan& plan,
      const core::RawWriteProgressCallback& onProgress,
      const core::RawWriteCancelCallback& isCancelled) const override;
  [[nodiscard]] RawWriteAvailability badBlockTestAvailability(
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] core::BadBlockTestResult testBadBlocks(
      const core::BlockDeviceInfo& target,
      const core::BadBlockTestOptions& options,
      const core::BadBlockTestProgressCallback& onProgress = {},
      const core::BadBlockTestCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] RawWriteAvailability captureAvailability(
      const core::BlockDeviceInfo& target,
      core::MediaCaptureFormat format) const override;
  [[nodiscard]] core::MediaCaptureResult capture(
      const core::BlockDeviceInfo& target,
      const std::filesystem::path& destination,
      const core::MediaCaptureOptions& options,
      const core::MediaCaptureProgressCallback& onProgress = {},
      const core::MediaCaptureCancelCallback& isCancelled = {}) const override;
};

std::string trim(std::string value) {
  const auto notSpace = [](const unsigned char character) { return !std::isspace(character); };
  const auto first = std::find_if(value.begin(), value.end(), notSpace);
  const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
  return first < last ? std::string(first, last) : std::string{};
}

std::string cfStringToUtf8(CFStringRef value) {
  if (value == nullptr) {
    return {};
  }
  if (const char* direct = CFStringGetCStringPtr(value, kCFStringEncodingUTF8)) {
    return direct;
  }

  const CFIndex length = CFStringGetLength(value);
  const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  if (capacity <= 1) {
    return {};
  }
  std::vector<char> buffer(static_cast<std::size_t>(capacity));
  if (!CFStringGetCString(value, buffer.data(), capacity, kCFStringEncodingUTF8)) {
    return {};
  }
  return buffer.data();
}

std::string errorMessage(const std::string& operation, const int errorNumber) {
  return operation + ": " + std::strerror(errorNumber);
}

std::string rawDevicePath(const std::string& devicePath) {
  constexpr std::string_view prefix = "/dev/disk";
  if (devicePath.rfind(prefix, 0) != 0 || devicePath.size() == prefix.size()) {
    return {};
  }
  const auto suffix = devicePath.substr(prefix.size());
  if (!std::all_of(suffix.begin(), suffix.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0;
      })) {
    return {};
  }
  return "/dev/rdisk" + suffix;
}

void report(const core::RawWriteProgressCallback& callback, const core::RawWriteStage stage,
            const std::uint64_t processed, const std::uint64_t total) {
  if (callback) {
    callback({stage, processed, total});
  }
}

bool cancelled(const core::RawWriteCancelCallback& callback) {
  return callback && callback();
}

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
  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
  void reset() noexcept {
    if (descriptor_ >= 0) {
      close(descriptor_);
      descriptor_ = -1;
    }
  }

 private:
  int descriptor_;
};

RawWriteAvailability macOsPrivilegeAvailability() {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  if (geteuid() != 0) {
    return {
        false, false,
        "This unsigned root-mode build must be launched from Terminal with sudo before physical-device operations are enabled"};
  }
  return {true, false,
          "Unsigned root mode is active; the complete Rufus++ process is running as root"};
#else
  return macos::privilegedHelperAvailability();
#endif
}

#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
std::string localOperationId() {
  return "root-" + std::to_string(getpid()) + '-' +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count());
}

template <typename Identifier>
std::optional<Identifier> parseSudoIdentifier(const char* value) {
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' ||
      parsed > std::numeric_limits<Identifier>::max()) {
    return std::nullopt;
  }
  return static_cast<Identifier>(parsed);
}

bool restoreInvokingUserOwnership(const int descriptor, std::string& error) {
  if (geteuid() != 0) {
    return true;
  }
  const char* uidText = std::getenv("SUDO_UID");
  const char* gidText = std::getenv("SUDO_GID");
  if (uidText == nullptr && gidText == nullptr) {
    return true;
  }
  const auto uid = parseSudoIdentifier<uid_t>(uidText);
  const auto gid = parseSudoIdentifier<gid_t>(gidText);
  if (!uid || !gid) {
    error = "The sudo caller identity is incomplete or invalid; refusing to create a root-owned capture";
    return false;
  }
  if (fchown(descriptor, *uid, *gid) != 0) {
    error = errorMessage("Unable to restore capture ownership", errno);
    return false;
  }
  return true;
}

bool restoreInvokingUserOwnership(const std::filesystem::path& path,
                                  std::string& error) {
  FileDescriptor output(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!output.valid()) {
    error = errorMessage("Unable to reopen the capture to restore ownership", errno);
    return false;
  }
  struct stat status {};
  if (fstat(output.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1) {
    error = "The completed capture is no longer a private regular file";
    return false;
  }
  return restoreInvokingUserOwnership(output.get(), error);
}

core::MediaCaptureResult captureRawInUnsignedRootMode(
    const core::BlockDeviceInfo& source,
    const std::filesystem::path& destination,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  const auto privilege = macOsPrivilegeAvailability();
  if (!privilege.available) {
    result.error = privilege.reason;
    return result;
  }
  if (options.format != core::MediaCaptureFormat::Raw || destination.empty()) {
    result.error = "The macOS root-mode transport accepts only a valid raw capture destination";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(destination, fileError) || fileError) {
    result.error = fileError ? "Unable to inspect the capture destination: " +
                                   fileError.message()
                             : "The capture destination already exists";
    return result;
  }
  const auto parent = destination.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : destination.parent_path();
  if (fileError || !std::filesystem::is_directory(parent, fileError) ||
      fileError) {
    result.error = "The capture destination directory is unavailable";
    return result;
  }
  auto partial = destination;
  partial += ".rufus-plus-plus-capture-" + localOperationId();
  FileDescriptor output(open(partial.c_str(),
                             O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                             S_IRUSR | S_IWUSR));
  if (!output.valid()) {
    result.error = errorMessage("Unable to create the private capture output", errno);
    return result;
  }
  struct PartialCleanup final {
    std::filesystem::path path;
    bool committed{};
    ~PartialCleanup() {
      if (!committed) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    }
  } cleanup{partial};
  if (!restoreInvokingUserOwnership(output.get(), result.error)) {
    return result;
  }
  result = macos::captureRawLocally(source, output.get(), options, onProgress,
                                    isCancelled);
  output.reset();
  if (!result.success) {
    return result;
  }
  std::filesystem::rename(partial, destination, fileError);
  if (fileError) {
    result.success = false;
    result.error = "Unable to commit the raw capture: " + fileError.message();
    return result;
  }
  cleanup.committed = true;
  return result;
}
#endif

struct DiskArbitrationOperation final {
  CFRunLoopRef runLoop{};
  bool complete{};
  std::string error;
};

void diskArbitrationCallback(DADiskRef, DADissenterRef dissenter, void* context) {
  auto& operation = *static_cast<DiskArbitrationOperation*>(context);
  if (dissenter != nullptr) {
    const CFStringRef status = DADissenterGetStatusString(dissenter);
    operation.error = cfStringToUtf8(status);
    if (operation.error.empty()) {
      operation.error = "Disk Arbitration status " +
                        std::to_string(DADissenterGetStatus(dissenter));
    }
  }
  operation.complete = true;
  if (operation.runLoop != nullptr) {
    CFRunLoopStop(operation.runLoop);
  }
}

DADissenterRef refuseClaimRelease(DADiskRef, void*) {
  return DADissenterCreate(kCFAllocatorDefault, kDAReturnBusy,
                           CFSTR("Rufus++ is writing this device"));
}

class DiskArbitrationAccess final {
 public:
  ~DiskArbitrationAccess() {
    if (claimed_ && disk_ != nullptr) {
      DADiskUnclaim(disk_);
    }
    if (scheduled_ && session_ != nullptr && runLoop_ != nullptr) {
      DASessionUnscheduleFromRunLoop(session_, runLoop_, kCFRunLoopDefaultMode);
    }
    if (disk_ != nullptr) {
      CFRelease(disk_);
    }
    if (session_ != nullptr) {
      CFRelease(session_);
    }
  }

  DiskArbitrationAccess(const DiskArbitrationAccess&) = delete;
  DiskArbitrationAccess& operator=(const DiskArbitrationAccess&) = delete;
  DiskArbitrationAccess() = default;

  bool initialize(const std::string& devicePath, std::string& error) {
    session_ = DASessionCreate(kCFAllocatorDefault);
    if (session_ == nullptr) {
      error = "Unable to create a Disk Arbitration session";
      return false;
    }
    disk_ = DADiskCreateFromBSDName(kCFAllocatorDefault, session_, devicePath.c_str());
    if (disk_ == nullptr) {
      error = "Disk Arbitration could not resolve the selected device";
      return false;
    }
    runLoop_ = CFRunLoopGetCurrent();
    DASessionScheduleWithRunLoop(session_, runLoop_, kCFRunLoopDefaultMode);
    scheduled_ = true;
    return true;
  }

  bool claim(std::string& error) {
    operation_ = {runLoop_, false, {}};
    DADiskClaim(disk_, kDADiskClaimOptionDefault, refuseClaimRelease, nullptr,
                diskArbitrationCallback, &operation_);
    if (!wait(error)) {
      return false;
    }
    claimed_ = true;
    return true;
  }

  bool unmount(std::string& error) {
    operation_ = {runLoop_, false, {}};
    DADiskUnmount(disk_, kDADiskUnmountOptionWhole, diskArbitrationCallback, &operation_);
    return wait(error);
  }

 private:
  bool wait(std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!operation_.complete && std::chrono::steady_clock::now() < deadline) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    }
    if (!operation_.complete) {
      error = "Disk Arbitration operation timed out";
      return false;
    }
    if (!operation_.error.empty()) {
      error = operation_.error;
      return false;
    }
    return true;
  }

  DASessionRef session_{};
  DADiskRef disk_{};
  CFRunLoopRef runLoop_{};
  DiskArbitrationOperation operation_{};
  bool scheduled_{};
  bool claimed_{};
};

using MacRawTarget = detail::SystemRawTarget;

std::string cfValueToString(CFTypeRef value) {
  if (value == nullptr) {
    return {};
  }
  if (CFGetTypeID(value) == CFStringGetTypeID()) {
    return trim(cfStringToUtf8(static_cast<CFStringRef>(value)));
  }
  if (CFGetTypeID(value) == CFDataGetTypeID()) {
    const auto data = static_cast<CFDataRef>(value);
    const auto length = CFDataGetLength(data);
    const auto* bytes = CFDataGetBytePtr(data);
    return bytes == nullptr || length <= 0
               ? std::string{}
               : trim(std::string(reinterpret_cast<const char*>(bytes),
                                  static_cast<std::size_t>(length)));
  }
  return {};
}

CFTypeRef copyProperty(const io_registry_entry_t entry, CFStringRef key,
                       const bool searchParents = false) {
  if (searchParents) {
    return IORegistryEntrySearchCFProperty(
        entry, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
  }
  return IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
}

std::string stringProperty(const io_registry_entry_t entry, CFStringRef key,
                           const bool searchParents = false) {
  CFTypeRef value = copyProperty(entry, key, searchParents);
  const std::string result = cfValueToString(value);
  if (value != nullptr) {
    CFRelease(value);
  }
  return result;
}

std::string dictionaryStringProperty(const io_registry_entry_t entry, CFStringRef dictionaryKey,
                                     CFStringRef valueKey) {
  CFTypeRef property = copyProperty(entry, dictionaryKey, true);
  std::string result;
  if (property != nullptr && CFGetTypeID(property) == CFDictionaryGetTypeID()) {
    const auto dictionary = static_cast<CFDictionaryRef>(property);
    result = cfValueToString(CFDictionaryGetValue(dictionary, valueKey));
  }
  if (property != nullptr) {
    CFRelease(property);
  }
  return result;
}

std::string nearestUsbDeviceSerial(const io_registry_entry_t media) {
  // Mass-storage drivers do not always copy the USB serial into the storage
  // Device Characteristics dictionary. Read the closest actual USB device,
  // stopping even when it has no serial so a hub's serial is never substituted.
  if (IOObjectRetain(media) != KERN_SUCCESS) return {};
  io_registry_entry_t current = media;
  while (current != IO_OBJECT_NULL) {
    if (IOObjectConformsTo(current, "IOUSBHostDevice") ||
        IOObjectConformsTo(current, "IOUSBDevice")) {
      auto serial = stringProperty(current, CFSTR("USB Serial Number"));
      if (serial.empty()) serial = stringProperty(current, CFSTR("kUSBSerialNumberString"));
      IOObjectRelease(current);
      return trim(serial);
    }
    io_registry_entry_t parent = IO_OBJECT_NULL;
    const auto status = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent);
    IOObjectRelease(current);
    current = status == KERN_SUCCESS ? parent : IO_OBJECT_NULL;
  }
  return {};
}

bool boolProperty(const io_registry_entry_t entry, CFStringRef key, const bool defaultValue) {
  CFTypeRef value = copyProperty(entry, key);
  bool result = defaultValue;
  if (value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID()) {
    result = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
  }
  if (value != nullptr) {
    CFRelease(value);
  }
  return result;
}

std::uint64_t numberProperty(const io_registry_entry_t entry, CFStringRef key) {
  CFTypeRef value = copyProperty(entry, key);
  std::int64_t result = 0;
  if (value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID()) {
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result);
  }
  if (value != nullptr) {
    CFRelease(value);
  }
  return result > 0 ? static_cast<std::uint64_t>(result) : 0;
}

std::string wholeDiskName(std::string bsdName) {
  if (bsdName.rfind("/dev/", 0) == 0) {
    bsdName.erase(0, 5);
  }
  if (bsdName.rfind("rdisk", 0) == 0) {
    bsdName.erase(0, 1);
  }
  if (bsdName.rfind("disk", 0) != 0) {
    return bsdName;
  }

  std::size_t index = 4;
  while (index < bsdName.size() && std::isdigit(static_cast<unsigned char>(bsdName[index]))) {
    ++index;
  }
  return bsdName.substr(0, index);
}

struct MountState final {
  std::map<std::string, std::vector<std::string>> mountPoints;
  std::string rootDevice;
  std::string rootBsdName;
};

MountState readMountState() {
  MountState result;
  struct statfs* mounts = nullptr;
  const int count = getmntinfo(&mounts, MNT_NOWAIT);
  for (int index = 0; index < count; ++index) {
    const std::string source = mounts[index].f_mntfromname;
    const std::string mountPoint = mounts[index].f_mntonname;
    const std::string whole = wholeDiskName(source);
    if (!whole.empty()) {
      result.mountPoints[whole].push_back(mountPoint);
    }
    if (mountPoint == "/") {
      result.rootDevice = whole;
      result.rootBsdName = source.rfind("/dev/", 0) == 0 ? source.substr(5) : source;
      if (result.rootBsdName.rfind("rdisk", 0) == 0) {
        result.rootBsdName.erase(0, 1);
      }
    }
  }
  return result;
}

std::set<std::string> relatedWholeDiskNames(const io_registry_entry_t media) {
  std::set<std::string> result;
  const auto addEntry = [&result](const io_registry_entry_t entry) {
    const std::string name = wholeDiskName(stringProperty(entry, CFSTR(kIOBSDNameKey)));
    if (name.rfind("disk", 0) == 0) {
      result.insert(name);
    }
  };
  addEntry(media);

  io_iterator_t iterator = IO_OBJECT_NULL;
  if (IORegistryEntryCreateIterator(media, kIOServicePlane, kIORegistryIterateRecursively,
                                    &iterator) != KERN_SUCCESS) {
    return result;
  }
  for (io_registry_entry_t entry = IOIteratorNext(iterator); entry != IO_OBJECT_NULL;
       entry = IOIteratorNext(iterator)) {
    addEntry(entry);
    IOObjectRelease(entry);
  }
  IOObjectRelease(iterator);
  return result;
}

std::set<std::string> rootBackingDiskNames(const std::string& rootBsdName) {
  std::set<std::string> result;
  if (rootBsdName.empty()) {
    return result;
  }

  io_service_t rootMedia = IOServiceGetMatchingService(
      kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, rootBsdName.c_str()));
  if (rootMedia == IO_OBJECT_NULL) {
    return result;
  }
  const auto addEntry = [&result](const io_registry_entry_t entry) {
    const std::string name = wholeDiskName(stringProperty(entry, CFSTR(kIOBSDNameKey)));
    if (name.rfind("disk", 0) == 0) {
      result.insert(name);
    }
  };
  addEntry(rootMedia);

  io_iterator_t iterator = IO_OBJECT_NULL;
  if (IORegistryEntryCreateIterator(rootMedia, kIOServicePlane,
                                    kIORegistryIterateRecursively | kIORegistryIterateParents,
                                    &iterator) == KERN_SUCCESS) {
    for (io_registry_entry_t entry = IOIteratorNext(iterator); entry != IO_OBJECT_NULL;
         entry = IOIteratorNext(iterator)) {
      addEntry(entry);
      IOObjectRelease(entry);
    }
    IOObjectRelease(iterator);
  }
  IOObjectRelease(rootMedia);
  return result;
}

core::DeviceBus parseBus(std::string interconnect) {
  std::transform(interconnect.begin(), interconnect.end(), interconnect.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (interconnect.find("usb") != std::string::npos) {
    return core::DeviceBus::Usb;
  }
  if (interconnect.find("secure digital") != std::string::npos ||
      interconnect == "sd") {
    return core::DeviceBus::Sd;
  }
  if (interconnect.find("thunderbolt") != std::string::npos) {
    return core::DeviceBus::Thunderbolt;
  }
  if (interconnect.find("nvme") != std::string::npos ||
      interconnect.find("pci-express") != std::string::npos) {
    return core::DeviceBus::Nvme;
  }
  if (interconnect.find("sata") != std::string::npos ||
      interconnect.find("ata") != std::string::npos) {
    return core::DeviceBus::Sata;
  }
  if (interconnect.find("virtual") != std::string::npos ||
      interconnect.find("disk image") != std::string::npos) {
    return core::DeviceBus::Virtual;
  }
  return core::DeviceBus::Unknown;
}

DeviceDiscoveryResult discoverBlockDevicesInternal() {
  DeviceDiscoveryResult result;
  const MountState mounts = readMountState();
  const auto systemDisks = rootBackingDiskNames(mounts.rootBsdName);

  CFMutableDictionaryRef matching = IOServiceMatching(kIOMediaClass);
  if (matching == nullptr) {
    result.warnings.emplace_back("IOKit could not create an IOMedia query");
    return result;
  }
  CFDictionarySetValue(matching, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);

  io_iterator_t iterator = IO_OBJECT_NULL;
  const kern_return_t status =
      IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
  if (status != KERN_SUCCESS) {
    result.warnings.emplace_back("IOKit physical-device discovery failed");
    return result;
  }

  for (io_registry_entry_t media = IOIteratorNext(iterator); media != IO_OBJECT_NULL;
       media = IOIteratorNext(iterator)) {
    core::BlockDeviceInfo device;
    const std::string bsdName = stringProperty(media, CFSTR(kIOBSDNameKey));
    if (bsdName.empty()) {
      IOObjectRelease(media);
      continue;
    }

    device.devicePath = "/dev/" + bsdName;
    device.capacityBytes = numberProperty(media, CFSTR(kIOMediaSizeKey));
    device.logicalSectorSize = static_cast<std::uint32_t>(
        numberProperty(media, CFSTR(kIOMediaPreferredBlockSizeKey)));
    device.removable = boolProperty(media, CFSTR(kIOMediaRemovableKey), false);
    device.ejectable = boolProperty(media, CFSTR(kIOMediaEjectableKey), false);
    device.writable = boolProperty(media, CFSTR(kIOMediaWritableKey), false);
    device.wholeDevice = boolProperty(media, CFSTR(kIOMediaWholeKey), true);

    device.vendor = dictionaryStringProperty(
        media, CFSTR("Device Characteristics"), CFSTR("Vendor Name"));
    device.model = dictionaryStringProperty(
        media, CFSTR("Device Characteristics"), CFSTR("Product Name"));
    device.serialNumber = dictionaryStringProperty(
        media, CFSTR("Device Characteristics"), CFSTR("Serial Number"));
    const std::string interconnect = dictionaryStringProperty(
        media, CFSTR("Protocol Characteristics"), CFSTR("Physical Interconnect"));
    device.bus = parseBus(interconnect);
    if (device.bus == core::DeviceBus::Usb && device.serialNumber.empty()) {
      device.serialNumber = nearestUsbDeviceSerial(media);
    }

    device.displayName = trim(device.vendor + " " + device.model);
    if (device.displayName.empty()) {
      device.displayName = bsdName;
    }
    std::uint64_t registryId = 0;
    static_cast<void>(IORegistryEntryGetRegistryEntryID(media, &registryId));
    if (!device.serialNumber.empty() || registryId != 0) {
      device.stableId =
          "macos:" +
          (device.serialNumber.empty() ? "registry-" + std::to_string(registryId)
                                       : device.serialNumber) +
          ':' + std::to_string(device.capacityBytes);
    }
    const auto relatedNames = relatedWholeDiskNames(media);
    device.systemDevice = relatedNames.find(mounts.rootDevice) != relatedNames.end() ||
                          std::any_of(relatedNames.begin(), relatedNames.end(),
                                      [&systemDisks](const auto& name) {
                                        return systemDisks.find(name) != systemDisks.end();
                                      });
    for (const auto& name : relatedNames) {
      if (const auto found = mounts.mountPoints.find(name);
          found != mounts.mountPoints.end()) {
        device.mountPoints.insert(device.mountPoints.end(), found->second.begin(),
                                  found->second.end());
      }
    }
    std::sort(device.mountPoints.begin(), device.mountPoints.end());
    device.mountPoints.erase(
        std::unique(device.mountPoints.begin(), device.mountPoints.end()),
        device.mountPoints.end());

    result.devices.push_back(std::move(device));
    IOObjectRelease(media);
  }
  IOObjectRelease(iterator);

  std::sort(result.devices.begin(), result.devices.end(),
            [](const core::BlockDeviceInfo& left, const core::BlockDeviceInfo& right) {
              return left.devicePath < right.devicePath;
            });
  return result;
}

PrivilegeStatus MacOSBlockDeviceBackend::privilegeStatus() const {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  if (geteuid() == 0) {
    return {PrivilegeRoute::Elevated,
            "The complete application is running as root; guarded physical-device operations are enabled"};
  }
  return {PrivilegeRoute::Unprivileged,
          "This development build is not running as root; physical-device operations are disabled"};
#else
  const auto helper = macos::privilegedHelperAvailability();
  if (helper.available) {
    return {PrivilegeRoute::PrivilegedHelper,
            "The Qt application remains unprivileged; authenticated physical-device operations are delegated to the signed helper"};
  }
  return {PrivilegeRoute::Unprivileged, helper.reason};
#endif
}

RawWriteAvailability MacOSBlockDeviceBackend::rawWriteAvailability(
    const core::BlockDeviceInfo& target) const {
  const auto eligibility = core::evaluateDeviceEligibility(target);
  if (eligibility != core::DeviceEligibility::Eligible) {
    return {false, false, "Target is not eligible: " +
                       std::string(core::deviceEligibilityName(eligibility))};
  }
  const std::string rawPath = rawDevicePath(target.devicePath);
  if (rawPath.empty()) {
    return {false, false, "The target is not a whole macOS disk path"};
  }
  return macOsPrivilegeAvailability();
}

RawWriteAvailability MacOSBlockDeviceBackend::requestRawWriteAuthorization() const {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  return macOsPrivilegeAvailability();
#else
  return macos::requestPrivilegedHelperAuthorization();
#endif
}

core::MacOsInstallerAnalysisResult
MacOSBlockDeviceBackend::analyzeMacOsInstallerApplication(
    const std::filesystem::path& applicationPath) const {
  return macos::analyzeInstallerApplication(applicationPath);
}

RawWriteAvailability MacOSBlockDeviceBackend::macOsInstallerAvailability(
    const core::MacOsInstallerInfo& installer,
    const core::BlockDeviceInfo& target) const {
  if (installer.applicationPath.empty() ||
      installer.createInstallMediaPath.empty()) {
    return {false, false, "The macOS installer application is invalid"};
  }
  const auto eligibility = core::evaluateDeviceEligibility(target);
  if (eligibility != core::DeviceEligibility::Eligible) {
    return {false, false, "Target is not eligible: " +
                       std::string(core::deviceEligibilityName(eligibility))};
  }
  if (rawDevicePath(target.devicePath).empty()) {
    return {false, false, "The target is not a whole macOS disk path"};
  }
  if (target.capacityBytes < installer.minimumTargetBytes) {
    return {false, false,
            "The target is smaller than the 16 GiB minimum for a macOS installer"};
  }
  return macOsPrivilegeAvailability();
}

core::RawWriteResult MacOSBlockDeviceBackend::createMacOsInstaller(
    const core::MacOsInstallerInfo& installer,
    const core::BlockDeviceInfo& target, const bool fullWipe,
    const core::MacOsInstallerProgressCallback& onProgress,
    const core::MacOsInstallerCancelCallback& isCancelled) const {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  core::RawWriteResult result;
  const auto availability = macOsInstallerAvailability(installer, target);
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  return macos::createInstallerLocally(installer, target, fullWipe,
                                       localOperationId(), onProgress,
                                       isCancelled);
#else
  return macos::createMacOsInstallerWithPrivilegedHelper(
      installer, target, fullWipe, onProgress, isCancelled);
#endif
}

core::RawWriteResult writeRawLocallyInternal(
    const core::RawWritePlan& plan,
    core::RawSourceIo* source,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) {
  core::RawWriteResult result;
  const std::uint64_t total = plan.bytesToWrite();
  report(onProgress, core::RawWriteStage::Revalidating, 0, total);

  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled before the target was modified";
    return result;
  }

  const auto sourceSafety = core::WritePlanBuilder::validateSource(plan);
  if (!sourceSafety.safe()) {
    result.error = sourceSafety.issues.front().message;
    return result;
  }

  const DeviceDiscoveryResult beforeUnmount = discoverBlockDevicesInternal();
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

  const std::string rawPath = rawDevicePath(plan.target().devicePath);
  if (rawPath.empty()) {
    result.error = "Refusing a target that is not a whole macOS disk path";
    return result;
  }
  if (access(rawPath.c_str(), W_OK) != 0) {
    result.error = errorMessage("Administrative raw-device access is unavailable", errno) +
                   "; no volume was unmounted";
    return result;
  }

  DiskArbitrationAccess arbitration;
  if (!arbitration.initialize(plan.target().devicePath, result.error)) {
    return result;
  }
  report(onProgress, core::RawWriteStage::Claiming, 0, total);
  if (!arbitration.claim(result.error)) {
    result.error = "Unable to claim the selected device exclusively: " + result.error;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled before the target was modified";
    return result;
  }

  report(onProgress, core::RawWriteStage::Unmounting, 0, total);
  if (!arbitration.unmount(result.error)) {
    result.error = "Unable to unmount every target volume: " + result.error;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled after unmounting but before modifying the target";
    return result;
  }

  const DeviceDiscoveryResult afterUnmount = discoverBlockDevicesInternal();
  const auto reobserved = std::find_if(
      afterUnmount.devices.begin(), afterUnmount.devices.end(), [&plan](const auto& device) {
        return device.devicePath == plan.target().devicePath;
      });
  if (reobserved == afterUnmount.devices.end()) {
    result.error = "The target disappeared after it was unmounted";
    return result;
  }
  const auto finalIdentity = detail::validateRawWriteTarget(plan, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  report(onProgress, core::RawWriteStage::Opening, 0, total);
  FileDescriptor descriptor(open(rawPath.c_str(), O_RDWR | O_EXCL | O_CLOEXEC));
  if (!descriptor.valid()) {
    result.error = errorMessage("Unable to open the raw device", errno);
    return result;
  }
  std::uint32_t sectorSize = 0;
  std::uint64_t blockCount = 0;
  std::uint32_t writable = 0;
  if (ioctl(descriptor.get(), DKIOCGETBLOCKSIZE, &sectorSize) != 0) {
    result.error = errorMessage("Unable to read the raw-device sector size", errno);
    return result;
  }
  if (sectorSize == 0) {
    result.error = "The opened raw device reported an invalid sector size";
    return result;
  }
  if (ioctl(descriptor.get(), DKIOCGETBLOCKCOUNT, &blockCount) != 0) {
    result.error = errorMessage("Unable to read the raw-device block count", errno);
    return result;
  }
  if (blockCount == 0) {
    result.error = "The opened raw device reported an invalid block count";
    return result;
  }
  if (ioctl(descriptor.get(), DKIOCISWRITABLE, &writable) != 0) {
    result.error = errorMessage("Unable to query raw-device writability", errno);
    return result;
  }
  if (writable == 0) {
    result.error = "The opened raw device reports that it is read-only";
    return result;
  }
  if (blockCount > std::numeric_limits<std::uint64_t>::max() / sectorSize) {
    result.error = "Raw-device capacity overflowed during identity validation";
    return result;
  }
  const std::uint64_t capacity = blockCount * sectorSize;
  if (capacity != plan.target().capacityBytes ||
      sectorSize != plan.target().logicalSectorSize) {
    result.error = "Raw-device geometry changed after the device was selected";
    return result;
  }

  static_cast<void>(fcntl(descriptor.get(), F_NOCACHE, 1));
  MacRawTarget rawTarget(descriptor.get(), capacity, sectorSize);
  const core::RawImageWriter writer;
  return detail::finishNativeWrite(plan,
      [&](const auto& progress) {
        return source == nullptr ? writer.write(plan, rawTarget, progress, isCancelled)
                                 : writer.write(plan, *source, rawTarget, progress, isCancelled);
      }, [&](std::string& error) {
        const auto safety = core::WritePlanBuilder::validateSource(plan);
        if (!safety.safe()) {
          error = safety.issues.front().message;
          return false;
        }
        return true;
      }, onProgress);
}

core::BadBlockTestResult testBadBlocksLocallyInternal(
    const core::BlockDeviceInfo& selected,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled) {
  core::BadBlockTestResult result;
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled before the target was modified";
    return result;
  }
  const auto eligibility = core::evaluateDeviceEligibility(selected);
  if (eligibility != core::DeviceEligibility::Eligible) {
    result.error = "Test target is not eligible: " +
                   std::string(core::deviceEligibilityName(eligibility));
    return result;
  }
  const DeviceDiscoveryResult beforeUnmount = discoverBlockDevicesInternal();
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
  const std::string rawPath = rawDevicePath(selected.devicePath);
  if (rawPath.empty()) {
    result.error = "Refusing a test target that is not a whole macOS disk path";
    return result;
  }
  if (access(rawPath.c_str(), W_OK) != 0) {
    result.error = errorMessage(
        "Administrative raw-device access is unavailable", errno);
    return result;
  }

  DiskArbitrationAccess arbitration;
  if (!arbitration.initialize(selected.devicePath, result.error) ||
      !arbitration.claim(result.error)) {
    result.error = "Unable to claim the selected test device exclusively: " +
                   result.error;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled before the target was modified";
    return result;
  }
  if (!arbitration.unmount(result.error)) {
    result.error = "Unable to unmount every test-device volume: " + result.error;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled after unmounting the target";
    return result;
  }

  const DeviceDiscoveryResult afterUnmount = discoverBlockDevicesInternal();
  const auto reobserved = std::find_if(
      afterUnmount.devices.begin(), afterUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (reobserved == afterUnmount.devices.end()) {
    result.error = "The test device disappeared after it was unmounted";
    return result;
  }
  const auto finalIdentity = safetyPolicy.validateIdentity(selected, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  FileDescriptor descriptor(open(rawPath.c_str(), O_RDWR | O_EXCL | O_CLOEXEC));
  if (!descriptor.valid()) {
    result.error = errorMessage("Unable to open the raw test device", errno);
    return result;
  }
  std::uint32_t sectorSize = 0;
  std::uint64_t blockCount = 0;
  std::uint32_t writable = 0;
  if (ioctl(descriptor.get(), DKIOCGETBLOCKSIZE, &sectorSize) != 0 ||
      ioctl(descriptor.get(), DKIOCGETBLOCKCOUNT, &blockCount) != 0 ||
      ioctl(descriptor.get(), DKIOCISWRITABLE, &writable) != 0 ||
      sectorSize == 0U || blockCount == 0U || writable == 0U ||
      blockCount > std::numeric_limits<std::uint64_t>::max() / sectorSize) {
    result.error = "Unable to validate the raw test-device geometry and writability";
    return result;
  }
  const std::uint64_t capacity = blockCount * sectorSize;
  if (capacity != selected.capacityBytes ||
      sectorSize != selected.logicalSectorSize) {
    result.error = "Raw test-device geometry changed after selection";
    return result;
  }
  static_cast<void>(fcntl(descriptor.get(), F_NOCACHE, 1));
  MacRawTarget target(descriptor.get(), capacity, sectorSize);
  const core::BadBlockTester tester;
  return tester.test(target, options, onProgress, isCancelled);
}

core::MediaCaptureResult captureRawLocallyInternal(
    const core::BlockDeviceInfo& selected, const int destinationDescriptor,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  if (options.format != core::MediaCaptureFormat::Raw) {
    result.error = "The macOS helper currently accepts only raw capture output";
    return result;
  }
  if (options.transferBytes < 4096U ||
      options.transferBytes > 64U * 1024U * 1024U) {
    result.error = "The capture transfer size is outside the supported bounds";
    return result;
  }
  struct stat destinationStatus {};
  struct statfs destinationFileSystem {};
  if (destinationDescriptor < 0 ||
      fstat(destinationDescriptor, &destinationStatus) != 0 ||
      !S_ISREG(destinationStatus.st_mode) || destinationStatus.st_size != 0 ||
      fstatfs(destinationDescriptor, &destinationFileSystem) != 0) {
    result.error = "The capture destination is not a new regular file";
    return result;
  }
  const std::string destinationDevice =
      destinationFileSystem.f_mntfromname;
  if (destinationDevice.rfind(selected.devicePath, 0) == 0 &&
      (destinationDevice.size() == selected.devicePath.size() ||
       destinationDevice[selected.devicePath.size()] == 's')) {
    result.error = "The capture destination is stored on the device being captured";
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Capture cancelled before the source was opened";
    return result;
  }
  const auto eligibility = core::evaluateDeviceEligibility(selected);
  if (eligibility != core::DeviceEligibility::Eligible) {
    result.error = "Capture source is not eligible: " +
                   std::string(core::deviceEligibilityName(eligibility));
    return result;
  }
  const DeviceDiscoveryResult beforeUnmount = discoverBlockDevicesInternal();
  const auto observed = std::find_if(
      beforeUnmount.devices.begin(), beforeUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (observed == beforeUnmount.devices.end()) {
    result.error = "The selected capture source is no longer connected";
    return result;
  }
  const core::SafetyPolicy safetyPolicy;
  const auto identity = safetyPolicy.validateIdentity(selected, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }
  const std::string rawPath = rawDevicePath(selected.devicePath);
  if (rawPath.empty() || access(rawPath.c_str(), R_OK) != 0) {
    result.error = "Administrative raw-device read access is unavailable";
    return result;
  }
  DiskArbitrationAccess arbitration;
  if (!arbitration.initialize(selected.devicePath, result.error) ||
      !arbitration.claim(result.error)) {
    result.error = "Unable to claim the capture source exclusively: " +
                   result.error;
    return result;
  }
  if (!arbitration.unmount(result.error)) {
    result.error = "Unable to unmount every capture-source volume: " +
                   result.error;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Capture cancelled after unmounting the source";
    return result;
  }
  const DeviceDiscoveryResult afterUnmount = discoverBlockDevicesInternal();
  const auto reobserved = std::find_if(
      afterUnmount.devices.begin(), afterUnmount.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (reobserved == afterUnmount.devices.end()) {
    result.error = "The capture source disappeared after unmounting";
    return result;
  }
  const auto finalIdentity = safetyPolicy.validateIdentity(selected, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }
  FileDescriptor source(open(rawPath.c_str(), O_RDONLY | O_EXCL | O_CLOEXEC));
  if (!source.valid()) {
    result.error = errorMessage("Unable to open the raw capture source", errno);
    return result;
  }
  std::uint32_t sectorSize = 0;
  std::uint64_t blockCount = 0;
  if (ioctl(source.get(), DKIOCGETBLOCKSIZE, &sectorSize) != 0 ||
      ioctl(source.get(), DKIOCGETBLOCKCOUNT, &blockCount) != 0 ||
      sectorSize == 0U || blockCount == 0U ||
      blockCount > std::numeric_limits<std::uint64_t>::max() / sectorSize ||
      blockCount * sectorSize != selected.capacityBytes ||
      sectorSize != selected.logicalSectorSize) {
    result.error = "Capture-source geometry changed before reading";
    return result;
  }
  static_cast<void>(fcntl(source.get(), F_NOCACHE, 1));
  static_cast<void>(fcntl(destinationDescriptor, F_NOCACHE, 1));
  std::vector<unsigned char> buffer(options.transferBytes);
  for (std::uint64_t offset = 0; offset < selected.capacityBytes;) {
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      result.error = "Media capture cancelled";
      return result;
    }
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        buffer.size(), selected.capacityBytes - offset));
    std::string transferError;
    MacRawTarget sourceIo(source.get(), selected.capacityBytes, sectorSize);
    if (!sourceIo.readAt(offset, buffer.data(), amount, transferError)) {
      result.error = transferError;
      return result;
    }
    std::size_t written = 0U;
    while (written < amount) {
      const ssize_t count = pwrite(
          destinationDescriptor, buffer.data() + written, amount - written,
          static_cast<off_t>(offset + written));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        result.error = errorMessage("Unable to write the capture destination",
                                    count < 0 ? errno : EIO);
        return result;
      }
      written += static_cast<std::size_t>(count);
    }
    offset += amount;
    result.bytesCaptured = offset;
    if (onProgress) {
      onProgress({core::MediaCaptureStage::Capturing, offset,
                  selected.capacityBytes});
    }
  }
  if (fsync(destinationDescriptor) != 0) {
    result.error = errorMessage("Unable to flush the capture destination", errno);
    return result;
  }
  if (options.verify) {
    std::vector<unsigned char> expected(options.transferBytes);
    std::vector<unsigned char> observedBytes(options.transferBytes);
    MacRawTarget sourceIo(source.get(), selected.capacityBytes, sectorSize);
    for (std::uint64_t offset = 0; offset < selected.capacityBytes;) {
      if (cancelled(isCancelled)) {
        result.cancelled = true;
        result.error = "Media capture verification cancelled";
        return result;
      }
      const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
          expected.size(), selected.capacityBytes - offset));
      if (!sourceIo.readAt(offset, expected.data(), amount, result.error)) {
        return result;
      }
      std::size_t read = 0U;
      while (read < amount) {
        const ssize_t count = pread(
            destinationDescriptor, observedBytes.data() + read, amount - read,
            static_cast<off_t>(offset + read));
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          result.error = "Unable to read the capture destination for verification";
          return result;
        }
        read += static_cast<std::size_t>(count);
      }
      if (!std::equal(expected.begin(), expected.begin() + amount,
                      observedBytes.begin())) {
        result.error = "Captured image verification failed";
        return result;
      }
      offset += amount;
      if (onProgress) {
        onProgress({core::MediaCaptureStage::Verifying, offset,
                    selected.capacityBytes});
      }
    }
  }
  result.outputSizeBytes = selected.capacityBytes;
  result.success = true;
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Complete, selected.capacityBytes,
                selected.capacityBytes});
  }
  return result;
}

core::MediaCaptureResult captureUdfIsoLocally(
    const core::BlockDeviceInfo& selected,
    const std::filesystem::path& destination,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  if (destination.empty() || selected.mountPoints.size() != 1U ||
      access("/usr/bin/hdiutil", X_OK) != 0) {
    result.error =
        "UDF ISO capture requires one mounted source volume and the macOS hdiutil provider";
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
  if (fileError || !std::filesystem::is_directory(parent, fileError) ||
      fileError) {
    result.error = "The UDF capture destination directory is unavailable";
    return result;
  }
  struct statfs destinationFileSystem {};
  if (statfs(parent.c_str(), &destinationFileSystem) != 0) {
    result.error = errorMessage(
        "Unable to identify the UDF capture destination filesystem", errno);
    return result;
  }
  const std::string destinationDevice = destinationFileSystem.f_mntfromname;
  if (destinationDevice.rfind(selected.devicePath, 0) == 0 &&
      (destinationDevice.size() == selected.devicePath.size() ||
       destinationDevice[selected.devicePath.size()] == 's')) {
    result.error = "The UDF capture destination is stored on the source device";
    return result;
  }
  const DeviceDiscoveryResult discovery = discoverBlockDevicesInternal();
  const auto observed = std::find_if(
      discovery.devices.begin(), discovery.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  const core::SafetyPolicy safetyPolicy;
  if (observed == discovery.devices.end()) {
    result.error = "The selected UDF source device is no longer connected";
    return result;
  }
  const auto identity = safetyPolicy.validateIdentity(selected, *observed);
  if (!identity.safe() || observed->mountPoints.size() != 1U ||
      observed->mountPoints.front() != selected.mountPoints.front()) {
    result.error = !identity.safe()
                       ? identity.issues.front().message
                       : "The mounted UDF source volume changed after selection";
    return result;
  }
  const std::filesystem::path sourceRoot =
      std::filesystem::u8path(selected.mountPoints.front());
  if (!std::filesystem::is_directory(sourceRoot, fileError) || fileError ||
      access(sourceRoot.c_str(), R_OK | X_OK) != 0) {
    result.error = "The selected UDF source volume is not readable";
    return result;
  }
  auto partial = destination.parent_path() /
                 (destination.stem().string() + ".rufus-plus-plus-capture-" +
                  std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count()) +
                  destination.extension().string());
  if (std::filesystem::exists(partial, fileError) || fileError) {
    result.error = "Unable to allocate a private UDF capture path";
    return result;
  }
  struct PartialCleanup final {
    std::filesystem::path path;
    bool committed{};
    ~PartialCleanup() {
      if (!committed) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    }
  } cleanup{partial};

  const std::string outputPath = partial.string();
  const std::string sourcePath = sourceRoot.string();
  const std::string label = selected.displayName.empty()
                                ? std::string{"RUFUSPP_CAPTURE"}
                                : selected.displayName.substr(0U, 63U);
  std::vector<std::string> arguments{
      "/usr/bin/hdiutil", "makehybrid", "-o", outputPath, sourcePath,
      "-udf", "-default-volume-name", label, "-quiet"};
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1U);
  for (auto& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    result.error = "Unable to initialize the UDF capture provider";
    return result;
  }
  static_cast<void>(posix_spawn_file_actions_addopen(
      &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0));
  static_cast<void>(posix_spawn_file_actions_addopen(
      &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
  pid_t child = -1;
  const int spawned = posix_spawn(&child, "/usr/bin/hdiutil", &actions,
                                  nullptr, argv.data(), *_NSGetEnviron());
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    result.error = "Unable to start the macOS UDF capture provider: " +
                   std::string(std::strerror(spawned));
    return result;
  }
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Capturing, 0U,
                selected.capacityBytes});
  }
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno == EINTR) {
      continue;
    }
    if (waited < 0) {
      result.error = errorMessage("Unable to wait for the UDF capture provider",
                                  errno);
      return result;
    }
    if (cancelled(isCancelled)) {
      static_cast<void>(kill(child, SIGTERM));
      bool stopped = false;
      for (unsigned int attempt = 0; attempt < 20U; ++attempt) {
        const pid_t cancelledWait = waitpid(child, &status, WNOHANG);
        if (cancelledWait == child) {
          stopped = true;
          break;
        }
        if (cancelledWait < 0 && errno != EINTR) {
          break;
        }
        usleep(50000U);
      }
      if (!stopped) {
        static_cast<void>(kill(child, SIGKILL));
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
      }
      result.cancelled = true;
      result.error = "UDF ISO capture cancelled";
      return result;
    }
    usleep(100000U);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    result.error = "The macOS UDF capture provider failed";
    return result;
  }
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Finalizing,
                selected.capacityBytes, selected.capacityBytes});
  }
  const core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(partial);
  if (!analysis.succeeded() || analysis.image->format != core::ImageFormat::Iso ||
      !analysis.image->capabilities.udf ||
      !analysis.image->capabilities.isoExtraction) {
    result.error =
        "The generated UDF ISO failed structural and directory-tree validation";
    return result;
  }
  result.outputSizeBytes = std::filesystem::file_size(partial, fileError);
  if (fileError || result.outputSizeBytes == 0U) {
    result.error = "Unable to inspect the completed UDF ISO: " +
                   (fileError ? fileError.message()
                              : std::string("the provider produced an empty image"));
    return result;
  }
  std::filesystem::rename(partial, destination, fileError);
  if (fileError) {
    result.error = "Unable to commit the UDF ISO capture: " +
                   fileError.message();
    return result;
  }
  cleanup.committed = true;
  result.success = true;
  result.bytesCaptured = selected.capacityBytes;
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Complete, selected.capacityBytes,
                selected.capacityBytes});
  }
  return result;
}

DeviceDiscoveryResult MacOSBlockDeviceBackend::discover() const {
  return discoverBlockDevicesInternal();
}

core::RawWriteResult MacOSBlockDeviceBackend::writeRaw(
    const core::RawWritePlan& plan,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) const {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  core::RawWriteResult result;
  const auto availability = rawWriteAvailability(plan.target());
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  auto source = core::openRawImageSource(plan.image());
  if (!source.succeeded()) {
    result.error = source.error;
    return result;
  }
  return macos::writeRawLocally(plan, source.source.get(), onProgress,
                                isCancelled);
#else
  return macos::writeRawWithPrivilegedHelper(plan, onProgress, isCancelled);
#endif
}

RawWriteAvailability MacOSBlockDeviceBackend::badBlockTestAvailability(
    const core::BlockDeviceInfo& target) const {
  return rawWriteAvailability(target);
}

core::BadBlockTestResult MacOSBlockDeviceBackend::testBadBlocks(
    const core::BlockDeviceInfo& target,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled) const {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  core::BadBlockTestResult result;
  const auto availability = badBlockTestAvailability(target);
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  return macos::testBadBlocksLocally(target, options, onProgress,
                                     isCancelled);
#else
  return macos::testBadBlocksWithPrivilegedHelper(target, options, onProgress,
                                                   isCancelled);
#endif
}

RawWriteAvailability MacOSBlockDeviceBackend::captureAvailability(
    const core::BlockDeviceInfo& target,
    const core::MediaCaptureFormat format) const {
  if (format == core::MediaCaptureFormat::Ffu) {
    return {false, false,
            "This capture format requires a separate macOS provider"};
  }
  if (format == core::MediaCaptureFormat::UdfIso) {
    if (target.stableId.empty() || target.devicePath.empty() ||
        !target.wholeDevice || target.systemDevice || !target.removable) {
      return {false, false,
              "UDF capture requires an identified whole removable non-system device"};
    }
    if (target.mountPoints.size() != 1U) {
      return {false, false,
              "UDF capture requires exactly one mounted source volume"};
    }
    if (access(target.mountPoints.front().c_str(), R_OK | X_OK) != 0) {
      return {false, false,
              "The mounted UDF source volume is not readable"};
    }
    if (access("/usr/bin/hdiutil", X_OK) != 0) {
      return {false, false,
              "The macOS hdiutil UDF provider is unavailable"};
    }
    return {true, false,
            "The macOS filesystem-aware UDF provider is available"};
  }
  constexpr std::uint64_t maximumVhdBytes =
      2040ULL * 1024ULL * 1024ULL * 1024ULL;
  if ((format == core::MediaCaptureFormat::FixedVhd ||
       format == core::MediaCaptureFormat::DynamicVhd) &&
      target.capacityBytes > maximumVhdBytes) {
    return {false, false, "Classic VHD capture is limited to 2040 GiB"};
  }
  return rawWriteAvailability(target);
}

core::MediaCaptureResult MacOSBlockDeviceBackend::capture(
    const core::BlockDeviceInfo& target,
    const std::filesystem::path& destination,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) const {
  if (options.format == core::MediaCaptureFormat::Raw) {
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
    return captureRawInUnsignedRootMode(target, destination, options,
                                        onProgress, isCancelled);
#else
    return macos::captureRawWithPrivilegedHelper(
        target, destination, options, onProgress, isCancelled);
#endif
  }
  if (options.format == core::MediaCaptureFormat::UdfIso) {
    auto result = captureUdfIsoLocally(target, destination, onProgress,
                                       isCancelled);
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
    if (result.success &&
        !restoreInvokingUserOwnership(destination, result.error)) {
      result.success = false;
      std::error_code ignored;
      std::filesystem::remove(destination, ignored);
    }
#endif
    return result;
  }
  core::MediaCaptureResult result;
  const auto availability = captureAvailability(target, options.format);
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  std::error_code fileError;
  if (destination.empty() || std::filesystem::exists(destination, fileError) ||
      fileError) {
    result.error = fileError
                       ? "Unable to inspect the capture destination: " +
                             fileError.message()
                       : destination.empty()
                             ? "The capture destination is invalid"
                             : "The capture destination already exists";
    return result;
  }
  auto rawIntermediate = destination;
  rawIntermediate += ".rufus-plus-plus-verified-raw-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  struct IntermediateCleanup final {
    std::filesystem::path path;
    ~IntermediateCleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{rawIntermediate};
  const core::MediaCaptureOptions rawOptions{
      core::MediaCaptureFormat::Raw, options.transferBytes, true};
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  const auto raw = captureRawInUnsignedRootMode(
      target, rawIntermediate, rawOptions, onProgress, isCancelled);
#else
  const auto raw = macos::captureRawWithPrivilegedHelper(
      target, rawIntermediate, rawOptions, onProgress, isCancelled);
#endif
  if (!raw.success) {
    return raw;
  }
  core::ImageInfo rawImage;
  rawImage.path = rawIntermediate.string();
  rawImage.displayName = rawIntermediate.filename().string();
  rawImage.format = core::ImageFormat::Raw;
  rawImage.sizeBytes = target.capacityBytes;
  rawImage.expandedSizeBytes = target.capacityBytes;
  rawImage.containerPayloadSizeBytes = target.capacityBytes;
  rawImage.capabilities.rawWrite = true;
  auto source = core::openRawImageSource(rawImage, rawIntermediate);
  if (!source.succeeded()) {
    result.error = "Unable to reopen the verified raw capture for conversion: " +
                   source.error;
    return result;
  }
  const core::MediaCaptureWriter writer;
  result = writer.capture(*source.source, destination, options, onProgress,
                          isCancelled);
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  if (result.success &&
      !restoreInvokingUserOwnership(destination, result.error)) {
    result.success = false;
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
  }
#endif
  return result;
}

}  // namespace

namespace macos {

DeviceDiscoveryResult discoverBlockDevices() {
  return discoverBlockDevicesInternal();
}

core::RawWriteResult writeRawLocally(
    const core::RawWritePlan& plan, core::RawSourceIo* source,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) {
  return writeRawLocallyInternal(plan, source, onProgress, isCancelled);
}

core::BadBlockTestResult testBadBlocksLocally(
    const core::BlockDeviceInfo& target,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled) {
  return testBadBlocksLocallyInternal(target, options, onProgress, isCancelled);
}

core::MediaCaptureResult captureRawLocally(
    const core::BlockDeviceInfo& source, const int destinationDescriptor,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  return captureRawLocallyInternal(source, destinationDescriptor, options,
                                   onProgress, isCancelled);
}

}  // namespace macos

std::unique_ptr<BlockDeviceBackend> makeMacOSBlockDeviceBackend() {
  return std::make_unique<MacOSBlockDeviceBackend>();
}

}  // namespace rufus::backend
