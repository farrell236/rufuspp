/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/deployment_quality.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include "rufus/core/format.hpp"

namespace rufus::core {
namespace {

std::string jsonEscape(const std::string& value) {
  std::ostringstream escaped;
  for (const unsigned char character : value) {
    switch (character) {
      case '\"':
        escaped << "\\\"";
        break;
      case '\\':
        escaped << "\\\\";
        break;
      case '\b':
        escaped << "\\b";
        break;
      case '\f':
        escaped << "\\f";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        if (character < 0x20U) {
          escaped << "\\u" << std::hex << std::setw(4)
                  << std::setfill('0') << static_cast<unsigned int>(character)
                  << std::dec;
        } else {
          escaped << static_cast<char>(character);
        }
        break;
    }
  }
  return escaped.str();
}

void appendJsonString(std::ostringstream& output, const char* name,
                      const std::string& value, const bool comma = true) {
  output << "\"" << name << "\":\"" << jsonEscape(value) << "\"";
  if (comma) {
    output << ',';
  }
}

std::string allocationUnit(const std::uint32_t bytes) {
  return bytes == 0U ? "Automatic" : std::string(formatByteSize(bytes));
}

void appendEntry(DeploymentPreflightReport& report, std::string label,
                 std::string value, std::string detail = {},
                 const PreflightLevel level = PreflightLevel::Information) {
  report.entries.push_back(
      {std::move(label), std::move(value), std::move(detail), level});
}

}  // namespace

bool DeploymentPreflightReport::ready() const noexcept {
  return std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.level == PreflightLevel::Blocker;
  });
}

std::string DeploymentPreflightReport::toText() const {
  std::ostringstream output;
  output << operation << " preflight\n";
  output << (ready() ? "READY" : "BLOCKED") << "\n\n";
  for (const auto& entry : entries) {
    output << entry.label << ": " << entry.value;
    if (entry.level != PreflightLevel::Information) {
      output << " [" << preflightLevelName(entry.level) << ']';
    }
    output << '\n';
    if (!entry.detail.empty()) {
      output << "  " << entry.detail << '\n';
    }
  }
  return output.str();
}

std::string DeploymentPreflightReport::toJson() const {
  std::ostringstream output;
  output << '{';
  appendJsonString(output, "schema", "rufus-plus-plus-preflight-v1");
  appendJsonString(output, "operation", operation);
  output << "\"ready\":" << (ready() ? "true" : "false") << ',';
  output << "\"entries\":[";
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    output << '{';
    appendJsonString(output, "label", entry.label);
    appendJsonString(output, "value", entry.value);
    appendJsonString(output, "detail", entry.detail);
    appendJsonString(output, "level", preflightLevelName(entry.level), false);
    output << '}' << (index + 1U == entries.size() ? "" : ",");
  }
  output << "]}";
  return output.str();
}

DeploymentPreflightReport buildDeploymentPreflight(
    const DeploymentPreflightInput& input) {
  DeploymentPreflightReport report;
  report.operation = input.operation.empty() ? "Deployment" : input.operation;
  appendEntry(report, "Source", input.image.displayName,
              std::string(imageFormatName(input.image.format)) + ", " +
                  std::string(formatByteSize(input.image.deploymentSizeBytes())));
  appendEntry(report, "Target", input.target.displayName,
              input.target.devicePath + "; " +
                  std::string(formatByteSize(input.target.capacityBytes)));
  appendEntry(report, "Device identity",
              input.target.stableId.empty() ? "Missing" : input.target.stableId,
              input.target.serialNumber.empty()
                  ? "No hardware serial was reported; path, geometry and model must remain stable."
                  : "Serial: " + input.target.serialNumber,
              input.target.stableId.empty() ? PreflightLevel::Blocker
                                            : PreflightLevel::Information);
  appendEntry(report, "Partition scheme",
              input.partitionScheme.empty() ? "Image-defined"
                                            : input.partitionScheme);
  appendEntry(report, "Target system",
              input.targetSystem.empty() ? "Image-defined"
                                         : input.targetSystem);
  appendEntry(report, "Filesystem",
              input.fileSystem.empty() ? "Image-defined" : input.fileSystem);
  appendEntry(report, "Cluster size", allocationUnit(input.clusterSizeBytes));
  appendEntry(report, "Format policy",
              input.quickFormat ? "Quick" : "Full overwrite");
  appendEntry(report, "Verification",
              verificationProfileName(input.verificationProfile));
  appendEntry(report, "Write size", input.writeSizePending
      ? "Calculated during preparation" : std::string(formatByteSize(input.bytesToWrite)),
      input.writeSizePending ? "Quick format skips unallocated space; progress shows the actual transfer total." : "");
  if (input.temporaryBytes != 0U) {
    appendEntry(report, "Temporary space",
                std::string(formatByteSize(input.temporaryBytes)));
  }
  for (const auto& transformation : input.transformations) {
    appendEntry(report, "Transformation", transformation);
  }
  for (const auto& dependency : input.dependencies) {
    const PreflightLevel level =
        dependency.required && !dependency.available
            ? PreflightLevel::Blocker
        : !dependency.available ? PreflightLevel::Warning
                                : PreflightLevel::Information;
    appendEntry(report, "Dependency: " + dependency.name,
                dependency.available ? "Available" : "Unavailable",
                dependency.detail, level);
  }
  for (const auto& warning : input.warnings) {
    appendEntry(report, "Warning", warning, {}, PreflightLevel::Warning);
  }
  for (const auto& blocker : input.blockers) {
    appendEntry(report, "Blocker", blocker, {}, PreflightLevel::Blocker);
  }
  if (!input.target.writable || input.target.systemDevice ||
      !input.target.wholeDevice) {
    appendEntry(report, "Safety policy", "Target rejected",
                input.target.systemDevice
                    ? "System devices cannot be overwritten."
                : !input.target.wholeDevice
                    ? "Only whole physical devices can be overwritten."
                    : "The selected target is read-only.",
                PreflightLevel::Blocker);
  } else {
    appendEntry(report, "Safety policy", "Eligible whole device");
  }
  return report;
}

std::string DeploymentReceipt::toJson() const {
  std::ostringstream output;
  output << '{';
  appendJsonString(output, "schema", "rufus-plus-plus-deployment-receipt-v1");
  appendJsonString(output, "application", application);
  appendJsonString(output, "applicationVersion", applicationVersion);
  appendJsonString(output, "startedAtUtc", startedAtUtc);
  appendJsonString(output, "finishedAtUtc", finishedAtUtc);
  output << "\"preflight\":" << preflight.toJson() << ',';
  output << "\"outcome\":{";
  output << "\"success\":" << (success ? "true" : "false") << ',';
  output << "\"cancelled\":" << (cancelled ? "true" : "false") << ',';
  output << "\"destructiveWriteStarted\":"
         << (destructiveWriteStarted ? "true" : "false") << ',';
  output << "\"bytesWritten\":" << bytesWritten << ',';
  output << "\"bytesVerified\":" << bytesVerified << ',';
  output << "\"verificationCompleted\":"
         << (verificationCompleted ? "true" : "false") << ',';
  appendJsonString(output, "sourceSha256", sourceSha256);
  appendJsonString(output, "error", error, false);
  output << "}}";
  return output.str();
}

ReceiptWriteResult writeDeploymentReceipt(
    const std::filesystem::path& destination,
    const DeploymentReceipt& receipt) {
  ReceiptWriteResult result;
  if (destination.empty() || destination.filename().empty()) {
    result.error = "The receipt destination is invalid";
    return result;
  }
  std::error_code error;
  const auto parent = destination.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      result.error = "Unable to create the receipt directory: " +
                     error.message();
      return result;
    }
  }
  auto partial = destination;
  partial += ".partial";
  std::filesystem::remove(partial, error);
  error.clear();
  {
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    const std::string payload = receipt.toJson();
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
    if (!output) {
      result.error = "Unable to write the deployment receipt";
      output.close();
      std::filesystem::remove(partial, error);
      return result;
    }
  }
  std::filesystem::rename(partial, destination, error);
  if (error) {
    std::filesystem::remove(partial, error);
    result.error = "Unable to commit the deployment receipt: " +
                   error.message();
    return result;
  }
  result.success = true;
  return result;
}

const char* preflightLevelName(const PreflightLevel level) noexcept {
  switch (level) {
    case PreflightLevel::Information:
      return "information";
    case PreflightLevel::Warning:
      return "warning";
    case PreflightLevel::Blocker:
      return "blocker";
  }
  return "unknown";
}

}  // namespace rufus::core
