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
#include <string>
#include <vector>

#include "rufus/core/media.hpp"
#include "rufus/core/write_plan.hpp"

namespace rufus::core {

enum class PreflightLevel {
  Information,
  Warning,
  Blocker,
};

struct PreflightEntry final {
  std::string label;
  std::string value;
  std::string detail;
  PreflightLevel level{PreflightLevel::Information};
};

struct DependencyStatus final {
  std::string name;
  bool required{};
  bool available{};
  std::string detail;
};

struct DeploymentPreflightInput final {
  std::string operation;
  ImageInfo image;
  BlockDeviceInfo target;
  std::string partitionScheme;
  std::string targetSystem;
  std::string fileSystem;
  std::uint32_t clusterSizeBytes{};
  bool quickFormat{};
  VerificationProfile verificationProfile{VerificationProfile::Standard};
  std::uint64_t bytesToWrite{};
  bool writeSizePending{};
  std::uint64_t temporaryBytes{};
  std::vector<std::string> transformations;
  std::vector<std::string> warnings;
  std::vector<std::string> blockers;
  std::vector<DependencyStatus> dependencies;
};

struct DeploymentPreflightReport final {
  std::string operation;
  std::vector<PreflightEntry> entries;

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::string toText() const;
  [[nodiscard]] std::string toJson() const;
};

[[nodiscard]] DeploymentPreflightReport buildDeploymentPreflight(
    const DeploymentPreflightInput& input);

struct DeploymentReceipt final {
  std::string application;
  std::string applicationVersion;
  std::string startedAtUtc;
  std::string finishedAtUtc;
  DeploymentPreflightReport preflight;
  bool success{};
  bool cancelled{};
  bool destructiveWriteStarted{};
  std::uint64_t bytesWritten{};
  std::uint64_t bytesVerified{};
  bool verificationCompleted{};
  std::string sourceSha256;
  std::string error;

  [[nodiscard]] std::string toJson() const;
};

struct ReceiptWriteResult final {
  bool success{};
  std::string error;
};

[[nodiscard]] ReceiptWriteResult writeDeploymentReceipt(
    const std::filesystem::path& destination,
    const DeploymentReceipt& receipt);

[[nodiscard]] const char* preflightLevelName(PreflightLevel level) noexcept;

}  // namespace rufus::core
