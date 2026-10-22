/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/raw_image_writer.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace rufus::core {

namespace {

constexpr std::uint64_t kTargetTailMetadataBytes = 1024ULL * 1024ULL;

void report(const RawWriteProgressCallback& callback, const RawWriteStage stage,
            const std::uint64_t processed, const std::uint64_t total) {
  if (callback) {
    callback({stage, processed, total});
  }
}

bool cancelled(const RawWriteCancelCallback& callback) {
  return callback && callback();
}

bool shouldVerifyBlock(const VerificationProfile profile,
                       const std::uint64_t blockIndex,
                       const std::uint64_t blockCount) {
  if (profile == VerificationProfile::Standard ||
      profile == VerificationProfile::Full) {
    return true;
  }
  if (profile != VerificationProfile::Fast || blockCount <= 16U) {
    return profile == VerificationProfile::Fast;
  }
  const std::uint64_t stride = std::max<std::uint64_t>(1U, blockCount / 14U);
  return blockIndex == 0U || blockIndex + 1U == blockCount ||
         blockIndex % stride == 0U;
}

RawWriteResult writeFromSource(const RawWritePlan& plan, RawSourceIo& source,
                               RawTargetIo& target,
                               const RawWriteProgressCallback& onProgress,
                               const RawWriteCancelCallback& isCancelled) {
  RawWriteResult result;
  report(onProgress, RawWriteStage::Revalidating, 0, plan.bytesToWrite());

  if (source.sizeBytes() != plan.image().deploymentSizeBytes()) {
    result.error = "The opened source size differs from the analyzed image";
    return result;
  }
  if (target.capacityBytes() != plan.target().capacityBytes ||
      target.logicalSectorSize() != plan.target().logicalSectorSize) {
    result.error = "The opened target geometry differs from the selected device";
    return result;
  }

  std::vector<unsigned char> buffer(plan.blockSize());
  for (const auto& range : plan.writeRanges()) {
    std::uint64_t withinRange = 0U;
    while (withinRange < range.sizeBytes) {
      if (cancelled(isCancelled)) {
        result.cancelled = true;
        result.error = result.destructiveWriteStarted
                           ? "Write cancelled after the target was partially overwritten"
                           : "Write cancelled before the target was modified";
        return result;
      }
      const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
          range.sizeBytes - withinRange, buffer.size()));
      const auto offset = range.offsetBytes + withinRange;
      if (!source.readAt(offset, buffer.data(), requested, result.error)) {
        if (result.error.empty()) {
          result.error = "The source image changed or could not be read completely";
        }
        return result;
      }
      // Native I/O may modify a prefix before reporting failure.
      result.destructiveWriteStarted = true;
      if (!target.writeAt(offset, buffer.data(), requested, result.error)) {
        if (result.error.empty()) result.error = "The target device rejected a write";
        return result;
      }
      withinRange += requested;
      result.bytesWritten += requested;
      report(onProgress, RawWriteStage::Writing, result.bytesWritten, plan.bytesToWrite());
    }
  }

  std::uint64_t tailBytes = 0;
  std::uint64_t tailOffset = target.capacityBytes();
  if (plan.clearTargetTailMetadata()) {
    tailBytes = std::min<std::uint64_t>(kTargetTailMetadataBytes, target.capacityBytes());
    tailBytes -= tailBytes % target.logicalSectorSize();
    tailOffset -= tailBytes;
    std::fill(buffer.begin(), buffer.end(), 0);
    std::uint64_t cleared = 0;
    report(onProgress, RawWriteStage::ClearingTargetMetadata, 0, tailBytes);
    while (cleared < tailBytes) {
      if (cancelled(isCancelled)) {
        result.cancelled = true;
        result.error = "Write cancelled after the target was partially overwritten";
        return result;
      }
      const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(
          tailBytes - cleared, static_cast<std::uint64_t>(buffer.size())));
      if (!target.writeAt(tailOffset + cleared, buffer.data(), requested, result.error)) {
        if (result.error.empty()) {
          result.error = "The target rejected trailing metadata removal";
        }
        return result;
      }
      result.destructiveWriteStarted = true;
      cleared += requested;
      report(onProgress, RawWriteStage::ClearingTargetMetadata, cleared, tailBytes);
    }
  }

  report(onProgress, RawWriteStage::Flushing, result.bytesWritten, plan.bytesToWrite());
  if (!target.flush(result.error)) {
    if (result.error.empty()) {
      result.error = "The target device could not flush its write cache";
    }
    return result;
  }

  if (plan.verify()) {
    std::vector<unsigned char> targetBuffer(plan.blockSize());
    std::uint64_t verified = 0;
    std::uint64_t blockIndex = 0;
    std::uint64_t blockCount = 0U;
    for (const auto& range : plan.writeRanges()) {
      blockCount += range.sizeBytes / plan.blockSize() +
                    (range.sizeBytes % plan.blockSize() != 0U ? 1U : 0U);
    }
    for (const auto& range : plan.writeRanges()) {
      std::uint64_t withinRange = 0U;
      while (withinRange < range.sizeBytes) {
        if (cancelled(isCancelled)) {
          result.cancelled = true;
          result.error = "Verification cancelled; the image was written but not fully verified";
          return result;
        }
        const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            range.sizeBytes - withinRange, buffer.size()));
        const auto offset = range.offsetBytes + withinRange;
        // Fast sampling must still include the boundaries of every metadata range.
        const bool verifyBlock = shouldVerifyBlock(
            plan.verificationProfile(), blockIndex, blockCount) ||
            (plan.verificationProfile() == VerificationProfile::Fast &&
             (withinRange == 0U || requested == range.sizeBytes - withinRange));
        if (!source.readAt(offset, buffer.data(), requested, result.error) ||
            (verifyBlock &&
             (!target.readAt(offset, targetBuffer.data(), requested, result.error) ||
              !std::equal(buffer.begin(),
                          buffer.begin() + static_cast<std::ptrdiff_t>(requested),
                          targetBuffer.begin())))) {
          if (result.error.empty()) result.error = "Target verification failed";
          return result;
        }
        if (verifyBlock) result.bytesVerified += requested;
        withinRange += requested;
        verified += requested;
        ++blockIndex;
        report(onProgress, RawWriteStage::Verifying, verified, plan.bytesToWrite());
      }
    }
    if (tailBytes != 0) {
      std::fill(buffer.begin(), buffer.end(), 0);
      std::uint64_t verifiedTail = 0;
      report(onProgress, RawWriteStage::VerifyingTargetMetadata, 0, tailBytes);
      while (verifiedTail < tailBytes) {
        if (cancelled(isCancelled)) {
          result.cancelled = true;
          result.error =
              "Verification cancelled; trailing partition metadata was not fully verified";
          return result;
        }
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            tailBytes - verifiedTail, static_cast<std::uint64_t>(buffer.size())));
        if (!target.readAt(tailOffset + verifiedTail, targetBuffer.data(), requested,
                           result.error) ||
            !std::equal(buffer.begin(),
                        buffer.begin() + static_cast<std::ptrdiff_t>(requested),
                        targetBuffer.begin())) {
          if (result.error.empty()) {
            result.error = "Trailing target metadata verification failed";
          }
          return result;
        }
        verifiedTail += requested;
        result.bytesVerified += requested;
        report(onProgress, RawWriteStage::VerifyingTargetMetadata, verifiedTail, tailBytes);
      }
    }
    result.verificationCompleted = true;
  }

  result.success = true;
  return result;
}

}  // namespace

RawWriteResult RawImageWriter::write(const RawWritePlan& plan, RawTargetIo& target,
                                     const RawWriteProgressCallback& onProgress,
                                     const RawWriteCancelCallback& isCancelled) const {
  const auto sourceSafety = WritePlanBuilder::validateSource(plan);
  if (!sourceSafety.safe()) {
    RawWriteResult result;
    result.error = sourceSafety.issues.front().message;
    return result;
  }
  auto opened = openRawImageSource(plan.image());
  if (!opened.succeeded()) {
    RawWriteResult result;
    result.error = opened.error.empty() ? "Unable to open the source image"
                                       : std::move(opened.error);
    return result;
  }
  RawWriteResult result = writeFromSource(plan, *opened.source, target, onProgress,
                                          isCancelled);

  const auto finalSourceSafety = WritePlanBuilder::validateSource(plan);
  if (result.success && !finalSourceSafety.safe()) {
    result.success = false;
    result.error = finalSourceSafety.issues.front().message;
  }
  if (result.success) {
    report(onProgress, RawWriteStage::Complete, plan.bytesToWrite(), plan.bytesToWrite());
  }
  return result;
}

RawWriteResult RawImageWriter::write(const RawWritePlan& plan, RawSourceIo& source,
                                     RawTargetIo& target,
                                     const RawWriteProgressCallback& onProgress,
                                     const RawWriteCancelCallback& isCancelled) const {
  auto result = writeFromSource(plan, source, target, onProgress, isCancelled);
  if (result.success) {
    report(onProgress, RawWriteStage::Complete, plan.bytesToWrite(), plan.bytesToWrite());
  }
  return result;
}

const char* rawWriteStageName(const RawWriteStage stage) noexcept {
  switch (stage) {
    case RawWriteStage::Revalidating:
      return "Revalidating";
    case RawWriteStage::Claiming:
      return "Claiming device";
    case RawWriteStage::Unmounting:
      return "Unmounting volumes";
    case RawWriteStage::Opening:
      return "Opening device";
    case RawWriteStage::Writing:
      return "Writing image";
    case RawWriteStage::ClearingTargetMetadata:
      return "Clearing old partition metadata";
    case RawWriteStage::Flushing:
      return "Flushing device";
    case RawWriteStage::Verifying:
      return "Verifying image";
    case RawWriteStage::VerifyingTargetMetadata:
      return "Verifying cleared metadata";
    case RawWriteStage::Complete:
      return "Complete";
  }
  return "Unknown";
}

}  // namespace rufus::core
