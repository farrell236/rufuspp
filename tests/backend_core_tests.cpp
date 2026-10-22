/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "iso_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "rufus/core/bad_block_test.hpp"
#include "rufus/core/deployment_quality.hpp"
#include "rufus/core/file_image_writer.hpp"
#include "rufus/core/image_analyzer.hpp"
#include "rufus/core/image_profile.hpp"
#include "rufus/core/iso_deployment.hpp"
#include "rufus/core/linux_persistence.hpp"
#include "rufus/core/media.hpp"
#include "rufus/core/media_capture.hpp"
#include "rufus/core/media_inspector.hpp"
#include "rufus/core/raw_image_writer.hpp"
#include "rufus/core/safety_policy.hpp"
#include "rufus/core/secure_boot_analyzer.hpp"
#include "rufus/core/standalone_media.hpp"
#include "rufus/core/windows_to_go.hpp"
#include "rufus/core/write_plan.hpp"
#include "linux_persistence_support.hpp"
#include "ext2_formatter.hpp"

namespace {

void expect(const bool condition, const std::string& description) {
  if (!condition) {
    std::cerr << description << '\n';
    std::exit(EXIT_FAILURE);
  }
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rufus-plus-plus-backend-tests-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void writeBytes(const std::filesystem::path& path,
                const std::vector<unsigned char>& bytes);

class FakeWimSplitter final : public rufus::core::WimSplitter {
 public:
  explicit FakeWimSplitter(const bool available = true) : available_(available) {}

  [[nodiscard]] bool available() const noexcept override { return available_; }

  [[nodiscard]] std::string availabilityReason() const override {
    return available_ ? std::string{} : "test wimlib backend is unavailable";
  }

  [[nodiscard]] rufus::core::WimSplitResult split(
      const std::filesystem::path& sourceWim,
      const std::filesystem::path& firstPartPath,
      const std::uint64_t maximumPartBytes,
      const rufus::core::WimSplitProgressCallback& onProgress,
      const rufus::core::WimSplitCancelCallback& isCancelled) const override {
    rufus::core::WimSplitResult result;
    called_ = true;
    observedSourceSize_ = std::filesystem::file_size(sourceWim);
    observedPartLimit_ = maximumPartBytes;
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "fake split cancelled";
      return result;
    }
    auto makePart = [](const std::uint16_t partNumber) {
      std::vector<unsigned char> bytes(256, 0);
      constexpr std::array<unsigned char, 8> magic =
          {'M', 'S', 'W', 'I', 'M', 0, 0, 0};
      std::copy(magic.begin(), magic.end(), bytes.begin());
      bytes[8] = 208;
      for (std::size_t index = 0; index < 16; ++index) {
        bytes[24 + index] = static_cast<unsigned char>(0x40U + index);
      }
      bytes[40] = static_cast<unsigned char>(partNumber);
      bytes[42] = 2;
      return bytes;
    };
    const auto secondPartPath =
        firstPartPath.parent_path() /
        (firstPartPath.stem().string() + "2" + firstPartPath.extension().string());
    writeBytes(firstPartPath, makePart(1));
    writeBytes(secondPartPath, makePart(2));
    if (onProgress) {
      onProgress({observedSourceSize_, observedSourceSize_, 2, 2});
    }
    result.success = true;
    result.parts = {firstPartPath, secondPartPath};
    return result;
  }

  [[nodiscard]] bool called() const noexcept { return called_; }
  [[nodiscard]] std::uint64_t observedSourceSize() const noexcept {
    return observedSourceSize_;
  }
  [[nodiscard]] std::uint64_t observedPartLimit() const noexcept {
    return observedPartLimit_;
  }

 private:
  bool available_{};
  mutable bool called_{};
  mutable std::uint64_t observedSourceSize_{};
  mutable std::uint64_t observedPartLimit_{};
};

class MemoryRawTarget final : public rufus::core::RawTargetIo {
 public:
  MemoryRawTarget(const std::uint64_t capacity, const std::uint32_t sectorSize)
      : bytes_(static_cast<std::size_t>(capacity), 0xa5), sectorSize_(sectorSize) {}

  [[nodiscard]] std::uint64_t capacityBytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logicalSectorSize() const noexcept override {
    return sectorSize_;
  }

  bool writeAt(const std::uint64_t offset, const unsigned char* data, const std::size_t size,
               std::string& error) override {
    if (offset > bytes_.size() || size > bytes_.size() - static_cast<std::size_t>(offset)) {
      error = "test target write is out of range";
      return false;
    }
    std::copy_n(data, size, bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    ++writeCount_;
    return true;
  }

  bool readAt(const std::uint64_t offset, unsigned char* data, const std::size_t size,
              std::string& error) override {
    if (offset > bytes_.size() || size > bytes_.size() - static_cast<std::size_t>(offset)) {
      error = "test target read is out of range";
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), size, data);
    if (corruptReads_ && size != 0U && offset == 0U) {
      data[0] ^= 0xffU;
    }
    return true;
  }

  bool flush(std::string& error) override {
    if (failFlush_) {
      error = "simulated flush failure";
      return false;
    }
    return true;
  }

  void setFailFlush(const bool fail) noexcept { failFlush_ = fail; }
  void setCorruptReads(const bool corrupt) noexcept { corruptReads_ = corrupt; }
  [[nodiscard]] const std::vector<unsigned char>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t writeCount() const noexcept { return writeCount_; }

 private:
  std::vector<unsigned char> bytes_;
  std::uint32_t sectorSize_{};
  std::size_t writeCount_{};
  bool failFlush_{};
  bool corruptReads_{};
};

class MemoryRawSource final : public rufus::core::RawSourceIo {
 public:
  explicit MemoryRawSource(std::vector<unsigned char> bytes) : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t sizeBytes() const noexcept override { return bytes_.size(); }

  bool readAt(const std::uint64_t offset, unsigned char* data, const std::size_t size,
              std::string& error) override {
    if (offset > bytes_.size() || size > bytes_.size() - static_cast<std::size_t>(offset)) {
      error = "test source read is out of range";
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), size, data);
    return true;
  }

 private:
  std::vector<unsigned char> bytes_;
};

void writeBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  expect(static_cast<bool>(output), "test fixture should be written");
}

void testMediaCapture(const std::filesystem::path& directory) {
  std::vector<unsigned char> bytes(4U * 1024U * 1024U, 0);
  for (std::size_t index = 0; index < 4096U; ++index) {
    bytes[index] = static_cast<unsigned char>((index * 37U) & 0xffU);
    bytes[3U * 1024U * 1024U + index] =
        static_cast<unsigned char>((index * 19U + 7U) & 0xffU);
  }
  const rufus::core::MediaCaptureWriter writer;
  const rufus::core::ImageAnalyzer analyzer;
  const std::array<std::pair<rufus::core::MediaCaptureFormat, const char*>, 4>
      formats{{
          {rufus::core::MediaCaptureFormat::Raw, "captured.img"},
          {rufus::core::MediaCaptureFormat::FixedVhd, "captured-fixed.vhd"},
          {rufus::core::MediaCaptureFormat::DynamicVhd, "captured-dynamic.vhd"},
          {rufus::core::MediaCaptureFormat::DynamicVhdx, "captured-dynamic.vhdx"},
      }};
  for (const auto& [format, name] : formats) {
    MemoryRawSource source(bytes);
    const auto destination = directory / name;
    std::size_t progressEvents = 0;
    const auto captured = writer.capture(
        source, destination, {format, 64U * 1024U, true},
        [&](const rufus::core::MediaCaptureProgress&) { ++progressEvents; });
    expect(captured.success && captured.bytesCaptured == bytes.size() &&
               std::filesystem::exists(destination) && progressEvents > 4U,
           std::string("media capture should create and verify ") + name +
               ": " + captured.error);
    const auto analysis = analyzer.analyze(destination);
    expect(analysis.succeeded() && analysis.image->capabilities.rawWrite &&
               analysis.image->deploymentSizeBytes() == bytes.size(),
           std::string("captured image should analyze as deployable: ") + name);
    auto opened = rufus::core::openRawImageSource(*analysis.image);
    expect(opened.succeeded(),
           std::string("captured image should reopen: ") + opened.error);
    std::vector<unsigned char> observed(bytes.size());
    std::string error;
    expect(opened.source->readAt(0, observed.data(), observed.size(), error) &&
               observed == bytes,
           std::string("captured virtual disk should reproduce source bytes: ") +
               error);
    if (format == rufus::core::MediaCaptureFormat::DynamicVhdx) {
      const auto little = [](const unsigned char* value, const unsigned int count) {
        std::uint64_t result = 0U;
        for (unsigned int index = 0; index < count; ++index) {
          result |= static_cast<std::uint64_t>(value[index]) << (index * 8U);
        }
        return result;
      };
      std::ifstream input(destination, std::ios::binary);
      for (const auto offset : {65536U, 131072U}) {
        std::array<unsigned char, 80> header{};
        input.seekg(offset);
        input.read(reinterpret_cast<char*>(header.data()), header.size());
        const auto logLength = little(header.data() + 68U, 4U);
        const auto logOffset = little(header.data() + 72U, 8U);
        expect(input && logLength >= 1024U * 1024U &&
                   logLength % (1024U * 1024U) == 0U && logOffset >= 1024U * 1024U &&
                   logOffset % (1024U * 1024U) == 0U &&
                   logOffset + logLength <= std::filesystem::file_size(destination) &&
                   std::all_of(header.begin() + 48U, header.begin() + 64U,
                               [](const auto byte) { return byte == 0U; }),
               "both VHDX headers must declare an aligned empty log after the header section");
      }
    }
  }

  MemoryRawSource cancelledSource(bytes);
  const auto cancelledPath = directory / "cancelled-capture.img";
  bool stop = false;
  const auto cancelled = writer.capture(
      cancelledSource, cancelledPath,
      {rufus::core::MediaCaptureFormat::Raw, 64U * 1024U, true},
      [&](const rufus::core::MediaCaptureProgress& progress) {
        stop = progress.stage == rufus::core::MediaCaptureStage::Capturing &&
               progress.bytesProcessed != 0U;
      },
      [&] { return stop; });
  expect(cancelled.cancelled && !std::filesystem::exists(cancelledPath),
         "cancelled capture must remove its partial output");
}

void testBadBlockEngine() {
  constexpr std::uint64_t capacity = 2U * 1024U * 1024U;
  const rufus::core::BadBlockTester tester;
  MemoryRawTarget clean(capacity, 512U);
  std::size_t progressEvents = 0;
  const auto passed = tester.test(
      clean, {2U, 64U * 1024U, 16U},
      [&](const rufus::core::BadBlockTestProgress&) { ++progressEvents; });
  expect(passed.completed && passed.success && !passed.cancelled &&
             passed.destructiveWriteStarted && passed.badSectorCount == 0U &&
             passed.bytesTested == capacity * 2U && progressEvents > 10U,
         "location-dependent bad-block patterns should survive two complete passes: " +
             passed.error);

  MemoryRawTarget corrupted(capacity, 512U);
  corrupted.setCorruptReads(true);
  const auto failed = tester.test(corrupted, {1U, 64U * 1024U, 16U});
  expect(failed.completed && !failed.success && failed.badSectorCount == 1U &&
             failed.firstBadSectorOffsets.size() == 1U &&
             failed.firstBadSectorOffsets.front() == 0U,
         "bad-block verification should report the exact corrupted sector");

  MemoryRawTarget cancelledTarget(capacity, 512U);
  bool cancel = false;
  const auto cancelled = tester.test(
      cancelledTarget, {1U, 64U * 1024U, 16U},
      [&](const rufus::core::BadBlockTestProgress& progress) {
        if (progress.stage == rufus::core::BadBlockTestStage::WritingPattern) {
          cancel = true;
        }
      },
      [&] { return cancel; });
  expect(cancelled.cancelled && cancelled.destructiveWriteStarted &&
             !cancelled.completed,
         "bad-block cancellation should report that destructive writing began");
}

void testStandaloneFat32(const TemporaryDirectory& temporary) {
  rufus::core::BlockDeviceInfo target;
  target.devicePath = "/dev/test";
  target.stableId = "standalone-test";
  target.capacityBytes = 128ULL * 1024ULL * 1024ULL;
  target.logicalSectorSize = 512U;
  target.removable = true;
  target.ejectable = true;
  target.writable = true;
  target.wholeDevice = true;
  const auto output = temporary.path() / "blank-fat32.img";
  std::size_t progressEvents = 0;
  const auto staged = rufus::core::stageBlankFat32Media(
      target, output, "PORTABLE",
      [&](const rufus::core::StandaloneMediaProgress&) { ++progressEvents; });
  expect(staged.success && staged.stagedImage.has_value() &&
             staged.stagedImage->capabilities.rawWrite &&
             staged.stagedImage->capabilities.validPartitionTable &&
             staged.stagedImage->partitionScheme ==
                 rufus::core::PartitionScheme::Mbr &&
             staged.stagedImage->sizeBytes < target.capacityBytes &&
             progressEvents == 4U,
         "blank FAT32 media should stage as a compact validated MBR prefix: " +
             staged.error);
  std::ifstream input(output, std::ios::binary);
  std::array<unsigned char, 512> mbr{};
  input.read(reinterpret_cast<char*>(mbr.data()), mbr.size());
  const std::uint32_t start =
      static_cast<std::uint32_t>(mbr[454U]) |
      static_cast<std::uint32_t>(mbr[455U]) << 8U |
      static_cast<std::uint32_t>(mbr[456U]) << 16U |
      static_cast<std::uint32_t>(mbr[457U]) << 24U;
  std::array<unsigned char, 512> boot{};
  input.seekg(static_cast<std::streamoff>(start) * 512U);
  input.read(reinterpret_cast<char*>(boot.data()), boot.size());
  expect(input && mbr[450] == 0x0cU && mbr[510] == 0x55U &&
             mbr[511] == 0xaaU &&
             std::string(reinterpret_cast<const char*>(boot.data() + 71U), 8U) ==
                 "PORTABLE",
         "blank FAT32 media should expose a verified partition and volume label");

  const auto fullOutput = temporary.path() / "full-fat32.img";
  const auto full = rufus::core::stageBlankFat32Media(
      target, fullOutput, "FULL", {}, {},
      rufus::core::StandaloneFormatOptions{1024U, false});
  std::ifstream fullInput(fullOutput, std::ios::binary);
  std::array<unsigned char, 512> fullBoot{};
  fullInput.seekg(2048ULL * 512ULL);
  fullInput.read(reinterpret_cast<char*>(fullBoot.data()), fullBoot.size());
  expect(full.success && full.stagedImage.has_value() &&
             full.stagedImage->sizeBytes == target.capacityBytes &&
             std::filesystem::file_size(fullOutput) == target.capacityBytes &&
             fullInput && fullBoot[13U] == 2U,
         "full FAT32 format should honor the 1 KiB cluster and cover the complete target: " +
             full.error);
  const auto invalidClusterOutput = temporary.path() / "invalid-fat32.img";
  const auto invalidCluster = rufus::core::stageBlankFat32Media(
      target, invalidClusterOutput, "INVALID", {}, {},
      rufus::core::StandaloneFormatOptions{65536U, true});
  expect(!invalidCluster.success &&
             !std::filesystem::exists(invalidClusterOutput),
         "FAT32 staging should reject allocation units larger than 32 KiB");

  const auto fat16Output = temporary.path() / "blank-fat16.img";
  const auto fat16 = rufus::core::stageBlankFat16Media(
      target, fat16Output, "PORTABLE");
  expect(fat16.success && fat16.stagedImage.has_value() &&
             fat16.stagedImage->sizeBytes < target.capacityBytes &&
             fat16.stagedImage->capabilities.validPartitionTable,
         "blank FAT16 media should stage as a compact validated MBR prefix: " +
             fat16.error);
  std::ifstream fat16Input(fat16Output, std::ios::binary);
  std::array<unsigned char, 512> fat16Mbr{};
  std::array<unsigned char, 512> fat16Boot{};
  fat16Input.read(reinterpret_cast<char*>(fat16Mbr.data()), fat16Mbr.size());
  fat16Input.seekg(2048ULL * 512ULL);
  fat16Input.read(reinterpret_cast<char*>(fat16Boot.data()), fat16Boot.size());
  expect(fat16Input && fat16Mbr[450] == 0x0eU &&
             std::string(reinterpret_cast<const char*>(fat16Boot.data() + 54U),
                         8U) == "FAT16   " &&
             std::string(reinterpret_cast<const char*>(fat16Boot.data() + 43U),
                         8U) == "PORTABLE",
         "blank FAT16 media should contain a verified FAT16 BPB and label");

  const auto freeDosOutput = temporary.path() / "freedos-fat32.img";
  std::vector<rufus::core::StandaloneMediaFile> files;
  files.push_back({"KERNEL.SYS", {'K', 'E', 'R', 'N', 'E', 'L'}});
  files.push_back({"COMMAND.COM", {'S', 'H', 'E', 'L', 'L'}});
  files.push_back({"AUTOEXEC.BAT", {'V', 'E', 'R', '\r', '\n'}});
  const auto freeDos = rufus::core::stageFat32Media(
      target, freeDosOutput, "FREEDOS",
      rufus::core::StandaloneBootMode::FreeDos, std::move(files));
  expect(freeDos.success && freeDos.stagedImage.has_value() &&
             freeDos.stagedImage->bootable &&
             freeDos.stagedImage->capabilities.biosBootable,
         "FreeDOS payloads should stage with a bootable FAT32 PBR: " +
             freeDos.error);
  std::ifstream freeDosInput(freeDosOutput, std::ios::binary);
  std::vector<unsigned char> freeDosBytes(
      static_cast<std::size_t>(freeDos.stagedImage->sizeBytes));
  freeDosInput.read(reinterpret_cast<char*>(freeDosBytes.data()),
                    static_cast<std::streamsize>(freeDosBytes.size()));
  const std::string bootMessage = "Loading FreeDOS";
  const std::string kernelPayload = "KERNEL";
  expect(freeDosInput &&
             std::search(freeDosBytes.begin(), freeDosBytes.end(),
                         bootMessage.begin(), bootMessage.end()) !=
                 freeDosBytes.end() &&
             std::search(freeDosBytes.begin(), freeDosBytes.end(),
                         kernelPayload.begin(), kernelPayload.end()) !=
                 freeDosBytes.end(),
         "FreeDOS staging should retain its bootstrap and payload bytes");

  const auto msDosOutput = temporary.path() / "msdos-fat32.img";
  std::vector<rufus::core::StandaloneMediaFile> msDosFiles;
  msDosFiles.push_back({"IO.SYS", {'M', 'S', '-', 'D', 'O', 'S', '-', 'I', 'O'}});
  msDosFiles.push_back({"MSDOS.SYS", {'[', 'O', 'p', 't', 'i', 'o', 'n', 's', ']'}});
  msDosFiles.push_back({"COMMAND.COM", {'M', 'S', '-', 'D', 'O', 'S', '-', 'S', 'H', 'E', 'L', 'L'}});
  const auto msDos = rufus::core::stageFat32Media(
      target, msDosOutput, "MSDOS",
      rufus::core::StandaloneBootMode::MsDos, std::move(msDosFiles));
  expect(msDos.success && msDos.stagedImage.has_value() &&
             msDos.stagedImage->bootable &&
             msDos.stagedImage->capabilities.biosBootable,
         "user-supplied MS-DOS 7/8 files should stage with a bootable FAT32 PBR: " +
             msDos.error);
  std::ifstream msDosInput(msDosOutput, std::ios::binary);
  const std::string msDosBytes{
      std::istreambuf_iterator<char>(msDosInput),
      std::istreambuf_iterator<char>()};
  expect(msDosBytes.find("Invalid system disk") != std::string::npos &&
             msDosBytes.find("MS-DOS-SHELL") != std::string::npos,
         "MS-DOS staging should retain its compatible bootstrap and supplied payloads");

  const auto incompleteOutput = temporary.path() / "incomplete-msdos.img";
  std::vector<rufus::core::StandaloneMediaFile> incompleteFiles;
  incompleteFiles.push_back({"IO.SYS", {'I', 'O'}});
  const auto incompleteMsDos = rufus::core::stageFat32Media(
      target, incompleteOutput, "MSDOS",
      rufus::core::StandaloneBootMode::MsDos, std::move(incompleteFiles));
  expect(!incompleteMsDos.success && !std::filesystem::exists(incompleteOutput),
         "MS-DOS staging must reject an incomplete user-supplied system-file set");

  const auto grubOutput = temporary.path() / "grub2-fat32.img";
  const auto grub = rufus::core::stageFat32Media(
      target, grubOutput, "GRUB2",
      rufus::core::StandaloneBootMode::Grub2, {});
  expect(grub.success && grub.stagedImage.has_value() &&
             grub.stagedImage->capabilities.biosBootable,
         "blank GRUB2 media should stage the embedded i386-pc bootstrap: " +
             grub.error);
  std::ifstream grubInput(grubOutput, std::ios::binary);
  std::array<unsigned char, 512> grubMbr{};
  grubInput.read(reinterpret_cast<char*>(grubMbr.data()), grubMbr.size());
  expect(grubInput && grubMbr[0] == 0xebU && grubMbr[510] == 0x55U &&
             grubMbr[511] == 0xaaU,
         "blank GRUB2 media should contain a signed GRUB MBR");

  const auto grub4DosOutput = temporary.path() / "grub4dos-fat32.img";
  std::vector<rufus::core::StandaloneMediaFile> grub4DosFiles;
  grub4DosFiles.push_back(
      {"GRLDR", {'U', 'S', 'E', 'R', '-', 'G', 'R', 'L', 'D', 'R'}});
  const auto grub4Dos = rufus::core::stageFat32Media(
      target, grub4DosOutput, "GRUB4DOS",
      rufus::core::StandaloneBootMode::Grub4Dos,
      std::move(grub4DosFiles));
  expect(grub4Dos.success && grub4Dos.stagedImage.has_value() &&
             grub4Dos.stagedImage->capabilities.biosBootable,
         "a user-supplied GRLDR should stage with the embedded Grub4DOS bootstrap: " +
             grub4Dos.error);
  std::ifstream grub4DosInput(grub4DosOutput, std::ios::binary);
  const std::string grub4DosBytes{
      std::istreambuf_iterator<char>(grub4DosInput),
      std::istreambuf_iterator<char>()};
  expect(grub4DosBytes.find("GRLDR") != std::string::npos &&
             grub4DosBytes.find("USER-GRLDR") != std::string::npos,
         "Grub4DOS media should retain its secondary record and supplied loader");

  const auto reactOsOutput = temporary.path() / "reactos-fat32.img";
  std::vector<rufus::core::StandaloneMediaFile> reactOsFiles;
  reactOsFiles.push_back(
      {"FREELDR.SYS", {'U', 'S', 'E', 'R', '-', 'F', 'R', 'E', 'E', 'L', 'D', 'R'}});
  const auto reactOs = rufus::core::stageFat32Media(
      target, reactOsOutput, "REACTOS",
      rufus::core::StandaloneBootMode::ReactOs,
      std::move(reactOsFiles));
  expect(reactOs.success && reactOs.stagedImage.has_value() &&
             reactOs.stagedImage->capabilities.biosBootable,
         "a user-supplied FREELDR.SYS should stage with the embedded ReactOS bootstrap: " +
             reactOs.error);
  std::ifstream reactOsInput(reactOsOutput, std::ios::binary);
  const std::string reactOsBytes{
      std::istreambuf_iterator<char>(reactOsInput),
      std::istreambuf_iterator<char>()};
  expect(reactOsBytes.find("FREELDR SYS") != std::string::npos &&
             reactOsBytes.find("USER-FREELDR") != std::string::npos,
         "ReactOS media should retain its FAT32 bootstrap and supplied loader");

  const auto syslinuxOutput = temporary.path() / "syslinux-fat32.img";
  const auto syslinux = rufus::core::stageFat32Media(
      target, syslinuxOutput, "SYSLINUX",
      rufus::core::StandaloneBootMode::Syslinux, {});
  expect(syslinux.success && syslinux.stagedImage.has_value() &&
             syslinux.stagedImage->capabilities.biosBootable,
         "blank Syslinux media should stage its patched loader and FAT32 boot sector: " +
             syslinux.error);
  std::ifstream syslinuxInput(syslinuxOutput, std::ios::binary);
  const std::string syslinuxBytes{
      std::istreambuf_iterator<char>(syslinuxInput),
      std::istreambuf_iterator<char>()};
  expect(syslinuxBytes.find("SYSLINUX 4.07") != std::string::npos &&
             syslinuxBytes.find("Syslinux media created by Rufus++") !=
                 std::string::npos,
         "Syslinux media should contain the embedded loader and prompt configuration");
  const std::size_t loaderStart = syslinuxBytes.find("\r\nSYSLINUX 4.07");
  const auto little32 = [&syslinuxBytes](const std::size_t offset) {
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(syslinuxBytes[offset])) |
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(syslinuxBytes[offset + 1U])) << 8U |
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(syslinuxBytes[offset + 2U])) << 16U |
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(syslinuxBytes[offset + 3U])) << 24U;
  };
  std::uint32_t syslinuxChecksum = 0U;
  if (loaderStart != std::string::npos &&
      loaderStart + 36488U <= syslinuxBytes.size()) {
    for (std::size_t offset = loaderStart;
         offset + 4U <= loaderStart + 36488U; offset += 4U) {
      syslinuxChecksum += little32(offset);
    }
  }
  std::ifstream syslinuxBootInput(syslinuxOutput, std::ios::binary);
  std::array<char, 8> syslinuxOem{};
  syslinuxBootInput.seekg(2048ULL * 512ULL + 3ULL);
  syslinuxBootInput.read(syslinuxOem.data(), syslinuxOem.size());
  expect(loaderStart != std::string::npos &&
             syslinuxChecksum == 0x3eb202feU && syslinuxBootInput &&
             std::string(syslinuxOem.data(), syslinuxOem.size()) == "SYSLINUX",
         "Syslinux staging should patch a checksum-valid loader and executable PBR");

  const auto uefiOutput = temporary.path() / "uefi-fat32.img";
  std::vector<rufus::core::StandaloneMediaFile> uefiFiles;
  uefiFiles.push_back(
      {"EFI/BOOT/BOOTX64.EFI", {'U', 'S', 'E', 'R', '-', 'U', 'E', 'F', 'I'}});
  const auto uefi = rufus::core::stageFat32Media(
      target, uefiOutput, "UEFI",
      rufus::core::StandaloneBootMode::Uefi, std::move(uefiFiles));
  expect(uefi.success && uefi.stagedImage.has_value() &&
             uefi.stagedImage->capabilities.uefiBootable &&
             !uefi.stagedImage->capabilities.biosBootable,
         "a user-supplied fallback application should stage as blank UEFI media: " +
             uefi.error);

  auto ext2Target = target;
  ext2Target.capacityBytes = 512ULL * 1024ULL * 1024ULL;
  const auto ext2Output = temporary.path() / "blank-ext2.img";
  const auto ext2 = rufus::core::stageBlankExt2Media(
      ext2Target, ext2Output, "PORTABLE_EXT2");
  expect(ext2.success && ext2.stagedImage.has_value() &&
             ext2.stagedImage->sizeBytes == ext2Target.capacityBytes &&
             std::filesystem::file_size(ext2Output) == ext2Target.capacityBytes,
         "the portable ext2 formatter should stage a verified full-target sparse image: " +
             ext2.error);
  std::ifstream ext2Input(ext2Output, std::ios::binary);
  std::array<unsigned char, 2> ext2Magic{};
  ext2Input.seekg(1024ULL * 1024ULL + 1024ULL + 56ULL);
  ext2Input.read(reinterpret_cast<char*>(ext2Magic.data()), ext2Magic.size());
  expect(ext2Input && ext2Magic[0] == 0x53U && ext2Magic[1] == 0xefU,
         "the standalone ext2 partition should contain the ext2 superblock magic");
}

using rufus::tests::writeIsoFixture;

enum class GrubFixtureLayout { BootGrub, BootGrub2, RootGrub };
enum class LinuxFixturePersistence { Casper, DebianLive, Unsupported };

void writeLinuxPersistenceIsoFixture(
    const std::filesystem::path& path,
    const GrubFixtureLayout grubLayout = GrubFixtureLayout::BootGrub,
    const LinuxFixturePersistence persistence =
        LinuxFixturePersistence::Casper) {
  constexpr std::uint32_t sectorSize = 2048;
  std::vector<unsigned char> bytes(30U * sectorSize, 0);
  auto writeBoth32 = [&bytes](const std::size_t offset,
                              const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      bytes[offset + index] =
          static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
      bytes[offset + 4U + index] =
          static_cast<unsigned char>((value >> ((3U - index) * 8U)) & 0xffU);
    }
  };
  auto writeBoth16 = [&bytes](const std::size_t offset,
                              const std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1U] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 2U] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 3U] = static_cast<unsigned char>(value & 0xffU);
  };
  auto record = [&bytes, &writeBoth32](const std::size_t offset,
                                       const std::uint32_t extent,
                                       const std::uint32_t size,
                                       const unsigned char flags,
                                       const std::vector<unsigned char>& name) {
    const std::size_t length =
        33U + name.size() + (name.size() % 2U == 0U ? 1U : 0U);
    bytes[offset] = static_cast<unsigned char>(length);
    writeBoth32(offset + 2U, extent);
    writeBoth32(offset + 10U, size);
    bytes[offset + 25U] = flags;
    bytes[offset + 28U] = 1;
    bytes[offset + 31U] = 1;
    bytes[offset + 32U] = static_cast<unsigned char>(name.size());
    std::copy(name.begin(), name.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33U));
    return length;
  };
  auto ascii = [](const std::string& value) {
    return std::vector<unsigned char>(value.begin(), value.end());
  };
  auto writeDirectory = [&](const std::size_t sector,
                            const std::size_t parentSector,
                            const std::vector<std::tuple<std::string,
                                                         std::uint32_t,
                                                         std::uint32_t,
                                                         unsigned char>>& entries) {
    std::size_t offset = sector * sectorSize;
    offset += record(offset, static_cast<std::uint32_t>(sector), sectorSize,
                     0x02, {0});
    offset += record(offset, static_cast<std::uint32_t>(parentSector),
                     sectorSize, 0x02, {1});
    for (const auto& [name, extent, size, flags] : entries) {
      offset += record(offset, extent, size, flags, ascii(name));
    }
  };
  auto descriptor = [&](const std::size_t sector, const unsigned char type) {
    const std::size_t offset = sector * sectorSize;
    bytes[offset] = type;
    constexpr std::array<char, 5> identifier = {'C', 'D', '0', '0', '1'};
    std::copy(identifier.begin(), identifier.end(), bytes.begin() + offset + 1U);
    bytes[offset + 6U] = 1;
    return offset;
  };

  const std::size_t primary = descriptor(16, 1);
  const std::string label = "RUFUSPP_LINUX_TEST";
  std::copy(label.begin(), label.end(), bytes.begin() + primary + 40U);
  writeBoth32(primary + 80U, static_cast<std::uint32_t>(bytes.size() / sectorSize));
  writeBoth16(primary + 128U, sectorSize);
  record(primary + 156U, 18, sectorSize, 0x02, {0});
  descriptor(17, 255);

  std::string config;
  switch (persistence) {
    case LinuxFixturePersistence::Casper:
      config =
          "menuentry 'Try Linux' {\n  linux /casper/vmlinuz quiet splash\n}\n";
      break;
    case LinuxFixturePersistence::DebianLive:
      config = "menuentry 'Try Linux' {\n  linux /live/vmlinuz boot=live "
               "components quiet\n}\n";
      break;
    case LinuxFixturePersistence::Unsupported:
      config = "menuentry 'Try Linux' {\n  linux /images/pxeboot/vmlinuz "
               "rd.live.image quiet\n}\n";
      break;
  }
  const bool rootGrub = grubLayout == GrubFixtureLayout::RootGrub;
  const std::string grubDirectory =
      grubLayout == GrubFixtureLayout::BootGrub2 ? "GRUB2" : "GRUB";
  const std::string grubManifestDirectory =
      grubLayout == GrubFixtureLayout::BootGrub2
          ? "boot/grub2"
          : rootGrub ? "grub" : "boot/grub";
  const std::string manifest =
      rufus::core::detail::md5Hex(config) + "  ./" +
      grubManifestDirectory + "/grub.cfg\n";
  std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t,
                         unsigned char>> rootEntries{
      {"EFI", 19, sectorSize, 0x02},
      {persistence == LinuxFixturePersistence::Casper ? "CASPER" : "LIVE",
       25, sectorSize, 0x02},
      {"MD5SUM.TXT;1", 27, static_cast<std::uint32_t>(manifest.size()), 0}};
  rootEntries.emplace_back(rootGrub ? grubDirectory : "BOOT",
                           rootGrub ? 23U : 22U, sectorSize, 0x02);
  writeDirectory(18, 18, rootEntries);
  writeDirectory(19, 18, {{"BOOT", 20, sectorSize, 0x02}});
  writeDirectory(20, 19, {{"BOOTX64.EFI;1", 21, 4, 0}});
  if (!rootGrub) {
    writeDirectory(22, 18, {{grubDirectory, 23, sectorSize, 0x02}});
  }
  writeDirectory(23, rootGrub ? 18 : 22,
                 {{"GRUB.CFG;1", 24,
                   static_cast<std::uint32_t>(config.size()), 0},
                  {"I386-PC", 28, sectorSize, 0x02}});
  writeDirectory(28, 23, {{"NORMAL.MOD;1", 29, 4, 0}});
  writeDirectory(25, 18, {{"FILESYSTEM.SQUASHFS;1", 26, 4, 0}});
  std::copy(config.begin(), config.end(), bytes.begin() + 24U * sectorSize);
  std::copy(manifest.begin(), manifest.end(), bytes.begin() + 27U * sectorSize);
  for (const std::size_t sector : {21U, 26U, 29U}) {
    bytes[sector * sectorSize] = 't';
    bytes[sector * sectorSize + 1U] = 'e';
    bytes[sector * sectorSize + 2U] = 's';
    bytes[sector * sectorSize + 3U] = 't';
  }
  writeBytes(path, bytes);
}

void writeUdfFixture(const std::filesystem::path& path) {
  constexpr std::size_t blockSize = 2048;
  std::vector<unsigned char> bytes(265U * blockSize, 0);
  auto write16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1U] = static_cast<unsigned char>(value >> 8U);
  };
  auto write32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };
  auto write64 = [&bytes](const std::size_t offset, const std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
      bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };
  auto crc16 = [&bytes](const std::size_t offset, const std::size_t size) {
    std::uint16_t crc = 0;
    for (std::size_t index = 0; index < size; ++index) {
      crc ^= static_cast<std::uint16_t>(bytes[offset + index]) << 8U;
      for (unsigned int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000U) != 0
                  ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                  : static_cast<std::uint16_t>(crc << 1U);
      }
    }
    return crc;
  };
  auto writeTag = [&bytes, &write16, &write32, &crc16](
                      const std::size_t offset, const std::uint16_t id,
                      const std::uint32_t location, const std::uint16_t crcLength) {
    write16(offset, id);
    write16(offset + 2U, 2);
    write16(offset + 6U, 1);
    write16(offset + 10U, crcLength);
    write32(offset + 12U, location);
    write16(offset + 8U, crc16(offset + 16U, crcLength));
    unsigned int checksum = 0;
    for (std::size_t index = 0; index < 16; ++index) {
      if (index != 4) {
        checksum += bytes[offset + index];
      }
    }
    bytes[offset + 4U] = static_cast<unsigned char>(checksum);
  };
  auto writeRecognition = [&bytes, blockSize](const std::size_t sector,
                                               const std::string& identifier) {
    const std::size_t offset = sector * blockSize;
    bytes[offset] = 0;
    std::copy(identifier.begin(), identifier.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1U));
    bytes[offset + 6U] = 1;
  };

  writeRecognition(16, "BEA01");
  writeRecognition(17, "NSR03");
  writeRecognition(18, "TEA01");

  const std::size_t anchor = 256U * blockSize;
  write32(anchor + 16U, 3U * blockSize);
  write32(anchor + 20U, 257);
  writeTag(anchor, 2, 256, 496);

  const std::size_t partition = 257U * blockSize;
  write16(partition + 20U, 1);
  write16(partition + 22U, 0);
  write32(partition + 188U, 260);
  write32(partition + 192U, 5);
  writeTag(partition, 5, 257, 496);

  const std::size_t volume = 258U * blockSize;
  const std::string label = "RUFUSPP_UDF_TEST";
  bytes[volume + 84U] = 8;
  std::copy(label.begin(), label.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(volume + 85U));
  bytes[volume + 84U + 127U] = static_cast<unsigned char>(label.size() + 1U);
  write32(volume + 212U, blockSize);
  write32(volume + 248U, blockSize);
  write32(volume + 252U, 0);
  write16(volume + 256U, 0);
  write32(volume + 264U, 6);
  write32(volume + 268U, 1);
  bytes[volume + 440U] = 1;
  bytes[volume + 441U] = 6;
  write16(volume + 442U, 1);
  write16(volume + 444U, 0);
  writeTag(volume, 6, 258, 496);

  writeTag(259U * blockSize, 8, 259, 496);

  const std::size_t fileSet = 260U * blockSize;
  write32(fileSet + 400U, blockSize);
  write32(fileSet + 404U, 1);
  write16(fileSet + 408U, 0);
  writeTag(fileSet, 256, 0, 496);

  const std::string filename = "README.TXT";
  const std::size_t nameLength = filename.size() + 1U;
  const std::size_t identifierLength = (38U + nameLength + 3U) & ~std::size_t{3U};
  const std::size_t rootEntry = 261U * blockSize;
  bytes[rootEntry + 27U] = 4;
  write64(rootEntry + 56U, identifierLength);
  write32(rootEntry + 168U, 0);
  write32(rootEntry + 172U, 8);
  write32(rootEntry + 176U, blockSize);
  write32(rootEntry + 180U, 2);
  writeTag(rootEntry, 261, 1, 160);

  const std::size_t identifier = 262U * blockSize;
  write16(identifier + 16U, 1);
  bytes[identifier + 18U] = 0;
  bytes[identifier + 19U] = static_cast<unsigned char>(nameLength);
  write32(identifier + 20U, blockSize);
  write32(identifier + 24U, 3);
  write16(identifier + 28U, 0);
  write16(identifier + 36U, 0);
  bytes[identifier + 38U] = 8;
  std::copy(filename.begin(), filename.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(identifier + 39U));
  writeTag(identifier, 257, 2, static_cast<std::uint16_t>(identifierLength - 16U));

  const std::size_t fileEntry = 263U * blockSize;
  bytes[fileEntry + 27U] = 5;
  write64(fileEntry + 56U, 4);
  write16(fileEntry + 34U, 3);
  write32(fileEntry + 168U, 0);
  write32(fileEntry + 172U, 4);
  const std::string contents = "udf!";
  std::copy(contents.begin(), contents.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(fileEntry + 176U));
  writeTag(fileEntry, 261, 3, 164);
  writeBytes(path, bytes);
}

void writeJolietIsoFixture(const std::filesystem::path& path) {
  constexpr std::size_t sectorSize = 2048;
  std::vector<unsigned char> bytes(23U * sectorSize, 0);
  auto writeBoth16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1U] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 2U] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 3U] = static_cast<unsigned char>(value & 0xffU);
  };
  auto writeBoth32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
      bytes[offset + 4U + index] =
          static_cast<unsigned char>((value >> ((3U - index) * 8U)) & 0xffU);
    }
  };
  auto record = [&bytes, &writeBoth32](const std::size_t offset, const std::uint32_t extent,
                                       const std::uint32_t size, const unsigned char flags,
                                       const std::vector<unsigned char>& name) {
    const std::size_t length = 33U + name.size() + (name.size() % 2U == 0 ? 1U : 0U);
    bytes[offset] = static_cast<unsigned char>(length);
    writeBoth32(offset + 2U, extent);
    writeBoth32(offset + 10U, size);
    bytes[offset + 25U] = flags;
    bytes[offset + 28U] = 1;
    bytes[offset + 31U] = 1;
    bytes[offset + 32U] = static_cast<unsigned char>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33U));
    return length;
  };
  auto descriptor = [&bytes, sectorSize](const std::size_t sector,
                                          const unsigned char type) {
    const std::size_t offset = sector * sectorSize;
    bytes[offset] = type;
    const std::string identifier = "CD001";
    std::copy(identifier.begin(), identifier.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1U));
    bytes[offset + 6U] = 1;
    return offset;
  };
  const std::size_t primary = descriptor(16, 1);
  writeBoth16(primary + 128U, sectorSize);
  record(primary + 156U, 20, sectorSize, 0x02, {0});

  const std::size_t joliet = descriptor(17, 2);
  writeBoth16(joliet + 128U, sectorSize);
  bytes[joliet + 88U] = '%';
  bytes[joliet + 89U] = '/';
  bytes[joliet + 90U] = 'E';
  const std::string label = "JOLIET_TEST";
  for (std::size_t index = 0; index < label.size(); ++index) {
    bytes[joliet + 40U + index * 2U + 1U] = static_cast<unsigned char>(label[index]);
  }
  record(joliet + 156U, 21, sectorSize, 0x02, {0});
  descriptor(18, 255);

  auto dots = [&record, sectorSize](const std::size_t sector) {
    std::size_t offset = sector * sectorSize;
    offset += record(offset, static_cast<std::uint32_t>(sector), sectorSize, 0x02, {0});
    record(offset, static_cast<std::uint32_t>(sector), sectorSize, 0x02, {1});
  };
  dots(20);
  std::size_t directoryOffset = 21U * sectorSize;
  directoryOffset += record(directoryOffset, 21, sectorSize, 0x02, {0});
  directoryOffset += record(directoryOffset, 21, sectorSize, 0x02, {1});
  const std::vector<unsigned char> unicodeName{
      0x00, 'R', 0x00, 0xe9, 0x00, 's', 0x00, 'u', 0x00, 'm', 0x00, 0xe9,
      0x00, '.', 0x00, 't', 0x00, 'x', 0x00, 't', 0x00, ';', 0x00, '1'};
  record(directoryOffset, 22, 4, 0, unicodeName);
  const std::string contents = "test";
  std::copy(contents.begin(), contents.end(), bytes.begin() + 22U * sectorSize);
  writeBytes(path, bytes);
}

void testMediaPolicy() {
  rufus::core::BlockDeviceInfo device;
  device.stableId = "test-device";
  device.devicePath = "/dev/rufus-test-device";
  device.capacityBytes = 16U * 1024U;
  device.logicalSectorSize = 512;
  device.removable = true;
  device.writable = true;
  device.wholeDevice = true;

  expect(rufus::core::evaluateDeviceEligibility(device) ==
             rufus::core::DeviceEligibility::Eligible,
         "safe removable whole device should be eligible");

  rufus::core::ImageInfo image;
  image.path = "/tmp/rufus-test-source.img";
  image.sizeBytes = 8U * 1024U;
  image.format = rufus::core::ImageFormat::Raw;

  const rufus::core::SafetyPolicy policy;
  expect(policy.validateWrite(image, device).safe(),
         "recognized image and eligible target should pass portable safety policy");

  device.systemDevice = true;
  expect(!policy.validateWrite(image, device).safe(), "system device must fail safety policy");
  device.systemDevice = false;

  auto changed = device;
  changed.capacityBytes += 512;
  expect(!policy.validateIdentity(device, changed).safe(),
         "capacity change must invalidate selected device identity");

  rufus::core::ImageInfo compressed = image;
  compressed.format = rufus::core::ImageFormat::Zstd;
  compressed.compressed = true;
  compressed.capabilities.validContainerMetadata = true;
  compressed.capabilities.compressedSizeKnown = true;
  compressed.expandedSizeBytes = device.capacityBytes + 512U;
  const auto compressedCheck = policy.validateWrite(compressed, device);
  expect(std::none_of(compressedCheck.issues.begin(), compressedCheck.issues.end(),
                      [](const rufus::core::SafetyIssue& issue) {
                        return issue.code == rufus::core::SafetyIssueCode::ExpandedSizeUnknown;
                      }),
         "known compressed sizes should not be reported as unknown");
  expect(std::any_of(compressedCheck.issues.begin(), compressedCheck.issues.end(),
                     [](const rufus::core::SafetyIssue& issue) {
                       return issue.code == rufus::core::SafetyIssueCode::DeviceTooSmall;
                     }),
         "target capacity checks should use a known expanded size");
}

void testImageAnalysis(const TemporaryDirectory& temporary) {
  const rufus::core::ImageAnalyzer analyzer;

  const auto isoPath = temporary.path() / "boot.iso";
  writeIsoFixture(isoPath);
  const auto iso = analyzer.analyze(isoPath);
  expect(iso.succeeded(), "ISO fixture should analyze successfully");
  expect(iso.image->format == rufus::core::ImageFormat::Iso,
         "ISO-9660 signature should be detected");
  expect(iso.image->bootable, "El Torito boot descriptor should mark ISO bootable");
  expect(iso.image->capabilities.validBootCatalog &&
             iso.image->capabilities.biosBootable,
         "a checksummed El Torito catalog should identify BIOS boot support");
  expect(iso.image->volumeLabel == "RUFUSPP_QT_TEST", "ISO volume label should be extracted");
  expect(iso.image->capabilities.isoExtraction,
         "a readable ISO directory should enable extracted-file mode");
  expect(iso.image->family == rufus::core::ImageFamily::WindowsInstaller,
         "nested Windows boot files should be detected from the ISO directory tree");
  expect(iso.image->capabilities.windowsImageMetadata &&
             iso.image->windowsImageCount == 1 && iso.image->windowsBootIndex == 1 &&
             iso.image->windowsVersionMajor == 10 && iso.image->windowsBuild == 22621,
         "WIM header and XML metadata should be validated inside the ISO");
  expect(iso.image->architecture == rufus::core::ImageArchitecture::X64,
         "WIM and EFI loader architecture should identify x86-64 media");
  expect(iso.image->capabilities.windowsToGo,
         "validated Windows metadata and EFI support should enable Windows To Go");
  expect(iso.image->windowsEditions.size() == 1 &&
             iso.image->windowsEditions.front().index == 1 &&
             iso.image->windowsEditions.front().name == "Windows 11 Test & Tools" &&
             iso.image->windowsEditions.front().description == "Test edition metadata" &&
             iso.image->windowsEditions.front().totalBytes == 7516192768ULL &&
             iso.image->windowsEditions.front().architecture ==
                 rufus::core::ImageArchitecture::X64,
         "WIM edition names, descriptions, sizes, and architectures should be retained");

  const auto malformedCatalogPath = temporary.path() / "malformed-catalog.iso";
  writeIsoFixture(malformedCatalogPath, true, false);
  const auto malformedCatalog = analyzer.analyze(malformedCatalogPath);
  expect(malformedCatalog.succeeded() &&
             !malformedCatalog.image->capabilities.validBootCatalog,
         "a corrupt El Torito checksum must not validate the boot catalog");
  expect(std::any_of(malformedCatalog.warnings.begin(), malformedCatalog.warnings.end(),
                     [](const std::string& warning) {
                       return warning.find("El Torito") != std::string::npos;
                     }),
         "a corrupt El Torito catalog should produce a diagnostic");

  const auto malformedWimPath = temporary.path() / "malformed-wim.iso";
  writeIsoFixture(malformedWimPath, false, true);
  const auto malformedWim = analyzer.analyze(malformedWimPath);
  expect(malformedWim.succeeded() &&
             !malformedWim.image->capabilities.standardWindowsInstallation &&
             !malformedWim.image->capabilities.windowsImageMetadata &&
             !malformedWim.image->capabilities.windowsToGo,
         "invalid WIM metadata must disable metadata-dependent Windows To Go");

  std::vector<unsigned char> gptBytes(80U * 512U, 0);
  auto writeGpt32 = [&gptBytes](const std::size_t offset, const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      gptBytes[offset + index] =
          static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };
  auto writeGpt64 = [&gptBytes](const std::size_t offset, const std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
      gptBytes[offset + index] =
          static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };
  auto gptCrc32 = [&gptBytes](const std::size_t offset, const std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
      crc ^= gptBytes[offset + index];
      for (unsigned int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
      }
    }
    return ~crc;
  };
  gptBytes[510] = 0x55;
  gptBytes[511] = 0xaa;
  gptBytes[446 + 4] = 0xee;
  writeGpt32(446 + 8, 1);
  writeGpt32(446 + 12, 79);
  const std::string signature = "EFI PART";
  std::copy(signature.begin(), signature.end(), gptBytes.begin() + 512);
  writeGpt32(512 + 8, 0x00010000U);
  writeGpt32(512 + 12, 92);
  writeGpt64(512 + 24, 1);
  writeGpt64(512 + 32, 79);
  writeGpt64(512 + 40, 3);
  writeGpt64(512 + 48, 78);
  writeGpt64(512 + 72, 2);
  writeGpt32(512 + 80, 4);
  writeGpt32(512 + 84, 128);
  const std::array<unsigned char, 16> efiSystemPartition{
      0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  std::copy(efiSystemPartition.begin(), efiSystemPartition.end(), gptBytes.begin() + 1024);
  gptBytes[1024 + 16] = 1;
  writeGpt64(1024 + 32, 3);
  writeGpt64(1024 + 40, 10);
  writeGpt32(512 + 88, gptCrc32(1024, 512));
  writeGpt32(512 + 16, gptCrc32(512, 92));
  const auto gptPath = temporary.path() / "disk.img";
  writeBytes(gptPath, gptBytes);
  const auto gpt = analyzer.analyze(gptPath);
  expect(gpt.succeeded(), "raw GPT fixture should analyze successfully");
  expect(gpt.image->format == rufus::core::ImageFormat::Raw,
         "IMG extension should be classified as raw image");
  expect(gpt.image->partitionScheme == rufus::core::PartitionScheme::Gpt,
         "GPT signature should be detected");
  expect(gpt.image->capabilities.validPartitionTable &&
             gpt.image->capabilities.uefiBootable,
         "a checksummed GPT with an in-range ESP should identify UEFI media");

  auto invalidGptBytes = gptBytes;
  invalidGptBytes[1024 + 40] ^= 1U;
  const auto invalidGptPath = temporary.path() / "invalid-gpt.img";
  writeBytes(invalidGptPath, invalidGptBytes);
  const auto invalidGpt = analyzer.analyze(invalidGptPath);
  expect(invalidGpt.succeeded() &&
             invalidGpt.image->partitionScheme == rufus::core::PartitionScheme::Unknown &&
             !invalidGpt.image->capabilities.validPartitionTable,
         "a GPT entry-array checksum mismatch must invalidate the partition table");
  expect(gpt.image->capabilities.validPartitionTable &&
             gpt.image->capabilities.uefiBootable,
         "GPT header, entry CRC, and EFI System Partition should validate");

  const auto udfPath = temporary.path() / "standalone.udf";
  writeUdfFixture(udfPath);
  const auto udf = analyzer.analyze(udfPath);
  expect(udf.succeeded() && udf.image->format == rufus::core::ImageFormat::Iso,
         "a standalone UDF volume should be recognized as an optical image");
  expect(udf.image->capabilities.udf && udf.image->capabilities.isoExtraction,
         "a validated UDF directory tree should enable extracted-file mode");
  expect(udf.image->volumeLabel == "RUFUSPP_UDF_TEST",
         "UDF logical volume label should be extracted");

  const auto jolietPath = temporary.path() / "joliet.iso";
  writeJolietIsoFixture(jolietPath);
  const auto joliet = analyzer.analyze(jolietPath);
  expect(joliet.succeeded() && joliet.image->capabilities.iso9660 &&
             joliet.image->capabilities.joliet && joliet.image->capabilities.isoExtraction,
         "a valid Joliet supplementary tree should be preferred for ISO extraction");
  expect(joliet.image->volumeLabel == "JOLIET_TEST",
         "Joliet UCS-2 volume labels should be decoded");

  const auto zstdPath = temporary.path() / "disk.img.zst";
  writeBytes(zstdPath,
             {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x04, 0x21, 0x00, 0x00,
              't', 'e', 's', 't'});
  const auto zstd = analyzer.analyze(zstdPath);
  expect(zstd.succeeded() && zstd.image->format == rufus::core::ImageFormat::Zstd,
         "a valid Zstandard frame should be recognized");
  expect(zstd.image->capabilities.validContainerMetadata &&
             zstd.image->capabilities.compressedSizeKnown &&
             zstd.image->expandedSizeBytes == 4,
         "Zstandard declared content size should be validated and reported");

  const auto xzPath = temporary.path() / "disk.img.xz";
  writeBytes(xzPath,
             {0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46,
              0x04, 0xc0, 0x08, 0x04, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x00, 0x00, 0x4c, 0x41, 0xbc, 0x27, 0x01, 0x00, 0x03, 0x74,
              0x65, 0x73, 0x74, 0x00, 0xa5, 0x75, 0x0c, 0xc1, 0xa7, 0xfd, 0x15, 0xfa,
              0x00, 0x01, 0x24, 0x04, 0x94, 0x90, 0x03, 0xd6, 0x1f, 0xb6, 0xf3, 0x7d,
              0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x59, 0x5a});
  const auto xz = analyzer.analyze(xzPath);
  expect(xz.succeeded() && xz.image->capabilities.compressedSizeKnown &&
             xz.image->expandedSizeBytes == 4,
         "checksummed XZ stream indexes should provide an exact expanded size");

  const auto gzipPath = temporary.path() / "disk.img.gz";
  writeBytes(gzipPath,
             {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x2b, 0x49,
              0x2d, 0x2e, 0x01, 0x00, 0x0c, 0x7e, 0x7f, 0xd8, 0x04, 0x00, 0x00, 0x00});
  const auto gzip = analyzer.analyze(gzipPath);
  expect(gzip.succeeded() && gzip.image->capabilities.validContainerMetadata &&
             gzip.image->capabilities.compressedSizeKnown &&
             gzip.image->expandedSizeBytes == 4,
         "gzip should be fully decoded to resolve and validate its exact expanded size");

  const auto bzip2Path = temporary.path() / "disk.img.bz2";
  writeBytes(bzip2Path,
             {0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59,
              0x33, 0x8b, 0xcf, 0xac, 0x00, 0x00, 0x01, 0x01, 0x80, 0x02,
              0x00, 0x0c, 0x00, 0x20, 0x00, 0x21, 0x98, 0x19, 0x84, 0x18,
              0x5d, 0xc9, 0x14, 0xe1, 0x42, 0x40, 0xce, 0x2f, 0x3e, 0xb0});
  const auto bzip2 = analyzer.analyze(bzip2Path);
  expect(bzip2.succeeded() &&
             bzip2.image->format == rufus::core::ImageFormat::Bzip2 &&
             bzip2.image->capabilities.validContainerMetadata &&
             bzip2.image->expandedSizeBytes == 4,
         "bzip2 images should receive a checksummed decompression preflight");

  const auto lzmaPath = temporary.path() / "disk.img.lzma";
  writeBytes(lzmaPath,
             {0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff, 0xff,
              0xff, 0xff, 0xff, 0xff, 0x00, 0x3a, 0x19, 0x4a, 0xce,
              0x26, 0x72, 0x83, 0x9f, 0xff, 0xfb, 0x13, 0x80, 0x00});
  const auto lzma = analyzer.analyze(lzmaPath);
  expect(lzma.succeeded() &&
             lzma.image->format == rufus::core::ImageFormat::Lzma &&
             lzma.image->capabilities.validContainerMetadata &&
             lzma.image->expandedSizeBytes == 4,
         "LZMA-alone images should receive a complete decompression preflight");

  std::vector<unsigned char> zipBytes(30U + 8U + 4U, 0);
  auto appendZip16 = [&zipBytes](const std::uint16_t value) {
    zipBytes.push_back(static_cast<unsigned char>(value & 0xffU));
    zipBytes.push_back(static_cast<unsigned char>(value >> 8U));
  };
  auto appendZip32 = [&zipBytes](const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      zipBytes.push_back(static_cast<unsigned char>((value >> (index * 8U)) & 0xffU));
    }
  };
  zipBytes[0] = 0x50;
  zipBytes[1] = 0x4b;
  zipBytes[2] = 0x03;
  zipBytes[3] = 0x04;
  zipBytes[4] = 20;
  zipBytes[14] = 0x0c;
  zipBytes[15] = 0x7e;
  zipBytes[16] = 0x7f;
  zipBytes[17] = 0xd8;
  zipBytes[18] = 4;
  zipBytes[22] = 4;
  zipBytes[26] = 8;
  const std::string zipName = "disk.img";
  std::copy(zipName.begin(), zipName.end(), zipBytes.begin() + 30);
  const std::string zipContents = "test";
  std::copy(zipContents.begin(), zipContents.end(), zipBytes.begin() + 38);
  const std::uint32_t centralOffset = static_cast<std::uint32_t>(zipBytes.size());
  appendZip32(0x02014b50U);
  appendZip16(20);
  appendZip16(20);
  appendZip16(0);
  appendZip16(0);
  appendZip16(0);
  appendZip16(0);
  appendZip32(0xd87f7e0cU);
  appendZip32(4);
  appendZip32(4);
  appendZip16(static_cast<std::uint16_t>(zipName.size()));
  appendZip16(0);
  appendZip16(0);
  appendZip16(0);
  appendZip16(0);
  appendZip32(0);
  appendZip32(0);
  zipBytes.insert(zipBytes.end(), zipName.begin(), zipName.end());
  const std::uint32_t centralSize =
      static_cast<std::uint32_t>(zipBytes.size()) - centralOffset;
  appendZip32(0x06054b50U);
  appendZip16(0);
  appendZip16(0);
  appendZip16(1);
  appendZip16(1);
  appendZip32(centralSize);
  appendZip32(centralOffset);
  appendZip16(0);
  const auto zipPath = temporary.path() / "disk.zip";
  writeBytes(zipPath, zipBytes);
  const auto zip = analyzer.analyze(zipPath);
  expect(zip.succeeded() && zip.image->capabilities.compressedSizeKnown &&
             zip.image->expandedSizeBytes == 4,
         "ZIP central-directory metadata should provide a validated expanded size");

  std::vector<unsigned char> deflatedZipBytes = zipBytes;
  deflatedZipBytes.erase(deflatedZipBytes.begin() + 38,
                         deflatedZipBytes.begin() + 42);
  const std::array<unsigned char, 6> rawDeflate = {
      0x2b, 0x49, 0x2d, 0x2e, 0x01, 0x00};
  deflatedZipBytes.insert(deflatedZipBytes.begin() + 38,
                          rawDeflate.begin(), rawDeflate.end());
  deflatedZipBytes[8] = 8;
  deflatedZipBytes[18] = 6;
  constexpr std::size_t deflatedCentralOffset = 44;
  deflatedZipBytes[deflatedCentralOffset + 10U] = 8;
  deflatedZipBytes[deflatedCentralOffset + 20U] = 6;
  constexpr std::size_t deflatedEndOffset =
      deflatedCentralOffset + 46U + 8U;
  deflatedZipBytes[deflatedEndOffset + 16U] =
      static_cast<unsigned char>(deflatedCentralOffset);
  const auto deflatedZipPath = temporary.path() / "deflated-disk.zip";
  writeBytes(deflatedZipPath, deflatedZipBytes);
  const auto deflatedZip = analyzer.analyze(deflatedZipPath);
  expect(deflatedZip.succeeded() &&
             deflatedZip.image->capabilities.validContainerMetadata &&
             deflatedZip.image->expandedSizeBytes == 4,
         "a single-file deflated ZIP image should be validated");

  rufus::core::BlockDeviceInfo compressedTarget;
  compressedTarget.stableId = "test:compressed-target:4096";
  compressedTarget.devicePath = "/dev/rufus-compressed-target";
  compressedTarget.displayName = "Virtual compressed-image target";
  compressedTarget.capacityBytes = 4096;
  compressedTarget.logicalSectorSize = 1;
  compressedTarget.bus = rufus::core::DeviceBus::Usb;
  compressedTarget.removable = true;
  compressedTarget.writable = true;
  compressedTarget.wholeDevice = true;
  const rufus::core::WritePlanBuilder compressedPlanner;
  const rufus::core::RawImageWriter compressedWriter;
  const std::array<const rufus::core::ImageAnalysisResult*, 7> compressedImages = {
      &gzip, &bzip2, &zip, &deflatedZip, &lzma, &xz, &zstd};
  for (const auto* compressedImage : compressedImages) {
    expect(compressedImage->image->capabilities.rawWrite,
           "a validated compressed image should enable DD mode");
    const auto plan = compressedPlanner.buildRawWrite(
        *compressedImage->image, compressedTarget, 4096, true);
    expect(plan.succeeded() && plan.plan->bytesToWrite() == 4,
           "a compressed-image plan should use the expanded byte count");
    MemoryRawTarget target(compressedTarget.capacityBytes,
                           compressedTarget.logicalSectorSize);
    const auto result = compressedWriter.write(*plan.plan, target);
    expect(result.success && result.bytesWritten == 4 &&
               std::equal(zipContents.begin(), zipContents.end(),
                          target.bytes().begin()),
           "compressed images should stream, rewind, and verify through the raw writer: " +
               result.error);
  }

  auto corruptGzip = std::vector<unsigned char>{
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x2b, 0x49,
      0x2d, 0x2e, 0x01, 0x00, 0x0d, 0x7e, 0x7f, 0xd8, 0x04, 0x00, 0x00, 0x00};
  const auto corruptGzipPath = temporary.path() / "corrupt.img.gz";
  writeBytes(corruptGzipPath, corruptGzip);
  const auto rejectedGzip = analyzer.analyze(corruptGzipPath);
  expect(rejectedGzip.succeeded() &&
             !rejectedGzip.image->capabilities.validContainerMetadata &&
             !rejectedGzip.image->capabilities.rawWrite,
         "a compressed image with an invalid payload checksum must not enable writing");
  const auto cancelledAnalysis = analyzer.analyze(
      gzipPath, gzipPath.filename().string(), [] { return true; });
  expect(cancelledAnalysis.cancelled && !cancelledAnalysis.succeeded(),
         "image analysis should honor cancellation before touching the source");

  std::vector<unsigned char> vhdBytes(1024, 0);
  const std::size_t footer = 512;
  const std::string vhdCookie = "conectix";
  std::copy(vhdCookie.begin(), vhdCookie.end(),
            vhdBytes.begin() + static_cast<std::ptrdiff_t>(footer));
  auto writeVhdBig32 = [&vhdBytes](const std::size_t offset, const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      vhdBytes[offset + index] =
          static_cast<unsigned char>((value >> ((3U - index) * 8U)) & 0xffU);
    }
  };
  auto writeVhdBig64 = [&vhdBytes](const std::size_t offset, const std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
      vhdBytes[offset + index] =
          static_cast<unsigned char>((value >> ((7U - index) * 8U)) & 0xffU);
    }
  };
  writeVhdBig64(footer + 48U, 512);
  writeVhdBig32(footer + 8U, 2);
  writeVhdBig32(footer + 12U, 0x00010000U);
  writeVhdBig64(footer + 16U, std::numeric_limits<std::uint64_t>::max());
  writeVhdBig32(footer + 60U, 2);
  std::uint32_t footerSum = 0;
  for (std::size_t index = footer; index < footer + 512U; ++index) {
    footerSum += vhdBytes[index];
  }
  writeVhdBig32(footer + 64U, ~footerSum);
  const auto vhdPath = temporary.path() / "disk.vhd";
  writeBytes(vhdPath, vhdBytes);
  const auto vhd = analyzer.analyze(vhdPath);
  expect(vhd.succeeded() && vhd.image->format == rufus::core::ImageFormat::Vhd &&
             vhd.image->capabilities.validContainerMetadata &&
             vhd.image->expandedSizeBytes == 512 &&
             vhd.image->containerPayloadSizeBytes == 512 &&
             vhd.image->capabilities.rawWrite,
         "a fixed checksummed VHD should expose its raw payload for deployment");
  auto vhdSource = rufus::core::openRawImageSource(*vhd.image);
  std::array<unsigned char, 512> vhdPayload{};
  std::string vhdReadError;
  expect(vhdSource.succeeded() && vhdSource.source->sizeBytes() == 512 &&
             vhdSource.source->readAt(0, vhdPayload.data(), vhdPayload.size(),
                                      vhdReadError) &&
             std::equal(vhdPayload.begin(), vhdPayload.end(), vhdBytes.begin()),
         "fixed VHD deployment must omit the trailing container footer: " +
             vhdReadError);

  constexpr std::uint32_t dynamicBlockBytes = 512U * 1024U;
  constexpr std::uint64_t dynamicVirtualBytes = 2ULL * dynamicBlockBytes;
  constexpr std::size_t dynamicHeader = 512U;
  constexpr std::size_t dynamicBat = 1536U;
  constexpr std::size_t dynamicBlock = 2048U;
  constexpr std::size_t dynamicBitmapBytes = 512U;
  const std::size_t dynamicFooter =
      dynamicBlock + dynamicBitmapBytes + dynamicBlockBytes;
  std::vector<unsigned char> dynamicVhdBytes(dynamicFooter + 512U, 0);
  auto writeDynamicBig32 = [&dynamicVhdBytes](const std::size_t offset,
                                               const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      dynamicVhdBytes[offset + index] =
          static_cast<unsigned char>((value >> ((3U - index) * 8U)) & 0xffU);
    }
  };
  auto writeDynamicBig64 = [&dynamicVhdBytes](const std::size_t offset,
                                               const std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
      dynamicVhdBytes[offset + index] =
          static_cast<unsigned char>((value >> ((7U - index) * 8U)) & 0xffU);
    }
  };
  const std::string dynamicCookie = "cxsparse";
  std::copy(dynamicCookie.begin(), dynamicCookie.end(),
            dynamicVhdBytes.begin() + dynamicHeader);
  writeDynamicBig64(dynamicHeader + 8U,
                    std::numeric_limits<std::uint64_t>::max());
  writeDynamicBig64(dynamicHeader + 16U, dynamicBat);
  writeDynamicBig32(dynamicHeader + 24U, 0x00010000U);
  writeDynamicBig32(dynamicHeader + 28U, 2U);
  writeDynamicBig32(dynamicHeader + 32U, dynamicBlockBytes);
  std::uint32_t dynamicHeaderSum = 0;
  for (std::size_t index = dynamicHeader;
       index < dynamicHeader + 1024U; ++index) {
    dynamicHeaderSum += dynamicVhdBytes[index];
  }
  writeDynamicBig32(dynamicHeader + 36U, ~dynamicHeaderSum);
  writeDynamicBig32(dynamicBat, dynamicBlock / 512U);
  writeDynamicBig32(dynamicBat + 4U, 0xffffffffU);
  dynamicVhdBytes[dynamicBlock] = 0xc0U;
  const std::size_t dynamicData = dynamicBlock + dynamicBitmapBytes;
  std::fill_n(dynamicVhdBytes.begin() + static_cast<std::ptrdiff_t>(dynamicData),
              512U, 0x5aU);
  std::fill_n(dynamicVhdBytes.begin() +
                  static_cast<std::ptrdiff_t>(dynamicData + 512U),
              512U, 0xa5U);
  std::copy(vhdCookie.begin(), vhdCookie.end(),
            dynamicVhdBytes.begin() + static_cast<std::ptrdiff_t>(dynamicFooter));
  writeDynamicBig32(dynamicFooter + 8U, 2U);
  writeDynamicBig32(dynamicFooter + 12U, 0x00010000U);
  writeDynamicBig64(dynamicFooter + 16U, dynamicHeader);
  writeDynamicBig64(dynamicFooter + 40U, dynamicVirtualBytes);
  writeDynamicBig64(dynamicFooter + 48U, dynamicVirtualBytes);
  writeDynamicBig32(dynamicFooter + 60U, 3U);
  std::uint32_t dynamicFooterSum = 0;
  for (std::size_t index = dynamicFooter;
       index < dynamicFooter + 512U; ++index) {
    dynamicFooterSum += dynamicVhdBytes[index];
  }
  writeDynamicBig32(dynamicFooter + 64U, ~dynamicFooterSum);
  std::copy_n(dynamicVhdBytes.begin() +
                  static_cast<std::ptrdiff_t>(dynamicFooter),
              512U, dynamicVhdBytes.begin());

  const auto dynamicVhdPath = temporary.path() / "dynamic-disk.vhd";
  writeBytes(dynamicVhdPath, dynamicVhdBytes);
  const auto dynamicVhd = analyzer.analyze(dynamicVhdPath);
  expect(dynamicVhd.succeeded() &&
             dynamicVhd.image->format == rufus::core::ImageFormat::Vhd &&
             dynamicVhd.image->containerPayloadLayout ==
                 rufus::core::ContainerPayloadLayout::DynamicVhd &&
             dynamicVhd.image->containerPayloadSizeBytes ==
                 dynamicVirtualBytes &&
             dynamicVhd.image->capabilities.rawWrite,
         "a checksummed dynamic VHD should expose its sparse virtual disk");
  auto dynamicSource = rufus::core::openRawImageSource(*dynamicVhd.image);
  std::array<unsigned char, 1536> dynamicPayload{};
  std::array<unsigned char, 512> unallocatedPayload{};
  std::string dynamicReadError;
  expect(dynamicSource.succeeded() &&
             dynamicSource.source->readAt(0, dynamicPayload.data(),
                                          dynamicPayload.size(),
                                          dynamicReadError) &&
             dynamicSource.source->readAt(dynamicBlockBytes,
                                          unallocatedPayload.data(),
                                          unallocatedPayload.size(),
                                          dynamicReadError) &&
             std::all_of(dynamicPayload.begin(), dynamicPayload.begin() + 512,
                         [](const unsigned char value) { return value == 0x5aU; }) &&
             std::all_of(dynamicPayload.begin() + 512,
                         dynamicPayload.begin() + 1024,
                         [](const unsigned char value) { return value == 0xa5U; }) &&
             std::all_of(dynamicPayload.begin() + 1024,
                         dynamicPayload.end(),
                         [](const unsigned char value) { return value == 0U; }) &&
             std::all_of(unallocatedPayload.begin(), unallocatedPayload.end(),
                         [](const unsigned char value) { return value == 0U; }),
         "dynamic VHD reads should reconstruct present, absent, and unallocated sectors: " +
             dynamicReadError);

  std::vector<unsigned char> vhdxBytes(4U * 1024U * 1024U, 0);
  auto writeVhdx16 = [&vhdxBytes](const std::size_t offset, const std::uint16_t value) {
    vhdxBytes[offset] = static_cast<unsigned char>(value & 0xffU);
    vhdxBytes[offset + 1U] = static_cast<unsigned char>(value >> 8U);
  };
  auto writeVhdx32 = [&vhdxBytes](const std::size_t offset, const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      vhdxBytes[offset + index] =
          static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };
  auto writeVhdx64 = [&vhdxBytes](const std::size_t offset, const std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
      vhdxBytes[offset + index] =
          static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };
  auto vhdxCrc32c = [&vhdxBytes](const std::size_t offset, const std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
      crc ^= vhdxBytes[offset + index];
      for (unsigned int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1U) ^ (0x82f63b78U & (0U - (crc & 1U)));
      }
    }
    return ~crc;
  };
  const std::string vhdxIdentifier = "vhdxfile";
  std::copy(vhdxIdentifier.begin(), vhdxIdentifier.end(), vhdxBytes.begin());
  const std::size_t vhdxHeader = 64U * 1024U;
  const std::string headerSignature = "head";
  std::copy(headerSignature.begin(), headerSignature.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(vhdxHeader));
  writeVhdx64(vhdxHeader + 8U, 1);
  writeVhdx32(vhdxHeader + 4U, vhdxCrc32c(vhdxHeader, 4096));

  const std::array<unsigned char, 16> batRegion{
      0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
      0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08};
  const std::array<unsigned char, 16> metadataRegion{
      0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
      0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e};
  const std::size_t regionTable = 192U * 1024U;
  const std::string regionSignature = "regi";
  std::copy(regionSignature.begin(), regionSignature.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(regionTable));
  writeVhdx32(regionTable + 8U, 2);
  std::copy(batRegion.begin(), batRegion.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(regionTable + 16U));
  writeVhdx64(regionTable + 32U, 2U * 1024U * 1024U);
  writeVhdx32(regionTable + 40U, 1024U * 1024U);
  writeVhdx32(regionTable + 44U, 1);
  std::copy(metadataRegion.begin(), metadataRegion.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(regionTable + 48U));
  writeVhdx64(regionTable + 64U, 1024U * 1024U);
  writeVhdx32(regionTable + 72U, 1024U * 1024U);
  writeVhdx32(regionTable + 76U, 1);
  writeVhdx32(regionTable + 4U, vhdxCrc32c(regionTable, 64U * 1024U));

  const std::size_t metadata = 1024U * 1024U;
  const std::string metadataSignature = "metadata";
  std::copy(metadataSignature.begin(), metadataSignature.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(metadata));
  writeVhdx16(metadata + 10U, 3);
  const std::array<unsigned char, 16> virtualDiskSize{
      0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
      0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
  std::copy(virtualDiskSize.begin(), virtualDiskSize.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(metadata + 32U));
  writeVhdx32(metadata + 48U, 64U * 1024U);
  writeVhdx32(metadata + 52U, 8);
  const std::array<unsigned char, 16> fileParameters{
      0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
      0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
  std::copy(fileParameters.begin(), fileParameters.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(metadata + 64U));
  writeVhdx32(metadata + 80U, 64U * 1024U + 8U);
  writeVhdx32(metadata + 84U, 8);
  const std::array<unsigned char, 16> logicalSectorSize{
      0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
      0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
  std::copy(logicalSectorSize.begin(), logicalSectorSize.end(),
            vhdxBytes.begin() + static_cast<std::ptrdiff_t>(metadata + 96U));
  writeVhdx32(metadata + 112U, 64U * 1024U + 16U);
  writeVhdx32(metadata + 116U, 4);
  writeVhdx64(metadata + 64U * 1024U, 16U * 1024U * 1024U);
  writeVhdx32(metadata + 64U * 1024U + 8U, 1024U * 1024U);
  writeVhdx32(metadata + 64U * 1024U + 12U, 0U);
  writeVhdx32(metadata + 64U * 1024U + 16U, 512U);
  writeVhdx64(2U * 1024U * 1024U, 3ULL * 1024ULL * 1024ULL | 6U);
  std::fill_n(vhdxBytes.begin() + 3U * 1024U * 1024U,
              4096U, 0x6cU);
  const auto vhdxPath = temporary.path() / "disk.vhdx";
  writeBytes(vhdxPath, vhdxBytes);
  const auto vhdx = analyzer.analyze(vhdxPath);
  expect(vhdx.succeeded() && vhdx.image->format == rufus::core::ImageFormat::Vhdx &&
             vhdx.image->capabilities.validContainerMetadata &&
             vhdx.image->expandedSizeBytes == 16U * 1024U * 1024U &&
             vhdx.image->containerPayloadLayout ==
                 rufus::core::ContainerPayloadLayout::DynamicVhdx &&
             vhdx.image->capabilities.rawWrite,
         "a parentless dynamic VHDX should expose its validated sparse payload");
  auto vhdxSource = rufus::core::openRawImageSource(*vhdx.image);
  std::array<unsigned char, 4096> vhdxAllocated{};
  std::array<unsigned char, 4096> vhdxUnallocated{};
  std::string vhdxReadError;
  expect(vhdxSource.succeeded() &&
             vhdxSource.source->readAt(0, vhdxAllocated.data(),
                                       vhdxAllocated.size(), vhdxReadError) &&
             vhdxSource.source->readAt(1024U * 1024U,
                                       vhdxUnallocated.data(),
                                       vhdxUnallocated.size(), vhdxReadError) &&
             std::all_of(vhdxAllocated.begin(), vhdxAllocated.end(),
                         [](const unsigned char value) { return value == 0x6cU; }) &&
             std::all_of(vhdxUnallocated.begin(), vhdxUnallocated.end(),
                         [](const unsigned char value) { return value == 0U; }),
         "dynamic VHDX reads should reconstruct allocated and zero blocks: " +
             vhdxReadError);

  std::vector<unsigned char> invalidVhdxBytes(512, 0);
  std::copy(vhdxIdentifier.begin(), vhdxIdentifier.end(), invalidVhdxBytes.begin());
  const auto invalidVhdxPath = temporary.path() / "invalid.vhdx";
  writeBytes(invalidVhdxPath, invalidVhdxBytes);
  const auto invalidVhdx = analyzer.analyze(invalidVhdxPath);
  expect(invalidVhdx.succeeded() &&
             invalidVhdx.image->format == rufus::core::ImageFormat::Vhdx &&
             !invalidVhdx.image->capabilities.validContainerMetadata,
         "a VHDX filename or identifier alone must not validate its metadata");

  std::vector<unsigned char> ffuBytes(4U * 4096U, 0);
  ffuBytes[0] = 32;
  const std::string ffuSignature = "SignedImage ";
  std::copy(ffuSignature.begin(), ffuSignature.end(), ffuBytes.begin() + 4);
  ffuBytes[16] = 4;
  const std::size_t ffuImageHeader = 4096;
  ffuBytes[ffuImageHeader] = 28;
  const std::string ffuImageSignature = "ImageFlash ";
  std::copy(ffuImageSignature.begin(), ffuImageSignature.end(),
            ffuBytes.begin() + static_cast<std::ptrdiff_t>(ffuImageHeader + 4U));
  const std::size_t ffuStoreHeader = 8192;
  ffuBytes[ffuStoreHeader + 204U] = 0x00;
  ffuBytes[ffuStoreHeader + 205U] = 0x10;
  ffuBytes[ffuStoreHeader + 208U] = 1;
  ffuBytes[ffuStoreHeader + 212U] = 8;
  const auto ffuPath = temporary.path() / "disk.ffu";
  writeBytes(ffuPath, ffuBytes);
  const auto ffu = analyzer.analyze(ffuPath);
  expect(ffu.succeeded() && ffu.image->format == rufus::core::ImageFormat::Ffu &&
             ffu.image->capabilities.validContainerMetadata,
         "an FFU security header should be recognized at its documented offset");
  expect(!rufus::core::openRawImageSource(*ffu.image).succeeded(),
         "a provider-applied FFU container must not open as raw container bytes");
  ffuBytes[ffuImageHeader + 4U] = 'X';
  const auto malformedFfuPath = temporary.path() / "malformed.ffu";
  writeBytes(malformedFfuPath, ffuBytes);
  const auto malformedFfu = analyzer.analyze(malformedFfuPath);
  expect(malformedFfu.succeeded() &&
             malformedFfu.image->format == rufus::core::ImageFormat::Ffu &&
             !malformedFfu.image->capabilities.validContainerMetadata,
         "an FFU filename and security signature must not bypass image/store-header validation");
}

void testWindowsToGoPlanning(const TemporaryDirectory& temporary) {
  const auto isoPath = temporary.path() / "windows-to-go.iso";
  writeIsoFixture(isoPath);
  const rufus::core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(isoPath);
  expect(analysis.succeeded() && analysis.image->capabilities.windowsToGo,
         "Windows To Go planning fixture should be eligible");

  rufus::core::BlockDeviceInfo target;
  target.stableId = "test:windows-to-go";
  target.devicePath = (temporary.path() / "physical-target").string();
  target.displayName = "Windows To Go target";
  target.capacityBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
  target.logicalSectorSize = 512;
  target.bus = rufus::core::DeviceBus::Usb;
  target.removable = true;
  target.ejectable = true;
  target.writable = true;
  target.wholeDevice = true;

  rufus::core::WindowsToGoOptions options;
  options.editionIndex = 1;
  options.userExperience.preventInternalDiskAccess = true;
  options.userExperience.bypassOnlineAccountRequirement = true;
  options.userExperience.createLocalAccount = true;
  options.userExperience.localAccountName = "Rufus' User";
  options.userExperience.useRegionalOptions = true;
  options.userExperience.localeName = "en-US";
  options.userExperience.disableDataCollection = true;
  options.userExperience.applyQualityOfLifeOptions = true;

  const rufus::core::WindowsToGoPlanner planner;
  const auto planned = planner.build(*analysis.image, target, options,
                                     "Portable Windows");
  expect(planned.succeeded() && planned.plan->edition().index == 1 &&
             planned.plan->volumeLabel() == "Portable Windows",
         "a validated edition and eligible target should produce a Windows To Go plan");
  const std::string& xml = planned.plan->unattendXml();
  expect(xml.find("<SanPolicy>4</SanPolicy>") != std::string::npos &&
             xml.find("BypassNRO") != std::string::npos &&
             xml.find("AllowTelemetry") != std::string::npos &&
             xml.find("DisableWindowsConsumerFeatures") != std::string::npos &&
             xml.find("<UILanguage>en-US</UILanguage>") != std::string::npos &&
             xml.find("<Name>Rufus&apos; User</Name>") != std::string::npos &&
             xml.find("<Value></Value>") != std::string::npos &&
             xml.find("<PlainText>true</PlainText>") != std::string::npos &&
             xml.find("UABhAHMAcwB3AG8AcgBkAA==") == std::string::npos &&
             xml.find("logonpasswordchg:yes") != std::string::npos &&
             xml.find("net accounts /maxpwage:unlimited") != std::string::npos,
         "Windows To Go unattended settings should encode all selected experience options");
  expect(xml.find("Rufus' User") == std::string::npos,
         "local account names must be XML escaped");

  const auto extractedWim = temporary.path() / "windows-to-go-install.wim";
  std::uint64_t extractionProgress = 0;
  const auto extraction = rufus::core::extractWindowsToGoSource(
      *planned.plan, extractedWim,
      [&](const rufus::core::WindowsToGoSourceProgress& progress) {
        extractionProgress = progress.bytesProcessed;
      });
  expect(extraction.success && extraction.bytesExtracted != 0 &&
             extractionProgress == extraction.bytesExtracted &&
             extraction.sourceEntry.find("INSTALL.WIM") != std::string::npos &&
             std::filesystem::file_size(extractedWim) == extraction.bytesExtracted,
         "Windows To Go should extract its validated install image with progress reporting");

  auto invalidOptions = options;
  invalidOptions.userExperience.localAccountName = "bad/name";
  expect(!planner.build(*analysis.image, target, invalidOptions, "Windows").succeeded(),
         "invalid Windows local account names must reject planning");
  invalidOptions = options;
  invalidOptions.editionIndex = 99;
  expect(!planner.build(*analysis.image, target, invalidOptions, "Windows").succeeded(),
         "an edition index not present in the WIM must reject planning");
  auto smallTarget = target;
  smallTarget.capacityBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
  expect(!planner.build(*analysis.image, smallTarget, options, "Windows").succeeded(),
         "a target smaller than the Windows To Go minimum must reject planning");
}

void testImageProfiles() {
  rufus::core::ImageInfo windows;
  windows.format = rufus::core::ImageFormat::Iso;
  windows.capabilities.windowsImageMetadata = true;
  windows.windowsVersionMajor = 10;
  rufus::core::ImageProfileResolver::apply(
      {{"BOOTMGR", 1, false},
       {"sources/install.wim", 3U * 1024U * 1024U * 1024U, false},
       {"efi/boot/bootx64.efi", 1, false}},
      windows);
  expect(windows.family == rufus::core::ImageFamily::WindowsInstaller,
         "Windows install and boot files should identify a Windows installer");
  expect(windows.capabilities.standardWindowsInstallation,
         "Windows installers should offer standard installation mode");
  expect(windows.capabilities.windowsToGo,
         "Windows installers should be marked as Windows To Go candidates");
  expect(windows.capabilities.windowsCustomization,
         "Windows installers should enable Windows customization metadata");
  expect(windows.capabilities.uefiBootable, "an EFI loader should identify UEFI media");

  rufus::core::ImageInfo linuxImage;
  linuxImage.format = rufus::core::ImageFormat::Iso;
  rufus::core::ImageProfileResolver::apply(
      {{"isolinux/isolinux.bin", 1, false},
       {"boot/grub/grub.cfg", 1, false},
       {"efi/boot/bootx64.efi", 1, false},
       {"casper/filesystem.squashfs", 1, false}},
      linuxImage, rufus::core::LinuxPersistenceStyle::Casper);
  expect(linuxImage.family == rufus::core::ImageFamily::LinuxLive,
         "Syslinux or GRUB live media should identify a Linux image");
  expect(linuxImage.capabilities.linuxPersistence,
         "supported Linux live media should offer persistence");
  expect(linuxImage.capabilities.usesSyslinux &&
             linuxImage.capabilities.usesGrub,
         "bootloader evidence should be retained in the image profile");
  expect(linuxImage.capabilities.linuxPersistenceStyle ==
             rufus::core::LinuxPersistenceStyle::Casper,
         "validated Casper boot entries should retain their persistence style");

  rufus::core::ImageInfo casperGrub4Dos;
  casperGrub4Dos.format = rufus::core::ImageFormat::Iso;
  rufus::core::ImageProfileResolver::apply(
      {{"GRLDR", 1, false}, {"CASPER", 2048, true}}, casperGrub4Dos,
      rufus::core::LinuxPersistenceStyle::Casper);
  expect(!casperGrub4Dos.capabilities.linuxPersistence,
         "BIOS-only GRUB4DOS media must not advertise the UEFI persistence path");

  rufus::core::ImageInfo tailsImage;
  tailsImage.format = rufus::core::ImageFormat::Iso;
  tailsImage.capabilities.uefiBootable = true;
  rufus::core::ImageProfileResolver::apply(
      {{"boot/grub/grub.cfg", 1, false},
       {"efi/boot/bootx64.efi", 1, false},
       {"live/Tails.module", 1, false}},
      tailsImage, rufus::core::LinuxPersistenceStyle::DebianLive);
  expect(!tailsImage.capabilities.linuxPersistence &&
             tailsImage.capabilities.linuxPersistenceStyle ==
                 rufus::core::LinuxPersistenceStyle::None,
         "Tails media must retain its native encrypted persistence workflow");

  rufus::core::ImageInfo biosGrub;
  biosGrub.format = rufus::core::ImageFormat::Iso;
  rufus::core::ImageProfileResolver::apply(
      {{"boot/grub/grub.cfg", 1, false},
       {"boot/grub/i386-pc/normal.mod", 1, false},
       {"live/filesystem.squashfs", 1, false}},
      biosGrub, rufus::core::LinuxPersistenceStyle::DebianLive);
  expect(biosGrub.capabilities.linuxPersistence &&
             biosGrub.capabilities.biosBootable &&
             !biosGrub.capabilities.uefiBootable,
         "a standard BIOS-only GRUB2 tree should enable extracted persistence mode");

  for (const std::string& grubPrefix : {std::string("boot/grub2"),
                                        std::string("grub")}) {
    rufus::core::ImageInfo alternateGrub;
    alternateGrub.format = rufus::core::ImageFormat::Iso;
    rufus::core::ImageProfileResolver::apply(
        {{grubPrefix + "/grub.cfg", 1, false},
         {grubPrefix + "/i386-pc/normal.mod", 1, false},
         {"live/filesystem.squashfs", 1, false}},
        alternateGrub, rufus::core::LinuxPersistenceStyle::DebianLive);
    expect(alternateGrub.capabilities.linuxPersistence &&
               alternateGrub.capabilities.biosBootable,
           "common alternate GRUB2 directory layouts should enable extracted persistence mode");
  }

  rufus::core::ImageInfo biosOnlyWindows;
  biosOnlyWindows.format = rufus::core::ImageFormat::Iso;
  biosOnlyWindows.capabilities.windowsImageMetadata = true;
  biosOnlyWindows.windowsVersionMajor = 10;
  rufus::core::ImageProfileResolver::apply(
      {{"bootmgr", 1, false}, {"sources/install.wim", 1, false}}, biosOnlyWindows);
  expect(biosOnlyWindows.capabilities.standardWindowsInstallation &&
             !biosOnlyWindows.capabilities.windowsToGo,
         "Windows To Go candidacy should require EFI boot support");

  rufus::core::ImageInfo windowsSeven;
  windowsSeven.format = rufus::core::ImageFormat::Iso;
  windowsSeven.capabilities.windowsImageMetadata = true;
  windowsSeven.windowsVersionMajor = 6;
  windowsSeven.windowsVersionMinor = 1;
  rufus::core::ImageProfileResolver::apply(
      {{"bootmgr", 1, false},
       {"sources/install.wim", 1, false},
       {"efi/boot/bootx64.efi", 1, false}},
      windowsSeven);
  expect(windowsSeven.capabilities.standardWindowsInstallation &&
             !windowsSeven.capabilities.windowsToGo,
         "pre-Windows-8 install images should not advertise Windows To Go");

  rufus::core::ImageInfo hybrid;
  hybrid.format = rufus::core::ImageFormat::Iso;
  hybrid.partitionScheme = rufus::core::PartitionScheme::Mbr;
  rufus::core::ImageProfileResolver::apply({{"README", 1, false}}, hybrid);
  expect(hybrid.capabilities.isoExtraction && hybrid.capabilities.rawWrite,
         "hybrid ISOs should expose both extracted-file and raw modes");

  rufus::core::ImageInfo largeFile;
  largeFile.format = rufus::core::ImageFormat::Iso;
  rufus::core::ImageProfileResolver::apply(
      {{"payload.bin", 4ULL * 1024ULL * 1024ULL * 1024ULL, false}}, largeFile);
  expect(largeFile.capabilities.containsLargeFile && largeFile.capabilities.requiresNtfs,
         "files at the FAT32 limit should require a large-file-capable filesystem");

  rufus::core::ImageInfo raw;
  raw.format = rufus::core::ImageFormat::Raw;
  rufus::core::ImageProfileResolver::apply({}, raw);
  expect(raw.capabilities.rawWrite, "raw images should expose only raw write mode");
}

void testFileWriter(const TemporaryDirectory& temporary) {
  std::vector<unsigned char> bytes(16U * 1024U);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<unsigned char>(index % 251U);
  }
  const auto sourcePath = temporary.path() / "source.img";
  writeBytes(sourcePath, bytes);

  const rufus::core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(sourcePath);
  expect(analysis.succeeded(), "writer source fixture should analyze successfully");

  const rufus::core::FileImageWriter writer;
  const auto destination = temporary.path() / "written.img";
  std::size_t progressEvents = 0;
  const auto result = writer.write(
      *analysis.image, destination, {4096, true},
      [&](const rufus::core::FileWriteProgress&) { ++progressEvents; });
  expect(result.success, "file-backed write and verification should succeed: " + result.error);
  expect(result.bytesWritten == bytes.size(), "writer should report the full byte count");
  expect(std::filesystem::file_size(destination) == bytes.size(),
         "written file should match source size");
  expect(progressEvents >= 4, "writer should report staged progress");

  const auto cancelledDestination = temporary.path() / "cancelled.img";
  bool cancel = false;
  const auto cancelled = writer.write(
      *analysis.image, cancelledDestination, {4096, false},
      [&](const rufus::core::FileWriteProgress& progress) {
        if (progress.stage == rufus::core::FileWriteStage::Writing) {
          cancel = true;
        }
      },
      [&cancel] { return cancel; });
  expect(cancelled.cancelled && !cancelled.success, "cancel request should stop file-backed write");
  expect(!std::filesystem::exists(cancelledDestination),
         "cancelled file-backed write should not publish a partial destination");

  const auto rawDeviceAttempt = writer.write(*analysis.image, "/dev/rufus-test-device");
  expect(!rawDeviceAttempt.success, "file-backed writer must reject raw-device paths");
}

void testRawWritePlanAndEngine(const TemporaryDirectory& temporary) {
  std::vector<unsigned char> sourceBytes(128U * 1024U);
  for (std::size_t index = 0; index < sourceBytes.size(); ++index) {
    sourceBytes[index] = static_cast<unsigned char>(index % 239U);
  }
  const auto sourcePath =
      temporary.path() / std::filesystem::u8path(u8"raw-sourc\u00e9.img");
  writeBytes(sourcePath, sourceBytes);

  const rufus::core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(sourcePath);
  expect(analysis.succeeded() && analysis.image->capabilities.rawWrite,
         "raw source should be eligible for a raw write plan");
  expect(analysis.image->path == sourcePath.u8string(),
         "portable image paths should retain their UTF-8 representation");
  const auto descriptorStyleAnalysis = analyzer.analyze(sourcePath, "source-from-fd.img");
  expect(descriptorStyleAnalysis.succeeded() &&
             descriptorStyleAnalysis.image->format == rufus::core::ImageFormat::Raw,
         "a filename hint should preserve format detection for descriptor-backed analysis");

  rufus::core::BlockDeviceInfo device;
  device.stableId = "test:virtual-target:2097152";
  device.devicePath = "/dev/rufus-virtual-target";
  device.displayName = "Virtual test target";
  device.capacityBytes = 2U * 1024U * 1024U;
  device.logicalSectorSize = 512;
  device.bus = rufus::core::DeviceBus::Usb;
  device.removable = true;
  device.writable = true;
  device.wholeDevice = true;

  const rufus::core::WritePlanBuilder builder;
  const auto planned = builder.buildRawWrite(*analysis.image, device, 4096, true);
  expect(planned.succeeded(), "aligned raw image and target should produce a write plan");

  MemoryRawTarget target(device.capacityBytes, device.logicalSectorSize);
  std::size_t progressEvents = 0;
  const rufus::core::RawImageWriter writer;
  const auto written = writer.write(
      *planned.plan, target,
      [&](const rufus::core::RawWriteProgress&) { ++progressEvents; });
  expect(written.success && written.destructiveWriteStarted,
         "virtual raw target should be written and verified");
  expect(written.bytesWritten == sourceBytes.size(),
         "raw writer should report the complete source byte count");
  expect(written.verificationCompleted &&
             written.bytesVerified == sourceBytes.size(),
         "standard verification should compare every written byte");
  expect(std::equal(sourceBytes.begin(), sourceBytes.end(), target.bytes().begin()),
         "virtual target bytes should match the source image");
  expect(target.bytes()[sourceBytes.size()] == 0xa5,
         "raw writer should leave bytes beyond the image untouched");
  expect(progressEvents >= 10, "raw writer should report writing, flush, and verification stages");

  const auto fastPlan = builder.buildRawWriteWithVerification(
      *analysis.image, device, rufus::core::VerificationProfile::Fast, 4096);
  MemoryRawTarget fastTarget(device.capacityBytes, device.logicalSectorSize);
  const auto fastWritten = writer.write(*fastPlan.plan, fastTarget);
  expect(fastWritten.success && fastWritten.verificationCompleted &&
             fastWritten.bytesVerified < fastWritten.bytesWritten &&
             fastWritten.bytesVerified >= 2U * 4096U,
         "fast verification should compare deterministic first, last, and distributed samples");
  const auto impossibleFullPlan = builder.buildRawWriteWithVerification(
      *analysis.image, device, rufus::core::VerificationProfile::Full, 4096);
  expect(!impossibleFullPlan.succeeded(),
         "full-device verification must reject a source that does not cover the target");

  const auto clearingPlan = builder.buildRawWrite(*analysis.image, device, 4096, true, true);
  expect(clearingPlan.succeeded(),
         "an image with one MiB of trailing room should allow metadata clearing");
  MemoryRawTarget clearingTarget(device.capacityBytes, device.logicalSectorSize);
  const auto cleared = writer.write(*clearingPlan.plan, clearingTarget);
  expect(cleared.success && clearingTarget.bytes().back() == 0 &&
             clearingTarget.bytes()[sourceBytes.size()] == 0xa5,
         "ISO-style raw plans should clear and verify only the final metadata region");

  MemoryRawSource descriptorSource(sourceBytes);
  MemoryRawTarget descriptorTarget(device.capacityBytes, device.logicalSectorSize);
  const auto descriptorWritten = writer.write(*planned.plan, descriptorSource, descriptorTarget);
  expect(descriptorWritten.success &&
             std::equal(sourceBytes.begin(), sourceBytes.end(), descriptorTarget.bytes().begin()),
         "an already-open source should use the same verified raw-copy engine");

  MemoryRawSource wrongSource(std::vector<unsigned char>(sourceBytes.size() - 1U));
  MemoryRawTarget wrongSourceTarget(device.capacityBytes, device.logicalSectorSize);
  const auto wrongSourceResult = writer.write(*planned.plan, wrongSource, wrongSourceTarget);
  expect(!wrongSourceResult.success && wrongSourceTarget.writeCount() == 0,
         "a supplied source with changed size must be rejected before target writes");

  MemoryRawTarget cancelledTarget(device.capacityBytes, device.logicalSectorSize);
  const auto cancelled = writer.write(*planned.plan, cancelledTarget, {}, [] { return true; });
  expect(cancelled.cancelled && !cancelled.destructiveWriteStarted,
         "pre-write cancellation must leave the target untouched");
  expect(cancelledTarget.writeCount() == 0, "cancelled raw write must not issue target writes");

  MemoryRawTarget partialTarget(device.capacityBytes, device.logicalSectorSize);
  bool cancelAfterFirstBlock = false;
  const auto partial = writer.write(
      *planned.plan, partialTarget,
      [&cancelAfterFirstBlock](const rufus::core::RawWriteProgress& progress) {
        if (progress.stage == rufus::core::RawWriteStage::Writing) {
          cancelAfterFirstBlock = true;
        }
      },
      [&cancelAfterFirstBlock] { return cancelAfterFirstBlock; });
  expect(partial.cancelled && partial.destructiveWriteStarted && partialTarget.writeCount() == 1,
         "mid-write cancellation must clearly report a partially overwritten target");

  MemoryRawTarget wrongGeometryTarget(device.capacityBytes + 512, device.logicalSectorSize);
  const auto wrongGeometry = writer.write(*planned.plan, wrongGeometryTarget);
  expect(!wrongGeometry.success && wrongGeometryTarget.writeCount() == 0,
         "target geometry changes must be rejected before the first write");

  MemoryRawTarget flushFailureTarget(device.capacityBytes, device.logicalSectorSize);
  flushFailureTarget.setFailFlush(true);
  const auto flushFailure = writer.write(*planned.plan, flushFailureTarget);
  expect(!flushFailure.success && flushFailure.destructiveWriteStarted,
         "a flush failure must report that destructive writing already occurred");

  std::vector<unsigned char> changedBytes = sourceBytes;
  changedBytes.push_back(0);
  writeBytes(sourcePath, changedBytes);
  MemoryRawTarget changedSourceTarget(device.capacityBytes, device.logicalSectorSize);
  const auto changedSource = writer.write(*planned.plan, changedSourceTarget);
  expect(!changedSource.success && changedSourceTarget.writeCount() == 0,
         "source changes after planning must be rejected before target writes");
}

void testDeploymentQuality(const TemporaryDirectory& temporary) {
  rufus::core::DeploymentPreflightInput input;
  input.operation = "Test ISO deployment";
  input.image.displayName = "source.iso";
  input.image.format = rufus::core::ImageFormat::Iso;
  input.image.sizeBytes = 64U * 1024U;
  input.target.stableId = "test:receipt-target";
  input.target.devicePath = "/dev/test-receipt";
  input.target.displayName = "Receipt target";
  input.target.capacityBytes = 128U * 1024U;
  input.target.logicalSectorSize = 512U;
  input.target.writable = true;
  input.target.wholeDevice = true;
  input.partitionScheme = "GPT";
  input.targetSystem = "UEFI";
  input.fileSystem = "FAT32";
  input.clusterSizeBytes = 4096U;
  input.verificationProfile = rufus::core::VerificationProfile::Standard;
  input.bytesToWrite = input.target.capacityBytes;
  input.transformations.push_back("Extract and verify the ISO file tree");
  input.dependencies.push_back(
      {"portable FAT32 stager", true, true, "Built in"});
  const auto report = rufus::core::buildDeploymentPreflight(input);
  expect(report.ready() &&
             report.toText().find("Test ISO deployment preflight") !=
                 std::string::npos &&
             report.toJson().find("\"ready\":true") != std::string::npos,
         "a valid deployment should produce a ready, serializable preflight report");
  input.writeSizePending = true;
  expect(rufus::core::buildDeploymentPreflight(input).toText().find(
             "Calculated during preparation") != std::string::npos,
         "range-write preflight must not present logical disk capacity as transferred bytes");
  input.writeSizePending = false;

  input.dependencies.push_back(
      {"missing required provider", true, false, "Not installed"});
  const auto blocked = rufus::core::buildDeploymentPreflight(input);
  expect(!blocked.ready() &&
             blocked.toText().find("BLOCKED") != std::string::npos,
         "a missing required dependency should block preflight");

  rufus::core::DeploymentReceipt receipt;
  receipt.application = "Rufus++";
  receipt.applicationVersion = "test";
  receipt.startedAtUtc = "2026-01-01T00:00:00Z";
  receipt.finishedAtUtc = "2026-01-01T00:01:00Z";
  receipt.preflight = report;
  receipt.success = true;
  receipt.destructiveWriteStarted = true;
  receipt.bytesWritten = input.bytesToWrite;
  receipt.bytesVerified = input.bytesToWrite;
  receipt.verificationCompleted = true;
  const auto receiptPath = temporary.path() / "deployment-receipt.json";
  const auto saved = rufus::core::writeDeploymentReceipt(receiptPath, receipt);
  std::ifstream receiptInput(receiptPath, std::ios::binary);
  const std::string receiptText{
      std::istreambuf_iterator<char>(receiptInput),
      std::istreambuf_iterator<char>()};
  expect(saved.success &&
             receiptText.find("rufus-plus-plus-deployment-receipt-v1") !=
                 std::string::npos &&
             receiptText.find("\"success\":true") != std::string::npos,
         "a deployment receipt should be committed as valid structured output");

  std::vector<unsigned char> mbrSample(2U * 1024U * 1024U, 0U);
  mbrSample[510U] = 0x55U;
  mbrSample[511U] = 0xaaU;
  mbrSample[446U] = 0x80U;
  mbrSample[450U] = 0x0cU;
  mbrSample[454U] = 0x00U;
  mbrSample[455U] = 0x08U;
  mbrSample[458U] = 0xe8U;
  mbrSample[459U] = 0x03U;
  std::copy_n("FAT32   ", 8U, mbrSample.begin() + 2048U * 512U + 82U);
  auto inspectDevice = input.target;
  inspectDevice.capacityBytes = 4U * 1024U * 1024U;
  const auto inspection =
      rufus::core::inspectMediaSamples(inspectDevice, mbrSample);
  expect(inspection.success && inspection.bootable &&
             inspection.partitionScheme == rufus::core::PartitionScheme::Mbr &&
             inspection.partitions.size() == 1U &&
             inspection.partitions.front().fileSystem == "FAT32",
         "bounded read-only inspection should identify an active MBR FAT32 partition");
}

void testSecureBootAnalysis(const TemporaryDirectory& temporary) {
  std::vector<unsigned char> efi(1024U, 0U);
  efi[0] = 'M';
  efi[1] = 'Z';
  efi[0x3cU] = 0x80U;
  std::copy_n("PE\0\0", 4U, efi.begin() + 0x80U);
  efi[0x80U + 6U] = 1U;
  efi[0x80U + 20U] = 0xf0U;
  efi[0x80U + 24U] = 0x0bU;
  efi[0x80U + 25U] = 0x02U;
  efi[0x80U + 24U + 60U + 1U] = 0x02U;
  const std::size_t directory = 0x80U + 24U + 112U;
  const std::size_t certificate = directory + 4U * 8U;
  efi[certificate] = 0x20U;
  efi[certificate + 1U] = 0x03U;
  efi[certificate + 4U] = 0x10U;
  const std::size_t section = 0x80U + 24U + 0xf0U;
  std::copy_n(".sbat\0\0\0", 8U, efi.begin() +
                                      static_cast<std::ptrdiff_t>(section));
  efi[section + 16U + 1U] = 0x01U;
  efi[section + 20U + 1U] = 0x02U;
  const std::string currentSbat =
      "sbat,1,SBAT Version,sbat,1,https://example.invalid\n"
      "shim,4,Vendor,shim,4,https://example.invalid\n";
  std::copy(currentSbat.begin(), currentSbat.end(), efi.begin() + 512U);
  const auto baseline = rufus::core::packagedSecureBootBaseline();
  const auto compatible =
      rufus::core::analyzeEfiImage("EFI/BOOT/BOOTX64.EFI", efi, baseline);
  const auto compatibleEfi = efi;
  expect(compatible.validPe && compatible.authenticodePresent &&
             compatible.disposition ==
                 rufus::core::SecureBootDisposition::Compatible &&
             compatible.sha256.size() == 64U &&
             compatible.authenticodeSha256.size() == 64U,
         "offline Secure Boot analysis should validate PE bounds, signature presence, SHA-256, and current SBAT");

  auto exactRevocation = baseline;
  exactRevocation.revokedSha256.insert(compatible.sha256);
  const auto revokedHash = rufus::core::analyzeEfiImage(
      "EFI/BOOT/BOOTX64.EFI", efi, exactRevocation);
  expect(revokedHash.disposition ==
             rufus::core::SecureBootDisposition::RevokedHash,
         "an exact offline SHA-256 revocation must be reported");

  const auto shimPosition =
      std::search(efi.begin(), efi.end(), currentSbat.begin(), currentSbat.end());
  expect(shimPosition != efi.end(), "the test SBAT payload should be present");
  const std::string shimMarker = "shim,4";
  const auto generation = std::search(shimPosition, efi.end(),
                                      shimMarker.begin(), shimMarker.end());
  expect(generation != efi.end(), "the test shim generation should be present");
  *(generation + 5U) = '3';
  const auto staleSbat =
      rufus::core::analyzeEfiImage("EFI/BOOT/BOOTX64.EFI", efi, baseline);
  expect(staleSbat.disposition ==
             rufus::core::SecureBootDisposition::RevokedSbat,
         "an EFI loader below the packaged SBAT minimum must be reported as revoked");

  const std::string databaseText =
      "# Offline additions\nsha256 " + compatible.sha256 +
      "\nsbat custom.loader 7\n";
  const auto databasePath = temporary.path() / "secure-boot-db.txt";
  writeBytes(databasePath,
             std::vector<unsigned char>(databaseText.begin(), databaseText.end()));
  const auto loaded = rufus::core::loadSecureBootDatabase(databasePath);
  expect(loaded.success && !loaded.database.trustedBaseline &&
             loaded.database.revokedSha256.count(compatible.sha256) == 1U &&
             loaded.database.minimumSbatGeneration.at("custom.loader") == 7U,
         "a user-supplied offline database should extend exact hashes and SBAT minimums without network access");

  std::vector<unsigned char> signatureList(76U, 0U);
  constexpr std::array<unsigned char, 16> sha256SignatureType{
      0x26U, 0x16U, 0xc4U, 0xc1U, 0x4cU, 0x50U, 0x92U, 0x40U,
      0xacU, 0xa9U, 0x41U, 0xf9U, 0x36U, 0x93U, 0x43U, 0x28U};
  std::copy(sha256SignatureType.begin(), sha256SignatureType.end(),
            signatureList.begin());
  const auto write32 = [&signatureList](const std::size_t offset,
                                        const std::uint32_t value) {
    for (unsigned int byte = 0; byte < 4U; ++byte) {
      signatureList[offset + byte] =
          static_cast<unsigned char>(value >> (byte * 8U));
    }
  };
  write32(16U, 76U);
  write32(20U, 0U);
  write32(24U, 48U);
  for (std::size_t index = 0; index < 32U; ++index) {
    signatureList[44U + index] = static_cast<unsigned char>(
        std::stoul(compatible.authenticodeSha256.substr(index * 2U, 2U),
                   nullptr, 16));
  }
  const auto signatureListPath = temporary.path() / "dbx.esl";
  writeBytes(signatureListPath, signatureList);
  const auto importedSignatureList =
      rufus::core::loadSecureBootDatabase(signatureListPath);
  expect(importedSignatureList.success &&
             importedSignatureList.database.revokedSha256.count(
                 compatible.authenticodeSha256) == 1U,
         "a firmware-standard EFI DBX signature list should import SHA-256 revocations offline");
  const auto revokedByFirmwareDbx = rufus::core::analyzeEfiImage(
      "EFI/BOOT/BOOTX64.EFI", compatibleEfi,
      importedSignatureList.database);
  expect(revokedByFirmwareDbx.disposition ==
             rufus::core::SecureBootDisposition::RevokedHash,
         "an imported DBX UEFI/Authenticode digest must revoke the matching EFI image");
}

std::uint16_t readLittle16(const unsigned char* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1]) << 8U;
}

std::uint32_t readLittle32(const unsigned char* bytes) {
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

void checkFatDirectoryParents(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  constexpr std::uint64_t partitionOffset = 2048ULL * 512ULL;
  std::array<unsigned char, 512> boot{};
  input.seekg(static_cast<std::streamoff>(partitionOffset));
  input.read(reinterpret_cast<char*>(boot.data()), boot.size());
  expect(input && readLittle16(boot.data() + 11) == 512U,
         "directory parent test requires the staged FAT32 partition");
  const std::uint32_t root = readLittle32(boot.data() + 44);
  const std::uint64_t dataStart = partitionOffset +
      (readLittle16(boot.data() + 14) +
       static_cast<std::uint64_t>(boot[16]) * readLittle32(boot.data() + 36)) * 512ULL;
  const std::size_t clusterBytes = static_cast<std::size_t>(boot[13]) * 512U;
  auto entryCluster = [](const unsigned char* entry) {
    return static_cast<std::uint32_t>(readLittle16(entry + 20)) << 16U |
           readLittle16(entry + 26);
  };
  std::vector<std::pair<std::uint32_t, std::uint32_t>> pending{{root, 0U}};
  std::size_t rootChildren = 0U, nestedChildren = 0U;
  for (std::size_t index = 0U; index < pending.size(); ++index) {
    expect(pending.size() <= 16U, "fixture directory traversal must remain bounded");
    const auto [cluster, parent] = pending[index];
    std::vector<unsigned char> directory(clusterBytes);
    input.seekg(static_cast<std::streamoff>(dataStart +
        static_cast<std::uint64_t>(cluster - 2U) * clusterBytes));
    input.read(reinterpret_cast<char*>(directory.data()), directory.size());
    expect(static_cast<bool>(input), "fixture directory cluster must be readable");
    if (index != 0U) {
      expect(directory[0] == '.' && directory[1] == ' ' &&
                 entryCluster(directory.data()) == cluster,
             "FAT dot entry must point to its own directory");
      expect(directory[32] == '.' && directory[33] == '.' && directory[34] == ' ' &&
                 entryCluster(directory.data() + 32) == parent,
             "FAT dotdot entry must use zero for root and the actual cluster for nested parents");
      if (parent == 0U) ++rootChildren;
      else ++nestedChildren;
    }
    for (std::size_t offset = 0U; offset + 32U <= directory.size(); offset += 32U) {
      const auto* entry = directory.data() + offset;
      if (entry[0] == 0U) break;
      if (entry[0] == 0xe5U || entry[0] == '.' || entry[11] == 0x0fU ||
          (entry[11] & 0x10U) == 0U) continue;
      const auto child = entryCluster(entry);
      expect(child >= 2U, "fixture child directory must have a valid cluster");
      pending.emplace_back(child, index == 0U ? 0U : cluster);
    }
  }
  expect(rootChildren >= 2U && nestedChildren >= 1U,
         "parent-pointer regression must cover both root and nested directories");
}

void testIsoModeDeployment(const TemporaryDirectory& temporary) {
  const auto sourcePath = temporary.path() / "deploy.iso";
  writeIsoFixture(sourcePath);
  const rufus::core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(sourcePath);
  expect(analysis.succeeded() && analysis.image->capabilities.isoExtraction &&
             analysis.image->capabilities.uefiBootable,
         "UEFI ISO fixture should support portable ISO deployment");

  rufus::core::BlockDeviceInfo device;
  device.stableId = "test:iso-target:536870912";
  device.devicePath = "/dev/rufus-iso-target";
  device.displayName = "Virtual ISO target";
  device.capacityBytes = 512ULL * 1024ULL * 1024ULL;
  device.logicalSectorSize = 512;
  device.bus = rufus::core::DeviceBus::Usb;
  device.removable = true;
  device.writable = true;
  device.wholeDevice = true;

  const rufus::core::IsoDeploymentPlanner planner;
  auto planned = planner.build(*analysis.image, device, "RUFUSPP_TEST");
  expect(planned.succeeded(), "UEFI/FAT32 ISO deployment should produce a plan");
  expect(planned.plan->biosBootable(),
         "Windows Setup media should receive the portable BIOS bootstrap");
  const auto outputPath = temporary.path() / "iso-stage.img";
  std::size_t progressEvents = 0;
  const rufus::core::IsoImageStager stager;
  const auto staged = stager.stage(
      *planned.plan, outputPath,
      [&](const rufus::core::IsoDeploymentProgress&) { ++progressEvents; });
  expect(staged.success && staged.stagedImage.has_value(),
         "ISO files should stage into a verified FAT32 image: " + staged.error);
  checkFatDirectoryParents(outputPath);
  expect(staged.filesExtracted == 3 && staged.bytesExtracted > 0,
         "ISO staging should extract every fixture file");
  expect(progressEvents >= 8, "ISO staging should report each major phase");
  expect(staged.stagedImage->format == rufus::core::ImageFormat::Raw &&
             staged.stagedImage->capabilities.rawWrite,
         "staged ISO output should feed the native raw-device backends");
  const rufus::core::WritePlanBuilder rawPlanner;
  const auto compactCommit = rawPlanner.buildRawWriteWithVerification(
      *staged.stagedImage, device, rufus::core::VerificationProfile::Standard,
      4U * 1024U * 1024U, planned.plan->requiresTrailingMetadataClear(), staged.writeRanges);
  expect(compactCommit.succeeded() && compactCommit.plan->clearTargetTailMetadata(),
         "compact quick MBR ISO commits must still clear stale trailing metadata");

  rufus::core::IsoDeploymentOptions gptOptions;
  gptOptions.partitionScheme = rufus::core::PartitionScheme::Gpt;
  gptOptions.targetSystem = rufus::core::IsoTargetSystem::Uefi;
  gptOptions.fileSystem = rufus::core::IsoFilesystemPreference::Fat32;
  gptOptions.clusterSizeBytes = 4096U;
  const rufus::core::IsoDeploymentPlanner gptPlanner(
      rufus::core::createSystemWimSplitter(), gptOptions);
  const auto gptPlan = gptPlanner.build(*analysis.image, device, "RUFUSPP_GPT");
  expect(gptPlan.succeeded() &&
             gptPlan.plan->partitionScheme() ==
                 rufus::core::PartitionScheme::Gpt &&
             !gptPlan.plan->biosBootable(),
         "a UEFI ISO should accept an explicit GPT/FAT32 layout");
  const auto gptPath = temporary.path() / "gpt-iso-stage.img";
  const auto gptStage = stager.stage(*gptPlan.plan, gptPath);
  std::ifstream gptInput(gptPath, std::ios::binary);
  std::array<unsigned char, 512> gptMbr{};
  std::array<char, 8> primarySignature{};
  std::array<char, 8> backupSignature{};
  std::array<unsigned char, 512> gptFatBoot{};
  gptInput.read(reinterpret_cast<char*>(gptMbr.data()), gptMbr.size());
  gptInput.seekg(512U);
  gptInput.read(primarySignature.data(), primarySignature.size());
  gptInput.seekg(static_cast<std::streamoff>(device.capacityBytes - 512U));
  gptInput.read(backupSignature.data(), backupSignature.size());
  gptInput.seekg(2048ULL * 512ULL);
  gptInput.read(reinterpret_cast<char*>(gptFatBoot.data()),
                gptFatBoot.size());
  expect(gptStage.success && gptStage.stagedImage.has_value() &&
             gptStage.stagedImage->sizeBytes == device.capacityBytes &&
             gptStage.stagedImage->partitionScheme ==
                 rufus::core::PartitionScheme::Gpt &&
             gptInput && gptMbr[450U] == 0xeeU &&
             std::string(primarySignature.data(), primarySignature.size()) ==
                 "EFI PART" &&
             std::string(backupSignature.data(), backupSignature.size()) ==
                 "EFI PART" &&
             gptFatBoot[13U] == 8U,
         "GPT ISO staging should write both GPT headers and honor the 4 KiB cluster: " +
             gptStage.error);
  checkFatDirectoryParents(gptPath);
  const auto gptCommit = rawPlanner.buildRawWriteWithVerification(
      *gptStage.stagedImage, device, rufus::core::VerificationProfile::Standard,
      4U * 1024U * 1024U, gptPlan.plan->requiresTrailingMetadataClear(), gptStage.writeRanges);
  expect(gptCommit.succeeded() && !gptCommit.plan->clearTargetTailMetadata() &&
             gptCommit.plan->bytesToWrite() < device.capacityBytes &&
             gptStage.writeRanges.back().offsetBytes + gptStage.writeRanges.back().sizeBytes == device.capacityBytes,
         "GPT ISO commits must preserve the staged backup partition table");
  std::vector<unsigned char> gptHead(2U * 1024U * 1024U);
  std::vector<unsigned char> gptTail(1024U * 1024U);
  gptInput.clear();
  gptInput.seekg(0);
  gptInput.read(reinterpret_cast<char*>(gptHead.data()),
                static_cast<std::streamsize>(gptHead.size()));
  gptInput.seekg(static_cast<std::streamoff>(device.capacityBytes -
                                             gptTail.size()));
  gptInput.read(reinterpret_cast<char*>(gptTail.data()),
                static_cast<std::streamsize>(gptTail.size()));
  const auto gptInspection =
      rufus::core::inspectMediaSamples(device, gptHead, gptTail);
  expect(gptInput && gptInspection.success &&
             gptInspection.primaryTableValid &&
             gptInspection.backupTableValid && gptInspection.bootable &&
             gptInspection.partitionScheme ==
                 rufus::core::PartitionScheme::Gpt &&
             !gptInspection.partitions.empty() &&
             gptInspection.partitions.front().fileSystem == "FAT32",
         "read-only inspection should validate the staged primary/backup GPT and FAT32 ESP");

  rufus::core::IsoDeploymentOptions invalidGptOptions = gptOptions;
  invalidGptOptions.targetSystem = rufus::core::IsoTargetSystem::Bios;
  const auto invalidGptPlan = rufus::core::IsoDeploymentPlanner(
      rufus::core::createSystemWimSplitter(), invalidGptOptions)
                                  .build(*analysis.image, device, "INVALID_GPT");
  expect(!invalidGptPlan.succeeded(),
         "GPT plus legacy BIOS must be rejected by the layout matrix");

  auto largeGptDevice = device;
  largeGptDevice.capacityBytes = 3ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  largeGptDevice.stableId = "test:gpt-ntfs-target:3tib";
  rufus::core::IsoDeploymentOptions largeNtfsOptions;
  largeNtfsOptions.ntfsAvailable = true;
  largeNtfsOptions.partitionScheme = rufus::core::PartitionScheme::Gpt;
  largeNtfsOptions.targetSystem = rufus::core::IsoTargetSystem::Uefi;
  largeNtfsOptions.fileSystem = rufus::core::IsoFilesystemPreference::Ntfs;
  largeNtfsOptions.clusterSizeBytes = 65536U;
  largeNtfsOptions.quickFormat = false;
  const auto largeNtfsPlan = rufus::core::IsoDeploymentPlanner(
      rufus::core::createSystemWimSplitter(), largeNtfsOptions)
                            .build(*analysis.image, largeGptDevice,
                                   "RUFUSPP_NTFS");
  expect(largeNtfsPlan.succeeded() &&
             largeNtfsPlan.plan->fileSystem() ==
                 rufus::core::IsoDeploymentFilesystem::Ntfs &&
             largeNtfsPlan.plan->partitionScheme() ==
                 rufus::core::PartitionScheme::Gpt &&
             largeNtfsPlan.plan->clusterSizeBytes() == 65536U &&
             !largeNtfsPlan.plan->quickFormat(),
         "GPT/NTFS planning should support large targets and retain format options");
  largeNtfsOptions.clusterSizeBytes = 131072U;
  const auto invalidNtfsCluster = rufus::core::IsoDeploymentPlanner(
      rufus::core::createSystemWimSplitter(), largeNtfsOptions)
                                      .build(*analysis.image, largeGptDevice,
                                             "INVALID_NTFS");
  expect(!invalidNtfsCluster.succeeded(),
         "NTFS allocation units larger than 64 KiB must be rejected");

  rufus::core::IsoDeploymentOptions fullOptions;
  fullOptions.partitionScheme = rufus::core::PartitionScheme::Mbr;
  fullOptions.targetSystem = rufus::core::IsoTargetSystem::BiosAndUefi;
  fullOptions.fileSystem = rufus::core::IsoFilesystemPreference::Fat32;
  fullOptions.quickFormat = false;
  const auto fullPlan = rufus::core::IsoDeploymentPlanner(
      rufus::core::createSystemWimSplitter(), fullOptions)
                            .build(*analysis.image, device, "RUFUSPP_FULL");
  expect(fullPlan.succeeded(),
         "full MBR/FAT32 ISO deployment should produce a plan");
  const auto fullIsoPath = temporary.path() / "full-iso-stage.img";
  const auto fullStage = stager.stage(*fullPlan.plan, fullIsoPath);
  expect(fullStage.success &&
             fullStage.writeRanges.empty() &&
             fullStage.stagedImage->sizeBytes == device.capacityBytes &&
             std::filesystem::file_size(fullIsoPath) == device.capacityBytes,
         "full ISO format should produce a complete-device zero-fill image: " +
             fullStage.error);
  const auto fullCommit = rawPlanner.buildRawWriteWithVerification(
      *fullStage.stagedImage, device, rufus::core::VerificationProfile::Full,
      4U * 1024U * 1024U, fullPlan.plan->requiresTrailingMetadataClear(), fullStage.writeRanges);
  expect(fullCommit.succeeded() && !fullCommit.plan->clearTargetTailMetadata(),
         "full-format ISO commits must accept a complete-device image without a trailing gap");

  auto validationAssets =
      std::make_shared<rufus::core::RuntimeUefiValidationAssets>();
  validationAssets->bootloaders.push_back(
      {"bootx64.efi", {'R', 'U', 'F', 'U', 'S', '-', 'U', 'E', 'F', 'I', '-', 'M', 'D', '5'}});
  rufus::core::IsoDeploymentOptions validationOptions;
  validationOptions.runtimeUefiValidation = validationAssets;
  const rufus::core::IsoDeploymentPlanner validationPlanner(
      rufus::core::createSystemWimSplitter(), validationOptions);
  const auto validationPlan =
      validationPlanner.build(*analysis.image, device, "RUFUSPP_MD5");
  expect(validationPlan.succeeded() &&
             validationPlan.plan->runtimeUefiValidation(),
         "a UEFI ISO should accept the offline runtime-validation plan");
  const auto validationPath = temporary.path() / "validation-stage.img";
  const auto validationStage =
      stager.stage(*validationPlan.plan, validationPath);
  expect(validationStage.success && validationStage.filesExtracted == 5,
         "runtime validation should retain the original loader and add a wrapper and manifest: " +
             validationStage.error);
  std::ifstream validationInput(validationPath, std::ios::binary);
  const std::string validationBytes{
      std::istreambuf_iterator<char>(validationInput),
      std::istreambuf_iterator<char>()};
  const std::string validatorMarker = "RUFUS-UEFI-MD5";
  const std::string testDigest = "098f6bcd4621d373cade4e832627b4f6";
  const std::string totalMarker = "# md5sum_totalbytes = 0x";
  expect(validationBytes.find(validatorMarker) != std::string::npos &&
             validationBytes.find(testDigest) != std::string::npos &&
             validationBytes.find(totalMarker) != std::string::npos,
         "the staged media should contain the validator and a correct complete MD5 manifest");

  auto noUefi = *analysis.image;
  noUefi.capabilities.uefiBootable = false;
  expect(!validationPlanner.build(noUefi, device, "RUFUSPP_MD5").succeeded(),
         "runtime validation must reject media without UEFI boot support");

  rufus::core::IsoDeploymentOptions ntfsOptions;
  ntfsOptions.maximumFatFileBytes = 1;
  ntfsOptions.wimSplitPartBytes = 1;
  ntfsOptions.ntfsAvailable = true;
  const rufus::core::IsoDeploymentPlanner ntfsPlanner(
      rufus::core::createSystemWimSplitter(), ntfsOptions);
  const auto ntfsPlan = ntfsPlanner.build(*analysis.image, device, "RUFUSPP_NTFS");
  expect(ntfsPlan.succeeded() &&
             ntfsPlan.plan->fileSystem() ==
                 rufus::core::IsoDeploymentFilesystem::Ntfs &&
             !ntfsPlan.plan->splitsWindowsImage(),
         "a host with NTFS staging should plan arbitrary FAT32-overflow files through UEFI:NTFS");
  auto oversizedNtfsDevice = device;
  oversizedNtfsDevice.capacityBytes =
      (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
       1U) *
      512U;
  expect(!ntfsPlanner
              .build(*analysis.image, oversizedNtfsDevice, "RUFUSPP_NTFS")
              .succeeded(),
         "an MBR/UEFI:NTFS plan must reject targets outside the MBR address range");
  const auto ntfsRoot = temporary.path() / "mounted-ntfs";
  std::filesystem::create_directory(ntfsRoot);
  const auto ntfsExtracted = stager.extractToDirectory(*ntfsPlan.plan, ntfsRoot);
  expect(ntfsExtracted.success && ntfsExtracted.filesExtracted == 3,
         "the NTFS path should extract and byte-verify every ISO file: " +
             ntfsExtracted.error);

  rufus::core::WindowsInstallationOptions windowsOptions;
  windowsOptions.userExperience.bypassHardwareRequirements = true;
  windowsOptions.userExperience.bypassOnlineAccountRequirement = true;
  windowsOptions.userExperience.disableAutomaticDeviceEncryption = true;
  windowsOptions.userExperience.createLocalAccount = true;
  windowsOptions.userExperience.localAccountName = "Setup User";
  const auto customizedPlan = planner.build(*analysis.image, device, "RUFUSPP_WUE", {},
                                            windowsOptions);
  expect(customizedPlan.succeeded() &&
             customizedPlan.plan->windowsUnattendXml().find("BypassTPMCheck") !=
                 std::string::npos &&
             customizedPlan.plan->windowsUnattendXml().find(
                 "PreventDeviceEncryption") != std::string::npos,
         "standard Windows Setup should plan the selected user-experience options");
  expect(customizedPlan.plan->windowsUnattendXml().find(
             "UABhAHMAcwB3AG8AcgBkAA==") == std::string::npos &&
             customizedPlan.plan->windowsUnattendXml().find("<Value></Value>") !=
                 std::string::npos,
         "local-account customization must not embed a fixed or user password");
  const auto customizedPath = temporary.path() / "customized-iso-stage.img";
  const auto customizedStage = stager.stage(*customizedPlan.plan, customizedPath);
  expect(customizedStage.success && customizedStage.filesExtracted == 4,
         "standard Windows Setup should embed and verify autounattend.xml: " +
             customizedStage.error);
  std::ifstream customizedInput(customizedPath, std::ios::binary);
  const std::string customizedBytes{
      std::istreambuf_iterator<char>(customizedInput),
      std::istreambuf_iterator<char>()};
  expect(customizedBytes.find("BypassTPMCheck") != std::string::npos &&
             customizedBytes.find("Setup User") != std::string::npos,
         "the staged FAT32 image should contain the generated Windows answer file");

  std::ifstream input(outputPath, std::ios::binary);
  std::array<unsigned char, 512> mbr{};
  input.read(reinterpret_cast<char*>(mbr.data()), mbr.size());
  expect(input && mbr[0] == 0x33 && mbr[446] == 0x80 &&
             mbr[510] == 0x55 && mbr[511] == 0xaa && mbr[450] == 0x0c,
         "Windows staging should contain an active, bootstrapped LBA FAT32 MBR partition");
  const std::uint32_t partitionStart = readLittle32(mbr.data() + 454);
  expect(partitionStart == 2048, "FAT32 partition should be aligned at one MiB");
  input.seekg(static_cast<std::streamoff>(partitionStart) * 512);
  std::array<unsigned char, 512> boot{};
  input.read(reinterpret_cast<char*>(boot.data()), boot.size());
  expect(input && readLittle16(boot.data() + 11) == 512 &&
             readLittle32(boot.data() + 44) == 2 && boot[510] == 0x55 && boot[511] == 0xaa,
         "staged partition should contain a valid FAT32 BIOS parameter block");
  constexpr std::array<unsigned char, 8> windowsOem =
      {'M', 'S', 'W', 'I', 'N', '4', '.', '1'};
  constexpr std::array<unsigned char, 7> bootmgrText =
      {'B', 'O', 'O', 'T', 'M', 'G', 'R'};
  expect(std::equal(windowsOem.begin(), windowsOem.end(), boot.begin() + 3) &&
             std::search(boot.begin(), boot.end(), bootmgrText.begin(), bootmgrText.end()) !=
                 boot.end(),
         "Windows FAT32 boot code should load BOOTMGR in legacy BIOS mode");

  const std::uint32_t fatSectors = readLittle32(boot.data() + 36);
  const std::uint32_t dataStart = partitionStart + 32U + 2U * fatSectors;
  input.seekg(static_cast<std::streamoff>(dataStart) * 512);
  std::array<unsigned char, 4096> root{};
  input.read(reinterpret_cast<char*>(root.data()), root.size());
  expect(static_cast<bool>(input), "staged root directory should be readable");
  std::uint32_t bootmgrCluster = 0;
  constexpr std::array<unsigned char, 11> bootmgrName =
      {'B', 'O', 'O', 'T', 'M', 'G', 'R', ' ', ' ', ' ', ' '};
  for (std::size_t offset = 0; offset < root.size(); offset += 32U) {
    if (root[offset] == 0) {
      break;
    }
    if (root[offset + 11] != 0x0f &&
        std::equal(bootmgrName.begin(), bootmgrName.end(), root.begin() + offset)) {
      bootmgrCluster = static_cast<std::uint32_t>(readLittle16(root.data() + offset + 20)) << 16U |
                       readLittle16(root.data() + offset + 26);
      break;
    }
  }
  expect(bootmgrCluster >= 2, "BOOTMGR should be present in the FAT32 root directory");
  const std::uint64_t bootmgrOffset =
      (static_cast<std::uint64_t>(dataStart) +
       static_cast<std::uint64_t>(bootmgrCluster - 2U) * boot[13]) * 512U;
  input.seekg(static_cast<std::streamoff>(bootmgrOffset));
  std::array<char, 4> payload{};
  input.read(payload.data(), payload.size());
  expect(input && std::string(payload.data(), payload.size()) == "test",
         "FAT32 file data should match its ISO extent");

  auto biosOnly = *analysis.image;
  biosOnly.capabilities.uefiBootable = false;
  const auto biosOnlyPlan = planner.build(biosOnly, device, "TEST");
  expect(biosOnlyPlan.succeeded() && biosOnlyPlan.plan->biosBootable(),
         "BIOS-only Windows Setup media should use the BOOTMGR deployment path");

  const auto fakeSplitter = std::make_shared<FakeWimSplitter>();
  const rufus::core::IsoDeploymentOptions splitOptions{400, 300};
  const rufus::core::IsoDeploymentPlanner splitPlanner(fakeSplitter, splitOptions);
  const auto splitPlan = splitPlanner.build(*analysis.image, device, "WIM_SPLIT");
  expect(splitPlan.succeeded() && splitPlan.plan->splitsWindowsImage(),
         "an oversized install.wim should plan a split-WIM FAT32 transformation");
  const auto splitOutputPath = temporary.path() / "split-wim-stage.img";
  bool sawWimPreparation = false;
  const auto splitStaged = stager.stage(
      *splitPlan.plan, splitOutputPath,
      [&](const rufus::core::IsoDeploymentProgress& progress) {
        sawWimPreparation = sawWimPreparation ||
                            progress.stage ==
                                rufus::core::IsoDeploymentStage::PreparingWindowsImage;
      });
  expect(splitStaged.success && splitStaged.filesExtracted == 4 &&
             fakeSplitter->called() && fakeSplitter->observedSourceSize() > 400 &&
             fakeSplitter->observedPartLimit() == 300 && sawWimPreparation,
         "install.wim should be extracted, split, staged, and verified as two .swm parts: " +
             splitStaged.error);
  auto splitWorkPath = splitOutputPath;
  splitWorkPath += ".wim-work";
  expect(!std::filesystem::exists(splitWorkPath),
         "private WIM preparation files should be removed after staging");

  const auto unavailableSplitter = std::make_shared<FakeWimSplitter>(false);
  const rufus::core::IsoDeploymentPlanner unavailablePlanner(unavailableSplitter,
                                                              splitOptions);
  const auto unavailablePlan =
      unavailablePlanner.build(*analysis.image, device, "WIM_SPLIT");
  expect(!unavailablePlan.succeeded() && !unavailablePlan.issues.empty() &&
             unavailablePlan.issues.back().message.find("unavailable") !=
                 std::string::npos,
         "large install.wim planning should expose an unavailable wimlib backend");

  const auto esdPath = temporary.path() / "deploy-esd.iso";
  writeIsoFixture(esdPath, false, false, true);
  const auto esdAnalysis = analyzer.analyze(esdPath);
  expect(esdAnalysis.succeeded() &&
             esdAnalysis.image->capabilities.standardWindowsInstallation,
         "a Windows ISO containing install.esd should be recognized");
  const auto esdPlan = splitPlanner.build(*esdAnalysis.image, device, "ESD_SPLIT");
  expect(esdPlan.succeeded() && esdPlan.plan->splitsWindowsImage(),
         "an oversized install.esd should plan a FAT32-compatible transformation");
  const auto esdOutputPath = temporary.path() / "split-esd-stage.img";
  const auto esdStage = stager.stage(*esdPlan.plan, esdOutputPath);
  expect(esdStage.success && esdStage.filesExtracted == 4,
         "install.esd should be transformed into validated split-WIM parts: " +
             esdStage.error);

  const auto cancelledPath = temporary.path() / "cancelled-iso-stage.img";
  const auto cancelled = stager.stage(*planned.plan, cancelledPath, {}, [] { return true; });
  expect(cancelled.cancelled && !std::filesystem::exists(cancelledPath),
         "pre-stage cancellation should not leave a temporary disk image");

  const auto udfPath = temporary.path() / "embedded-deploy.udf";
  writeUdfFixture(udfPath);
  const auto udfAnalysis = analyzer.analyze(udfPath);
  expect(udfAnalysis.succeeded() && udfAnalysis.image->capabilities.udf,
         "embedded UDF fixture should analyze successfully");
  auto uefiUdf = *udfAnalysis.image;
  uefiUdf.capabilities.uefiBootable = true;
  const auto udfPlan = planner.build(uefiUdf, device, "UDF_TEST");
  expect(udfPlan.succeeded(), "ordinary UDF file trees should produce ISO-mode plans");
  const auto udfOutputPath = temporary.path() / "udf-stage.img";
  const auto udfStaged = stager.stage(*udfPlan.plan, udfOutputPath);
  expect(udfStaged.success && udfStaged.filesExtracted == 1 &&
             udfStaged.bytesExtracted == 4,
         "embedded UDF file payloads should be extracted and verified: " + udfStaged.error);

  const auto jolietPath = temporary.path() / "unicode-deploy.iso";
  writeJolietIsoFixture(jolietPath);
  const auto jolietAnalysis = analyzer.analyze(jolietPath);
  expect(jolietAnalysis.succeeded() && jolietAnalysis.image->capabilities.joliet,
         "Joliet deployment fixture should analyze successfully");
  auto uefiJoliet = *jolietAnalysis.image;
  uefiJoliet.capabilities.uefiBootable = true;
  const auto jolietPlan = planner.build(uefiJoliet, device, "JOLIET");
  expect(jolietPlan.succeeded(), "Joliet file trees should produce ISO-mode plans");
  const auto jolietOutputPath = temporary.path() / "joliet-stage.img";
  const auto jolietStaged = stager.stage(*jolietPlan.plan, jolietOutputPath);
  expect(jolietStaged.success && jolietStaged.filesExtracted == 1,
         "Unicode Joliet names should stage through FAT long filenames: " +
             jolietStaged.error);

  auto fourKnDevice = device;
  fourKnDevice.stableId = "test:iso-target:4kn";
  fourKnDevice.logicalSectorSize = 4096;
  const auto fourKnPlan = planner.build(uefiUdf, fourKnDevice, "FOUR_KN");
  expect(fourKnPlan.succeeded(), "ISO mode should plan for a 4Kn removable target");
  const auto fourKnOutputPath = temporary.path() / "four-kn-stage.img";
  const auto fourKnStaged = stager.stage(*fourKnPlan.plan, fourKnOutputPath);
  expect(fourKnStaged.success && fourKnStaged.stagedImage &&
             fourKnStaged.stagedImage->sizeBytes % 4096U == 0,
         "4Kn ISO deployment images should be target-sector aligned: " +
             fourKnStaged.error);
}

void testLinuxPersistence(const TemporaryDirectory& temporary) {
  expect(rufus::core::detail::md5Hex("abc") ==
             "900150983cd24fb0d6963f7d28e17f72",
         "the persistence checksum implementation should match the MD5 test vector");
  std::string manifest =
      "00000000000000000000000000000000  ./boot/grub/grub.cfg\n"
      "11111111111111111111111111111111  ./README\n";
  const std::unordered_map<std::string, std::string> replacements = {
      {"boot/grub/grub.cfg", "900150983cd24fb0d6963f7d28e17f72"}};
  expect(rufus::core::detail::updateMd5Manifest(manifest, replacements) &&
             manifest.rfind("900150983cd24fb0d6963f7d28e17f72", 0) == 0 &&
             manifest.find("11111111111111111111111111111111") !=
                 std::string::npos,
         "checksum repair should update only transformed boot files");

  const std::string ubuntuConfig =
      "menuentry 'Try Ubuntu' {\n"
      "  linux /casper/vmlinuz quiet splash maybe-ubiquity ---\n"
      "}\n";
  const auto ubuntuPatch = rufus::core::patchLinuxPersistenceBootConfiguration(
      "boot/grub/grub.cfg", ubuntuConfig,
      rufus::core::LinuxPersistenceStyle::Casper);
  expect(rufus::core::detectLinuxPersistenceStyle(
             "boot/grub/grub.cfg", ubuntuConfig) ==
             rufus::core::LinuxPersistenceStyle::Casper,
         "Casper persistence should be detected from a matching kernel entry");
  expect(ubuntuPatch.recognizedConfiguration && ubuntuPatch.modified &&
             ubuntuPatch.contents.find("/casper/vmlinuz persistent") !=
                 std::string::npos,
         "Ubuntu GRUB entries should receive the persistent kernel option");
  const auto ubuntuAgain = rufus::core::patchLinuxPersistenceBootConfiguration(
      "boot/grub/grub.cfg", ubuntuPatch.contents,
      rufus::core::LinuxPersistenceStyle::Casper);
  expect(!ubuntuAgain.modified && ubuntuAgain.contents == ubuntuPatch.contents,
         "Linux persistence boot patching should be idempotent");

  const auto preseedPatch = rufus::core::patchLinuxPersistenceBootConfiguration(
      "boot/grub/grub.cfg",
      "linux /casper/vmlinuz quiet maybe-ubiquity file=/cdrom/preseed/linux.seed\r\n",
      rufus::core::LinuxPersistenceStyle::Casper);
  expect(preseedPatch.modified &&
             preseedPatch.contents.find("persistent file=/cdrom/preseed") !=
                 std::string::npos &&
             preseedPatch.contents.find("maybe-ubiquity") == std::string::npos &&
             preseedPatch.contents.find("\r\n") != std::string::npos,
         "preseed-based GRUB entries should preserve CRLF, add persistence, and remove maybe-ubiquity");

  const auto mintPatch = rufus::core::patchLinuxPersistenceBootConfiguration(
      "isolinux/live.cfg",
      "label live\n  kernel /casper/vmlinuz\n  append boot=casper quiet\n",
      rufus::core::LinuxPersistenceStyle::Casper);
  expect(mintPatch.modified &&
             mintPatch.contents.find("boot=casper persistent") !=
                 std::string::npos &&
             mintPatch.contents.find("kernel /casper/vmlinuz persistent") ==
                 std::string::npos,
         "Linux Mint Syslinux entries should receive the persistent option");

  const auto debianPatch = rufus::core::patchLinuxPersistenceBootConfiguration(
      "isolinux/menu.cfg", "append boot=live components quiet\n",
      rufus::core::LinuxPersistenceStyle::DebianLive);
  expect(rufus::core::detectLinuxPersistenceStyle(
             "isolinux/menu.cfg", "append boot=live components quiet\n") ==
             rufus::core::LinuxPersistenceStyle::DebianLive,
         "Debian persistence should be detected from a boot=live kernel entry");
  expect(debianPatch.modified &&
             debianPatch.contents.find("boot=live persistence") !=
                 std::string::npos,
         "Debian Live entries should receive the persistence option");
  const auto unrelatedPatch = rufus::core::patchLinuxPersistenceBootConfiguration(
      "README.txt", "boot=live\n",
      rufus::core::LinuxPersistenceStyle::DebianLive);
  expect(!unrelatedPatch.recognizedConfiguration && !unrelatedPatch.modified,
         "unrelated ISO files must never be changed by persistence patching");
  expect(rufus::core::detectLinuxPersistenceStyle(
             "boot/grub/grub.cfg",
             "linux /images/pxeboot/vmlinuz rd.live.image quiet\n") ==
             rufus::core::LinuxPersistenceStyle::None,
         "generic live-media kernel entries must not be mistaken for Debian persistence");

  const auto udfPath = temporary.path() / "persistence-source.udf";
  writeUdfFixture(udfPath);
  const rufus::core::ImageAnalyzer analyzer;
  const auto analysis = analyzer.analyze(udfPath);
  expect(analysis.succeeded(), "persistence fixture should analyze successfully");
  auto linuxImage = *analysis.image;
  linuxImage.family = rufus::core::ImageFamily::LinuxLive;
  linuxImage.capabilities.linuxPersistence = true;
  linuxImage.capabilities.usesGrub = true;
  linuxImage.capabilities.linuxPersistenceStyle =
      rufus::core::LinuxPersistenceStyle::DebianLive;
  linuxImage.capabilities.uefiBootable = true;

  rufus::core::BlockDeviceInfo device;
  device.stableId = "test:persistence-target:805306368";
  device.devicePath = "/dev/rufus-persistence-target";
  device.displayName = "Virtual persistence target";
  device.capacityBytes = 768ULL * 1024ULL * 1024ULL;
  device.logicalSectorSize = 512;
  device.bus = rufus::core::DeviceBus::Usb;
  device.removable = true;
  device.writable = true;
  device.wholeDevice = true;

  const rufus::core::IsoDeploymentPlanner planner;
  const auto tooSmall = planner.build(
      linuxImage, device, "LIVE_TEST",
      rufus::core::LinuxPersistenceOptions{128ULL * 1024ULL * 1024ULL});
  expect(!tooSmall.succeeded(),
         "persistence planning should enforce the 256 MiB minimum");

  auto planned = planner.build(
      linuxImage, device, "LIVE_TEST",
      rufus::core::LinuxPersistenceOptions{256ULL * 1024ULL * 1024ULL});
  expect(planned.succeeded() && planned.plan->hasLinuxPersistence() &&
             planned.plan->linuxPersistenceStyle() ==
                 rufus::core::LinuxPersistenceStyle::DebianLive,
         "Debian-style media should produce a persistence deployment plan");
  const auto outputPath = temporary.path() / "persistence-stage.img";
  bool sawPersistenceFormatting = false;
  const rufus::core::IsoImageStager stager;
  const auto staged = stager.stage(
      *planned.plan, outputPath,
      [&](const rufus::core::IsoDeploymentProgress& progress) {
        sawPersistenceFormatting =
            sawPersistenceFormatting ||
            progress.stage ==
                rufus::core::IsoDeploymentStage::CreatingPersistence;
      });
  expect(staged.success && staged.stagedImage && sawPersistenceFormatting,
         "Linux persistence should stage and verify a complete disk image: " +
             staged.error);
  expect(staged.stagedImage->sizeBytes == device.capacityBytes &&
             std::filesystem::file_size(outputPath) == device.capacityBytes,
         "a persistence stage should span the complete target layout");
  const auto debianCommit = rufus::core::WritePlanBuilder{}.buildRawWriteWithVerification(
      *staged.stagedImage, device, rufus::core::VerificationProfile::Standard,
      4U * 1024U * 1024U, planned.plan->requiresTrailingMetadataClear(), staged.writeRanges);
  expect(debianCommit.succeeded() && !debianCommit.plan->clearTargetTailMetadata(),
         "quick MBR Debian persistence must commit without requiring an unused trailing region");

  std::ifstream input(outputPath, std::ios::binary);
  std::array<unsigned char, 512> mbr{};
  input.read(reinterpret_cast<char*>(mbr.data()), mbr.size());
  const std::uint32_t persistenceStart = readLittle32(mbr.data() + 470U);
  const std::uint32_t persistenceSectors = readLittle32(mbr.data() + 474U);
  expect(input && mbr[450] == 0x0c && mbr[466] == 0x83 &&
             persistenceStart != 0U && persistenceSectors != 0U,
         "the staged MBR should contain FAT32 and Linux persistence partitions");
  input.seekg(static_cast<std::streamoff>(persistenceStart) * 512U + 1024U);
  std::array<unsigned char, 1024> superblock{};
  input.read(reinterpret_cast<char*>(superblock.data()), superblock.size());
  const std::string persistenceLabel(
      reinterpret_cast<const char*>(superblock.data() + 120U), 11U);
  expect(input && readLittle16(superblock.data() + 56U) == 0xef53U &&
             persistenceLabel == "persistence",
         "the second partition should contain a labeled ext2 filesystem");

  auto casperImage = linuxImage;
  casperImage.capabilities.linuxPersistenceStyle =
      rufus::core::LinuxPersistenceStyle::Casper;
  const auto casperPlan = planner.build(
      casperImage, device, "LIVE_TEST",
      rufus::core::LinuxPersistenceOptions{256ULL * 1024ULL * 1024ULL});
  expect(casperPlan.succeeded() &&
             casperPlan.plan->linuxPersistenceStyle() ==
                 rufus::core::LinuxPersistenceStyle::Casper,
         "Casper directory detection should select the casper-rw convention");

  const auto casperIsoPath = temporary.path() / "casper-persistence.iso";
  writeLinuxPersistenceIsoFixture(casperIsoPath);
  const auto casperAnalysis = analyzer.analyze(casperIsoPath);
  expect(casperAnalysis.succeeded() &&
             casperAnalysis.image->capabilities.linuxPersistence &&
             casperAnalysis.image->capabilities.linuxPersistenceStyle ==
                 rufus::core::LinuxPersistenceStyle::Casper,
         "a real Casper boot entry should enable Casper persistence planning");

  const auto debianIsoPath = temporary.path() / "debian-persistence.iso";
  writeLinuxPersistenceIsoFixture(
      debianIsoPath, GrubFixtureLayout::BootGrub,
      LinuxFixturePersistence::DebianLive);
  const auto debianAnalysis = analyzer.analyze(debianIsoPath);
  expect(debianAnalysis.succeeded() &&
             debianAnalysis.image->capabilities.linuxPersistence &&
             debianAnalysis.image->capabilities.linuxPersistenceStyle ==
                 rufus::core::LinuxPersistenceStyle::DebianLive,
         "a real boot=live entry should enable Debian Live persistence planning");

  const auto unsupportedIsoPath =
      temporary.path() / "unsupported-persistence.iso";
  writeLinuxPersistenceIsoFixture(
      unsupportedIsoPath, GrubFixtureLayout::BootGrub,
      LinuxFixturePersistence::Unsupported);
  const auto unsupportedAnalysis = analyzer.analyze(unsupportedIsoPath);
  expect(unsupportedAnalysis.succeeded() &&
             unsupportedAnalysis.image->capabilities.usesGrub &&
             !unsupportedAnalysis.image->capabilities.linuxPersistence &&
             unsupportedAnalysis.image->capabilities.linuxPersistenceStyle ==
                 rufus::core::LinuxPersistenceStyle::None,
         "GRUB media without Casper or Debian Live entries must not advertise persistence");
  expect(!planner
              .build(*unsupportedAnalysis.image, device, "UNSUPPORTED",
                     rufus::core::LinuxPersistenceOptions{
                         256ULL * 1024ULL * 1024ULL})
              .succeeded(),
         "persistence planning must reject an image without a validated style");

  const auto integratedCasperPlan = planner.build(
      *casperAnalysis.image, device, "CASPER_TEST",
      rufus::core::LinuxPersistenceOptions{256ULL * 1024ULL * 1024ULL});
  expect(integratedCasperPlan.succeeded(),
         "the detected Casper ISO should produce a persistence plan");
  expect(integratedCasperPlan.plan->legacyBiosBootstrap() ==
             rufus::core::LegacyBiosBootstrap::Grub2,
         "a standard GRUB i386-pc module tree should enable legacy BIOS boot");
  const auto casperOutputPath = temporary.path() / "casper-persistence-stage.img";
  const auto casperStaged = stager.stage(*integratedCasperPlan.plan,
                                         casperOutputPath);
  expect(casperStaged.success,
         "Casper ISO persistence should stage successfully: " +
             casperStaged.error);
  const auto casperCommit = rufus::core::WritePlanBuilder{}.buildRawWriteWithVerification(
      *casperStaged.stagedImage, device, rufus::core::VerificationProfile::Standard,
      4U * 1024U * 1024U, integratedCasperPlan.plan->requiresTrailingMetadataClear(), casperStaged.writeRanges);
  expect(casperCommit.succeeded() && !casperCommit.plan->clearTargetTailMetadata() &&
             casperCommit.plan->bytesToWrite() < device.capacityBytes / 4U,
         "quick MBR Casper persistence must skip free space and retain the metadata ranges");
  expect(!rufus::core::WritePlanBuilder{}.buildRawWriteWithVerification(
              *casperStaged.stagedImage, device,
              rufus::core::VerificationProfile::Standard,
              4U * 1024U * 1024U, true).succeeded(),
         "the raw planner must continue rejecting tail clearing that overlaps persistence data");

  const auto grub2IsoPath = temporary.path() / "grub2-persistence.iso";
  writeLinuxPersistenceIsoFixture(grub2IsoPath,
                                  GrubFixtureLayout::BootGrub2);
  const auto grub2Analysis = analyzer.analyze(grub2IsoPath);
  expect(grub2Analysis.succeeded() &&
             grub2Analysis.image->capabilities.linuxPersistence,
         "a /boot/grub2 module tree should be recognized for persistence");
  const auto grub2Plan = planner.build(
      *grub2Analysis.image, device, "GRUB2_TEST",
      rufus::core::LinuxPersistenceOptions{256ULL * 1024ULL * 1024ULL});
  expect(grub2Plan.succeeded() &&
             grub2Plan.plan->legacyBiosBootstrap() ==
                 rufus::core::LegacyBiosBootstrap::Grub2,
         "a /boot/grub2 tree should be aliased for the legacy GRUB bootstrap");
  const auto grub2OutputPath = temporary.path() / "grub2-persistence-stage.img";
  const auto grub2Staged = stager.stage(*grub2Plan.plan, grub2OutputPath);
  expect(grub2Staged.success,
         "the aliased /boot/grub2 tree should stage successfully: " +
             grub2Staged.error);

  const auto rootGrubIsoPath = temporary.path() / "root-grub-persistence.iso";
  writeLinuxPersistenceIsoFixture(rootGrubIsoPath,
                                  GrubFixtureLayout::RootGrub);
  const auto rootGrubAnalysis = analyzer.analyze(rootGrubIsoPath);
  expect(rootGrubAnalysis.succeeded() &&
             rootGrubAnalysis.image->capabilities.linuxPersistence,
         "a root /grub module tree should be recognized for persistence");
  const auto rootGrubPlan = planner.build(
      *rootGrubAnalysis.image, device, "ROOT_GRUB",
      rufus::core::LinuxPersistenceOptions{256ULL * 1024ULL * 1024ULL});
  expect(rootGrubPlan.succeeded() &&
             rootGrubPlan.plan->legacyBiosBootstrap() ==
                 rufus::core::LegacyBiosBootstrap::Grub2,
         "a root /grub tree should be aliased for the legacy GRUB bootstrap");
  const auto rootGrubOutputPath =
      temporary.path() / "root-grub-persistence-stage.img";
  const auto rootGrubStaged =
      stager.stage(*rootGrubPlan.plan, rootGrubOutputPath);
  expect(rootGrubStaged.success,
         "the aliased root /grub tree should stage successfully: " +
             rootGrubStaged.error);

  std::ifstream grubInput(casperOutputPath, std::ios::binary);
  std::array<unsigned char, 516> grubPrefix{};
  grubInput.read(reinterpret_cast<char*>(grubPrefix.data()),
                 static_cast<std::streamsize>(grubPrefix.size()));
  expect(grubInput && grubPrefix[0] == 0xeb && grubPrefix[1] == 0x63 &&
             grubPrefix[446] == 0x80 && grubPrefix[510] == 0x55 &&
             grubPrefix[511] == 0xaa && grubPrefix[512] == 0x52 &&
             grubPrefix[513] == 0x56,
         "GRUB media should contain the active GRUB MBR and core image in the pre-partition gap");

  std::ifstream casperInput(casperOutputPath, std::ios::binary);
  std::vector<unsigned char> stagedPrefix(8U * 1024U * 1024U, 0);
  casperInput.read(reinterpret_cast<char*>(stagedPrefix.data()),
                   static_cast<std::streamsize>(stagedPrefix.size()));
  expect(static_cast<bool>(casperInput),
         "the staged Casper FAT32 payload should be readable");
  const std::string persistentOption = "/casper/vmlinuz persistent";
  expect(std::search(stagedPrefix.begin(), stagedPrefix.end(),
                     persistentOption.begin(), persistentOption.end()) !=
             stagedPrefix.end(),
         "staged Casper boot configuration should contain the persistent option");
  const std::string originalConfig =
      "menuentry 'Try Linux' {\n  linux /casper/vmlinuz quiet splash\n}\n";
  const auto expectedPatch =
      rufus::core::patchLinuxPersistenceBootConfiguration(
          "boot/grub/grub.cfg", originalConfig,
          rufus::core::LinuxPersistenceStyle::Casper);
  const std::string patchedDigest =
      rufus::core::detail::md5Hex(expectedPatch.contents);
  expect(std::search(stagedPrefix.begin(), stagedPrefix.end(),
                     patchedDigest.begin(), patchedDigest.end()) !=
             stagedPrefix.end(),
         "the staged MD5 manifest should contain the transformed config checksum");

  casperInput.clear();
  casperInput.seekg(470U);
  std::array<unsigned char, 4> casperStartBytes{};
  casperInput.read(reinterpret_cast<char*>(casperStartBytes.data()),
                   casperStartBytes.size());
  const std::uint32_t casperStart = readLittle32(casperStartBytes.data());
  casperInput.seekg(static_cast<std::streamoff>(casperStart) * 512U + 1024U +
                    120U);
  std::array<char, 9> casperLabel{};
  casperInput.read(casperLabel.data(), casperLabel.size());
  expect(casperInput && std::string(casperLabel.data(), casperLabel.size()) ==
                             "casper-rw",
         "Casper persistence should use the casper-rw ext2 label");

  const auto persistenceOffset = static_cast<std::uint64_t>(casperStart) * 512U;
  casperInput.seekg(static_cast<std::streamoff>(persistenceOffset + 1024U));
  std::array<unsigned char, 1024> casperSuper{};
  casperInput.read(reinterpret_cast<char*>(casperSuper.data()), casperSuper.size());
  expect(casperInput && readLittle16(casperSuper.data() + 52U) == 0U &&
             readLittle16(casperSuper.data() + 54U) == 20U &&
             readLittle32(casperSuper.data() + 48U) == readLittle32(casperSuper.data() + 64U),
         "new ext2 volumes need correct mount counters and an untruncated write timestamp");
  const auto filesystemBlocks = readLittle32(casperSuper.data() + 4U);
  const auto groupBlocks = readLittle32(casperSuper.data() + 32U);
  const auto groupCount = (filesystemBlocks + groupBlocks - 1U) / groupBlocks;
  const auto inodeTableBytes = static_cast<std::uint64_t>(readLittle32(casperSuper.data() + 40U)) *
                              readLittle16(casperSuper.data() + 88U);
  for (std::uint32_t group = 0U; group < groupCount; ++group) {
    casperInput.seekg(static_cast<std::streamoff>(persistenceOffset + 4096U + group * 32U));
    std::array<unsigned char, 32> descriptor{};
    casperInput.read(reinterpret_cast<char*>(descriptor.data()), descriptor.size());
    const auto tableOffset = persistenceOffset + static_cast<std::uint64_t>(readLittle32(descriptor.data() + 8U)) * 4096U;
    expect(casperInput && std::any_of(casperStaged.writeRanges.begin(), casperStaged.writeRanges.end(),
        [&](const auto& range) {
          return tableOffset >= range.offsetBytes && tableOffset - range.offsetBytes <= range.sizeBytes &&
                 inodeTableBytes <= range.sizeBytes - (tableOffset - range.offsetBytes);
        }), "every inode table, including zero-filled unused entries, must be included in the write map");
  }
  casperInput.seekg(static_cast<std::streamoff>(persistenceOffset + 4096U));
  std::array<unsigned char, 32> casperDescriptor{};
  casperInput.read(reinterpret_cast<char*>(casperDescriptor.data()), casperDescriptor.size());
  expect(casperInput && readLittle16(casperDescriptor.data() + 16U) == 6U,
         "the group directory counter must include root and all five shutdown-hook directories");
  const auto inodeTable = persistenceOffset +
      static_cast<std::uint64_t>(readLittle32(casperDescriptor.data() + 8U)) * 4096U;
  std::uint32_t currentInode = 2U;
  for (const char* component : {"upper", "usr", "lib", "systemd", "system-shutdown",
                                "90-rufuspp-persistence"}) {
    casperInput.seekg(static_cast<std::streamoff>(inodeTable + (currentInode - 1U) * 256U));
    std::array<unsigned char, 256> directoryInode{};
    casperInput.read(reinterpret_cast<char*>(directoryInode.data()), directoryInode.size());
    expect(casperInput && (readLittle16(directoryInode.data()) & 0170000U) == 0040000U,
           "the persistence hook path must contain real directories");
    const auto block = readLittle32(directoryInode.data() + 40U);
    casperInput.seekg(static_cast<std::streamoff>(persistenceOffset +
                                                static_cast<std::uint64_t>(block) * 4096U));
    std::array<unsigned char, 4096> directory{};
    casperInput.read(reinterpret_cast<char*>(directory.data()), directory.size());
    std::uint32_t found = 0U;
    for (std::size_t offset = 0U; offset + 8U <= directory.size();) {
      const auto recordBytes = readLittle16(directory.data() + offset + 4U);
      const auto nameBytes = directory[offset + 6U];
      expect(recordBytes >= 8U && offset + recordBytes <= directory.size() &&
                 nameBytes <= recordBytes - 8U, "ext2 hook directory records must be bounded");
      const std::string name(reinterpret_cast<const char*>(directory.data() + offset + 8U),
                             nameBytes);
      if (name == component) {
        found = readLittle32(directory.data() + offset);
        break;
      }
      offset += recordBytes;
    }
    expect(found != 0U, std::string("missing persistence shutdown component: ") + component);
    currentInode = found;
  }
  casperInput.seekg(static_cast<std::streamoff>(inodeTable + (currentInode - 1U) * 256U));
  std::array<unsigned char, 256> hookInode{};
  casperInput.read(reinterpret_cast<char*>(hookInode.data()), hookInode.size());
  const auto hookSize = readLittle32(hookInode.data() + 4U);
  const auto hookOffset = persistenceOffset +
      static_cast<std::uint64_t>(readLittle32(hookInode.data() + 40U)) * 4096U;
  expect(casperInput && readLittle16(hookInode.data()) == 0100755U &&
             hookSize > 0U && hookSize <= 4096U,
         "the persistence finalizer must be a bounded executable regular file");
  casperInput.seekg(static_cast<std::streamoff>(hookOffset));
  std::string hook(hookSize, '\0');
  casperInput.read(hook.data(), hook.size());
  expect(casperInput && hook.find("UUID=$uuid") != std::string::npos &&
             hook.find("mount -o remount,ro \"$point\"") != std::string::npos &&
             hook.find("/dev/sda") == std::string::npos,
         "the shutdown hook must finalize the UUID-pinned filesystem without a disk-number assumption");
  rufus::core::detail::Ext2FormatOptions verifyOptions{
      persistenceOffset, 256ULL * 1024ULL * 1024ULL, "casper-rw", false, true};
  std::string verifyError;
  expect(rufus::core::detail::verifyExt2Filesystem(casperOutputPath, verifyOptions, verifyError),
         "the complete persistence hook tree should verify: " + verifyError);
  {
    std::fstream corrupt(casperOutputPath, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(static_cast<std::streamoff>(persistenceOffset + 4096U + 16U));
    corrupt.put(1);
  }
  expect(!rufus::core::detail::verifyExt2Filesystem(casperOutputPath, verifyOptions, verifyError),
         "staging verification must reject an incorrect group directory counter");
  {
    std::fstream restore(casperOutputPath, std::ios::binary | std::ios::in | std::ios::out);
    restore.seekp(static_cast<std::streamoff>(persistenceOffset + 4096U + 16U));
    restore.put(6);
  }
  {
    std::fstream corrupt(casperOutputPath, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(static_cast<std::streamoff>(hookOffset));
    corrupt.put('!');
  }
  expect(!rufus::core::detail::verifyExt2Filesystem(casperOutputPath, verifyOptions, verifyError),
         "staging verification must reject a damaged persistence shutdown hook");
  {
    std::fstream restore(casperOutputPath, std::ios::binary | std::ios::in | std::ios::out);
    restore.seekp(static_cast<std::streamoff>(hookOffset));
    restore.put(hook.front());
  }

  auto workPath = outputPath;
  workPath += ".persistence-work";
  expect(!std::filesystem::exists(workPath),
         "private persistence transformation files should be removed after staging");
}

}  // namespace

int main(const int argc, const char* const argv[]) {
  if (argc == 3 && std::string(argv[1]) == "--capture-fixtures") {
    const auto directory = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
    expect(!std::filesystem::is_symlink(directory) &&
               std::filesystem::create_directory(directory),
           "capture fixtures require a new private ordinary directory");
    testMediaCapture(directory);
    return EXIT_SUCCESS;
  }
  expect(argc == 1, "unexpected test arguments");
  const TemporaryDirectory temporary;
  testMediaPolicy();
  testImageAnalysis(temporary);
  testWindowsToGoPlanning(temporary);
  testImageProfiles();
  testFileWriter(temporary);
  testMediaCapture(temporary.path());
  testBadBlockEngine();
  testStandaloneFat32(temporary);
  testRawWritePlanAndEngine(temporary);
  testDeploymentQuality(temporary);
  testSecureBootAnalysis(temporary);
  testIsoModeDeployment(temporary);
  testLinuxPersistence(temporary);
  return EXIT_SUCCESS;
}
