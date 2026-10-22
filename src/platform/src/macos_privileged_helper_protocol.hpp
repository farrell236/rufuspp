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

#include <cstdint>

#ifndef RUFUSPP_MACOS_APP_IDENTIFIER
#define RUFUSPP_MACOS_APP_IDENTIFIER "org.rufusplusplus.app"
#endif

#ifndef RUFUSPP_MACOS_HELPER_IDENTIFIER
#define RUFUSPP_MACOS_HELPER_IDENTIFIER "org.rufusplusplus.app.privileged-helper"
#endif

#ifndef RUFUSPP_MACOS_TEAM_IDENTIFIER
#define RUFUSPP_MACOS_TEAM_IDENTIFIER ""
#endif

namespace rufus::backend::macos::protocol {

inline constexpr std::int64_t kVersion = 7;
inline constexpr char kServiceName[] = RUFUSPP_MACOS_HELPER_IDENTIFIER;
inline constexpr char kDaemonPlistName[] = RUFUSPP_MACOS_HELPER_IDENTIFIER ".plist";

inline constexpr char kCommand[] = "command";
inline constexpr char kCommandPing[] = "ping";
inline constexpr char kCommandWrite[] = "write";
inline constexpr char kCommandBadBlockTest[] = "bad-block-test";
inline constexpr char kCommandCapture[] = "capture";
inline constexpr char kCommandCancel[] = "cancel";
inline constexpr char kCommandCreateMacOsInstaller[] = "create-macos-installer";
inline constexpr char kVersionKey[] = "version";
inline constexpr char kOperationId[] = "operation-id";
inline constexpr char kSourceName[] = "source-name";
inline constexpr char kSourceDescriptor[] = "source-fd";
inline constexpr char kDestinationDescriptor[] = "destination-fd";
inline constexpr char kSourceSize[] = "source-size";
inline constexpr char kTargetPath[] = "target-path";
inline constexpr char kTargetStableId[] = "target-stable-id";
inline constexpr char kTargetCapacity[] = "target-capacity";
inline constexpr char kTargetSectorSize[] = "target-sector-size";
inline constexpr char kBlockSize[] = "block-size";
inline constexpr char kVerify[] = "verify";
inline constexpr char kVerificationProfile[] = "verification-profile";
inline constexpr char kClearTargetTailMetadata[] = "clear-target-tail-metadata";
inline constexpr char kWriteRanges[] = "write-ranges";
inline constexpr char kRangeOffset[] = "offset";
inline constexpr char kRangeSize[] = "size";
inline constexpr char kPassCount[] = "pass-count";
inline constexpr char kPass[] = "pass";
inline constexpr char kMaximumReportedOffsets[] = "maximum-reported-offsets";
inline constexpr char kInstallerApplicationPath[] = "installer-application-path";
inline constexpr char kInstallerToolPath[] = "installer-tool-path";
inline constexpr char kInstallerVersion[] = "installer-version";
inline constexpr char kInstallerBuild[] = "installer-build";
inline constexpr char kInstallerPayloadSize[] = "installer-payload-size";
inline constexpr char kFullWipe[] = "full-wipe";

inline constexpr char kMessageType[] = "message-type";
inline constexpr char kMessageProgress[] = "progress";
inline constexpr char kStage[] = "stage";
inline constexpr char kProcessed[] = "processed";
inline constexpr char kTotal[] = "total";
inline constexpr char kSuccess[] = "success";
inline constexpr char kCancelled[] = "cancelled";
inline constexpr char kWriteStarted[] = "write-started";
inline constexpr char kBytesWritten[] = "bytes-written";
inline constexpr char kBytesVerified[] = "bytes-verified";
inline constexpr char kVerificationCompleted[] = "verification-completed";
inline constexpr char kCompleted[] = "completed";
inline constexpr char kBytesTested[] = "bytes-tested";
inline constexpr char kBadSectorCount[] = "bad-sector-count";
inline constexpr char kBadSectorOffsets[] = "bad-sector-offsets";
inline constexpr char kError[] = "error";

}  // namespace rufus::backend::macos::protocol
