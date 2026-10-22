/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#include <setupapi.h>
#include <winioctl.h>

#include "rufus/backend/block_device_backend.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "system_raw_io.hpp"
#include "raw_write_safety.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rufus::backend {

namespace {

class WindowsBlockDeviceBackend final : public BlockDeviceBackend {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "Windows storage interfaces";
  }

  [[nodiscard]] BackendCapabilities capabilities() const noexcept override {
    BackendCapabilities result;
    result.physicalDeviceDiscovery = true;
    result.readOnlyInspection = true;
    result.unmountVolumes = true;
    result.exclusiveAccess = true;
    result.rawWrite = true;
    result.flush = true;
    result.identityRevalidation = true;
    result.rawVerification = true;
    result.ffuApply = true;
    result.mediaCapture = true;
    result.badBlockTest = true;
    return result;
  }

  [[nodiscard]] PrivilegeStatus privilegeStatus() const override;

  [[nodiscard]] DeviceDiscoveryResult discover() const override;
  [[nodiscard]] RawWriteAvailability rawWriteAvailability(
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] RawWriteAvailability requestRawWriteAuthorization() const override;
  [[nodiscard]] core::RawWriteResult writeRaw(
      const core::RawWritePlan& plan,
      const core::RawWriteProgressCallback& onProgress = {},
      const core::RawWriteCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] RawWriteAvailability ffuApplyAvailability(
      const core::ImageInfo& image,
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] core::RawWriteResult applyFfu(
      const core::ImageInfo& image, const core::BlockDeviceInfo& target,
      const core::RawWriteProgressCallback& onProgress = {},
      const core::RawWriteCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] RawWriteAvailability captureAvailability(
      const core::BlockDeviceInfo& target,
      core::MediaCaptureFormat format) const override;
  [[nodiscard]] core::MediaCaptureResult capture(
      const core::BlockDeviceInfo& target,
      const std::filesystem::path& destination,
      const core::MediaCaptureOptions& options,
      const core::MediaCaptureProgressCallback& onProgress = {},
      const core::MediaCaptureCancelCallback& isCancelled = {}) const override;
  [[nodiscard]] RawWriteAvailability badBlockTestAvailability(
      const core::BlockDeviceInfo& target) const override;
  [[nodiscard]] core::BadBlockTestResult testBadBlocks(
      const core::BlockDeviceInfo& target,
      const core::BadBlockTestOptions& options,
      const core::BadBlockTestProgressCallback& onProgress = {},
      const core::BadBlockTestCancelCallback& isCancelled = {}) const override;
};

class DeviceInfoSet final {
 public:
  explicit DeviceInfoSet(const HDEVINFO handle) : handle_(handle) {}
  ~DeviceInfoSet() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      SetupDiDestroyDeviceInfoList(handle_);
    }
  }
  DeviceInfoSet(const DeviceInfoSet&) = delete;
  DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
  [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }

 private:
  HDEVINFO handle_;
};

class WindowsHandle final {
 public:
  explicit WindowsHandle(const HANDLE handle) : handle_(handle) {}
  ~WindowsHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }
  WindowsHandle(const WindowsHandle&) = delete;
  WindowsHandle& operator=(const WindowsHandle&) = delete;
  WindowsHandle(WindowsHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = INVALID_HANDLE_VALUE;
  }
  WindowsHandle& operator=(WindowsHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_;
};

std::string trim(std::string value) {
  const auto notSpace = [](const unsigned char character) { return !std::isspace(character); };
  const auto first = std::find_if(value.begin(), value.end(), notSpace);
  const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
  return first < last ? std::string(first, last) : std::string{};
}

std::string wideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0,
                                            nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), required, nullptr, nullptr);
  return result;
}

std::wstring utf8ToWide(const std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int required = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0);
  if (required <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), required) == 0) {
    return {};
  }
  return result;
}

std::string errorMessage(const std::string_view action,
                         const DWORD error = GetLastError()) {
  std::vector<wchar_t> buffer(1024);
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer.data(),
      static_cast<DWORD>(buffer.size()), nullptr);
  std::string result(action);
  result += " (Windows error " + std::to_string(error) + ')';
  if (length != 0) {
    std::wstring detail(buffer.data(), length);
    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' ')) {
      detail.pop_back();
    }
    result += ": " + wideToUtf8(detail);
  }
  return result;
}

void report(const core::RawWriteProgressCallback& callback,
            const core::RawWriteStage stage, const std::uint64_t processed,
            const std::uint64_t total) {
  if (callback) {
    callback({stage, processed, total});
  }
}

bool cancelled(const core::RawWriteCancelCallback& callback) {
  return callback && callback();
}

std::optional<DWORD> physicalDriveNumber(const std::string_view path) {
  constexpr std::string_view prefix = "\\\\.\\PhysicalDrive";
  if (path.size() <= prefix.size()) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(path[index])) !=
        std::tolower(static_cast<unsigned char>(prefix[index]))) {
      return std::nullopt;
    }
  }
  DWORD result = 0;
  for (std::size_t index = prefix.size(); index < path.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(path[index]);
    if (!std::isdigit(character)) {
      return std::nullopt;
    }
    const DWORD digit = static_cast<DWORD>(character - '0');
    if (result > (std::numeric_limits<DWORD>::max() - digit) / 10U) {
      return std::nullopt;
    }
    result = result * 10U + digit;
  }
  return result;
}

using detail::positionalRead;
using WindowsRawTarget = detail::SystemRawTarget;

class WindowsRawSource final : public core::RawSourceIo {
 public:
  WindowsRawSource(const HANDLE handle, const std::uint64_t capacity)
      : handle_(handle), capacity_(capacity) {}

  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override {
    return capacity_;
  }

  bool readAt(const std::uint64_t offset, unsigned char* data,
              const std::size_t size, std::string& error) override {
    return positionalRead(handle_, offset, data, size, error);
  }

 private:
  HANDLE handle_;
  std::uint64_t capacity_;
};

struct RawGeometry final {
  std::uint64_t capacity{};
  std::uint32_t sectorSize{};
};

std::optional<RawGeometry> queryRawGeometry(const HANDLE handle,
                                            std::string& error) {
  DISK_GEOMETRY_EX geometry{};
  DWORD returned = 0;
  if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                       &geometry, sizeof(geometry), &returned, nullptr) ||
      geometry.DiskSize.QuadPart <= 0 || geometry.Geometry.BytesPerSector == 0) {
    error = errorMessage("Unable to read the raw-device geometry");
    return std::nullopt;
  }

  RawGeometry result;
  result.capacity = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
  result.sectorSize = geometry.Geometry.BytesPerSector;

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageAccessAlignmentProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
  if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                      &alignment, sizeof(alignment), &returned, nullptr) &&
      returned >= sizeof(alignment) && alignment.BytesPerLogicalSector != 0) {
    result.sectorSize = alignment.BytesPerLogicalSector;
  }
  return result;
}

bool queryDiskNumbers(const HANDLE volume, std::set<DWORD>& disks) {
  std::vector<unsigned char> buffer(64U * 1024U);
  DWORD returned = 0;
  if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                       buffer.data(), static_cast<DWORD>(buffer.size()), &returned,
                       nullptr) ||
      returned < offsetof(VOLUME_DISK_EXTENTS, Extents)) {
    return false;
  }
  const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  const std::size_t capacity =
      (returned - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT);
  const DWORD count = static_cast<DWORD>(
      std::min<std::size_t>(extents->NumberOfDiskExtents, capacity));
  for (DWORD index = 0; index < count; ++index) {
    disks.insert(extents->Extents[index].DiskNumber);
  }
  return count != 0;
}

std::wstring volumeOpenPath(std::wstring name) {
  if (!name.empty() && name.back() == L'\\') {
    name.pop_back();
  }
  return name;
}

bool volumeNamesForDisk(const DWORD diskNumber, std::vector<std::wstring>& names,
                        std::string& error) {
  std::vector<wchar_t> name(1024);
  HANDLE search = FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()));
  if (search == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_NO_MORE_FILES) {
      return true;
    }
    error = errorMessage("Unable to enumerate Windows volumes");
    return false;
  }

  bool succeeded = true;
  for (;;) {
    const std::wstring openPath = volumeOpenPath(name.data());
    WindowsHandle volume(CreateFileW(openPath.c_str(), 0,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr));
    std::set<DWORD> disks;
    if (volume.valid() && queryDiskNumbers(volume.get(), disks) &&
        disks.count(diskNumber) != 0) {
      names.emplace_back(name.data());
    }

    if (!FindNextVolumeW(search, name.data(), static_cast<DWORD>(name.size()))) {
      if (GetLastError() != ERROR_NO_MORE_FILES) {
        error = errorMessage("Windows volume enumeration ended unexpectedly");
        succeeded = false;
      }
      break;
    }
  }
  FindVolumeClose(search);
  return succeeded;
}

std::vector<std::string> mountedPathsForDisk(const DWORD diskNumber) {
  std::vector<std::wstring> volumeNames;
  std::string ignored;
  if (!volumeNamesForDisk(diskNumber, volumeNames, ignored)) {
    return {};
  }
  std::vector<std::string> paths;
  for (const auto& volumeName : volumeNames) {
    DWORD required = 0U;
    static_cast<void>(GetVolumePathNamesForVolumeNameW(
        volumeName.c_str(), nullptr, 0U, &required));
    if (required <= 1U) {
      continue;
    }
    std::vector<wchar_t> buffer(required, L'\0');
    if (!GetVolumePathNamesForVolumeNameW(
            volumeName.c_str(), buffer.data(), required, &required)) {
      continue;
    }
    std::wstring preferred;
    const auto isDriveRoot = [](const std::wstring_view path) {
      const bool letter = path.size() == 3U &&
                          ((path[0] >= L'A' && path[0] <= L'Z') ||
                           (path[0] >= L'a' && path[0] <= L'z'));
      return letter && path[1] == L':' &&
             (path[2] == L'\\' || path[2] == L'/');
    };
    for (const wchar_t* path = buffer.data(); *path != L'\0';
         path += std::wcslen(path) + 1U) {
      const std::wstring candidate(path);
      if (preferred.empty() ||
          (isDriveRoot(candidate) && !isDriveRoot(preferred)) ||
          (isDriveRoot(candidate) == isDriveRoot(preferred) &&
           candidate.size() < preferred.size())) {
        preferred = candidate;
      }
    }
    if (!preferred.empty()) {
      // Keep one usable root per volume. This lets the UDF provider require
      // exactly one mounted volume without rejecting a volume merely because
      // Windows assigned it both a drive letter and a directory mount point.
      paths.push_back(wideToUtf8(preferred));
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

class LockedVolumes final {
 public:
  bool acquire(const DWORD diskNumber, std::string& error) {
    std::vector<std::wstring> names;
    if (!volumeNamesForDisk(diskNumber, names, error)) {
      return false;
    }
    DWORD returned = 0;
    for (const auto& name : names) {
      const std::wstring openPath = volumeOpenPath(name);
      WindowsHandle volume(CreateFileW(
          openPath.c_str(), GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
      if (!volume.valid()) {
        error = errorMessage("Unable to open target volume " + wideToUtf8(name));
        handles_.clear();
        return false;
      }
      if (!DeviceIoControl(volume.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                           &returned, nullptr)) {
        error = errorMessage("Unable to lock target volume " + wideToUtf8(name));
        handles_.clear();
        return false;
      }
      handles_.push_back(std::move(volume));
    }
    for (auto& volume : handles_) {
      if (!DeviceIoControl(volume.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr,
                           0, &returned, nullptr)) {
        error = errorMessage("Unable to dismount a locked target volume");
        handles_.clear();
        return false;
      }
    }
    return true;
  }

  void release() noexcept { handles_.clear(); }

 private:
  std::vector<WindowsHandle> handles_;
};

bool sourceUsesDisk(const std::wstring& sourcePath, const DWORD targetDisk,
                    bool& usesTarget, std::string& error) {
  usesTarget = false;
  std::vector<wchar_t> volumeRoot(1024);
  if (!GetVolumePathNameW(sourcePath.c_str(), volumeRoot.data(),
                          static_cast<DWORD>(volumeRoot.size()))) {
    error = errorMessage("Unable to identify the volume containing the source image");
    return false;
  }
  if (GetDriveTypeW(volumeRoot.data()) == DRIVE_REMOTE) {
    return true;
  }
  std::vector<wchar_t> volumeName(1024);
  if (!GetVolumeNameForVolumeMountPointW(volumeRoot.data(), volumeName.data(),
                                         static_cast<DWORD>(volumeName.size()))) {
    error = errorMessage("Unable to identify the source image's physical volume");
    return false;
  }
  const std::wstring openPath = volumeOpenPath(volumeName.data());
  WindowsHandle volume(CreateFileW(openPath.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr));
  if (!volume.valid()) {
    error = errorMessage("Unable to inspect the source image's physical volume");
    return false;
  }
  std::set<DWORD> disks;
  if (!queryDiskNumbers(volume.get(), disks)) {
    error = errorMessage("Unable to read the source volume's disk extents");
    return false;
  }
  usesTarget = disks.count(targetDisk) != 0;
  return true;
}

bool processIsElevated() {
  HANDLE tokenHandle = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle)) {
    return false;
  }
  WindowsHandle token(tokenHandle);
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  return GetTokenInformation(token.get(), TokenElevation, &elevation,
                             sizeof(elevation), &returned) &&
         elevation.TokenIsElevated != 0;
}

std::wstring currentExecutable() {
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (static_cast<std::size_t>(length) < buffer.size() - 1U) {
      return std::wstring(buffer.data(), length);
    }
    if (buffer.size() > 32768U) {
      return {};
    }
    buffer.resize(buffer.size() * 2U);
  }
}

std::filesystem::path udfCaptureProvider() {
  const std::wstring executable = currentExecutable();
  if (executable.empty()) {
    return {};
  }
  const auto candidate =
      std::filesystem::path(executable).parent_path() / L"oscdimg.exe";
  const DWORD attributes = GetFileAttributesW(candidate.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
                 (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
             ? candidate
             : std::filesystem::path{};
}

std::filesystem::path findWindowsExecutable(const wchar_t* name) {
  std::array<wchar_t, 32768> directory{};
  const UINT length = GetSystemDirectoryW(
      directory.data(), static_cast<UINT>(directory.size()));
  if (length == 0 || static_cast<std::size_t>(length) >= directory.size()) {
    return {};
  }
  const std::filesystem::path candidate =
      std::filesystem::path(directory.data()) / name;
  const DWORD attributes = GetFileAttributesW(candidate.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
                 (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
             ? candidate
             : std::filesystem::path{};
}

core::RawWriteResult runDismFfuApply(
    const std::filesystem::path& dism, const std::wstring& sourcePath,
    const std::wstring& targetPath, const std::uint64_t targetBytes,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) {
  core::RawWriteResult result;
  const std::wstring imageArgument = L"/ImageFile:" + sourcePath;
  const std::wstring driveArgument = L"/ApplyDrive:" + targetPath;
  const std::vector<std::wstring> arguments{
      dism.wstring(), L"/English", L"/Apply-Ffu", imageArgument,
      driveArgument};
  std::vector<const wchar_t*> nativeArguments;
  nativeArguments.reserve(arguments.size() + 1U);
  for (const auto& argument : arguments) {
    nativeArguments.push_back(argument.c_str());
  }
  nativeArguments.push_back(nullptr);

  report(onProgress, core::RawWriteStage::Writing, 0, targetBytes);
  const intptr_t spawned = _wspawnv(_P_NOWAIT, dism.c_str(),
                                     nativeArguments.data());
  if (spawned == -1) {
    result.error = "Unable to start DISM for FFU deployment";
    return result;
  }
  WindowsHandle process(reinterpret_cast<HANDLE>(spawned));
  result.destructiveWriteStarted = true;
  for (;;) {
    const DWORD wait = WaitForSingleObject(process.get(), 100);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_TIMEOUT) {
      result.error = errorMessage("Unable to wait for DISM FFU deployment");
      return result;
    }
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      static_cast<void>(TerminateProcess(process.get(), ERROR_CANCELLED));
      static_cast<void>(WaitForSingleObject(process.get(), INFINITE));
      result.error = "FFU deployment was cancelled; the target may contain a partial image";
      return result;
    }
  }
  DWORD exitCode = ERROR_GEN_FAILURE;
  if (!GetExitCodeProcess(process.get(), &exitCode)) {
    result.error = errorMessage("Unable to read the DISM FFU deployment result");
    return result;
  }
  if (exitCode != ERROR_SUCCESS) {
    result.error = "DISM /Apply-Ffu failed with exit code " +
                   std::to_string(exitCode);
    return result;
  }
  result.success = true;
  result.bytesWritten = targetBytes;
  return result;
}

core::MediaCaptureResult runDismFfuCapture(
    const std::filesystem::path& dism,
    const std::filesystem::path& destination,
    const std::wstring& sourcePath, const std::uint64_t sourceBytes,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  const std::wstring imageArgument = L"/ImageFile:" + destination.wstring();
  const std::wstring driveArgument = L"/CaptureDrive:" + sourcePath;
  const std::vector<std::wstring> arguments{
      dism.wstring(), L"/English", L"/Capture-Ffu", imageArgument,
      driveArgument, L"/Name:Rufus++ capture"};
  std::vector<const wchar_t*> nativeArguments;
  nativeArguments.reserve(arguments.size() + 1U);
  for (const auto& argument : arguments) {
    nativeArguments.push_back(argument.c_str());
  }
  nativeArguments.push_back(nullptr);
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Capturing, 0U, sourceBytes});
  }
  const intptr_t spawned = _wspawnv(_P_NOWAIT, dism.c_str(),
                                     nativeArguments.data());
  if (spawned == -1) {
    result.error = "Unable to start DISM for FFU capture";
    return result;
  }
  WindowsHandle process(reinterpret_cast<HANDLE>(spawned));
  for (;;) {
    const DWORD wait = WaitForSingleObject(process.get(), 100);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_TIMEOUT) {
      result.error = errorMessage("Unable to wait for DISM FFU capture");
      return result;
    }
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      static_cast<void>(TerminateProcess(process.get(), ERROR_CANCELLED));
      static_cast<void>(WaitForSingleObject(process.get(), INFINITE));
      result.error = "FFU capture was cancelled";
      return result;
    }
  }
  DWORD exitCode = ERROR_GEN_FAILURE;
  if (!GetExitCodeProcess(process.get(), &exitCode)) {
    result.error = errorMessage("Unable to read the DISM FFU capture result");
    return result;
  }
  if (exitCode != ERROR_SUCCESS) {
    result.error = "DISM /Capture-Ffu failed with exit code " +
                   std::to_string(exitCode);
    return result;
  }
  std::error_code fileError;
  result.outputSizeBytes = std::filesystem::file_size(destination, fileError);
  if (fileError || result.outputSizeBytes == 0U) {
    result.error = "DISM completed without producing a readable FFU image";
    return result;
  }
  result.bytesCaptured = sourceBytes;
  result.success = true;
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Finalizing, sourceBytes, sourceBytes});
  }
  return result;
}

core::MediaCaptureResult runOscdimgUdfCapture(
    const std::filesystem::path& provider,
    const std::filesystem::path& destination,
    const std::wstring& sourceRoot, const std::uint64_t sourceBytes,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  const std::vector<std::wstring> arguments{
      provider.wstring(), L"-u2", L"-udfver102", L"-lRUFUSPP_CAPTURE",
      sourceRoot, destination.wstring()};
  std::vector<const wchar_t*> native;
  native.reserve(arguments.size() + 1U);
  for (const auto& argument : arguments) {
    native.push_back(argument.c_str());
  }
  native.push_back(nullptr);
  if (onProgress) {
    onProgress({core::MediaCaptureStage::Capturing, 0U, sourceBytes});
  }
  const intptr_t spawned =
      _wspawnv(_P_NOWAIT, provider.c_str(), native.data());
  if (spawned == -1) {
    result.error = "Unable to start the installed Windows UDF capture provider";
    return result;
  }
  WindowsHandle process(reinterpret_cast<HANDLE>(spawned));
  for (;;) {
    const DWORD wait = WaitForSingleObject(process.get(), 100U);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_TIMEOUT) {
      result.error = "Unable to wait for the Windows UDF capture provider";
      return result;
    }
    if (cancelled(isCancelled)) {
      result.cancelled = true;
      static_cast<void>(TerminateProcess(process.get(), ERROR_CANCELLED));
      static_cast<void>(WaitForSingleObject(process.get(), INFINITE));
      result.error = "UDF ISO capture was cancelled";
      return result;
    }
  }
  DWORD exitCode = ERROR_GEN_FAILURE;
  if (!GetExitCodeProcess(process.get(), &exitCode) ||
      exitCode != ERROR_SUCCESS) {
    result.error = "The Windows UDF capture provider failed";
    return result;
  }
  std::error_code fileError;
  result.outputSizeBytes = std::filesystem::file_size(destination, fileError);
  if (fileError || result.outputSizeBytes == 0U) {
    result.error = "The Windows UDF provider produced no readable image";
    return result;
  }
  result.bytesCaptured = sourceBytes;
  result.success = true;
  return result;
}

std::string registryDisplayName(const HDEVINFO devices, SP_DEVINFO_DATA& deviceInfo) {
  std::vector<wchar_t> buffer(512);
  DWORD required = 0;
  DWORD type = 0;
  auto readProperty = [&](const DWORD property) {
    return SetupDiGetDeviceRegistryPropertyW(
        devices, &deviceInfo, property, &type,
        reinterpret_cast<PBYTE>(buffer.data()),
        static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), &required) != FALSE;
  };
  if (!readProperty(SPDRP_FRIENDLYNAME) && !readProperty(SPDRP_DEVICEDESC)) {
    return {};
  }
  return trim(wideToUtf8(buffer.data()));
}

std::string deviceInstanceId(const HDEVINFO devices, SP_DEVINFO_DATA& deviceInfo) {
  DWORD required = 0;
  SetupDiGetDeviceInstanceIdW(devices, &deviceInfo, nullptr, 0, &required);
  if (required == 0) {
    return {};
  }
  std::vector<wchar_t> buffer(required);
  if (!SetupDiGetDeviceInstanceIdW(devices, &deviceInfo, buffer.data(),
                                   static_cast<DWORD>(buffer.size()), nullptr)) {
    return {};
  }
  return wideToUtf8(buffer.data());
}

std::string descriptorString(const std::vector<unsigned char>& buffer, const DWORD offset) {
  if (offset == 0 || static_cast<std::size_t>(offset) >= buffer.size()) {
    return {};
  }
  const auto* begin = reinterpret_cast<const char*>(buffer.data() + offset);
  const auto* end = reinterpret_cast<const char*>(buffer.data() + buffer.size());
  const auto* terminator = std::find(begin, end, '\0');
  return trim(std::string(begin, terminator));
}

core::DeviceBus translateBus(const STORAGE_BUS_TYPE bus) {
  switch (bus) {
    case BusTypeUsb:
      return core::DeviceBus::Usb;
    case BusTypeSd:
    case BusTypeMmc:
      return core::DeviceBus::Sd;
    case BusTypeNvme:
      return core::DeviceBus::Nvme;
    case BusTypeAta:
    case BusTypeSata:
      return core::DeviceBus::Sata;
    case BusTypeVirtual:
    case BusTypeFileBackedVirtual:
      return core::DeviceBus::Virtual;
    default:
      return core::DeviceBus::Unknown;
  }
}

std::set<DWORD> systemDiskNumbers() {
  std::set<DWORD> result;
  std::vector<wchar_t> windowsDirectory(MAX_PATH);
  const UINT directoryLength = GetWindowsDirectoryW(
      windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
  if (directoryLength < 2U ||
      static_cast<std::size_t>(directoryLength) >= windowsDirectory.size()) {
    return result;
  }

  std::wstring volumePath = L"\\\\.\\";
  volumePath.append(windowsDirectory.data(), 2);
  WindowsHandle volume(CreateFileW(volumePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr));
  if (!volume.valid()) {
    return result;
  }

  std::vector<unsigned char> buffer(4096);
  DWORD returned = 0;
  if (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                       buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
    return result;
  }
  const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  const std::size_t extentCapacity =
      returned > offsetof(VOLUME_DISK_EXTENTS, Extents)
          ? (returned - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT)
          : 0;
  const DWORD extentCount = static_cast<DWORD>(
      std::min<std::size_t>(extents->NumberOfDiskExtents, extentCapacity));
  for (DWORD index = 0; index < extentCount; ++index) {
    result.insert(extents->Extents[index].DiskNumber);
  }
  return result;
}

DeviceDiscoveryResult WindowsBlockDeviceBackend::discover() const {
  DeviceDiscoveryResult result;
  DeviceInfoSet devices(SetupDiGetClassDevsW(
      &GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
  if (devices.get() == INVALID_HANDLE_VALUE) {
    result.warnings.emplace_back("Windows disk-interface discovery failed");
    return result;
  }

  const std::set<DWORD> systemDisks = systemDiskNumbers();
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    if (!SetupDiEnumDeviceInterfaces(devices.get(), nullptr, &GUID_DEVINTERFACE_DISK,
                                     index, &interfaceData)) {
      if (GetLastError() != ERROR_NO_MORE_ITEMS) {
        result.warnings.emplace_back("Windows did not enumerate every disk interface");
      }
      break;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(devices.get(), &interfaceData, nullptr, 0,
                                     &required, nullptr);
    if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
      continue;
    }
    std::vector<unsigned char> detailBuffer(required);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA deviceInfo{};
    deviceInfo.cbSize = sizeof(deviceInfo);
    if (!SetupDiGetDeviceInterfaceDetailW(devices.get(), &interfaceData, detail, required,
                                          nullptr, &deviceInfo)) {
      continue;
    }

    WindowsHandle disk(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr));
    if (!disk.valid()) {
      continue;
    }

    STORAGE_DEVICE_NUMBER number{};
    DWORD returned = 0;
    if (!DeviceIoControl(disk.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                         &number, sizeof(number), &returned, nullptr)) {
      continue;
    }

    DISK_GEOMETRY_EX geometry{};
    if (!DeviceIoControl(disk.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                         &geometry, sizeof(geometry), &returned, nullptr)) {
      continue;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<unsigned char> descriptorBuffer(4096);
    if (!DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                         descriptorBuffer.data(), static_cast<DWORD>(descriptorBuffer.size()),
                         &returned, nullptr) || returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
      continue;
    }
    descriptorBuffer.resize(returned);
    const auto* descriptor =
        reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descriptorBuffer.data());

    core::BlockDeviceInfo device;
    device.devicePath = "\\\\.\\PhysicalDrive" + std::to_string(number.DeviceNumber);
    device.vendor = descriptorString(descriptorBuffer, descriptor->VendorIdOffset);
    device.model = descriptorString(descriptorBuffer, descriptor->ProductIdOffset);
    device.serialNumber = descriptorString(descriptorBuffer, descriptor->SerialNumberOffset);
    const std::string instanceId = deviceInstanceId(devices.get(), deviceInfo);
    device.displayName = registryDisplayName(devices.get(), deviceInfo);
    if (device.displayName.empty()) {
      device.displayName = trim(device.vendor + " " + device.model);
    }
    if (device.displayName.empty()) {
      device.displayName = "Physical disk " + std::to_string(number.DeviceNumber);
    }
    if (geometry.DiskSize.QuadPart > 0) {
      device.capacityBytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
    }
    device.logicalSectorSize = geometry.Geometry.BytesPerSector;
    device.bus = translateBus(descriptor->BusType);
    device.removable = descriptor->RemovableMedia != FALSE;
    device.ejectable = device.removable || device.bus == core::DeviceBus::Usb ||
                       device.bus == core::DeviceBus::Sd;
    const BOOL writable = DeviceIoControl(disk.get(), IOCTL_DISK_IS_WRITABLE, nullptr, 0,
                                          nullptr, 0, &returned, nullptr);
    device.writable = writable != FALSE || GetLastError() != ERROR_WRITE_PROTECT;
    device.systemDevice = systemDisks.count(number.DeviceNumber) != 0;
    device.wholeDevice = true;
    device.mountPoints = mountedPathsForDisk(number.DeviceNumber);
    device.stableId = "windows:" +
                      (device.serialNumber.empty()
                           ? (instanceId.empty() ? std::to_string(number.DeviceNumber)
                                                 : instanceId)
                           : device.serialNumber) +
                      ':' + std::to_string(device.capacityBytes);
    result.devices.push_back(std::move(device));
  }

  std::sort(result.devices.begin(), result.devices.end(),
            [](const core::BlockDeviceInfo& left, const core::BlockDeviceInfo& right) {
              return left.devicePath < right.devicePath;
            });
  return result;
}

PrivilegeStatus WindowsBlockDeviceBackend::privilegeStatus() const {
  if (processIsElevated()) {
    return {PrivilegeRoute::Elevated,
            "This application process has Administrator access; guarded physical-device operations are enabled"};
  }
  return {PrivilegeRoute::Unprivileged,
          "This application process is not elevated; select AUTHORIZE to open an Administrator window when physical-device access is needed"};
}

RawWriteAvailability WindowsBlockDeviceBackend::rawWriteAvailability(
    const core::BlockDeviceInfo& target) const {
  const auto eligibility = core::evaluateDeviceEligibility(target);
  if (eligibility != core::DeviceEligibility::Eligible) {
    return {false, false, "Target is not eligible: " +
                       std::string(core::deviceEligibilityName(eligibility))};
  }
  if (!physicalDriveNumber(target.devicePath).has_value()) {
    return {false, false, "The target is not a whole Windows PhysicalDrive path"};
  }
  if (!processIsElevated()) {
    return {false, !currentExecutable().empty(),
            "Administrator access is required. Select AUTHORIZE to open an elevated Rufus++ "
            "window."};
  }

  const std::wstring devicePath = utf8ToWide(target.devicePath);
  if (devicePath.empty()) {
    return {false, false, "The target device path is not valid UTF-8"};
  }
  WindowsHandle disk(CreateFileW(
      devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  if (!disk.valid()) {
    return {false, false, errorMessage("The target cannot be opened for raw read/write access")};
  }
  return {true, false, "Administrator raw-device access is available"};
}

RawWriteAvailability WindowsBlockDeviceBackend::captureAvailability(
    const core::BlockDeviceInfo& target,
    const core::MediaCaptureFormat format) const {
  const auto eligibility = core::evaluateDeviceEligibility(target);
  if (eligibility != core::DeviceEligibility::Eligible) {
    return {false, false, "Capture source is not eligible: " +
                       std::string(core::deviceEligibilityName(eligibility))};
  }
  if (!physicalDriveNumber(target.devicePath).has_value()) {
    return {false, false,
            "Capture requires a whole Windows PhysicalDrive path"};
  }
  if (format == core::MediaCaptureFormat::UdfIso) {
    if (target.mountPoints.size() != 1U) {
      return {false, false,
              "UDF ISO capture requires exactly one mounted source volume"};
    }
    if (udfCaptureProvider().empty()) {
      return {false, false,
              "Place a trusted Windows ADK oscdimg.exe beside Rufus++ to enable UDF capture"};
    }
    return {true, false,
            "The user-installed Windows UDF capture provider is available"};
  }
  if (format == core::MediaCaptureFormat::Ffu &&
      findWindowsExecutable(L"dism.exe").empty()) {
    return {false, false,
            "Windows DISM with the FFU capture provider is not available"};
  }
  constexpr std::uint64_t maximumVhdBytes =
      2040ULL * 1024ULL * 1024ULL * 1024ULL;
  if ((format == core::MediaCaptureFormat::FixedVhd ||
       format == core::MediaCaptureFormat::DynamicVhd) &&
      target.capacityBytes > maximumVhdBytes) {
    return {false, false, "Classic VHD capture is limited to 2040 GiB"};
  }
  if (!processIsElevated()) {
    return {false, !currentExecutable().empty(),
            "Administrator access is required. Select AUTHORIZE to open an elevated Rufus++ window."};
  }
  return {true, false, "Administrator capture access is available"};
}

core::MediaCaptureResult WindowsBlockDeviceBackend::capture(
    const core::BlockDeviceInfo& selected,
    const std::filesystem::path& destination,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) const {
  core::MediaCaptureResult result;
  const auto ready = captureAvailability(selected, options.format);
  if (!ready.available) {
    result.error = ready.reason;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Capture cancelled before the device was opened";
    return result;
  }
  const auto diskNumber = physicalDriveNumber(selected.devicePath);
  const std::wstring devicePath = utf8ToWide(selected.devicePath);
  if (!diskNumber.has_value() || devicePath.empty() || destination.empty()) {
    result.error = "Capture source or destination path is invalid";
    return result;
  }
  std::error_code pathError;
  if (std::filesystem::exists(destination, pathError) || pathError) {
    result.error = pathError
                       ? "Unable to inspect the capture destination: " +
                             pathError.message()
                       : "The capture destination already exists";
    return result;
  }
  const auto destinationParent = destination.parent_path().empty()
                                     ? std::filesystem::current_path(pathError)
                                     : destination.parent_path();
  if (pathError || destinationParent.empty() ||
      !std::filesystem::is_directory(destinationParent, pathError) ||
      pathError) {
    result.error = "Unable to identify the capture destination directory";
    return result;
  }
  bool destinationUsesTarget = false;
  if (!sourceUsesDisk(destinationParent.wstring(), *diskNumber,
                      destinationUsesTarget, result.error)) {
    return result;
  }
  if (destinationUsesTarget) {
    result.error = "The capture destination is stored on the device being captured";
    return result;
  }

  if (options.format == core::MediaCaptureFormat::UdfIso) {
    const DeviceDiscoveryResult current = discover();
    const auto observed = std::find_if(
        current.devices.begin(), current.devices.end(),
        [&selected](const core::BlockDeviceInfo& device) {
          return device.devicePath == selected.devicePath;
        });
    if (observed == current.devices.end() ||
        observed->mountPoints != selected.mountPoints) {
      result.error = "The mounted UDF source volume changed after selection";
      return result;
    }
    const core::SafetyPolicy safetyPolicy;
    const auto identity = safetyPolicy.validateIdentity(selected, *observed);
    if (!identity.safe()) {
      result.error = identity.issues.front().message;
      return result;
    }
    auto partial = destinationParent /
                   (destination.stem().wstring() + L".rufus-plus-plus-capture-" +
                    std::to_wstring(GetCurrentProcessId()) +
                    destination.extension().wstring());
    if (std::filesystem::exists(partial, pathError) || pathError) {
      result.error = "Unable to allocate a private UDF capture path";
      return result;
    }
    const auto cleanup = [&] {
      std::error_code ignored;
      std::filesystem::remove(partial, ignored);
    };
    const std::wstring sourceRoot =
        utf8ToWide(selected.mountPoints.front());
    if (sourceRoot.empty()) {
      result.error = "The mounted UDF source path is not valid UTF-8";
      return result;
    }
    result = runOscdimgUdfCapture(
        udfCaptureProvider(), partial, sourceRoot, selected.capacityBytes,
        onProgress, isCancelled);
    if (!result.success) {
      cleanup();
      return result;
    }
    if (onProgress) {
      onProgress({core::MediaCaptureStage::Verifying,
                  selected.capacityBytes, selected.capacityBytes});
    }
    const core::ImageAnalyzer analyzer;
    const auto analysis = analyzer.analyze(partial);
    if (!analysis.succeeded() ||
        analysis.image->format != core::ImageFormat::Iso ||
        !analysis.image->capabilities.udf ||
        !analysis.image->capabilities.isoExtraction) {
      result.success = false;
      result.error =
          "The generated UDF ISO failed structural and directory-tree validation";
      cleanup();
      return result;
    }
    std::filesystem::rename(partial, destination, pathError);
    if (pathError) {
      result.success = false;
      result.error = "Unable to commit the UDF ISO capture: " +
                     pathError.message();
      cleanup();
      return result;
    }
    if (onProgress) {
      onProgress({core::MediaCaptureStage::Complete,
                  selected.capacityBytes, selected.capacityBytes});
    }
    return result;
  }

  const DeviceDiscoveryResult beforeLock = discover();
  const auto observed = std::find_if(
      beforeLock.devices.begin(), beforeLock.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (observed == beforeLock.devices.end()) {
    result.error = "The selected capture device is no longer connected";
    return result;
  }
  const core::SafetyPolicy safetyPolicy;
  const auto identity = safetyPolicy.validateIdentity(selected, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }

  LockedVolumes volumes;
  if (!volumes.acquire(*diskNumber, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Capture cancelled after locking the source volumes";
    return result;
  }

  const DeviceDiscoveryResult afterLock = discover();
  const auto reobserved = std::find_if(
      afterLock.devices.begin(), afterLock.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (reobserved == afterLock.devices.end()) {
    result.error = "The capture device disappeared after its volumes were locked";
    return result;
  }
  const auto finalIdentity = safetyPolicy.validateIdentity(selected, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  if (options.format == core::MediaCaptureFormat::Ffu) {
    auto partial = destination.parent_path() /
                   (destination.stem().wstring() + L".rufus-plus-plus-capture-" +
                    std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(GetTickCount64()) +
                    destination.extension().wstring());
    if (std::filesystem::exists(partial, pathError) || pathError) {
      result.error = "Unable to allocate a private FFU capture path";
      return result;
    }
    volumes.release();
    result = runDismFfuCapture(findWindowsExecutable(L"dism.exe"), partial,
                               devicePath, selected.capacityBytes, onProgress,
                               isCancelled);
    const auto removePartial = [&] {
      std::error_code ignored;
      std::filesystem::remove(partial, ignored);
    };
    if (!result.success) {
      removePartial();
      return result;
    }
    const core::ImageAnalyzer analyzer;
    const auto analysis = analyzer.analyze(partial);
    if (!analysis.succeeded() ||
        analysis.image->format != core::ImageFormat::Ffu ||
        !analysis.image->capabilities.validContainerMetadata) {
      result.success = false;
      result.error = "DISM produced an FFU image that failed structural validation";
      removePartial();
      return result;
    }
    std::filesystem::rename(partial, destination, pathError);
    if (pathError) {
      result.success = false;
      result.error = "Unable to commit the FFU capture: " +
                     pathError.message();
      removePartial();
      return result;
    }
    if (onProgress) {
      onProgress({core::MediaCaptureStage::Complete,
                  selected.capacityBytes, selected.capacityBytes});
    }
    return result;
  }

  WindowsHandle disk(CreateFileW(
      devicePath.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!disk.valid()) {
    result.error = errorMessage("Unable to open the physical disk for capture");
    return result;
  }
  const auto geometry = queryRawGeometry(disk.get(), result.error);
  if (!geometry.has_value() || geometry->capacity != selected.capacityBytes ||
      geometry->sectorSize != selected.logicalSectorSize) {
    if (result.error.empty()) {
      result.error = "Capture-device geometry changed before reading";
    }
    return result;
  }

  WindowsRawSource source(disk.get(), geometry->capacity);
  const core::MediaCaptureWriter writer;
  return writer.capture(source, destination, options, onProgress, isCancelled);
}

RawWriteAvailability WindowsBlockDeviceBackend::badBlockTestAvailability(
    const core::BlockDeviceInfo& target) const {
  return rawWriteAvailability(target);
}

core::BadBlockTestResult WindowsBlockDeviceBackend::testBadBlocks(
    const core::BlockDeviceInfo& selected,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled) const {
  core::BadBlockTestResult result;
  const auto ready = badBlockTestAvailability(selected);
  if (!ready.available) {
    result.error = ready.reason;
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled before the target was modified";
    return result;
  }
  const auto diskNumber = physicalDriveNumber(selected.devicePath);
  const std::wstring devicePath = utf8ToWide(selected.devicePath);
  if (!diskNumber.has_value() || devicePath.empty()) {
    result.error = "Bad-block testing requires a whole Windows PhysicalDrive";
    return result;
  }

  const DeviceDiscoveryResult beforeLock = discover();
  const auto observed = std::find_if(
      beforeLock.devices.begin(), beforeLock.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (observed == beforeLock.devices.end()) {
    result.error = "The selected test device is no longer connected";
    return result;
  }
  const core::SafetyPolicy safetyPolicy;
  const auto identity = safetyPolicy.validateIdentity(selected, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }

  LockedVolumes volumes;
  if (!volumes.acquire(*diskNumber, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Bad-block test cancelled after locking the target volumes";
    return result;
  }
  const DeviceDiscoveryResult afterLock = discover();
  const auto reobserved = std::find_if(
      afterLock.devices.begin(), afterLock.devices.end(),
      [&selected](const auto& device) {
        return device.devicePath == selected.devicePath;
      });
  if (reobserved == afterLock.devices.end()) {
    result.error = "The test device disappeared after its volumes were locked";
    return result;
  }
  const auto finalIdentity = safetyPolicy.validateIdentity(selected, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  WindowsHandle disk(CreateFileW(
      devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_WRITE_THROUGH, nullptr));
  if (!disk.valid()) {
    result.error = errorMessage("Unable to open the physical disk for testing");
    return result;
  }
  const auto geometry = queryRawGeometry(disk.get(), result.error);
  if (!geometry.has_value() || geometry->capacity != selected.capacityBytes ||
      geometry->sectorSize != selected.logicalSectorSize) {
    if (result.error.empty()) {
      result.error = "Test-device geometry changed before writing";
    }
    return result;
  }
  WindowsRawTarget target(disk.get(), geometry->capacity,
                          geometry->sectorSize);
  const core::BadBlockTester tester;
  return tester.test(target, options, onProgress, isCancelled);
}

RawWriteAvailability WindowsBlockDeviceBackend::ffuApplyAvailability(
    const core::ImageInfo& image,
    const core::BlockDeviceInfo& target) const {
  if (image.format != core::ImageFormat::Ffu ||
      !image.capabilities.validContainerMetadata) {
    return {false, false,
            "FFU deployment requires a structurally validated FFU image"};
  }
  const auto rawAccess = rawWriteAvailability(target);
  if (!rawAccess.available) {
    return rawAccess;
  }
  if (findWindowsExecutable(L"dism.exe").empty()) {
    return {false, false,
            "Windows DISM with the FFU provider is not available"};
  }
  return {true, false, "Windows DISM FFU deployment is available"};
}

RawWriteAvailability WindowsBlockDeviceBackend::requestRawWriteAuthorization() const {
  if (processIsElevated()) {
    return {true, false, "This Rufus++ process already has administrator access"};
  }
  const std::wstring executable = currentExecutable();
  if (executable.empty()) {
    return {false, false, "Unable to identify the Rufus++ executable for elevation"};
  }

  SHELLEXECUTEINFOW request{};
  request.cbSize = sizeof(request);
  request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  request.lpVerb = L"runas";
  request.lpFile = executable.c_str();
  request.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&request)) {
    const DWORD error = GetLastError();
    return {false, true,
            error == ERROR_CANCELLED
                ? "Administrator authorization was cancelled"
                : errorMessage("Unable to launch an elevated Rufus++ window", error)};
  }
  if (request.hProcess != nullptr) {
    CloseHandle(request.hProcess);
  }
  return {false, false,
          "An elevated Rufus++ window was opened. Continue the operation in that window."};
}

core::RawWriteResult WindowsBlockDeviceBackend::applyFfu(
    const core::ImageInfo& image, const core::BlockDeviceInfo& selectedTarget,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) const {
  core::RawWriteResult result;
  report(onProgress, core::RawWriteStage::Revalidating, 0,
         selectedTarget.capacityBytes);
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "FFU deployment was cancelled before the target was modified";
    return result;
  }
  const auto ready = ffuApplyAvailability(image, selectedTarget);
  if (!ready.available) {
    result.error = ready.reason;
    return result;
  }
  const auto diskNumber = physicalDriveNumber(selectedTarget.devicePath);
  if (!diskNumber.has_value()) {
    result.error = "Refusing an FFU target that is not a whole Windows PhysicalDrive path";
    return result;
  }
  const std::wstring sourcePath = utf8ToWide(image.path);
  const std::wstring targetPath = utf8ToWide(selectedTarget.devicePath);
  if (sourcePath.empty() || targetPath.empty()) {
    result.error = "The FFU source or target path is not valid UTF-8";
    return result;
  }

  // Keep this handle open without write/delete sharing for the whole DISM run.
  // DISM must use a pathname, so this prevents a validated file from being
  // replaced between our final analysis and the provider opening it.
  WindowsHandle sourceHandle(CreateFileW(
      sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!sourceHandle.valid()) {
    result.error = errorMessage("Unable to lock the FFU source image");
    return result;
  }
  BY_HANDLE_FILE_INFORMATION sourceInformation{};
  LARGE_INTEGER sourceSize{};
  if (GetFileType(sourceHandle.get()) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(sourceHandle.get(), &sourceInformation) ||
      (sourceInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      !GetFileSizeEx(sourceHandle.get(), &sourceSize) ||
      sourceSize.QuadPart <= 0 ||
      static_cast<std::uint64_t>(sourceSize.QuadPart) != image.sizeBytes) {
    result.error = "The opened FFU source is not the image file that was analyzed";
    return result;
  }
  const core::ImageAnalyzer analyzer;
  const auto reanalyzed = analyzer.analyze(std::filesystem::path(sourcePath));
  if (!reanalyzed.succeeded() ||
      reanalyzed.image->format != core::ImageFormat::Ffu ||
      !reanalyzed.image->capabilities.validContainerMetadata) {
    result.error = "The FFU source no longer passes structural validation";
    return result;
  }
  bool sourceOnTarget = false;
  if (!sourceUsesDisk(sourcePath, *diskNumber, sourceOnTarget, result.error)) {
    return result;
  }
  if (sourceOnTarget) {
    result.error = "The FFU source is stored on the selected target device";
    return result;
  }

  const DeviceDiscoveryResult beforeDismount = discover();
  const auto observed = std::find_if(
      beforeDismount.devices.begin(), beforeDismount.devices.end(),
      [&selectedTarget](const auto& device) {
        return device.devicePath == selectedTarget.devicePath;
      });
  if (observed == beforeDismount.devices.end()) {
    result.error = "The selected FFU target is no longer connected";
    return result;
  }
  const core::SafetyPolicy safetyPolicy;
  const auto identity = safetyPolicy.validateIdentity(selectedTarget, *observed);
  if (!identity.safe()) {
    result.error = identity.issues.front().message;
    return result;
  }

  report(onProgress, core::RawWriteStage::Unmounting, 0,
         selectedTarget.capacityBytes);
  LockedVolumes lockedVolumes;
  if (!lockedVolumes.acquire(*diskNumber, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "FFU deployment was cancelled after dismounting but before writing";
    return result;
  }
  const DeviceDiscoveryResult afterDismount = discover();
  const auto reobserved = std::find_if(
      afterDismount.devices.begin(), afterDismount.devices.end(),
      [&selectedTarget](const auto& device) {
        return device.devicePath == selectedTarget.devicePath;
      });
  if (reobserved == afterDismount.devices.end()) {
    result.error = "The FFU target disappeared after its volumes were dismounted";
    return result;
  }
  const auto finalIdentity =
      safetyPolicy.validateIdentity(selectedTarget, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  // DISM needs to acquire the physical drive itself. Retain the source lock,
  // but release our volume locks after the identity check and dismount.
  lockedVolumes.release();
  const auto dism = findWindowsExecutable(L"dism.exe");
  result = runDismFfuApply(dism, sourcePath, targetPath,
                           selectedTarget.capacityBytes, onProgress,
                           isCancelled);
  if (!result.success) {
    return result;
  }

  report(onProgress, core::RawWriteStage::Flushing,
         selectedTarget.capacityBytes, selectedTarget.capacityBytes);
  WindowsHandle refreshed(CreateFileW(
      targetPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, 0, nullptr));
  DWORD returned = 0;
  if (!refreshed.valid() ||
      !DeviceIoControl(refreshed.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr,
                       0, nullptr, 0, &returned, nullptr)) {
    result.success = false;
    result.error = errorMessage(
        "The FFU was applied, but Windows could not refresh the disk layout");
    return result;
  }
  report(onProgress, core::RawWriteStage::Complete,
         selectedTarget.capacityBytes, selectedTarget.capacityBytes);
  return result;
}

core::RawWriteResult WindowsBlockDeviceBackend::writeRaw(
    const core::RawWritePlan& plan,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) const {
  core::RawWriteResult result;
  const std::uint64_t total = plan.bytesToWrite();
  report(onProgress, core::RawWriteStage::Revalidating, 0, total);

  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled before the target was modified";
    return result;
  }
  if (!processIsElevated()) {
    result.error = "Administrator raw-device access is required";
    return result;
  }
  const auto diskNumber = physicalDriveNumber(plan.target().devicePath);
  if (!diskNumber.has_value()) {
    result.error = "Refusing a target that is not a whole Windows PhysicalDrive path";
    return result;
  }
  const auto sourceSafety = core::WritePlanBuilder::validateSource(plan);
  if (!sourceSafety.safe()) {
    result.error = sourceSafety.issues.front().message;
    return result;
  }

  const std::wstring sourcePath = utf8ToWide(plan.image().path);
  if (sourcePath.empty()) {
    result.error = "The source image path is not valid UTF-8";
    return result;
  }
  WindowsHandle sourceHandle(CreateFileW(
      sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!sourceHandle.valid()) {
    result.error = errorMessage("Unable to open the source image");
    return result;
  }
  BY_HANDLE_FILE_INFORMATION sourceInformation{};
  LARGE_INTEGER sourceSize{};
  if (GetFileType(sourceHandle.get()) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(sourceHandle.get(), &sourceInformation) ||
      (sourceInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      !GetFileSizeEx(sourceHandle.get(), &sourceSize) || sourceSize.QuadPart < 0 ||
      static_cast<std::uint64_t>(sourceSize.QuadPart) != plan.image().sizeBytes) {
    result.error = "The opened source is not the regular image file that was analyzed";
    return result;
  }
  bool sourceOnTarget = false;
  if (!sourceUsesDisk(sourcePath, *diskNumber, sourceOnTarget, result.error)) {
    return result;
  }
  if (sourceOnTarget) {
    result.error = "The source image is stored on the selected target device";
    return result;
  }
  auto openedSource = core::openRawImageSource(plan.image());
  if (!openedSource.succeeded()) {
    result.error = openedSource.error.empty()
                       ? "Unable to open the source image"
                       : std::move(openedSource.error);
    return result;
  }

  const DeviceDiscoveryResult beforeDismount = discover();
  const auto observed = std::find_if(
      beforeDismount.devices.begin(), beforeDismount.devices.end(), [&plan](const auto& device) {
        return device.devicePath == plan.target().devicePath;
      });
  if (observed == beforeDismount.devices.end()) {
    result.error = "The selected target is no longer connected";
    return result;
  }
  const auto targetSafety = detail::validateRawWriteTarget(plan, *observed);
  if (!targetSafety.safe()) {
    result.error = targetSafety.issues.front().message;
    return result;
  }

  report(onProgress, core::RawWriteStage::Claiming, 0, total);
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled before the target was modified";
    return result;
  }
  LockedVolumes lockedVolumes;
  report(onProgress, core::RawWriteStage::Unmounting, 0, total);
  if (!lockedVolumes.acquire(*diskNumber, result.error)) {
    return result;
  }
  if (cancelled(isCancelled)) {
    result.cancelled = true;
    result.error = "Write cancelled after dismounting but before modifying the target";
    return result;
  }

  const DeviceDiscoveryResult afterDismount = discover();
  const auto reobserved = std::find_if(
      afterDismount.devices.begin(), afterDismount.devices.end(), [&plan](const auto& device) {
        return device.devicePath == plan.target().devicePath;
      });
  if (reobserved == afterDismount.devices.end()) {
    result.error = "The target disappeared after its volumes were dismounted";
    return result;
  }
  const auto finalIdentity = detail::validateRawWriteTarget(plan, *reobserved);
  if (!finalIdentity.safe()) {
    result.error = finalIdentity.issues.front().message;
    return result;
  }

  report(onProgress, core::RawWriteStage::Opening, 0, total);
  const std::wstring targetPath = utf8ToWide(plan.target().devicePath);
  WindowsHandle targetHandle(CreateFileW(
      targetPath.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!targetHandle.valid()) {
    result.error = errorMessage("Unable to open the physical disk for raw access");
    return result;
  }
  STORAGE_DEVICE_NUMBER openedNumber{};
  DWORD returned = 0;
  if (!DeviceIoControl(targetHandle.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER,
                       nullptr, 0, &openedNumber, sizeof(openedNumber), &returned,
                       nullptr) ||
      openedNumber.DeviceType != FILE_DEVICE_DISK ||
      openedNumber.DeviceNumber != *diskNumber) {
    result.error = "The opened physical disk identity does not match the selected target";
    return result;
  }
  if (!DeviceIoControl(targetHandle.get(), IOCTL_DISK_IS_WRITABLE, nullptr, 0,
                       nullptr, 0, &returned, nullptr)) {
    result.error = GetLastError() == ERROR_WRITE_PROTECT
                       ? "The opened physical disk reports that it is read-only"
                       : errorMessage("Unable to confirm physical-disk writability");
    return result;
  }
  const auto geometry = queryRawGeometry(targetHandle.get(), result.error);
  if (!geometry.has_value()) {
    return result;
  }
  if (geometry->capacity != plan.target().capacityBytes ||
      geometry->sectorSize != plan.target().logicalSectorSize) {
    result.error = "Physical-disk geometry changed after the device was selected";
    return result;
  }

  WindowsRawTarget target(targetHandle.get(), geometry->capacity,
                          geometry->sectorSize);
  const core::RawImageWriter writer;
  return detail::finishNativeWrite(plan,
      [&](const auto& progress) {
        return writer.write(plan, *openedSource.source, target, progress, isCancelled);
      }, [&](std::string& error) {
        const auto safety = core::WritePlanBuilder::validateSource(plan);
        if (!safety.safe()) {
          error = safety.issues.front().message;
          return false;
        }
        if (!DeviceIoControl(targetHandle.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr,
                             0, nullptr, 0, &returned, nullptr)) {
          error = errorMessage(
              "The image was written and verified, but Windows could not refresh the disk layout");
          return false;
        }
        return true;
      }, onProgress);
}

}  // namespace

std::unique_ptr<BlockDeviceBackend> makeWindowsBlockDeviceBackend() {
  return std::make_unique<WindowsBlockDeviceBackend>();
}

}  // namespace rufus::backend
