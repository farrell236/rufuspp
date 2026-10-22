/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/backend/windows_to_go_image_stager.hpp"
#include "rufus/backend/ntfs_iso_image_stager.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rufus/core/wim_applier.hpp"
#include "ntfs_iso_format.hpp"
#include "ntfs_bootstrap.hpp"
#include "../../portable/src/windows_boot_code.hpp"
#include "uefi_ntfs_bootstrap_data.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>
#include <process.h>
#else
#include "ntfs_file_tree.hpp"
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rufus::backend {
namespace {

constexpr std::uint64_t kSectorSize = detail::kNtfsIsoSectorSize;
constexpr std::uint64_t kPartitionAlignmentSectors = detail::kNtfsIsoFirstSector;
constexpr std::uint64_t kEspSectors = 260ULL * 1024ULL * 1024ULL / kSectorSize;
constexpr std::uint64_t kMsrSectors = 16ULL * 1024ULL * 1024ULL / kSectorSize;
constexpr std::size_t kGptEntryCount = 128;
constexpr std::size_t kGptEntrySize = 128;
constexpr std::uint64_t kGptEntrySectors =
    kGptEntryCount * kGptEntrySize / kSectorSize;
constexpr std::uint64_t kStagingReserveBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kUefiNtfsPartitionBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetTailReserveBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kNtfsStagingReserveBytes = 256ULL * 1024ULL * 1024ULL;

bool resizeSparseImage(const std::filesystem::path& path,
                       const std::uint64_t sizeBytes, std::string& error) {
#if defined(_WIN32)
  if (sizeBytes >
      static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
    error = "The staging image exceeds the Windows file-size limit";
    return false;
  }
  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "Unable to open the Windows staging image for sparse allocation (Windows error " +
            std::to_string(GetLastError()) + ')';
    return false;
  }
  DWORD returned = 0;
  if (!DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                       &returned, nullptr)) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    error = "The temporary filesystem cannot create sparse staging images (Windows error " +
            std::to_string(code) + ')';
    return false;
  }
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(sizeBytes);
  if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(file)) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    error = "Unable to size the sparse Windows staging image (Windows error " +
            std::to_string(code) + ')';
    return false;
  }
  if (!CloseHandle(file)) {
    error = "Unable to close the sparse Windows staging image (Windows error " +
            std::to_string(GetLastError()) + ')';
    return false;
  }
  return true;
#else
  std::error_code fileError;
  std::filesystem::resize_file(path, sizeBytes, fileError);
  if (fileError) {
    error = "Unable to size the sparse staging image: " + fileError.message();
    return false;
  }
  return true;
#endif
}

void put16(unsigned char* output, const std::uint16_t value) {
  output[0] = static_cast<unsigned char>(value & 0xffU);
  output[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put32(unsigned char* output, const std::uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index) {
    output[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

void put64(unsigned char* output, const std::uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index) {
    output[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

bool decodeUefiNtfsImage(std::vector<unsigned char>& decoded,
                         std::string& error) {
  const auto valueOf = [](const char character) -> int {
    if (character >= 'A' && character <= 'Z') {
      return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
      return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
      return character - '0' + 52;
    }
    return character == '+' ? 62 : character == '/' ? 63 : -1;
  };
  decoded.clear();
  decoded.reserve(static_cast<std::size_t>(kUefiNtfsPartitionBytes));
  for (std::size_t part = 0;
       part < std::size(detail::kUefiNtfsImageBase64); ++part) {
    const std::string_view encoded = detail::kUefiNtfsImageBase64[part];
    if (encoded.empty() || encoded.size() % 4U != 0U) {
      error = "The embedded UEFI:NTFS image has invalid encoding";
      return false;
    }
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4U) {
      const int first = valueOf(encoded[offset]);
      const int second = valueOf(encoded[offset + 1U]);
      const int third = encoded[offset + 2U] == '='
                            ? 0
                            : valueOf(encoded[offset + 2U]);
      const int fourth = encoded[offset + 3U] == '='
                             ? 0
                             : valueOf(encoded[offset + 3U]);
      const bool finalGroup =
          part + 1U == std::size(detail::kUefiNtfsImageBase64) &&
          offset + 4U == encoded.size();
      if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
          (encoded[offset + 2U] == '=' && encoded[offset + 3U] != '=') ||
          ((encoded[offset + 2U] == '=' || encoded[offset + 3U] == '=') &&
           !finalGroup)) {
        error = "The embedded UEFI:NTFS image is corrupt";
        decoded.clear();
        return false;
      }
      const std::uint32_t group =
          static_cast<std::uint32_t>(first) << 18U |
          static_cast<std::uint32_t>(second) << 12U |
          static_cast<std::uint32_t>(third) << 6U |
          static_cast<std::uint32_t>(fourth);
      decoded.push_back(static_cast<unsigned char>(group >> 16U));
      if (encoded[offset + 2U] != '=') {
        decoded.push_back(static_cast<unsigned char>(group >> 8U));
      }
      if (encoded[offset + 3U] != '=') {
        decoded.push_back(static_cast<unsigned char>(group));
      }
    }
  }
  if (decoded.size() != kUefiNtfsPartitionBytes || decoded[510] != 0x55U ||
      decoded[511] != 0xaaU) {
    error = "The embedded UEFI:NTFS image failed its size or boot-sector check";
    decoded.clear();
    return false;
  }
  return true;
}

bool createNtfsMbrImage(const std::filesystem::path& path,
                        const std::uint64_t capacityBytes,
                        const core::IsoTargetSystem targetSystem,
                        std::string& error) {
  if (capacityBytes % kSectorSize != 0U) {
    error = "The NTFS staging capacity is not aligned to 512-byte sectors";
    return false;
  }
  const std::uint64_t sectorCount = capacityBytes / kSectorSize;
  const std::uint64_t firstDataSector = kPartitionAlignmentSectors;
  const bool bios = targetSystem == core::IsoTargetSystem::Bios ||
                    targetSystem == core::IsoTargetSystem::BiosAndUefi;
  const bool uefi = targetSystem != core::IsoTargetSystem::Bios;
  const std::uint64_t bootSectors = uefi ? kUefiNtfsPartitionBytes / kSectorSize : 0U;
  const std::uint64_t tailSectors = kTargetTailReserveBytes / kSectorSize;
  if (sectorCount <= firstDataSector + bootSectors + tailSectors ||
      sectorCount > std::numeric_limits<std::uint32_t>::max()) {
    error = "The target is outside the supported MBR/UEFI:NTFS size range";
    return false;
  }
  const std::uint64_t bootFirst = sectorCount - tailSectors - bootSectors;
  const std::uint64_t ntfsSectors = bootFirst - firstDataSector;
  if (ntfsSectors < 64ULL * 1024ULL) {
    error = "The target is too small for NTFS ISO deployment";
    return false;
  }

  std::vector<unsigned char> uefiNtfs;
  if (uefi && !decodeUefiNtfsImage(uefiNtfs, error)) {
    return false;
  }
  std::array<unsigned char, kSectorSize> mbr{};
  if (bios) {
    std::copy(core::detail::kWindowsMbrBootstrap.begin(),
              core::detail::kWindowsMbrBootstrap.end(), mbr.begin());
  }
  std::random_device random;
  put32(mbr.data() + 440U, static_cast<std::uint32_t>(random()) | 1U);
  const auto chs = [](unsigned char* destination, const std::uint64_t lba) {
    const auto cylinder = lba / (255U * 63U);
    if (cylinder > 1023U) {
      destination[0] = 0xfeU; destination[1] = 0xffU; destination[2] = 0xffU;
    } else {
      destination[0] = static_cast<unsigned char>((lba / 63U) % 255U);
      destination[1] = static_cast<unsigned char>((lba % 63U + 1U) | ((cylinder >> 2U) & 0xc0U));
      destination[2] = static_cast<unsigned char>(cylinder);
    }
  };
  unsigned char* const ntfs = mbr.data() + 446U;
  ntfs[0] = 0x80U;
  chs(ntfs + 1U, firstDataSector);
  ntfs[4] = 0x07U;
  chs(ntfs + 5U, bootFirst - 1U);
  put32(ntfs + 8U, static_cast<std::uint32_t>(firstDataSector));
  put32(ntfs + 12U, static_cast<std::uint32_t>(ntfsSectors));
  if (uefi) {
    unsigned char* const boot = mbr.data() + 462U;
    chs(boot + 1U, bootFirst);
    boot[4] = 0xefU;
    chs(boot + 5U, bootFirst + bootSectors - 1U);
    put32(boot + 8U, static_cast<std::uint32_t>(bootFirst));
    put32(boot + 12U, static_cast<std::uint32_t>(bootSectors));
  }
  mbr[510] = 0x55U;
  mbr[511] = 0xaaU;

  std::ofstream create(path, std::ios::binary | std::ios::trunc);
  if (!create) {
    error = "Unable to create the NTFS ISO staging image";
    return false;
  }
  create.close();
  if (!resizeSparseImage(path, capacityBytes, error)) {
    return false;
  }
  std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
  output.write(reinterpret_cast<const char*>(mbr.data()),
               static_cast<std::streamsize>(mbr.size()));
  if (uefi) {
    output.seekp(static_cast<std::streamoff>(bootFirst * kSectorSize));
    output.write(reinterpret_cast<const char*>(uefiNtfs.data()),
                 static_cast<std::streamsize>(uefiNtfs.size()));
  }
  output.flush();
  if (!output) {
    error = "Unable to write the MBR/UEFI:NTFS staging layout";
    return false;
  }
  return true;
}

std::string normalizedNtfsLabel(const std::string_view input) {
  std::string result;
  result.reserve(std::min<std::size_t>(input.size(), 32U));
  constexpr std::string_view forbidden = "\\/:*?\"<>|";
  for (const unsigned char character : input) {
    if (result.size() == 32U) {
      break;
    }
    if (character < 0x20U || character == 0x7fU ||
        (character < 0x80U &&
         forbidden.find(static_cast<char>(character)) != std::string_view::npos)) {
      result.push_back('_');
    } else {
      result.push_back(static_cast<char>(character));
    }
  }
  return result.empty() ? std::string("RUFUS") : result;
}

bool validateNtfsStagingSpace(const core::IsoDeploymentPlan& plan,
                              const std::filesystem::path& outputPath,
                              std::string& error) {
  std::error_code fileError;
  const auto parent = outputPath.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : outputPath.parent_path();
  if (fileError) {
    error = "Unable to resolve the NTFS staging directory: " +
            fileError.message();
    return false;
  }
  const auto space = std::filesystem::space(parent, fileError);
  if (fileError) {
    error = "Unable to check free space for NTFS ISO staging: " +
            fileError.message();
    return false;
  }
  const auto requirements = estimateNtfsIsoStagingRequirements(
      plan.image().expandedSizeBytes, plan.target().capacityBytes,
      plan.quickFormat());
  if (!requirements.success) {
    error = requirements.error;
    return false;
  }
  if (space.available < requirements.temporaryBytes) {
    error = "Not enough temporary disk space to stage the extracted NTFS ISO";
    return false;
  }
  return true;
}

std::uint32_t crc32(const unsigned char* data, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

using GuidBytes = std::array<unsigned char, 16>;

GuidBytes randomGuid() {
  std::random_device random;
  GuidBytes value{};
  for (unsigned char& byte : value) {
    byte = static_cast<unsigned char>(random());
  }
  value[7] = static_cast<unsigned char>((value[7] & 0x0fU) | 0x40U);
  value[8] = static_cast<unsigned char>((value[8] & 0x3fU) | 0x80U);
  return value;
}

void putPartition(std::vector<unsigned char>& entries, const std::size_t index,
                  const GuidBytes& type, const GuidBytes& unique,
                  const std::uint64_t firstLba, const std::uint64_t lastLba,
                  const std::u16string_view name) {
  unsigned char* const entry = entries.data() + index * kGptEntrySize;
  std::copy(type.begin(), type.end(), entry);
  std::copy(unique.begin(), unique.end(), entry + 16);
  put64(entry + 32, firstLba);
  put64(entry + 40, lastLba);
  const std::size_t count = std::min<std::size_t>(name.size(), 36);
  for (std::size_t character = 0; character < count; ++character) {
    put16(entry + 56 + character * 2U, name[character]);
  }
}

std::array<unsigned char, kSectorSize> makeGptHeader(
    const std::uint64_t currentLba, const std::uint64_t backupLba,
    const std::uint64_t lastUsableLba, const std::uint64_t entriesLba,
    const GuidBytes& diskGuid, const std::uint32_t entriesCrc) {
  std::array<unsigned char, kSectorSize> header{};
  constexpr std::array<unsigned char, 8> signature{'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
  std::copy(signature.begin(), signature.end(), header.begin());
  put32(header.data() + 8, 0x00010000U);
  put32(header.data() + 12, 92);
  put64(header.data() + 24, currentLba);
  put64(header.data() + 32, backupLba);
  put64(header.data() + 40, kPartitionAlignmentSectors);
  put64(header.data() + 48, lastUsableLba);
  std::copy(diskGuid.begin(), diskGuid.end(), header.begin() + 56);
  put64(header.data() + 72, entriesLba);
  put32(header.data() + 80, kGptEntryCount);
  put32(header.data() + 84, kGptEntrySize);
  put32(header.data() + 88, entriesCrc);
  put32(header.data() + 16, crc32(header.data(), 92));
  return header;
}

bool createGptImage(const std::filesystem::path& path,
                    const std::uint64_t capacityBytes, std::string& error) {
  if (capacityBytes % kSectorSize != 0) {
    error = "Windows To Go target capacity is not sector aligned";
    return false;
  }
  const std::uint64_t sectorCount = capacityBytes / kSectorSize;
  const std::uint64_t backupHeaderLba = sectorCount - 1U;
  const std::uint64_t backupEntriesLba = backupHeaderLba - kGptEntrySectors;
  const std::uint64_t lastUsableLba = backupEntriesLba - 1U;
  const std::uint64_t espFirst = kPartitionAlignmentSectors;
  const std::uint64_t espLast = espFirst + kEspSectors - 1U;
  const std::uint64_t msrFirst = espLast + 1U;
  const std::uint64_t msrLast = msrFirst + kMsrSectors - 1U;
  const std::uint64_t windowsFirst =
      ((msrLast + 1U + kPartitionAlignmentSectors - 1U) /
       kPartitionAlignmentSectors) * kPartitionAlignmentSectors;
  if (sectorCount < 64ULL * 1024ULL * 1024ULL || windowsFirst >= lastUsableLba) {
    error = "The target is too small for the Windows To Go GPT layout";
    return false;
  }

  std::ofstream create(path, std::ios::binary | std::ios::trunc);
  if (!create) {
    error = "Unable to create the Windows To Go staging image";
    return false;
  }
  create.close();
  if (!resizeSparseImage(path, capacityBytes, error)) {
    return false;
  }

  std::vector<unsigned char> entries(kGptEntryCount * kGptEntrySize, 0);
  constexpr GuidBytes efiType{0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                              0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  constexpr GuidBytes msrType{0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d,
                              0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae};
  constexpr GuidBytes basicDataType{0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
  putPartition(entries, 0, efiType, randomGuid(), espFirst, espLast, u"EFI System");
  putPartition(entries, 1, msrType, randomGuid(), msrFirst, msrLast,
               u"Microsoft reserved");
  putPartition(entries, 2, basicDataType, randomGuid(), windowsFirst,
               lastUsableLba, u"Windows To Go");
  const std::uint32_t entriesCrc = crc32(entries.data(), entries.size());
  const GuidBytes diskGuid = randomGuid();
  const auto primaryHeader = makeGptHeader(1, backupHeaderLba, lastUsableLba, 2,
                                           diskGuid, entriesCrc);
  const auto backupHeader = makeGptHeader(backupHeaderLba, 1, lastUsableLba,
                                          backupEntriesLba, diskGuid, entriesCrc);

  std::array<unsigned char, kSectorSize> protectiveMbr{};
  protectiveMbr[446 + 4] = 0xee;
  put32(protectiveMbr.data() + 446 + 8, 1);
  put32(protectiveMbr.data() + 446 + 12,
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            sectorCount - 1U, std::numeric_limits<std::uint32_t>::max())));
  protectiveMbr[510] = 0x55;
  protectiveMbr[511] = 0xaa;

  std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!output) {
    error = "Unable to open the Windows To Go staging image layout";
    return false;
  }
  const auto writeAt = [&output](const std::uint64_t offset, const void* data,
                                  const std::size_t size) {
    output.seekp(static_cast<std::streamoff>(offset));
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
  };
  if (!writeAt(0, protectiveMbr.data(), protectiveMbr.size()) ||
      !writeAt(kSectorSize, primaryHeader.data(), primaryHeader.size()) ||
      !writeAt(2U * kSectorSize, entries.data(), entries.size()) ||
      !writeAt(backupEntriesLba * kSectorSize, entries.data(), entries.size()) ||
      !writeAt(backupHeaderLba * kSectorSize, backupHeader.data(), backupHeader.size())) {
    error = "Unable to write the Windows To Go GPT layout";
    return false;
  }
  output.flush();
  if (!output) {
    error = "Unable to flush the Windows To Go GPT layout";
    return false;
  }
  return true;
}

bool createNtfsGptImage(const std::filesystem::path& path,
                        const std::uint64_t capacityBytes,
                        std::string& error) {
  if (capacityBytes % kSectorSize != 0U) {
    error = "The GPT/NTFS staging capacity is not sector aligned";
    return false;
  }
  const std::uint64_t sectorCount = capacityBytes / kSectorSize;
  const std::uint64_t backupHeaderLba = sectorCount - 1U;
  const std::uint64_t backupEntriesLba = backupHeaderLba - kGptEntrySectors;
  const std::uint64_t lastUsableLba = backupEntriesLba - 1U;
  const std::uint64_t ntfsFirst = kPartitionAlignmentSectors;
  const std::uint64_t bootSectors = kUefiNtfsPartitionBytes / kSectorSize;
  if (lastUsableLba <= ntfsFirst + bootSectors + 64ULL * 1024ULL) {
    error = "The target is too small for GPT/NTFS ISO deployment";
    return false;
  }
  const std::uint64_t bootFirst = lastUsableLba - bootSectors + 1U;
  const std::uint64_t ntfsLast = bootFirst - 1U;

  std::vector<unsigned char> uefiNtfs;
  if (!decodeUefiNtfsImage(uefiNtfs, error)) {
    return false;
  }
  std::ofstream create(path, std::ios::binary | std::ios::trunc);
  if (!create) {
    error = "Unable to create the GPT/NTFS ISO staging image";
    return false;
  }
  create.close();
  if (!resizeSparseImage(path, capacityBytes, error)) {
    return false;
  }

  std::vector<unsigned char> entries(kGptEntryCount * kGptEntrySize, 0U);
  constexpr GuidBytes basicDataType{
      0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
      0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
  constexpr GuidBytes efiType{
      0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  putPartition(entries, 0U, basicDataType, randomGuid(), ntfsFirst, ntfsLast,
               u"Rufus++ ISO");
  putPartition(entries, 1U, efiType, randomGuid(), bootFirst, lastUsableLba,
               u"UEFI:NTFS");
  const std::uint32_t entriesCrc = crc32(entries.data(), entries.size());
  const GuidBytes diskGuid = randomGuid();
  const auto primaryHeader = makeGptHeader(
      1U, backupHeaderLba, lastUsableLba, 2U, diskGuid, entriesCrc);
  const auto backupHeader = makeGptHeader(
      backupHeaderLba, 1U, lastUsableLba, backupEntriesLba, diskGuid,
      entriesCrc);
  std::array<unsigned char, kSectorSize> protectiveMbr{};
  protectiveMbr[450U] = 0xeeU;
  put32(protectiveMbr.data() + 454U, 1U);
  put32(protectiveMbr.data() + 458U,
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            sectorCount - 1U, std::numeric_limits<std::uint32_t>::max())));
  protectiveMbr[510U] = 0x55U;
  protectiveMbr[511U] = 0xaaU;

  std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
  const auto writeAt = [&output](const std::uint64_t offset, const void* data,
                                  const std::size_t size) {
    output.seekp(static_cast<std::streamoff>(offset));
    output.write(static_cast<const char*>(data),
                 static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
  };
  if (!output || !writeAt(0U, protectiveMbr.data(), protectiveMbr.size()) ||
      !writeAt(kSectorSize, primaryHeader.data(), primaryHeader.size()) ||
      !writeAt(2U * kSectorSize, entries.data(), entries.size()) ||
      !writeAt(bootFirst * kSectorSize, uefiNtfs.data(), uefiNtfs.size()) ||
      !writeAt(backupEntriesLba * kSectorSize, entries.data(), entries.size()) ||
      !writeAt(backupHeaderLba * kSectorSize, backupHeader.data(),
               backupHeader.size())) {
    error = "Unable to write the GPT/NTFS ISO layout";
    return false;
  }
  output.flush();
  if (!output) {
    error = "Unable to flush the GPT/NTFS ISO layout";
    return false;
  }
  return true;
}

bool createNtfsIsoLayoutImage(const std::filesystem::path& path,
                              const std::uint64_t capacityBytes,
                              const core::PartitionScheme partitionScheme,
                              const core::IsoTargetSystem targetSystem,
                              std::string& error) {
  if (targetSystem == core::IsoTargetSystem::Automatic ||
      (partitionScheme == core::PartitionScheme::Gpt && targetSystem != core::IsoTargetSystem::Uefi)) {
    error = "NTFS BIOS boot requires MBR; a concrete target system is required";
    return false;
  }
  return partitionScheme == core::PartitionScheme::Gpt
             ? createNtfsGptImage(path, capacityBytes, error)
             : createNtfsMbrImage(path, capacityBytes, targetSystem, error);
}

bool validateStagingSpace(const core::WindowsToGoPlan& plan,
                          const std::filesystem::path& outputPath,
                          std::string& error) {
  std::error_code fileError;
  const auto parent = outputPath.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : outputPath.parent_path();
  if (fileError) {
    error = "Unable to resolve the Windows To Go staging directory: " +
            fileError.message();
    return false;
  }
  const auto space = std::filesystem::space(parent, fileError);
  if (fileError) {
    error = "Unable to check free space for Windows To Go staging: " +
            fileError.message();
    return false;
  }
  std::uint64_t required = plan.image().sizeBytes;
  const std::uint64_t expanded = plan.edition().totalBytes == 0
                                     ? 20ULL * 1024ULL * 1024ULL * 1024ULL
                                     : plan.edition().totalBytes;
  if (required > std::numeric_limits<std::uint64_t>::max() - expanded ||
      required + expanded >
          std::numeric_limits<std::uint64_t>::max() - kStagingReserveBytes) {
    error = "The Windows To Go staging space requirement overflowed";
    return false;
  }
  required += expanded + kStagingReserveBytes;
  if (space.available < required) {
    error = "Not enough temporary disk space to extract and apply the selected Windows edition";
    return false;
  }
  return true;
}

class ScopedWork final {
 public:
  ~ScopedWork() {
    if (keep_) {
      return;
    }
    std::error_code ignored;
    if (!output_.empty()) {
      std::filesystem::remove(output_, ignored);
    }
    ignored.clear();
    if (!directory_.empty()) {
      std::filesystem::remove_all(directory_, ignored);
    }
  }

  void set(std::filesystem::path directory, std::filesystem::path output) {
    directory_ = std::move(directory);
    output_ = std::move(output);
  }
  void keepOutput() noexcept { keep_ = true; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path output_;
  bool keep_{};
};

#if !defined(_WIN32)

struct CommandResult final {
  int exitCode{-1};
  bool cancelled{};
  std::string output;
};

std::filesystem::path findExecutable(const std::string_view name) {
  if (name.find('/') != std::string_view::npos) {
    const std::filesystem::path path(name);
    return access(path.c_str(), X_OK) == 0 ? path : std::filesystem::path{};
  }
#if defined(__APPLE__)
  constexpr std::array<std::string_view, 8> searchDirectories{
      "/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin",
      "/usr/local/sbin", "/usr/bin", "/usr/sbin", "/bin", "/sbin"};
#else
  constexpr std::array<std::string_view, 6> searchDirectories{
      "/usr/local/bin", "/usr/local/sbin", "/usr/bin", "/usr/sbin",
      "/bin", "/sbin"};
#endif
  for (const std::string_view directory : searchDirectories) {
    const std::filesystem::path candidate = std::filesystem::path(directory) / name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return {};
}

CommandResult runCommand(const std::vector<std::string>& arguments,
                         const WindowsToGoCancelCallback& isCancelled = {}) {
  CommandResult result;
  if (arguments.empty()) {
    result.output = "No command was provided";
    return result;
  }
  const std::filesystem::path executable = findExecutable(arguments.front());
  if (executable.empty()) {
    result.output = arguments.front() + " is not installed";
    return result;
  }
  int pipeDescriptors[2]{};
  if (pipe(pipeDescriptors) != 0) {
    result.output = "Unable to create a command output pipe";
    return result;
  }
  const pid_t child = fork();
  if (child == 0) {
    static_cast<void>(setpgid(0, 0));
    close(pipeDescriptors[0]);
    static_cast<void>(dup2(pipeDescriptors[1], STDOUT_FILENO));
    static_cast<void>(dup2(pipeDescriptors[1], STDERR_FILENO));
    close(pipeDescriptors[1]);
    std::vector<char*> nativeArguments;
    nativeArguments.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) {
      nativeArguments.push_back(const_cast<char*>(argument.c_str()));
    }
    nativeArguments.push_back(nullptr);
    execv(executable.c_str(), nativeArguments.data());
    _exit(127);
  }
  close(pipeDescriptors[1]);
  if (child < 0) {
    close(pipeDescriptors[0]);
    result.output = "Unable to start " + arguments.front();
    return result;
  }
  // Establish the process group from both sides of the fork. The child does
  // the same before exec, while this closes the race where cancellation lands
  // just before it gets scheduled.
  static_cast<void>(setpgid(child, child));
  const int pipeFlags = fcntl(pipeDescriptors[0], F_GETFL);
  if (pipeFlags < 0 ||
      fcntl(pipeDescriptors[0], F_SETFL, pipeFlags | O_NONBLOCK) != 0) {
    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    close(pipeDescriptors[0]);
    result.output = "Unable to configure the command output pipe";
    return result;
  }
  const auto terminateChild = [child](int& status) {
    static_cast<void>(kill(-child, SIGTERM));
    static_cast<void>(kill(child, SIGTERM));
    for (unsigned int attempt = 0; attempt < 40; ++attempt) {
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child || (waited < 0 && errno == ECHILD)) {
        return;
      }
      if (waited < 0 && errno != EINTR) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    static_cast<void>(kill(-child, SIGKILL));
    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  };
  int status = 0;
  bool running = true;
  bool waitFailed = false;
  std::array<char, 4096> buffer{};
  while (running) {
    for (;;) {
      const ssize_t amount = read(pipeDescriptors[0], buffer.data(), buffer.size());
      if (amount <= 0) {
        break;
      }
      result.output.append(buffer.data(), static_cast<std::size_t>(amount));
    }
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      running = false;
      continue;
    }
    if (waited < 0 && errno != EINTR) {
      result.output += "\nUnable to wait for " + arguments.front();
      waitFailed = true;
      running = false;
      continue;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      terminateChild(status);
      running = false;
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  for (;;) {
    const ssize_t amount = read(pipeDescriptors[0], buffer.data(), buffer.size());
    if (amount <= 0) {
      break;
    }
    result.output.append(buffer.data(), static_cast<std::size_t>(amount));
  }
  close(pipeDescriptors[0]);
  if (!result.cancelled && !waitFailed && WIFEXITED(status)) {
    result.exitCode = WEXITSTATUS(status);
  }
  return result;
}

std::string trim(std::string value) {
  const auto whitespace = [](const unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
  };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), whitespace));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(),
              value.end());
  return value;
}

std::string commandFailure(const std::string_view action,
                           const CommandResult& command) {
  const std::string detail = trim(command.output);
  return std::string(action) + " failed" +
         (detail.empty() ? std::string{} : ": " + detail);
}

#if defined(__APPLE__)
std::filesystem::path mountedPath(const std::string& device) {
  const CommandResult info = runCommand({"diskutil", "info", device});
  if (info.exitCode != 0) {
    return {};
  }
  constexpr std::string_view field = "Mount Point:";
  const std::size_t position = info.output.find(field);
  if (position == std::string::npos) {
    return {};
  }
  const std::size_t end = info.output.find('\n', position);
  return trim(info.output.substr(position + field.size(), end - position - field.size()));
}
#endif

class PosixWindowsToGoImageStager final : public WindowsToGoImageStager {
 public:
  PosixWindowsToGoImageStager() : wim_(core::createSystemWimApplicator()) {}

  [[nodiscard]] WindowsToGoAvailability availability() const override {
#if !defined(__APPLE__)
    if (geteuid() != 0) {
      return {false,
              "Windows To Go staging requires the Linux application to run as administrator"
      };
    }
#endif
    if (wim_ == nullptr || !wim_->available()) {
      return {false, wim_ == nullptr ? "No wimlib application backend is available"
                                     : wim_->availabilityReason()};
    }
    std::vector<std::string> required{
#if defined(__APPLE__)
        "hdiutil", "diskutil", "newfs_msdos", "mkntfs", "ntfs-3g", "umount",
        "bcd-sys", "hivexsh", "hivexregedit", "peres", "xxd"
#else
        "losetup", "mkfs.fat", "mkntfs", "ntfs-3g", "mount", "umount",
        "bcd-sys", "hivexsh", "hivexregedit", "peres", "xxd", "setfattr",
        "fatattr"
#endif
    };
    std::string missing;
    for (const auto& tool : required) {
      if (findExecutable(tool).empty()) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += tool;
      }
    }
    if (!missing.empty()) {
      return {false, "Windows To Go host tools are missing: " + missing};
    }
    return {true, {}};
  }

  [[nodiscard]] WindowsToGoStageResult stage(
      const core::WindowsToGoPlan& plan,
      const std::filesystem::path& outputPath,
      const WindowsToGoProgressCallback& onProgress,
      const WindowsToGoCancelCallback& isCancelled) const override {
    WindowsToGoStageResult result;
    const WindowsToGoAvailability ready = availability();
    if (!ready.available) {
      result.error = ready.reason;
      return result;
    }
    if (plan.target().logicalSectorSize != kSectorSize) {
      result.error = "Windows To Go GPT staging currently supports 512-byte-sector targets";
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Windows To Go staging was cancelled before it began";
      return result;
    }

    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) || fileError) {
      result.error = fileError ? "Unable to validate the staging output: " +
                                     fileError.message()
                               : "The Windows To Go staging output already exists";
      return result;
    }
    if (!validateStagingSpace(plan, outputPath, result.error)) {
      return result;
    }
    auto workPath = outputPath;
    workPath += ".wtg-work";
    if (!std::filesystem::create_directory(workPath, fileError)) {
      result.error = fileError ? "Unable to create the Windows To Go work directory: " +
                                     fileError.message()
                               : "The Windows To Go work directory already exists";
      return result;
    }
    std::filesystem::permissions(workPath, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, fileError);
    ScopedWork scoped;
    scoped.set(workPath, outputPath);
    if (fileError) {
      result.error = "Unable to secure the Windows To Go work directory: " +
                     fileError.message();
      return result;
    }

    const auto installImage = workPath / "install-image.wim";
    const auto unattended = workPath / "unattend.xml";
    const auto espMount = workPath / "esp";
    const auto windowsMount = workPath / "windows";
    std::filesystem::path espSystemPath = espMount;
    std::filesystem::create_directories(espMount, fileError);
    std::filesystem::create_directories(windowsMount, fileError);
    std::ofstream answerFile(unattended, std::ios::binary | std::ios::trunc);
    answerFile.write(plan.unattendXml().data(),
                     static_cast<std::streamsize>(plan.unattendXml().size()));
    answerFile.flush();
    if (fileError || !answerFile) {
      result.error = "Unable to prepare private Windows To Go configuration files";
      return result;
    }
    answerFile.close();

    if (onProgress) {
      onProgress({WindowsToGoStage::ExtractingInstallImage, 0, 0,
                  "Extracting the selected Windows install image"});
    }
    const auto extracted = core::extractWindowsToGoSource(
        plan, installImage,
        [&](const core::WindowsToGoSourceProgress& progress) {
          if (onProgress) {
            onProgress({WindowsToGoStage::ExtractingInstallImage,
                        progress.bytesProcessed, progress.totalBytes,
                        "Extracting " + std::string(progress.totalBytes == 0
                                                        ? "install image"
                                                        : "sources/install.wim or install.esd")});
          }
        },
        isCancelled);
    if (!extracted.success) {
      result.cancelled = extracted.cancelled;
      result.error = extracted.error;
      return result;
    }
    if (onProgress) {
      onProgress({WindowsToGoStage::CreatingDiskLayout, 0, 0,
                  "Creating GPT, EFI, Microsoft reserved, and Windows partitions"});
    }
    if (!createGptImage(outputPath, plan.target().capacityBytes, result.error)) {
      return result;
    }

    std::string disk;
    std::string espDevice;
    std::string windowsDevice;
    bool espMounted = false;
    bool windowsMounted = false;
    const auto cleanupDevices = [&] {
      bool succeeded = true;
      if (windowsMounted) {
        const auto unmount = runCommand({"umount", windowsMount.string()});
        succeeded = unmount.exitCode == 0 && succeeded;
        windowsMounted = unmount.exitCode != 0;
      }
      if (espMounted) {
#if defined(__APPLE__)
        const auto unmount = runCommand({"diskutil", "unmount", espDevice});
#else
        const auto unmount = runCommand({"umount", espMount.string()});
#endif
        succeeded = unmount.exitCode == 0 && succeeded;
        espMounted = unmount.exitCode != 0;
      }
      if (!disk.empty() && !windowsMounted && !espMounted) {
#if defined(__APPLE__)
        const auto detach = runCommand({"hdiutil", "detach", disk});
#else
        const auto detach = runCommand({"losetup", "-d", disk});
#endif
        succeeded = detach.exitCode == 0 && succeeded;
        if (detach.exitCode == 0) {
          disk.clear();
        }
      }
      return succeeded && disk.empty() && !windowsMounted && !espMounted;
    };

#if defined(__APPLE__)
    CommandResult attach = runCommand(
        {"hdiutil", "attach", "-nomount", "-noverify", outputPath.string()},
        isCancelled);
    if (attach.exitCode != 0) {
      result.cancelled = attach.cancelled;
      result.error = commandFailure("Attaching the staging image", attach);
      return result;
    }
    const std::size_t deviceStart = attach.output.find("/dev/disk");
    const std::size_t deviceEnd = attach.output.find_first_of(" \t\r\n", deviceStart);
    if (deviceStart == std::string::npos) {
      result.error = "hdiutil did not return a staging disk device";
      return result;
    }
    disk = attach.output.substr(deviceStart, deviceEnd - deviceStart);
    espDevice = disk + "s1";
    windowsDevice = disk + "s3";
    const std::string rawEsp = "/dev/r" + espDevice.substr(5);
    const std::string rawWindows = "/dev/r" + windowsDevice.substr(5);
#else
    CommandResult attach = runCommand(
        {"losetup", "--find", "--show", "--partscan", outputPath.string()},
        isCancelled);
    if (attach.exitCode != 0) {
      result.cancelled = attach.cancelled;
      result.error = commandFailure("Attaching the staging image", attach);
      return result;
    }
    disk = trim(attach.output);
    if (disk.empty() || disk.rfind("/dev/loop", 0) != 0) {
      result.error = "losetup did not return a staging loop device";
      cleanupDevices();
      return result;
    }
    espDevice = disk + "p1";
    windowsDevice = disk + "p3";
    for (unsigned int attempt = 0; attempt < 50 &&
                                   (!std::filesystem::exists(espDevice) ||
                                    !std::filesystem::exists(windowsDevice));
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const std::string& rawEsp = espDevice;
    const std::string& rawWindows = windowsDevice;
#endif

    if (onProgress) {
      onProgress({WindowsToGoStage::FormattingFilesystems, 0, 0,
                  "Formatting the EFI and Windows partitions"});
    }
#if defined(__APPLE__)
    CommandResult formatEsp = runCommand(
        {"newfs_msdos", "-F", "32", "-v", "RUFUSPP_ESP", rawEsp}, isCancelled);
#else
    CommandResult formatEsp = runCommand(
        {"mkfs.fat", "-F", "32", "-n", "RUFUSPP_ESP", rawEsp}, isCancelled);
#endif
    if (formatEsp.exitCode != 0) {
      result.cancelled = formatEsp.cancelled;
      result.error = commandFailure("Formatting the EFI partition", formatEsp);
      cleanupDevices();
      return result;
    }
    CommandResult formatWindows = runCommand(
        {"mkntfs", "-F", "-Q", "-L",
         normalizedNtfsLabel(plan.volumeLabel()), rawWindows},
        isCancelled);
    if (formatWindows.exitCode != 0) {
      result.cancelled = formatWindows.cancelled;
      result.error = commandFailure("Formatting the Windows partition", formatWindows);
      cleanupDevices();
      return result;
    }

    if (onProgress) {
      onProgress({WindowsToGoStage::ApplyingWindows, 0, 0,
                  "Applying " + plan.edition().name + " to NTFS"});
    }
    const core::WimApplyResult applied = wim_->apply(
        installImage, plan.edition().index, rawWindows, true, unattended,
        [&](const core::WimApplyProgress& progress) {
          if (onProgress) {
            onProgress({WindowsToGoStage::ApplyingWindows,
                        progress.bytesProcessed, progress.totalBytes,
                        "Applying " + plan.edition().name});
          }
        },
        isCancelled);
    if (!applied.success) {
      result.cancelled = applied.cancelled;
      result.error = applied.error;
      cleanupDevices();
      return result;
    }

    if (onProgress) {
      onProgress({WindowsToGoStage::ConfiguringBoot, 0, 0,
                  "Constructing Windows Boot Manager and BCD data"});
    }
#if defined(__APPLE__)
    CommandResult mountEsp = runCommand({"diskutil", "mount", espDevice}, isCancelled);
    if (mountEsp.exitCode == 0) {
      const auto discovered = mountedPath(espDevice);
      if (!discovered.empty()) {
        espSystemPath = discovered;
      } else {
        fileError = std::make_error_code(std::errc::no_such_file_or_directory);
      }
    }
    espMounted = mountEsp.exitCode == 0;
#else
    CommandResult mountEsp = runCommand({"mount", espDevice, espMount.string()}, isCancelled);
    espMounted = mountEsp.exitCode == 0;
#endif
    if (!espMounted || fileError) {
      result.cancelled = mountEsp.cancelled;
      result.error = fileError ? "Unable to access the mounted EFI partition: " +
                                     fileError.message()
                               : commandFailure("Mounting the EFI partition", mountEsp);
      cleanupDevices();
      return result;
    }
    CommandResult mountWindows = runCommand(
        {"ntfs-3g", windowsDevice, windowsMount.string(), "-o", "windows_names"},
        isCancelled);
    windowsMounted = mountWindows.exitCode == 0;
    if (!windowsMounted) {
      result.cancelled = mountWindows.cancelled;
      result.error = commandFailure("Mounting the Windows partition", mountWindows);
      cleanupDevices();
      return result;
    }
    CommandResult boot = runCommand(
        {"bcd-sys", windowsMount.string(), "-s", espSystemPath.string(),
         "-f", "uefi", "-c"},
        isCancelled);
    if (boot.exitCode != 0) {
      result.cancelled = boot.cancelled;
      result.error = commandFailure("Configuring Windows Boot Manager", boot);
      cleanupDevices();
      return result;
    }
    if (!cleanupDevices()) {
      result.error =
          "Unable to unmount or detach the Windows To Go staging image cleanly";
      return result;
    }

    if (onProgress) {
      onProgress({WindowsToGoStage::Finalizing, plan.target().capacityBytes,
                  plan.target().capacityBytes,
                  "Finalizing the Windows To Go disk image"});
    }
    core::ImageInfo image;
    image.path = outputPath.string();
    image.displayName = outputPath.filename().string();
    image.volumeLabel = plan.volumeLabel();
    image.sizeBytes = plan.target().capacityBytes;
    image.expandedSizeBytes = image.sizeBytes;
    image.format = core::ImageFormat::Raw;
    image.partitionScheme = core::PartitionScheme::Gpt;
    image.family = core::ImageFamily::WindowsInstaller;
    image.architecture = plan.edition().architecture;
    image.bootable = true;
    image.capabilities.rawWrite = true;
    image.capabilities.validPartitionTable = true;
    image.capabilities.uefiBootable = true;
    result.stagedImage = std::move(image);
    result.success = true;
    scoped.keepOutput();
    std::filesystem::remove_all(workPath, fileError);
    if (onProgress) {
      onProgress({WindowsToGoStage::Complete, plan.target().capacityBytes,
                  plan.target().capacityBytes, "Windows To Go staging complete"});
    }
    return result;
  }

 private:
  std::shared_ptr<const core::WimApplicator> wim_;
};

class PosixNtfsIsoImageStager final : public NtfsIsoImageStager {
 public:
  [[nodiscard]] NtfsIsoAvailability availability() const override {
    const auto formatter = detail::bundledNtfsFormatter();
    return formatter.empty()
               ? NtfsIsoAvailability{false, "The bundled NTFS formatter is missing from this application"}
               : NtfsIsoAvailability{true, {}};
  }

  [[nodiscard]] core::IsoDeploymentResult stage(
      const core::IsoDeploymentPlan& plan,
      const std::filesystem::path& outputPath,
      const core::IsoDeploymentProgressCallback& onProgress,
      const core::IsoDeploymentCancelCallback& isCancelled) const override {
    core::IsoDeploymentResult result;
    const auto ready = availability();
    if (!ready.available) {
      result.error = ready.reason;
      return result;
    }
    if (plan.fileSystem() != core::IsoDeploymentFilesystem::Ntfs ||
        plan.target().logicalSectorSize != kSectorSize) {
      result.error = "The plan is not a supported 512-byte-sector NTFS ISO deployment";
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "NTFS ISO staging was cancelled before it began";
      return result;
    }
    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) || fileError) {
      result.error = fileError
                         ? "Unable to validate the NTFS staging output: " +
                               fileError.message()
                         : "The NTFS staging output already exists";
      return result;
    }
    if (!validateNtfsStagingSpace(plan, outputPath, result.error)) {
      return result;
    }
    auto workPath = outputPath;
    workPath += ".ntfs-work";
    if (!std::filesystem::create_directory(workPath, fileError)) {
      result.error = fileError
                         ? "Unable to create the NTFS staging work directory: " +
                               fileError.message()
                         : "The NTFS staging work directory already exists";
      return result;
    }
    std::filesystem::permissions(workPath, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 fileError);
    ScopedWork scoped;
    scoped.set(workPath, outputPath);
    if (fileError) {
      result.error = "Unable to secure the NTFS staging work directory: " +
                     fileError.message();
      return result;
    }
    if (onProgress) {
      onProgress({core::IsoDeploymentStage::Formatting, 0, 0,
                  plan.targetSystem() == core::IsoTargetSystem::Bios
                      ? "Creating NTFS partition for Windows BIOS boot"
                      : "Creating NTFS and UEFI:NTFS partitions"});
    }
    if (!createNtfsIsoLayoutImage(outputPath, plan.target().capacityBytes,
                                  plan.partitionScheme(), plan.targetSystem(), result.error)) {
      return result;
    }
    // Derive the extent from the authored table rather than duplicating its
    // MBR/GPT arithmetic in the formatter and copy path.
    std::ifstream layout(outputPath, std::ios::binary);
    const bool gpt = plan.partitionScheme() == core::PartitionScheme::Gpt;
    std::array<unsigned char, 16> extent{};
    layout.seekg(gpt ? 2U * kSectorSize + 32U : 446U + 8U);
    layout.read(reinterpret_cast<char*>(extent.data()), gpt ? 16 : 8);
    const auto le = [&](const std::size_t offset, const std::size_t bytes) {
      std::uint64_t value = 0;
      for (std::size_t index = 0; index < bytes; ++index) {
        value |= static_cast<std::uint64_t>(extent[offset + index]) << (8U * index);
      }
      return value;
    };
    const auto first = le(0, gpt ? 8 : 4);
    const auto sectors = gpt ? le(8, 8) - first + 1U : le(4, 4);
    if (!layout || first != kPartitionAlignmentSectors || sectors == 0 ||
        sectors > plan.target().capacityBytes / kSectorSize - first) {
      result.error = "Invalid NTFS partition extent in the staging layout";
      return result;
    }
    layout.close();
    const auto partitionOffset = first * kSectorSize;
    const auto partitionBytes = sectors * kSectorSize;
    const auto partitionPath = workPath / "filesystem.ntfs";
    const int partitionFile = open(partitionPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (partitionFile < 0) {
      result.error = "Unable to create the private NTFS partition file";
      return result;
    }
    const int sized = ftruncate(partitionFile, static_cast<off_t>(partitionBytes));
    const int closed = close(partitionFile);
    if (sized != 0 || closed != 0) {
      result.error = "Unable to size the private NTFS partition file";
      return result;
    }
    const auto formatCommand = detail::ntfsIsoFormatCommand(
        partitionPath.string(), plan.quickFormat(), plan.clusterSizeBytes(),
        normalizedNtfsLabel(plan.volumeLabel()), detail::bundledNtfsFormatter().string());
    const auto formatted = runCommand(formatCommand, isCancelled);
    if (formatted.exitCode != 0) {
      result.cancelled = formatted.cancelled;
      result.error = commandFailure("Formatting the NTFS staging file", formatted);
      return result;
    }
    auto tree = detail::NtfsFileTree::open(partitionPath, result.error);
    if (!tree) return result;
    // The extractor streams from the ISO into libntfs-3g and reopens the
    // filesystem read-only before byte-comparing every file with its source.
    const auto extractingProgress = [&](const core::IsoDeploymentProgress& progress) {
      if (onProgress && progress.stage != core::IsoDeploymentStage::Complete) onProgress(progress);
    };
    result = core::IsoImageStager{}.extractToFileTree(plan, *tree, extractingProgress, isCancelled);
    if (!result.success) return result;
    result.success = false;
    std::vector<core::RawWriteRange> partitionRanges;
    if (!tree->allocatedRanges(partitionRanges, partitionBytes, result.error) ||
        !tree->close(result.error)) return result;
    tree.reset();
    if (plan.legacyBiosBootstrap() == core::LegacyBiosBootstrap::WindowsBootManager &&
        !detail::installWindowsNtfsBootstrap(partitionPath, 0, partitionBytes, result.error)) return result;
    if (!plan.quickFormat()) partitionRanges = {{0, partitionBytes}};

    std::ifstream input(partitionPath, std::ios::binary);
    std::fstream assembled(outputPath, std::ios::binary | std::ios::in | std::ios::out);
    std::vector<unsigned char> expected(1024U * 1024U);
    std::vector<unsigned char> observed(expected.size());
    std::uint64_t total = 0;
    for (const auto& range : partitionRanges) {
      if (range.offsetBytes > partitionBytes || range.sizeBytes > partitionBytes - range.offsetBytes) {
        result.error = "NTFS allocation range exceeds the staging partition";
        return result;
      }
      total += range.sizeBytes;
    }
    std::uint64_t copied = 0;
    for (const auto& range : partitionRanges) {
      std::uint64_t offset = 0;
      while (offset < range.sizeBytes) {
        if (isCancelled && isCancelled()) {
          result.cancelled = true;
          result.error = "NTFS staging cancelled while assembling the image";
          return result;
        }
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            expected.size(), range.sizeBytes - offset));
        const auto sourceOffset = range.offsetBytes + offset;
        const auto destinationOffset = partitionOffset + sourceOffset;
        input.seekg(static_cast<std::streamoff>(sourceOffset));
        input.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(amount));
        assembled.seekp(static_cast<std::streamoff>(destinationOffset));
        assembled.write(reinterpret_cast<const char*>(expected.data()), static_cast<std::streamsize>(amount));
        assembled.flush();
        assembled.seekg(static_cast<std::streamoff>(destinationOffset));
        assembled.read(reinterpret_cast<char*>(observed.data()), static_cast<std::streamsize>(amount));
        if (!input || !assembled || !std::equal(expected.begin(), expected.begin() + amount, observed.begin())) {
          result.error = "Unable to copy and verify the NTFS partition in the assembled image";
          return result;
        }
        copied += amount;
        offset += amount;
        if (onProgress) onProgress({core::IsoDeploymentStage::Verifying, copied, total,
                                   "Assembling and verifying NTFS image"});
      }
    }
    assembled.close();
    if (!assembled) {
      result.error = "Unable to close the assembled NTFS image";
      return result;
    }
    if (plan.quickFormat()) {
      result.writeRanges.push_back({0, partitionOffset});
      for (const auto& range : partitionRanges) {
        result.writeRanges.push_back({partitionOffset + range.offsetBytes, range.sizeBytes});
      }
      const auto tail = partitionOffset + partitionBytes;
      result.writeRanges.push_back({tail, plan.target().capacityBytes - tail});
      std::vector<core::RawWriteRange> merged;
      for (const auto& range : result.writeRanges) {
        if (!merged.empty() && merged.back().offsetBytes + merged.back().sizeBytes >= range.offsetBytes) {
          auto& last = merged.back();
          last.sizeBytes = std::max(last.offsetBytes + last.sizeBytes,
                                   range.offsetBytes + range.sizeBytes) - last.offsetBytes;
        } else {
          merged.push_back(range);
        }
      }
      result.writeRanges = std::move(merged);
    }
    result.success = true;

    core::ImageInfo staged;
    staged.path = outputPath.string();
    staged.displayName = outputPath.filename().string();
    staged.volumeLabel = plan.volumeLabel();
    staged.sizeBytes = plan.target().capacityBytes;
    staged.expandedSizeBytes = staged.sizeBytes;
    staged.format = core::ImageFormat::Raw;
    staged.partitionScheme = plan.partitionScheme();
    staged.family = plan.image().family;
    staged.architecture = plan.image().architecture;
    staged.bootable = true;
    staged.capabilities.rawWrite = true;
    staged.capabilities.uefiBootable = plan.targetSystem() != core::IsoTargetSystem::Bios;
    staged.capabilities.biosBootable = plan.legacyBiosBootstrap() == core::LegacyBiosBootstrap::WindowsBootManager;
    staged.capabilities.validPartitionTable = true;
    result.stagedImage = std::move(staged);
    scoped.keepOutput();
    std::filesystem::remove_all(workPath, fileError);
    if (onProgress) {
      onProgress({core::IsoDeploymentStage::Complete,
                  result.bytesExtracted, result.bytesExtracted,
                  "NTFS ISO staging complete"});
    }
    return result;
  }
};

#else

std::wstring utf8ToWide(const std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0);
  if (required <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(),
                          required) != required) {
    return {};
  }
  return result;
}

std::filesystem::path findWindowsExecutable(const wchar_t* name) {
  std::array<wchar_t, 32768> directory{};
  const UINT length = GetSystemDirectoryW(
      directory.data(), static_cast<UINT>(directory.size()));
  if (length == 0 || static_cast<std::size_t>(length) >= directory.size()) {
    return {};
  }
  const std::filesystem::path candidate =
      std::filesystem::path(directory.data()) / name;
  const DWORD attributes = GetFileAttributesW(candidate.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
                 (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
             ? candidate
             : std::filesystem::path{};
}

struct WindowsCommandResult final {
  int exitCode{-1};
  bool cancelled{};
};

WindowsCommandResult runWindowsCommand(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const WindowsToGoCancelCallback& isCancelled = {}) {
  WindowsCommandResult result;
  if (executable.empty() || arguments.empty()) {
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    return result;
  }
  std::vector<const wchar_t*> native;
  native.reserve(arguments.size() + 1U);
  for (const auto& argument : arguments) {
    native.push_back(argument.c_str());
  }
  native.push_back(nullptr);
  const intptr_t spawned = _wspawnv(_P_NOWAIT, executable.c_str(), native.data());
  if (spawned == -1) {
    return result;
  }
  const HANDLE process = reinterpret_cast<HANDLE>(spawned);
  for (;;) {
    const DWORD wait = WaitForSingleObject(process, 100);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_TIMEOUT) {
      CloseHandle(process);
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      static_cast<void>(TerminateProcess(process, ERROR_CANCELLED));
      static_cast<void>(WaitForSingleObject(process, INFINITE));
      break;
    }
  }
  DWORD exitCode = ERROR_GEN_FAILURE;
  if (!result.cancelled && GetExitCodeProcess(process, &exitCode) &&
      exitCode <= static_cast<DWORD>(std::numeric_limits<int>::max())) {
    result.exitCode = static_cast<int>(exitCode);
  }
  CloseHandle(process);
  return result;
}

void putBigEndian32(unsigned char* output, const std::uint32_t value) {
  output[0] = static_cast<unsigned char>((value >> 24U) & 0xffU);
  output[1] = static_cast<unsigned char>((value >> 16U) & 0xffU);
  output[2] = static_cast<unsigned char>((value >> 8U) & 0xffU);
  output[3] = static_cast<unsigned char>(value & 0xffU);
}

void putBigEndian64(unsigned char* output, const std::uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index) {
    output[index] =
        static_cast<unsigned char>((value >> ((7U - index) * 8U)) & 0xffU);
  }
}

bool appendFixedVhdFooter(const std::filesystem::path& path,
                          const std::uint64_t capacityBytes,
                          std::string& error) {
  std::array<unsigned char, 512> footer{};
  constexpr std::array<unsigned char, 8> cookie{'c', 'o', 'n', 'e', 'c', 't', 'i', 'x'};
  std::copy(cookie.begin(), cookie.end(), footer.begin());
  putBigEndian32(footer.data() + 8, 2);
  putBigEndian32(footer.data() + 12, 0x00010000U);
  putBigEndian64(footer.data() + 16, std::numeric_limits<std::uint64_t>::max());
  constexpr std::uint64_t windowsToUnixEpochSeconds = 946684800ULL;
  const auto unixSeconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  putBigEndian32(footer.data() + 24, static_cast<std::uint32_t>(
      unixSeconds > windowsToUnixEpochSeconds
          ? unixSeconds - windowsToUnixEpochSeconds
          : 0));
  constexpr std::array<unsigned char, 4> creator{'R', 'f', 'Q', 't'};
  constexpr std::array<unsigned char, 4> host{'W', 'i', '2', 'k'};
  std::copy(creator.begin(), creator.end(), footer.begin() + 28);
  putBigEndian32(footer.data() + 32, 0x00010000U);
  std::copy(host.begin(), host.end(), footer.begin() + 36);
  putBigEndian64(footer.data() + 40, capacityBytes);
  putBigEndian64(footer.data() + 48, capacityBytes);

  std::uint64_t sectors = std::min<std::uint64_t>(
      capacityBytes / 512U, 65535ULL * 16ULL * 255ULL);
  std::uint32_t sectorsPerTrack = 17;
  std::uint32_t heads = static_cast<std::uint32_t>(
      (sectors / sectorsPerTrack + 1023U) / 1024U);
  heads = std::max<std::uint32_t>(heads, 4);
  if (sectors >= static_cast<std::uint64_t>(heads) * 1024U * sectorsPerTrack ||
      heads > 16U) {
    heads = 16;
    sectorsPerTrack = 31;
  }
  if (sectors >= static_cast<std::uint64_t>(heads) * 1024U * sectorsPerTrack) {
    sectorsPerTrack = 63;
  }
  const std::uint32_t cylinders = static_cast<std::uint32_t>(
      sectors / (static_cast<std::uint64_t>(heads) * sectorsPerTrack));
  footer[56] = static_cast<unsigned char>((cylinders >> 8U) & 0xffU);
  footer[57] = static_cast<unsigned char>(cylinders & 0xffU);
  footer[58] = static_cast<unsigned char>(heads);
  footer[59] = static_cast<unsigned char>(sectorsPerTrack);
  putBigEndian32(footer.data() + 60, 2);
  const GuidBytes identifier = randomGuid();
  std::copy(identifier.begin(), identifier.end(), footer.begin() + 68);
  std::uint32_t sum = 0;
  for (const unsigned char byte : footer) {
    sum += byte;
  }
  putBigEndian32(footer.data() + 64, ~sum);

  std::ofstream output(path, std::ios::binary | std::ios::app);
  output.write(reinterpret_cast<const char*>(footer.data()),
               static_cast<std::streamsize>(footer.size()));
  output.flush();
  if (!output) {
    error = "Unable to append the fixed-VHD attachment footer";
    return false;
  }
  return true;
}

std::pair<wchar_t, wchar_t> availableDriveLetters() {
  const DWORD mask = GetLogicalDrives();
  wchar_t first = 0;
  wchar_t second = 0;
  for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
    const DWORD bit = 1UL << (letter - L'A');
    if ((mask & bit) != 0) {
      continue;
    }
    if (first == 0) {
      first = letter;
    } else {
      second = letter;
      break;
    }
  }
  return {first, second};
}

class WindowsWindowsToGoImageStager final : public WindowsToGoImageStager {
 public:
  WindowsWindowsToGoImageStager() : wim_(core::createSystemWimApplicator()) {}

  [[nodiscard]] WindowsToGoAvailability availability() const override {
    if (!IsUserAnAdmin()) {
      return {false, "Windows To Go staging requires an elevated application process"};
    }
    if (wim_ == nullptr || !wim_->available()) {
      return {false, wim_ == nullptr ? "No wimlib application backend is available"
                                     : wim_->availabilityReason()};
    }
    if (findWindowsExecutable(L"diskpart.exe").empty() ||
        findWindowsExecutable(L"bcdboot.exe").empty()) {
      return {false, "Windows diskpart.exe and bcdboot.exe are required"};
    }
    const auto letters = availableDriveLetters();
    if (letters.first == 0 || letters.second == 0) {
      return {false, "Two unused drive letters are required for Windows To Go staging"};
    }
    return {true, {}};
  }

  [[nodiscard]] WindowsToGoStageResult stage(
      const core::WindowsToGoPlan& plan,
      const std::filesystem::path& outputPath,
      const WindowsToGoProgressCallback& onProgress,
      const WindowsToGoCancelCallback& isCancelled) const override {
    WindowsToGoStageResult result;
    const WindowsToGoAvailability ready = availability();
    if (!ready.available) {
      result.error = ready.reason;
      return result;
    }
    if (plan.target().logicalSectorSize != kSectorSize) {
      result.error = "Windows To Go GPT staging currently supports 512-byte-sector targets";
      return result;
    }
    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) || fileError) {
      result.error = fileError ? "Unable to validate the staging output: " +
                                     fileError.message()
                               : "The Windows To Go staging output already exists";
      return result;
    }
    if (!validateStagingSpace(plan, outputPath, result.error)) {
      return result;
    }
    auto workPath = outputPath;
    workPath += L".wtg-work";
    if (!std::filesystem::create_directory(workPath, fileError)) {
      result.error = fileError ? "Unable to create the Windows To Go work directory: " +
                                     fileError.message()
                               : "The Windows To Go work directory already exists";
      return result;
    }
    ScopedWork scoped;
    scoped.set(workPath, outputPath);
    const auto installImage = workPath / L"install-image.wim";
    const auto unattended = workPath / L"unattend.xml";
    std::ofstream answerFile(unattended, std::ios::binary | std::ios::trunc);
    answerFile.write(plan.unattendXml().data(),
                     static_cast<std::streamsize>(plan.unattendXml().size()));
    answerFile.flush();
    if (!answerFile) {
      result.error = "Unable to prepare the Windows To Go answer file";
      return result;
    }
    answerFile.close();
    if (onProgress) {
      onProgress({WindowsToGoStage::ExtractingInstallImage, 0, 0,
                  "Extracting the selected Windows install image"});
    }
    const auto extracted = core::extractWindowsToGoSource(
        plan, installImage,
        [&](const core::WindowsToGoSourceProgress& progress) {
          if (onProgress) {
            onProgress({WindowsToGoStage::ExtractingInstallImage,
                        progress.bytesProcessed, progress.totalBytes,
                        "Extracting sources/install.wim or install.esd"});
          }
        },
        isCancelled);
    if (!extracted.success) {
      result.cancelled = extracted.cancelled;
      result.error = extracted.error;
      return result;
    }
    if (onProgress) {
      onProgress({WindowsToGoStage::CreatingDiskLayout, 0, 0,
                  "Creating the attachable GPT disk image"});
    }
    if (!createGptImage(outputPath, plan.target().capacityBytes, result.error) ||
        !appendFixedVhdFooter(outputPath, plan.target().capacityBytes, result.error)) {
      return result;
    }

    const auto letters = availableDriveLetters();
    const wchar_t windowsLetter = letters.first;
    const wchar_t espLetter = letters.second;
    const auto diskpart = findWindowsExecutable(L"diskpart.exe");
    const auto bcdboot = findWindowsExecutable(L"bcdboot.exe");
    const auto attachScript = workPath / L"attach.txt";
    const auto detachScript = workPath / L"detach.txt";
    const auto absoluteOutput = std::filesystem::absolute(outputPath, fileError);
    if (fileError) {
      result.error = "Unable to resolve the Windows To Go staging path: " +
                     fileError.message();
      return result;
    }
    {
      std::wofstream script(attachScript, std::ios::trunc);
      script << L"select vdisk file=\"" << absoluteOutput.native() << L"\"\n"
             << L"attach vdisk\n"
             << L"select partition 1\n"
             << L"format fs=fat32 quick label=RUFUSPP_ESP\n"
             << L"assign letter=" << espLetter << L"\n"
             << L"select partition 3\n"
             << L"format fs=ntfs quick label=\""
             << utf8ToWide(normalizedNtfsLabel(plan.volumeLabel()))
             << L"\"\n"
             << L"assign letter=" << windowsLetter << L"\n";
      if (!script) {
        result.error = "Unable to create the diskpart attachment script";
        return result;
      }
    }
    {
      std::wofstream script(detachScript, std::ios::trunc);
      script << L"select vdisk file=\"" << absoluteOutput.native() << L"\"\n"
             << L"detach vdisk\n";
    }
    if (onProgress) {
      onProgress({WindowsToGoStage::FormattingFilesystems, 0, 0,
                  "Formatting the EFI and Windows partitions"});
    }
    const WindowsCommandResult attached = runWindowsCommand(
        diskpart, {L"diskpart.exe", L"/s", attachScript.native()}, isCancelled);
    bool vhdAttached = attached.exitCode == 0;
    const auto detach = [&] {
      if (!vhdAttached) {
        return true;
      }
      const auto detached = runWindowsCommand(
          diskpart, {L"diskpart.exe", L"/s", detachScript.native()});
      if (detached.exitCode != 0) {
        return false;
      }
      vhdAttached = false;
      return true;
    };
    if (!vhdAttached) {
      result.cancelled = attached.cancelled;
      result.error = "diskpart could not attach and format the Windows To Go staging image";
      return result;
    }
    std::wstring windowsRoot;
    windowsRoot.push_back(windowsLetter);
    windowsRoot += L":\\";
    std::wstring espRoot;
    espRoot.push_back(espLetter);
    espRoot += L":\\";
    if (onProgress) {
      onProgress({WindowsToGoStage::ApplyingWindows, 0, 0,
                  "Applying " + plan.edition().name + " to NTFS"});
    }
    const core::WimApplyResult applied = wim_->apply(
        installImage, plan.edition().index, std::filesystem::path(windowsRoot),
        false, unattended,
        [&](const core::WimApplyProgress& progress) {
          if (onProgress) {
            onProgress({WindowsToGoStage::ApplyingWindows,
                        progress.bytesProcessed, progress.totalBytes,
                        "Applying " + plan.edition().name});
          }
        },
        isCancelled);
    if (!applied.success) {
      result.cancelled = applied.cancelled;
      result.error = applied.error;
      if (!detach()) {
        result.error += "; diskpart also could not detach the staging image";
      }
      return result;
    }
    if (onProgress) {
      onProgress({WindowsToGoStage::ConfiguringBoot, 0, 0,
                  "Running BCDBoot for UEFI startup"});
    }
    const std::wstring windowsDirectory = windowsRoot + L"Windows";
    const WindowsCommandResult booted = runWindowsCommand(
        bcdboot,
        {L"bcdboot.exe", windowsDirectory, L"/s", espRoot, L"/f", L"UEFI"},
        isCancelled);
    if (booted.exitCode != 0) {
      result.cancelled = booted.cancelled;
      result.error = "BCDBoot could not construct the Windows To Go boot files";
      if (!detach()) {
        result.error += "; diskpart also could not detach the staging image";
      }
      return result;
    }
    if (!detach()) {
      result.error = "diskpart could not detach the Windows To Go staging image";
      return result;
    }
    std::filesystem::resize_file(outputPath, plan.target().capacityBytes, fileError);
    if (fileError) {
      result.error = "Unable to remove the staging-only VHD footer: " +
                     fileError.message();
      return result;
    }
    if (onProgress) {
      onProgress({WindowsToGoStage::Finalizing, plan.target().capacityBytes,
                  plan.target().capacityBytes,
                  "Finalizing the Windows To Go disk image"});
    }
    core::ImageInfo image;
    image.path = outputPath.string();
    image.displayName = outputPath.filename().string();
    image.volumeLabel = plan.volumeLabel();
    image.sizeBytes = plan.target().capacityBytes;
    image.expandedSizeBytes = image.sizeBytes;
    image.format = core::ImageFormat::Raw;
    image.partitionScheme = core::PartitionScheme::Gpt;
    image.family = core::ImageFamily::WindowsInstaller;
    image.architecture = plan.edition().architecture;
    image.bootable = true;
    image.capabilities.rawWrite = true;
    image.capabilities.validPartitionTable = true;
    image.capabilities.uefiBootable = true;
    result.stagedImage = std::move(image);
    result.success = true;
    scoped.keepOutput();
    std::filesystem::remove_all(workPath, fileError);
    if (onProgress) {
      onProgress({WindowsToGoStage::Complete, plan.target().capacityBytes,
                  plan.target().capacityBytes, "Windows To Go staging complete"});
    }
    return result;
  }

 private:
  std::shared_ptr<const core::WimApplicator> wim_;
};

class WindowsNtfsIsoImageStager final : public NtfsIsoImageStager {
 public:
  [[nodiscard]] NtfsIsoAvailability availability() const override {
    if (!IsUserAnAdmin()) {
      return {false,
              "NTFS ISO staging requires an elevated application process"};
    }
    if (findWindowsExecutable(L"diskpart.exe").empty()) {
      return {false, "Windows diskpart.exe is required for NTFS ISO staging"};
    }
    if (availableDriveLetters().first == 0) {
      return {false, "An unused drive letter is required for NTFS ISO staging"};
    }
    return {true, {}};
  }

  [[nodiscard]] core::IsoDeploymentResult stage(
      const core::IsoDeploymentPlan& plan,
      const std::filesystem::path& outputPath,
      const core::IsoDeploymentProgressCallback& onProgress,
      const core::IsoDeploymentCancelCallback& isCancelled) const override {
    core::IsoDeploymentResult result;
    const auto ready = availability();
    if (!ready.available) {
      result.error = ready.reason;
      return result;
    }
    if (plan.fileSystem() != core::IsoDeploymentFilesystem::Ntfs ||
        plan.target().logicalSectorSize != kSectorSize) {
      result.error = "The plan is not a supported 512-byte-sector NTFS ISO deployment";
      return result;
    }
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "NTFS ISO staging was cancelled before it began";
      return result;
    }
    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) || fileError) {
      result.error = fileError
                         ? "Unable to validate the NTFS staging output: " +
                               fileError.message()
                         : "The NTFS staging output already exists";
      return result;
    }
    if (!validateNtfsStagingSpace(plan, outputPath, result.error)) {
      return result;
    }
    auto workPath = outputPath;
    workPath += L".ntfs-work";
    if (!std::filesystem::create_directory(workPath, fileError)) {
      result.error = fileError
                         ? "Unable to create the NTFS staging work directory: " +
                               fileError.message()
                         : "The NTFS staging work directory already exists";
      return result;
    }
    ScopedWork scoped;
    scoped.set(workPath, outputPath);
    if (onProgress) {
      onProgress({core::IsoDeploymentStage::Formatting, 0, 0,
                  "Creating MBR, NTFS, and UEFI:NTFS partitions"});
    }
    if (!createNtfsIsoLayoutImage(outputPath, plan.target().capacityBytes,
                                  plan.partitionScheme(), plan.targetSystem(), result.error) ||
        !appendFixedVhdFooter(outputPath, plan.target().capacityBytes,
                              result.error)) {
      return result;
    }

    const wchar_t letter = availableDriveLetters().first;
    const auto diskpart = findWindowsExecutable(L"diskpart.exe");
    const auto attachScript = workPath / L"attach.txt";
    const auto detachScript = workPath / L"detach.txt";
    const auto absoluteOutput = std::filesystem::absolute(outputPath, fileError);
    if (fileError) {
      result.error = "Unable to resolve the NTFS staging path: " +
                     fileError.message();
      return result;
    }
    {
      std::wofstream script(attachScript, std::ios::trunc);
      script << L"select vdisk file=\"" << absoluteOutput.native() << L"\"\n"
             << L"attach vdisk\n"
             << L"select partition 1\n"
             << L"format fs=ntfs"
             << (plan.quickFormat() ? L" quick" : L"")
             << (plan.clusterSizeBytes() == 0U
                     ? std::wstring{}
                     : L" unit=" + std::to_wstring(plan.clusterSizeBytes()))
             << L" label=\""
             << utf8ToWide(normalizedNtfsLabel(plan.volumeLabel())) << L"\"\n"
             << L"assign letter=" << letter << L"\n";
      if (!script) {
        result.error = "Unable to create the NTFS diskpart attachment script";
        return result;
      }
    }
    {
      std::wofstream script(detachScript, std::ios::trunc);
      script << L"select vdisk file=\"" << absoluteOutput.native() << L"\"\n"
             << L"detach vdisk\n";
      if (!script) {
        result.error = "Unable to create the NTFS diskpart detach script";
        return result;
      }
    }
    const WindowsCommandResult attached = runWindowsCommand(
        diskpart, {L"diskpart.exe", L"/s", attachScript.native()},
        isCancelled);
    bool vhdAttached = attached.exitCode == 0;
    const auto detach = [&] {
      if (!vhdAttached) {
        return true;
      }
      const auto detached = runWindowsCommand(
          diskpart, {L"diskpart.exe", L"/s", detachScript.native()});
      if (detached.exitCode != 0) {
        return false;
      }
      vhdAttached = false;
      return true;
    };
    if (!vhdAttached) {
      result.cancelled = attached.cancelled;
      result.error = "diskpart could not attach and format the NTFS staging image";
      return result;
    }
    std::wstring mountedRoot;
    mountedRoot.push_back(letter);
    mountedRoot += L":\\";
    const auto extractingProgress = [&](const core::IsoDeploymentProgress& progress) {
      if (onProgress && progress.stage != core::IsoDeploymentStage::Complete) onProgress(progress);
    };
    result = core::IsoImageStager{}.extractToDirectory(
        plan, std::filesystem::path(mountedRoot), extractingProgress, isCancelled);
    if (!detach()) {
      result.success = false;
      result.error = "diskpart could not detach the NTFS staging image";
      return result;
    }
    if (!result.success) {
      return result;
    }
    std::filesystem::resize_file(outputPath, plan.target().capacityBytes,
                                 fileError);
    if (fileError) {
      result.success = false;
      result.error = "Unable to remove the staging-only VHD footer: " +
                     fileError.message();
      return result;
    }

    if (plan.legacyBiosBootstrap() == core::LegacyBiosBootstrap::WindowsBootManager) {
      std::ifstream layout(outputPath, std::ios::binary);
      std::array<unsigned char, 512> mbr{};
      layout.read(reinterpret_cast<char*>(mbr.data()), mbr.size());
      std::uint64_t sectors = 0;
      for (unsigned i = 0; i < 4; ++i) sectors |= std::uint64_t(mbr[458U + i]) << (8U * i);
      if (!layout || !detail::installWindowsNtfsBootstrap(
          outputPath, kPartitionAlignmentSectors * kSectorSize, sectors * kSectorSize, result.error)) {
        result.success = false;
        if (result.error.empty()) result.error = "Unable to read the Windows NTFS staging extent";
        return result;
      }
    }

    core::ImageInfo staged;
    staged.path = outputPath.string();
    staged.displayName = outputPath.filename().string();
    staged.volumeLabel = plan.volumeLabel();
    staged.sizeBytes = plan.target().capacityBytes;
    staged.expandedSizeBytes = staged.sizeBytes;
    staged.format = core::ImageFormat::Raw;
    staged.partitionScheme = plan.partitionScheme();
    staged.family = plan.image().family;
    staged.architecture = plan.image().architecture;
    staged.bootable = true;
    staged.capabilities.rawWrite = true;
    staged.capabilities.uefiBootable = plan.targetSystem() != core::IsoTargetSystem::Bios;
    staged.capabilities.biosBootable = plan.legacyBiosBootstrap() == core::LegacyBiosBootstrap::WindowsBootManager;
    staged.capabilities.validPartitionTable = true;
    result.stagedImage = std::move(staged);
    scoped.keepOutput();
    std::filesystem::remove_all(workPath, fileError);
    if (onProgress) {
      onProgress({core::IsoDeploymentStage::Complete,
                  result.bytesExtracted, result.bytesExtracted,
                  "NTFS ISO staging complete"});
    }
    return result;
  }
};

#endif

}  // namespace

NtfsIsoStagingRequirements estimateNtfsIsoStagingRequirements(
    const std::uint64_t expandedImageBytes,
    const std::uint64_t targetCapacityBytes, const bool quickFormat) {
  NtfsIsoStagingRequirements result;
  const auto overflow = [&] {
    result.error = "The NTFS staging space requirement overflowed";
    return result;
  };
  std::uint64_t allowance = targetCapacityBytes;
  if (quickFormat) {
    if (expandedImageBytes >
        (std::numeric_limits<std::uint64_t>::max() - 9U) / 11U) {
      return overflow();
    }
    allowance = (expandedImageBytes * 11U + 9U) / 10U;
  }
#if !defined(_WIN32)
  // The formatted partition and assembled output coexist until verification.
  if (allowance > std::numeric_limits<std::uint64_t>::max() / 2U) {
    return overflow();
  }
  allowance *= 2U;
#endif
  if (allowance > std::numeric_limits<std::uint64_t>::max() -
                      kNtfsStagingReserveBytes) {
    return overflow();
  }
  result.temporaryBytes = allowance + kNtfsStagingReserveBytes;
#if !defined(_WIN32)
  result.writeSizePending = quickFormat;
#endif
  result.success = true;
  return result;
}

std::unique_ptr<WindowsToGoImageStager> makePlatformWindowsToGoImageStager() {
#if defined(_WIN32)
  return std::make_unique<WindowsWindowsToGoImageStager>();
#else
  return std::make_unique<PosixWindowsToGoImageStager>();
#endif
}

std::unique_ptr<NtfsIsoImageStager> makePlatformNtfsIsoImageStager() {
#if defined(_WIN32)
  return std::make_unique<WindowsNtfsIsoImageStager>();
#else
  return std::make_unique<PosixNtfsIsoImageStager>();
#endif
}

NtfsIsoLayoutResult createNtfsIsoMbrImage(
    const std::filesystem::path& outputPath,
    const std::uint64_t capacityBytes,
    const core::IsoTargetSystem targetSystem) {
  return createNtfsIsoDiskImage(outputPath, capacityBytes, core::PartitionScheme::Mbr, targetSystem);
}

NtfsIsoLayoutResult createNtfsIsoDiskImage(
    const std::filesystem::path& outputPath,
    const std::uint64_t capacityBytes,
    const core::PartitionScheme partitionScheme,
    const core::IsoTargetSystem targetSystem) {
  NtfsIsoLayoutResult result;
  std::error_code fileError;
  const auto status = std::filesystem::symlink_status(outputPath, fileError);
  if (std::filesystem::exists(status) ||
      (fileError && fileError != std::errc::no_such_file_or_directory)) {
    result.error = "The NTFS layout output already exists or cannot be inspected";
    return result;
  }
  if (partitionScheme != core::PartitionScheme::Mbr &&
      partitionScheme != core::PartitionScheme::Gpt) {
    result.error = "A concrete MBR or GPT NTFS layout is required";
    return result;
  }
  result.success = createNtfsIsoLayoutImage(
      outputPath, capacityBytes, partitionScheme, targetSystem, result.error);
  if (!result.success) {
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);
  }
  return result;
}

WindowsToGoLayoutResult createWindowsToGoGptImage(
    const std::filesystem::path& outputPath,
    const std::uint64_t capacityBytes) {
  WindowsToGoLayoutResult result;
  result.success = createGptImage(outputPath, capacityBytes, result.error);
  if (!result.success) {
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);
  }
  return result;
}

const char* windowsToGoStageName(const WindowsToGoStage stage) noexcept {
  switch (stage) {
    case WindowsToGoStage::ExtractingInstallImage:
      return "Extracting Windows image";
    case WindowsToGoStage::CreatingDiskLayout:
      return "Creating disk layout";
    case WindowsToGoStage::FormattingFilesystems:
      return "Formatting filesystems";
    case WindowsToGoStage::ApplyingWindows:
      return "Applying Windows";
    case WindowsToGoStage::ConfiguringBoot:
      return "Configuring boot files";
    case WindowsToGoStage::Finalizing:
      return "Finalizing";
    case WindowsToGoStage::Complete:
      return "Complete";
  }
  return "Windows To Go";
}

}  // namespace rufus::backend
