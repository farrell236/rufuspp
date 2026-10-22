/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/write_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <system_error>

namespace rufus::core {

namespace {

constexpr std::size_t kMinimumBlockSize = 4096;
constexpr std::size_t kMaximumBlockSize = 64U * 1024U * 1024U;
constexpr std::uint64_t kTargetTailMetadataBytes = 1024ULL * 1024ULL;

void appendIssues(std::vector<SafetyIssue>& destination, SafetyCheckResult source) {
  destination.insert(destination.end(), std::make_move_iterator(source.issues.begin()),
                     std::make_move_iterator(source.issues.end()));
}

}  // namespace

RawWritePlanResult WritePlanBuilder::buildRawWrite(const ImageInfo& image,
                                                   const BlockDeviceInfo& target,
                                                   const std::size_t blockSize,
                                                   const bool verify,
                                                   const bool clearTargetTailMetadata) const {
  return buildRawWriteWithVerification(
      image, target,
      verify ? VerificationProfile::Standard : VerificationProfile::None,
      blockSize, clearTargetTailMetadata);
}

RawWritePlanResult WritePlanBuilder::buildRawWriteWithVerification(
    const ImageInfo& image, const BlockDeviceInfo& target,
    const VerificationProfile verificationProfile,
    const std::size_t blockSize,
    const bool clearTargetTailMetadata,
    std::vector<RawWriteRange> writeRanges) const {
  RawWritePlanResult result;
  const SafetyPolicy policy;
  appendIssues(result.issues, policy.validateWrite(image, target));

  std::uint64_t transferBytes = image.deploymentSizeBytes();
  if (!writeRanges.empty()) {
    bool valid = image.format == ImageFormat::Raw && !image.compressed &&
                 image.containerPayloadLayout == ContainerPayloadLayout::None &&
                 image.containerPayloadOffsetBytes == 0U &&
                 image.containerPayloadSizeBytes == 0U &&
                 target.logicalSectorSize != 0U &&
                 writeRanges.size() <= kMaximumRawWriteRanges &&
                 !clearTargetTailMetadata;
    transferBytes = 0U;
    std::uint64_t previousEnd = 0U;
    for (const auto& range : writeRanges) {
      if (!valid || range.sizeBytes == 0U || range.offsetBytes < previousEnd ||
          range.offsetBytes > image.deploymentSizeBytes() ||
          range.sizeBytes > image.deploymentSizeBytes() - range.offsetBytes ||
          range.offsetBytes % target.logicalSectorSize != 0U ||
          range.sizeBytes % target.logicalSectorSize != 0U) {
        valid = false;
        break;
      }
      previousEnd = range.offsetBytes + range.sizeBytes;
      // Ordered, disjoint ranges bounded by the source cannot overflow their sum.
      transferBytes += range.sizeBytes;
    }
    if (!valid) {
      result.issues.push_back({SafetyIssueCode::ModeUnsupported,
          "Initialized write ranges must be ordered, disjoint, sector-aligned and within an ordinary raw image"});
    }
  } else {
    writeRanges.push_back({0U, image.deploymentSizeBytes()});
  }

  if (!image.capabilities.rawWrite) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported, "The selected image does not support DD/raw mode"});
  }
  if (target.logicalSectorSize == 0) {
    result.issues.push_back(
        {SafetyIssueCode::InvalidSectorSize, "The target logical sector size is unknown"});
  } else {
    const std::uint64_t deploymentBytes = image.deploymentSizeBytes();
    if (deploymentBytes % target.logicalSectorSize != 0) {
      result.issues.push_back(
          {SafetyIssueCode::ImageNotSectorAligned,
           "The image size is not aligned to the target logical sector size"});
    }
    if (blockSize % target.logicalSectorSize != 0) {
      result.issues.push_back(
          {SafetyIssueCode::InvalidBlockSize,
           "The transfer block size is not aligned to the target logical sector size"});
    }
  }
  if (blockSize < kMinimumBlockSize || blockSize > kMaximumBlockSize) {
    result.issues.push_back(
        {SafetyIssueCode::InvalidBlockSize, "Transfer blocks must be between 4 KiB and 64 MiB"});
  }
  if (verificationProfile == VerificationProfile::Full &&
      (image.deploymentSizeBytes() != target.capacityBytes ||
       transferBytes != target.capacityBytes)) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "Full-device verification requires an image or full-format stage that covers the complete target"});
  }
  if (clearTargetTailMetadata && target.logicalSectorSize != 0) {
    const std::uint64_t tailBytes = std::min<std::uint64_t>(
        kTargetTailMetadataBytes, target.capacityBytes);
    const std::uint64_t alignedTailBytes =
        tailBytes - tailBytes % target.logicalSectorSize;
    if (target.capacityBytes % target.logicalSectorSize != 0 || alignedTailBytes == 0 ||
        image.deploymentSizeBytes() >
            target.capacityBytes - alignedTailBytes) {
      result.issues.push_back(
          {SafetyIssueCode::DeviceTooSmall,
           "The target lacks reserved trailing space for stale partition-metadata removal"});
    }
  }

  std::error_code error;
  const std::filesystem::path sourcePath = std::filesystem::u8path(image.path);
  const auto sourceSize = std::filesystem::file_size(sourcePath, error);
  if (error || sourceSize != image.sizeBytes) {
    result.issues.push_back(
        {SafetyIssueCode::SourceChanged,
         error ? "The source image is unavailable: " + error.message()
               : "The source image size changed after analysis"});
  }
  error.clear();
  const auto lastWriteTime = std::filesystem::last_write_time(sourcePath, error);
  if (error) {
    result.issues.push_back(
        {SafetyIssueCode::SourceChanged,
         "The source image timestamp is unavailable: " + error.message()});
  }

  if (result.issues.empty()) {
    result.plan = RawWritePlan(image, target, blockSize, verificationProfile,
                               clearTargetTailMetadata, lastWriteTime,
                               std::move(writeRanges), transferBytes);
  }
  return result;
}

const char* verificationProfileName(
    const VerificationProfile profile) noexcept {
  switch (profile) {
    case VerificationProfile::None:
      return "None";
    case VerificationProfile::Fast:
      return "Fast sampled verification";
    case VerificationProfile::Standard:
      return "Standard full-write verification";
    case VerificationProfile::Full:
      return "Full-device verification";
  }
  return "Unknown verification profile";
}

SafetyCheckResult WritePlanBuilder::validateSource(const RawWritePlan& plan) {
  SafetyCheckResult result;
  std::error_code error;
  const std::filesystem::path path = std::filesystem::u8path(plan.image().path);
  const auto sourceSize = std::filesystem::file_size(path, error);
  if (error || sourceSize != plan.image().sizeBytes) {
    result.issues.push_back(
        {SafetyIssueCode::SourceChanged,
         error ? "The source image is unavailable: " + error.message()
               : "The source image size changed after the write was planned"});
    return result;
  }

  const auto lastWriteTime = std::filesystem::last_write_time(path, error);
  if (error || lastWriteTime != plan.sourceLastWriteTime()) {
    result.issues.push_back(
        {SafetyIssueCode::SourceChanged,
         error ? "The source image timestamp is unavailable: " + error.message()
               : "The source image changed after the write was planned"});
  }
  return result;
}

}  // namespace rufus::core
