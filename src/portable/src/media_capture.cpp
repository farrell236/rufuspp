/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/media_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "rufus/core/media.hpp"

namespace rufus::core {
namespace {

constexpr std::uint64_t kSectorBytes = 512U;
constexpr std::uint32_t kDynamicBlockBytes = 2U * 1024U * 1024U;
constexpr std::uint64_t kMaximumClassicVhdBytes =
    2040ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMinimumTransferBytes = 4096U;
constexpr std::size_t kMaximumTransferBytes = 64U * 1024U * 1024U;

struct PartialFile final {
  std::filesystem::path path;
  bool committed{};

  ~PartialFile() {
    if (!committed) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};

void putBig32(unsigned char* bytes, const std::uint32_t value) {
  for (unsigned int index = 0; index < 4U; ++index) {
    bytes[index] = static_cast<unsigned char>(value >> ((3U - index) * 8U));
  }
}

void putBig64(unsigned char* bytes, const std::uint64_t value) {
  for (unsigned int index = 0; index < 8U; ++index) {
    bytes[index] = static_cast<unsigned char>(value >> ((7U - index) * 8U));
  }
}

void putLittle16(unsigned char* bytes, const std::uint16_t value) {
  bytes[0] = static_cast<unsigned char>(value);
  bytes[1] = static_cast<unsigned char>(value >> 8U);
}

void putLittle32(unsigned char* bytes, const std::uint32_t value) {
  for (unsigned int index = 0; index < 4U; ++index) {
    bytes[index] = static_cast<unsigned char>(value >> (index * 8U));
  }
}

void putLittle64(unsigned char* bytes, const std::uint64_t value) {
  for (unsigned int index = 0; index < 8U; ++index) {
    bytes[index] = static_cast<unsigned char>(value >> (index * 8U));
  }
}

std::uint32_t crc32c(const unsigned char* bytes, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= bytes[index];
    for (unsigned int bit = 0; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

std::filesystem::path partialPathFor(const std::filesystem::path& destination) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  auto partial = destination;
  partial += ".rufus-plus-plus-capture-" + std::to_string(nonce);
  return partial;
}

void report(const MediaCaptureProgressCallback& callback,
            const MediaCaptureStage stage, const std::uint64_t processed,
            const std::uint64_t total) {
  if (callback) {
    callback({stage, processed, total});
  }
}

bool cancelled(const MediaCaptureCancelCallback& callback) {
  return callback && callback();
}

std::array<unsigned char, 16> randomUuid() {
  std::array<unsigned char, 16> uuid{};
  std::random_device random;
  for (auto& byte : uuid) {
    byte = static_cast<unsigned char>(random());
  }
  uuid[6] = static_cast<unsigned char>((uuid[6] & 0x0fU) | 0x40U);
  uuid[8] = static_cast<unsigned char>((uuid[8] & 0x3fU) | 0x80U);
  return uuid;
}

std::uint32_t vhdTimestamp() {
  constexpr std::int64_t unixToVhdEpoch = 946684800LL;
  const auto unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if (unixSeconds <= unixToVhdEpoch) {
    return 0;
  }
  return static_cast<std::uint32_t>(std::min<std::int64_t>(
      unixSeconds - unixToVhdEpoch,
      std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t vhdGeometry(const std::uint64_t sizeBytes) {
  std::uint64_t sectors = std::min<std::uint64_t>(
      sizeBytes / kSectorBytes, 65535ULL * 16ULL * 255ULL);
  std::uint32_t sectorsPerTrack = 17U;
  std::uint32_t cylinderHeads =
      static_cast<std::uint32_t>((sectors + sectorsPerTrack - 1U) /
                                 sectorsPerTrack);
  std::uint32_t heads = (cylinderHeads + 1023U) / 1024U;
  heads = std::max<std::uint32_t>(heads, 4U);
  if (cylinderHeads >= heads * 1024U || heads > 16U) {
    sectorsPerTrack = 31U;
    heads = 16U;
    cylinderHeads = static_cast<std::uint32_t>(
        (sectors + sectorsPerTrack - 1U) / sectorsPerTrack);
  }
  if (cylinderHeads >= heads * 1024U) {
    sectorsPerTrack = 63U;
    heads = 16U;
    cylinderHeads = static_cast<std::uint32_t>(
        (sectors + sectorsPerTrack - 1U) / sectorsPerTrack);
  }
  const std::uint32_t cylinders = std::min<std::uint32_t>(
      cylinderHeads / heads, std::numeric_limits<std::uint16_t>::max());
  return (cylinders << 16U) | (heads << 8U) | sectorsPerTrack;
}

std::array<unsigned char, 512> makeVhdFooter(const std::uint64_t sizeBytes,
                                              const bool dynamic,
                                              const std::array<unsigned char, 16>& uuid) {
  std::array<unsigned char, 512> footer{};
  std::copy_n("conectix", 8, footer.begin());
  putBig32(footer.data() + 8U, 2U);
  putBig32(footer.data() + 12U, 0x00010000U);
  putBig64(footer.data() + 16U,
           dynamic ? kSectorBytes : std::numeric_limits<std::uint64_t>::max());
  putBig32(footer.data() + 24U, vhdTimestamp());
  std::copy_n("Rufs", 4, footer.begin() + 28U);
  putBig32(footer.data() + 32U, 0x00010000U);
  std::copy_n("Wi2k", 4, footer.begin() + 36U);
  putBig64(footer.data() + 40U, sizeBytes);
  putBig64(footer.data() + 48U, sizeBytes);
  putBig32(footer.data() + 56U, vhdGeometry(sizeBytes));
  putBig32(footer.data() + 60U, dynamic ? 3U : 2U);
  std::copy(uuid.begin(), uuid.end(), footer.begin() + 68U);
  std::uint32_t sum = 0;
  for (const auto byte : footer) {
    sum += byte;
  }
  putBig32(footer.data() + 64U, ~sum);
  return footer;
}

std::array<unsigned char, 1024> makeDynamicHeader(
    const std::uint64_t tableOffset, const std::uint32_t entries) {
  std::array<unsigned char, 1024> header{};
  std::copy_n("cxsparse", 8, header.begin());
  putBig64(header.data() + 8U, std::numeric_limits<std::uint64_t>::max());
  putBig64(header.data() + 16U, tableOffset);
  putBig32(header.data() + 24U, 0x00010000U);
  putBig32(header.data() + 28U, entries);
  putBig32(header.data() + 32U, kDynamicBlockBytes);
  std::uint32_t sum = 0;
  for (const auto byte : header) {
    sum += byte;
  }
  putBig32(header.data() + 36U, ~sum);
  return header;
}

struct VhdxLayout final {
  std::vector<unsigned char> preamble;
  std::uint64_t batOffset{};
  std::uint32_t batLength{};
  std::uint64_t payloadOffset{};
};

bool makeVhdxLayout(const std::uint64_t virtualBytes,
                    const std::uint32_t logicalSectorBytes,
                    VhdxLayout& layout, std::string& error) {
  constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
  constexpr std::uint64_t metadataOffset = mebibyte;
  constexpr std::uint32_t metadataLength = 1024U * 1024U;
  constexpr std::uint64_t batOffset = 2U * mebibyte;
  constexpr std::uint32_t logLength = 1024U * 1024U;
  const std::uint64_t blockCount =
      (virtualBytes - 1U) / kDynamicBlockBytes + 1U;
  const std::uint64_t chunkRatio =
      (static_cast<std::uint64_t>(1U) << 23U) * logicalSectorBytes /
      kDynamicBlockBytes;
  if (chunkRatio == 0U) {
    error = "The source sector size cannot be represented by VHDX";
    return false;
  }
  const std::uint64_t highestBatIndex =
      blockCount - 1U + (blockCount - 1U) / chunkRatio;
  if (highestBatIndex > (64ULL * mebibyte) / 8U - 1U) {
    error = "The source is too large for the supported VHDX allocation table";
    return false;
  }
  const std::uint64_t batBytes = (highestBatIndex + 1U) * 8U;
  const std::uint64_t alignedBatBytes =
      std::max<std::uint64_t>(mebibyte,
                              (batBytes + mebibyte - 1U) / mebibyte * mebibyte);
  if (alignedBatBytes > std::numeric_limits<std::uint32_t>::max() ||
      batOffset > std::numeric_limits<std::uint64_t>::max() - alignedBatBytes - logLength) {
    error = "The VHDX allocation table is too large";
    return false;
  }
  layout.batOffset = batOffset;
  layout.batLength = static_cast<std::uint32_t>(alignedBatBytes);
  // Even a clean, newly created VHDX must locate its log after the
  // 1-MiB header section. Reserve an empty non-overlapping log so other
  // implementations can open the file; a zero LogGuid means no replay.
  const std::uint64_t logOffset = batOffset + alignedBatBytes;
  layout.payloadOffset = logOffset + logLength;
  if (layout.payloadOffset > std::numeric_limits<std::size_t>::max()) {
    error = "The VHDX metadata prefix is too large for this host";
    return false;
  }
  layout.preamble.assign(static_cast<std::size_t>(layout.payloadOffset), 0U);

  auto* const bytes = layout.preamble.data();
  std::copy_n("vhdxfile", 8, bytes);
  const std::u16string creator = u"Rufus++ cross-platform media capture";
  for (std::size_t index = 0; index < creator.size(); ++index) {
    putLittle16(bytes + 8U + index * 2U,
                static_cast<std::uint16_t>(creator[index]));
  }

  const auto writeHeader = [&](const std::size_t offset,
                               const std::uint64_t sequence) {
    auto* const header = bytes + offset;
    std::copy_n("head", 4, header);
    putLittle64(header + 8U, sequence);
    const auto fileWrite = randomUuid();
    const auto dataWrite = randomUuid();
    std::copy(fileWrite.begin(), fileWrite.end(), header + 16U);
    std::copy(dataWrite.begin(), dataWrite.end(), header + 32U);
    putLittle16(header + 66U, 1U);
    putLittle32(header + 68U, logLength);
    putLittle64(header + 72U, logOffset);
    putLittle32(header + 4U, crc32c(header, 4096U));
  };
  writeHeader(64U * 1024U, 1U);
  writeHeader(128U * 1024U, 2U);

  constexpr std::array<unsigned char, 16> batRegion{
      0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
      0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
  constexpr std::array<unsigned char, 16> metadataRegion{
      0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
      0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
  const auto writeRegionTable = [&](const std::size_t offset) {
    auto* const table = bytes + offset;
    std::copy_n("regi", 4, table);
    putLittle32(table + 8U, 2U);
    std::copy(batRegion.begin(), batRegion.end(), table + 16U);
    putLittle64(table + 32U, layout.batOffset);
    putLittle32(table + 40U, layout.batLength);
    putLittle32(table + 44U, 1U);
    std::copy(metadataRegion.begin(), metadataRegion.end(), table + 48U);
    putLittle64(table + 64U, metadataOffset);
    putLittle32(table + 72U, metadataLength);
    putLittle32(table + 76U, 1U);
    putLittle32(table + 4U, crc32c(table, 64U * 1024U));
  };
  writeRegionTable(192U * 1024U);
  writeRegionTable(256U * 1024U);

  constexpr std::array<unsigned char, 16> fileParameters{
      0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
      0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
  constexpr std::array<unsigned char, 16> virtualDiskSize{
      0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
      0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
  constexpr std::array<unsigned char, 16> logicalSectorSize{
      0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
      0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
  constexpr std::array<unsigned char, 16> physicalSectorSize{
      0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44,
      0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56};
  constexpr std::array<unsigned char, 16> page83Data{
      0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45,
      0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46};
  auto* const metadata = bytes + metadataOffset;
  std::copy_n("metadata", 8, metadata);
  putLittle16(metadata + 10U, 5U);
  constexpr std::uint32_t itemBase = 64U * 1024U;
  const auto putMetadataEntry = [&](const std::size_t entry,
                                    const std::array<unsigned char, 16>& id,
                                    const std::uint32_t itemOffset,
                                    const std::uint32_t itemLength,
                                    const std::uint32_t flags) {
    auto* const record = metadata + 32U + entry * 32U;
    std::copy(id.begin(), id.end(), record);
    putLittle32(record + 16U, itemOffset);
    putLittle32(record + 20U, itemLength);
    putLittle32(record + 24U, flags);
  };
  putMetadataEntry(0U, fileParameters, itemBase, 8U, 4U);
  putMetadataEntry(1U, virtualDiskSize, itemBase + 8U, 8U, 6U);
  putMetadataEntry(2U, logicalSectorSize, itemBase + 16U, 4U, 6U);
  putMetadataEntry(3U, physicalSectorSize, itemBase + 20U, 4U, 6U);
  putMetadataEntry(4U, page83Data, itemBase + 24U, 16U, 6U);
  putLittle32(metadata + itemBase, kDynamicBlockBytes);
  putLittle32(metadata + itemBase + 4U, 0U);
  putLittle64(metadata + itemBase + 8U, virtualBytes);
  putLittle32(metadata + itemBase + 16U, logicalSectorBytes);
  putLittle32(metadata + itemBase + 20U, logicalSectorBytes);
  const auto page83 = randomUuid();
  std::copy(page83.begin(), page83.end(), metadata + itemBase + 24U);
  return true;
}

bool writeBytes(std::ofstream& output, const unsigned char* bytes,
                const std::size_t size, std::string& error) {
  output.write(reinterpret_cast<const char*>(bytes),
               static_cast<std::streamsize>(size));
  if (!output) {
    error = "Unable to write the capture image";
    return false;
  }
  return true;
}

bool verifyCapture(RawSourceIo& source, const std::filesystem::path& path,
                   ImageInfo capturedImage, const std::size_t transferBytes,
                   const MediaCaptureProgressCallback& onProgress,
                   const MediaCaptureCancelCallback& isCancelled,
                   MediaCaptureResult& result) {
  capturedImage.path = path.string();
  auto opened = openRawImageSource(capturedImage, path);
  if (!opened.succeeded()) {
    result.error = "Unable to verify the captured image: " + opened.error;
    return false;
  }
  std::vector<unsigned char> expected(transferBytes);
  std::vector<unsigned char> observed(transferBytes);
  for (std::uint64_t offset = 0; offset < source.sizeBytes();) {
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      result.error = "Media capture verification cancelled";
      return false;
    }
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        transferBytes, source.sizeBytes() - offset));
    if (!source.readAt(offset, expected.data(), amount, result.error) ||
        !opened.source->readAt(offset, observed.data(), amount, result.error) ||
        !std::equal(expected.begin(), expected.begin() + amount,
                    observed.begin())) {
      if (result.error.empty()) {
        result.error = "Captured image verification failed";
      }
      return false;
    }
    offset += amount;
    report(onProgress, MediaCaptureStage::Verifying, offset,
           source.sizeBytes());
  }
  return true;
}

}  // namespace

MediaCaptureResult MediaCaptureWriter::capture(
    RawSourceIo& source, const std::filesystem::path& destination,
    const MediaCaptureOptions& options,
    const MediaCaptureProgressCallback& onProgress,
    const MediaCaptureCancelCallback& isCancelled) const {
  MediaCaptureResult result;
  if (source.sizeBytes() == 0U || destination.empty()) {
    result.error = "Capture source or destination is invalid";
    return result;
  }
  if (source.sizeBytes() % kSectorBytes != 0U &&
      options.format != MediaCaptureFormat::Raw) {
    result.error = "VHD capture requires a source size aligned to 512 bytes";
    return result;
  }
  if ((options.format == MediaCaptureFormat::FixedVhd ||
       options.format == MediaCaptureFormat::DynamicVhd) &&
      source.sizeBytes() > kMaximumClassicVhdBytes) {
    result.error = "Classic VHD capture is limited to 2040 GiB";
    return result;
  }
  if (options.transferBytes < kMinimumTransferBytes ||
      options.transferBytes > kMaximumTransferBytes) {
    result.error = "Capture transfer size must be between 4 KiB and 64 MiB";
    return result;
  }
  if (options.format == MediaCaptureFormat::Ffu ||
      options.format == MediaCaptureFormat::UdfIso) {
    result.error = "The selected capture format requires a platform provider";
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
  if (fileError || !std::filesystem::is_directory(parent, fileError) || fileError) {
    result.error = "The capture destination directory is unavailable";
    return result;
  }
  std::uint64_t worstCaseBytes = source.sizeBytes();
  if (options.format == MediaCaptureFormat::FixedVhd) {
    if (worstCaseBytes > std::numeric_limits<std::uint64_t>::max() - 512U) {
      result.error = "The source is too large for a fixed VHD";
      return result;
    }
    worstCaseBytes += 512U;
  } else if (options.format == MediaCaptureFormat::DynamicVhd) {
    const std::uint64_t blocks =
        (source.sizeBytes() - 1U) / kDynamicBlockBytes + 1U;
    if (blocks > std::numeric_limits<std::uint32_t>::max() ||
        blocks * 4U > 64ULL * 1024ULL * 1024ULL ||
        source.sizeBytes() > std::numeric_limits<std::uint64_t>::max() -
                                 blocks * 512U - kDynamicBlockBytes - 4096U) {
      result.error = "The source is too large for the dynamic VHD allocation table";
      return result;
    }
    worstCaseBytes += blocks * 512U + kDynamicBlockBytes + 4096U;
  } else if (options.format == MediaCaptureFormat::DynamicVhdx) {
    VhdxLayout layout;
    std::string layoutError;
    if (!makeVhdxLayout(source.sizeBytes(), static_cast<std::uint32_t>(kSectorBytes),
                        layout, layoutError) ||
        source.sizeBytes() > std::numeric_limits<std::uint64_t>::max() -
                                 layout.payloadOffset - kDynamicBlockBytes) {
      result.error = layoutError.empty() ? "The source is too large for VHDX capture"
                                         : std::move(layoutError);
      return result;
    }
    worstCaseBytes += layout.payloadOffset + kDynamicBlockBytes;
  }
  const auto space = std::filesystem::space(parent, fileError);
  if (fileError || space.available < worstCaseBytes) {
    result.error = fileError ? "Unable to inspect free space: " + fileError.message()
                             : "Not enough free space for the capture image";
    return result;
  }

  PartialFile partial{partialPathFor(destination)};
  if (std::filesystem::exists(partial.path, fileError) || fileError) {
    result.error = "Unable to allocate a private capture path";
    return result;
  }
  std::ofstream output(partial.path, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create the capture image";
    return result;
  }

  const auto uuid = randomUuid();
  const bool dynamicVhd = options.format == MediaCaptureFormat::DynamicVhd;
  const bool dynamicVhdx = options.format == MediaCaptureFormat::DynamicVhdx;
  const auto footer = makeVhdFooter(source.sizeBytes(), dynamicVhd, uuid);
  std::vector<unsigned char> buffer(
      dynamicVhd || dynamicVhdx ? kDynamicBlockBytes : options.transferBytes);
  std::uint64_t tableOffset = 0;
  std::vector<unsigned char> table;
  VhdxLayout vhdxLayout;
  if (dynamicVhd) {
    const std::uint64_t blocks =
        (source.sizeBytes() - 1U) / kDynamicBlockBytes + 1U;
    tableOffset = 3U * kSectorBytes;
    table.assign(static_cast<std::size_t>(blocks * 4U), 0xffU);
    const auto header = makeDynamicHeader(
        tableOffset, static_cast<std::uint32_t>(blocks));
    if (!writeBytes(output, footer.data(), footer.size(), result.error) ||
        !writeBytes(output, header.data(), header.size(), result.error) ||
        !writeBytes(output, table.data(), table.size(), result.error)) {
      return result;
    }
    const std::uint64_t tableEnd = tableOffset + table.size();
    const std::uint64_t alignedTableEnd =
        (tableEnd + kSectorBytes - 1U) / kSectorBytes * kSectorBytes;
    std::vector<unsigned char> padding(
        static_cast<std::size_t>(alignedTableEnd - tableEnd));
    if (!padding.empty() &&
        !writeBytes(output, padding.data(), padding.size(), result.error)) {
      return result;
    }
  } else if (dynamicVhdx) {
    if (!makeVhdxLayout(source.sizeBytes(),
                        static_cast<std::uint32_t>(kSectorBytes), vhdxLayout,
                        result.error)) {
      return result;
    }
    tableOffset = vhdxLayout.batOffset;
    table.resize(vhdxLayout.batLength, 0U);
    if (!writeBytes(output, vhdxLayout.preamble.data(),
                    vhdxLayout.preamble.size(), result.error)) {
      return result;
    }
  }

  for (std::uint64_t offset = 0; offset < source.sizeBytes();) {
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      result.error = "Media capture cancelled";
      return result;
    }
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        buffer.size(), source.sizeBytes() - offset));
    if (!source.readAt(offset, buffer.data(), amount, result.error)) {
      return result;
    }
    if (dynamicVhd || dynamicVhdx) {
      const bool allZero = std::all_of(
          buffer.begin(), buffer.begin() + amount,
          [](const unsigned char byte) { return byte == 0U; });
      if (!allZero) {
        const auto position = output.tellp();
        if (position < 0) {
          result.error = "Unable to determine the dynamic image payload offset";
          return result;
        }
        if (dynamicVhd) {
          if (static_cast<std::uint64_t>(position) / kSectorBytes >
              std::numeric_limits<std::uint32_t>::max()) {
            result.error = "Dynamic VHD capture exceeded its sector address range";
            return result;
          }
          const std::uint32_t sectorOffset = static_cast<std::uint32_t>(
              static_cast<std::uint64_t>(position) / kSectorBytes);
          putBig32(table.data() +
                       static_cast<std::size_t>(offset / kDynamicBlockBytes) * 4U,
                   sectorOffset);
          std::array<unsigned char, 512> bitmap{};
          const std::uint64_t validSectors =
              (static_cast<std::uint64_t>(amount) + kSectorBytes - 1U) /
              kSectorBytes;
          for (std::uint64_t sector = 0; sector < validSectors; ++sector) {
            bitmap[static_cast<std::size_t>(sector / 8U)] |=
                static_cast<unsigned char>(0x80U >> (sector % 8U));
          }
          if (!writeBytes(output, bitmap.data(), bitmap.size(), result.error)) {
            return result;
          }
        } else {
          constexpr std::uint64_t chunkRatio =
              (static_cast<std::uint64_t>(1U) << 23U) * kSectorBytes /
              kDynamicBlockBytes;
          const std::uint64_t block = offset / kDynamicBlockBytes;
          const std::uint64_t batIndex = block + block / chunkRatio;
          const std::uint64_t physical = static_cast<std::uint64_t>(position);
          if (physical % (1024U * 1024U) != 0U ||
              physical > 0xfffffffffff00000ULL ||
              batIndex > table.size() / 8U - 1U) {
            result.error = "Dynamic VHDX capture exceeded its allocation bounds";
            return result;
          }
          putLittle64(table.data() + static_cast<std::size_t>(batIndex) * 8U,
                      physical | 6U);
        }
        if (!writeBytes(output, buffer.data(), amount, result.error)) {
          return result;
        }
        if (amount < buffer.size()) {
          std::vector<unsigned char> zeroes(buffer.size() - amount);
          if (!writeBytes(output, zeroes.data(), zeroes.size(), result.error)) {
            return result;
          }
        }
      }
    } else if (!writeBytes(output, buffer.data(), amount, result.error)) {
      return result;
    }
    offset += amount;
    result.bytesCaptured = offset;
    report(onProgress, MediaCaptureStage::Capturing, offset,
           source.sizeBytes());
  }

  report(onProgress, MediaCaptureStage::Finalizing, result.bytesCaptured,
         source.sizeBytes());
  if (dynamicVhd || dynamicVhdx) {
    const auto end = output.tellp();
    output.seekp(static_cast<std::streamoff>(tableOffset), std::ios::beg);
    if (!output || !writeBytes(output, table.data(), table.size(), result.error)) {
      return result;
    }
    output.seekp(end);
    if (!output) {
      result.error = dynamicVhd
                         ? "Unable to seek to the end of the dynamic VHD"
                         : "Unable to seek to the end of the dynamic VHDX";
      return result;
    }
  }
  if (options.format == MediaCaptureFormat::FixedVhd || dynamicVhd) {
    if (!writeBytes(output, footer.data(), footer.size(), result.error)) {
      return result;
    }
  }
  output.flush();
  if (!output) {
    result.error = "Unable to flush the capture image";
    return result;
  }
  output.close();
  result.outputSizeBytes = std::filesystem::file_size(partial.path, fileError);
  if (fileError) {
    result.error = "Unable to inspect the completed capture image: " +
                   fileError.message();
    return result;
  }

  if (options.verify) {
    ImageInfo captured;
    captured.sizeBytes = result.outputSizeBytes;
    captured.expandedSizeBytes = source.sizeBytes();
    captured.containerPayloadSizeBytes = source.sizeBytes();
    captured.capabilities.rawWrite = true;
    if (options.format == MediaCaptureFormat::FixedVhd) {
      captured.format = ImageFormat::Vhd;
      captured.containerPayloadLayout = ContainerPayloadLayout::Contiguous;
    } else if (dynamicVhd) {
      captured.format = ImageFormat::Vhd;
      captured.containerPayloadLayout = ContainerPayloadLayout::DynamicVhd;
      captured.containerAllocationTableOffsetBytes = tableOffset;
      captured.containerBlockSizeBytes = kDynamicBlockBytes;
      captured.containerLogicalSectorSize = kSectorBytes;
    } else if (dynamicVhdx) {
      captured.format = ImageFormat::Vhdx;
      captured.containerPayloadLayout = ContainerPayloadLayout::DynamicVhdx;
      captured.containerAllocationTableOffsetBytes = tableOffset;
      captured.containerBlockSizeBytes = kDynamicBlockBytes;
      captured.containerLogicalSectorSize = kSectorBytes;
    } else {
      captured.format = ImageFormat::Raw;
    }
    if (!verifyCapture(source, partial.path, captured, options.transferBytes,
                       onProgress, isCancelled, result)) {
      return result;
    }
  }

  std::filesystem::rename(partial.path, destination, fileError);
  if (fileError) {
    result.error = "Unable to commit the capture image: " + fileError.message();
    return result;
  }
  partial.committed = true;
  result.success = true;
  report(onProgress, MediaCaptureStage::Complete, source.sizeBytes(),
         source.sizeBytes());
  return result;
}

const char* mediaCaptureFormatName(const MediaCaptureFormat format) noexcept {
  switch (format) {
    case MediaCaptureFormat::Raw:
      return "DD/raw image";
    case MediaCaptureFormat::FixedVhd:
      return "Fixed VHD";
    case MediaCaptureFormat::DynamicVhd:
      return "Dynamic VHD";
    case MediaCaptureFormat::DynamicVhdx:
      return "Dynamic VHDX";
    case MediaCaptureFormat::Ffu:
      return "FFU";
    case MediaCaptureFormat::UdfIso:
      return "UDF ISO";
  }
  return "Media image";
}

const char* mediaCaptureStageName(const MediaCaptureStage stage) noexcept {
  switch (stage) {
    case MediaCaptureStage::Capturing:
      return "Capturing device";
    case MediaCaptureStage::Finalizing:
      return "Finalizing image";
    case MediaCaptureStage::Verifying:
      return "Verifying capture";
    case MediaCaptureStage::Complete:
      return "Capture complete";
  }
  return "Media capture";
}

}  // namespace rufus::core
