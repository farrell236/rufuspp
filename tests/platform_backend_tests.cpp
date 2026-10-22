/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <unistd.h>
#endif

#include "rufus/backend/block_device_backend.hpp"
#include "rufus/backend/ntfs_iso_image_stager.hpp"
#include "rufus/backend/standalone_filesystem_stager.hpp"
#include "rufus/backend/windows_to_go_image_stager.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "../src/platform/src/ntfs_iso_format.hpp"

namespace {
bool checkNtfsStagingRequirements() {
  using rufus::backend::estimateNtfsIsoStagingRequirements;
  constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t reserve = 256ULL * 1024ULL * 1024ULL;
#if defined(_WIN32)
  constexpr std::uint64_t copies = 1;
  constexpr bool rangeWrites = false;
#else
  constexpr std::uint64_t copies = 2;
  constexpr bool rangeWrites = true;
#endif
  const auto quick = estimateNtfsIsoStagingRequirements(10U * gib, 32U * gib, true);
  const auto full = estimateNtfsIsoStagingRequirements(10U * gib, 32U * gib, false);
  const auto largerTarget = estimateNtfsIsoStagingRequirements(10U * gib, 64U * gib, true);
  const auto rounded = estimateNtfsIsoStagingRequirements(11, 32U * gib, true);
  if (!quick.success || quick.temporaryBytes != copies * 11U * gib + reserve ||
      quick.writeSizePending != rangeWrites ||
      !full.success || full.temporaryBytes != copies * 32U * gib + reserve || full.writeSizePending ||
      !largerTarget.success || largerTarget.temporaryBytes != quick.temporaryBytes ||
      !rounded.success || rounded.temporaryBytes != copies * 13U + reserve) {
    std::cerr << "NTFS space/write estimates must match quick/full host staging and round up\n";
    return false;
  }
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  for (const auto& overflow : {
           estimateNtfsIsoStagingRequirements(maximum, 32U * gib, true),
           estimateNtfsIsoStagingRequirements(gib, maximum, false),
           estimateNtfsIsoStagingRequirements(gib, (maximum - reserve) / copies + 1U, false)}) {
    if (overflow.success || overflow.error.empty() || overflow.temporaryBytes != 0) {
      std::cerr << "Overflowing NTFS space estimates must fail without a wrapped byte count\n";
      return false;
    }
  }
  return true;
}
}  // namespace

int main() {
  if (!checkNtfsStagingRequirements()) return EXIT_FAILURE;
  const auto backend = rufus::backend::makePlatformBlockDeviceBackend();
  if (!backend) {
    std::cerr << "configured platform must provide a discovery backend\n";
    return EXIT_FAILURE;
  }

  const auto privilege = backend->privilegeStatus();
  if (privilege.detail.empty()) {
    std::cerr << "the platform backend must describe its current privilege route\n";
    return EXIT_FAILURE;
  }
  switch (privilege.route) {
    case rufus::backend::PrivilegeRoute::Unprivileged:
    case rufus::backend::PrivilegeRoute::Elevated:
    case rufus::backend::PrivilegeRoute::PrivilegedHelper:
      break;
  }

  const auto capabilities = backend->capabilities();
  if (!capabilities.physicalDeviceDiscovery) {
    std::cerr << "platform backend must expose physical-device discovery\n";
    return EXIT_FAILURE;
  }
  if (!capabilities.rawWrite || !capabilities.unmountVolumes ||
      !capabilities.exclusiveAccess || !capabilities.flush ||
      !capabilities.identityRevalidation || !capabilities.rawVerification ||
      !capabilities.badBlockTest) {
    std::cerr << "platform backend must expose the complete guarded raw-write contract\n";
    return EXIT_FAILURE;
  }
#if defined(_WIN32)
  if (!capabilities.ffuApply) {
    std::cerr << "the Windows backend must expose its DISM FFU provider\n";
    return EXIT_FAILURE;
  }
#else
  if (capabilities.ffuApply) {
    std::cerr << "non-Windows backends must not advertise the Windows FFU provider\n";
    return EXIT_FAILURE;
  }
#endif
#if defined(__APPLE__)
  if (!capabilities.macOsInstallerCreation) {
    std::cerr << "the macOS backend must expose native installer creation\n";
    return EXIT_FAILURE;
  }
#if RUFUSPP_MACOS_UNSIGNED_ROOT_MODE
  if (backend->name().find("unsigned root mode") == std::string_view::npos) {
    std::cerr << "the macOS root-mode backend must identify its privilege boundary\n";
    return EXIT_FAILURE;
  }
  const auto rootAuthorization = backend->requestRawWriteAuthorization();
  if (geteuid() == 0) {
    if (privilege.route != rufus::backend::PrivilegeRoute::Elevated) {
      std::cerr << "root mode must report the elevated privilege route\n";
      return EXIT_FAILURE;
    }
    if (!rootAuthorization.available ||
        rootAuthorization.authorizationCanBeRequested) {
      std::cerr << "root mode must accept an actual root effective UID without authorization\n";
      return EXIT_FAILURE;
    }
  } else if (rootAuthorization.available ||
             rootAuthorization.authorizationCanBeRequested ||
             rootAuthorization.reason.find("sudo") == std::string::npos) {
    std::cerr << "root mode must reject a non-root process with sudo guidance\n";
    return EXIT_FAILURE;
  } else if (privilege.route !=
             rufus::backend::PrivilegeRoute::Unprivileged) {
    std::cerr << "non-root mode must report the unprivileged route\n";
    return EXIT_FAILURE;
  }
#endif
#else
  if (capabilities.macOsInstallerCreation) {
    std::cerr << "non-macOS backends must not advertise macOS installer creation\n";
    return EXIT_FAILURE;
  }
#endif

  const auto result = backend->discover();
  for (const auto& device : result.devices) {
    if (device.stableId.empty() || device.devicePath.empty() || !device.wholeDevice ||
        device.capacityBytes == 0) {
      std::cerr << "discovery returned incomplete physical-device metadata for "
                << device.devicePath << '\n';
      return EXIT_FAILURE;
    }
  }

  const rufus::core::BlockDeviceInfo ineligibleTarget;
  const auto unavailable = backend->rawWriteAvailability(ineligibleTarget);
  if (unavailable.available || unavailable.reason.empty()) {
    std::cerr << "raw-write preflight must reject an ineligible target with a reason\n";
    return EXIT_FAILURE;
  }
  const auto badBlockUnavailable =
      backend->badBlockTestAvailability(ineligibleTarget);
  if (badBlockUnavailable.available || badBlockUnavailable.reason.empty()) {
    std::cerr << "bad-block preflight must reject an ineligible target with a reason\n";
    return EXIT_FAILURE;
  }

  rufus::core::MacOsInstallerInfo emptyInstaller;
  const auto macOsUnavailable =
      backend->macOsInstallerAvailability(emptyInstaller, ineligibleTarget);
  if (macOsUnavailable.available || macOsUnavailable.reason.empty()) {
    std::cerr << "macOS installer preflight must reject incomplete input with a reason\n";
    return EXIT_FAILURE;
  }

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(__APPLE__)
  const auto fakeInstaller = std::filesystem::temp_directory_path() /
                             ("rufus-plus-plus-fake-installer-" +
                              std::to_string(nonce) + ".app");
  std::filesystem::create_directories(fakeInstaller / "Contents" / "Resources");
  {
    std::ofstream tool(fakeInstaller / "Contents" / "Resources" /
                       "createinstallmedia");
    tool << "not an Apple installer";
  }
  std::filesystem::permissions(
      fakeInstaller / "Contents" / "Resources" / "createinstallmedia",
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
  const auto fakeAnalysis =
      backend->analyzeMacOsInstallerApplication(fakeInstaller);
  std::error_code fakeCleanupError;
  std::filesystem::remove_all(fakeInstaller, fakeCleanupError);
  if (fakeAnalysis.succeeded() || fakeAnalysis.error.empty()) {
    std::cerr << "macOS installer analysis accepted an unsigned fake application\n";
    return EXIT_FAILURE;
  }
#endif
  const auto layoutPath = std::filesystem::temp_directory_path() /
                          ("rufus-plus-plus-wtg-layout-test-" + std::to_string(nonce) + ".img");
  constexpr std::uint64_t layoutBytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
  const auto layout =
      rufus::backend::createWindowsToGoGptImage(layoutPath, layoutBytes);
  if (!layout.success) {
    std::cerr << "Windows To Go GPT layout creation failed: " << layout.error << '\n';
    return EXIT_FAILURE;
  }
  const rufus::core::ImageAnalyzer analyzer;
  const auto analyzedLayout = analyzer.analyze(layoutPath);
  std::error_code ignored;
  std::filesystem::remove(layoutPath, ignored);
  if (!analyzedLayout.succeeded() ||
      analyzedLayout.image->partitionScheme != rufus::core::PartitionScheme::Gpt ||
      !analyzedLayout.image->capabilities.validPartitionTable ||
      analyzedLayout.image->sizeBytes != layoutBytes) {
    std::cerr << "Windows To Go staging layout did not validate as a complete GPT image\n";
    return EXIT_FAILURE;
  }

  const auto ntfsLayoutPath = std::filesystem::temp_directory_path() /
                              ("rufus-plus-plus-ntfs-layout-test-" +
                               std::to_string(nonce) + ".img");
  constexpr std::uint64_t ntfsLayoutBytes =
      64ULL * 1024ULL * 1024ULL;
#if !defined(_WIN32)
  const std::string ntfsStagingDisk = "/private/staging/filesystem.ntfs";
  const std::string ntfsPartitionDevice = ntfsStagingDisk;
  const auto ntfsQuickCommand = rufus::backend::detail::ntfsIsoFormatCommand(
      ntfsStagingDisk, true, 0U, "Windows Setup");
  const auto ntfsFullCommand = rufus::backend::detail::ntfsIsoFormatCommand(
      ntfsStagingDisk, false, 8192U, "Windows Setup");
  const auto optionValue = [](const std::vector<std::string>& command,
                               const std::string& option) {
    const auto found = std::find(command.begin(), command.end(), option);
    return found != command.end() && found + 1 != command.end()
               ? *(found + 1) : std::string{};
  };
  if (ntfsQuickCommand.front() != "rufus-mkntfs" ||
      ntfsQuickCommand.back() != ntfsPartitionDevice ||
      ntfsFullCommand.back() != ntfsPartitionDevice ||
      optionValue(ntfsQuickCommand, "-s") != "512" ||
      optionValue(ntfsQuickCommand, "-p") != "2048" ||
      optionValue(ntfsQuickCommand, "-S") != "63" ||
      optionValue(ntfsQuickCommand, "-H") != "255" ||
      optionValue(ntfsQuickCommand, "-L") != "Windows Setup" ||
      std::find(ntfsQuickCommand.begin(), ntfsQuickCommand.end(), "-Q") ==
          ntfsQuickCommand.end() ||
      !optionValue(ntfsQuickCommand, "-c").empty() ||
      std::find(ntfsFullCommand.begin(), ntfsFullCommand.end(), "-Q") !=
          ntfsFullCommand.end() ||
      optionValue(ntfsFullCommand, "-c") != "8192") {
    std::cerr << "NTFS formatter must use an ordinary partition file with explicit geometry\n";
    return EXIT_FAILURE;
  }
#endif
  const auto ntfsLayout = rufus::backend::createNtfsIsoMbrImage(
      ntfsLayoutPath, ntfsLayoutBytes);
  if (!ntfsLayout.success) {
    std::cerr << "NTFS/UEFI:NTFS layout creation failed: "
              << ntfsLayout.error << '\n';
    return EXIT_FAILURE;
  }
  std::array<unsigned char, 512> mbr{};
  std::array<unsigned char, 512> uefiBootSector{};
  std::ifstream ntfsImage(ntfsLayoutPath, std::ios::binary);
  ntfsImage.read(reinterpret_cast<char*>(mbr.data()),
                 static_cast<std::streamsize>(mbr.size()));
  const std::uint32_t bootStart =
      static_cast<std::uint32_t>(mbr[462 + 8]) |
      static_cast<std::uint32_t>(mbr[462 + 9]) << 8U |
      static_cast<std::uint32_t>(mbr[462 + 10]) << 16U |
      static_cast<std::uint32_t>(mbr[462 + 11]) << 24U;
  const auto littleEndian32 = [](const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           static_cast<std::uint32_t>(bytes[1]) << 8U |
           static_cast<std::uint32_t>(bytes[2]) << 16U |
           static_cast<std::uint32_t>(bytes[3]) << 24U;
  };
  ntfsImage.seekg(static_cast<std::streamoff>(bootStart) * 512);
  ntfsImage.read(reinterpret_cast<char*>(uefiBootSector.data()),
                 static_cast<std::streamsize>(uefiBootSector.size()));
  ntfsImage.close();
  std::filesystem::remove(ntfsLayoutPath, ignored);
  if (mbr[510] != 0x55U || mbr[511] != 0xaaU || mbr[446 + 4] != 0x07U ||
      mbr[462 + 4] != 0xefU || bootStart == 0U ||
      littleEndian32(mbr.data() + 446U + 8U) !=
          rufus::backend::detail::kNtfsIsoFirstSector ||
      std::string(reinterpret_cast<const char*>(uefiBootSector.data() + 3U),
                  8U) != "mkfs.fat" ||
      uefiBootSector[510] != 0x55U || uefiBootSector[511] != 0xaaU) {
    std::cerr << "NTFS staging layout or embedded UEFI:NTFS image is invalid\n";
    return EXIT_FAILURE;
  }

  const auto gptNtfsLayoutPath = std::filesystem::temp_directory_path() /
                                 ("rufus-plus-plus-gpt-ntfs-layout-test-" +
                                  std::to_string(nonce) + ".img");
  const auto gptNtfsLayout = rufus::backend::createNtfsIsoDiskImage(
      gptNtfsLayoutPath, ntfsLayoutBytes,
      rufus::core::PartitionScheme::Gpt);
  if (!gptNtfsLayout.success) {
    std::cerr << "GPT NTFS/UEFI:NTFS layout creation failed: "
              << gptNtfsLayout.error << '\n';
    return EXIT_FAILURE;
  }
  const auto analyzedGptNtfs = analyzer.analyze(gptNtfsLayoutPath);
  std::ifstream gptNtfsImage(gptNtfsLayoutPath, std::ios::binary);
  std::array<unsigned char, 512> protectiveMbr{};
  std::array<char, 8> primaryGpt{};
  std::array<char, 8> backupGpt{};
  std::array<unsigned char, 128> ntfsGptEntry{};
  gptNtfsImage.read(reinterpret_cast<char*>(protectiveMbr.data()),
                    protectiveMbr.size());
  gptNtfsImage.seekg(512U);
  gptNtfsImage.read(primaryGpt.data(), primaryGpt.size());
  gptNtfsImage.seekg(2U * 512U);
  gptNtfsImage.read(reinterpret_cast<char*>(ntfsGptEntry.data()),
                    ntfsGptEntry.size());
  gptNtfsImage.seekg(static_cast<std::streamoff>(ntfsLayoutBytes - 512U));
  gptNtfsImage.read(backupGpt.data(), backupGpt.size());
  gptNtfsImage.close();
  std::filesystem::remove(gptNtfsLayoutPath, ignored);
  if (!analyzedGptNtfs.succeeded() ||
      analyzedGptNtfs.image->partitionScheme !=
          rufus::core::PartitionScheme::Gpt ||
      protectiveMbr[450U] != 0xeeU ||
      littleEndian32(ntfsGptEntry.data() + 32U) !=
          rufus::backend::detail::kNtfsIsoFirstSector ||
      littleEndian32(ntfsGptEntry.data() + 36U) != 0U ||
      std::string(primaryGpt.data(), primaryGpt.size()) != "EFI PART" ||
      std::string(backupGpt.data(), backupGpt.size()) != "EFI PART") {
    std::cerr << "GPT NTFS staging layout did not validate both GPT headers\n";
    return EXIT_FAILURE;
  }

  const auto windowsToGo =
      rufus::backend::makePlatformWindowsToGoImageStager();
  if (!windowsToGo ||
      (!windowsToGo->availability().available &&
       windowsToGo->availability().reason.empty())) {
    std::cerr << "Windows To Go preflight must return an actionable availability result\n";
    return EXIT_FAILURE;
  }
  const auto ntfsIso = rufus::backend::makePlatformNtfsIsoImageStager();
  if (!ntfsIso ||
      (!ntfsIso->availability().available &&
       ntfsIso->availability().reason.empty())) {
    std::cerr << "NTFS ISO preflight must return an actionable availability result\n";
    return EXIT_FAILURE;
  }
  const auto filesystemStager =
      rufus::backend::makePlatformStandaloneFilesystemStager();
  if (!filesystemStager) {
    std::cerr << "the platform must provide a standalone filesystem stager\n";
    return EXIT_FAILURE;
  }
  rufus::core::BlockDeviceInfo formatTarget;
  formatTarget.capacityBytes = 128ULL * 1024ULL * 1024ULL;
  formatTarget.logicalSectorSize = 512U;
  for (const auto filesystem : {
           rufus::backend::StandaloneFilesystem::Ntfs,
           rufus::backend::StandaloneFilesystem::UefiNtfs,
           rufus::backend::StandaloneFilesystem::ExFat,
           rufus::backend::StandaloneFilesystem::Udf,
           rufus::backend::StandaloneFilesystem::ReFs,
           rufus::backend::StandaloneFilesystem::Ext3}) {
    const auto formatAvailability =
        filesystemStager->availability(filesystem, formatTarget);
    if (!formatAvailability.available && formatAvailability.reason.empty()) {
      std::cerr << "filesystem staging preflight must explain unavailable providers\n";
      return EXIT_FAILURE;
    }
  }
#if defined(__APPLE__)
  if (std::getenv("RUFUSPP_TEST_HOST_FORMATTERS") != nullptr) {
    for (const auto filesystem : {
             rufus::backend::StandaloneFilesystem::ExFat,
             rufus::backend::StandaloneFilesystem::Udf}) {
      if (!filesystemStager->availability(filesystem, formatTarget).available) {
        continue;
      }
      const auto formatPath =
          std::filesystem::temp_directory_path() /
          ("rufus-plus-plus-host-format-test-" + std::to_string(nonce) + "-" +
           rufus::backend::standaloneFilesystemName(filesystem) + ".img");
      const auto formatted = filesystemStager->stage(
          filesystem, formatTarget, formatPath, "RUFUSPP_TEST");
      std::filesystem::remove(formatPath, ignored);
      if (!formatted.success || !formatted.stagedImage.has_value()) {
        std::cerr << "macOS host formatter integration failed for "
                  << rufus::backend::standaloneFilesystemName(filesystem)
                  << ": " << formatted.error << '\n';
        return EXIT_FAILURE;
      }
    }
  }
#endif

  std::cout << backend->name() << " discovered " << result.devices.size()
            << " physical device(s) with " << result.warnings.size() << " warning(s)\n";
  return EXIT_SUCCESS;
}
