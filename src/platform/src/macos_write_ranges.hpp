/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <string>
#include <vector>
#include <xpc/xpc.h>

#include "macos_privileged_helper_protocol.hpp"
#include "rufus/core/write_range.hpp"

namespace rufus::backend::macos::protocol {

inline void encodeWriteRanges(xpc_object_t request,
                               const std::vector<core::RawWriteRange>& ranges) {
  xpc_object_t array = xpc_array_create(nullptr, 0U);
  for (const auto& range : ranges) {
    xpc_object_t item = xpc_dictionary_create(nullptr, nullptr, 0U);
    xpc_dictionary_set_uint64(item, kRangeOffset, range.offsetBytes);
    xpc_dictionary_set_uint64(item, kRangeSize, range.sizeBytes);
    xpc_array_append_value(array, item);
  }
  xpc_dictionary_set_value(request, kWriteRanges, array);
}

inline bool decodeWriteRanges(xpc_object_t request,
                               std::vector<core::RawWriteRange>& ranges,
                               std::string& error) {
  ranges.clear();
  const auto array = xpc_dictionary_get_value(request, kWriteRanges);
  if (array == nullptr || xpc_get_type(array) != XPC_TYPE_ARRAY ||
      xpc_array_get_count(array) > core::kMaximumRawWriteRanges) {
    error = "The write request contains a missing, invalid or excessive range array";
    return false;
  }
  for (std::size_t index = 0U; index < xpc_array_get_count(array); ++index) {
    const auto item = xpc_array_get_value(array, index);
    const auto offset = xpc_get_type(item) == XPC_TYPE_DICTIONARY
                            ? xpc_dictionary_get_value(item, kRangeOffset) : nullptr;
    const auto size = xpc_get_type(item) == XPC_TYPE_DICTIONARY
                          ? xpc_dictionary_get_value(item, kRangeSize) : nullptr;
    if (offset == nullptr || size == nullptr ||
        xpc_get_type(offset) != XPC_TYPE_UINT64 || xpc_get_type(size) != XPC_TYPE_UINT64) {
      error = "Initialized write ranges require unsigned offset and size fields";
      ranges.clear();
      return false;
    }
    ranges.push_back({xpc_uint64_get_value(offset), xpc_uint64_get_value(size)});
  }
  // The shared planner subsequently validates alignment, ordering and bounds.
  return true;
}

}  // namespace rufus::backend::macos::protocol
