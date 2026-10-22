/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <memory>
#include <filesystem>
#include <vector>
#include "rufus/core/iso_deployment.hpp"

namespace rufus::backend::detail {

// Operates only on a newly formatted ordinary file in a private staging directory.
// This class cannot open physical block/character devices or symbolic links.
class NtfsFileTree final : public core::IsoDeploymentFileTree {
 public:
  static std::unique_ptr<NtfsFileTree> open(const std::filesystem::path& path,
                                          std::string& error);
  ~NtfsFileTree() override;
  bool createDirectories(const std::string& path, std::string& error) override;
  bool createFile(const std::string& path, std::string& error) override;
  bool write(const unsigned char* data, std::size_t size, std::string& error) override;
  bool closeFile(std::string& error) override;
  bool reopenReadOnly(std::string& error) override;
  bool openFileForRead(const std::string& path, std::uint64_t expectedSize,
                       std::string& error) override;
  bool read(unsigned char* data, std::size_t size, std::string& error) override;
  bool allocatedRanges(std::vector<core::RawWriteRange>& ranges,
                       std::uint64_t partitionBytes, std::string& error) const;
  bool close(std::string& error);

 private:
  struct Impl;
  explicit NtfsFileTree(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Only the adjacent, application-bundled formatter is accepted (never PATH).
std::filesystem::path bundledNtfsFormatter();

}  // namespace rufus::backend::detail
