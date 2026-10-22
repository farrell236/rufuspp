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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "rufus/core/iso_deployment.hpp"

namespace rufus::backend {

struct NtfsIsoAvailability final {
  bool available{};
  std::string reason;
};

struct NtfsIsoStagingRequirements final {
  bool success{};
  std::uint64_t temporaryBytes{};
  bool writeSizePending{};
  std::string error;
};

// Shared by GUI preflight and the backend's free-space guard. Direct NTFS
// staging on macOS/Linux holds two files; Windows uses one native VHD and
// currently writes it densely, even for quick formats.
[[nodiscard]] NtfsIsoStagingRequirements estimateNtfsIsoStagingRequirements(
    std::uint64_t expandedImageBytes, std::uint64_t targetCapacityBytes,
    bool quickFormat);

class NtfsIsoImageStager {
 public:
  virtual ~NtfsIsoImageStager() = default;

  [[nodiscard]] virtual NtfsIsoAvailability availability() const = 0;
  [[nodiscard]] virtual core::IsoDeploymentResult stage(
      const core::IsoDeploymentPlan& plan,
      const std::filesystem::path& outputPath,
      const core::IsoDeploymentProgressCallback& onProgress = {},
      const core::IsoDeploymentCancelCallback& isCancelled = {}) const = 0;
};

[[nodiscard]] std::unique_ptr<NtfsIsoImageStager>
makePlatformNtfsIsoImageStager();

struct NtfsIsoLayoutResult final {
  bool success{};
  std::string error;
};

// Creates an MBR skeleton, with Windows MBR code for BIOS targets and the
// UEFI:NTFS helper for UEFI targets. Does not format the NTFS partition.
[[nodiscard]] NtfsIsoLayoutResult createNtfsIsoMbrImage(
    const std::filesystem::path& outputPath, std::uint64_t capacityBytes,
    core::IsoTargetSystem targetSystem = core::IsoTargetSystem::Uefi);

// GPT supports UEFI only; MBR also supports BIOS and BIOS + UEFI.
[[nodiscard]] NtfsIsoLayoutResult createNtfsIsoDiskImage(
    const std::filesystem::path& outputPath, std::uint64_t capacityBytes,
    core::PartitionScheme partitionScheme,
    core::IsoTargetSystem targetSystem = core::IsoTargetSystem::Uefi);

}  // namespace rufus::backend
