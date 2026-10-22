/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>

namespace rufus::core {

// Same source and destination offsets in a generated, uncompressed raw image.
// These are authored by the formatter, never inferred from zero-valued data.
struct RawWriteRange final {
  std::uint64_t offsetBytes{};
  std::uint64_t sizeBytes{};
};

inline constexpr std::size_t kMaximumRawWriteRanges = 65536U;

}  // namespace rufus::core
