/* SPDX-License-Identifier: GPL-3.0-or-later */
// Opt-in physical qualification. Never registered as an automatic CTest.
// Writes use the unmodified production backend's guarded root-mode path.
#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>

#include "rufus/backend/block_device_backend.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/standalone_media.hpp"

namespace {
using namespace rufus::core;
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void check(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
std::string quoted(const std::string& value) {
  const char* hex = "0123456789abcdef";
  std::string output = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') { output += '\\'; output += static_cast<char>(byte); }
    else if (byte < 32U) { output += "\\u00"; output += hex[byte >> 4U]; output += hex[byte & 15U]; }
    else output += static_cast<char>(byte);
  }
  return output + '"';
}
void eligibleUsb(const BlockDeviceInfo& device, const bool requireSerial) {
  check(device.bus == DeviceBus::Usb && !device.systemDevice && device.wholeDevice &&
            device.writable && evaluateDeviceEligibility(device) == DeviceEligibility::Eligible,
        "only an eligible, non-system whole USB device is accepted");
  check(!device.stableId.empty() && (!requireSerial || !device.serialNumber.empty()),
        "physical qualification requires a reported serial and stable identity");
  check(device.logicalSectorSize == 512U && device.capacityBytes >= 4ULL * 1024U * 1024U * 1024U &&
            device.capacityBytes <= 64ULL * 1024U * 1024U * 1024U,
        "qualification is bounded to 4..64 GiB USB devices with 512-byte sectors");
}
void printDevice(const BlockDeviceInfo& device) {
  std::cout << "{\"devicePath\":" << quoted(device.devicePath)
            << ",\"stableId\":" << quoted(device.stableId)
            << ",\"serial\":" << quoted(device.serialNumber)
            << ",\"vendor\":" << quoted(device.vendor)
            << ",\"model\":" << quoted(device.model)
            << ",\"capacityBytes\":" << device.capacityBytes
            << ",\"sectorBytes\":" << device.logicalSectorSize
            << ",\"bus\":\"USB\",\"wholeDevice\":true,\"writable\":true,\"systemDevice\":false"
            << ",\"mountPoints\":[";
  bool first = true;
  for (const auto& mount : device.mountPoints) {
    if (!first) std::cout << ',';
    first = false;
    std::cout << quoted(mount);
  }
  std::cout << "]}\n";
}
BlockDeviceInfo select(const rufus::backend::BlockDeviceBackend& backend,
                        const std::string& value, const bool bySerial, const bool requireSerial = true) {
  const auto discovery = backend.discover();
  for (const auto& warning : discovery.warnings) std::cerr << warning << '\n';
  std::vector<BlockDeviceInfo> matches;
  for (const auto& device : discovery.devices) {
    const bool matchesValue = bySerial ? device.serialNumber == value
        : std::find(device.mountPoints.begin(), device.mountPoints.end(), value) != device.mountPoints.end();
    if (matchesValue) matches.push_back(device);
  }
  check(matches.size() == 1U, "expected exactly one USB device matching the selected mount/serial");
  eligibleUsb(matches.front(), requireSerial);
  return matches.front();
}
std::uint64_t number(const char* value) {
  const std::string text(value);
  check(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos,
        "expected an unsigned identity number");
  return std::stoull(text);
}
MediaCaptureFormat captureFormat(const std::string& name) {
  if (name == "raw") return MediaCaptureFormat::Raw;
  if (name == "fixed-vhd") return MediaCaptureFormat::FixedVhd;
  if (name == "dynamic-vhd") return MediaCaptureFormat::DynamicVhd;
  if (name == "dynamic-vhdx") return MediaCaptureFormat::DynamicVhdx;
  throw std::runtime_error("expected raw, fixed-vhd, dynamic-vhd or dynamic-vhdx");
}
ImageInfo checkedCapture(const std::filesystem::path& source,
                         const MediaCaptureFormat format, const std::uint64_t capacity) {
  check(std::filesystem::is_regular_file(source), "capture must be an ordinary file");
  const auto analysis = ImageAnalyzer{}.analyze(source);
  const auto expected = format == MediaCaptureFormat::Raw ? ImageFormat::Raw :
      format == MediaCaptureFormat::DynamicVhdx ? ImageFormat::Vhdx : ImageFormat::Vhd;
  check(analysis.succeeded() && analysis.image->format == expected &&
            analysis.image->capabilities.rawWrite && analysis.image->deploymentSizeBytes() == capacity,
        "capture format or decoded size does not match the authorized USB");
  auto decoded = openRawImageSource(*analysis.image, source);
  check(decoded.succeeded() && decoded.source->sizeBytes() == capacity,
        "capture decoder rejected the captured disk");
  return *analysis.image;
}
BlockDeviceInfo pinnedTarget(const rufus::backend::BlockDeviceBackend& backend,
                              const char* serial, const char* stable,
                              const char* capacity, const char* sector) {
  const auto target = select(backend, serial, true);
  check(target.serialNumber == serial && target.stableId == stable &&
            target.capacityBytes == number(capacity) && target.logicalSectorSize == number(sector),
        "USB identity or geometry does not match the authorized snapshot");
  return target;
}
void outsideTarget(const std::filesystem::path& path, const BlockDeviceInfo& target) {
  struct stat sourceStatus {};
  check(stat(path.c_str(), &sourceStatus) == 0, "unable to inspect local artifact storage");
  for (const auto& mount : target.mountPoints) {
    struct stat mountStatus {};
    check(stat(mount.c_str(), &mountStatus) == 0 && sourceStatus.st_dev != mountStatus.st_dev,
          "test artifacts must be stored outside the USB");
  }
}
int writeVerified(const rufus::backend::BlockDeviceBackend& backend,
                  const ImageInfo& image, const BlockDeviceInfo& target, const bool clearTail) {
  const auto availability = backend.rawWriteAvailability(target);
  check(availability.available, availability.reason);
  const auto planning = WritePlanBuilder{}.buildRawWriteWithVerification(
      image, target, VerificationProfile::Standard, 1024U * 1024U, clearTail);
  check(planning.succeeded(), "guarded write plan rejected the image or target");
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  RawWriteStage lastStage = RawWriteStage::Revalidating;
  std::uint64_t lastBytes = 0U;
  const auto result = backend.writeRaw(*planning.plan, [&](const auto& progress) {
    if (progress.stage != lastStage || progress.bytesProcessed < lastBytes ||
        progress.bytesProcessed - lastBytes >= 128U * 1024U * 1024U ||
        progress.bytesProcessed == progress.totalBytes) {
      std::cerr << rawWriteStageName(progress.stage) << ": " << progress.bytesProcessed
                << '/' << progress.totalBytes << '\n';
      lastStage = progress.stage;
      lastBytes = progress.bytesProcessed;
    }
  }, [] { return interrupted != 0; });
  std::cout << "{\"success\":" << (result.success ? "true" : "false")
            << ",\"cancelled\":" << (result.cancelled ? "true" : "false")
            << ",\"destructiveWriteStarted\":" << (result.destructiveWriteStarted ? "true" : "false")
            << ",\"bytesWritten\":" << result.bytesWritten
            << ",\"bytesVerified\":" << result.bytesVerified
            << ",\"verificationCompleted\":" << (result.verificationCompleted ? "true" : "false")
            << ",\"error\":" << quoted(result.error) << "}\n";
  return result.success && result.verificationCompleted ? 0 : 1;
}
// Manual qualification only: keep the host filesystem driver from mounting
// and updating FAT metadata between native verification and independent reads.
// This process does not claim or open the device and cannot write to it.
struct MountGuardIdentity final {
  const rufus::backend::BlockDeviceBackend* backend{};
  BlockDeviceInfo expected;
};
DADissenterRef denyTestUsbMount(DADiskRef disk, void* context) {
  const auto& identity = *static_cast<const MountGuardIdentity*>(context);
  DADiskRef whole = DADiskCopyWholeDisk(disk);
  const char* observed = whole == nullptr ? nullptr : DADiskGetBSDName(whole);
  const std::string name = observed == nullptr ? "" : observed;
  if (whole != nullptr) CFRelease(whole);
  bool matches = false;
  // Discovery uses IOKit and mount-table reads, with no DA request/claim. Match
  // the pinned physical identity on every callback, including after a replug
  // changes the disk number. This can be armed before the USB is connected.
  for (const auto& device : identity.backend->discover().devices) {
    if (std::filesystem::path(device.devicePath).filename() == name &&
        device.serialNumber == identity.expected.serialNumber &&
        device.stableId == identity.expected.stableId &&
        device.capacityBytes == identity.expected.capacityBytes &&
        device.logicalSectorSize == identity.expected.logicalSectorSize &&
        device.bus == DeviceBus::Usb && device.wholeDevice && !device.systemDevice) {
      matches = true;
    }
  }
  if (!matches) return nullptr;
  std::cout << "{\"mountDenied\":true,\"wholeDisk\":" << quoted(name) << "}" << std::endl;
  return DADissenterCreate(kCFAllocatorDefault, kDAReturnNotPermitted,
      CFSTR("Rufus++ physical qualification is keeping this USB unmounted"));
}
int guardHostMounts(const rufus::backend::BlockDeviceBackend& backend, const BlockDeviceInfo& target) {
  const auto parent = getppid();
  MountGuardIdentity identity{&backend, target};
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  check(session != nullptr, "unable to create mount-approval session");
  auto* context = &identity;
  DARegisterDiskMountApprovalCallback(session, nullptr, denyTestUsbMount, context);
  CFRunLoopRef loop = CFRunLoopGetCurrent();
  DASessionScheduleWithRunLoop(session, loop, kCFRunLoopDefaultMode);
  std::signal(SIGINT, SIG_IGN);
  std::signal(SIGTERM, stop);
  std::cout << "{\"mountGuardReady\":true,\"serial\":" << quoted(target.serialNumber) << "}" << std::endl;
  while (interrupted == 0 && getppid() == parent) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, true);
  }
  DAUnregisterApprovalCallback(session, reinterpret_cast<void*>(denyTestUsbMount), context);
  DASessionUnscheduleFromRunLoop(session, loop, kCFRunLoopDefaultMode);
  CFRelease(session);
  return 0;
}
}  // namespace

int main(const int argc, const char* const argv[]) {
  try {
    const auto backend = rufus::backend::makePlatformBlockDeviceBackend();
    check(backend != nullptr, "native backend unavailable");
    // These manual-only actions preserve the production capture/format/write
    // path. Every device action requires a serial, stable ID and exact geometry.
    const std::string action = argc > 1 ? argv[1] : "";
    if (action == "--guard-host-mounts" && argc == 6) {
      BlockDeviceInfo identity;
      identity.serialNumber = argv[2];
      identity.stableId = argv[3];
      identity.capacityBytes = number(argv[4]);
      identity.logicalSectorSize = static_cast<std::uint32_t>(number(argv[5]));
      check(!identity.serialNumber.empty() && identity.capacityBytes >= 4ULL * 1024U * 1024U * 1024U &&
                identity.capacityBytes <= 64ULL * 1024U * 1024U * 1024U && number(argv[5]) == 512U &&
                identity.stableId == "macos:" + identity.serialNumber + ':' + std::to_string(identity.capacityBytes),
            "mount guard requires a complete bounded macOS physical identity");
      return guardHostMounts(*backend, identity);
    }
    if (action == "--inspect-capture" && argc == 5) {
      const auto image = checkedCapture(std::filesystem::canonical(argv[2]),
                                        captureFormat(argv[3]), number(argv[4]));
      std::cout << "{\"success\":true,\"decodedBytes\":" << image.deploymentSizeBytes() << "}\n";
      return 0;
    }
    if (action == "--format-fat32-authorized" && argc == 7) {
      const auto target = pinnedTarget(*backend, argv[3], argv[4], argv[5], argv[6]);
      const auto output = std::filesystem::absolute(argv[2]);
      check(!std::filesystem::exists(output) && !std::filesystem::is_symlink(output),
            "format staging destination must be new");
      outsideTarget(output.parent_path(), target);
      const auto staged = stageBlankFat32Media(target, output, "RUFUS_TEST");
      check(staged.success && staged.stagedImage.has_value(), "FAT32 staging failed: " + staged.error);
      return writeVerified(*backend, *staged.stagedImage, target, true);
    }
    if (action == "--restore-capture-authorized" && argc == 8) {
      const auto format = captureFormat(argv[3]);
      const auto target = pinnedTarget(*backend, argv[4], argv[5], argv[6], argv[7]);
      const auto source = std::filesystem::canonical(argv[2]);
      outsideTarget(source, target);
      const auto image = checkedCapture(source, format, target.capacityBytes);
      return writeVerified(*backend, image, target, false);
    }
    if (action == "--capture-authorized" && argc == 8) {
      const auto format = captureFormat(argv[2]);
      const auto target = pinnedTarget(*backend, argv[4], argv[5], argv[6], argv[7]);
      const auto destination = std::filesystem::absolute(argv[3]);
      outsideTarget(destination.parent_path(), target);
      const auto availability = backend->captureAvailability(target, format);
      check(availability.available, availability.reason);
      std::signal(SIGINT, stop);
      std::signal(SIGTERM, stop);
      MediaCaptureStage lastStage = MediaCaptureStage::Complete;
      std::uint64_t lastBytes = 0U;
      const auto result = backend->capture(target, destination, {format, 8U * 1024U * 1024U, true},
          [&](const auto& progress) {
            if (progress.stage != lastStage || progress.bytesProcessed < lastBytes ||
                progress.bytesProcessed - lastBytes >= 128U * 1024U * 1024U ||
                progress.bytesProcessed == progress.totalBytes) {
              std::cerr << mediaCaptureStageName(progress.stage) << ": " << progress.bytesProcessed
                        << '/' << progress.totalBytes << '\n';
              lastStage = progress.stage;
              lastBytes = progress.bytesProcessed;
            }
          }, [] { return interrupted != 0; });
      std::cout << "{\"success\":" << (result.success ? "true" : "false")
                << ",\"cancelled\":" << (result.cancelled ? "true" : "false")
                << ",\"bytesCaptured\":" << result.bytesCaptured
                << ",\"outputSizeBytes\":" << result.outputSizeBytes
                << ",\"error\":" << quoted(result.error) << "}\n";
      return result.success ? 0 : 1;
    }
    if (argc == 3 && (std::string(argv[1]) == "--inspect-staged-gpt" ||
                      std::string(argv[1]) == "--inspect-staged-mbr")) {
      const bool gpt = std::string(argv[1]) == "--inspect-staged-gpt";
      const auto source = std::filesystem::canonical(std::filesystem::u8path(argv[2]));
      struct stat status {};
      check(stat(source.c_str(), &status) == 0 && S_ISREG(status.st_mode),
            "staged source must be an ordinary file");
      const auto analysis = ImageAnalyzer{}.analyze(source);
      check(analysis.succeeded() && analysis.image->format == ImageFormat::Raw &&
                analysis.image->partitionScheme == (gpt ? PartitionScheme::Gpt : PartitionScheme::Mbr) &&
                analysis.image->capabilities.rawWrite,
            "native analyzer did not recognize the staged raw partitioned image");
      std::cout << "{\"success\":true,\"partitionScheme\":" << quoted(gpt ? "GPT" : "MBR")
                << ",\"capacityBytes\":"
                << analysis.image->sizeBytes << "}\n";
      return 0;
    }
    if (argc == 3) {
      const std::string action(argv[1]);
      check(action == "--inspect-mounted" || action == "--inspect-serial", "unknown read-only action");
      printDevice(select(*backend, argv[2], action == "--inspect-serial", false));
      return 0;
    }
    const bool stagedGpt = argc >= 9 && std::string(argv[1]) == "--write-staged-gpt-authorized";
    const bool stagedMbr = argc >= 9 && (std::string(argv[1]) == "--write-staged-mbr-bios-authorized" ||
                                       std::string(argv[1]) == "--write-staged-mbr-dual-authorized");
    const bool staged = stagedGpt || stagedMbr;
    check(staged || (argc == 8 && std::string(argv[1]) == "--write-authorized"),
          "write requires an explicitly authorized mount, stable ID, serial and exact geometry");
    const auto source = std::filesystem::canonical(std::filesystem::u8path(argv[2]));
    const auto target = select(*backend, argv[3], staged);
    check(target.stableId == argv[4] && target.serialNumber == argv[staged ? 3 : 5] &&
              target.capacityBytes == number(argv[staged ? 5 : 6]) &&
              target.logicalSectorSize == number(argv[staged ? 6 : 7]),
          "USB identity or geometry does not match the authorized snapshot");
    struct stat sourceStatus {};
    check(stat(source.c_str(), &sourceStatus) == 0 && S_ISREG(sourceStatus.st_mode),
          "source must be a regular file");
    for (const auto& mount : target.mountPoints) {
      struct stat mountStatus {};
      check(stat(mount.c_str(), &mountStatus) == 0 && S_ISDIR(mountStatus.st_mode) &&
                sourceStatus.st_dev != mountStatus.st_dev,
            "source must be stored outside every target volume");
    }
    const auto availability = backend->rawWriteAvailability(target);
    check(availability.available, availability.reason);
    const auto analysis = ImageAnalyzer{}.analyze(source);
    check(analysis.succeeded() && analysis.image->capabilities.rawWrite &&
              (staged ? (analysis.image->format == ImageFormat::Raw &&
                            analysis.image->partitionScheme == (stagedGpt ? PartitionScheme::Gpt : PartitionScheme::Mbr) &&
                            analysis.image->sizeBytes == target.capacityBytes)
                         : (analysis.image->format == ImageFormat::Iso &&
                            analysis.image->capabilities.uefiBootable)),
          "expected the authorized raw-writable ISO or full-size staged image");
    std::vector<RawWriteRange> ranges;
    if (staged) {
      check((argc - 7) % 2 == 0 && (argc - 7) / 2 <= 128,
            "staged write requires a bounded initialized-range list");
      for (int index = 7; index < argc; index += 2) {
        ranges.push_back({number(argv[index]), number(argv[index + 1])});
      }
      check(!ranges.empty() && ranges.front().offsetBytes == 0U &&
                ranges.front().sizeBytes >= 1024U * 1024U &&
                ranges.back().offsetBytes <= target.capacityBytes &&
                ranges.back().sizeBytes == target.capacityBytes - ranges.back().offsetBytes,
            "staged write ranges must include the first MiB and actual device end");
    }
    const auto planning = WritePlanBuilder{}.buildRawWriteWithVerification(
        *analysis.image, target, VerificationProfile::Standard, 1024U * 1024U, !staged, ranges);
    check(planning.succeeded(), "guarded write plan rejected the source or target");
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    RawWriteStage lastStage = RawWriteStage::Revalidating;
    std::uint64_t lastBytes = 0U;
    std::cerr << "Authorized physical USB target: " << target.vendor << ' ' << target.model
              << ", serial " << target.serialNumber << ", " << target.capacityBytes << " bytes\n";
    const auto result = backend->writeRaw(*planning.plan, [&](const auto& progress) {
      if (progress.stage != lastStage || progress.bytesProcessed < lastBytes ||
          progress.bytesProcessed - lastBytes >= 32U * 1024U * 1024U ||
          progress.bytesProcessed == progress.totalBytes) {
        std::cerr << rawWriteStageName(progress.stage) << ": " << progress.bytesProcessed
                  << '/' << progress.totalBytes << '\n';
        lastStage = progress.stage;
        lastBytes = progress.bytesProcessed;
      }
    }, [] { return interrupted != 0; });
    std::cout << "{\"success\":" << (result.success ? "true" : "false")
              << ",\"cancelled\":" << (result.cancelled ? "true" : "false")
              << ",\"destructiveWriteStarted\":" << (result.destructiveWriteStarted ? "true" : "false")
              << ",\"bytesWritten\":" << result.bytesWritten
              << ",\"bytesVerified\":" << result.bytesVerified
              << ",\"verificationCompleted\":" << (result.verificationCompleted ? "true" : "false")
              << ",\"error\":" << quoted(result.error) << "}\n";
    return result.success && result.verificationCompleted ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Physical USB qualification refused/failed: " << error.what() << '\n';
    return 1;
  }
}
