/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "rufus/core/raw_image_writer.hpp"

namespace rufus::backend::detail {

// One system call, rather than one whole transfer. This seam lets tests inject
// short I/O and EINTR into the exact retry loop used by the native backends.
struct NativeIoResult final {
  std::size_t bytes{};
  bool success{};
  bool interrupted{};
  std::string error;
};

class NativeIo {
 public:
  virtual ~NativeIo() = default;
  virtual NativeIoResult read(std::uint64_t offset, unsigned char* data,
                              std::size_t size) = 0;
  virtual NativeIoResult write(std::uint64_t offset, const unsigned char* data,
                               std::size_t size) = 0;
  virtual NativeIoResult syncFile() = 0;
  virtual NativeIoResult syncDevice() = 0;
  virtual std::size_t maximumTransfer() const noexcept = 0;
  virtual std::uint64_t maximumOffset() const noexcept = 0;
};

inline bool nativeTransfer(NativeIo& io, const bool writing,
                            const std::uint64_t offset, unsigned char* data,
                            const std::size_t size, std::string& error) {
  if (offset > io.maximumOffset() || size > io.maximumOffset() - offset ||
      io.maximumTransfer() == 0U) {
    error = "Raw transfer exceeds the platform offset limit";
    return false;
  }
  std::size_t completed = 0U;
  while (completed < size) {
    const auto requested = std::min(size - completed, io.maximumTransfer());
    const auto result = writing ? io.write(offset + completed, data + completed, requested)
                                : io.read(offset + completed, data + completed, requested);
    if (!result.success) {
      if (result.interrupted) continue;
      error = result.error.empty() ? "Native raw I/O failed" : result.error;
      return false;
    }
    if (result.bytes == 0U || result.bytes > requested) {
      error = writing ? "The raw target returned an invalid write length"
                      : "Unexpected end or invalid length from the raw source or target";
      return false;
    }
    completed += result.bytes;
  }
  return true;
}

class NativeRawTarget final : public core::RawTargetIo {
 public:
  NativeRawTarget(NativeIo& io, const std::uint64_t capacity,
                   const std::uint32_t sectorSize)
      : io_(io), capacity_(capacity), sectorSize_(sectorSize) {}
  std::uint64_t capacityBytes() const noexcept override { return capacity_; }
  std::uint32_t logicalSectorSize() const noexcept override { return sectorSize_; }
  bool writeAt(const std::uint64_t offset, const unsigned char* data,
               const std::size_t size, std::string& error) override {
    return inBounds(offset, size, error) &&
           nativeTransfer(io_, true, offset, const_cast<unsigned char*>(data), size, error);
  }
  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    return inBounds(offset, size, error) && nativeTransfer(io_, false, offset, data, size, error);
  }
  bool flush(std::string& error) override {
    for (const bool device : {false, true}) {
      NativeIoResult result;
      do {
        result = device ? io_.syncDevice() : io_.syncFile();
      } while (!result.success && result.interrupted);
      if (!result.success) {
        error = result.error.empty() ? "Unable to flush the native target" : result.error;
        return false;
      }
    }
    return true;
  }

 private:
  bool inBounds(const std::uint64_t offset, const std::size_t size, std::string& error) const {
    if (sectorSize_ == 0U || offset > capacity_ || size > capacity_ - offset ||
        offset % sectorSize_ != 0U || size % sectorSize_ != 0U) {
      error = "Raw-device transfer is outside the target or is not sector aligned";
      return false;
    }
    return true;
  }
  NativeIo& io_;
  std::uint64_t capacity_;
  std::uint32_t sectorSize_;
};

}  // namespace rufus::backend::detail
