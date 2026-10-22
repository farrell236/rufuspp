/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_fixture.hpp"
#include "../src/platform/src/ntfs_file_tree.hpp"
#include "../src/platform/src/ntfs_iso_format.hpp"
#include "../src/platform/src/ntfs_bootstrap.hpp"
#include "../src/portable/src/windows_boot_code.hpp"
#include "rufus/backend/ntfs_iso_image_stager.hpp"
#include "rufus/core/image_analyzer.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
#include "../third_party/windows-ntfs-boot/br_ntfs_0x0.h"
#include "../third_party/windows-ntfs-boot/br_ntfs_0x54.h"
using rufus::backend::detail::NtfsFileTree;
void require(const bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "rufus-ntfs-test-XXXXXX").string();
    const auto* created = mkdtemp(pattern.data());
    require(created != nullptr, "Creating private test directory");
    path = created;
  }
  ~TemporaryDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void format(const std::filesystem::path& path, const std::uint64_t bytes,
            const unsigned cluster = 4096) {
  std::ofstream output(path, std::ios::binary);
  output.seekp(static_cast<std::streamoff>(bytes - 1U));
  output.put('\0');
  output.close();
  require(static_cast<bool>(output), "Creating ordinary NTFS test file");
  const auto formatter = rufus::backend::detail::bundledNtfsFormatter();
  require(!formatter.empty(), "Bundled formatter available without PATH");
  const auto command = rufus::backend::detail::ntfsIsoFormatCommand(
      path.string(), true, cluster, "NTFS_TEST", formatter.string());
  std::vector<char*> argv;
  for (const auto& part : command) argv.push_back(const_cast<char*>(part.c_str()));
  argv.push_back(nullptr);
  const auto child = fork();
  require(child >= 0, "Starting private formatter");
  if (child == 0) { execv(formatter.c_str(), argv.data()); _exit(127); }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Formatting ordinary file with bundled formatter");
}
rufus::core::IsoDeploymentPlan planFor(const std::filesystem::path& iso,
    const rufus::core::PartitionScheme scheme, const std::uint64_t bytes,
    const bool quick = true,
    const rufus::core::IsoTargetSystem firmware = rufus::core::IsoTargetSystem::Uefi,
    const unsigned cluster = 4096) {
  const auto analysis = rufus::core::ImageAnalyzer{}.analyze(iso);
  require(analysis.succeeded(), "Analyzing source ISO");
  rufus::core::BlockDeviceInfo target;
  target.stableId = "unit-test:no-device-opened";
  target.devicePath = "/no-physical-device";
  target.capacityBytes = bytes;
  target.logicalSectorSize = 512;
  target.bus = rufus::core::DeviceBus::Usb;
  target.removable = target.writable = target.wholeDevice = true;
  rufus::core::IsoDeploymentOptions options;
  options.ntfsAvailable = true;
  options.partitionScheme = scheme;
  options.targetSystem = firmware;
  options.fileSystem = rufus::core::IsoFilesystemPreference::Ntfs;
  options.clusterSizeBytes = cluster;
  options.quickFormat = quick;
  const auto result = rufus::core::IsoDeploymentPlanner(
      rufus::core::createSystemWimSplitter(), options).build(*analysis.image, target, "WINDOWS_TEST");
  require(result.succeeded(), "Planning NTFS ISO deployment: " +
      (result.issues.empty() ? std::string{} : result.issues.front().message));
  return *result.plan;
}
void testFiles(const TemporaryDirectory& temporary) {
  std::string error;
  require(!NtfsFileTree::open("/dev/null", error), "Reject physical character devices");
  const auto path = temporary.path / "files.ntfs";
  constexpr std::uint64_t capacity = 128ULL * 1024ULL * 1024ULL;
  format(path, capacity);
  const auto link = temporary.path / "link.ntfs";
  std::filesystem::create_symlink(path, link);
  require(!NtfsFileTree::open(link, error), "Reject symlink volumes");
  std::filesystem::remove(link);
  std::filesystem::create_hard_link(path, link);
  require(!NtfsFileTree::open(path, error), "Reject hard-linked volumes");
  std::filesystem::remove(link);
  auto tree = NtfsFileTree::open(path, error);
  require(tree != nullptr, error);
  require(!tree->createFile("../escape", error), "Reject path traversal");
  require(!tree->createFile("/absolute", error), "Reject absolute paths");
  require(!tree->createFile("file:stream", error), "Reject alternate data streams");
  require(tree->createDirectories("empty/nested", error), error);
  require(tree->createDirectories(u8"case/café", error), error);
  require(!tree->createDirectories(u8"case/CAFÉ", error), "Reject Unicode case-colliding directories");
  std::vector<unsigned char> bytes(1024U * 1024U + 17U);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(i * 29U);
  const std::string unicode = u8"sources/日本語/café-😀.bin";
  require(tree->createFile(unicode, error), error);
  require(tree->write(bytes.data(), bytes.size(), error), error);
  require(tree->closeFile(error), error);
  require(!tree->createFile(u8"sources/日本語/CAFÉ-😀.BIN", error), "Reject Unicode case-colliding files");
  require(tree->createFile("empty.txt", error) && tree->closeFile(error), error);
  // Force MFT growth and a nonresident directory index.
  for (unsigned i = 0; i < 300; ++i) {
    require(tree->createFile("many/item-" + std::to_string(i), error), error);
    require(tree->write(bytes.data(), 79, error) && tree->closeFile(error), error);
  }
  require(!tree->createFile("EMPTY.TXT", error), "Reject case-colliding file names");
  require(tree->reopenReadOnly(error), error);
  require(!tree->createFile("readonly", error), "Read-only verification cannot write");
  require(!tree->openFileForRead(unicode, bytes.size() - 1U, error), "Reject wrong file size");
  require(tree->openFileForRead(unicode, bytes.size(), error), error);
  std::vector<unsigned char> observed(bytes.size());
  require(tree->read(observed.data(), observed.size(), error), error);
  require(observed == bytes, "Unicode file readback after closing and reopening volume");
  require(tree->openFileForRead("empty.txt", 0, error) && tree->closeFile(error), error);
  for (unsigned i = 0; i < 300; ++i) {
    require(tree->openFileForRead("many/item-" + std::to_string(i), 79, error), error);
    require(tree->read(observed.data(), 79, error), error);
    require(std::equal(bytes.begin(), bytes.begin() + 79, observed.begin()), "Directory index file readback");
  }
  require(tree->closeFile(error), error);
  std::vector<rufus::core::RawWriteRange> ranges;
  require(tree->allocatedRanges(ranges, capacity, error), error);
  require(!ranges.empty() && ranges.back().offsetBytes == capacity - 512U && ranges.back().sizeBytes == 512U,
          "Allocation map includes backup boot sector");
  require(tree->close(error), error);
}

class FaultTree final : public rufus::core::IsoDeploymentFileTree {
 public:
  FaultTree(NtfsFileTree& tree, const bool failFlush) : tree_(tree), failFlush_(failFlush) {}
  bool createDirectories(const std::string& p, std::string& e) override { return tree_.createDirectories(p, e); }
  bool createFile(const std::string& p, std::string& e) override { return tree_.createFile(p, e); }
  bool write(const unsigned char* p, std::size_t n, std::string& e) override { return tree_.write(p, n, e); }
  bool closeFile(std::string& e) override { return tree_.closeFile(e); }
  bool reopenReadOnly(std::string& e) override {
    if (failFlush_) { e = "Injected volume flush failure"; return false; }
    return tree_.reopenReadOnly(e);
  }
  bool openFileForRead(const std::string& p, std::uint64_t n, std::string& e) override {
    return tree_.openFileForRead(p, n, e);
  }
  bool read(unsigned char* p, std::size_t n, std::string& e) override {
    if (!tree_.read(p, n, e)) return false;
    if (n) p[0] ^= 1U;
    return true;
  }
 private:
  NtfsFileTree& tree_;
  bool failFlush_;
};

void testFailures(const TemporaryDirectory& temporary) {
  const auto source = temporary.path / "fault-source.iso";
  rufus::tests::writeIsoFixture(source);
  const auto plan = planFor(source, rufus::core::PartitionScheme::Gpt, 128ULL * 1024ULL * 1024ULL);
  for (const bool flush : {true, false}) {
    const auto path = temporary.path / (flush ? "flush.ntfs" : "corruption.ntfs");
    format(path, 64ULL * 1024ULL * 1024ULL);
    std::string error;
    auto tree = NtfsFileTree::open(path, error);
    require(tree != nullptr, error);
    FaultTree faulty(*tree, flush);
    const auto result = rufus::core::IsoImageStager{}.extractToFileTree(plan, faulty);
    require(!result.success && !result.error.empty(), "Flush failure or corrupt readback cannot report success");
  }
  const auto path = temporary.path / "full-filesystem.ntfs";
  format(path, 64ULL * 1024ULL * 1024ULL);
  std::string error;
  auto tree = NtfsFileTree::open(path, error);
  require(tree != nullptr && tree->createFile("too-large.bin", error), error);
  std::vector<unsigned char> bytes(1024U * 1024U, 0x37U);
  bool rejected = false;
  for (unsigned i = 0; i < 70; ++i) {
    if (!tree->write(bytes.data(), bytes.size(), error)) { rejected = true; break; }
  }
  require(rejected && !error.empty(), "NTFS allocation exhaustion reports failure");
}

void verifyAppliedRanges(const rufus::core::IsoDeploymentPlan& plan,
    const rufus::core::IsoDeploymentResult& result, const std::filesystem::path& stage,
    const std::filesystem::path& directory) {
  const auto target = directory / "nonzero-target.img";
  std::vector<char> buffer(1024U * 1024U, static_cast<char>(0xa5));
  std::ofstream initial(target, std::ios::binary | std::ios::trunc);
  for (std::uint64_t offset = 0; offset < plan.target().capacityBytes; offset += buffer.size()) {
    const auto n = std::min<std::uint64_t>(buffer.size(), plan.target().capacityBytes - offset);
    initial.write(buffer.data(), static_cast<std::streamsize>(n));
  }
  initial.close();
  std::ifstream source(stage, std::ios::binary);
  std::fstream output(target, std::ios::binary | std::ios::in | std::ios::out);
  for (const auto& range : result.writeRanges) {
    for (std::uint64_t offset = 0; offset < range.sizeBytes;) {
      const auto n = std::min<std::uint64_t>(buffer.size(), range.sizeBytes - offset);
      source.seekg(static_cast<std::streamoff>(range.offsetBytes + offset));
      source.read(buffer.data(), static_cast<std::streamsize>(n));
      output.seekp(static_cast<std::streamoff>(range.offsetBytes + offset));
      output.write(buffer.data(), static_cast<std::streamsize>(n));
      require(source && output, "Applying NTFS write map over stale nonzero data");
      offset += n;
    }
  }
  output.close();
  const auto analysis = rufus::core::ImageAnalyzer{}.analyze(target);
  require(analysis.succeeded() && analysis.image->partitionScheme == plan.partitionScheme(),
          "Written MBR/GPT metadata validates with skipped sectors still nonzero");
  std::ifstream input(target, std::ios::binary);
  const bool gpt = plan.partitionScheme() == rufus::core::PartitionScheme::Gpt;
  std::array<unsigned char, 16> extent{};
  input.seekg(gpt ? 2U * 512U + 32U : 446U + 8U);
  input.read(reinterpret_cast<char*>(extent.data()), gpt ? 16 : 8);
  const auto le = [&](const unsigned offset, const unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(extent[offset + i]) << (8U * i);
    return value;
  };
  const auto first = le(0, gpt ? 8 : 4);
  const auto partitionBytes = (gpt ? le(8, 8) - first + 1U : le(4, 4)) * 512U;
  input.seekg(1024U * 1024U);
  const auto partition = directory / "written-partition.ntfs";
  std::ofstream copy(partition, std::ios::binary | std::ios::trunc);
  for (std::uint64_t offset = 0; offset < partitionBytes;) {
    const auto n = std::min<std::uint64_t>(buffer.size(), partitionBytes - offset);
    input.read(buffer.data(), static_cast<std::streamsize>(n));
    copy.write(buffer.data(), static_cast<std::streamsize>(n));
    require(input && copy, "Reading written NTFS partition");
    offset += n;
  }
  copy.close();
  std::array<char, 8192> expectedBoot{}, writtenBoot{};
  source.seekg(1024U * 1024U);
  source.read(expectedBoot.data(), expectedBoot.size());
  input.seekg(1024U * 1024U);
  input.read(writtenBoot.data(), writtenBoot.size());
  require(source && input && expectedBoot == writtenBoot,
          "Allocation map transfers the entire NTFS bootstrap, including its later stages");
  std::string error;
  auto tree = NtfsFileTree::open(partition, error);
  require(tree != nullptr && tree->reopenReadOnly(error), error);
  for (const auto* name : {"bootmgr", "efi/boot/bootx64.efi"}) {
    std::array<unsigned char, 4> data{};
    require(tree->openFileForRead(name, data.size(), error) && tree->read(data.data(), data.size(), error), error);
    require(std::string(reinterpret_cast<char*>(data.data()), data.size()) == "test",
            "File bytes survive mapped write over stale media");
  }
  require(tree->close(error), error);
  std::ifstream boot(partition, std::ios::binary);
  std::array<char, 512> primary{}, backup{};
  boot.read(primary.data(), primary.size());
  boot.seekg(static_cast<std::streamoff>(partitionBytes - 512U));
  boot.read(backup.data(), backup.size());
  require(boot && primary == backup, "Mapped write includes the exact backup NTFS boot sector");
}

void testStage(const TemporaryDirectory& temporary) {
  const auto source = temporary.path / "source.iso";
  rufus::tests::writeIsoFixture(source);
  auto stager = rufus::backend::makePlatformNtfsIsoImageStager();
  require(stager->availability().available, "NTFS staging available without installed NTFS tools or root");
  using S = rufus::core::PartitionScheme;
  using F = rufus::core::IsoTargetSystem;
  const std::array<std::pair<S, F>, 4> profiles = {{{S::Mbr, F::Uefi}, {S::Gpt, F::Uefi},
                                                 {S::Mbr, F::Bios}, {S::Mbr, F::BiosAndUefi}}};
  unsigned profile = 0;
  for (const auto& [scheme, firmware] : profiles) {
    const auto plan = planFor(source, scheme, 128ULL * 1024ULL * 1024ULL, true, firmware,
                              firmware == F::BiosAndUefi ? 65536U : firmware == F::Bios ? 512U : 4096U);
    const auto output = temporary.path / ("profile-" + std::to_string(profile++) + ".img");
    unsigned completions = 0;
    const auto result = stager->stage(plan, output, [&](const auto& p) {
      if (p.stage == rufus::core::IsoDeploymentStage::Complete) ++completions;
    });
    require(result.success && result.stagedImage.has_value(), result.error);
    const bool bios = firmware != F::Uefi;
    require(plan.biosBootable() == bios && result.stagedImage->capabilities.biosBootable == bios &&
            result.stagedImage->capabilities.uefiBootable == (firmware != F::Bios),
            "Staged capabilities match the selected BIOS/UEFI profile");
    std::ifstream layout(output, std::ios::binary);
    std::array<unsigned char, 512> mbr{};
    layout.read(reinterpret_cast<char*>(mbr.data()), mbr.size());
    if (bios) require(mbr[446] == 0x80 && mbr[450] == 0x07 &&
        std::equal(rufus::core::detail::kWindowsMbrBootstrap.begin(),
                   rufus::core::detail::kWindowsMbrBootstrap.end(), mbr.begin()),
        "Windows MBR code selects the active NTFS partition");
    if (scheme == S::Mbr) require(mbr[466] == (firmware == F::Bios ? 0 : 0xef),
                                "Only UEFI targets contain the helper partition");
    require(completions == 1U && result.filesExtracted >= 3U, "Complete emitted only after assembly and verification");
    require(!result.writeRanges.empty(), "Quick NTFS stage exports allocation write map");
    std::uint64_t written = 0, end = 0;
    for (const auto& range : result.writeRanges) {
      require(range.offsetBytes >= end && range.sizeBytes != 0 && range.offsetBytes % 512U == 0 &&
              range.sizeBytes % 512U == 0 && range.offsetBytes + range.sizeBytes <= plan.target().capacityBytes,
              "Ordered, aligned and bounded write map");
      end = range.offsetBytes + range.sizeBytes;
      written += range.sizeBytes;
    }
    require(end == plan.target().capacityBytes && written < end / 2U,
            "Quick map preserves the disk tail without writing unused capacity");
    const auto analysis = rufus::core::ImageAnalyzer{}.analyze(output);
    require(analysis.succeeded() && analysis.image->partitionScheme == scheme,
            "Assembled partition layout validates");
    verifyAppliedRanges(plan, result, output, temporary.path);
    std::ifstream input(output, std::ios::binary);
    std::array<char, 512> boot{};
    input.seekg(1024U * 1024U);
    input.read(boot.data(), boot.size());
    require(input && std::string(boot.data() + 3, 8) == "NTFS    " &&
            static_cast<unsigned char>(boot[28]) == 0 && static_cast<unsigned char>(boot[29]) == 8,
            "Assembled NTFS boot sector records the actual 2048-sector offset");
    if (bios) {
      std::array<unsigned char, 8192> bootCode{};
      input.seekg(1024U * 1024U);
      input.read(reinterpret_cast<char*>(bootCode.data()), bootCode.size());
      require(input && std::equal(std::begin(br_ntfs_0x0), std::end(br_ntfs_0x0), bootCode.begin()) &&
              std::equal(std::begin(br_ntfs_0x54), std::end(br_ntfs_0x54), bootCode.begin() + 0x54),
              "Assembled Windows NTFS bootstrap matches the pinned upstream tables");
    }
    const auto refused = stager->stage(plan, output);
    require(!refused.success && std::filesystem::file_size(output) == plan.target().capacityBytes,
            "Existing output is preserved");
    bool cancel = false;
    const auto cancelledPath = temporary.path / "cancel.img";
    const auto cancelled = stager->stage(plan, cancelledPath, [&](const auto& p) {
      if (p.stage == rufus::core::IsoDeploymentStage::Extracting && p.bytesProcessed > 0) cancel = true;
    }, [&] { return cancel; });
    require(cancelled.cancelled && !cancelled.success && !std::filesystem::exists(cancelledPath) &&
            !std::filesystem::exists(cancelledPath.string() + ".ntfs-work"),
            "Cancellation removes partial outputs and private work directory");
  }
  const auto fullPlan = planFor(source, rufus::core::PartitionScheme::Gpt, 64ULL * 1024ULL * 1024ULL, false);
  const auto full = stager->stage(fullPlan, temporary.path / "full.img");
  require(full.success && full.writeRanges.empty(), "Full format retains dense zero-fill semantics: " + full.error);
}
void testBootstrapGuards(const TemporaryDirectory& temporary) {
  using rufus::backend::detail::installWindowsNtfsBootstrap;
  constexpr std::uint64_t capacity = 64ULL * 1024ULL * 1024ULL;
  std::string error;
  require(!installWindowsNtfsBootstrap("/dev/null", 0, capacity, error), "Bootstrap refuses devices");
  for (const unsigned cluster : {512U, 4096U, 65536U}) {
    const auto path = temporary.path / "bootstrap.ntfs";
    format(path, capacity, cluster);
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 8192> original{}, patched{};
    input.read(reinterpret_cast<char*>(original.data()), original.size());
    input.close();
    const auto link = temporary.path / "bootstrap-link";
    std::filesystem::create_symlink(path, link);
    require(!installWindowsNtfsBootstrap(link, 0, capacity, error), "Bootstrap refuses symlinks");
    std::filesystem::remove(link);
    std::filesystem::create_hard_link(path, link);
    require(!installWindowsNtfsBootstrap(path, 0, capacity, error), "Bootstrap refuses hard links");
    std::filesystem::remove(link);
    require(!installWindowsNtfsBootstrap(path, 512, capacity, error) &&
            !installWindowsNtfsBootstrap(path, 0, capacity - 512U, error),
            "Bootstrap refuses out-of-bounds or mismatched partition extents");
    // An inconsistent backup must be detected before the primary is modified.
    {
      std::fstream bad(path, std::ios::binary | std::ios::in | std::ios::out);
      bad.seekp(static_cast<std::streamoff>(capacity - 512U)); bad.put('!');
    }
    require(!installWindowsNtfsBootstrap(path, 0, capacity, error), "Bootstrap refuses inconsistent backup");
    {
      std::fstream repair(path, std::ios::binary | std::ios::in | std::ios::out);
      repair.read(reinterpret_cast<char*>(patched.data()), patched.size());
      require(patched == original, "Rejected bootstrap leaves the primary region untouched");
      repair.seekp(static_cast<std::streamoff>(capacity - 512U));
      repair.write(reinterpret_cast<char*>(original.data()), 512);
    }
    require(installWindowsNtfsBootstrap(path, 0, capacity, error), error);
    std::ifstream verify(path, std::ios::binary);
    verify.read(reinterpret_cast<char*>(patched.data()), patched.size());
    require(std::equal(original.begin() + 11, original.begin() + 0x54, patched.begin() + 11),
            "BPB, MFT locations and serial remain exact");
    require(std::equal(original.begin() + 0x54 + sizeof(br_ntfs_0x54), original.end(),
                       patched.begin() + 0x54 + sizeof(br_ntfs_0x54)),
            "Bytes outside the bootstrap templates remain exact");
    std::array<unsigned char, 512> backup{};
    verify.seekg(static_cast<std::streamoff>(capacity - 512U));
    verify.read(reinterpret_cast<char*>(backup.data()), backup.size());
    require(verify && std::equal(backup.begin(), backup.end(), patched.begin()), "Patched backup matches primary");
    auto tree = NtfsFileTree::open(path, error);
    require(tree && tree->reopenReadOnly(error) && tree->close(error), "Patched NTFS remains readable: " + error);
  }
}

void testBiosPlanning(const TemporaryDirectory& temporary) {
  using namespace rufus::core;
  const auto source = temporary.path / "bios-planning.iso";
  rufus::tests::writeIsoFixture(source);
  auto image = *ImageAnalyzer{}.analyze(source).image;
  BlockDeviceInfo target;
  target.stableId = "unit:no-device"; target.devicePath = "/no-device";
  target.capacityBytes = 128ULL * 1024ULL * 1024ULL; target.logicalSectorSize = 512;
  target.bus = DeviceBus::Usb; target.removable = target.writable = target.wholeDevice = true;
  IsoDeploymentOptions options;
  options.ntfsAvailable = true; options.fileSystem = IsoFilesystemPreference::Ntfs;
  options.partitionScheme = PartitionScheme::Mbr; options.targetSystem = IsoTargetSystem::BiosAndUefi;
  const auto build = [&] { return IsoDeploymentPlanner(createSystemWimSplitter(), options).build(image, target, "TEST"); };
  require(build().succeeded(), "Windows NTFS MBR dual boot is supported");
  options.targetSystem = IsoTargetSystem::Bios;
  require(build().succeeded(), "Windows NTFS MBR BIOS boot is supported");
  image.architecture = ImageArchitecture::X86;
  require(build().succeeded(), "32-bit Windows can use the BIOS bootstrap");
  image.architecture = ImageArchitecture::Arm64;
  require(!build().succeeded(), "ARM Windows cannot use the x86 BIOS bootstrap");
  image.architecture = ImageArchitecture::X64; image.family = ImageFamily::LinuxLive;
  require(!build().succeeded(), "GRUB NTFS BIOS boot is not advertised");
  image.family = ImageFamily::WindowsInstaller;
  options.partitionScheme = PartitionScheme::Gpt;
  require(!build().succeeded(), "GPT BIOS rejected");
  const auto invalidLayout = temporary.path / "invalid-gpt-bios.img";
  require(!rufus::backend::createNtfsIsoDiskImage(invalidLayout, target.capacityBytes,
      PartitionScheme::Gpt, IsoTargetSystem::Bios).success && !std::filesystem::exists(invalidLayout),
      "Layout API rejects GPT BIOS too");
  {
    std::ofstream existing(invalidLayout); existing << "preserve me"; existing.close();
    require(!rufus::backend::createNtfsIsoDiskImage(invalidLayout, target.capacityBytes,
        PartitionScheme::Gpt, IsoTargetSystem::Bios).success &&
        std::filesystem::file_size(invalidLayout) == 11U, "Rejected layout preserves existing files");
  }
  options.partitionScheme = PartitionScheme::Mbr;
  // Rename the ISO's root directory entry, leaving cached image capabilities
  // unchanged, to verify the planner checks the actual source file tree.
  {
    std::fstream file(source, std::ios::binary | std::ios::in | std::ios::out);
    const std::string oldName = "BOOTMGR;1";
    std::vector<char> bytes(image.sizeBytes);
    file.read(bytes.data(), bytes.size());
    const auto it = std::search(bytes.begin(), bytes.end(), oldName.begin(), oldName.end());
    require(it != bytes.end(), "Fixture BOOTMGR entry located");
    *it = 'X'; file.seekp(0); file.write(bytes.data(), bytes.size());
  }
  require(!build().succeeded(), "BIOS plan refuses missing root BOOTMGR despite cached capability flags");
}

void testLarge(const TemporaryDirectory& temporary) {
  const auto path = temporary.path / "large.ntfs";
  constexpr std::uint64_t size = (1ULL << 32U) + 12345U;
  format(path, 5ULL * 1024ULL * 1024ULL * 1024ULL);
  std::string error;
  auto tree = NtfsFileTree::open(path, error);
  require(tree != nullptr && tree->createFile("sources/install.wim", error), error);
  std::vector<unsigned char> expected(1024U * 1024U), observed(expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<unsigned char>(i * 31U + 7U);
  for (std::uint64_t offset = 0; offset < size;) {
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(expected.size(), size - offset));
    require(tree->write(expected.data(), amount, error), error);
    offset += amount;
  }
  require(tree->closeFile(error) && tree->reopenReadOnly(error) &&
          tree->openFileForRead("sources/install.wim", size, error), error);
  for (std::uint64_t offset = 0; offset < size;) {
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(expected.size(), size - offset));
    require(tree->read(observed.data(), amount, error), error);
    require(std::equal(expected.begin(), expected.begin() + amount, observed.begin()), "Readback beyond 4 GiB");
    offset += amount;
  }
  require(tree->close(error), error);
}
}  // namespace
int main(const int argc, char** argv) {
  try {
    TemporaryDirectory temporary;
    if (argc == 2 && std::string(argv[1]) == "--large") {
      testLarge(temporary);
    } else if (argc == 4 && (std::string(argv[1]) == "--stage-iso" ||
                            std::string(argv[1]) == "--stage-bios-iso" ||
                            std::string(argv[1]) == "--stage-dual-iso")) {
      const bool uefi = std::string(argv[1]) == "--stage-iso";
      const auto firmware = uefi ? rufus::core::IsoTargetSystem::Uefi :
          std::string(argv[1]) == "--stage-bios-iso" ? rufus::core::IsoTargetSystem::Bios :
          rufus::core::IsoTargetSystem::BiosAndUefi;
      const auto plan = planFor(argv[2], uefi ? rufus::core::PartitionScheme::Gpt :
          rufus::core::PartitionScheme::Mbr, 15472047104ULL, true, firmware);
      const auto begin = std::chrono::steady_clock::now();
      auto last = begin;
      const auto result = rufus::backend::makePlatformNtfsIsoImageStager()->stage(plan, argv[3], [&](const auto& p) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(10) || p.stage == rufus::core::IsoDeploymentStage::Complete) {
          std::cout << rufus::core::isoDeploymentStageName(p.stage) << ": " << p.bytesProcessed << '/' << p.totalBytes
                    << ' ' << p.currentPath << std::endl;
          last = now;
        }
      });
      require(result.success, result.error);
      std::uint64_t written = 0;
      for (const auto& range : result.writeRanges) written += range.sizeBytes;
      std::cout << "Files=" << result.filesExtracted << " payloadBytes=" << result.bytesExtracted
                << " writeBytes=" << written << " diskBytes=" << plan.target().capacityBytes << std::endl;
    } else {
      testFiles(temporary);
      testFailures(temporary);
      testStage(temporary);
      testBootstrapGuards(temporary);
      testBiosPlanning(temporary);
    }
    std::cout << "Direct NTFS checks passed; no device opened or mounted.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "NTFS check failed: " << error.what() << '\n';
    return 1;
  }
}
