/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace rufus::backend::detail {
// Only private, unmounted ordinary staging files are accepted. Never a device.
bool installWindowsNtfsBootstrap(const std::filesystem::path& path,
                                std::uint64_t partitionOffset,
                                std::uint64_t partitionBytes,
                                std::string& error);
}
