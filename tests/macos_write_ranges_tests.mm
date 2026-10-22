/* SPDX-License-Identifier: GPL-3.0-or-later */
// XPC serialization only: no service, privileges, filesystem or device access.
#include <iostream>
#include <stdexcept>
#include "macos_write_ranges.hpp"

namespace wire = rufus::backend::macos::protocol;
using rufus::core::RawWriteRange;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    std::vector<RawWriteRange> observed;
    std::string error;
    xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
    require(!wire::decodeWriteRanges(request, observed, error), "missing array must fail closed");
    xpc_dictionary_set_string(request, wire::kWriteRanges, "invalid");
    require(!wire::decodeWriteRanges(request, observed, error), "non-array must fail closed");
    wire::encodeWriteRanges(request, {});
    require(wire::decodeWriteRanges(request, observed, error) && observed.empty(),
            "dense writes must round-trip as an explicit empty array");
    const std::vector<RawWriteRange> expected{{0U, 4096U}, {8ULL * 1024U * 1024U * 1024U, 8192U}};
    wire::encodeWriteRanges(request, expected);
    require(wire::decodeWriteRanges(request, observed, error) && observed.size() == 2U &&
                observed[1].offsetBytes == expected[1].offsetBytes && observed[1].sizeBytes == 8192U,
            "range offsets must survive beyond 32-bit limits");
    xpc_object_t array = xpc_dictionary_get_value(request, wire::kWriteRanges);
    xpc_object_t item = xpc_array_get_value(array, 0U);
    xpc_dictionary_set_int64(item, wire::kRangeOffset, -1);
    require(!wire::decodeWriteRanges(request, observed, error) && observed.empty(),
            "signed offsets must fail, not silently become zero");
    xpc_dictionary_set_uint64(item, wire::kRangeOffset, 0U);
    xpc_dictionary_set_string(item, wire::kRangeSize, "4096");
    require(!wire::decodeWriteRanges(request, observed, error), "string sizes must fail");
    xpc_array_set_string(array, 0U, "invalid item");
    require(!wire::decodeWriteRanges(request, observed, error), "non-dictionary entries must fail");
    xpc_object_t excessive = xpc_array_create(nullptr, 0U);
    for (std::size_t index = 0U; index <= rufus::core::kMaximumRawWriteRanges; ++index) {
      xpc_array_append_value(excessive, item);
    }
    xpc_dictionary_set_value(request, wire::kWriteRanges, excessive);
    require(!wire::decodeWriteRanges(request, observed, error), "excessive arrays must fail");
    std::cout << "PASS typed XPC range transport and rejection guards\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
