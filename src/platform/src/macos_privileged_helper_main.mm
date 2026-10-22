/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "macos_privileged_helper_protocol.hpp"
#include "macos_write_ranges.hpp"
#include "macos_installer_operations.hpp"
#include "macos_raw_device_operations.hpp"
#include "raw_write_safety.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/write_plan.hpp"

namespace {

namespace core = rufus::core;
namespace macos = rufus::backend::macos;
namespace wire = rufus::backend::macos::protocol;

constexpr bool kSigningConfigured = sizeof(RUFUSPP_MACOS_TEAM_IDENTIFIER) > 1;

class FileDescriptor final {
 public:
  explicit FileDescriptor(const int value = -1) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

 private:
  int value_;
};

struct Operation final {
  std::string id;
  std::atomic_bool cancelled{false};
};

std::mutex operationMutex;
std::shared_ptr<Operation> activeOperation;

std::string appRequirement() {
  return "anchor apple generic and identifier \"" + std::string(RUFUSPP_MACOS_APP_IDENTIFIER) +
         "\" and certificate leaf[subject.OU] = \"" +
         std::string(RUFUSPP_MACOS_TEAM_IDENTIFIER) + "\"";
}

bool isString(xpc_object_t request, const char* key) {
  xpc_object_t value = xpc_dictionary_get_value(request, key);
  return value != nullptr && xpc_get_type(value) == XPC_TYPE_STRING;
}

bool isUnsignedInteger(xpc_object_t request, const char* key) {
  xpc_object_t value = xpc_dictionary_get_value(request, key);
  return value != nullptr && xpc_get_type(value) == XPC_TYPE_UINT64;
}

bool isBoolean(xpc_object_t request, const char* key) {
  xpc_object_t value = xpc_dictionary_get_value(request, key);
  return value != nullptr && xpc_get_type(value) == XPC_TYPE_BOOL;
}

void setError(xpc_object_t reply, const std::string& error) {
  xpc_dictionary_set_int64(reply, wire::kVersionKey, wire::kVersion);
  xpc_dictionary_set_bool(reply, wire::kSuccess, false);
  xpc_dictionary_set_string(reply, wire::kError, error.c_str());
}

void sendError(xpc_connection_t peer, xpc_object_t request, const std::string& error) {
  xpc_object_t reply = xpc_dictionary_create_reply(request);
  if (reply != nullptr) {
    setError(reply, error);
    xpc_connection_send_message(peer, reply);
  }
}

bool sameFile(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_mode == right.st_mode && left.st_size == right.st_size &&
         left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
         left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec;
}

core::RawWriteResult executeWrite(xpc_object_t request,
                                  const std::shared_ptr<Operation>& operation,
                                  xpc_connection_t peer) {
  core::RawWriteResult result;
  if (!isString(request, wire::kSourceName) ||
      !isString(request, wire::kTargetPath) ||
      !isString(request, wire::kTargetStableId) ||
      !isUnsignedInteger(request, wire::kSourceSize) ||
      !isUnsignedInteger(request, wire::kTargetCapacity) ||
      !isUnsignedInteger(request, wire::kTargetSectorSize) ||
      !isUnsignedInteger(request, wire::kBlockSize) ||
      !isBoolean(request, wire::kVerify) ||
      !isUnsignedInteger(request, wire::kVerificationProfile) ||
      !isBoolean(request, wire::kClearTargetTailMetadata)) {
    result.error = "The write request is missing required typed fields";
    return result;
  }

  std::vector<core::RawWriteRange> writeRanges;
  if (!wire::decodeWriteRanges(request, writeRanges, result.error)) return result;
  const char* sourceName = xpc_dictionary_get_string(request, wire::kSourceName);
  const char* targetPath = xpc_dictionary_get_string(request, wire::kTargetPath);
  const char* stableId = xpc_dictionary_get_string(request, wire::kTargetStableId);
  const std::uint64_t sourceSize = xpc_dictionary_get_uint64(request, wire::kSourceSize);
  const std::uint64_t targetCapacity =
      xpc_dictionary_get_uint64(request, wire::kTargetCapacity);
  const std::uint64_t sectorSize =
      xpc_dictionary_get_uint64(request, wire::kTargetSectorSize);
  const std::uint64_t blockSize = xpc_dictionary_get_uint64(request, wire::kBlockSize);
  const std::uint64_t verificationProfileValue =
      xpc_dictionary_get_uint64(request, wire::kVerificationProfile);
  if (sourceName == nullptr || targetPath == nullptr || stableId == nullptr ||
      sourceSize == 0 || targetCapacity == 0 || sectorSize == 0 ||
      sectorSize > std::numeric_limits<std::uint32_t>::max() ||
      blockSize > std::numeric_limits<std::size_t>::max()) {
    result.error = "The write request contains invalid bounds";
    return result;
  }
  if (verificationProfileValue <
          static_cast<std::uint64_t>(core::VerificationProfile::Fast) ||
      verificationProfileValue >
          static_cast<std::uint64_t>(core::VerificationProfile::Full)) {
    result.error = "The write request contains an invalid verification profile";
    return result;
  }
  const std::string sourceNameValue(sourceName);
  if (sourceNameValue.empty() || sourceNameValue.size() > 255 ||
      sourceNameValue.find('/') != std::string::npos ||
      sourceNameValue.find('\\') != std::string::npos) {
    result.error = "The source display name is invalid";
    return result;
  }
  if (!xpc_dictionary_get_bool(request, wire::kVerify)) {
    result.error = "The privileged helper requires post-write verification";
    return result;
  }

  FileDescriptor source(xpc_dictionary_dup_fd(request, wire::kSourceDescriptor));
  if (!source.valid()) {
    result.error = "The write request did not provide a readable source descriptor";
    return result;
  }
  struct stat descriptorStat {};
  if (fstat(source.get(), &descriptorStat) != 0 || !S_ISREG(descriptorStat.st_mode) ||
      descriptorStat.st_size < 0 ||
      static_cast<std::uint64_t>(descriptorStat.st_size) != sourceSize) {
    result.error = "The source descriptor is not the expected regular image file";
    return result;
  }

  const std::string descriptorPath = "/dev/fd/" + std::to_string(source.get());
  const core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(descriptorPath, sourceNameValue);
  struct stat analyzedStat {};
  if (!analysis.succeeded() || fstat(source.get(), &analyzedStat) != 0 ||
      !sameFile(descriptorStat, analyzedStat) || analysis.image->sizeBytes != sourceSize) {
    result.error = analysis.succeeded()
                       ? "The source image changed while the helper re-analyzed it"
                       : "The helper rejected the source image: " + analysis.error;
    return result;
  }

  const auto discovery = macos::discoverBlockDevices();
  const auto found = std::find_if(
      discovery.devices.begin(), discovery.devices.end(), [targetPath](const auto& device) {
        return device.devicePath == targetPath;
      });
  if (found == discovery.devices.end()) {
    result.error = "The selected target is no longer connected";
    return result;
  }
  if (found->stableId != stableId || found->capacityBytes != targetCapacity ||
      found->logicalSectorSize != sectorSize) {
    result.error = "The selected target identity changed before authorization";
    return result;
  }

  const core::WritePlanBuilder builder;
  auto planning = builder.buildRawWriteWithVerification(
      *analysis.image, *found,
      static_cast<core::VerificationProfile>(verificationProfileValue),
      static_cast<std::size_t>(blockSize),
      xpc_dictionary_get_bool(request, wire::kClearTargetTailMetadata),
      std::move(writeRanges));
  if (!planning.succeeded()) {
    result.error = planning.issues.empty() ? "The privileged write plan was rejected"
                                           : planning.issues.front().message;
    return result;
  }

  auto openedSource = core::openRawImageSource(*analysis.image,
                                               descriptorPath);
  if (!openedSource.succeeded()) {
    result.error = openedSource.error.empty()
                       ? "Unable to open the authorized source image"
                       : std::move(openedSource.error);
    return result;
  }
  const core::RawWriteProgressCallback onProgress =
      [peer, operation](const core::RawWriteProgress& progress) {
        xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_int64(message, wire::kVersionKey, wire::kVersion);
        xpc_dictionary_set_string(message, wire::kMessageType, wire::kMessageProgress);
        xpc_dictionary_set_string(message, wire::kOperationId, operation->id.c_str());
        xpc_dictionary_set_int64(message, wire::kStage,
                                 static_cast<std::int64_t>(progress.stage));
        xpc_dictionary_set_uint64(message, wire::kProcessed, progress.bytesProcessed);
        xpc_dictionary_set_uint64(message, wire::kTotal, progress.totalBytes);
        xpc_connection_send_message(peer, message);
      };
  return rufus::backend::detail::finishNativeWrite(*planning.plan,
      [&](const core::RawWriteProgressCallback& progress) {
        return macos::writeRawLocally(*planning.plan, openedSource.source.get(), progress,
            [operation] { return operation->cancelled.load(); });
      }, [&](std::string& error) {
        struct stat finalStat {};
        if (fstat(source.get(), &finalStat) != 0 || !sameFile(descriptorStat, finalStat)) {
          error = "The source image changed during the write";
          return false;
        }
        return true;
      }, onProgress);
}

core::BadBlockTestResult executeBadBlockTest(
    xpc_object_t request, const std::shared_ptr<Operation>& operation,
    xpc_connection_t peer) {
  core::BadBlockTestResult result;
  if (!isString(request, wire::kTargetPath) ||
      !isString(request, wire::kTargetStableId) ||
      !isUnsignedInteger(request, wire::kTargetCapacity) ||
      !isUnsignedInteger(request, wire::kTargetSectorSize) ||
      !isUnsignedInteger(request, wire::kBlockSize) ||
      !isUnsignedInteger(request, wire::kPassCount) ||
      !isUnsignedInteger(request, wire::kMaximumReportedOffsets)) {
    result.error = "The bad-block request is missing required typed fields";
    return result;
  }
  const char* targetPath =
      xpc_dictionary_get_string(request, wire::kTargetPath);
  const char* stableId =
      xpc_dictionary_get_string(request, wire::kTargetStableId);
  const std::uint64_t targetCapacity =
      xpc_dictionary_get_uint64(request, wire::kTargetCapacity);
  const std::uint64_t sectorSize =
      xpc_dictionary_get_uint64(request, wire::kTargetSectorSize);
  const std::uint64_t transferBytes =
      xpc_dictionary_get_uint64(request, wire::kBlockSize);
  const std::uint64_t passCount =
      xpc_dictionary_get_uint64(request, wire::kPassCount);
  const std::uint64_t maximumOffsets =
      xpc_dictionary_get_uint64(request, wire::kMaximumReportedOffsets);
  if (targetPath == nullptr || stableId == nullptr || targetCapacity == 0U ||
      sectorSize == 0U ||
      sectorSize > std::numeric_limits<std::uint32_t>::max() ||
      transferBytes > std::numeric_limits<std::size_t>::max() ||
      passCount == 0U || passCount > 4U || maximumOffsets > 4096U) {
    result.error = "The bad-block request contains invalid bounds";
    return result;
  }
  const auto discovery = macos::discoverBlockDevices();
  const auto found = std::find_if(
      discovery.devices.begin(), discovery.devices.end(),
      [targetPath](const auto& device) {
        return device.devicePath == targetPath;
      });
  if (found == discovery.devices.end()) {
    result.error = "The selected test device is no longer connected";
    return result;
  }
  if (found->stableId != stableId || found->capacityBytes != targetCapacity ||
      found->logicalSectorSize != sectorSize) {
    result.error = "The selected test-device identity changed before authorization";
    return result;
  }
  const core::BadBlockTestOptions options{
      static_cast<unsigned int>(passCount),
      static_cast<std::size_t>(transferBytes),
      static_cast<std::size_t>(maximumOffsets)};
  return macos::testBadBlocksLocally(
      *found, options,
      [peer, operation](const core::BadBlockTestProgress& progress) {
        xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_int64(message, wire::kVersionKey, wire::kVersion);
        xpc_dictionary_set_string(message, wire::kMessageType,
                                  wire::kMessageProgress);
        xpc_dictionary_set_string(message, wire::kOperationId,
                                  operation->id.c_str());
        xpc_dictionary_set_int64(message, wire::kStage,
                                 static_cast<std::int64_t>(progress.stage));
        xpc_dictionary_set_uint64(message, wire::kPass, progress.pass);
        xpc_dictionary_set_uint64(message, wire::kPassCount,
                                  progress.passCount);
        xpc_dictionary_set_uint64(message, wire::kProcessed,
                                  progress.bytesProcessed);
        xpc_dictionary_set_uint64(message, wire::kTotal,
                                  progress.totalBytes);
        xpc_connection_send_message(peer, message);
      },
      [operation] { return operation->cancelled.load(); });
}

core::MediaCaptureResult executeCapture(
    xpc_object_t request, const std::shared_ptr<Operation>& operation,
    xpc_connection_t peer) {
  core::MediaCaptureResult result;
  if (!isString(request, wire::kTargetPath) ||
      !isString(request, wire::kTargetStableId) ||
      !isUnsignedInteger(request, wire::kTargetCapacity) ||
      !isUnsignedInteger(request, wire::kTargetSectorSize) ||
      !isUnsignedInteger(request, wire::kBlockSize) ||
      !isBoolean(request, wire::kVerify)) {
    result.error = "The capture request is missing required typed fields";
    return result;
  }
  const char* targetPath =
      xpc_dictionary_get_string(request, wire::kTargetPath);
  const char* stableId =
      xpc_dictionary_get_string(request, wire::kTargetStableId);
  const std::uint64_t targetCapacity =
      xpc_dictionary_get_uint64(request, wire::kTargetCapacity);
  const std::uint64_t sectorSize =
      xpc_dictionary_get_uint64(request, wire::kTargetSectorSize);
  const std::uint64_t transferBytes =
      xpc_dictionary_get_uint64(request, wire::kBlockSize);
  if (targetPath == nullptr || stableId == nullptr || targetCapacity == 0U ||
      sectorSize == 0U ||
      sectorSize > std::numeric_limits<std::uint32_t>::max() ||
      transferBytes < 4096U || transferBytes > 64U * 1024U * 1024U ||
      transferBytes > std::numeric_limits<std::size_t>::max()) {
    result.error = "The capture request contains invalid bounds";
    return result;
  }
  FileDescriptor destination(
      xpc_dictionary_dup_fd(request, wire::kDestinationDescriptor));
  struct stat destinationStatus {};
  if (!destination.valid() || fstat(destination.get(), &destinationStatus) != 0 ||
      !S_ISREG(destinationStatus.st_mode) || destinationStatus.st_size != 0 ||
      destinationStatus.st_nlink != 1) {
    result.error = "The capture request did not provide a new regular destination file";
    return result;
  }
  const auto discovery = macos::discoverBlockDevices();
  const auto found = std::find_if(
      discovery.devices.begin(), discovery.devices.end(),
      [targetPath](const auto& device) {
        return device.devicePath == targetPath;
      });
  if (found == discovery.devices.end()) {
    result.error = "The selected capture source is no longer connected";
    return result;
  }
  if (found->stableId != stableId || found->capacityBytes != targetCapacity ||
      found->logicalSectorSize != sectorSize) {
    result.error = "The selected capture-source identity changed before authorization";
    return result;
  }
  const core::MediaCaptureOptions options{
      core::MediaCaptureFormat::Raw,
      static_cast<std::size_t>(transferBytes),
      xpc_dictionary_get_bool(request, wire::kVerify)};
  return macos::captureRawLocally(
      *found, destination.get(), options,
      [peer, operation](const core::MediaCaptureProgress& progress) {
        xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_int64(message, wire::kVersionKey, wire::kVersion);
        xpc_dictionary_set_string(message, wire::kMessageType,
                                  wire::kMessageProgress);
        xpc_dictionary_set_string(message, wire::kOperationId,
                                  operation->id.c_str());
        xpc_dictionary_set_int64(message, wire::kStage,
                                 static_cast<std::int64_t>(progress.stage));
        xpc_dictionary_set_uint64(message, wire::kProcessed,
                                  progress.bytesProcessed);
        xpc_dictionary_set_uint64(message, wire::kTotal,
                                  progress.totalBytes);
        xpc_connection_send_message(peer, message);
      },
      [operation] { return operation->cancelled.load(); });
}

core::RawWriteResult executeMacOsInstaller(
    xpc_object_t request, const std::shared_ptr<Operation>& operation,
    xpc_connection_t peer) {
  core::RawWriteResult result;
  if (!isString(request, wire::kInstallerApplicationPath) ||
      !isString(request, wire::kInstallerToolPath) ||
      !isString(request, wire::kSourceName) ||
      !isString(request, wire::kInstallerVersion) ||
      !isString(request, wire::kInstallerBuild) ||
      !isUnsignedInteger(request, wire::kInstallerPayloadSize) ||
      !isBoolean(request, wire::kFullWipe) ||
      !isString(request, wire::kTargetPath) ||
      !isString(request, wire::kTargetStableId) ||
      !isUnsignedInteger(request, wire::kTargetCapacity) ||
      !isUnsignedInteger(request, wire::kTargetSectorSize)) {
    result.error = "The macOS installer request is missing required typed fields";
    return result;
  }
  const char* applicationPath =
      xpc_dictionary_get_string(request, wire::kInstallerApplicationPath);
  const char* toolPath =
      xpc_dictionary_get_string(request, wire::kInstallerToolPath);
  const char* displayName =
      xpc_dictionary_get_string(request, wire::kSourceName);
  const char* version =
      xpc_dictionary_get_string(request, wire::kInstallerVersion);
  const char* build = xpc_dictionary_get_string(request, wire::kInstallerBuild);
  const char* targetPath = xpc_dictionary_get_string(request, wire::kTargetPath);
  const char* stableId =
      xpc_dictionary_get_string(request, wire::kTargetStableId);
  const std::uint64_t targetCapacity =
      xpc_dictionary_get_uint64(request, wire::kTargetCapacity);
  const std::uint64_t targetSectorSize =
      xpc_dictionary_get_uint64(request, wire::kTargetSectorSize);
  if (applicationPath == nullptr || toolPath == nullptr ||
      displayName == nullptr || version == nullptr || build == nullptr ||
      targetPath == nullptr || stableId == nullptr ||
      std::strlen(applicationPath) < 5U || std::strlen(applicationPath) > 4096U ||
      std::strlen(toolPath) < 5U || std::strlen(toolPath) > 4096U ||
      std::strlen(displayName) > 512U || std::strlen(version) > 128U ||
      std::strlen(build) > 128U || std::strlen(targetPath) > 128U ||
      std::strlen(stableId) > 1024U || targetCapacity == 0U ||
      targetSectorSize == 0U ||
      targetSectorSize > std::numeric_limits<std::uint32_t>::max()) {
    result.error = "The macOS installer request contains invalid bounds";
    return result;
  }

  const auto discovery = macos::discoverBlockDevices();
  const auto found = std::find_if(
      discovery.devices.begin(), discovery.devices.end(),
      [targetPath](const auto& device) {
        return device.devicePath == targetPath;
      });
  if (found == discovery.devices.end()) {
    result.error = "The selected macOS installer target is no longer connected";
    return result;
  }
  if (found->stableId != stableId || found->capacityBytes != targetCapacity ||
      found->logicalSectorSize != targetSectorSize) {
    result.error = "The selected macOS installer target identity changed before authorization";
    return result;
  }

  core::MacOsInstallerInfo installer;
  installer.applicationPath = applicationPath;
  installer.createInstallMediaPath = toolPath;
  installer.displayName = displayName;
  installer.version = version;
  installer.build = build;
  installer.payloadSizeBytes =
      xpc_dictionary_get_uint64(request, wire::kInstallerPayloadSize);
  return macos::createInstallerLocally(
      installer, *found, xpc_dictionary_get_bool(request, wire::kFullWipe),
      operation->id,
      [peer, operation](const core::MacOsInstallerProgress& progress) {
        xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_int64(message, wire::kVersionKey, wire::kVersion);
        xpc_dictionary_set_string(message, wire::kMessageType,
                                  wire::kMessageProgress);
        xpc_dictionary_set_string(message, wire::kOperationId,
                                  operation->id.c_str());
        xpc_dictionary_set_int64(message, wire::kStage,
                                 static_cast<std::int64_t>(progress.stage));
        xpc_dictionary_set_uint64(message, wire::kProcessed,
                                  progress.bytesProcessed);
        xpc_dictionary_set_uint64(message, wire::kTotal,
                                  progress.totalBytes);
        xpc_dictionary_set_string(message, wire::kSourceName,
                                  progress.detail.c_str());
        xpc_connection_send_message(peer, message);
      },
      [operation] { return operation->cancelled.load(); });
}

void handleWrite(xpc_connection_t peer, xpc_object_t request) {
  if (!isString(request, wire::kOperationId)) {
    sendError(peer, request, "The write request has no operation identifier");
    return;
  }
  const char* identifier = xpc_dictionary_get_string(request, wire::kOperationId);
  if (identifier == nullptr || std::strlen(identifier) < 8 || std::strlen(identifier) > 128) {
    sendError(peer, request, "The operation identifier is invalid");
    return;
  }

  auto operation = std::make_shared<Operation>();
  operation->id = identifier;
  {
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation) {
      sendError(peer, request, "Another destructive operation is already active");
      return;
    }
    activeOperation = operation;
  }

  xpc_object_t reply = xpc_dictionary_create_reply(request);
  if (reply == nullptr) {
    std::lock_guard<std::mutex> lock(operationMutex);
    activeOperation.reset();
    return;
  }

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const core::RawWriteResult result = executeWrite(request, operation, peer);
    xpc_dictionary_set_int64(reply, wire::kVersionKey, wire::kVersion);
    xpc_dictionary_set_bool(reply, wire::kSuccess, result.success);
    xpc_dictionary_set_bool(reply, wire::kCancelled, result.cancelled);
    xpc_dictionary_set_bool(reply, wire::kWriteStarted, result.destructiveWriteStarted);
    xpc_dictionary_set_uint64(reply, wire::kBytesWritten, result.bytesWritten);
    xpc_dictionary_set_uint64(reply, wire::kBytesVerified,
                              result.bytesVerified);
    xpc_dictionary_set_bool(reply, wire::kVerificationCompleted,
                            result.verificationCompleted);
    xpc_dictionary_set_string(reply, wire::kError, result.error.c_str());
    xpc_connection_send_message(peer, reply);
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation == operation) {
      activeOperation.reset();
    }
  });
}

void handleMacOsInstaller(xpc_connection_t peer, xpc_object_t request) {
  if (!isString(request, wire::kOperationId)) {
    sendError(peer, request,
              "The macOS installer request has no operation identifier");
    return;
  }
  const char* identifier =
      xpc_dictionary_get_string(request, wire::kOperationId);
  if (identifier == nullptr || std::strlen(identifier) < 8U ||
      std::strlen(identifier) > 128U) {
    sendError(peer, request, "The operation identifier is invalid");
    return;
  }
  auto operation = std::make_shared<Operation>();
  operation->id = identifier;
  {
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation) {
      sendError(peer, request, "Another destructive operation is already active");
      return;
    }
    activeOperation = operation;
  }
  xpc_object_t reply = xpc_dictionary_create_reply(request);
  if (reply == nullptr) {
    std::lock_guard<std::mutex> lock(operationMutex);
    activeOperation.reset();
    return;
  }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const core::RawWriteResult result =
        executeMacOsInstaller(request, operation, peer);
    xpc_dictionary_set_int64(reply, wire::kVersionKey, wire::kVersion);
    xpc_dictionary_set_bool(reply, wire::kSuccess, result.success);
    xpc_dictionary_set_bool(reply, wire::kCancelled, result.cancelled);
    xpc_dictionary_set_bool(reply, wire::kWriteStarted,
                            result.destructiveWriteStarted);
    xpc_dictionary_set_uint64(reply, wire::kBytesWritten,
                              result.bytesWritten);
    xpc_dictionary_set_uint64(reply, wire::kBytesVerified,
                              result.bytesVerified);
    xpc_dictionary_set_bool(reply, wire::kVerificationCompleted,
                            result.verificationCompleted);
    xpc_dictionary_set_string(reply, wire::kError, result.error.c_str());
    xpc_connection_send_message(peer, reply);
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation == operation) {
      activeOperation.reset();
    }
  });
}

void handleBadBlockTest(xpc_connection_t peer, xpc_object_t request) {
  if (!isString(request, wire::kOperationId)) {
    sendError(peer, request,
              "The bad-block request has no operation identifier");
    return;
  }
  const char* identifier =
      xpc_dictionary_get_string(request, wire::kOperationId);
  if (identifier == nullptr || std::strlen(identifier) < 8U ||
      std::strlen(identifier) > 128U) {
    sendError(peer, request, "The operation identifier is invalid");
    return;
  }
  auto operation = std::make_shared<Operation>();
  operation->id = identifier;
  {
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation) {
      sendError(peer, request, "Another destructive operation is already active");
      return;
    }
    activeOperation = operation;
  }
  xpc_object_t reply = xpc_dictionary_create_reply(request);
  if (reply == nullptr) {
    std::lock_guard<std::mutex> lock(operationMutex);
    activeOperation.reset();
    return;
  }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const core::BadBlockTestResult result =
        executeBadBlockTest(request, operation, peer);
    xpc_dictionary_set_int64(reply, wire::kVersionKey, wire::kVersion);
    xpc_dictionary_set_bool(reply, wire::kCompleted, result.completed);
    xpc_dictionary_set_bool(reply, wire::kSuccess, result.success);
    xpc_dictionary_set_bool(reply, wire::kCancelled, result.cancelled);
    xpc_dictionary_set_bool(reply, wire::kWriteStarted,
                            result.destructiveWriteStarted);
    xpc_dictionary_set_uint64(reply, wire::kBytesTested,
                              result.bytesTested);
    xpc_dictionary_set_uint64(reply, wire::kBadSectorCount,
                              result.badSectorCount);
    if (!result.firstBadSectorOffsets.empty()) {
      xpc_dictionary_set_data(
          reply, wire::kBadSectorOffsets,
          result.firstBadSectorOffsets.data(),
          result.firstBadSectorOffsets.size() * sizeof(std::uint64_t));
    }
    xpc_dictionary_set_string(reply, wire::kError, result.error.c_str());
    xpc_connection_send_message(peer, reply);
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation == operation) {
      activeOperation.reset();
    }
  });
}

void handleCapture(xpc_connection_t peer, xpc_object_t request) {
  if (!isString(request, wire::kOperationId)) {
    sendError(peer, request, "The capture request has no operation identifier");
    return;
  }
  const char* identifier =
      xpc_dictionary_get_string(request, wire::kOperationId);
  if (identifier == nullptr || std::strlen(identifier) < 8U ||
      std::strlen(identifier) > 128U) {
    sendError(peer, request, "The operation identifier is invalid");
    return;
  }
  auto operation = std::make_shared<Operation>();
  operation->id = identifier;
  {
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation) {
      sendError(peer, request, "Another destructive operation is already active");
      return;
    }
    activeOperation = operation;
  }
  xpc_object_t reply = xpc_dictionary_create_reply(request);
  if (reply == nullptr) {
    std::lock_guard<std::mutex> lock(operationMutex);
    activeOperation.reset();
    return;
  }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const core::MediaCaptureResult result =
        executeCapture(request, operation, peer);
    xpc_dictionary_set_int64(reply, wire::kVersionKey, wire::kVersion);
    xpc_dictionary_set_bool(reply, wire::kSuccess, result.success);
    xpc_dictionary_set_bool(reply, wire::kCancelled, result.cancelled);
    xpc_dictionary_set_uint64(reply, wire::kBytesWritten,
                              result.bytesCaptured);
    xpc_dictionary_set_uint64(reply, wire::kBytesTested,
                              result.outputSizeBytes);
    xpc_dictionary_set_string(reply, wire::kError, result.error.c_str());
    xpc_connection_send_message(peer, reply);
    std::lock_guard<std::mutex> lock(operationMutex);
    if (activeOperation == operation) {
      activeOperation.reset();
    }
  });
}

void handleCancel(xpc_object_t request) {
  if (!isString(request, wire::kOperationId)) {
    return;
  }
  const char* identifier = xpc_dictionary_get_string(request, wire::kOperationId);
  std::lock_guard<std::mutex> lock(operationMutex);
  if (identifier != nullptr && activeOperation && activeOperation->id == identifier) {
    activeOperation->cancelled.store(true);
  }
}

void handleMessage(xpc_connection_t peer, xpc_object_t request) {
  if (xpc_get_type(request) != XPC_TYPE_DICTIONARY ||
      xpc_dictionary_get_int64(request, wire::kVersionKey) != wire::kVersion ||
      !isString(request, wire::kCommand)) {
    sendError(peer, request, "Unsupported privileged-helper protocol request");
    return;
  }
  const char* command = xpc_dictionary_get_string(request, wire::kCommand);
  if (std::strcmp(command, wire::kCommandPing) == 0) {
    xpc_object_t reply = xpc_dictionary_create_reply(request);
    if (reply != nullptr) {
      xpc_dictionary_set_int64(reply, wire::kVersionKey, wire::kVersion);
      xpc_dictionary_set_bool(reply, wire::kSuccess, true);
      xpc_connection_send_message(peer, reply);
    }
  } else if (std::strcmp(command, wire::kCommandWrite) == 0) {
    handleWrite(peer, request);
  } else if (std::strcmp(command, wire::kCommandBadBlockTest) == 0) {
    handleBadBlockTest(peer, request);
  } else if (std::strcmp(command, wire::kCommandCapture) == 0) {
    handleCapture(peer, request);
  } else if (std::strcmp(command,
                         wire::kCommandCreateMacOsInstaller) == 0) {
    handleMacOsInstaller(peer, request);
  } else if (std::strcmp(command, wire::kCommandCancel) == 0) {
    handleCancel(request);
  } else {
    sendError(peer, request, "Unknown privileged-helper command");
  }
}

}  // namespace

int main() {
  @autoreleasepool {
    if (!kSigningConfigured || geteuid() != 0) {
      return 78;
    }

    xpc_connection_t listener = xpc_connection_create_mach_service(
        wire::kServiceName, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (listener == nullptr) {
      return 1;
    }
    const std::string requirement = appRequirement();
    if (xpc_connection_set_peer_code_signing_requirement(listener, requirement.c_str()) != 0) {
      return 78;
    }

    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
      if (xpc_get_type(event) != XPC_TYPE_CONNECTION) {
        return;
      }
      xpc_connection_t peer = static_cast<xpc_connection_t>(event);
      xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
        handleMessage(peer, message);
      });
      xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
  }
  dispatch_main();
}
