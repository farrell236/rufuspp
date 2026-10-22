/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/safety_policy.hpp"

#include <filesystem>
#include <system_error>

namespace rufus::core {

namespace {

bool pathsReferToSameFile(const std::string& left, const std::string& right) {
  if (left.empty() || right.empty()) {
    return false;
  }

  std::error_code error;
  const auto leftPath = std::filesystem::u8path(left);
  const auto rightPath = std::filesystem::u8path(right);
  const bool equivalent = std::filesystem::equivalent(leftPath, rightPath, error);
  if (!error) {
    return equivalent;
  }

  error.clear();
  const auto normalizedLeft = std::filesystem::weakly_canonical(leftPath, error);
  if (error) {
    return left == right;
  }
  const auto normalizedRight = std::filesystem::weakly_canonical(rightPath, error);
  return error ? left == right : normalizedLeft == normalizedRight;
}

}  // namespace

SafetyCheckResult SafetyPolicy::validateWrite(const ImageInfo& image,
                                              const BlockDeviceInfo& target) const {
  SafetyCheckResult result;

  if (image.path.empty()) {
    result.issues.push_back({SafetyIssueCode::ImageMissing, "No source image was selected"});
  }
  if (image.sizeBytes == 0) {
    result.issues.push_back({SafetyIssueCode::ImageEmpty, "The source image is empty"});
  }
  if (image.format == ImageFormat::Unknown) {
    result.issues.push_back(
        {SafetyIssueCode::ImageUnsupported, "The source image format is not recognized"});
  }
  if (image.compressed && !image.capabilities.compressedSizeKnown) {
    result.issues.push_back({SafetyIssueCode::ExpandedSizeUnknown,
                             "The expanded size of the compressed image is unknown"});
  }
  if (image.compressed && !image.capabilities.validContainerMetadata) {
    result.issues.push_back(
        {SafetyIssueCode::ImageUnsupported,
         "The compressed image did not pass container and checksum validation"});
  }

  const DeviceEligibility eligibility = evaluateDeviceEligibility(target);
  if (eligibility != DeviceEligibility::Eligible) {
    result.issues.push_back(
        {SafetyIssueCode::DeviceIneligible,
         "Target is not eligible: " + std::string(deviceEligibilityName(eligibility))});
  }
  const std::uint64_t requiredBytes = image.deploymentSizeBytes();
  if (requiredBytes > target.capacityBytes) {
    result.issues.push_back(
        {SafetyIssueCode::DeviceTooSmall, "The target device is smaller than the source image"});
  }
  if (pathsReferToSameFile(image.path, target.devicePath)) {
    result.issues.push_back(
        {SafetyIssueCode::SourceIsTarget, "The source image and target device are the same path"});
  }

  return result;
}

SafetyCheckResult SafetyPolicy::validateIdentity(const BlockDeviceInfo& selected,
                                                 const BlockDeviceInfo& observed) const {
  SafetyCheckResult result;
  // Losing an identity field that was known at selection is also a mismatch.
  // Do not accept a replacement observation with weaker identity evidence.
  const bool serialChanged = !selected.serialNumber.empty() &&
                             selected.serialNumber != observed.serialNumber;
  if (selected.stableId.empty() || selected.stableId != observed.stableId ||
      selected.devicePath != observed.devicePath || selected.capacityBytes != observed.capacityBytes ||
      selected.logicalSectorSize != observed.logicalSectorSize || serialChanged) {
    result.issues.push_back(
        {SafetyIssueCode::IdentityChanged,
         "The target device identity changed after it was selected"});
  }
  return result;
}

}  // namespace rufus::core
