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
#include <functional>
#include <memory>
#include <string>

#include "rufus/core/write_plan.hpp"

namespace rufus::core {

enum class RawWriteStage {
  Revalidating,
  Claiming,
  Unmounting,
  Opening,
  Writing,
  ClearingTargetMetadata,
  Flushing,
  Verifying,
  VerifyingTargetMetadata,
  Complete,
};

struct RawWriteProgress final {
  RawWriteStage stage{RawWriteStage::Revalidating};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
};

struct RawWriteResult final {
  bool success{};
  bool cancelled{};
  // A failed write may have modified part of a block. True once any target
  // write is attempted; bytesWritten counts only complete image-block writes.
  bool destructiveWriteStarted{};
  std::uint64_t bytesWritten{};
  std::uint64_t bytesVerified{};
  bool verificationCompleted{};
  std::string error;
};

using RawWriteProgressCallback = std::function<void(const RawWriteProgress&)>;
using RawWriteCancelCallback = std::function<bool()>;

class RawSourceIo {
 public:
  virtual ~RawSourceIo() = default;

  [[nodiscard]] virtual std::uint64_t sizeBytes() const noexcept = 0;
  virtual bool readAt(std::uint64_t offset, unsigned char* data, std::size_t size,
                      std::string& error) = 0;
};

class RawTargetIo {
 public:
  virtual ~RawTargetIo() = default;

  [[nodiscard]] virtual std::uint64_t capacityBytes() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t logicalSectorSize() const noexcept = 0;
  virtual bool writeAt(std::uint64_t offset, const unsigned char* data, std::size_t size,
                       std::string& error) = 0;
  virtual bool readAt(std::uint64_t offset, unsigned char* data, std::size_t size,
                      std::string& error) = 0;
  virtual bool flush(std::string& error) = 0;
};

struct RawSourceOpenResult final {
  std::unique_ptr<RawSourceIo> source;
  std::string error;

  [[nodiscard]] bool succeeded() const noexcept { return source != nullptr; }
};

// Opens an ordinary or compressed image as a random-access-shaped source for
// the raw writer. Compressed sources support the writer's sequential write pass
// and rewind once for verification; arbitrary seeking is rejected.
[[nodiscard]] RawSourceOpenResult openRawImageSource(
    const ImageInfo& image,
    const std::filesystem::path& pathOverride = {});

class RawImageWriter final {
 public:
  [[nodiscard]] RawWriteResult write(const RawWritePlan& plan, RawTargetIo& target,
                                     const RawWriteProgressCallback& onProgress = {},
                                     const RawWriteCancelCallback& isCancelled = {}) const;
  [[nodiscard]] RawWriteResult write(const RawWritePlan& plan, RawSourceIo& source,
                                     RawTargetIo& target,
                                     const RawWriteProgressCallback& onProgress = {},
                                     const RawWriteCancelCallback& isCancelled = {}) const;
};

[[nodiscard]] const char* rawWriteStageName(RawWriteStage stage) noexcept;

}  // namespace rufus::core
