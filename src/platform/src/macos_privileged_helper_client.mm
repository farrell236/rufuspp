/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "macos_privileged_helper_client.hpp"

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>

#include "macos_privileged_helper_protocol.hpp"
#include "macos_write_ranges.hpp"

namespace rufus::backend::macos {

namespace {

namespace wire = protocol;

constexpr bool kSigningConfigured = sizeof(RUFUSPP_MACOS_TEAM_IDENTIFIER) > 1;

std::string nsString(NSString* value) {
  if (value == nil) {
    return {};
  }
  const char* utf8 = value.UTF8String;
  return utf8 == nullptr ? std::string{} : std::string(utf8);
}

NSString* daemonPlistName() {
  return [NSString stringWithUTF8String:wire::kDaemonPlistName];
}

std::string helperRequirement() {
  return "anchor apple generic and identifier \"" + std::string(RUFUSPP_MACOS_HELPER_IDENTIFIER) +
         "\" and certificate leaf[subject.OU] = \"" +
         std::string(RUFUSPP_MACOS_TEAM_IDENTIFIER) + "\"";
}

std::string xpcError(xpc_object_t object) {
  const char* description = xpc_dictionary_get_string(object, XPC_ERROR_KEY_DESCRIPTION);
  return description == nullptr ? "The privileged helper connection failed" : description;
}

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
  void reset() noexcept {
    if (value_ >= 0) {
      close(value_);
      value_ = -1;
    }
  }

 private:
  int value_;
};

RawWriteAvailability configurationUnavailable() {
  return {false, false,
          "This development build has no macOS Team ID. Configure "
          "RUFUSPP_MACOS_TEAM_IDENTIFIER and sign both the app and helper before enabling writes."};
}

}  // namespace

RawWriteAvailability privilegedHelperAvailability() {
  if (!kSigningConfigured) {
    return configurationUnavailable();
  }

  if (@available(macOS 13.0, *)) {
    SMAppService* service = [SMAppService daemonServiceWithPlistName:daemonPlistName()];
    switch (service.status) {
      case SMAppServiceStatusEnabled:
        return {true, false, {}};
      case SMAppServiceStatusNotRegistered:
        return {false, true,
                "Administrative disk access has not been registered. Select AUTHORIZE to begin."};
      case SMAppServiceStatusRequiresApproval:
        return {false, true,
                "Administrative disk access is awaiting approval in System Settings."};
      case SMAppServiceStatusNotFound:
        return {false, false,
                "The privileged helper is missing from this application bundle."};
    }
    return {false, false, "The privileged helper reported an unknown registration state."};
  }
  return {false, false, "Raw writing requires macOS 13 or later."};
}

RawWriteAvailability requestPrivilegedHelperAuthorization() {
  if (!kSigningConfigured) {
    return configurationUnavailable();
  }

  if (@available(macOS 13.0, *)) {
    SMAppService* service = [SMAppService daemonServiceWithPlistName:daemonPlistName()];
    if (service.status == SMAppServiceStatusEnabled) {
      return {true, false, {}};
    }
    if (service.status == SMAppServiceStatusRequiresApproval) {
      [SMAppService openSystemSettingsLoginItems];
      return {false, true,
              "Approve Rufus++ administrative disk access in System Settings, then return here."};
    }

    NSError* error = nil;
    if (![service registerAndReturnError:&error]) {
      const std::string detail = nsString(error.localizedDescription);
      return {false, true,
              detail.empty() ? "macOS declined privileged-helper registration" : detail};
    }

    if (service.status == SMAppServiceStatusRequiresApproval) {
      [SMAppService openSystemSettingsLoginItems];
      return {false, true,
              "Approve Rufus++ administrative disk access in System Settings, then return here."};
    }
    return privilegedHelperAvailability();
  }
  return {false, false, "Raw writing requires macOS 13 or later."};
}

core::RawWriteResult writeRawWithPrivilegedHelper(
    const core::RawWritePlan& plan,
    const core::RawWriteProgressCallback& onProgress,
    const core::RawWriteCancelCallback& isCancelled) {
  core::RawWriteResult result;
  const auto availability = privilegedHelperAvailability();
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }

  FileDescriptor source(open(plan.image().path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!source.valid()) {
    result.error = "Unable to open the selected source image: " + std::string(std::strerror(errno));
    return result;
  }
  struct stat sourceStat {};
  if (fstat(source.get(), &sourceStat) != 0 || !S_ISREG(sourceStat.st_mode) ||
      sourceStat.st_size < 0 ||
      static_cast<std::uint64_t>(sourceStat.st_size) != plan.image().sizeBytes) {
    result.error = "The opened source is not the unchanged regular image file that was analyzed";
    return result;
  }

  dispatch_queue_t queue = dispatch_queue_create("org.rufusplusplus.app.helper-client", DISPATCH_QUEUE_SERIAL);
  xpc_connection_t connection = xpc_connection_create_mach_service(
      wire::kServiceName, queue, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
  if (connection == nullptr) {
    result.error = "Unable to create the privileged helper connection";
    return result;
  }
  const std::string requirement = helperRequirement();
  const int requirementError =
      xpc_connection_set_peer_code_signing_requirement(connection, requirement.c_str());
  if (requirementError != 0) {
    result.error = "Invalid helper code-signing requirement: " +
                   std::string(std::strerror(requirementError));
    return result;
  }

  NSString* operationString = [NSUUID UUID].UUIDString;
  const std::string operationId = nsString(operationString);
  const core::RawWriteProgressCallback progressCallback = onProgress;
  const core::RawWriteCancelCallback cancelCallback = isCancelled;
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  __block core::RawWriteResult replyResult;
  __block bool receivedReply = false;

  xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_int64(event, wire::kVersionKey) != wire::kVersion) {
      return;
    }
    const char* type = xpc_dictionary_get_string(event, wire::kMessageType);
    const char* eventOperation = xpc_dictionary_get_string(event, wire::kOperationId);
    if (type == nullptr || eventOperation == nullptr || operationId != eventOperation ||
        std::strcmp(type, wire::kMessageProgress) != 0) {
      return;
    }
    const std::int64_t stage = xpc_dictionary_get_int64(event, wire::kStage);
    if (stage < static_cast<std::int64_t>(core::RawWriteStage::Revalidating) ||
        stage > static_cast<std::int64_t>(core::RawWriteStage::Complete)) {
      return;
    }
    if (progressCallback) {
      progressCallback({static_cast<core::RawWriteStage>(stage),
                        xpc_dictionary_get_uint64(event, wire::kProcessed),
                        xpc_dictionary_get_uint64(event, wire::kTotal)});
    }
  });
  xpc_connection_resume(connection);

  xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
  xpc_dictionary_set_int64(request, wire::kVersionKey, wire::kVersion);
  xpc_dictionary_set_string(request, wire::kCommand, wire::kCommandWrite);
  xpc_dictionary_set_string(request, wire::kOperationId, operationId.c_str());
  xpc_dictionary_set_string(request, wire::kSourceName, plan.image().displayName.c_str());
  xpc_dictionary_set_fd(request, wire::kSourceDescriptor, source.get());
  xpc_dictionary_set_uint64(request, wire::kSourceSize,
                            plan.image().sizeBytes);
  xpc_dictionary_set_string(request, wire::kTargetPath, plan.target().devicePath.c_str());
  xpc_dictionary_set_string(request, wire::kTargetStableId, plan.target().stableId.c_str());
  xpc_dictionary_set_uint64(request, wire::kTargetCapacity, plan.target().capacityBytes);
  xpc_dictionary_set_uint64(request, wire::kTargetSectorSize,
                            plan.target().logicalSectorSize);
  xpc_dictionary_set_uint64(request, wire::kBlockSize, plan.blockSize());
  xpc_dictionary_set_bool(request, wire::kVerify, plan.verify());
  xpc_dictionary_set_uint64(
      request, wire::kVerificationProfile,
      static_cast<std::uint64_t>(plan.verificationProfile()));
  xpc_dictionary_set_bool(request, wire::kClearTargetTailMetadata,
                          plan.clearTargetTailMetadata());
  wire::encodeWriteRanges(request, plan.writesWholeImage()
      ? std::vector<core::RawWriteRange>{} : plan.writeRanges());

  xpc_connection_send_message_with_reply(connection, request, queue, ^(xpc_object_t reply) {
    if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
      replyResult.error = xpcError(reply);
    } else if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY ||
               xpc_dictionary_get_int64(reply, wire::kVersionKey) != wire::kVersion) {
      replyResult.error = "The privileged helper returned an invalid protocol response";
    } else {
      replyResult.success = xpc_dictionary_get_bool(reply, wire::kSuccess);
      replyResult.cancelled = xpc_dictionary_get_bool(reply, wire::kCancelled);
      replyResult.destructiveWriteStarted =
          xpc_dictionary_get_bool(reply, wire::kWriteStarted);
      replyResult.bytesWritten = xpc_dictionary_get_uint64(reply, wire::kBytesWritten);
      replyResult.bytesVerified =
          xpc_dictionary_get_uint64(reply, wire::kBytesVerified);
      replyResult.verificationCompleted =
          xpc_dictionary_get_bool(reply, wire::kVerificationCompleted);
      const char* error = xpc_dictionary_get_string(reply, wire::kError);
      if (error != nullptr) {
        replyResult.error = error;
      }
    }
    receivedReply = true;
    dispatch_semaphore_signal(finished);
  });

  bool cancelSent = false;
  while (dispatch_semaphore_wait(finished,
                                 dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
    if (!cancelSent && cancelCallback && cancelCallback()) {
      xpc_object_t cancel = xpc_dictionary_create(nullptr, nullptr, 0);
      xpc_dictionary_set_int64(cancel, wire::kVersionKey, wire::kVersion);
      xpc_dictionary_set_string(cancel, wire::kCommand, wire::kCommandCancel);
      xpc_dictionary_set_string(cancel, wire::kOperationId, operationId.c_str());
      xpc_connection_send_message(connection, cancel);
      cancelSent = true;
    }
  }

  xpc_connection_cancel(connection);
  if (!receivedReply) {
    result.error = "The privileged helper disconnected without a result";
    return result;
  }
  return replyResult;
}

core::BadBlockTestResult testBadBlocksWithPrivilegedHelper(
    const core::BlockDeviceInfo& target,
    const core::BadBlockTestOptions& options,
    const core::BadBlockTestProgressCallback& onProgress,
    const core::BadBlockTestCancelCallback& isCancelled) {
  core::BadBlockTestResult result;
  const auto availability = privilegedHelperAvailability();
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  if (options.passes == 0U || options.passes > 4U ||
      options.maximumReportedOffsets > 4096U) {
    result.error = "The bad-block test options are outside the supported bounds";
    return result;
  }

  dispatch_queue_t queue = dispatch_queue_create(
      "org.rufusplusplus.app.helper-test-client", DISPATCH_QUEUE_SERIAL);
  xpc_connection_t connection = xpc_connection_create_mach_service(
      wire::kServiceName, queue, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
  if (connection == nullptr) {
    result.error = "Unable to create the privileged helper connection";
    return result;
  }
  const std::string requirement = helperRequirement();
  const int requirementError = xpc_connection_set_peer_code_signing_requirement(
      connection, requirement.c_str());
  if (requirementError != 0) {
    result.error = "Invalid helper code-signing requirement: " +
                   std::string(std::strerror(requirementError));
    return result;
  }

  const std::string operationId = nsString([NSUUID UUID].UUIDString);
  const core::BadBlockTestProgressCallback progressCallback = onProgress;
  const core::BadBlockTestCancelCallback cancelCallback = isCancelled;
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  __block core::BadBlockTestResult replyResult;
  __block bool receivedReply = false;
  xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_int64(event, wire::kVersionKey) != wire::kVersion) {
      return;
    }
    const char* type = xpc_dictionary_get_string(event, wire::kMessageType);
    const char* eventOperation =
        xpc_dictionary_get_string(event, wire::kOperationId);
    if (type == nullptr || eventOperation == nullptr ||
        operationId != eventOperation ||
        std::strcmp(type, wire::kMessageProgress) != 0) {
      return;
    }
    const std::int64_t stage = xpc_dictionary_get_int64(event, wire::kStage);
    if (stage < static_cast<std::int64_t>(
                    core::BadBlockTestStage::WritingPattern) ||
        stage > static_cast<std::int64_t>(core::BadBlockTestStage::Complete)) {
      return;
    }
    if (progressCallback) {
      progressCallback(
          {static_cast<core::BadBlockTestStage>(stage),
           static_cast<unsigned int>(
               xpc_dictionary_get_uint64(event, wire::kPass)),
           static_cast<unsigned int>(
               xpc_dictionary_get_uint64(event, wire::kPassCount)),
           xpc_dictionary_get_uint64(event, wire::kProcessed),
           xpc_dictionary_get_uint64(event, wire::kTotal)});
    }
  });
  xpc_connection_resume(connection);

  xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
  xpc_dictionary_set_int64(request, wire::kVersionKey, wire::kVersion);
  xpc_dictionary_set_string(request, wire::kCommand,
                            wire::kCommandBadBlockTest);
  xpc_dictionary_set_string(request, wire::kOperationId,
                            operationId.c_str());
  xpc_dictionary_set_string(request, wire::kTargetPath,
                            target.devicePath.c_str());
  xpc_dictionary_set_string(request, wire::kTargetStableId,
                            target.stableId.c_str());
  xpc_dictionary_set_uint64(request, wire::kTargetCapacity,
                            target.capacityBytes);
  xpc_dictionary_set_uint64(request, wire::kTargetSectorSize,
                            target.logicalSectorSize);
  xpc_dictionary_set_uint64(request, wire::kBlockSize,
                            options.transferBytes);
  xpc_dictionary_set_uint64(request, wire::kPassCount, options.passes);
  xpc_dictionary_set_uint64(request, wire::kMaximumReportedOffsets,
                            options.maximumReportedOffsets);

  xpc_connection_send_message_with_reply(
      connection, request, queue, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
          replyResult.error = xpcError(reply);
        } else if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY ||
                   xpc_dictionary_get_int64(reply, wire::kVersionKey) !=
                       wire::kVersion) {
          replyResult.error =
              "The privileged helper returned an invalid protocol response";
        } else {
          replyResult.completed =
              xpc_dictionary_get_bool(reply, wire::kCompleted);
          replyResult.success = xpc_dictionary_get_bool(reply, wire::kSuccess);
          replyResult.cancelled =
              xpc_dictionary_get_bool(reply, wire::kCancelled);
          replyResult.destructiveWriteStarted =
              xpc_dictionary_get_bool(reply, wire::kWriteStarted);
          replyResult.bytesTested =
              xpc_dictionary_get_uint64(reply, wire::kBytesTested);
          replyResult.badSectorCount =
              xpc_dictionary_get_uint64(reply, wire::kBadSectorCount);
          std::size_t dataSize = 0;
          const void* data = xpc_dictionary_get_data(
              reply, wire::kBadSectorOffsets, &dataSize);
          if (data != nullptr && dataSize % sizeof(std::uint64_t) == 0U) {
            replyResult.firstBadSectorOffsets.resize(
                dataSize / sizeof(std::uint64_t));
            std::memcpy(replyResult.firstBadSectorOffsets.data(), data,
                        dataSize);
          }
          const char* error = xpc_dictionary_get_string(reply, wire::kError);
          if (error != nullptr) {
            replyResult.error = error;
          }
        }
        receivedReply = true;
        dispatch_semaphore_signal(finished);
      });

  bool cancelSent = false;
  while (dispatch_semaphore_wait(
             finished,
             dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
    if (!cancelSent && cancelCallback && cancelCallback()) {
      xpc_object_t cancel = xpc_dictionary_create(nullptr, nullptr, 0);
      xpc_dictionary_set_int64(cancel, wire::kVersionKey, wire::kVersion);
      xpc_dictionary_set_string(cancel, wire::kCommand, wire::kCommandCancel);
      xpc_dictionary_set_string(cancel, wire::kOperationId,
                                operationId.c_str());
      xpc_connection_send_message(connection, cancel);
      cancelSent = true;
    }
  }
  xpc_connection_cancel(connection);
  if (!receivedReply) {
    result.error = "The privileged helper disconnected without a result";
    return result;
  }
  return replyResult;
}

core::MediaCaptureResult captureRawWithPrivilegedHelper(
    const core::BlockDeviceInfo& source,
    const std::filesystem::path& destination,
    const core::MediaCaptureOptions& options,
    const core::MediaCaptureProgressCallback& onProgress,
    const core::MediaCaptureCancelCallback& isCancelled) {
  core::MediaCaptureResult result;
  const auto availability = privilegedHelperAvailability();
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  if (options.format != core::MediaCaptureFormat::Raw || destination.empty()) {
    result.error = "The macOS helper accepts only a valid raw capture destination";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(destination, fileError) || fileError) {
    result.error = fileError ? "Unable to inspect the capture destination: " +
                                   fileError.message()
                             : "The capture destination already exists";
    return result;
  }
  const auto parent = destination.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : destination.parent_path();
  if (fileError || !std::filesystem::is_directory(parent, fileError) ||
      fileError) {
    result.error = "The capture destination directory is unavailable";
    return result;
  }
  auto partial = destination;
  partial += ".rufus-plus-plus-capture-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  FileDescriptor output(open(partial.c_str(),
                             O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                             S_IRUSR | S_IWUSR));
  if (!output.valid()) {
    result.error = "Unable to create the private capture output: " +
                   std::string(std::strerror(errno));
    return result;
  }
  const auto cleanup = [&] {
    output.reset();
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
  };

  dispatch_queue_t queue = dispatch_queue_create(
      "org.rufusplusplus.app.helper-capture-client", DISPATCH_QUEUE_SERIAL);
  xpc_connection_t connection = xpc_connection_create_mach_service(
      wire::kServiceName, queue, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
  if (connection == nullptr) {
    result.error = "Unable to create the privileged helper connection";
    cleanup();
    return result;
  }
  const std::string requirement = helperRequirement();
  const int requirementError = xpc_connection_set_peer_code_signing_requirement(
      connection, requirement.c_str());
  if (requirementError != 0) {
    result.error = "Invalid helper code-signing requirement: " +
                   std::string(std::strerror(requirementError));
    cleanup();
    return result;
  }
  const std::string operationId = nsString([NSUUID UUID].UUIDString);
  const core::MediaCaptureProgressCallback progressCallback = onProgress;
  const core::MediaCaptureCancelCallback cancelCallback = isCancelled;
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  __block core::MediaCaptureResult replyResult;
  __block bool receivedReply = false;
  xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_int64(event, wire::kVersionKey) != wire::kVersion) {
      return;
    }
    const char* type = xpc_dictionary_get_string(event, wire::kMessageType);
    const char* eventOperation =
        xpc_dictionary_get_string(event, wire::kOperationId);
    if (type == nullptr || eventOperation == nullptr ||
        operationId != eventOperation ||
        std::strcmp(type, wire::kMessageProgress) != 0) {
      return;
    }
    const std::int64_t stage = xpc_dictionary_get_int64(event, wire::kStage);
    if (stage < static_cast<std::int64_t>(core::MediaCaptureStage::Capturing) ||
        stage > static_cast<std::int64_t>(core::MediaCaptureStage::Complete)) {
      return;
    }
    if (progressCallback) {
      progressCallback(
          {static_cast<core::MediaCaptureStage>(stage),
           xpc_dictionary_get_uint64(event, wire::kProcessed),
           xpc_dictionary_get_uint64(event, wire::kTotal)});
    }
  });
  xpc_connection_resume(connection);
  xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
  xpc_dictionary_set_int64(request, wire::kVersionKey, wire::kVersion);
  xpc_dictionary_set_string(request, wire::kCommand, wire::kCommandCapture);
  xpc_dictionary_set_string(request, wire::kOperationId,
                            operationId.c_str());
  xpc_dictionary_set_fd(request, wire::kDestinationDescriptor, output.get());
  xpc_dictionary_set_string(request, wire::kTargetPath,
                            source.devicePath.c_str());
  xpc_dictionary_set_string(request, wire::kTargetStableId,
                            source.stableId.c_str());
  xpc_dictionary_set_uint64(request, wire::kTargetCapacity,
                            source.capacityBytes);
  xpc_dictionary_set_uint64(request, wire::kTargetSectorSize,
                            source.logicalSectorSize);
  xpc_dictionary_set_uint64(request, wire::kBlockSize,
                            options.transferBytes);
  xpc_dictionary_set_bool(request, wire::kVerify, options.verify);
  xpc_connection_send_message_with_reply(
      connection, request, queue, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
          replyResult.error = xpcError(reply);
        } else if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY ||
                   xpc_dictionary_get_int64(reply, wire::kVersionKey) !=
                       wire::kVersion) {
          replyResult.error =
              "The privileged helper returned an invalid protocol response";
        } else {
          replyResult.success = xpc_dictionary_get_bool(reply, wire::kSuccess);
          replyResult.cancelled =
              xpc_dictionary_get_bool(reply, wire::kCancelled);
          replyResult.bytesCaptured =
              xpc_dictionary_get_uint64(reply, wire::kBytesWritten);
          replyResult.outputSizeBytes =
              xpc_dictionary_get_uint64(reply, wire::kBytesTested);
          const char* error = xpc_dictionary_get_string(reply, wire::kError);
          if (error != nullptr) {
            replyResult.error = error;
          }
        }
        receivedReply = true;
        dispatch_semaphore_signal(finished);
      });
  bool cancelSent = false;
  while (dispatch_semaphore_wait(
             finished,
             dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
    if (!cancelSent && cancelCallback && cancelCallback()) {
      xpc_object_t cancel = xpc_dictionary_create(nullptr, nullptr, 0);
      xpc_dictionary_set_int64(cancel, wire::kVersionKey, wire::kVersion);
      xpc_dictionary_set_string(cancel, wire::kCommand, wire::kCommandCancel);
      xpc_dictionary_set_string(cancel, wire::kOperationId,
                                operationId.c_str());
      xpc_connection_send_message(connection, cancel);
      cancelSent = true;
    }
  }
  xpc_connection_cancel(connection);
  if (!receivedReply || !replyResult.success) {
    result = receivedReply ? replyResult : result;
    if (!receivedReply) {
      result.error = "The privileged helper disconnected without a result";
    }
    cleanup();
    return result;
  }
  output.reset();
  std::filesystem::rename(partial, destination, fileError);
  if (fileError) {
    result = replyResult;
    result.success = false;
    result.error = "Unable to commit the capture image: " + fileError.message();
    cleanup();
    return result;
  }
  return replyResult;
}

core::RawWriteResult createMacOsInstallerWithPrivilegedHelper(
    const core::MacOsInstallerInfo& installer,
    const core::BlockDeviceInfo& target, const bool fullWipe,
    const core::MacOsInstallerProgressCallback& onProgress,
    const core::MacOsInstallerCancelCallback& isCancelled) {
  core::RawWriteResult result;
  const auto availability = privilegedHelperAvailability();
  if (!availability.available) {
    result.error = availability.reason;
    return result;
  }
  if (installer.applicationPath.empty() ||
      installer.createInstallMediaPath.empty() || target.devicePath.empty() ||
      target.stableId.empty() || target.capacityBytes == 0U ||
      target.logicalSectorSize == 0U) {
    result.error = "The macOS installer request is incomplete";
    return result;
  }

  dispatch_queue_t queue = dispatch_queue_create(
      "org.rufusplusplus.app.helper-macos-installer-client", DISPATCH_QUEUE_SERIAL);
  xpc_connection_t connection = xpc_connection_create_mach_service(
      wire::kServiceName, queue, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
  if (connection == nullptr) {
    result.error = "Unable to create the privileged helper connection";
    return result;
  }
  const std::string requirement = helperRequirement();
  const int requirementError = xpc_connection_set_peer_code_signing_requirement(
      connection, requirement.c_str());
  if (requirementError != 0) {
    result.error = "Invalid helper code-signing requirement: " +
                   std::string(std::strerror(requirementError));
    return result;
  }

  const std::string operationId = nsString([NSUUID UUID].UUIDString);
  const core::MacOsInstallerProgressCallback progressCallback = onProgress;
  const core::MacOsInstallerCancelCallback cancelCallback = isCancelled;
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  __block core::RawWriteResult replyResult;
  __block bool receivedReply = false;
  xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
    if (xpc_get_type(event) != XPC_TYPE_DICTIONARY ||
        xpc_dictionary_get_int64(event, wire::kVersionKey) != wire::kVersion) {
      return;
    }
    const char* type = xpc_dictionary_get_string(event, wire::kMessageType);
    const char* eventOperation =
        xpc_dictionary_get_string(event, wire::kOperationId);
    if (type == nullptr || eventOperation == nullptr ||
        operationId != eventOperation ||
        std::strcmp(type, wire::kMessageProgress) != 0) {
      return;
    }
    const std::int64_t stage = xpc_dictionary_get_int64(event, wire::kStage);
    if (stage < static_cast<std::int64_t>(
                    core::MacOsInstallerStage::Revalidating) ||
        stage > static_cast<std::int64_t>(
                    core::MacOsInstallerStage::Complete)) {
      return;
    }
    if (progressCallback) {
      const char* detail = xpc_dictionary_get_string(event, wire::kSourceName);
      progressCallback(
          {static_cast<core::MacOsInstallerStage>(stage),
           xpc_dictionary_get_uint64(event, wire::kProcessed),
           xpc_dictionary_get_uint64(event, wire::kTotal),
           detail == nullptr ? std::string{} : std::string(detail)});
    }
  });
  xpc_connection_resume(connection);

  xpc_object_t request = xpc_dictionary_create(nullptr, nullptr, 0);
  xpc_dictionary_set_int64(request, wire::kVersionKey, wire::kVersion);
  xpc_dictionary_set_string(request, wire::kCommand,
                            wire::kCommandCreateMacOsInstaller);
  xpc_dictionary_set_string(request, wire::kOperationId,
                            operationId.c_str());
  xpc_dictionary_set_string(request, wire::kInstallerApplicationPath,
                            installer.applicationPath.c_str());
  xpc_dictionary_set_string(request, wire::kInstallerToolPath,
                            installer.createInstallMediaPath.c_str());
  xpc_dictionary_set_string(request, wire::kSourceName,
                            installer.displayName.c_str());
  xpc_dictionary_set_string(request, wire::kInstallerVersion,
                            installer.version.c_str());
  xpc_dictionary_set_string(request, wire::kInstallerBuild,
                            installer.build.c_str());
  xpc_dictionary_set_uint64(request, wire::kInstallerPayloadSize,
                            installer.payloadSizeBytes);
  xpc_dictionary_set_bool(request, wire::kFullWipe, fullWipe);
  xpc_dictionary_set_string(request, wire::kTargetPath,
                            target.devicePath.c_str());
  xpc_dictionary_set_string(request, wire::kTargetStableId,
                            target.stableId.c_str());
  xpc_dictionary_set_uint64(request, wire::kTargetCapacity,
                            target.capacityBytes);
  xpc_dictionary_set_uint64(request, wire::kTargetSectorSize,
                            target.logicalSectorSize);
  xpc_connection_send_message_with_reply(
      connection, request, queue, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
          replyResult.error = xpcError(reply);
        } else if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY ||
                   xpc_dictionary_get_int64(reply, wire::kVersionKey) !=
                       wire::kVersion) {
          replyResult.error =
              "The privileged helper returned an invalid protocol response";
        } else {
          replyResult.success = xpc_dictionary_get_bool(reply, wire::kSuccess);
          replyResult.cancelled =
              xpc_dictionary_get_bool(reply, wire::kCancelled);
          replyResult.destructiveWriteStarted =
              xpc_dictionary_get_bool(reply, wire::kWriteStarted);
          replyResult.bytesWritten =
              xpc_dictionary_get_uint64(reply, wire::kBytesWritten);
          replyResult.bytesVerified =
              xpc_dictionary_get_uint64(reply, wire::kBytesVerified);
          replyResult.verificationCompleted =
              xpc_dictionary_get_bool(reply, wire::kVerificationCompleted);
          const char* error = xpc_dictionary_get_string(reply, wire::kError);
          if (error != nullptr) {
            replyResult.error = error;
          }
        }
        receivedReply = true;
        dispatch_semaphore_signal(finished);
      });
  bool cancelSent = false;
  while (dispatch_semaphore_wait(
             finished,
             dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
    if (!cancelSent && cancelCallback && cancelCallback()) {
      xpc_object_t cancel = xpc_dictionary_create(nullptr, nullptr, 0);
      xpc_dictionary_set_int64(cancel, wire::kVersionKey, wire::kVersion);
      xpc_dictionary_set_string(cancel, wire::kCommand,
                                wire::kCommandCancel);
      xpc_dictionary_set_string(cancel, wire::kOperationId,
                                operationId.c_str());
      xpc_connection_send_message(connection, cancel);
      cancelSent = true;
    }
  }
  xpc_connection_cancel(connection);
  if (!receivedReply) {
    result.error = "The privileged helper disconnected without a result";
    return result;
  }
  return replyResult;
}

}  // namespace rufus::backend::macos
