/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rufus/core/file_image_writer.hpp"
#include "rufus/core/raw_image_writer.hpp"

namespace {

using namespace rufus::core;
constexpr std::size_t kBlock = 4096U;
constexpr std::size_t kTail = 1024U * 1024U;
constexpr unsigned char kUntouched = 0xa5;

void require(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rufus-writer-tests-" + std::to_string(nonce));
    require(std::filesystem::create_directory(path_), "create isolated test directory");
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::vector<unsigned char> pattern(const std::size_t size) {
  std::vector<unsigned char> bytes(size);
  for (std::size_t index = 0; index < size; ++index) {
    // Different blocks must differ, so an incorrect offset cannot pass by accident.
    bytes[index] = static_cast<unsigned char>((index * 37U + index / 251U) % 256U);
  }
  return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  require(static_cast<bool>(output), "write fixture: " + path.u8string());
}

std::vector<unsigned char> readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(static_cast<bool>(input), "open fixture: " + path.u8string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Range final {
  std::uint64_t offset;
  std::size_t size;
};

class MemorySource final : public RawSourceIo {
 public:
  explicit MemorySource(std::vector<unsigned char> data) : bytes(std::move(data)) {}
  std::uint64_t sizeBytes() const noexcept override { return bytes.size(); }
  bool readAt(const std::uint64_t offset, unsigned char* data, const std::size_t size,
              std::string& error) override {
    reads.push_back({offset, size});
    if (reads.size() == failReadCall) {
      error = failureMessage;
      return false;
    }
    require(offset <= bytes.size() && size <= bytes.size() - offset,
            "source read must be in bounds");
    std::copy_n(bytes.data() + offset, size, data);
    return true;
  }

  std::vector<unsigned char> bytes;
  std::vector<Range> reads;
  std::size_t failReadCall{};
  std::string failureMessage{"injected source read failure"};
};

class MemoryTarget final : public RawTargetIo {
 public:
  explicit MemoryTarget(const std::size_t size, const std::uint32_t sector = 512U)
      : bytes(size, kUntouched), sectorSize(sector) {}
  std::uint64_t capacityBytes() const noexcept override { return bytes.size(); }
  std::uint32_t logicalSectorSize() const noexcept override { return sectorSize; }
  bool writeAt(const std::uint64_t offset, const unsigned char* data,
               const std::size_t size, std::string& error) override {
    require(flushCalls == 0U, "all target writes must precede flushing");
    require(offset <= bytes.size() && size <= bytes.size() - offset,
            "target write must be in bounds");
    require(offset % sectorSize == 0U && size % sectorSize == 0U,
            "target writes must be sector aligned");
    writes.push_back({offset, size});
    if (writes.size() == failWriteCall) {
      // A native transport can modify a prefix before returning false.
      if (partialWriteOnFailure) {
        std::copy_n(data, std::min<std::size_t>(sectorSize, size), bytes.data() + offset);
      }
      error = failureMessage;
      return false;
    }
    std::copy_n(data, size, bytes.data() + offset);
    return true;
  }
  bool readAt(const std::uint64_t offset, unsigned char* data, const std::size_t size,
              std::string& error) override {
    require(flushCalls == 1U && !failFlush, "verification must follow a successful flush");
    require(offset <= bytes.size() && size <= bytes.size() - offset,
            "target read must be in bounds");
    reads.push_back({offset, size});
    if (reads.size() == failReadCall) {
      error = failureMessage;
      return false;
    }
    std::copy_n(bytes.data() + offset, size, data);
    if (corruptOffset && *corruptOffset >= offset && *corruptOffset - offset < size) {
      data[*corruptOffset - offset] ^= 0xffU;
    }
    return true;
  }
  bool flush(std::string& error) override {
    ++flushCalls;
    if (failFlush) {
      error = failureMessage;
      return false;
    }
    return true;
  }

  std::vector<unsigned char> bytes;
  std::uint32_t sectorSize;
  std::vector<Range> writes;
  std::vector<Range> reads;
  std::size_t failWriteCall{};
  std::size_t failReadCall{};
  std::size_t flushCalls{};
  bool partialWriteOnFailure{};
  bool failFlush{};
  std::optional<std::uint64_t> corruptOffset;
  std::string failureMessage{"injected target I/O failure"};
};

struct Fixture final {
  explicit Fixture(const std::size_t size = 3U * kBlock + 512U,
                   const std::size_t capacity = 2U * kTail)
      : bytes(pattern(size)), sourcePath(temporary.path() / "source.img"),
        destination(temporary.path() / "destination.img"), source(bytes), target(capacity) {
    writeFile(sourcePath, bytes);
    image.path = sourcePath.u8string();
    image.sizeBytes = size;
    image.format = ImageFormat::Raw;
    image.capabilities.rawWrite = true;
    device.stableId = "unit-test:memory-only";
    device.devicePath = (temporary.path() / "memory-target").u8string();
    device.capacityBytes = capacity;
    device.logicalSectorSize = 512U;
    device.bus = DeviceBus::Usb;
    device.removable = true;
    device.writable = true;
    device.wholeDevice = true;
  }

  RawWritePlan plan(const VerificationProfile verification = VerificationProfile::Standard,
                    const bool clearTail = false,
                    std::vector<RawWriteRange> ranges = {}) const {
    auto result = WritePlanBuilder{}.buildRawWriteWithVerification(
        image, device, verification, kBlock, clearTail, std::move(ranges));
    require(result.succeeded(), result.issues.empty() ? "build test plan"
                                                     : result.issues.front().message);
    return *result.plan;
  }

  RawWriteProgressCallback rawProgress() {
    return [this](const RawWriteProgress& progress) { rawEvents.push_back(progress); };
  }
  FileWriteProgressCallback fileProgress() {
    return [this](const FileWriteProgress& progress) { fileEvents.push_back(progress); };
  }
  bool rawStage(const RawWriteStage stage) const {
    return std::any_of(rawEvents.begin(), rawEvents.end(),
                       [stage](const auto& event) { return event.stage == stage; });
  }
  bool fileStage(const FileWriteStage stage) const {
    return std::any_of(fileEvents.begin(), fileEvents.end(),
                       [stage](const auto& event) { return event.stage == stage; });
  }
  void rawFailed(const RawWriteResult& result, const std::uint64_t written,
                 const bool destructive, const bool cancelled = false) const {
    require(!result.success && !result.error.empty(), "failure must include a diagnostic");
    require(result.cancelled == cancelled, "correct cancellation state");
    require(result.bytesWritten == written, "count only complete successful writes");
    require(result.destructiveWriteStarted == destructive, "report any possible target change");
    require(!result.verificationCompleted, "failed verification must not claim completion");
    require(!rawStage(RawWriteStage::Complete), "failed operation must never report Complete");
  }
  void fileFailed(const FileWriteResult& result, const bool cancelled = false) const {
    require(!result.success && !result.error.empty(), "failure must include a diagnostic");
    require(result.cancelled == cancelled, "correct cancellation state");
    require(!std::filesystem::exists(destination), "failure must not publish destination");
    require(!fileStage(FileWriteStage::Complete), "failure must not report Complete");
    noPartials();
  }
  void noPartials() const {
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path())) {
      require(entry.path().filename().string().find(".rufus-plus-plus-partial-") ==
                  std::string::npos,
              "temporary output must be removed on every exit path");
    }
  }

  TemporaryDirectory temporary;
  std::vector<unsigned char> bytes;
  std::filesystem::path sourcePath;
  std::filesystem::path destination;
  ImageInfo image;
  BlockDeviceInfo device;
  MemorySource source;
  MemoryTarget target;
  std::vector<RawWriteProgress> rawEvents;
  std::vector<FileWriteProgress> fileEvents;
};

struct Test final {
  std::string name;
  std::function<void()> run;
};

void addRawTests(std::vector<Test>& tests) {
  tests.push_back({"initialized ranges seek over gaps and write required zero metadata", [] {
    Fixture f(2U * kTail, 2U * kTail);
    std::fill_n(f.bytes.begin(), kBlock, static_cast<unsigned char>(0));
    f.source.bytes = f.bytes;
    writeFile(f.sourcePath, f.bytes);
    const std::vector<RawWriteRange> ranges{{0U, kBlock}, {kTail, 2U * kBlock},
                                           {2U * kTail - 512U, 512U}};
    const auto plan = f.plan(VerificationProfile::Standard, false, ranges);
    const auto result = RawImageWriter{}.write(plan, f.source, f.target, f.rawProgress());
    require(result.success && result.verificationCompleted &&
                result.bytesWritten == 3U * kBlock + 512U &&
                result.bytesVerified == result.bytesWritten, "count and verify only transferred bytes");
    for (std::size_t offset = 0U; offset < f.target.bytes.size(); ++offset) {
      const bool copied = std::any_of(ranges.begin(), ranges.end(), [offset](const auto& range) {
        return offset >= range.offsetBytes && offset - range.offsetBytes < range.sizeBytes;
      });
      require(f.target.bytes[offset] == (copied ? f.bytes[offset] : kUntouched),
              "all initialized bytes, including zeros, must match; all gaps must stay untouched");
    }
    for (const auto& event : f.rawEvents) {
      require(event.totalBytes == plan.bytesToWrite() && event.bytesProcessed <= event.totalBytes,
              "progress must describe actual transfers, not logical disk size");
    }
    require(f.source.reads.size() == f.target.writes.size() + f.target.reads.size(),
            "do not read skipped regions from the source either");
  }});
  const std::vector<std::pair<std::string, std::vector<RawWriteRange>>> invalidRanges{
      {"empty extent", {{0U, 0U}}}, {"offset alignment", {{1U, 512U}}},
      {"size alignment", {{0U, 513U}}}, {"out of bounds", {{2U * kTail, 512U}}},
      {"overlap", {{0U, kBlock}, {512U, kBlock}}},
      {"unsorted", {{kBlock, kBlock}, {0U, kBlock}}},
      {"overflow", {{~std::uint64_t{0} - 511U, 1024U}}}};
  for (const auto& invalid : invalidRanges) {
    tests.push_back({"reject initialized range / " + invalid.first, [ranges = invalid.second] {
      Fixture f(2U * kTail, 2U * kTail);
      require(!WritePlanBuilder{}.buildRawWriteWithVerification(
          f.image, f.device, VerificationProfile::Standard, kBlock, false, ranges).succeeded(),
          "invalid ranges must be rejected before any I/O");
    }});
  }
  for (const std::string mode : {"full verification", "tail clear", "compressed", "container", "ISO", "too many", "zero sector"}) {
    tests.push_back({"reject range mode / " + mode, [mode] {
      Fixture f(2U * kTail, 2U * kTail);
      std::vector<RawWriteRange> ranges{{0U, kBlock}};
      if (mode == "compressed") { f.image.compressed = true; f.image.expandedSizeBytes = f.image.sizeBytes; }
      if (mode == "container") f.image.containerPayloadOffsetBytes = 512U;
      if (mode == "ISO") f.image.format = ImageFormat::Iso;
      if (mode == "too many") ranges.resize(kMaximumRawWriteRanges + 1U, {0U, 512U});
      if (mode == "zero sector") f.device.logicalSectorSize = 0U;
      require(!WritePlanBuilder{}.buildRawWriteWithVerification(
          f.image, f.device, mode == "full verification" ? VerificationProfile::Full : VerificationProfile::Standard,
          kBlock, mode == "tail clear", ranges).succeeded(), "unsupported range modes must fail closed");
    }});
  }
  for (const std::string failure : {"source", "write", "partial write", "readback", "corruption", "cancel"}) {
    tests.push_back({"later initialized range failure / " + failure, [failure] {
      Fixture f(2U * kTail, 2U * kTail);
      const auto plan = f.plan(VerificationProfile::Standard, false, {{0U, kBlock}, {kTail, kBlock}});
      if (failure == "source") f.source.failReadCall = 2U;
      if (failure == "write" || failure == "partial write") f.target.failWriteCall = 2U;
      f.target.partialWriteOnFailure = failure == "partial write";
      if (failure == "readback") f.target.failReadCall = 2U;
      if (failure == "corruption") f.target.corruptOffset = kTail + 7U;
      const auto result = RawImageWriter{}.write(plan, f.source, f.target, f.rawProgress(), [&] {
        return failure == "cancel" && !f.target.writes.empty();
      });
      const bool fullyWritten = failure == "readback" || failure == "corruption";
      f.rawFailed(result, fullyWritten ? 2U * kBlock : kBlock, true, failure == "cancel");
      require(f.target.bytes[kBlock] == kUntouched, "failure must not touch the skipped gap");
    }});
  }
  tests.push_back({"fast verification checks tiny metadata ranges between large ranges", [] {
    Fixture f(4U * kTail, 4U * kTail);
    const auto plan = f.plan(VerificationProfile::Fast, false,
        {{0U, 40U * kBlock}, {kTail, kBlock}, {3U * kTail, 40U * kBlock}});
    f.target.corruptOffset = kTail + 1U;
    const auto result = RawImageWriter{}.write(plan, f.source, f.target, f.rawProgress());
    require(!result.success && !result.verificationCompleted && !f.rawStage(RawWriteStage::Complete),
            "sampling must not miss a small isolated metadata range");
  }});
  for (const auto profile : {VerificationProfile::None, VerificationProfile::Fast,
                             VerificationProfile::Standard, VerificationProfile::Full}) {
    tests.push_back({std::string("copy / ") + verificationProfileName(profile), [profile] {
      constexpr std::size_t size = 3U * kBlock + 512U;
      Fixture f(size, profile == VerificationProfile::Full ? size : 2U * kTail);
      const auto result = RawImageWriter{}.write(f.plan(profile), f.source, f.target,
                                                 f.rawProgress());
      require(result.success && !result.cancelled && result.error.empty(), "copy succeeds");
      require(result.destructiveWriteStarted && result.bytesWritten == size, "complete byte count");
      require(std::equal(f.bytes.begin(), f.bytes.end(), f.target.bytes.begin()), "exact image bytes");
      require(std::all_of(f.target.bytes.begin() + size, f.target.bytes.end(),
                          [](auto byte) { return byte == kUntouched; }), "preserve unwritten bytes");
      require(f.target.writes.size() == 4U && f.target.writes.back().offset == 3U * kBlock &&
                  f.target.writes.back().size == 512U, "copy short final sector exactly once");
      require(f.target.flushCalls == 1U, "flush even when verification is disabled");
      const bool verified = profile != VerificationProfile::None;
      require(result.verificationCompleted == verified &&
                  result.bytesVerified == (verified ? size : 0U), "verification byte count");
      require(f.target.reads.empty() == !verified, "no readback with verification disabled");
      require(f.rawEvents.front().stage == RawWriteStage::Revalidating &&
                  f.rawEvents.back().stage == RawWriteStage::Complete, "progress stage boundaries");
      for (const auto& event : f.rawEvents) {
        require(event.totalBytes == size && event.bytesProcessed <= size, "bounded progress");
      }
    }});
  }

  tests.push_back({"fast verification samples first and final blocks", [] {
    Fixture f(32U * kBlock + 512U);
    const auto result = RawImageWriter{}.write(f.plan(VerificationProfile::Fast), f.source, f.target);
    require(result.success && result.verificationCompleted, "sampled verification succeeds");
    require(result.bytesVerified > kBlock && result.bytesVerified < f.bytes.size(), "samples a subset");
    require(f.target.reads.front().offset == 0U &&
                f.target.reads.back().offset == 32U * kBlock &&
                f.target.reads.back().size == 512U, "verify both ends including short last block");
    std::uint64_t counted = 0;
    for (const auto& range : f.target.reads) {
      counted += range.size;
    }
    require(result.bytesVerified == counted, "count bytes actually compared");
  }});

  for (const bool finalBlock : {false, true}) {
    tests.push_back({finalBlock ? "fast verification detects final-block corruption"
                                : "standard verification detects middle-block corruption", [finalBlock] {
      Fixture f(32U * kBlock + 512U);
      f.target.corruptOffset = finalBlock ? f.bytes.size() - 1U : kBlock + 19U;
      const auto result = RawImageWriter{}.write(
          f.plan(finalBlock ? VerificationProfile::Fast : VerificationProfile::Standard),
          f.source, f.target, f.rawProgress());
      f.rawFailed(result, f.bytes.size(), true);
    }});
  }

  for (const std::string change : {"source smaller", "source larger", "capacity", "sector size"}) {
    tests.push_back({"revalidate / " + change, [change] {
      Fixture f;
      const auto plan = f.plan();
      if (change == "source smaller") f.source.bytes.pop_back();
      if (change == "source larger") f.source.bytes.push_back(0U);
      if (change == "capacity") f.target.bytes.resize(f.target.bytes.size() + 512U);
      if (change == "sector size") f.target.sectorSize = 4096U;
      const auto result = RawImageWriter{}.write(plan, f.source, f.target, f.rawProgress());
      f.rawFailed(result, 0U, false);
      require(f.source.reads.empty() && f.target.writes.empty() && f.target.flushCalls == 0U,
              "reject changed geometry before all I/O");
    }});
  }

  for (const std::size_t call : {1U, 2U, 5U}) {
    for (const bool diagnostic : {false, true}) {
      tests.push_back({"source read failure / call " + std::to_string(call) +
                          (diagnostic ? " / diagnostic" : " / fallback"), [call, diagnostic] {
        Fixture f;
        f.source.failReadCall = call;  // Four write reads, then verification.
        if (!diagnostic) f.source.failureMessage.clear();
        const auto result = RawImageWriter{}.write(f.plan(), f.source, f.target, f.rawProgress());
        const auto written = call == 1U ? 0U : call == 2U ? kBlock : f.bytes.size();
        f.rawFailed(result, written, call != 1U);
        if (diagnostic) require(result.error == f.source.failureMessage, "preserve source diagnostic");
        require(f.target.flushCalls == (call == 5U ? 1U : 0U), "stop immediately on source error");
      }});
    }
  }

  for (const std::size_t call : {1U, 2U}) {
    for (const bool diagnostic : {false, true}) {
      tests.push_back({"partial target write failure / call " + std::to_string(call) +
                          (diagnostic ? " / diagnostic" : " / fallback"), [call, diagnostic] {
        Fixture f;
        f.target.failWriteCall = call;
        f.target.partialWriteOnFailure = true;
        if (!diagnostic) f.target.failureMessage.clear();
        const auto result = RawImageWriter{}.write(f.plan(), f.source, f.target, f.rawProgress());
        f.rawFailed(result, (call - 1U) * kBlock, true);
        require(f.target.bytes[(call - 1U) * kBlock] == f.bytes[(call - 1U) * kBlock],
                "failing transport really modified part of the target");
        require(f.target.flushCalls == 0U && f.target.reads.empty(), "no I/O after write failure");
        if (diagnostic) require(result.error == f.target.failureMessage, "preserve write diagnostic");
      }});
    }
  }

  for (const bool readFailure : {false, true}) {
    for (const bool diagnostic : {false, true}) {
      tests.push_back({std::string(readFailure ? "readback" : "flush") + " failure" +
                          (diagnostic ? " / diagnostic" : " / fallback"), [readFailure, diagnostic] {
        Fixture f;
        f.target.failFlush = !readFailure;
        f.target.failReadCall = readFailure ? 2U : 0U;
        if (!diagnostic) f.target.failureMessage.clear();
        const auto result = RawImageWriter{}.write(f.plan(), f.source, f.target, f.rawProgress());
        f.rawFailed(result, f.bytes.size(), true);
        require(result.bytesVerified == (readFailure ? kBlock : 0U), "retain verification progress");
        if (diagnostic) require(result.error == f.target.failureMessage, "preserve target diagnostic");
        if (!readFailure) require(f.target.reads.empty(), "failed flush prevents verification");
      }});
    }
  }

  tests.push_back({"tail clearing preserves image and gap, verifies all cleared bytes", [] {
    Fixture f;
    const auto result = RawImageWriter{}.write(f.plan(VerificationProfile::Standard, true),
                                               f.source, f.target, f.rawProgress());
    require(result.success && result.verificationCompleted, "tail clear and verification succeed");
    require(std::equal(f.bytes.begin(), f.bytes.end(), f.target.bytes.begin()), "preserve image");
    require(std::all_of(f.target.bytes.begin() + f.bytes.size(), f.target.bytes.begin() + kTail,
                        [](auto byte) { return byte == kUntouched; }), "preserve gap before tail");
    require(std::all_of(f.target.bytes.begin() + kTail, f.target.bytes.end(),
                        [](auto byte) { return byte == 0U; }), "clear entire final MiB");
    require(result.bytesWritten == f.bytes.size() &&
                result.bytesVerified == f.bytes.size() + kTail, "separate image and tail accounting");
    require(f.rawStage(RawWriteStage::ClearingTargetMetadata) &&
                f.rawStage(RawWriteStage::VerifyingTargetMetadata), "report tail stages");
  }});

  for (const std::string fault : {"write", "read", "corruption"}) {
    tests.push_back({"tail failure / " + fault, [fault] {
      Fixture f;
      if (fault == "write") f.target.failWriteCall = 5U;
      if (fault == "read") f.target.failReadCall = 5U;
      if (fault == "corruption") f.target.corruptOffset = f.target.bytes.size() - 1U;
      const auto result = RawImageWriter{}.write(f.plan(VerificationProfile::Standard, true),
                                                 f.source, f.target, f.rawProgress());
      f.rawFailed(result, f.bytes.size(), true);
      require(f.target.flushCalls == (fault == "write" ? 0U : 1U), "tail failure stops later I/O");
    }});
  }

  for (const auto stage : {RawWriteStage::Revalidating, RawWriteStage::Writing,
                           RawWriteStage::ClearingTargetMetadata, RawWriteStage::Verifying,
                           RawWriteStage::VerifyingTargetMetadata}) {
    tests.push_back({std::string("cancel / ") + rawWriteStageName(stage), [stage] {
      Fixture f;
      bool cancel = false;
      const auto result = RawImageWriter{}.write(
          f.plan(VerificationProfile::Standard, true), f.source, f.target,
          [&](const RawWriteProgress& progress) {
            f.rawEvents.push_back(progress);
            if (progress.stage == stage) cancel = true;
          }, [&] { return cancel; });
      const auto written = stage == RawWriteStage::Revalidating ? 0U :
                           stage == RawWriteStage::Writing ? kBlock : f.bytes.size();
      f.rawFailed(result, written, stage != RawWriteStage::Revalidating, true);
      if (stage == RawWriteStage::Revalidating) {
        require(f.target.writes.empty() && f.source.reads.empty(), "pre-cancel leaves target untouched");
      }
    }});
  }

  for (const std::string change : {"missing", "size", "timestamp"}) {
    tests.push_back({"file source changed before write / " + change, [change] {
      Fixture f;
      const auto plan = f.plan();
      if (change == "missing") std::filesystem::remove(f.sourcePath);
      if (change == "size") std::filesystem::resize_file(f.sourcePath, f.bytes.size() + 512U);
      if (change == "timestamp") {
        std::filesystem::last_write_time(f.sourcePath,
            plan.sourceLastWriteTime() + std::chrono::seconds(2));
      }
      const auto result = RawImageWriter{}.write(plan, f.target, f.rawProgress());
      f.rawFailed(result, 0U, false);
      require(f.target.writes.empty() && f.target.flushCalls == 0U, "revalidate file before writes");
    }});
  }

  tests.push_back({"file source revalidation precedes Complete", [] {
    Fixture f;
    const auto plan = f.plan(VerificationProfile::None);
    const auto result = RawImageWriter{}.write(plan, f.target,
        [&](const RawWriteProgress& progress) {
          f.rawEvents.push_back(progress);
          if (progress.stage == RawWriteStage::Writing) {
            std::filesystem::last_write_time(f.sourcePath,
                plan.sourceLastWriteTime() + std::chrono::seconds(2));
          }
        });
    f.rawFailed(result, f.bytes.size(), true);
  }});

  tests.push_back({"file source overload copies and verifies", [] {
    Fixture f;
    const auto result = RawImageWriter{}.write(f.plan(), f.target, f.rawProgress());
    require(result.success && result.verificationCompleted &&
                result.bytesVerified == f.bytes.size(), "file source verified successfully");
    require(std::equal(f.bytes.begin(), f.bytes.end(), f.target.bytes.begin()), "file source exact bytes");
    require(f.rawEvents.back().stage == RawWriteStage::Complete, "report success after revalidation");
  }});
}

void addFileTests(std::vector<Test>& tests) {
  for (const bool verify : {false, true}) {
    for (const std::size_t size : {kBlock, 3U * kBlock + 37U}) {
      tests.push_back({std::string("copy / ") + (verify ? "verified / " : "unverified / ") +
                          std::to_string(size), [verify, size] {
        Fixture f(size);
        const auto result = FileImageWriter{}.write(f.image, f.destination, {kBlock, verify},
                                                     f.fileProgress());
        require(result.success && !result.cancelled && result.error.empty(), "file copy succeeds");
        require(result.bytesWritten == f.bytes.size() && readFile(f.destination) == f.bytes,
                "publish exact source bytes including short final block");
        require(f.fileEvents.front().stage == FileWriteStage::Validating &&
                    f.fileEvents.back().stage == FileWriteStage::Complete, "progress stage boundaries");
        require(f.fileStage(FileWriteStage::Flushing) &&
                    f.fileStage(FileWriteStage::Verifying) == verify, "optional verification");
        std::uint64_t written = 0U, verified = 0U;
        for (const auto& event : f.fileEvents) {
          require(event.totalBytes == size && event.bytesProcessed <= size, "bounded progress");
          if (event.stage == FileWriteStage::Writing) {
            require(event.bytesProcessed > written, "write progress increases");
            written = event.bytesProcessed;
          }
          if (event.stage == FileWriteStage::Verifying) {
            require(event.bytesProcessed > verified, "verification progress increases");
            verified = event.bytesProcessed;
          }
        }
        require(written == size && verified == (verify ? size : 0U), "final progress byte counts");
        f.noPartials();
      }});
    }
  }

  for (const std::size_t blockSize : {std::size_t{0}, kBlock - 1U,
                                     std::size_t{64U * 1024U * 1024U + 1U}}) {
    tests.push_back({"reject block size / " + std::to_string(blockSize), [blockSize] {
      Fixture f;
      const auto result = FileImageWriter{}.write(f.image, f.destination, {blockSize, true},
                                                   f.fileProgress());
      f.fileFailed(result);
      require(result.bytesWritten == 0U, "reject invalid block size before writing");
    }});
  }

  for (const std::string invalid : {"empty source path", "zero size", "missing source", "stale size",
                                    "missing parent", "empty destination", "directory", "unix device",
                                    "windows device"}) {
    tests.push_back({"reject / " + invalid, [invalid] {
      Fixture f;
      auto destination = f.destination;
      if (invalid == "empty source path") f.image.path.clear();
      if (invalid == "zero size") f.image.sizeBytes = 0U;
      if (invalid == "missing source") std::filesystem::remove(f.sourcePath);
      if (invalid == "stale size") ++f.image.sizeBytes;
      if (invalid == "missing parent") destination = f.temporary.path() / "absent" / "out.img";
      if (invalid == "empty destination") destination.clear();
      if (invalid == "directory") destination = f.temporary.path();
      if (invalid == "unix device") destination = "/dev/rufus-unit-test-never-opened";
      if (invalid == "windows device") destination = R"(\\.\PhysicalDrive99999)";
      const auto result = FileImageWriter{}.write(f.image, destination, {kBlock, true},
                                                   f.fileProgress());
      f.fileFailed(result);
      require(result.bytesWritten == 0U, "invalid input never reaches writes");
    }});
  }

  tests.push_back({"existing destination is preserved", [] {
    Fixture f;
    const auto original = pattern(73U);
    writeFile(f.destination, original);
    const auto result = FileImageWriter{}.write(f.image, f.destination);
    require(!result.success && !result.error.empty() && result.bytesWritten == 0U,
            "reject existing destination before any write");
    require(readFile(f.destination) == original, "never replace an existing file");
    f.noPartials();
  }});

  tests.push_back({"source cannot also be destination", [] {
    Fixture f;
    const auto result = FileImageWriter{}.write(f.image, f.sourcePath);
    require(!result.success && result.bytesWritten == 0U, "reject self-overwrite");
    require(readFile(f.sourcePath) == f.bytes, "preserve source file");
    f.noPartials();
  }});

  for (const auto stage : {FileWriteStage::Validating, FileWriteStage::Writing,
                           FileWriteStage::Flushing, FileWriteStage::Verifying}) {
    tests.push_back({"cancel / stage " + std::to_string(static_cast<int>(stage)), [stage] {
      Fixture f;
      bool cancel = false;
      const auto result = FileImageWriter{}.write(f.image, f.destination, {kBlock, true},
          [&](const FileWriteProgress& progress) {
            f.fileEvents.push_back(progress);
            if (progress.stage == stage) cancel = true;
          }, [&] { return cancel; });
      f.fileFailed(result, true);
      const auto expected = stage == FileWriteStage::Validating ? 0U :
                            stage == FileWriteStage::Writing ? kBlock : f.bytes.size();
      require(result.bytesWritten == expected, "cancel retains accurate copy progress");
    }});
  }

  tests.push_back({"source truncation during write removes partial output", [] {
    // Large enough that truncation affects bytes beyond the stream's read buffer.
    Fixture f(128U * kBlock);
    const auto result = FileImageWriter{}.write(f.image, f.destination, {kBlock, true},
        [&](const FileWriteProgress& progress) {
          f.fileEvents.push_back(progress);
          if (progress.stage == FileWriteStage::Writing && progress.bytesProcessed == kBlock) {
            std::filesystem::resize_file(f.sourcePath, kBlock);
          }
        });
    f.fileFailed(result);
    require(result.bytesWritten >= kBlock && result.bytesWritten < f.image.sizeBytes,
            "truncated source must not appear fully written");
  }});

  tests.push_back({"corrupt temporary output fails verification and is removed", [] {
    Fixture f(128U * kBlock);
    bool corrupted = false;
    const auto result = FileImageWriter{}.write(f.image, f.destination, {kBlock, true},
        [&](const FileWriteProgress& progress) {
          f.fileEvents.push_back(progress);
          // At first verification progress the output stream is closed.
          if (progress.stage == FileWriteStage::Verifying && !corrupted) {
            for (const auto& entry : std::filesystem::directory_iterator(f.temporary.path())) {
              if (entry.path() != f.sourcePath) {
                std::fstream output(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
                output.seekp(static_cast<std::streamoff>(f.bytes.size() - 1U));
                output.put(static_cast<char>(f.bytes.back() ^ 0xffU));
                output.close();
                require(static_cast<bool>(output), "inject verification corruption");
                corrupted = true;
              }
            }
          }
        });
    require(corrupted, "corruption injection must run");
    f.fileFailed(result);
    require(result.bytesWritten == f.bytes.size(), "verification fails after complete copy");
  }});

  tests.push_back({"failed commit removes partial output and preserves competing directory", [] {
    Fixture f;
    const auto occupied = f.destination / "keep.txt";
    const auto original = pattern(23U);
    const auto result = FileImageWriter{}.write(f.image, f.destination, {kBlock, true},
        [&](const FileWriteProgress& progress) {
          f.fileEvents.push_back(progress);
          if (progress.stage == FileWriteStage::Flushing) {
            std::filesystem::create_directory(f.destination);
            writeFile(occupied, original);
          }
        });
    require(!result.success && !result.cancelled && !result.error.empty(), "commit must fail");
    require(!f.fileStage(FileWriteStage::Complete), "failed commit must not report Complete");
    require(readFile(occupied) == original, "preserve unrelated data at destination");
    f.noPartials();
  }});
}

}  // namespace

int main(const int argc, const char* const argv[]) {
  if (argc != 2 || (std::string(argv[1]) != "raw" && std::string(argv[1]) != "file")) {
    std::cerr << "Usage: " << argv[0] << " raw|file\n";
    return 2;
  }
  std::vector<Test> tests;
  if (std::string(argv[1]) == "raw") addRawTests(tests);
  else addFileTests(tests);
  std::size_t failures = 0U;
  for (const auto& test : tests) {
    try {
      test.run();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " " << argv[1]
            << " writer cases passed\n";
  return failures == 0U ? 0 : 1;
}
