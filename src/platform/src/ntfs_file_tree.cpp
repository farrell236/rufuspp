/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ntfs_file_tree.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

// Use the exact ABI configuration that built the bundled C library.
#define HAVE_CONFIG_H 1
extern "C" {
#include <rufuspp_bundled_ntfs/volume.h>
#include <rufuspp_bundled_ntfs/dir.h>
#include <rufuspp_bundled_ntfs/unistr.h>
#include <rufuspp_bundled_ntfs/endians.h>
}
#undef min
#undef max

namespace rufus::backend::detail {
namespace {
std::mutex ntfsMutex;
bool failure(std::string& error, const std::string& action) {
  error = action + ": " + std::strerror(errno == 0 ? EIO : errno);
  return false;
}
bool safePath(const std::string& path) {
  if (path.empty()) return true;
  if (path.front() == '/' || path.find('\0') != std::string::npos ||
      path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
  for (const auto& part : std::filesystem::u8path(path)) {
    if (part == "." || part == ".." || part.empty()) return false;
  }
  return true;
}
ntfs_inode* createChild(ntfs_inode* parent, const std::string& name, const mode_t type) {
  ntfschar* unicode = nullptr;
  const int length = ntfs_mbstoucs(name.c_str(), &unicode);
  if (length <= 0 || length > 255) {
    std::free(unicode);
    errno = EINVAL;
    return nullptr;
  }
  const auto existing = ntfs_inode_lookup_by_name(parent, unicode, length);
  if (existing != static_cast<u64>(-1) || errno != ENOENT) {
    const int saved = existing != static_cast<u64>(-1) ? EEXIST : errno;
    std::free(unicode);
    errno = saved;
    return nullptr;
  }
  auto* inode = ntfs_create(parent, const_cpu_to_le32(0), unicode,
                            static_cast<u8>(length), type);
  std::free(unicode);
  return inode;
}
}  // namespace

struct NtfsFileTree::Impl {
  // libntfs-3g has process-global encoding/cache settings. Serialize sessions.
  std::unique_lock<std::mutex> lock{ntfsMutex};
  std::filesystem::path path;
  ntfs_volume* volume{};
  ntfs_inode* inode{};
  ntfs_attr* attribute{};
  std::uint64_t offset{};
  bool readOnly{};
  std::map<std::uint64_t, std::string> directoryPaths;
};

NtfsFileTree::NtfsFileTree(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NtfsFileTree::~NtfsFileTree() {
  std::string ignored;
  static_cast<void>(close(ignored));
}

std::unique_ptr<NtfsFileTree> NtfsFileTree::open(const std::filesystem::path& path,
                                               std::string& error) {
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
    error = "Direct NTFS staging requires an ordinary file without links";
    return {};
  }
  auto impl = std::make_unique<Impl>();
  impl->path = path;
  ntfs_set_char_encoding("UTF-8");
  impl->volume = ntfs_mount(path.c_str(), NTFS_MNT_EXCLUSIVE);
  if (!impl->volume) {
    failure(error, "Opening the NTFS staging filesystem");
    return {};
  }
  if (ntfs_set_ignore_case(impl->volume) != 0) {
    failure(error, "Enabling Windows-compatible NTFS name lookup");
    ntfs_umount(impl->volume, FALSE);
    return {};
  }
  return std::unique_ptr<NtfsFileTree>(new NtfsFileTree(std::move(impl)));
}

bool NtfsFileTree::createDirectories(const std::string& path, std::string& error) {
  if (!impl_->volume || impl_->readOnly || !safePath(path)) {
    error = "Invalid path or NTFS session is not writable";
    return false;
  }
  std::string current;
  for (const auto& component : std::filesystem::u8path(path)) {
    const std::string name = component.u8string();
    const std::string parentPath = current.empty() ? "/" : current;
    if (!current.empty()) current += '/';
    current += name;
    auto* directory = ntfs_pathname_to_inode(impl_->volume, nullptr, current.c_str());
    if (!directory) {
      if (errno != ENOENT) return failure(error, "Looking up NTFS directory " + current);
      auto* parent = ntfs_pathname_to_inode(impl_->volume, nullptr, parentPath.c_str());
      if (!parent) return failure(error, "Opening NTFS parent " + parentPath);
      directory = createChild(parent, name, S_IFDIR);
      const int saved = errno;
      const int closed = ntfs_inode_close(parent);
      if (!directory) {
        errno = saved;
        return failure(error, "Creating NTFS directory " + current);
      }
      if (closed != 0) {
        ntfs_inode_close(directory);
        return failure(error, "Closing NTFS parent directory");
      }
    }
    const bool isDirectory = (directory->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
    const auto number = directory->mft_no;
    const int closed = ntfs_inode_close(directory);
    if (!isDirectory || closed != 0) {
      error = "NTFS directory collides with a file, or failed to close: " + current;
      return false;
    }
    const auto previous = impl_->directoryPaths.emplace(number, current);
    if (!previous.second && previous.first->second != current) {
      error = "Case-colliding NTFS directory paths: " + current;
      return false;
    }
  }
  return true;
}

bool NtfsFileTree::createFile(const std::string& path, std::string& error) {
  if (path.empty() || !safePath(path) || !closeFile(error)) {
    if (error.empty()) error = "Invalid NTFS file path";
    return false;
  }
  const auto relative = std::filesystem::u8path(path);
  if (!createDirectories(relative.parent_path().generic_u8string(), error)) return false;
  const auto parentPath = relative.parent_path().empty() ? "/" : relative.parent_path().generic_u8string();
  auto* parent = ntfs_pathname_to_inode(impl_->volume, nullptr, parentPath.c_str());
  if (!parent) return failure(error, "Opening NTFS parent directory");
  impl_->inode = createChild(parent, relative.filename().u8string(), S_IFREG);
  const int saved = errno;
  const int closed = ntfs_inode_close(parent);
  if (!impl_->inode) {
    errno = saved;
    return failure(error, "Creating NTFS file " + path);
  }
  if (closed != 0) return failure(error, "Closing NTFS parent directory");
  impl_->attribute = ntfs_attr_open(impl_->inode, AT_DATA, AT_UNNAMED, 0);
  impl_->offset = 0;
  return impl_->attribute != nullptr || failure(error, "Opening NTFS file data");
}

bool NtfsFileTree::write(const unsigned char* data, const std::size_t size, std::string& error) {
  if (!impl_->attribute || impl_->readOnly ||
      size > static_cast<std::uint64_t>(std::numeric_limits<s64>::max()) - impl_->offset) {
    error = "Invalid NTFS write or file size overflow";
    return false;
  }
  std::size_t completed = 0;
  while (completed < size) {
    const auto amount = ntfs_attr_pwrite(impl_->attribute, static_cast<s64>(impl_->offset),
                                       static_cast<s64>(size - completed), data + completed);
    if (amount <= 0) return failure(error, "Writing NTFS file data");
    completed += static_cast<std::size_t>(amount);
    impl_->offset += static_cast<std::uint64_t>(amount);
  }
  return true;
}

bool NtfsFileTree::closeFile(std::string& error) {
  bool ok = true;
  if (impl_->attribute) {
    if (!impl_->readOnly && ntfs_attr_pclose(impl_->attribute) != 0) {
      ok = failure(error, "Flushing NTFS file data");
    }
    ntfs_attr_close(impl_->attribute);
    impl_->attribute = nullptr;
  }
  if (impl_->inode) {
    const int result = ntfs_inode_close(impl_->inode);
    impl_->inode = nullptr;
    if (result != 0) ok = failure(error, "Closing NTFS file metadata");
  }
  return ok;
}

bool NtfsFileTree::close(std::string& error) {
  bool ok = closeFile(error);
  if (impl_->volume) {
    const int result = ntfs_umount(impl_->volume, FALSE);
    impl_->volume = nullptr;
    if (result != 0) ok = failure(error, "Flushing and closing the NTFS filesystem");
  }
  return ok;
}

bool NtfsFileTree::reopenReadOnly(std::string& error) {
  if (!close(error)) return false;
  impl_->volume = ntfs_mount(impl_->path.c_str(), NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
  impl_->readOnly = true;
  if (!impl_->volume) return failure(error, "Reopening the NTFS filesystem read-only");
  if (ntfs_set_ignore_case(impl_->volume) != 0) return failure(error, "Enabling NTFS name lookup");
  if ((impl_->volume->flags & VOLUME_IS_DIRTY) != 0) {
    error = "The staged NTFS filesystem is marked dirty after closing";
    return false;
  }
  return true;
}

bool NtfsFileTree::openFileForRead(const std::string& path, const std::uint64_t expectedSize,
                                  std::string& error) {
  if (!impl_->volume || !impl_->readOnly || path.empty() || !safePath(path) || !closeFile(error)) {
    error = "Invalid NTFS read-only session or path";
    return false;
  }
  impl_->inode = ntfs_pathname_to_inode(impl_->volume, nullptr, path.c_str());
  if (!impl_->inode) return failure(error, "Reopening NTFS file " + path);
  impl_->attribute = ntfs_attr_open(impl_->inode, AT_DATA, AT_UNNAMED, 0);
  if (!impl_->attribute) return failure(error, "Reopening NTFS file data");
  if (impl_->attribute->data_size < 0 ||
      static_cast<std::uint64_t>(impl_->attribute->data_size) != expectedSize) {
    error = "NTFS file has an unexpected size: " + path;
    return false;
  }
  impl_->offset = 0;
  return true;
}

bool NtfsFileTree::read(unsigned char* data, const std::size_t size, std::string& error) {
  if (!impl_->attribute || !impl_->readOnly ||
      size > static_cast<std::uint64_t>(std::numeric_limits<s64>::max()) - impl_->offset) {
    error = "Invalid NTFS read or offset overflow";
    return false;
  }
  std::size_t completed = 0;
  while (completed < size) {
    const auto amount = ntfs_attr_pread(impl_->attribute, static_cast<s64>(impl_->offset),
                                      static_cast<s64>(size - completed), data + completed);
    if (amount <= 0) return failure(error, "Reading NTFS file data");
    completed += static_cast<std::size_t>(amount);
    impl_->offset += static_cast<std::uint64_t>(amount);
  }
  return true;
}

bool NtfsFileTree::allocatedRanges(std::vector<core::RawWriteRange>& ranges,
                                  const std::uint64_t partitionBytes, std::string& error) const {
  const auto* volume = impl_->volume;
  if (!volume || !impl_->readOnly || volume->nr_clusters <= 0 ||
      volume->cluster_size < 512U || volume->cluster_size > 65536U ||
      static_cast<std::uint64_t>(volume->nr_clusters) > partitionBytes / volume->cluster_size) {
    error = "Invalid NTFS allocation bitmap geometry";
    return false;
  }
  const auto clusters = static_cast<std::uint64_t>(volume->nr_clusters);
  std::array<unsigned char, 8192> bitmap{};
  ranges.clear();
  const auto append = [&](const std::uint64_t offset, const std::uint64_t size) {
    if (!ranges.empty() && ranges.back().offsetBytes + ranges.back().sizeBytes == offset) {
      ranges.back().sizeBytes += size;
    } else {
      ranges.push_back({offset, size});
    }
  };
  for (std::uint64_t base = 0; base < clusters; base += bitmap.size() * 8ULL) {
    const auto bits = std::min<std::uint64_t>(bitmap.size() * 8ULL, clusters - base);
    const auto bytes = (bits + 7U) / 8U;
    if (ntfs_attr_pread(volume->lcnbmp_na, static_cast<s64>(base / 8U),
                        static_cast<s64>(bytes), bitmap.data()) != static_cast<s64>(bytes)) {
      return failure(error, "Reading the NTFS allocation bitmap");
    }
    for (std::uint64_t bit = 0; bit < bits; ++bit) {
      if ((bitmap[bit / 8U] & (1U << (bit % 8U))) != 0) {
        append((base + bit) * volume->cluster_size, volume->cluster_size);
      }
    }
    if (ranges.size() > core::kMaximumRawWriteRanges - 4U) {
      error = "NTFS allocation is too fragmented for a bounded write map";
      return false;
    }
  }
  // mkntfs writes a backup boot sector at the last sector, outside cluster data.
  ranges.push_back({partitionBytes - 512U, 512U});
  return true;
}

}  // namespace rufus::backend::detail
