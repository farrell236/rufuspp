/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "native_raw_io.hpp"

#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/disk.h>
#else
#include <linux/fs.h>
#endif
#endif

namespace rufus::backend::detail {

#if defined(_WIN32)
using NativeHandle = HANDLE;
#else
using NativeHandle = int;
#endif

// Borrows an already-authorized handle. Discovery, eligibility, exclusive access
// and handle identity checks stay in the platform backend before construction.
class SystemNativeIo final : public NativeIo {
 public:
  explicit SystemNativeIo(const NativeHandle handle) : handle_(handle) {}
  std::size_t maximumTransfer() const noexcept override {
#if defined(_WIN32)
    return std::numeric_limits<DWORD>::max();
#else
    return static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
#endif
  }
  std::uint64_t maximumOffset() const noexcept override {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
#else
    return static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
#endif
  }
  NativeIoResult read(const std::uint64_t offset, unsigned char* data,
                       const std::size_t size) override {
#if defined(_WIN32)
    auto seek = seekTo(offset);
    if (!seek.success) return seek;
    DWORD count = 0;
    if (!ReadFile(handle_, data, static_cast<DWORD>(size), &count, nullptr)) {
      return failure("Raw read failed", GetLastError());
    }
    return {count, true, false, {}};
#else
    const auto count = pread(handle_, data, size, static_cast<off_t>(offset));
    if (count < 0) return failure("Raw read failed", errno);
    return {static_cast<std::size_t>(count), true, false, {}};
#endif
  }
  NativeIoResult write(const std::uint64_t offset, const unsigned char* data,
                        const std::size_t size) override {
#if defined(_WIN32)
    auto seek = seekTo(offset);
    if (!seek.success) return seek;
    DWORD count = 0;
    if (!WriteFile(handle_, data, static_cast<DWORD>(size), &count, nullptr)) {
      return failure("Raw write failed", GetLastError());
    }
    return {count, true, false, {}};
#else
    const auto count = pwrite(handle_, data, size, static_cast<off_t>(offset));
    if (count < 0) return failure("Raw write failed", errno);
    return {static_cast<std::size_t>(count), true, false, {}};
#endif
  }
  NativeIoResult syncFile() override {
#if defined(_WIN32)
    if (!FlushFileBuffers(handle_)) return failure("Unable to flush target", GetLastError());
#else
    if (fsync(handle_) != 0) return failure("Unable to flush target", errno);
#endif
    return {0, true, false, {}};
  }
  NativeIoResult syncDevice() override {
#if defined(__APPLE__)
    if (ioctl(handle_, DKIOCSYNCHRONIZECACHE) != 0) {
      return failure("Unable to synchronize device cache", errno);
    }
#elif !defined(_WIN32)
    if (ioctl(handle_, BLKFLSBUF, 0) != 0) {
      return failure("Unable to invalidate target block cache", errno);
    }
#endif
    // Windows FlushFileBuffers performs the native flush in syncFile().
    return {0, true, false, {}};
  }

 private:
#if defined(_WIN32)
  static NativeIoResult failure(const char* action, const DWORD code) {
    return {0, false, false,
            std::string(action) + ": " + std::system_category().message(static_cast<int>(code))};
  }
  NativeIoResult seekTo(const std::uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
      return failure("Unable to seek in raw source or target", GetLastError());
    }
    return {0, true, false, {}};
  }
#else
  static NativeIoResult failure(const char* action, const int code) {
    return {0, false, code == EINTR,
            std::string(action) + ": " + std::generic_category().message(code)};
  }
#endif
  NativeHandle handle_;
};

class SystemRawTarget final : public core::RawTargetIo {
 public:
  SystemRawTarget(const NativeHandle handle, const std::uint64_t capacity,
                   const std::uint32_t sectorSize)
      : io_(handle), target_(io_, capacity, sectorSize) {}
  SystemRawTarget(const SystemRawTarget&) = delete;
  SystemRawTarget& operator=(const SystemRawTarget&) = delete;
  std::uint64_t capacityBytes() const noexcept override { return target_.capacityBytes(); }
  std::uint32_t logicalSectorSize() const noexcept override { return target_.logicalSectorSize(); }
  bool readAt(const std::uint64_t offset, unsigned char* data, const std::size_t size,
              std::string& error) override { return target_.readAt(offset, data, size, error); }
  bool writeAt(const std::uint64_t offset, const unsigned char* data, const std::size_t size,
               std::string& error) override { return target_.writeAt(offset, data, size, error); }
  bool flush(std::string& error) override { return target_.flush(error); }

 private:
  SystemNativeIo io_;
  NativeRawTarget target_;
};

inline bool positionalRead(const NativeHandle handle, const std::uint64_t offset,
                           unsigned char* data, const std::size_t size, std::string& error) {
  SystemNativeIo io(handle);
  return nativeTransfer(io, false, offset, data, size, error);
}

}  // namespace rufus::backend::detail
