/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ntfs_bootstrap.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace rufus::backend::detail {
namespace {
// Unmodified GPL ms-sys tables; provenance and source accompany the bundle.
#include "../../../third_party/windows-ntfs-boot/br_ntfs_0x0.h"
#include "../../../third_party/windows-ntfs-boot/br_ntfs_0x54.h"
static_assert(sizeof(br_ntfs_0x0) == 11);
static_assert(0x54 + sizeof(br_ntfs_0x54) <= 8192);
std::uint64_t le(const unsigned char* p, const unsigned n) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(p[i]) << (8U * i);
  return v;
}
}

bool installWindowsNtfsBootstrap(const std::filesystem::path& path,
                                const std::uint64_t partitionOffset,
                                const std::uint64_t partitionBytes,
                                std::string& error) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::is_regular_file(status) ||
      std::filesystem::hard_link_count(path, ec) != 1U || ec) {
    error = "The Windows NTFS bootstrap requires a private ordinary staging file";
    return false;
  }
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec || partitionOffset % 512U != 0 || partitionBytes % 512U != 0 ||
      partitionBytes < 32ULL * 1024ULL * 1024ULL || partitionOffset > bytes ||
      partitionBytes > bytes - partitionOffset ||
      bytes > std::uint64_t(std::numeric_limits<std::streamoff>::max())) {
    error = "Invalid Windows NTFS bootstrap partition extent";
    return false;
  }
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  std::array<unsigned char, 8192> boot{};
  std::array<unsigned char, 512> backup{};
  file.seekg(static_cast<std::streamoff>(partitionOffset));
  file.read(reinterpret_cast<char*>(boot.data()), boot.size());
  file.seekg(static_cast<std::streamoff>(partitionOffset + partitionBytes - 512U));
  file.read(reinterpret_cast<char*>(backup.data()), backup.size());
  const auto clusterSectors = boot[13];
  // NTFS's sector count excludes the final backup sector. Geometry must match
  // the actual partition; accepting an unrelated BPB would produce unsafe code.
  if (!file || std::string(reinterpret_cast<char*>(boot.data() + 3), 8) != "NTFS    " ||
      le(boot.data() + 11, 2) != 512U || clusterSectors == 0 ||
      clusterSectors > 128U || (clusterSectors & (clusterSectors - 1U)) != 0 ||
      le(boot.data() + 28, 4) != 2048U || boot[36] != 0x80U ||
      le(boot.data() + 24, 2) == 0 || le(boot.data() + 26, 2) == 0 ||
      le(boot.data() + 40, 8) != partitionBytes / 512U - 1U ||
      boot[510] != 0x55U || boot[511] != 0xaaU ||
      !std::equal(backup.begin(), backup.end(), boot.begin())) {
    error = "The NTFS boot sector, backup or partition geometry is inconsistent";
    return false;
  }
  // Keep BPB/MFT locations, serial number and checksum, and all bytes outside
  // the upstream code tables. The filesystem has already been closed cleanly.
  std::copy(std::begin(br_ntfs_0x0), std::end(br_ntfs_0x0), boot.begin());
  std::copy(std::begin(br_ntfs_0x54), std::end(br_ntfs_0x54), boot.begin() + 0x54);
  file.seekp(static_cast<std::streamoff>(partitionOffset));
  file.write(reinterpret_cast<const char*>(boot.data()), boot.size());
  file.seekp(static_cast<std::streamoff>(partitionOffset + partitionBytes - 512U));
  file.write(reinterpret_cast<const char*>(boot.data()), backup.size());
  file.flush();
  file.close();
  if (!file) {
    error = "Unable to flush the Windows NTFS bootstrap";
    return false;
  }
  std::ifstream verify(path, std::ios::binary);
  std::array<unsigned char, 8192> observed{};
  verify.seekg(static_cast<std::streamoff>(partitionOffset));
  verify.read(reinterpret_cast<char*>(observed.data()), observed.size());
  verify.seekg(static_cast<std::streamoff>(partitionOffset + partitionBytes - 512U));
  verify.read(reinterpret_cast<char*>(backup.data()), backup.size());
  if (!verify || boot != observed || !std::equal(backup.begin(), backup.end(), boot.begin())) {
    error = "Windows NTFS bootstrap readback failed";
    return false;
  }
  return true;
}
}
