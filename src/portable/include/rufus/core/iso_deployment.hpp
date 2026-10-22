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
#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rufus/core/linux_persistence.hpp"
#include "rufus/core/media.hpp"
#include "rufus/core/safety_policy.hpp"
#include "rufus/core/wim_splitter.hpp"
#include "rufus/core/windows_to_go.hpp"
#include "rufus/core/write_range.hpp"

namespace rufus::core {

namespace detail {
struct IsoDeploymentContent;
}

struct RuntimeUefiValidationBootloader final {
  // UEFI fallback filename, for example bootx64.efi.
  std::string filename;
  std::vector<unsigned char> data;
};

struct RuntimeUefiValidationAssets final {
  std::vector<RuntimeUefiValidationBootloader> bootloaders;
};

enum class IsoTargetSystem {
  Automatic,
  Bios,
  Uefi,
  BiosAndUefi,
};

enum class IsoFilesystemPreference {
  Automatic,
  Fat32,
  Ntfs,
};

struct IsoDeploymentOptions final {
  // FAT32 directory entries store a file size in 32 bits. The lower test value
  // is useful for exercising WIM transformation without a multi-gigabyte
  // fixture; production callers should keep the defaults.
  std::uint64_t maximumFatFileBytes{0xffffffffULL};
  std::uint64_t wimSplitPartBytes{3800ULL * 1024ULL * 1024ULL};
  // Set by a host integration when it can create, populate and verify NTFS
  // staging images and install the requested BIOS/UEFI bootstraps.
  bool ntfsAvailable{};
  // Unknown preserves the traditional automatic MBR choice. GPT is valid for
  // UEFI-only deployment and includes primary and backup partition tables.
  PartitionScheme partitionScheme{PartitionScheme::Unknown};
  IsoTargetSystem targetSystem{IsoTargetSystem::Automatic};
  IsoFilesystemPreference fileSystem{IsoFilesystemPreference::Automatic};
  // Zero selects a standards-compliant default. Explicit values are bytes per
  // allocation unit and are validated against the chosen filesystem/sector.
  std::uint32_t clusterSizeBytes{};
  // A full format writes and verifies zeroes across all otherwise-unused
  // target sectors. Quick format may stage only initialized MBR/FAT metadata.
  bool quickFormat{true};
  // When provided, ISO mode replaces each matching UEFI fallback loader with
  // the validation app, retains the original as boot*_original.efi, and emits
  // a complete md5sum.txt manifest. The host owns asset provenance.
  std::shared_ptr<const RuntimeUefiValidationAssets> runtimeUefiValidation;
};

struct WindowsInstallationOptions final {
  WindowsUserExperienceOptions userExperience;
};

enum class LegacyBiosBootstrap {
  None,
  WindowsBootManager,
  Grub2,
  Grub4Dos,
  ReactOs,
  Syslinux,
  FreeDos,
  MsDos,
};

enum class IsoDeploymentFilesystem {
  Fat32,
  Ntfs,
};

// ISO mode creates a standards-based MBR or GPT FAT32/NTFS layout. MBR Windows
// installer media can receive BOOTMGR-compatible BIOS bootstrap code, and an oversized
// sources/install.wim or install.esd is transformed into split .swm parts
// through wimlib.
class IsoDeploymentPlan final {
 public:
  [[nodiscard]] const ImageInfo& image() const noexcept { return image_; }
  [[nodiscard]] const BlockDeviceInfo& target() const noexcept { return target_; }
  [[nodiscard]] const std::string& volumeLabel() const noexcept { return volumeLabel_; }
  [[nodiscard]] std::uint64_t sourceBytes() const noexcept { return image_.sizeBytes; }
  [[nodiscard]] std::filesystem::file_time_type sourceLastWriteTime() const noexcept {
    return sourceLastWriteTime_;
  }
  [[nodiscard]] bool biosBootable() const noexcept {
    return legacyBiosBootstrap_ != LegacyBiosBootstrap::None;
  }
  [[nodiscard]] LegacyBiosBootstrap legacyBiosBootstrap() const noexcept {
    return legacyBiosBootstrap_;
  }
  [[nodiscard]] IsoDeploymentFilesystem fileSystem() const noexcept {
    return fileSystem_;
  }
  [[nodiscard]] PartitionScheme partitionScheme() const noexcept {
    return partitionScheme_;
  }
  [[nodiscard]] IsoTargetSystem targetSystem() const noexcept {
    return targetSystem_;
  }
  [[nodiscard]] std::uint32_t clusterSizeBytes() const noexcept {
    return options_.clusterSizeBytes;
  }
  [[nodiscard]] bool quickFormat() const noexcept {
    return options_.quickFormat;
  }
  [[nodiscard]] bool splitsWindowsImage() const noexcept { return splitWindowsImage_; }
  [[nodiscard]] bool hasLinuxPersistence() const noexcept {
    return linuxPersistenceStyle_ != LinuxPersistenceStyle::None;
  }
  // Only compact quick MBR/FAT32 stages leave the target's end unwritten.
  // Persistence, GPT and full-format stages already include that region.
  [[nodiscard]] bool requiresTrailingMetadataClear() const noexcept {
    return fileSystem_ == IsoDeploymentFilesystem::Fat32 &&
           partitionScheme_ == PartitionScheme::Mbr && options_.quickFormat &&
           !hasLinuxPersistence();
  }
  [[nodiscard]] LinuxPersistenceStyle linuxPersistenceStyle() const noexcept {
    return linuxPersistenceStyle_;
  }
  [[nodiscard]] std::uint64_t linuxPersistenceBytes() const noexcept {
    return linuxPersistenceBytes_;
  }
  [[nodiscard]] const std::string& windowsUnattendXml() const noexcept {
    return windowsUnattendXml_;
  }
  [[nodiscard]] bool runtimeUefiValidation() const noexcept {
    return options_.runtimeUefiValidation != nullptr;
  }

 private:
  friend class IsoDeploymentPlanner;
  friend class IsoImageStager;

  IsoDeploymentPlan(ImageInfo image, BlockDeviceInfo target, std::string volumeLabel,
                    std::filesystem::file_time_type sourceLastWriteTime,
                    std::shared_ptr<const detail::IsoDeploymentContent> content,
                    std::shared_ptr<const WimSplitter> wimSplitter,
                    IsoDeploymentOptions options,
                    IsoDeploymentFilesystem fileSystem,
                    PartitionScheme partitionScheme,
                    IsoTargetSystem targetSystem,
                    LegacyBiosBootstrap legacyBiosBootstrap,
                    bool splitWindowsImage,
                    LinuxPersistenceStyle linuxPersistenceStyle,
                    std::uint64_t linuxPersistenceBytes,
                    std::string windowsUnattendXml)
      : image_(std::move(image)),
        target_(std::move(target)),
        volumeLabel_(std::move(volumeLabel)),
        sourceLastWriteTime_(sourceLastWriteTime),
        content_(std::move(content)),
        wimSplitter_(std::move(wimSplitter)),
        options_(options),
        fileSystem_(fileSystem),
        partitionScheme_(partitionScheme),
        targetSystem_(targetSystem),
        legacyBiosBootstrap_(legacyBiosBootstrap),
        splitWindowsImage_(splitWindowsImage),
        linuxPersistenceStyle_(linuxPersistenceStyle),
        linuxPersistenceBytes_(linuxPersistenceBytes),
        windowsUnattendXml_(std::move(windowsUnattendXml)) {}

  ImageInfo image_;
  BlockDeviceInfo target_;
  std::string volumeLabel_;
  std::filesystem::file_time_type sourceLastWriteTime_{};
  std::shared_ptr<const detail::IsoDeploymentContent> content_;
  std::shared_ptr<const WimSplitter> wimSplitter_;
  IsoDeploymentOptions options_;
  IsoDeploymentFilesystem fileSystem_{IsoDeploymentFilesystem::Fat32};
  PartitionScheme partitionScheme_{PartitionScheme::Mbr};
  IsoTargetSystem targetSystem_{IsoTargetSystem::Automatic};
  LegacyBiosBootstrap legacyBiosBootstrap_{LegacyBiosBootstrap::None};
  bool splitWindowsImage_{};
  LinuxPersistenceStyle linuxPersistenceStyle_{LinuxPersistenceStyle::None};
  std::uint64_t linuxPersistenceBytes_{};
  std::string windowsUnattendXml_;
};

struct IsoDeploymentPlanResult final {
  std::optional<IsoDeploymentPlan> plan;
  std::vector<SafetyIssue> issues;
  std::vector<std::string> warnings;

  [[nodiscard]] bool succeeded() const noexcept { return plan.has_value(); }
};

class IsoDeploymentPlanner final {
 public:
  explicit IsoDeploymentPlanner(
      std::shared_ptr<const WimSplitter> wimSplitter = createSystemWimSplitter(),
      IsoDeploymentOptions options = {});

  [[nodiscard]] IsoDeploymentPlanResult build(const ImageInfo& image,
                                              const BlockDeviceInfo& target,
                                              std::string volumeLabel,
                                              LinuxPersistenceOptions persistence = {},
                                              WindowsInstallationOptions windows = {}) const;

 private:
  std::shared_ptr<const WimSplitter> wimSplitter_;
  IsoDeploymentOptions options_;
};

enum class IsoDeploymentStage {
  Planning,
  PreparingWindowsImage,
  Formatting,
  CreatingPersistence,
  Extracting,
  Verifying,
  Complete,
};

struct IsoDeploymentProgress final {
  IsoDeploymentStage stage{IsoDeploymentStage::Planning};
  std::uint64_t bytesProcessed{};
  std::uint64_t totalBytes{};
  std::string currentPath;
};

struct IsoDeploymentResult final {
  bool success{};
  bool cancelled{};
  std::uint64_t filesExtracted{};
  std::uint64_t bytesExtracted{};
  std::optional<ImageInfo> stagedImage;
  // Empty means a dense write. Quick stages may describe initialized ranges
  // inside a full-size sparse source; omitted bytes are unallocated space.
  std::vector<RawWriteRange> writeRanges;
  std::string error;
};

using IsoDeploymentProgressCallback =
    std::function<void(const IsoDeploymentProgress&)>;
using IsoDeploymentCancelCallback = std::function<bool()>;

// Sequential file operations let the validated ISO extractor target a filesystem
// library directly. Paths are validated relative ISO paths; implementations must
// reject existing files and flush all metadata before reopening read-only.
class IsoDeploymentFileTree {
 public:
  virtual ~IsoDeploymentFileTree() = default;
  virtual bool createDirectories(const std::string& path, std::string& error) = 0;
  virtual bool createFile(const std::string& path, std::string& error) = 0;
  virtual bool write(const unsigned char* data, std::size_t size, std::string& error) = 0;
  virtual bool closeFile(std::string& error) = 0;
  virtual bool reopenReadOnly(std::string& error) = 0;
  virtual bool openFileForRead(const std::string& path, std::uint64_t expectedSize,
                               std::string& error) = 0;
  virtual bool read(unsigned char* data, std::size_t size, std::string& error) = 0;
};

class IsoImageStager final {
 public:
  // outputPath must not exist. On success it contains the sector-aligned
  // initialized portion of a complete FAT32 disk layout. Quick MBR plans may
  // use a compact prefix; GPT and full-format plans include the complete sparse
  // target. For quick GPT/persistence stages, pass result.writeRanges to the
  // writer so initialized metadata reaches its disk offsets without copying
  // unallocated space. Full formats leave that map empty for dense zero-fill.
  [[nodiscard]] IsoDeploymentResult stage(
      const IsoDeploymentPlan& plan, const std::filesystem::path& outputPath,
      const IsoDeploymentProgressCallback& onProgress = {},
      const IsoDeploymentCancelCallback& isCancelled = {}) const;

  // Extracts and verifies a validated NTFS-mode plan into an already mounted,
  // newly formatted filesystem. The platform stager owns partitioning,
  // formatting, mounting, and the UEFI:NTFS bootstrap.
  [[nodiscard]] IsoDeploymentResult extractToDirectory(
      const IsoDeploymentPlan& plan, const std::filesystem::path& outputRoot,
      const IsoDeploymentProgressCallback& onProgress = {},
      const IsoDeploymentCancelCallback& isCancelled = {}) const;

  [[nodiscard]] IsoDeploymentResult extractToFileTree(
      const IsoDeploymentPlan& plan, IsoDeploymentFileTree& destination,
      const IsoDeploymentProgressCallback& onProgress = {},
      const IsoDeploymentCancelCallback& isCancelled = {}) const;
};

[[nodiscard]] const char* isoDeploymentStageName(IsoDeploymentStage stage) noexcept;

}  // namespace rufus::core
