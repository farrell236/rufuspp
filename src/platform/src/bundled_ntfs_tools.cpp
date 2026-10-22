/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ntfs_file_tree.hpp"
#include <array>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
namespace rufus::backend::detail {
std::filesystem::path bundledNtfsFormatter() {
  std::filesystem::path executable;
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
  executable = buffer.data();
#else
  std::array<char, 4096> buffer{};
  const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (size <= 0 || static_cast<std::size_t>(size) == buffer.size()) return {};
  executable = std::string(buffer.data(), static_cast<std::size_t>(size));
#endif
  std::error_code ec;
  executable = std::filesystem::canonical(executable, ec);
  if (ec) return {};
  const auto formatter = executable.parent_path() / "rufus-mkntfs";
  struct stat status{};
  return lstat(formatter.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
                 access(formatter.c_str(), X_OK) == 0 ? formatter : std::filesystem::path{};
}
}  // namespace rufus::backend::detail
