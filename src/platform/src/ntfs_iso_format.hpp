/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rufus::backend::detail {

inline constexpr std::uint64_t kNtfsIsoSectorSize = 512;
inline constexpr std::uint64_t kNtfsIsoFirstSector = 2048;

inline std::vector<std::string> ntfsIsoFormatCommand(
    const std::string& partitionFile, const bool quickFormat,
    const std::uint32_t clusterSizeBytes, const std::string& volumeLabel,
    const std::string& formatter = "rufus-mkntfs") {
  // Format a private ordinary partition file, with geometry matching its final
  // disk position. No host mount, loop device or filesystem extension is used.
  std::vector<std::string> command{
      formatter, "-F", "-s", std::to_string(kNtfsIsoSectorSize),
      "-p", std::to_string(kNtfsIsoFirstSector), "-S", "63", "-H", "255"};
  if (quickFormat) {
    command.push_back("-Q");
  }
  if (clusterSizeBytes != 0U) {
    command.insert(command.end(), {"-c", std::to_string(clusterSizeBytes)});
  }
  command.insert(command.end(),
                 {"-L", volumeLabel, partitionFile});
  return command;
}

}  // namespace rufus::backend::detail
