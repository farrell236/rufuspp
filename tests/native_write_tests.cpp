/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "system_raw_io.hpp"
#include "raw_write_safety.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#endif

namespace {
using namespace rufus::core;
using namespace rufus::backend::detail;

void check(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Call final { std::string name; std::uint64_t offset; std::size_t size; };
class ScriptedIo final : public NativeIo {
 public:
  std::vector<unsigned char> bytes = std::vector<unsigned char>(16384U, 0xa5U);
  std::vector<Call> calls;
  std::deque<NativeIoResult> reads, writes, fileSyncs, deviceSyncs;
  std::size_t transferLimit{4096U};
  std::uint64_t offsetLimit{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
  NativeIoResult read(const std::uint64_t offset, unsigned char* data,
                       const std::size_t size) override {
    calls.push_back({"read", offset, size});
    const auto result = next(reads, size);
    if (result.success && result.bytes <= size) {
      check(offset <= bytes.size() && result.bytes <= bytes.size() - offset, "fake read range");
      std::copy_n(bytes.data() + offset, result.bytes, data);
    }
    return result;
  }
  NativeIoResult write(const std::uint64_t offset, const unsigned char* data,
                        const std::size_t size) override {
    calls.push_back({"write", offset, size});
    const auto result = next(writes, size);
    if (result.success && result.bytes <= size) {
      check(offset <= bytes.size() && result.bytes <= bytes.size() - offset, "fake write range");
      std::copy_n(data, result.bytes, bytes.data() + offset);
    }
    return result;
  }
  NativeIoResult syncFile() override {
    calls.push_back({"fsync", 0, 0});
    return next(fileSyncs, 0);
  }
  NativeIoResult syncDevice() override {
    calls.push_back({"cache", 0, 0});
    return next(deviceSyncs, 0);
  }
  std::size_t maximumTransfer() const noexcept override { return transferLimit; }
  std::uint64_t maximumOffset() const noexcept override { return offsetLimit; }

 private:
  static NativeIoResult next(std::deque<NativeIoResult>& script, const std::size_t size) {
    if (script.empty()) return {size, true, false, {}};
    auto result = script.front();
    script.pop_front();
    return result;
  }
};

struct Fixture final {
  Fixture() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = std::filesystem::temp_directory_path() /
                ("rufus-native-tests-" + std::to_string(nonce));
    check(std::filesystem::create_directory(directory), "create isolated fixture");
    source = directory / "source.img";
    std::ofstream output(source, std::ios::binary);
    std::vector<char> bytes(8192U, 'x');
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "create source");
    image.path = source.u8string();
    image.sizeBytes = bytes.size();
    image.format = ImageFormat::Raw;
    image.capabilities.rawWrite = true;
    device.stableId = "test:identity";
    device.serialNumber = "known-serial";
    device.devicePath = (directory / "unopened-target").u8string();
    device.capacityBytes = 16384U;
    device.logicalSectorSize = 512U;
    device.bus = DeviceBus::Usb;
    device.removable = device.writable = device.wholeDevice = true;
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  RawWritePlan plan() const {
    auto result = WritePlanBuilder{}.buildRawWrite(image, device, 4096U);
    check(result.succeeded(), "build fixture plan");
    return *result.plan;
  }
  std::filesystem::path directory, source;
  ImageInfo image;
  BlockDeviceInfo device;
};

struct Test final { std::string name; std::function<void()> run; };

void transportTests(std::vector<Test>& tests) {
  for (const bool writing : {false, true}) {
    const auto prefix = std::string(writing ? "write / " : "read / ");
    tests.push_back({prefix + "short calls resume at correct offsets", [writing] {
      ScriptedIo io;
      auto& script = writing ? io.writes : io.reads;
      script = {{512U, true, false, {}}, {1024U, true, false, {}}};
      std::array<unsigned char, 4096U> buffer{};
      buffer.fill(0x73U);
      NativeRawTarget target(io, io.bytes.size(), 512U);
      std::string error;
      const bool success = writing ? target.writeAt(4096U, buffer.data(), buffer.size(), error)
                                   : target.readAt(4096U, buffer.data(), buffer.size(), error);
      check(success && error.empty(), "short calls complete");
      check(io.calls.size() == 3U && io.calls[0].offset == 4096U &&
                io.calls[1].offset == 4608U && io.calls[2].offset == 5632U &&
                io.calls[2].size == 2560U, "advance offset and remaining length exactly");
      const auto value = writing ? 0x73U : 0xa5U;
      check(std::all_of(buffer.begin(), buffer.end(),
                         [value](auto byte) { return byte == value; }), "read bytes intact");
      if (writing) {
        check(std::all_of(io.bytes.begin() + 4096U, io.bytes.begin() + 8192U,
                           [](auto byte) { return byte == 0x73U; }), "short writes preserve every byte");
        check(io.bytes[4095U] == 0xa5U && io.bytes[8192U] == 0xa5U, "no adjacent write");
      }
    }});
    tests.push_back({prefix + "EINTR retries without advancing", [writing] {
      ScriptedIo io;
      auto& script = writing ? io.writes : io.reads;
      script = {{0U, false, true, "interrupted"}, {512U, true, false, {}},
                {0U, false, true, "interrupted"}};
      std::array<unsigned char, 4096U> buffer{};
      std::string error;
      check(nativeTransfer(io, writing, 0U, buffer.data(), buffer.size(), error), "retry interruptions");
      check(io.calls.size() == 4U && io.calls[0].offset == 0U && io.calls[1].offset == 0U &&
                io.calls[2].offset == 512U && io.calls[3].offset == 512U, "do not skip interrupted data");
    }});
    for (const std::string fault : {"zero", "oversize", "permission", "disconnect", "empty diagnostic"}) {
      tests.push_back({prefix + "failure after partial transfer / " + fault, [writing, fault] {
        ScriptedIo io;
        auto& script = writing ? io.writes : io.reads;
        script.push_back({512U, true, false, {}});
        if (fault == "zero") script.push_back({0U, true, false, {}});
        else if (fault == "oversize") script.push_back({4097U, true, false, {}});
        else script.push_back({0U, false, false, fault == "empty diagnostic" ? "" : fault});
        std::array<unsigned char, 4096U> buffer{};
        std::string error;
        check(!nativeTransfer(io, writing, 0U, buffer.data(), buffer.size(), error), "fail incomplete I/O");
        check(!error.empty() && io.calls.size() == 2U, "stop with diagnostic, no spin or later I/O");
        if (fault == "permission" || fault == "disconnect") check(error == fault, "preserve OS error");
      }});
    }
    tests.push_back({prefix + "per-call platform size cap", [writing] {
      ScriptedIo io;
      io.transferLimit = 1024U;
      std::array<unsigned char, 4096U> buffer{};
      std::string error;
      check(nativeTransfer(io, writing, 0U, buffer.data(), buffer.size(), error), "chunk large transfer");
      check(io.calls.size() == 4U && io.calls.back().offset == 3072U, "respect native call limit");
    }});
    for (const std::string invalid : {"beyond capacity", "cross capacity", "unaligned offset",
                                       "unaligned size", "unknown sector", "offset overflow", "range overflow"}) {
      tests.push_back({prefix + "reject / " + invalid, [writing, invalid] {
        ScriptedIo io;
        std::uint64_t offset = 0U, capacity = io.bytes.size();
        std::size_t size = 4096U;
        std::uint32_t sector = 512U;
        if (invalid == "beyond capacity") offset = capacity + 512U;
        if (invalid == "cross capacity") offset = capacity - 512U;
        if (invalid == "unaligned offset") offset = 1U;
        if (invalid == "unaligned size") size = 4095U;
        if (invalid == "unknown sector") sector = 0U;
        if (invalid == "offset overflow" || invalid == "range overflow") {
          io.offsetLimit = 8191U;
          offset = invalid == "offset overflow" ? 8192U : 7680U;
        }
        NativeRawTarget target(io, capacity, sector);
        std::array<unsigned char, 4096U> buffer{};
        std::string error;
        const bool success = writing ? target.writeAt(offset, buffer.data(), size, error)
                                     : target.readAt(offset, buffer.data(), size, error);
        check(!success && !error.empty() && io.calls.empty(), "reject before entering OS");
      }});
    }
  }
  for (const bool deviceFailure : {false, true}) {
    tests.push_back({deviceFailure ? "flush / cache failure" : "flush / fsync failure", [deviceFailure] {
      ScriptedIo io;
      (deviceFailure ? io.deviceSyncs : io.fileSyncs).push_back({0U, false, false, "flush failure"});
      NativeRawTarget target(io, io.bytes.size(), 512U);
      std::string error;
      check(!target.flush(error) && error == "flush failure", "propagate flush error");
      check(io.calls.size() == (deviceFailure ? 2U : 1U) && io.calls.front().name == "fsync",
            "file flush precedes cache operation; stop on failure");
    }});
  }
  tests.push_back({"flush / interrupted file and cache flushes retry", [] {
    ScriptedIo io;
    io.fileSyncs.push_back({0U, false, true, "interrupted"});
    io.deviceSyncs.push_back({0U, false, true, "interrupted"});
    NativeRawTarget target(io, io.bytes.size(), 512U);
    std::string error;
    check(target.flush(error), "flush retries interruptions");
    check(io.calls.size() == 4U && io.calls[0].name == "fsync" && io.calls[1].name == "fsync" &&
              io.calls[2].name == "cache" && io.calls[3].name == "cache", "preserve flush ordering");
  }});
  tests.push_back({"4Kn target / aligned read and write", [] {
    ScriptedIo io;
    NativeRawTarget target(io, io.bytes.size(), 4096U);
    std::array<unsigned char, 4096U> written{}, read{};
    written.fill(0x37U);
    std::string error;
    check(target.writeAt(4096U, written.data(), written.size(), error) &&
              target.readAt(4096U, read.data(), read.size(), error) && read == written,
          "support valid 4096-byte logical sectors");
    check(!target.writeAt(512U, written.data(), written.size(), error), "reject 512-byte offset on 4Kn");
  }});
  tests.push_back({"zero-length transfer / no system call", [] {
    ScriptedIo io;
    NativeRawTarget target(io, io.bytes.size(), 512U);
    std::string error;
    check(target.writeAt(io.bytes.size(), nullptr, 0U, error) &&
              target.readAt(io.bytes.size(), nullptr, 0U, error) && io.calls.empty(),
          "empty transfer at end performs no I/O");
  }});
  tests.push_back({"flush / empty native diagnostic still fails", [] {
    ScriptedIo io;
    io.deviceSyncs.push_back({0U, false, false, {}});
    NativeRawTarget target(io, io.bytes.size(), 512U);
    std::string error;
    check(!target.flush(error) && !error.empty(), "never hide a failed native cache operation");
  }});
}

void safetyTests(std::vector<Test>& tests) {
  for (const std::string change : {"stable id", "path", "capacity", "sector", "serial",
                                   "serial disappears", "system disk", "read only", "partition",
                                   "fixed disk", "identity disappears"}) {
    tests.push_back({"revalidation / " + change, [change] {
      Fixture f;
      const auto plan = f.plan();
      check(validateRawWriteTarget(plan, f.device).safe(), "initial device eligible");
      auto observed = f.device;
      if (change == "stable id") observed.stableId += "-replacement";
      if (change == "path") observed.devicePath += "-replacement";
      if (change == "capacity") observed.capacityBytes += 512U;
      if (change == "sector") observed.logicalSectorSize = 4096U;
      if (change == "serial") observed.serialNumber = "other-drive";
      if (change == "serial disappears") observed.serialNumber.clear();
      if (change == "system disk") observed.systemDevice = true;
      if (change == "read only") observed.writable = false;
      if (change == "partition") observed.wholeDevice = false;
      if (change == "fixed disk") { observed.removable = false; observed.bus = DeviceBus::Sata; }
      if (change == "identity disappears") observed.stableId.clear();
      check(!validateRawWriteTarget(plan, observed).safe(), "reject unsafe device after unmount");
    }});
  }
  tests.push_back({"revalidation / mount points may disappear normally", [] {
    Fixture f;
    f.device.mountPoints = {"/unit-test/mounted-volume"};
    const auto plan = f.plan();
    auto observed = f.device;
    observed.mountPoints.clear();
    check(validateRawWriteTarget(plan, observed).safe(), "unmount itself is not identity drift");
  }});
  for (const bool hardLink : {false, true}) {
    tests.push_back({hardLink ? "source alias / hard link" : "source alias / identical path", [hardLink] {
      Fixture f;
      const auto alias = f.directory / "alias.img";
      if (hardLink) std::filesystem::create_hard_link(f.source, alias);
      f.device.devicePath = (hardLink ? alias : f.source).u8string();
      check(!WritePlanBuilder{}.buildRawWrite(f.image, f.device, 4096U).succeeded(),
            "never build source-is-target plan through a file alias");
    }});
  }
  for (const std::string outcome : {"success", "refresh failure", "copy failure", "cancel"}) {
    tests.push_back({"native completion / " + outcome, [outcome] {
      Fixture f;
      ScriptedIo io;
      NativeRawTarget target(io, io.bytes.size(), 512U);
      const auto plan = f.plan();
      if (outcome == "copy failure") io.writes.push_back({0U, false, false, "write failed"});
      bool finalized = false;
      std::size_t completeEvents = 0U;
      const auto result = finishNativeWrite(plan, [&](const auto& progress) {
        return RawImageWriter{}.write(plan, target, progress, [&] { return outcome == "cancel"; });
      }, [&](std::string& error) {
        finalized = true;
        if (outcome == "refresh failure") { error = "partition refresh failed"; return false; }
        return true;
      }, [&](const RawWriteProgress& progress) {
        if (progress.stage == RawWriteStage::Complete) {
          check(finalized, "finalize before Complete");
          ++completeEvents;
        }
      });
      check(result.success == (outcome == "success"), "correct final success state");
      check(completeEvents == (result.success ? 1U : 0U), "only successful operation reports Complete");
      check(finalized == (outcome == "success" || outcome == "refresh failure"),
            "never refresh after failed or cancelled copying");
      if (outcome == "refresh failure") {
        check(result.destructiveWriteStarted && result.bytesWritten == f.image.sizeBytes &&
                  result.verificationCompleted && result.error == "partition refresh failed",
              "retain write and verification facts after native finalization failure");
      }
    }});
  }
  tests.push_back({"native completion / source changed after verified descriptor copy", [] {
    Fixture f;
    ScriptedIo io;
    NativeRawTarget target(io, io.bytes.size(), 512U);
    const auto plan = f.plan();
    auto source = openRawImageSource(plan.image());
    check(source.succeeded(), "open descriptor-style source");
    std::size_t completeEvents = 0U;
    const auto result = finishNativeWrite(plan, [&](const auto& progress) {
      return RawImageWriter{}.write(plan, *source.source, target, progress);
    }, [&](std::string& error) {
      const auto safety = WritePlanBuilder::validateSource(plan);
      if (!safety.safe()) { error = safety.issues.front().message; return false; }
      return true;
    }, [&](const RawWriteProgress& progress) {
      if (progress.stage == RawWriteStage::Writing) {
        std::filesystem::last_write_time(f.source,
            plan.sourceLastWriteTime() + std::chrono::seconds(2));
      }
      if (progress.stage == RawWriteStage::Complete) ++completeEvents;
    });
    check(!result.success && result.destructiveWriteStarted && result.verificationCompleted &&
              result.bytesWritten == f.image.sizeBytes && !result.error.empty() && completeEvents == 0U,
          "a source-change failure must supersede a verified copy without false completion");
  }});
  for (const bool outerSuccess : {false, true}) {
    tests.push_back({outerSuccess ? "helper completion / nested finalization succeeds once"
                                  : "helper completion / final descriptor check fails", [outerSuccess] {
      Fixture f;
      const auto plan = f.plan();
      ScriptedIo io;
      NativeRawTarget target(io, io.bytes.size(), 512U);
      bool checkedDescriptor = false;
      std::size_t completeEvents = 0U;
      const auto result = finishNativeWrite(plan, [&](const auto& outerProgress) {
        return finishNativeWrite(plan, [&](const auto& innerProgress) {
          return RawImageWriter{}.write(plan, target, innerProgress);
        }, [](std::string&) { return true; }, outerProgress);
      }, [&](std::string& error) {
        checkedDescriptor = true;
        if (!outerSuccess) error = "final descriptor identity changed";
        return outerSuccess;
      }, [&](const RawWriteProgress& progress) {
        if (progress.stage == RawWriteStage::Complete) {
          check(checkedDescriptor, "do not publish inner transport completion to helper client");
          ++completeEvents;
        }
      });
      check(result.success == outerSuccess && completeEvents == (outerSuccess ? 1U : 0U),
            "only outermost successful helper operation completes");
    }});
  }
}

void systemTests(std::vector<Test>& tests) {
  tests.push_back({"system calls / invalid handle read, write, flush", [] {
#if defined(_WIN32)
    SystemNativeIo io(INVALID_HANDLE_VALUE);
#else
    SystemNativeIo io(-1);
#endif
    std::array<unsigned char, 512U> bytes{};
    check(!io.read(0U, bytes.data(), bytes.size()).success, "invalid native read fails");
    check(!io.write(0U, bytes.data(), bytes.size()).success, "invalid native write fails");
    check(!io.syncFile().success, "invalid native flush fails");
  }});
  tests.push_back({"system calls / real file write, flush, close, reopen and readback", [] {
    Fixture f;
#if defined(_WIN32)
    const auto handle = CreateFileW(f.source.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "open owned fixture");
    struct Owner { HANDLE value; ~Owner() { CloseHandle(value); } } owner{handle};
#else
    const int handle = open(f.source.c_str(), O_RDWR | O_CLOEXEC);
    check(handle >= 0, "open owned fixture");
    struct Owner { int value; ~Owner() { if (value >= 0) close(value); } } owner{handle};
#endif
    SystemNativeIo io(handle);
    std::array<unsigned char, 4096U> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(i % 251U);
    std::string error;
    check(nativeTransfer(io, true, 4096U, bytes.data(), bytes.size(), error), "real OS write succeeds");
    check(io.syncFile().success, "real OS file flush succeeds");
#if defined(_WIN32)
    CloseHandle(owner.value);
    owner.value = INVALID_HANDLE_VALUE;
#else
    close(owner.value);
    owner.value = -1;
#endif
    std::ifstream reopened(f.source, std::ios::binary);
    reopened.seekg(4096);
    std::array<unsigned char, 4096U> read{};
    reopened.read(reinterpret_cast<char*>(read.data()), static_cast<std::streamsize>(read.size()));
    check(reopened.gcount() == static_cast<std::streamsize>(read.size()) && read == bytes,
          "independent reopened handle sees exact bytes");
  }});
  tests.push_back({"system calls / read-only handle refuses write", [] {
    Fixture f;
#if defined(_WIN32)
    const auto handle = CreateFileW(f.source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "open read-only fixture");
    struct Owner { HANDLE value; ~Owner() { CloseHandle(value); } } owner{handle};
#else
    const int handle = open(f.source.c_str(), O_RDONLY | O_CLOEXEC);
    check(handle >= 0, "open read-only fixture");
    struct Owner { int value; ~Owner() { close(value); } } owner{handle};
#endif
    SystemNativeIo io(handle);
    std::array<unsigned char, 512U> bytes{};
    const auto result = io.write(0U, bytes.data(), bytes.size());
    check(!result.success && !result.error.empty(), "propagate actual OS permission error");
  }});
}
}  // namespace

int main() {
  std::vector<Test> tests;
  transportTests(tests);
  safetyTests(tests);
  systemTests(tests);
  std::size_t failures = 0U;
  for (const auto& test : tests) {
    try { test.run(); std::cout << "PASS " << test.name << '\n'; }
    catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " native writer cases passed\n";
  return failures == 0U ? 0 : 1;
}
