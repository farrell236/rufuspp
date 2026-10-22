/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include "rufus/core/media.hpp"
#include "rufus/core/safety_policy.hpp"
#include "rufus/core/write_range.hpp"

namespace rufus::core {

enum class VerificationProfile {
  None,
  Fast,
  Standard,
  Full,
};

[[nodiscard]] const char* verificationProfileName(
    VerificationProfile profile) noexcept;

class RawWritePlan final {
 public:
  [[nodiscard]] const ImageInfo& image() const noexcept { return image_; }
  [[nodiscard]] const BlockDeviceInfo& target() const noexcept { return target_; }
  // Transfer count, which may be smaller than the source's logical size.
  [[nodiscard]] std::uint64_t bytesToWrite() const noexcept {
    return bytesToWrite_;
  }
  [[nodiscard]] const std::vector<RawWriteRange>& writeRanges() const noexcept {
    return writeRanges_;
  }
  [[nodiscard]] bool writesWholeImage() const noexcept {
    return bytesToWrite_ == image_.deploymentSizeBytes();
  }
  [[nodiscard]] std::size_t blockSize() const noexcept { return blockSize_; }
  [[nodiscard]] bool verify() const noexcept {
    return verificationProfile_ != VerificationProfile::None;
  }
  [[nodiscard]] VerificationProfile verificationProfile() const noexcept {
    return verificationProfile_;
  }
  [[nodiscard]] bool clearTargetTailMetadata() const noexcept {
    return clearTargetTailMetadata_;
  }
  [[nodiscard]] std::filesystem::file_time_type sourceLastWriteTime() const noexcept {
    return sourceLastWriteTime_;
  }

 private:
  friend class WritePlanBuilder;

  RawWritePlan(ImageInfo image, BlockDeviceInfo target, std::size_t blockSize,
               VerificationProfile verificationProfile,
               bool clearTargetTailMetadata,
               std::filesystem::file_time_type sourceLastWriteTime,
               std::vector<RawWriteRange> writeRanges,
               std::uint64_t bytesToWrite)
      : image_(std::move(image)),
        target_(std::move(target)),
        blockSize_(blockSize),
        verificationProfile_(verificationProfile),
        clearTargetTailMetadata_(clearTargetTailMetadata),
        sourceLastWriteTime_(sourceLastWriteTime),
        writeRanges_(std::move(writeRanges)), bytesToWrite_(bytesToWrite) {}

  ImageInfo image_;
  BlockDeviceInfo target_;
  std::size_t blockSize_{};
  VerificationProfile verificationProfile_{VerificationProfile::Standard};
  bool clearTargetTailMetadata_{};
  std::filesystem::file_time_type sourceLastWriteTime_{};
  std::vector<RawWriteRange> writeRanges_;
  std::uint64_t bytesToWrite_{};
};

struct RawWritePlanResult final {
  std::optional<RawWritePlan> plan;
  std::vector<SafetyIssue> issues;

  [[nodiscard]] bool succeeded() const noexcept { return plan.has_value(); }
};

class WritePlanBuilder final {
 public:
  [[nodiscard]] RawWritePlanResult buildRawWrite(
      const ImageInfo& image, const BlockDeviceInfo& target,
      std::size_t blockSize = 4U * 1024U * 1024U, bool verify = true,
      bool clearTargetTailMetadata = false) const;

  [[nodiscard]] RawWritePlanResult buildRawWriteWithVerification(
      const ImageInfo& image, const BlockDeviceInfo& target,
      VerificationProfile verificationProfile,
      std::size_t blockSize = 4U * 1024U * 1024U,
      bool clearTargetTailMetadata = false,
      // Only formatter-authored, ordinary raw images may omit unused ranges.
      std::vector<RawWriteRange> writeRanges = {}) const;

  [[nodiscard]] static SafetyCheckResult validateSource(const RawWritePlan& plan);
};

}  // namespace rufus::core
