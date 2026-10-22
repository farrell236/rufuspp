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

#include "rufus/backend/block_device_backend.hpp"
#include "rufus/core/image_analyzer.hpp"

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
}  // namespace

int main(const int argc, const char* const argv[]) {
  try {
    const auto backend = rufus::backend::makePlatformBlockDeviceBackend();
    check(backend != nullptr, "native backend unavailable");
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
