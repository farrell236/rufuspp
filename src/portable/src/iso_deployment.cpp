/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rufus/core/iso_deployment.hpp"
#include "rufus/core/standalone_media.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ext2_formatter.hpp"
#include "grub2_bootstrap_data.hpp"
#include "iso9660_reader.hpp"
#include "linux_persistence_support.hpp"
#include "udf_reader.hpp"
#include "windows_boot_code.hpp"

namespace rufus::core {

namespace detail {

struct IsoDeploymentContent final {
  std::vector<ImageContentEntry> entries;
  std::vector<ImageFileRecord> files;
};

}  // namespace detail

namespace {

constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t kGibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kFat32MinimumClusters = 65525U;
constexpr std::uint32_t kFat32MaximumCluster = 0x0ffffff4U;
constexpr std::uint32_t kFat32EndOfChain = 0x0fffffffU;
constexpr std::size_t kTransferBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumDepth = 64;
constexpr std::size_t kWimHeaderProbeBytes = 48;
constexpr std::uint32_t kGptEntryCount = 128U;
constexpr std::uint32_t kGptEntrySize = 128U;

#include "boot_data/freedos_fat32_0x52.inc"
#include "boot_data/freedos_fat32_0x3f0.inc"
#include "boot_data/msdos_fat32_0x52.inc"
#include "boot_data/msdos_fat32_0x3f0.inc"
#include "boot_data/grub4dos_mbr.inc"
#include "boot_data/reactos_mbr.inc"
#include "boot_data/reactos_fat32_0x52.inc"
#include "boot_data/reactos_fat32_0x3f0.inc"
#include "boot_data/reactos_fat32_0x1c00.inc"
#include "boot_data/syslinux4_ldlinux_sys.inc"
#include "boot_data/syslinux4_ldlinux_bss.inc"

void appendIssues(std::vector<SafetyIssue>& destination, SafetyCheckResult source) {
  destination.insert(destination.end(), std::make_move_iterator(source.issues.begin()),
                     std::make_move_iterator(source.issues.end()));
}

void put16(unsigned char* output, const std::uint16_t value) {
  output[0] = static_cast<unsigned char>(value & 0xffU);
  output[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put32(unsigned char* output, const std::uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index) {
    output[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

void put64(unsigned char* output, const std::uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index) {
    output[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint32_t crc32(const unsigned char* data, const std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned int bit = 0; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

using GuidBytes = std::array<unsigned char, 16>;

GuidBytes randomGuid() {
  std::random_device random;
  GuidBytes value{};
  for (unsigned char& byte : value) {
    byte = static_cast<unsigned char>(random());
  }
  value[7] = static_cast<unsigned char>((value[7] & 0x0fU) | 0x40U);
  value[8] = static_cast<unsigned char>((value[8] & 0x3fU) | 0x80U);
  return value;
}

class Md5Stream final {
 public:
  void update(const unsigned char* data, std::size_t size) {
    totalBytes_ += size;
    while (size != 0U) {
      const std::size_t count = std::min<std::size_t>(size, 64U - used_);
      std::copy_n(data, count, block_.begin() + static_cast<std::ptrdiff_t>(used_));
      data += count;
      size -= count;
      used_ += count;
      if (used_ == block_.size()) {
        transform(block_.data());
        used_ = 0U;
      }
    }
  }

  std::string finish() {
    const std::uint64_t bitLength = totalBytes_ * 8U;
    const unsigned char marker = 0x80U;
    update(&marker, 1U);
    const unsigned char zero = 0U;
    while (used_ != 56U) {
      update(&zero, 1U);
    }
    std::array<unsigned char, 8> length{};
    for (unsigned int index = 0; index < length.size(); ++index) {
      length[index] = static_cast<unsigned char>(bitLength >> (index * 8U));
    }
    update(length.data(), length.size());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : state_) {
      for (unsigned int index = 0; index < 4U; ++index) {
        output << std::setw(2) << ((value >> (index * 8U)) & 0xffU);
      }
    }
    return output.str();
  }

 private:
  static std::uint32_t rotate(const std::uint32_t value,
                              const unsigned int count) {
    return (value << count) | (value >> (32U - count));
  }

  void transform(const unsigned char* input) {
    static constexpr std::array<unsigned int, 64> shifts{
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
        0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
        0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
        0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
        0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
        0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
        0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
        0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
        0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
        0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
        0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
        0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
        0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
    std::array<std::uint32_t, 16> words{};
    for (unsigned int word = 0; word < words.size(); ++word) {
      for (unsigned int byte = 0; byte < 4U; ++byte) {
        words[word] |= static_cast<std::uint32_t>(input[word * 4U + byte])
                       << (byte * 8U);
      }
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    for (unsigned int index = 0; index < 64U; ++index) {
      std::uint32_t function = 0U;
      unsigned int word = 0U;
      if (index < 16U) {
        function = (b & c) | (~b & d);
        word = index;
      } else if (index < 32U) {
        function = (d & b) | (~d & c);
        word = (5U * index + 1U) % 16U;
      } else if (index < 48U) {
        function = b ^ c ^ d;
        word = (3U * index + 5U) % 16U;
      } else {
        function = c ^ (b | ~d);
        word = (7U * index) % 16U;
      }
      const std::uint32_t previous = d;
      d = c;
      c = b;
      b += rotate(a + function + constants[index] + words[word],
                  shifts[index]);
      a = previous;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }

  std::array<std::uint32_t, 4> state_{
      0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
  std::array<unsigned char, 64> block_{};
  std::uint64_t totalBytes_{};
  std::size_t used_{};
};

bool checkedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t& output) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  output = left + right;
  return true;
}

std::uint16_t get16(const unsigned char* input) {
  return static_cast<std::uint16_t>(input[0]) |
         static_cast<std::uint16_t>(input[1]) << 8U;
}

std::uint32_t get32(const unsigned char* input) {
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

std::string asciiFold(std::string value) {
  for (char& character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 'A' && byte <= 'Z') {
      character = static_cast<char>(byte - 'A' + 'a');
    }
  }
  return value;
}

bool isWindowsInstallImagePath(const std::string& path) {
  const std::string folded = asciiFold(path);
  return folded == "sources/install.wim" || folded == "sources/install.esd";
}

bool prepareGrubBiosCompatibilityTree(detail::IsoDeploymentContent& content,
                                      std::string& aliasedPrefix) {
  const auto hasFile = [&content](const std::string_view expected) {
    return std::any_of(content.files.begin(), content.files.end(),
                       [expected](const detail::ImageFileRecord& file) {
                         return asciiFold(file.path) == expected;
                       });
  };
  if (hasFile("boot/grub/grub.cfg") &&
      hasFile("boot/grub/i386-pc/normal.mod")) {
    return true;
  }
  std::string sourcePrefix;
  for (const std::string_view candidate : {std::string_view("boot/grub2"),
                                           std::string_view("grub")}) {
    if (hasFile(std::string(candidate) + "/grub.cfg") &&
        hasFile(std::string(candidate) + "/i386-pc/normal.mod")) {
      sourcePrefix = std::string(candidate);
      break;
    }
  }
  if (sourcePrefix.empty()) {
    return false;
  }
  const auto targetConflicts = [&content] {
    return std::any_of(
               content.entries.begin(), content.entries.end(),
               [](const ImageContentEntry& entry) {
                 const std::string path = asciiFold(entry.path);
                 return path == "boot/grub" || path.rfind("boot/grub/", 0) == 0;
               }) ||
           std::any_of(content.files.begin(), content.files.end(),
                       [](const detail::ImageFileRecord& file) {
                         const std::string path = asciiFold(file.path);
                         return path == "boot/grub" ||
                                path.rfind("boot/grub/", 0) == 0;
                       });
  };
  if (targetConflicts()) {
    return false;
  }
  const auto remap = [&sourcePrefix](const std::string& path) {
    const std::string folded = asciiFold(path);
    return std::string("boot/grub") + path.substr(sourcePrefix.size(),
                                                   path.size() - sourcePrefix.size());
  };
  const auto originalEntries = content.entries;
  for (const auto& entry : originalEntries) {
    const std::string folded = asciiFold(entry.path);
    if (folded == sourcePrefix ||
        folded.rfind(sourcePrefix + '/', 0) == 0) {
      auto alias = entry;
      alias.path = remap(entry.path);
      content.entries.push_back(std::move(alias));
    }
  }
  const auto originalFiles = content.files;
  for (const auto& file : originalFiles) {
    const std::string folded = asciiFold(file.path);
    if (folded == sourcePrefix ||
        folded.rfind(sourcePrefix + '/', 0) == 0) {
      auto alias = file;
      alias.path = remap(file.path);
      content.files.push_back(std::move(alias));
    }
  }
  if (sourcePrefix == "grub") {
    const bool hasBootDirectory = std::any_of(
        content.entries.begin(), content.entries.end(),
        [](const ImageContentEntry& entry) {
          return entry.directory && asciiFold(entry.path) == "boot";
        });
    if (!hasBootDirectory) {
      content.entries.push_back({"boot", 0, true});
    }
  }
  aliasedPrefix = std::move(sourcePrefix);
  return true;
}

bool utf8ToUtf16(const std::string_view input, std::u16string& output) {
  output.clear();
  for (std::size_t index = 0; index < input.size();) {
    const auto first = static_cast<unsigned char>(input[index]);
    std::uint32_t codePoint = 0;
    std::size_t length = 0;
    if (first < 0x80U) {
      codePoint = first;
      length = 1;
    } else if ((first & 0xe0U) == 0xc0U) {
      codePoint = first & 0x1fU;
      length = 2;
    } else if ((first & 0xf0U) == 0xe0U) {
      codePoint = first & 0x0fU;
      length = 3;
    } else if ((first & 0xf8U) == 0xf0U) {
      codePoint = first & 0x07U;
      length = 4;
    } else {
      return false;
    }
    if (length > input.size() - index) {
      return false;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      const auto byte = static_cast<unsigned char>(input[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      codePoint = (codePoint << 6U) | (byte & 0x3fU);
    }
    const bool overlong = (length == 2 && codePoint < 0x80U) ||
                          (length == 3 && codePoint < 0x800U) ||
                          (length == 4 && codePoint < 0x10000U);
    if (overlong || codePoint > 0x10ffffU ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
      return false;
    }
    if (codePoint < 0x10000U) {
      output.push_back(static_cast<char16_t>(codePoint));
    } else {
      codePoint -= 0x10000U;
      output.push_back(static_cast<char16_t>(0xd800U + (codePoint >> 10U)));
      output.push_back(static_cast<char16_t>(0xdc00U + (codePoint & 0x3ffU)));
    }
    index += length;
  }
  return true;
}

bool isReservedDosName(const std::string& name) {
  std::string base = name.substr(0, name.find('.'));
  for (char& character : base) {
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  }
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
    return true;
  }
  return base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) &&
         base[3] >= '1' && base[3] <= '9';
}

bool validateComponent(const std::string& component, std::u16string& utf16,
                       std::string& error) {
  if (component.empty() || component == "." || component == "..") {
    error = "Optical image contains an empty or relative path component";
    return false;
  }
  if (!utf8ToUtf16(component, utf16) || utf16.empty() || utf16.size() > 255) {
    error = "Optical image contains a filename that is not valid FAT32 Unicode";
    return false;
  }
  if (component.back() == ' ' || component.back() == '.') {
    error = "Optical image contains a filename ending in a space or period: " + component;
    return false;
  }
  if (isReservedDosName(component)) {
    error = "Optical image contains a DOS-reserved filename: " + component;
    return false;
  }
  constexpr std::string_view forbidden = "\"*/:<>?\\|";
  for (const char16_t character : utf16) {
    if (character < 0x20U || character == 0x7fU ||
        (character < 0x80U && forbidden.find(static_cast<char>(character)) !=
                                  std::string_view::npos)) {
      error = "Optical image contains a filename that FAT32 cannot represent: " + component;
      return false;
    }
  }
  return true;
}

bool splitPath(const std::string& path, std::vector<std::string>& components,
               std::string& error) {
  components.clear();
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string::npos) {
    error = "Optical image contains an unsafe path: " + path;
    return false;
  }
  std::size_t begin = 0;
  while (begin < path.size()) {
    const std::size_t end = path.find('/', begin);
    components.push_back(path.substr(begin, end == std::string::npos
                                               ? std::string::npos
                                               : end - begin));
    if (components.size() > kMaximumDepth) {
      error = "Optical image path depth exceeds the FAT32 deployment limit: " + path;
      return false;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1U;
  }
  return true;
}

struct FatNode final {
  std::string name;
  std::string path;
  std::u16string longName;
  std::array<unsigned char, 11> shortName{};
  bool directory{true};
  const detail::ImageFileRecord* source{};
  FatNode* parent{};
  std::vector<std::unique_ptr<FatNode>> children;
  std::uint32_t firstCluster{};
  std::uint32_t clusterCount{};
};

FatNode* findChild(FatNode& parent, const std::string& name) {
  const std::string key = asciiFold(name);
  for (const auto& child : parent.children) {
    if (asciiFold(child->name) == key) {
      return child.get();
    }
  }
  return nullptr;
}

FatNode* addDirectoryPath(FatNode& root, const std::vector<std::string>& components,
                          const std::size_t count, std::string& error) {
  FatNode* current = &root;
  std::string path;
  for (std::size_t index = 0; index < count; ++index) {
    std::u16string utf16;
    if (!validateComponent(components[index], utf16, error)) {
      return nullptr;
    }
    path = path.empty() ? components[index] : path + '/' + components[index];
    FatNode* child = findChild(*current, components[index]);
    if (child == nullptr) {
      auto created = std::make_unique<FatNode>();
      created->name = components[index];
      created->path = path;
      created->longName = std::move(utf16);
      created->parent = current;
      child = created.get();
      current->children.push_back(std::move(created));
    } else if (!child->directory) {
      error = "Optical image path is both a file and directory: " + path;
      return nullptr;
    }
    current = child;
  }
  return current;
}

bool buildTree(const detail::IsoDeploymentContent& content, FatNode& root,
               std::uint64_t& totalFileBytes, std::string& error) {
  std::vector<std::string> components;
  for (const auto& entry : content.entries) {
    if (!entry.directory) {
      continue;
    }
    if (!splitPath(entry.path, components, error) ||
        addDirectoryPath(root, components, components.size(), error) == nullptr) {
      return false;
    }
  }

  totalFileBytes = 0;
  for (const auto& file : content.files) {
    if (!splitPath(file.path, components, error) || components.empty()) {
      return false;
    }
    FatNode* parent = addDirectoryPath(root, components, components.size() - 1U, error);
    if (parent == nullptr) {
      return false;
    }
    std::u16string utf16;
    if (!validateComponent(components.back(), utf16, error)) {
      return false;
    }
    if (findChild(*parent, components.back()) != nullptr) {
      error = "Optical image contains duplicate or case-colliding paths: " + file.path;
      return false;
    }
    if (file.sizeBytes > std::numeric_limits<std::uint32_t>::max()) {
      error = "A source file is too large for FAT32: " + file.path;
      return false;
    }
    if (!checkedAdd(totalFileBytes, file.sizeBytes, totalFileBytes)) {
      error = "Optical image file sizes overflow the deployment size";
      return false;
    }
    auto node = std::make_unique<FatNode>();
    node->name = components.back();
    node->path = file.path;
    node->longName = std::move(utf16);
    node->directory = false;
    node->source = &file;
    node->parent = parent;
    parent->children.push_back(std::move(node));
  }
  return true;
}

bool legalShortCharacter(const unsigned char character) {
  if ((character >= 'A' && character <= 'Z') ||
      (character >= '0' && character <= '9')) {
    return true;
  }
  constexpr std::string_view allowed = "$%'-_@~`!(){}^#&";
  return allowed.find(static_cast<char>(character)) != std::string_view::npos;
}

std::string shortPart(const std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const unsigned char character : input) {
    if (character < 0x80U) {
      const auto upper = static_cast<unsigned char>(std::toupper(character));
      result.push_back(legalShortCharacter(upper) ? static_cast<char>(upper) : '_');
    } else {
      result.push_back('_');
    }
  }
  return result;
}

std::array<unsigned char, 11> packShortName(const std::string& base,
                                            const std::string& extension) {
  std::array<unsigned char, 11> result{};
  result.fill(' ');
  std::copy_n(base.begin(), std::min<std::size_t>(8, base.size()), result.begin());
  std::copy_n(extension.begin(), std::min<std::size_t>(3, extension.size()), result.begin() + 8);
  if (result[0] == 0xe5U) {
    result[0] = 0x05U;
  }
  return result;
}

std::string shortKey(const std::array<unsigned char, 11>& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool assignShortNames(FatNode& directory, std::string& error) {
  std::set<std::string> used;
  for (auto& child : directory.children) {
    const std::size_t dot = child->name.find_last_of('.');
    const bool splitExtension = dot != std::string::npos && dot != 0;
    const std::string_view baseView(child->name.data(), splitExtension ? dot : child->name.size());
    const std::string_view extensionView =
        splitExtension ? std::string_view(child->name).substr(dot + 1U) : std::string_view{};
    std::string base = shortPart(baseView);
    std::string extension = shortPart(extensionView);
    if (base.empty()) {
      base = "FILE";
    }
    extension.resize(std::min<std::size_t>(3, extension.size()));

    bool direct = baseView.size() <= 8 && extensionView.size() <= 3;
    for (const unsigned char character : baseView) {
      direct = direct && character < 0x80U &&
               legalShortCharacter(static_cast<unsigned char>(std::toupper(character)));
    }
    for (const unsigned char character : extensionView) {
      direct = direct && character < 0x80U &&
               legalShortCharacter(static_cast<unsigned char>(std::toupper(character)));
    }
    auto candidate = packShortName(base, extension);
    if (!direct || used.count(shortKey(candidate)) != 0) {
      bool assigned = false;
      for (unsigned int suffix = 1; suffix < 1000000U; ++suffix) {
        const std::string tail = "~" + std::to_string(suffix);
        const std::size_t stemLength = 8U - std::min<std::size_t>(7, tail.size());
        const std::string stem = base.substr(0, std::min(base.size(), stemLength)) + tail;
        candidate = packShortName(stem, extension);
        if (used.emplace(shortKey(candidate)).second) {
          assigned = true;
          break;
        }
      }
      if (!assigned) {
        error = "Unable to create a unique FAT32 short name in: " + directory.path;
        return false;
      }
    } else {
      used.emplace(shortKey(candidate));
    }
    child->shortName = candidate;
  }
  for (auto& child : directory.children) {
    if (child->directory && !assignShortNames(*child, error)) {
      return false;
    }
  }
  return true;
}

std::size_t longNameEntryCount(const FatNode& node) {
  return std::max<std::size_t>(1, (node.longName.size() + 12U) / 13U);
}

struct FatLayout final {
  std::uint32_t sectorBytes{};
  PartitionScheme partitionScheme{PartitionScheme::Mbr};
  std::uint32_t partitionStart{};
  std::uint32_t partitionSectors{};
  std::uint32_t sectorsPerCluster{};
  std::uint32_t reservedSectors{32};
  std::uint32_t fatSectors{};
  std::uint32_t totalClusters{};
  std::uint64_t dataStartSector{};
  std::uint32_t persistenceStart{};
  std::uint32_t persistenceSectors{};

  [[nodiscard]] std::uint64_t clusterBytes() const noexcept {
    return static_cast<std::uint64_t>(sectorBytes) * sectorsPerCluster;
  }

  [[nodiscard]] std::uint64_t clusterOffset(const std::uint32_t cluster) const noexcept {
    return (dataStartSector + static_cast<std::uint64_t>(cluster - 2U) * sectorsPerCluster) *
           sectorBytes;
  }
};

bool writeAt(std::ofstream& output, std::uint64_t offset,
             const unsigned char* bytes, std::size_t size,
             std::string& error);
bool writeAt(std::ofstream& output, std::uint64_t offset,
             const std::vector<unsigned char>& bytes, std::string& error);

void putGptPartition(std::vector<unsigned char>& entries,
                     const std::size_t index, const GuidBytes& type,
                     const GuidBytes& unique, const std::uint64_t firstLba,
                     const std::uint64_t lastLba,
                     const std::u16string_view name) {
  unsigned char* const entry = entries.data() + index * kGptEntrySize;
  std::copy(type.begin(), type.end(), entry);
  std::copy(unique.begin(), unique.end(), entry + 16U);
  put64(entry + 32U, firstLba);
  put64(entry + 40U, lastLba);
  const std::size_t characters = std::min<std::size_t>(name.size(), 36U);
  for (std::size_t character = 0; character < characters; ++character) {
    put16(entry + 56U + character * 2U,
          static_cast<std::uint16_t>(name[character]));
  }
}

std::vector<unsigned char> makeGptHeader(
    const std::uint32_t sectorBytes, const std::uint64_t currentLba,
    const std::uint64_t backupLba, const std::uint64_t firstUsableLba,
    const std::uint64_t lastUsableLba, const std::uint64_t entriesLba,
    const GuidBytes& diskGuid, const std::uint32_t entriesCrc) {
  std::vector<unsigned char> header(sectorBytes, 0U);
  constexpr std::array<unsigned char, 8> signature{
      'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
  std::copy(signature.begin(), signature.end(), header.begin());
  put32(header.data() + 8U, 0x00010000U);
  put32(header.data() + 12U, 92U);
  put64(header.data() + 24U, currentLba);
  put64(header.data() + 32U, backupLba);
  put64(header.data() + 40U, firstUsableLba);
  put64(header.data() + 48U, lastUsableLba);
  std::copy(diskGuid.begin(), diskGuid.end(), header.begin() + 56U);
  put64(header.data() + 72U, entriesLba);
  put32(header.data() + 80U, kGptEntryCount);
  put32(header.data() + 84U, kGptEntrySize);
  put32(header.data() + 88U, entriesCrc);
  put32(header.data() + 16U, crc32(header.data(), 92U));
  return header;
}

bool writeGptStructures(std::ofstream& output, const FatLayout& layout,
                        std::string& error) {
  const std::uint64_t deviceSectors =
      layout.partitionScheme == PartitionScheme::Gpt
          ? (layout.persistenceSectors == 0U
                 ? static_cast<std::uint64_t>(layout.partitionStart) +
                       layout.partitionSectors +
                       (static_cast<std::uint64_t>(kGptEntryCount) *
                            kGptEntrySize +
                        layout.sectorBytes - 1U) /
                           layout.sectorBytes +
                       1U
                 : static_cast<std::uint64_t>(layout.persistenceStart) +
                       layout.persistenceSectors +
                       (static_cast<std::uint64_t>(kGptEntryCount) *
                            kGptEntrySize +
                        layout.sectorBytes - 1U) /
                           layout.sectorBytes +
                       1U)
          : 0U;
  const std::uint64_t entrySectors =
      (static_cast<std::uint64_t>(kGptEntryCount) * kGptEntrySize +
       layout.sectorBytes - 1U) /
      layout.sectorBytes;
  if (deviceSectors <= entrySectors + 2U) {
    error = "The GPT target is too small for backup partition metadata";
    return false;
  }
  const std::uint64_t backupHeaderLba = deviceSectors - 1U;
  const std::uint64_t backupEntriesLba = backupHeaderLba - entrySectors;
  const std::uint64_t firstUsableLba = 2U + entrySectors;
  const std::uint64_t lastUsableLba = backupEntriesLba - 1U;
  std::vector<unsigned char> entries(kGptEntryCount * kGptEntrySize, 0U);
  constexpr GuidBytes efiSystemType{
      0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  constexpr GuidBytes linuxFilesystemType{
      0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
      0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
  putGptPartition(entries, 0U, efiSystemType, randomGuid(),
                  layout.partitionStart,
                  static_cast<std::uint64_t>(layout.partitionStart) +
                      layout.partitionSectors - 1U,
                  u"Rufus++ ISO");
  if (layout.persistenceSectors != 0U) {
    putGptPartition(entries, 1U, linuxFilesystemType, randomGuid(),
                    layout.persistenceStart,
                    static_cast<std::uint64_t>(layout.persistenceStart) +
                        layout.persistenceSectors - 1U,
                    u"Persistence");
  }
  const std::uint32_t entriesCrc = crc32(entries.data(), entries.size());
  const GuidBytes diskGuid = randomGuid();
  const auto primaryHeader = makeGptHeader(
      layout.sectorBytes, 1U, backupHeaderLba, firstUsableLba,
      lastUsableLba, 2U, diskGuid, entriesCrc);
  const auto backupHeader = makeGptHeader(
      layout.sectorBytes, backupHeaderLba, 1U, firstUsableLba,
      lastUsableLba, backupEntriesLba, diskGuid, entriesCrc);
  return writeAt(output, layout.sectorBytes, primaryHeader, error) &&
         writeAt(output, 2ULL * layout.sectorBytes, entries, error) &&
         writeAt(output, backupEntriesLba * layout.sectorBytes, entries,
                 error) &&
         writeAt(output, backupHeaderLba * layout.sectorBytes, backupHeader,
                 error);
}

bool makeLayout(const BlockDeviceInfo& target,
                const std::uint64_t requestedPersistenceBytes,
                const PartitionScheme partitionScheme,
                const std::uint32_t requestedClusterBytes,
                FatLayout& layout, std::string& error) {
  const std::uint32_t sector = target.logicalSectorSize;
  if (sector < 512U || sector > 4096U || (sector & (sector - 1U)) != 0U) {
    error = "ISO mode requires a 512, 1024, 2048, or 4096 byte target sector";
    return false;
  }
  if (target.capacityBytes % sector != 0) {
    error = "The target capacity is not aligned to its logical sector size";
    return false;
  }
  const std::uint64_t start = (kMebibyte + sector - 1U) / sector;
  const std::uint64_t deviceSectors = target.capacityBytes / sector;
  const std::uint64_t gptEntrySectors =
      (static_cast<std::uint64_t>(kGptEntryCount) * kGptEntrySize + sector - 1U) /
      sector;
  if (partitionScheme != PartitionScheme::Mbr &&
      partitionScheme != PartitionScheme::Gpt) {
    error = "A concrete MBR or GPT partition scheme is required";
    return false;
  }
  if (deviceSectors <= start + (partitionScheme == PartitionScheme::Gpt
                                    ? gptEntrySectors + 1U
                                    : 0U)) {
    error = "The target is too small for the selected partition scheme";
    return false;
  }
  if (partitionScheme == PartitionScheme::Mbr &&
      deviceSectors - start > std::numeric_limits<std::uint32_t>::max()) {
    error = "MBR mode supports targets only through the 32-bit sector limit";
    return false;
  }
  const std::uint64_t alignmentSectors = (kMebibyte + sector - 1U) / sector;
  const std::uint64_t usableEndExclusive =
      partitionScheme == PartitionScheme::Gpt
          ? deviceSectors - gptEntrySectors - 1U
          : deviceSectors;
  std::uint64_t persistenceSectors = 0;
  std::uint64_t persistenceStart = usableEndExclusive;
  if (requestedPersistenceBytes != 0U) {
    const std::uint64_t requestedSectors =
        (requestedPersistenceBytes + sector - 1U) / sector;
    if (requestedSectors > std::numeric_limits<std::uint64_t>::max() -
                               (alignmentSectors - 1U)) {
      error = "The requested Linux persistence size overflows";
      return false;
    }
    persistenceSectors =
        ((requestedSectors + alignmentSectors - 1U) / alignmentSectors) *
        alignmentSectors;
    if (persistenceSectors >= usableEndExclusive - start) {
      error = "The requested Linux persistence partition leaves no room for ISO contents";
      return false;
    }
    persistenceStart = usableEndExclusive - persistenceSectors;
    persistenceStart = (persistenceStart / alignmentSectors) * alignmentSectors;
    persistenceSectors = usableEndExclusive - persistenceStart;
    if (persistenceStart > std::numeric_limits<std::uint32_t>::max() ||
        persistenceSectors > std::numeric_limits<std::uint32_t>::max()) {
      error = "The Linux persistence partition exceeds the MBR sector limit";
      return false;
    }
  }
  const std::uint64_t partitionSectors = persistenceStart - start;
  if (partitionSectors > std::numeric_limits<std::uint32_t>::max()) {
    error = "The FAT32 data partition exceeds its 32-bit sector-count limit";
    return false;
  }
  const std::uint64_t partitionBytes = partitionSectors * sector;
  std::uint64_t desiredClusterBytes = 4096;
  if (partitionBytes >= 32U * kGibibyte) {
    desiredClusterBytes = 32768;
  } else if (partitionBytes >= 16U * kGibibyte) {
    desiredClusterBytes = 16384;
  } else if (partitionBytes >= 8U * kGibibyte) {
    desiredClusterBytes = 8192;
  }

  if (requestedClusterBytes != 0U &&
      (requestedClusterBytes < sector || requestedClusterBytes > 32768U ||
       requestedClusterBytes % sector != 0U ||
       (requestedClusterBytes & (requestedClusterBytes - 1U)) != 0U)) {
    error =
        "FAT32 cluster size must be a power of two from the logical sector size through 32 KiB";
    return false;
  }
  if (requestedClusterBytes != 0U) {
    desiredClusterBytes = requestedClusterBytes;
  }
  std::uint32_t firstSectorsPerCluster = 1;
  while (static_cast<std::uint64_t>(firstSectorsPerCluster) * sector < desiredClusterBytes) {
    firstSectorsPerCluster *= 2U;
  }
  const std::uint32_t maximumSectorsPerCluster = 32768U / sector;
  std::uint32_t sectorsPerCluster = firstSectorsPerCluster;
  for (unsigned int candidate = 0; candidate < 16; ++candidate) {
    std::uint64_t fatSectors = 1;
    std::uint64_t clusters = 0;
    for (unsigned int iteration = 0; iteration < 32; ++iteration) {
      if (partitionSectors <= 32U + 2U * fatSectors) {
        break;
      }
      clusters = (partitionSectors - 32U - 2U * fatSectors) / sectorsPerCluster;
      const std::uint64_t requiredFatSectors =
          ((clusters + 2U) * 4U + sector - 1U) / sector;
      if (requiredFatSectors == fatSectors) {
        break;
      }
      fatSectors = requiredFatSectors;
    }
    if (partitionSectors <= 32U + 2U * fatSectors) {
      continue;
    }
    clusters = (partitionSectors - 32U - 2U * fatSectors) / sectorsPerCluster;
    if (clusters < kFat32MinimumClusters && sectorsPerCluster > 1U) {
      if (requestedClusterBytes != 0U) {
        break;
      }
      sectorsPerCluster /= 2U;
      continue;
    }
    if (clusters > kFat32MaximumCluster - 1U &&
        sectorsPerCluster < maximumSectorsPerCluster) {
      if (requestedClusterBytes != 0U) {
        break;
      }
      sectorsPerCluster *= 2U;
      continue;
    }
    if (clusters < kFat32MinimumClusters || clusters > kFat32MaximumCluster - 1U ||
        fatSectors > std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    layout.sectorBytes = sector;
    layout.partitionScheme = partitionScheme;
    layout.partitionStart = static_cast<std::uint32_t>(start);
    layout.partitionSectors = static_cast<std::uint32_t>(partitionSectors);
    layout.sectorsPerCluster = sectorsPerCluster;
    layout.fatSectors = static_cast<std::uint32_t>(fatSectors);
    layout.totalClusters = static_cast<std::uint32_t>(clusters);
    layout.dataStartSector = start + layout.reservedSectors + 2ULL * fatSectors;
    layout.persistenceStart = static_cast<std::uint32_t>(persistenceStart);
    layout.persistenceSectors = static_cast<std::uint32_t>(persistenceSectors);
    return true;
  }
  error = requestedClusterBytes == 0U
              ? "Target capacity cannot be represented by a compatible FAT32 layout"
              : "The selected FAT32 cluster size is incompatible with the target capacity";
  return false;
}

void collectNodes(FatNode& node, std::vector<FatNode*>& directories,
                  std::vector<FatNode*>& files) {
  if (node.directory) {
    directories.push_back(&node);
    for (auto& child : node.children) {
      collectNodes(*child, directories, files);
    }
  } else {
    files.push_back(&node);
  }
}

bool allocateNodes(FatNode& root, const FatLayout& layout,
                   std::vector<FatNode*>& directories, std::vector<FatNode*>& files,
                   std::uint32_t& nextCluster, std::unordered_set<std::uint32_t>& chainEnds,
                   std::string& error) {
  collectNodes(root, directories, files);
  const std::uint64_t clusterBytes = layout.clusterBytes();
  nextCluster = 2;
  for (FatNode* directory : directories) {
    std::uint64_t entries = directory == &root ? 2U : 3U;
    for (const auto& child : directory->children) {
      entries += 1U + longNameEntryCount(*child);
    }
    const std::uint64_t bytes = entries * 32U;
    const std::uint64_t clusters = std::max<std::uint64_t>(1, (bytes + clusterBytes - 1U) /
                                                                  clusterBytes);
    if (clusters > std::numeric_limits<std::uint32_t>::max() ||
        nextCluster > layout.totalClusters + 1U ||
        clusters > layout.totalClusters + 2ULL - nextCluster) {
      error = "The target does not have enough FAT32 directory space";
      return false;
    }
    directory->firstCluster = nextCluster;
    directory->clusterCount = static_cast<std::uint32_t>(clusters);
    nextCluster += directory->clusterCount;
    chainEnds.emplace(nextCluster - 1U);
  }
  for (FatNode* file : files) {
    const std::uint64_t clusters =
        (file->source->sizeBytes + clusterBytes - 1U) / clusterBytes;
    if (clusters == 0) {
      continue;
    }
    if (clusters > std::numeric_limits<std::uint32_t>::max() ||
        nextCluster > layout.totalClusters + 1U ||
        clusters > layout.totalClusters + 2ULL - nextCluster) {
      error = "The extracted ISO contents do not fit on the selected target";
      return false;
    }
    file->firstCluster = nextCluster;
    file->clusterCount = static_cast<std::uint32_t>(clusters);
    nextCluster += file->clusterCount;
    chainEnds.emplace(nextCluster - 1U);
  }
  return true;
}

bool writeAt(std::ofstream& output, const std::uint64_t offset,
             const unsigned char* bytes, const std::size_t size, std::string& error) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    error = "Staged disk image offset exceeds the host file API limit";
    return false;
  }
  output.seekp(static_cast<std::streamoff>(offset));
  output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  if (!output) {
    error = "Unable to write the staged disk image";
    return false;
  }
  return true;
}

bool writeAt(std::ofstream& output, const std::uint64_t offset,
             const std::vector<unsigned char>& bytes, std::string& error) {
  return writeAt(output, offset, bytes.data(), bytes.size(), error);
}

std::array<unsigned char, 11> volumeLabelBytes(const std::string& input) {
  std::array<unsigned char, 11> result{};
  result.fill(' ');
  std::size_t output = 0;
  for (const unsigned char character : input) {
    if (output == result.size()) {
      break;
    }
    if (character >= 0x20U && character < 0x7fU && character != '"' && character != '*' &&
        character != '+' && character != ',' && character != '/' && character != ':' &&
        character != ';' && character != '<' && character != '=' && character != '>' &&
        character != '?' && character != '[' && character != '\\' && character != ']' &&
        character != '|') {
      result[output++] = static_cast<unsigned char>(std::toupper(character));
    } else if (character >= 0x80U) {
      result[output++] = '_';
    }
  }
  if (output == 0) {
    constexpr char fallback[] = "RUFUS";
    std::copy(std::begin(fallback), std::end(fallback) - 1, result.begin());
  }
  return result;
}

bool decodeEmbeddedBase64(const std::string_view encoded,
                          const std::size_t expectedBytes,
                          std::vector<unsigned char>& decoded,
                          std::string& error) {
  const auto valueOf = [](const char character) -> int {
    if (character >= 'A' && character <= 'Z') {
      return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
      return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
      return character - '0' + 52;
    }
    if (character == '+') {
      return 62;
    }
    if (character == '/') {
      return 63;
    }
    return -1;
  };
  if (encoded.empty() || encoded.size() % 4U != 0U) {
    error = "The embedded GRUB bootstrap has invalid encoding";
    return false;
  }
  decoded.clear();
  decoded.reserve(encoded.size() / 4U * 3U);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 4U) {
    const int first = valueOf(encoded[offset]);
    const int second = valueOf(encoded[offset + 1U]);
    const int third = encoded[offset + 2U] == '='
                          ? 0
                          : valueOf(encoded[offset + 2U]);
    const int fourth = encoded[offset + 3U] == '='
                           ? 0
                           : valueOf(encoded[offset + 3U]);
    if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
        (encoded[offset + 2U] == '=' && encoded[offset + 3U] != '=') ||
        ((encoded[offset + 2U] == '=' || encoded[offset + 3U] == '=') &&
         offset + 4U != encoded.size())) {
      error = "The embedded GRUB bootstrap is corrupt";
      return false;
    }
    const std::uint32_t group =
        static_cast<std::uint32_t>(first) << 18U |
        static_cast<std::uint32_t>(second) << 12U |
        static_cast<std::uint32_t>(third) << 6U |
        static_cast<std::uint32_t>(fourth);
    decoded.push_back(static_cast<unsigned char>(group >> 16U));
    if (encoded[offset + 2U] != '=') {
      decoded.push_back(static_cast<unsigned char>(group >> 8U));
    }
    if (encoded[offset + 3U] != '=') {
      decoded.push_back(static_cast<unsigned char>(group));
    }
  }
  if (decoded.size() != expectedBytes) {
    error = "The embedded GRUB bootstrap has an unexpected size";
    decoded.clear();
    return false;
  }
  return true;
}

// Syslinux's installer is unusual: LDLINUX.SYS contains a small patch table
// that must name the exact disk sectors allocated to that file, and its boot
// sector contains another pointer to the first sector. Keeping the patcher in
// the portable layer lets every host produce identical, fully staged media
// without invoking a platform formatter or downloader.
bool patchSyslinux4(std::vector<unsigned char>& image,
                    const std::size_t originalImageBytes,
                    std::vector<unsigned char>& bootSector,
                    const std::vector<std::uint64_t>& sectors,
                    std::string& error) {
  constexpr std::uint32_t kLdlinuxMagic = 0x3eb202feU;
  constexpr std::size_t kPatchAreaBytes = 24U;
  constexpr std::size_t kExtendedPatchAreaBytes = 20U;
  constexpr std::size_t kSectorBytes = 512U;
  const std::size_t requiredSectors =
      (originalImageBytes + kSectorBytes - 1U) / kSectorBytes + 2U;
  if (originalImageBytes < kPatchAreaBytes ||
      image.size() != originalImageBytes + 2U * kSectorBytes ||
      bootSector.size() != kSectorBytes || sectors.size() < requiredSectors) {
    error = "The embedded Syslinux image or its allocated sector map is incomplete";
    return false;
  }

  std::size_t patchOffset = std::string::npos;
  for (std::size_t offset = 0;
       offset + kPatchAreaBytes <= originalImageBytes; offset += 4U) {
    if (get32(image.data() + offset) == kLdlinuxMagic) {
      patchOffset = offset;
      break;
    }
  }
  if (patchOffset == std::string::npos) {
    error = "The embedded Syslinux LDLINUX.SYS patch table is missing";
    return false;
  }

  const std::size_t extendedOffset = get16(image.data() + patchOffset + 22U);
  if (extendedOffset + kExtendedPatchAreaBytes > originalImageBytes) {
    error = "The embedded Syslinux extended patch table is invalid";
    return false;
  }
  const std::size_t advancePointerOffset =
      get16(image.data() + extendedOffset);
  const std::size_t extentOffset =
      get16(image.data() + extendedOffset + 10U);
  const std::size_t extentCapacity =
      get16(image.data() + extendedOffset + 12U);
  const std::size_t firstSectorLowOffset =
      get16(image.data() + extendedOffset + 14U);
  const std::size_t firstSectorHighOffset =
      get16(image.data() + extendedOffset + 16U);
  if (advancePointerOffset + 16U > originalImageBytes ||
      extentCapacity > (originalImageBytes - std::min(extentOffset, originalImageBytes)) /
                           10U ||
      extentOffset > originalImageBytes ||
      firstSectorLowOffset + 4U > bootSector.size() ||
      firstSectorHighOffset + 4U > bootSector.size()) {
    error = "The embedded Syslinux patch offsets are outside their images";
    return false;
  }

  put32(bootSector.data() + firstSectorLowOffset,
        static_cast<std::uint32_t>(sectors.front()));
  put32(bootSector.data() + firstSectorHighOffset,
        static_cast<std::uint32_t>(sectors.front() >> 32U));
  put16(image.data() + patchOffset + 8U,
        static_cast<std::uint16_t>(requiredSectors - 2U));
  put16(image.data() + patchOffset + 10U, 2U);
  put32(image.data() + patchOffset + 12U,
        static_cast<std::uint32_t>(originalImageBytes / 4U));
  std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(extentOffset),
              extentCapacity * 10U, 0U);

  std::size_t extentCount = 0U;
  std::uint32_t loadAddress = 0x8000U;
  std::uint32_t extentBaseAddress = loadAddress;
  std::uint64_t extentLba = 0U;
  std::uint16_t extentLength = 0U;
  const auto flushExtent = [&]() -> bool {
    if (extentLength == 0U) {
      return true;
    }
    if (extentCount >= extentCapacity) {
      error = "The embedded Syslinux image has insufficient extent slots";
      return false;
    }
    unsigned char* const extent =
        image.data() + extentOffset + extentCount * 10U;
    put64(extent, extentLba);
    put16(extent + 8U, extentLength);
    ++extentCount;
    return true;
  };

  // Sector zero is patched directly into the boot sector. The final two
  // sectors carry the redundant auxiliary data vector.
  for (std::size_t index = 1U; index + 2U < requiredSectors; ++index) {
    const std::uint64_t sector = sectors[index];
    if (extentLength != 0U) {
      const std::uint32_t extendedBytes =
          (static_cast<std::uint32_t>(extentLength) + 1U) * kSectorBytes;
      const bool consecutive = sector == extentLba + extentLength;
      const bool withinTransfer = extendedBytes < 65536U;
      const bool sameSegment =
          ((loadAddress ^ (extentBaseAddress + extendedBytes - 1U)) &
           0xffff0000U) == 0U;
      if (consecutive && withinTransfer && sameSegment) {
        ++extentLength;
        loadAddress += kSectorBytes;
        continue;
      }
      if (!flushExtent()) {
        return false;
      }
    }
    extentBaseAddress = loadAddress;
    extentLba = sector;
    extentLength = 1U;
    loadAddress += kSectorBytes;
  }
  if (!flushExtent()) {
    return false;
  }
  put64(image.data() + advancePointerOffset, sectors[requiredSectors - 2U]);
  put64(image.data() + advancePointerOffset + 8U,
        sectors[requiredSectors - 1U]);

  put32(image.data() + patchOffset + 16U, 0U);
  std::uint32_t checksum = kLdlinuxMagic;
  for (std::size_t offset = 0; offset + 4U <= originalImageBytes;
       offset += 4U) {
    checksum -= get32(image.data() + offset);
  }
  put32(image.data() + patchOffset + 16U, checksum);
  return true;
}

bool writeBootStructures(std::ofstream& output, const FatLayout& layout,
                         const std::array<unsigned char, 11>& label,
                         const std::uint32_t allocatedClusters,
                         const LegacyBiosBootstrap legacyBiosBootstrap,
                         const std::vector<unsigned char>* syslinuxBootSector,
                         std::string& error) {
  const bool windowsBiosBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::WindowsBootManager;
  const bool freeDosBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::FreeDos;
  const bool msDosBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::MsDos;
  const bool grubBiosBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::Grub2;
  const bool grub4DosBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::Grub4Dos;
  const bool reactOsBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::ReactOs;
  const bool syslinuxBootable =
      legacyBiosBootstrap == LegacyBiosBootstrap::Syslinux;
  const auto nonce = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::uint32_t volumeId = static_cast<std::uint32_t>(nonce ^ (nonce >> 32U) ^
                                                     layout.partitionSectors ^
                                                     allocatedClusters);
  if (volumeId == 0) {
    volumeId = 0x52554655U;
  }
  std::vector<unsigned char> sector(layout.sectorBytes, 0);
  if (windowsBiosBootable || freeDosBootable || msDosBootable ||
      syslinuxBootable) {
    static_assert(detail::kWindowsMbrBootstrap.size() == 440);
    std::copy(detail::kWindowsMbrBootstrap.begin(),
              detail::kWindowsMbrBootstrap.end(), sector.begin());
  } else if (grubBiosBootable) {
    std::vector<unsigned char> bootImage;
    if (!decodeEmbeddedBase64(detail::kGrub2BootImageBase64,
                              detail::kGrub2BootImageBytes, bootImage,
                              error)) {
      return false;
    }
    constexpr std::size_t mbrBootstrapBytes = 432;
    std::copy_n(bootImage.begin(), mbrBootstrapBytes, sector.begin());
  } else if (grub4DosBootable) {
    constexpr std::size_t mbrBootstrapBytes = 423U;
    static_assert(sizeof(grub4dos_mbr) >= 512U);
    std::copy_n(grub4dos_mbr, mbrBootstrapBytes, sector.begin());
  } else if (reactOsBootable) {
    static_assert(sizeof(reactos_mbr_0x0) <= 440U);
    std::copy_n(reactos_mbr_0x0, sizeof(reactos_mbr_0x0), sector.begin());
  }
  put32(sector.data() + 440, volumeId);
  if (layout.partitionScheme == PartitionScheme::Gpt) {
    sector[450] = 0xeeU;
    put32(sector.data() + 454U, 1U);
    const std::uint64_t sectorCount =
        layout.persistenceSectors == 0U
            ? static_cast<std::uint64_t>(layout.partitionStart) +
                  layout.partitionSectors +
                  (static_cast<std::uint64_t>(kGptEntryCount) * kGptEntrySize +
                   layout.sectorBytes - 1U) /
                      layout.sectorBytes +
                  1U
            : static_cast<std::uint64_t>(layout.persistenceStart) +
                  layout.persistenceSectors +
                  (static_cast<std::uint64_t>(kGptEntryCount) * kGptEntrySize +
                   layout.sectorBytes - 1U) /
                      layout.sectorBytes +
                  1U;
    put32(sector.data() + 458U,
          static_cast<std::uint32_t>(std::min<std::uint64_t>(
              sectorCount - 1U, std::numeric_limits<std::uint32_t>::max())));
  } else {
    sector[446] = legacyBiosBootstrap == LegacyBiosBootstrap::None ? 0x00 : 0x80;
    sector[447] = 0xfe;
    sector[448] = 0xff;
    sector[449] = 0xff;
    sector[450] = 0x0c;
    sector[451] = 0xfe;
    sector[452] = 0xff;
    sector[453] = 0xff;
    put32(sector.data() + 454, layout.partitionStart);
    put32(sector.data() + 458, layout.partitionSectors);
    if (layout.persistenceSectors != 0U) {
      sector[462] = 0x00;
      sector[463] = 0xfe;
      sector[464] = 0xff;
      sector[465] = 0xff;
      sector[466] = 0x83;
      sector[467] = 0xfe;
      sector[468] = 0xff;
      sector[469] = 0xff;
      put32(sector.data() + 470, layout.persistenceStart);
      put32(sector.data() + 474, layout.persistenceSectors);
    }
  }
  sector[510] = 0x55;
  sector[511] = 0xaa;
  if (!writeAt(output, 0, sector, error)) {
    return false;
  }
  if (layout.partitionScheme == PartitionScheme::Gpt &&
      !writeGptStructures(output, layout, error)) {
    return false;
  }
  if (grub4DosBootable) {
    const std::uint64_t availableBytes =
        static_cast<std::uint64_t>(layout.partitionStart - 1U) *
        layout.sectorBytes;
    const std::size_t sbrBytes = sizeof(grub4dos_mbr) - 512U;
    if (sbrBytes > availableBytes ||
        !writeAt(output, layout.sectorBytes, grub4dos_mbr + 512U,
                 sbrBytes, error)) {
      if (error.empty()) {
        error = "The staged partition leaves no room for the Grub4DOS secondary boot record";
      }
      return false;
    }
  }
  if (grubBiosBootable) {
    std::vector<unsigned char> coreImage;
    if (!decodeEmbeddedBase64(detail::kGrub2CoreImageBase64,
                              detail::kGrub2CoreImageBytes, coreImage,
                              error)) {
      return false;
    }
    const std::uint64_t availableBytes =
        static_cast<std::uint64_t>(layout.partitionStart - 1U) *
        layout.sectorBytes;
    if (coreImage.size() > availableBytes) {
      error = "The staged partition leaves no room for the GRUB BIOS core image";
      return false;
    }
    if (!writeAt(output, layout.sectorBytes, coreImage, error)) {
      return false;
    }
  }

  const std::uint64_t reservedBytes =
      static_cast<std::uint64_t>(layout.reservedSectors) * layout.sectorBytes;
  if (reservedBytes > std::numeric_limits<std::size_t>::max()) {
    error = "The FAT32 reserved region exceeds the host memory limit";
    return false;
  }
  std::vector<unsigned char> reserved(static_cast<std::size_t>(reservedBytes), 0);
  unsigned char* const boot = reserved.data();
  boot[0] = 0xeb;
  boot[1] = 0x58;
  boot[2] = 0x90;
  constexpr char rufusOem[] = "RUFUS++ ";
  constexpr char windowsOem[] = "MSWIN4.1";
  const char* const oem = windowsBiosBootable || msDosBootable
                              ? windowsOem
                              : rufusOem;
  std::copy_n(oem, 8, boot + 3);
  put16(boot + 11, static_cast<std::uint16_t>(layout.sectorBytes));
  boot[13] = static_cast<unsigned char>(layout.sectorsPerCluster);
  put16(boot + 14, static_cast<std::uint16_t>(layout.reservedSectors));
  boot[16] = 2;
  boot[21] = 0xf8;
  put16(boot + 24, 63);
  put16(boot + 26, 255);
  put32(boot + 28, layout.partitionStart);
  put32(boot + 32, layout.partitionSectors);
  put32(boot + 36, layout.fatSectors);
  put32(boot + 44, 2);
  put16(boot + 48, 1);
  put16(boot + 50, 6);
  boot[64] = 0x80;
  boot[66] = 0x29;
  put32(boot + 67, volumeId);
  std::copy(label.begin(), label.end(), boot + 71);
  constexpr char type[] = "FAT32   ";
  std::copy(std::begin(type), std::end(type) - 1, boot + 82);

  if (windowsBiosBootable) {
    static_assert(0x52U + detail::kFat32BootCodeAt0x52.size() < 0x3f0U);
    static_assert(0x3f0U + detail::kFat32BootCodeAt0x3f0.size() <= 0x600U);
    static_assert(0x1800U + detail::kFat32BootCodeAt0x1800.size() <= 0x1a00U);
    std::copy(detail::kFat32BootCodeAt0x52.begin(),
              detail::kFat32BootCodeAt0x52.end(), reserved.begin() + 0x52U);
    std::copy(detail::kFat32BootCodeAt0x3f0.begin(),
              detail::kFat32BootCodeAt0x3f0.end(), reserved.begin() + 0x3f0U);
    std::copy(detail::kFat32BootCodeAt0x1800.begin(),
              detail::kFat32BootCodeAt0x1800.end(), reserved.begin() + 0x1800U);
  } else if (freeDosBootable) {
    if (0x52U + sizeof(br_fat32_0x52) > reserved.size() ||
        0x3f0U + sizeof(br_fat32_0x3f0) > reserved.size()) {
      error = "The FAT32 reserved region is too small for the FreeDOS bootstrap";
      return false;
    }
    std::copy_n(br_fat32_0x52, sizeof(br_fat32_0x52),
                reserved.begin() + 0x52U);
    std::copy_n(br_fat32_0x3f0, sizeof(br_fat32_0x3f0),
                reserved.begin() + 0x3f0U);
  } else if (msDosBootable) {
    if (0x52U + sizeof(msdos_fat32_0x52) > reserved.size() ||
        0x3f0U + sizeof(msdos_fat32_0x3f0) > reserved.size()) {
      error = "The FAT32 reserved region is too small for the MS-DOS bootstrap";
      return false;
    }
    std::copy_n(msdos_fat32_0x52, sizeof(msdos_fat32_0x52),
                reserved.begin() + 0x52U);
    std::copy_n(msdos_fat32_0x3f0, sizeof(msdos_fat32_0x3f0),
                reserved.begin() + 0x3f0U);
  } else if (reactOsBootable) {
    if (0x52U + sizeof(reactos_fat32_0x52) > reserved.size() ||
        0x3f0U + sizeof(reactos_fat32_0x3f0) > reserved.size() ||
        0x1c00U + sizeof(reactos_fat32_0x1c00) > reserved.size()) {
      error = "The FAT32 reserved region is too small for the ReactOS bootstrap";
      return false;
    }
    std::copy_n(reactos_fat32_0x52, sizeof(reactos_fat32_0x52),
                reserved.begin() + 0x52U);
    std::copy_n(reactos_fat32_0x3f0, sizeof(reactos_fat32_0x3f0),
                reserved.begin() + 0x3f0U);
    std::copy_n(reactos_fat32_0x1c00, sizeof(reactos_fat32_0x1c00),
                reserved.begin() + 0x1c00U);
  } else if (syslinuxBootable) {
    if (syslinuxBootSector == nullptr ||
        syslinuxBootSector->size() != layout.sectorBytes ||
        layout.sectorBytes != 512U) {
      error = "The patched Syslinux boot sector is unavailable";
      return false;
    }
    // Preserve the generated FAT32 BPB while installing Syslinux's jump/OEM
    // header and executable boot code, matching syslinux_make_bootsect(VFAT).
    std::copy_n(syslinuxBootSector->begin(), 11U, boot);
    std::copy_n(syslinuxBootSector->begin() + 90U, 420U, boot + 90U);
  }
  boot[510] = 0x55;
  boot[511] = 0xaa;

  // The backup boot sector is a byte-for-byte copy of the completed primary
  // boot sector, including the optional BOOTMGR bootstrap.
  std::copy_n(boot, layout.sectorBytes,
              reserved.begin() + 6ULL * layout.sectorBytes);

  unsigned char* const fsInfo = reserved.data() + layout.sectorBytes;
  put32(fsInfo, 0x41615252U);
  put32(fsInfo + 484, 0x61417272U);
  put32(fsInfo + 488, layout.totalClusters - allocatedClusters);
  put32(fsInfo + 492,
        allocatedClusters < layout.totalClusters ? 2U + allocatedClusters : 0xffffffffU);
  put32(fsInfo + 508, 0xaa550000U);
  std::copy_n(fsInfo, layout.sectorBytes,
              reserved.begin() + 7ULL * layout.sectorBytes);

  const std::uint64_t partitionOffset =
      static_cast<std::uint64_t>(layout.partitionStart) * layout.sectorBytes;
  return writeAt(output, partitionOffset, reserved, error);
}

bool writeFatCopies(std::ofstream& output, const FatLayout& layout,
                    const std::uint32_t nextCluster,
                    const std::unordered_set<std::uint32_t>& chainEnds,
                    const IsoDeploymentCancelCallback& isCancelled,
                    std::string& error) {
  const std::size_t chunkBytes = std::max<std::size_t>(layout.sectorBytes, kTransferBytes);
  const std::uint64_t fatBytes =
      static_cast<std::uint64_t>(layout.fatSectors) * layout.sectorBytes;
  const std::uint64_t partitionOffset =
      static_cast<std::uint64_t>(layout.partitionStart) * layout.sectorBytes;
  for (unsigned int copy = 0; copy < 2; ++copy) {
    const std::uint64_t fatOffset = partitionOffset +
        static_cast<std::uint64_t>(layout.reservedSectors + copy * layout.fatSectors) *
            layout.sectorBytes;
    for (std::uint64_t offset = 0; offset < fatBytes; offset += chunkBytes) {
      if (isCancelled && isCancelled()) {
        error = "ISO deployment cancelled while creating the FAT32 allocation table";
        return false;
      }
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uint64_t>(chunkBytes, fatBytes - offset));
      std::vector<unsigned char> buffer(amount, 0);
      const std::uint64_t firstEntry = offset / 4U;
      const std::size_t entries = amount / 4U;
      for (std::size_t index = 0; index < entries; ++index) {
        const std::uint64_t entry = firstEntry + index;
        std::uint32_t value = 0;
        if (entry == 0) {
          value = 0x0ffffff8U;
        } else if (entry == 1) {
          value = 0xffffffffU;
        } else if (entry < nextCluster) {
          value = chainEnds.count(static_cast<std::uint32_t>(entry)) != 0
                      ? kFat32EndOfChain
                      : static_cast<std::uint32_t>(entry + 1U);
        }
        put32(buffer.data() + index * 4U, value);
      }
      if (!writeAt(output, fatOffset + offset, buffer, error)) {
        return false;
      }
    }
  }
  return true;
}

unsigned char shortNameChecksum(const std::array<unsigned char, 11>& name) {
  unsigned char checksum = 0;
  for (const unsigned char character : name) {
    checksum = static_cast<unsigned char>(((checksum & 1U) != 0U ? 0x80U : 0U) +
                                          (checksum >> 1U) + character);
  }
  return checksum;
}

void writeLongNameUnit(unsigned char* entry, const std::size_t index,
                       const char16_t value) {
  static constexpr std::array<std::size_t, 13> offsets =
      {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  put16(entry + offsets[index], static_cast<std::uint16_t>(value));
}

void appendLongEntries(std::vector<unsigned char>& directory, std::size_t& offset,
                       const FatNode& child) {
  const std::size_t count = longNameEntryCount(child);
  const unsigned char checksum = shortNameChecksum(child.shortName);
  for (std::size_t diskIndex = 0; diskIndex < count; ++diskIndex) {
    const std::size_t sequence = count - diskIndex;
    unsigned char* entry = directory.data() + offset;
    std::fill_n(entry, 32, 0xff);
    entry[0] = static_cast<unsigned char>(sequence | (sequence == count ? 0x40U : 0U));
    entry[11] = 0x0f;
    entry[12] = 0;
    entry[13] = checksum;
    put16(entry + 26, 0);
    const std::size_t first = (sequence - 1U) * 13U;
    for (std::size_t unit = 0; unit < 13; ++unit) {
      const std::size_t source = first + unit;
      const char16_t value = source < child.longName.size()
                                 ? child.longName[source]
                                 : source == child.longName.size() ? char16_t{0}
                                                                    : char16_t{0xffffU};
      writeLongNameUnit(entry, unit, value);
    }
    offset += 32U;
  }
}

void writeShortEntry(unsigned char* entry, const std::array<unsigned char, 11>& name,
                     const unsigned char attributes, const std::uint32_t firstCluster,
                     const std::uint32_t size) {
  std::fill_n(entry, 32, 0);
  std::copy(name.begin(), name.end(), entry);
  entry[11] = attributes;
  put16(entry + 16, 0x0021U);
  put16(entry + 18, 0x0021U);
  put16(entry + 24, 0x0021U);
  put16(entry + 20, static_cast<std::uint16_t>(firstCluster >> 16U));
  put16(entry + 26, static_cast<std::uint16_t>(firstCluster & 0xffffU));
  put32(entry + 28, size);
}

bool writeDirectories(std::ofstream& output, const FatLayout& layout,
                      const std::vector<FatNode*>& directories,
                      const std::array<unsigned char, 11>& label,
                      const IsoDeploymentCancelCallback& isCancelled,
                      std::string& error) {
  for (const FatNode* directory : directories) {
    if (isCancelled && isCancelled()) {
      error = "ISO deployment cancelled while creating FAT32 directories";
      return false;
    }
    const std::uint64_t size =
        static_cast<std::uint64_t>(directory->clusterCount) * layout.clusterBytes();
    if (size > std::numeric_limits<std::size_t>::max()) {
      error = "A FAT32 directory exceeds the host memory limit";
      return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size), 0);
    std::size_t offset = 0;
    if (directory->parent == nullptr) {
      writeShortEntry(bytes.data() + offset, label, 0x08, 0, 0);
      offset += 32U;
    } else {
      std::array<unsigned char, 11> dot{};
      dot.fill(' ');
      dot[0] = '.';
      writeShortEntry(bytes.data() + offset, dot, 0x10, directory->firstCluster, 0);
      offset += 32U;
      dot[1] = '.';
      // FAT32 uses zero for a root parent, even though the root has a cluster.
      const std::uint32_t parentCluster = directory->parent->parent == nullptr
                                              ? 0U : directory->parent->firstCluster;
      writeShortEntry(bytes.data() + offset, dot, 0x10, parentCluster, 0);
      offset += 32U;
    }
    for (const auto& child : directory->children) {
      appendLongEntries(bytes, offset, *child);
      writeShortEntry(bytes.data() + offset, child->shortName,
                      child->directory ? 0x10U : 0x20U, child->firstCluster,
                      child->directory ? 0U
                                       : static_cast<std::uint32_t>(child->source->sizeBytes));
      offset += 32U;
    }
    if (!writeAt(output, layout.clusterOffset(directory->firstCluster), bytes, error)) {
      return false;
    }
  }
  return true;
}

class ScopedWorkDirectory final {
 public:
  ~ScopedWorkDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  ScopedWorkDirectory(const ScopedWorkDirectory&) = delete;
  ScopedWorkDirectory& operator=(const ScopedWorkDirectory&) = delete;

  ScopedWorkDirectory() = default;

  bool create(const std::filesystem::path& path, std::string& error) {
    std::error_code fileError;
    if (!std::filesystem::create_directory(path, fileError)) {
      error = fileError ? "Unable to create the private WIM staging directory: " +
                              fileError.message()
                        : "The private WIM staging directory already exists";
      return false;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, fileError);
    if (fileError) {
      std::filesystem::remove(path, fileError);
      error = "Unable to secure the private WIM staging directory";
      return false;
    }
#endif
    path_ = path;
    return true;
  }

 private:
  std::filesystem::path path_;
};

bool extractToRegularFile(std::ifstream& image,
                          const detail::ImageFileRecord& source,
                          const std::filesystem::path& outputPath,
                          const IsoDeploymentProgressCallback& onProgress,
                          const IsoDeploymentCancelCallback& isCancelled,
                          bool& cancelled, std::string& error) {
  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Unable to create a temporary copy of sources/install.wim";
    return false;
  }
  std::vector<unsigned char> buffer(kTransferBytes);
  for (std::uint64_t offset = 0; offset < source.sizeBytes;) {
    if (isCancelled && isCancelled()) {
      cancelled = true;
      error = "ISO deployment cancelled while preparing sources/install.wim";
      return false;
    }
    const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        buffer.size(), source.sizeBytes - offset));
    if (!detail::readImageFile(image, source, offset, buffer.data(), amount, error)) {
      return false;
    }
    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(amount));
    if (!output) {
      error = "Unable to write the temporary install.wim";
      return false;
    }
    offset += amount;
    if (onProgress) {
      onProgress({IsoDeploymentStage::PreparingWindowsImage, offset, source.sizeBytes,
                  "Extracting sources/install.wim"});
    }
  }
  output.flush();
  if (!output) {
    error = "Unable to flush the temporary install.wim";
    return false;
  }
  return true;
}

bool validateSplitWimParts(const std::vector<std::filesystem::path>& parts,
                           const std::uint64_t maximumFileBytes,
                           std::vector<std::uint64_t>& sizes,
                           std::string& error) {
  if (parts.empty() || parts.size() > std::numeric_limits<std::uint16_t>::max()) {
    error = "wimlib produced an invalid number of split-WIM parts";
    return false;
  }
  std::array<unsigned char, 16> expectedGuid{};
  sizes.clear();
  for (std::size_t index = 0; index < parts.size(); ++index) {
    std::error_code fileError;
    const auto status = std::filesystem::symlink_status(parts[index], fileError);
    if (fileError || status.type() != std::filesystem::file_type::regular) {
      error = fileError ? "Unable to validate a split-WIM part: " +
                              fileError.message()
                        : "wimlib produced a non-regular split-WIM part";
      return false;
    }
    const std::uint64_t size = std::filesystem::file_size(parts[index], fileError);
    if (fileError || size < kWimHeaderProbeBytes || size > maximumFileBytes ||
        size > std::numeric_limits<std::uint32_t>::max()) {
      error = fileError
                  ? "Unable to validate a split-WIM part: " + fileError.message()
                  : size > maximumFileBytes
                        ? "A split-WIM part still exceeds FAT32's file-size limit; the WIM contains an indivisible resource that requires an NTFS strategy"
                        : "wimlib produced an invalid split-WIM part size";
      return false;
    }
    std::ifstream input(parts[index], std::ios::binary);
    std::array<unsigned char, kWimHeaderProbeBytes> header{};
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    constexpr std::array<unsigned char, 8> magic =
        {'M', 'S', 'W', 'I', 'M', 0, 0, 0};
    if (!input || !std::equal(magic.begin(), magic.end(), header.begin()) ||
        get32(header.data() + 8) < kWimHeaderProbeBytes) {
      error = "wimlib produced a split part with an invalid WIM header";
      return false;
    }
    std::array<unsigned char, 16> guid{};
    std::copy_n(header.begin() + 24, guid.size(), guid.begin());
    if (index == 0) {
      expectedGuid = guid;
    }
    if (guid != expectedGuid || get16(header.data() + 40) != index + 1U ||
        get16(header.data() + 42) != parts.size()) {
      error = "The split-WIM parts do not form one complete, ordered set";
      return false;
    }
    sizes.push_back(size);
  }
  return true;
}

bool prepareWindowsInstallImage(
    detail::IsoDeploymentContent& content, std::ifstream& image,
    const std::filesystem::path& workDirectory,
    const std::shared_ptr<const WimSplitter>& splitter,
    const IsoDeploymentOptions& options,
    const IsoDeploymentProgressCallback& onProgress,
    const IsoDeploymentCancelCallback& isCancelled,
    bool& cancelled, std::string& error) {
  const auto source = std::find_if(
      content.files.begin(), content.files.end(), [&](const detail::ImageFileRecord& file) {
        return file.sizeBytes > options.maximumFatFileBytes &&
               isWindowsInstallImagePath(file.path);
      });
  if (source == content.files.end()) {
    return true;
  }
  if (splitter == nullptr || !splitter->available()) {
    error = splitter == nullptr ? "No WIM splitting backend is available"
                                : splitter->availabilityReason();
    return false;
  }

  const bool sourceIsEsd = asciiFold(source->path) == "sources/install.esd";
  const auto extractedWim = workDirectory / (sourceIsEsd ? "install.esd" : "install.wim");
  if (onProgress) {
    onProgress({IsoDeploymentStage::PreparingWindowsImage, 0, source->sizeBytes,
                "Extracting " + source->path});
  }
  if (!extractToRegularFile(image, *source, extractedWim, onProgress,
                            isCancelled, cancelled, error)) {
    return false;
  }
  if (isCancelled && isCancelled()) {
    cancelled = true;
    error = "ISO deployment cancelled before preparing the Windows install image";
    return false;
  }

  const auto firstPart = workDirectory / "install.swm";
  const WimSplitResult split = splitter->split(
      extractedWim, firstPart, options.wimSplitPartBytes,
      [&](const WimSplitProgress& progress) {
        if (onProgress) {
          onProgress({IsoDeploymentStage::PreparingWindowsImage,
                      progress.bytesProcessed, progress.totalBytes,
                      std::string(sourceIsEsd ? "Converting and splitting install.esd (part "
                                              : "Splitting install.wim (part ") +
                          std::to_string(progress.currentPart) + '/' +
                          std::to_string(progress.totalParts) + ')'});
        }
      },
      isCancelled);
  if (!split.success) {
    cancelled = split.cancelled;
    error = split.error.empty() ? "Unable to prepare the Windows install image" : split.error;
    return false;
  }

  std::vector<std::uint64_t> partSizes;
  if (!validateSplitWimParts(split.parts, options.maximumFatFileBytes,
                             partSizes, error)) {
    return false;
  }
  const std::string sourcePath = source->path;
  const std::size_t slash = sourcePath.find_last_of('/');
  const std::string directory =
      slash == std::string::npos ? std::string{} : sourcePath.substr(0, slash + 1U);
  content.files.erase(source);
  content.entries.erase(
      std::remove_if(content.entries.begin(), content.entries.end(),
                     [&](const ImageContentEntry& entry) {
                       return !entry.directory &&
                              asciiFold(entry.path) == asciiFold(sourcePath);
                     }),
      content.entries.end());
  for (std::size_t index = 0; index < split.parts.size(); ++index) {
    const std::string filename =
        index == 0 ? "install.swm" : "install" + std::to_string(index + 1U) + ".swm";
    const std::string deployedPath = directory + filename;
    content.entries.push_back({deployedPath, partSizes[index], false});
    content.files.push_back(
        {deployedPath, partSizes[index], {}, {}, split.parts[index]});
  }
  return true;
}

bool readDeploymentFile(std::ifstream& image,
                        const detail::ImageFileRecord& file,
                        std::string& contents, std::string& error) {
  constexpr std::uint64_t maximumConfigurationBytes = 16ULL * kMebibyte;
  if (file.sizeBytes > maximumConfigurationBytes ||
      file.sizeBytes > std::numeric_limits<std::size_t>::max()) {
    error = "A Linux boot configuration or checksum manifest is unexpectedly large: " +
            file.path;
    return false;
  }
  contents.resize(static_cast<std::size_t>(file.sizeBytes));
  return contents.empty() ||
         detail::readImageFile(image, file, 0,
                               reinterpret_cast<unsigned char*>(contents.data()),
                               contents.size(), error);
}

bool writePreparedFile(const std::filesystem::path& path,
                       const std::string_view contents, std::string& error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Unable to create a prepared deployment file";
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output) {
    error = "Unable to write a prepared deployment file";
    return false;
  }
  return true;
}

void updateContentEntrySize(detail::IsoDeploymentContent& content,
                            const std::string& path,
                            const std::uint64_t size) {
  const std::string normalized = detail::normalizedPersistencePath(path);
  for (auto& entry : content.entries) {
    if (!entry.directory &&
        detail::normalizedPersistencePath(entry.path) == normalized) {
      entry.sizeBytes = size;
      return;
    }
  }
}

bool prepareLinuxPersistenceFiles(
    detail::IsoDeploymentContent& content,
    std::ifstream& image,
    const std::filesystem::path& workDirectory,
    const LinuxPersistenceStyle style,
    std::size_t& configurationsRecognized,
    std::size_t& configurationsModified,
    std::string& error) {
  configurationsRecognized = 0;
  configurationsModified = 0;
  std::unordered_map<std::string, std::string> updatedDigests;
  std::size_t preparedIndex = 0;
  for (auto& file : content.files) {
    if (!detail::isLinuxBootConfigurationPath(file.path)) {
      continue;
    }
    std::string contents;
    if (!readDeploymentFile(image, file, contents, error)) {
      return false;
    }
    const auto patch = patchLinuxPersistenceBootConfiguration(file.path, contents, style);
    configurationsRecognized += patch.recognizedConfiguration ? 1U : 0U;
    if (!patch.modified) {
      continue;
    }
    const auto preparedPath =
        workDirectory / ("boot-config-" + std::to_string(preparedIndex++) + ".cfg");
    if (!writePreparedFile(preparedPath, patch.contents, error)) {
      return false;
    }
    file.externalPath = preparedPath;
    file.sizeBytes = patch.contents.size();
    updateContentEntrySize(content, file.path, file.sizeBytes);
    updatedDigests[detail::normalizedPersistencePath(file.path)] =
        detail::md5Hex(patch.contents);
    ++configurationsModified;
  }

  if (updatedDigests.empty()) {
    return true;
  }
  for (auto& file : content.files) {
    if (!detail::isMd5ManifestPath(file.path)) {
      continue;
    }
    std::string manifest;
    if (!readDeploymentFile(image, file, manifest, error)) {
      return false;
    }
    if (!detail::updateMd5Manifest(manifest, updatedDigests)) {
      continue;
    }
    const auto preparedPath =
        workDirectory / ("checksum-" + std::to_string(preparedIndex++) + ".txt");
    if (!writePreparedFile(preparedPath, manifest, error)) {
      return false;
    }
    file.externalPath = preparedPath;
    file.sizeBytes = manifest.size();
    updateContentEntrySize(content, file.path, file.sizeBytes);
  }
  return true;
}

bool prepareRuntimeUefiValidation(
    detail::IsoDeploymentContent& content, std::ifstream& image,
    const RuntimeUefiValidationAssets& assets,
    const IsoDeploymentCancelCallback& isCancelled, std::string& error) {
  std::unordered_map<std::string, const std::vector<unsigned char>*> validators;
  for (const auto& asset : assets.bootloaders) {
    const std::string filename =
        detail::normalizedPersistencePath(asset.filename);
    if (filename.empty() || filename.find('/') != std::string::npos ||
        filename.rfind("boot", 0) != 0 ||
        filename.size() < 9U ||
        filename.substr(filename.size() - 4U) != ".efi" ||
        asset.data.empty() || !validators.emplace(filename, &asset.data).second) {
      error = "Runtime UEFI validation assets are malformed or duplicated";
      return false;
    }
  }
  if (validators.empty()) {
    error = "No runtime UEFI validation bootloaders were provided";
    return false;
  }

  content.files.erase(
      std::remove_if(content.files.begin(), content.files.end(),
                     [](const detail::ImageFileRecord& file) {
                       return detail::isMd5ManifestPath(file.path);
                     }),
      content.files.end());
  content.entries.erase(
      std::remove_if(content.entries.begin(), content.entries.end(),
                     [](const ImageContentEntry& entry) {
                       return !entry.directory &&
                              detail::isMd5ManifestPath(entry.path);
                     }),
      content.entries.end());

  std::unordered_set<std::string> validatorPaths;
  std::vector<detail::ImageFileRecord> insertedValidators;
  const std::size_t originalCount = content.files.size();
  for (std::size_t index = 0; index < originalCount; ++index) {
    auto& file = content.files[index];
    const std::string normalized =
        detail::normalizedPersistencePath(file.path);
    constexpr std::string_view prefix = "efi/boot/";
    if (normalized.rfind(prefix, 0) != 0) {
      continue;
    }
    const std::string filename = normalized.substr(prefix.size());
    const auto validator = validators.find(filename);
    if (validator == validators.end()) {
      continue;
    }
    const std::string originalPath = file.path;
    const std::size_t extension = file.path.size() - 4U;
    file.path.insert(extension, "_original");
    for (auto& entry : content.entries) {
      if (!entry.directory &&
          detail::normalizedPersistencePath(entry.path) == normalized) {
        entry.path = file.path;
        break;
      }
    }
    detail::ImageFileRecord replacement;
    replacement.path = originalPath;
    replacement.sizeBytes = validator->second->size();
    replacement.embeddedData = *validator->second;
    insertedValidators.push_back(std::move(replacement));
    validatorPaths.emplace(normalized);
  }
  if (insertedValidators.empty()) {
    error = "The ISO has no supported UEFI fallback loader to wrap for runtime validation";
    return false;
  }
  for (auto& validator : insertedValidators) {
    content.entries.push_back({validator.path, validator.sizeBytes, false});
    content.files.push_back(std::move(validator));
  }

  std::vector<std::size_t> order;
  order.reserve(content.files.size());
  for (std::size_t index = 0; index < content.files.size(); ++index) {
    if (validatorPaths.count(
            detail::normalizedPersistencePath(content.files[index].path)) == 0U) {
      order.push_back(index);
    }
  }
  std::sort(order.begin(), order.end(), [&content](const std::size_t left,
                                                   const std::size_t right) {
    return detail::normalizedPersistencePath(content.files[left].path) <
           detail::normalizedPersistencePath(content.files[right].path);
  });
  std::vector<unsigned char> buffer(kTransferBytes);
  std::ostringstream manifestBody;
  std::uint64_t totalBytes = 0U;
  for (const std::size_t index : order) {
    if (isCancelled && isCancelled()) {
      error = "ISO deployment cancelled while creating the runtime validation manifest";
      return false;
    }
    const auto& file = content.files[index];
    if (!checkedAdd(totalBytes, file.sizeBytes, totalBytes)) {
      error = "Runtime validation byte count overflowed";
      return false;
    }
    Md5Stream hash;
    for (std::uint64_t offset = 0; offset < file.sizeBytes;) {
      if (isCancelled && isCancelled()) {
        error = "ISO deployment cancelled while hashing " + file.path;
        return false;
      }
      const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
          buffer.size(), file.sizeBytes - offset));
      if (!detail::readImageFile(image, file, offset, buffer.data(), amount,
                                 error)) {
        error += " (while hashing " + file.path + ')';
        return false;
      }
      hash.update(buffer.data(), amount);
      offset += amount;
    }
    manifestBody << hash.finish() << "  ./" << file.path << '\n';
  }
  std::ostringstream manifest;
  manifest << "# md5sum_totalbytes = 0x" << std::hex << totalBytes << '\n'
           << manifestBody.str();
  const std::string manifestText = manifest.str();
  detail::ImageFileRecord manifestFile;
  manifestFile.path = "md5sum.txt";
  manifestFile.sizeBytes = manifestText.size();
  manifestFile.embeddedData.assign(manifestText.begin(), manifestText.end());
  content.entries.push_back(
      {manifestFile.path, manifestFile.sizeBytes, false});
  content.files.push_back(std::move(manifestFile));
  return true;
}

bool sourceUnchanged(const IsoDeploymentPlan& plan, std::string& error) {
  std::error_code fileError;
  const auto path = std::filesystem::u8path(plan.image().path);
  const auto size = std::filesystem::file_size(path, fileError);
  if (fileError || size != plan.image().sizeBytes) {
    error = fileError ? "The source ISO is unavailable: " + fileError.message()
                      : "The source ISO size changed after deployment was planned";
    return false;
  }
  const auto timestamp = std::filesystem::last_write_time(path, fileError);
  if (fileError || timestamp != plan.sourceLastWriteTime()) {
    error = fileError ? "The source ISO timestamp is unavailable: " + fileError.message()
                      : "The source ISO changed after deployment was planned";
    return false;
  }
  return true;
}

}  // namespace

IsoDeploymentPlanner::IsoDeploymentPlanner(
    std::shared_ptr<const WimSplitter> wimSplitter,
    const IsoDeploymentOptions options)
    : wimSplitter_(std::move(wimSplitter)), options_(options) {}

IsoDeploymentPlanResult IsoDeploymentPlanner::build(const ImageInfo& image,
                                                     const BlockDeviceInfo& target,
                                                     std::string volumeLabel,
                                                     const LinuxPersistenceOptions persistence,
                                                     const WindowsInstallationOptions windows) const {
  IsoDeploymentPlanResult result;
  const SafetyPolicy safety;
  appendIssues(result.issues, safety.validateWrite(image, target));
  const PartitionScheme partitionScheme =
      options_.partitionScheme == PartitionScheme::Unknown
          ? PartitionScheme::Mbr
          : options_.partitionScheme;
  IsoTargetSystem targetSystem = options_.targetSystem;
  if (targetSystem == IsoTargetSystem::Automatic) {
    targetSystem = partitionScheme == PartitionScheme::Gpt
                       ? IsoTargetSystem::Uefi
                   : image.capabilities.biosBootable &&
                             image.capabilities.uefiBootable
                       ? IsoTargetSystem::BiosAndUefi
                   : image.capabilities.biosBootable ? IsoTargetSystem::Bios
                                                     : IsoTargetSystem::Uefi;
  }
  const bool targetNeedsBios = targetSystem == IsoTargetSystem::Bios ||
                               targetSystem == IsoTargetSystem::BiosAndUefi;
  const bool targetNeedsUefi = targetSystem == IsoTargetSystem::Uefi ||
                               targetSystem == IsoTargetSystem::BiosAndUefi;
  if (partitionScheme == PartitionScheme::Gpt && targetNeedsBios) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "GPT ISO deployment supports UEFI (non-CSM) targets only"});
  }
  if (targetNeedsUefi && !image.capabilities.uefiBootable) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "The selected target system requires UEFI boot files that are not present in the image"});
  }
  if (image.format != ImageFormat::Iso || !image.capabilities.isoExtraction) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "ISO mode requires a readable ISO-9660, Joliet, or UDF file tree"});
  }
  const bool linuxPersistence = persistence.sizeBytes != 0U;
  if (linuxPersistence &&
      (!image.capabilities.linuxPersistence ||
       image.capabilities.linuxPersistenceStyle ==
           LinuxPersistenceStyle::None)) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "Linux persistence requires a validated Casper or Debian Live boot entry"});
  }
  if (linuxPersistence && persistence.sizeBytes < 256ULL * kMebibyte) {
    result.issues.push_back(
        {SafetyIssueCode::DeviceTooSmall,
         "A Linux persistence partition must be at least 256 MiB"});
  }
  const bool windowsInstaller = image.family == ImageFamily::WindowsInstaller &&
                                image.capabilities.standardWindowsInstallation;
  std::string windowsUnattendXml;
  const auto& experience = windows.userExperience;
  const bool hasWindowsCustomization =
      experience.bypassHardwareRequirements ||
      experience.bypassOnlineAccountRequirement || experience.createLocalAccount ||
      experience.useRegionalOptions || experience.disableDataCollection ||
      experience.disableAutomaticDeviceEncryption ||
      experience.applyQualityOfLifeOptions;
  if (windowsInstaller && hasWindowsCustomization) {
    const auto unattended = createWindowsUnattend(
        image.architecture, windows.userExperience,
        WindowsDeploymentMode::StandardInstallation);
    if (!unattended.succeeded()) {
      result.issues.push_back(
          {SafetyIssueCode::ModeUnsupported, unattended.error});
    } else {
      windowsUnattendXml = unattended.xml;
    }
  }
  const bool windowsBiosBootable =
      targetNeedsBios && windowsInstaller && image.capabilities.biosBootable &&
      target.logicalSectorSize == 512;
  const bool potentialGrubBiosBootable =
      targetNeedsBios && !windowsInstaller && image.capabilities.biosBootable &&
      image.capabilities.usesGrub && target.logicalSectorSize == 512;
  if (targetNeedsBios && !windowsBiosBootable && !potentialGrubBiosBootable) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "The selected BIOS target requires a supported 512-byte-sector Windows or GRUB2 bootstrap"});
  }
  if (options_.maximumFatFileBytes == 0 ||
      options_.maximumFatFileBytes > std::numeric_limits<std::uint32_t>::max() ||
      options_.wimSplitPartBytes == 0 ||
      options_.wimSplitPartBytes > options_.maximumFatFileBytes) {
    result.issues.push_back(
        {SafetyIssueCode::ImageUnsupported,
         "The ISO deployment file-size policy is invalid"});
  }
  if (targetNeedsBios && windowsInstaller && image.capabilities.biosBootable &&
      target.logicalSectorSize != 512) {
    result.warnings.emplace_back(
        "The target uses non-512-byte sectors; Windows Setup will be UEFI-bootable but its legacy BIOS bootstrap will be omitted");
  }
  if (targetNeedsBios && !windowsInstaller && image.capabilities.usesGrub &&
      image.capabilities.biosBootable && target.logicalSectorSize != 512U) {
    result.warnings.emplace_back(
        "The target uses non-512-byte sectors; the GRUB legacy BIOS bootstrap will be omitted");
  }

  FatLayout ignoredLayout;
  std::string layoutError;
  const bool preliminaryNtfs =
      !linuxPersistence &&
      (options_.fileSystem == IsoFilesystemPreference::Ntfs ||
       (options_.fileSystem == IsoFilesystemPreference::Automatic &&
        image.capabilities.requiresNtfs));
  if (preliminaryNtfs) {
    if (target.logicalSectorSize != 512U ||
        target.capacityBytes % target.logicalSectorSize != 0U) {
      layoutError = "UEFI:NTFS staging requires a 512-byte-sector target";
    } else if (partitionScheme == PartitionScheme::Mbr &&
               target.capacityBytes / target.logicalSectorSize >
                   std::numeric_limits<std::uint32_t>::max()) {
      layoutError =
          "MBR/UEFI:NTFS staging requires a target smaller than the 32-bit sector limit";
    } else if (partitionScheme == PartitionScheme::Gpt &&
               target.capacityBytes < 64ULL * kMebibyte) {
      layoutError = "The target is too small for a GPT/UEFI:NTFS layout";
    }
  } else {
    static_cast<void>(makeLayout(target, persistence.sizeBytes,
                                 partitionScheme, options_.clusterSizeBytes,
                                 ignoredLayout, layoutError));
  }
  if (!layoutError.empty()) {
    result.issues.push_back(
        {SafetyIssueCode::InvalidSectorSize, std::move(layoutError)});
  }

  std::error_code fileError;
  const auto sourcePath = std::filesystem::u8path(image.path);
  const auto sourceSize = std::filesystem::file_size(sourcePath, fileError);
  if (fileError || sourceSize != image.sizeBytes) {
    result.issues.push_back(
        {SafetyIssueCode::SourceChanged,
         fileError ? "The source ISO is unavailable: " + fileError.message()
                   : "The source ISO size changed after analysis"});
  }
  fileError.clear();
  const auto timestamp = std::filesystem::last_write_time(sourcePath, fileError);
  if (fileError) {
    result.issues.push_back(
        {SafetyIssueCode::SourceChanged,
         "The source ISO timestamp is unavailable: " + fileError.message()});
  }
  if (!result.issues.empty()) {
    return result;
  }

  const auto iso = detail::readIso9660Contents(sourcePath);
  const auto udf = detail::readUdfContents(sourcePath);
  const bool preferUdf = udf.valid && (!iso.valid || udf.entries.size() > iso.entries.size());
  if (!iso.valid && !udf.valid) {
    result.issues.push_back(
        {SafetyIssueCode::ImageUnsupported, "The optical image file tree is no longer readable"});
    return result;
  }
  auto content = std::make_shared<detail::IsoDeploymentContent>();
  if (preferUdf) {
    content->entries = udf.entries;
    content->files = udf.files;
    result.warnings.insert(result.warnings.end(), udf.warnings.begin(),
                           udf.warnings.end());
  } else {
    content->entries = iso.entries;
    content->files = iso.files;
    result.warnings.insert(result.warnings.end(), iso.warnings.begin(),
                           iso.warnings.end());
  }
  std::string grubAliasedPrefix;
  const bool grubBiosBootable =
      potentialGrubBiosBootable &&
      prepareGrubBiosCompatibilityTree(*content, grubAliasedPrefix);
  if (!grubAliasedPrefix.empty()) {
    result.warnings.emplace_back(
        "The " + grubAliasedPrefix +
        " module tree will also be staged as /boot/grub for legacy BIOS boot");
  }
  if (potentialGrubBiosBootable && !grubBiosBootable) {
    result.warnings.emplace_back(
        "The image does not contain the standard /boot/grub/i386-pc module tree; its extracted layout will be UEFI-only");
  }
  if (!image.capabilities.uefiBootable && !windowsBiosBootable &&
      !grubBiosBootable) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "The BIOS-only image does not contain a compatible standard GRUB2 module tree"});
    return result;
  }
  std::set<std::string> readableFiles;
  for (const auto& file : content->files) {
    readableFiles.emplace(file.path);
  }
  for (const auto& entry : content->entries) {
    if (!entry.directory && readableFiles.count(entry.path) == 0) {
      result.issues.push_back(
          {SafetyIssueCode::ImageUnsupported,
           "The optical image contains a file whose data cannot be extracted: " + entry.path});
      return result;
    }
  }
  std::uint64_t extractedBytes = 0;
  for (const auto& file : content->files) {
    if (!checkedAdd(extractedBytes, file.sizeBytes, extractedBytes)) {
      result.issues.push_back(
          {SafetyIssueCode::ImageUnsupported,
           "The extracted ISO content size overflows"});
      return result;
    }
  }
  if (linuxPersistence) {
    const std::uint64_t fatPartitionBytes =
        static_cast<std::uint64_t>(ignoredLayout.partitionSectors) *
        ignoredLayout.sectorBytes;
    const std::uint64_t projectedBytes =
        extractedBytes > std::numeric_limits<std::uint64_t>::max() / 11U
            ? std::numeric_limits<std::uint64_t>::max()
            : (extractedBytes * 11U + 9U) / 10U;
    if (projectedBytes > fatPartitionBytes) {
      result.issues.push_back(
          {SafetyIssueCode::DeviceTooSmall,
           "The requested persistence size leaves less than Rufus++'s 110% ISO-content allowance"});
      return result;
    }
    const bool hasBootConfiguration = std::any_of(
        content->files.begin(), content->files.end(),
        [](const detail::ImageFileRecord& file) {
          return detail::isLinuxBootConfigurationPath(file.path);
        });
    if (!hasBootConfiguration) {
      result.warnings.emplace_back(
          "No recognized GRUB/Syslinux configuration was found; the persistence partition will be created but this distribution may require manual kernel arguments");
    }
  }
  bool splitWindowsImage = false;
  bool payloadRequiresNtfs = false;
  for (const auto& file : content->files) {
    if (file.sizeBytes <= options_.maximumFatFileBytes) {
      continue;
    }
    if (windowsInstaller && isWindowsInstallImagePath(file.path) &&
        !splitWindowsImage) {
      splitWindowsImage = true;
      continue;
    }
    payloadRequiresNtfs = true;
  }
  if (options_.fileSystem == IsoFilesystemPreference::Fat32 &&
      payloadRequiresNtfs) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "The selected FAT32 filesystem cannot store one or more files in this image"});
    return result;
  }
  const bool requiresNtfs =
      payloadRequiresNtfs ||
      options_.fileSystem == IsoFilesystemPreference::Ntfs;
  if (requiresNtfs) {
    if (linuxPersistence) {
      result.issues.push_back(
          {SafetyIssueCode::ModeUnsupported,
           "Linux persistence cannot be combined with an ISO that requires NTFS staging"});
      return result;
    }
    const bool hasBootmgr = std::any_of(
        content->files.begin(), content->files.end(),
        [](const detail::ImageFileRecord& file) {
          return asciiFold(file.path) == "bootmgr" && file.sizeBytes != 0;
        });
    if (targetNeedsBios &&
        (!windowsBiosBootable || !hasBootmgr ||
         (image.architecture != ImageArchitecture::X86 &&
          image.architecture != ImageArchitecture::X64))) {
      result.issues.push_back(
          {SafetyIssueCode::ModeUnsupported,
           "NTFS legacy BIOS boot requires an x86/x64 Windows installer with a root BOOTMGR file"});
      return result;
    }
    if (target.logicalSectorSize != 512U) {
      result.issues.push_back(
          {SafetyIssueCode::InvalidSectorSize,
           "NTFS staging currently requires a 512-byte-sector target"});
      return result;
    }
    if (partitionScheme == PartitionScheme::Mbr &&
        target.capacityBytes / target.logicalSectorSize >
            std::numeric_limits<std::uint32_t>::max()) {
      result.issues.push_back(
          {SafetyIssueCode::ModeUnsupported,
           "MBR/UEFI:NTFS staging requires a target smaller than the 32-bit sector limit"});
      return result;
    }
    if (!options_.ntfsAvailable) {
      result.issues.push_back(
          {SafetyIssueCode::ModeUnsupported,
           "This ISO requires NTFS, but the host NTFS staging provider is unavailable"});
      return result;
    }
    // NTFS can retain the original install image. Splitting is only a FAT32
    // compatibility transformation.
    splitWindowsImage = false;
    if (targetNeedsUefi) result.warnings.emplace_back(
        "The ISO will be extracted to NTFS with the UEFI:NTFS helper partition");
    if (targetNeedsBios) result.warnings.emplace_back(
        "Windows MBR and NTFS BOOTMGR boot code will be installed for legacy BIOS boot");
  }
  if (requiresNtfs && options_.clusterSizeBytes != 0U &&
      (target.logicalSectorSize == 0U ||
       options_.clusterSizeBytes < target.logicalSectorSize ||
       options_.clusterSizeBytes > 65536U ||
       options_.clusterSizeBytes % target.logicalSectorSize != 0U ||
       (options_.clusterSizeBytes & (options_.clusterSizeBytes - 1U)) != 0U)) {
    result.issues.push_back(
        {SafetyIssueCode::InvalidSectorSize,
         "NTFS cluster size must be a power of two from the logical sector size through 64 KiB"});
    return result;
  }
  if (options_.runtimeUefiValidation != nullptr &&
      !targetNeedsUefi) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "Runtime media validation requires a UEFI-bootable ISO"});
    return result;
  }
  if (options_.runtimeUefiValidation != nullptr && requiresNtfs) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         "Runtime media validation is currently available for FAT32 ISO mode only"});
    return result;
  }
  if (splitWindowsImage &&
      (wimSplitter_ == nullptr || !wimSplitter_->available())) {
    result.issues.push_back(
        {SafetyIssueCode::ModeUnsupported,
         wimSplitter_ == nullptr ? "No WIM splitting backend is available"
                                 : wimSplitter_->availabilityReason()});
    return result;
  }
  if (splitWindowsImage) {
    result.warnings.emplace_back(
        "The Windows install image will be converted when necessary and split into FAT32-compatible .swm parts before writing");
  }
  if (volumeLabel.empty()) {
    volumeLabel = image.volumeLabel.empty() ? "RUFUS" : image.volumeLabel;
  }
  const LinuxPersistenceStyle persistenceStyle =
      !linuxPersistence
          ? LinuxPersistenceStyle::None
          : image.capabilities.linuxPersistenceStyle;
  const std::uint64_t alignedPersistenceBytes =
      linuxPersistence
          ? static_cast<std::uint64_t>(ignoredLayout.persistenceSectors) *
                ignoredLayout.sectorBytes
          : 0U;
  const LegacyBiosBootstrap legacyBiosBootstrap =
      windowsBiosBootable
          ? LegacyBiosBootstrap::WindowsBootManager
          : grubBiosBootable ? LegacyBiosBootstrap::Grub2
                              : LegacyBiosBootstrap::None;
  result.plan = IsoDeploymentPlan(image, target, std::move(volumeLabel), timestamp,
                                  std::move(content), wimSplitter_, options_,
                                  requiresNtfs ? IsoDeploymentFilesystem::Ntfs
                                               : IsoDeploymentFilesystem::Fat32,
                                  partitionScheme, targetSystem,
                                  legacyBiosBootstrap, splitWindowsImage,
                                  persistenceStyle, alignedPersistenceBytes,
                                  std::move(windowsUnattendXml));
  return result;
}

IsoDeploymentResult IsoImageStager::stage(
    const IsoDeploymentPlan& plan, const std::filesystem::path& outputPath,
    const IsoDeploymentProgressCallback& onProgress,
    const IsoDeploymentCancelCallback& isCancelled) const {
  IsoDeploymentResult result;
  if (onProgress) {
    onProgress({IsoDeploymentStage::Planning, 0, 0, {}});
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "ISO deployment was cancelled before staging began";
    return result;
  }
  if (plan.fileSystem() != IsoDeploymentFilesystem::Fat32) {
    result.error =
        "NTFS ISO plans must be handled by a platform NTFS staging provider";
    return result;
  }
  std::string error;
  if (!sourceUnchanged(plan, error)) {
    result.error = std::move(error);
    return result;
  }
  const auto sourcePath = std::filesystem::u8path(plan.image().path);
  std::ifstream sourceImage(sourcePath, std::ios::binary);
  if (!sourceImage) {
    result.error = "Unable to open the source ISO for deployment";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(outputPath, fileError) || fileError) {
    result.error = fileError ? "Unable to validate the staging path: " + fileError.message()
                             : "The ISO staging output path already exists";
    return result;
  }

  const auto stagingParent = outputPath.parent_path().empty()
                                 ? std::filesystem::current_path(fileError)
                                 : outputPath.parent_path();
  if (fileError) {
    result.error = "Unable to resolve the ISO staging directory: " + fileError.message();
    return result;
  }

  detail::IsoDeploymentContent preparedContent = *plan.content_;
  ScopedWorkDirectory workDirectory;
  std::filesystem::path contentWorkPath;
  if (plan.splitWindowsImage_ || plan.hasLinuxPersistence() ||
      !plan.windowsUnattendXml_.empty()) {
    auto workPath = outputPath;
    workPath += plan.hasLinuxPersistence()
                    ? ".persistence-work"
                    : plan.splitWindowsImage_ ? ".wim-work" : ".windows-work";
    if (!workDirectory.create(workPath, result.error)) {
      return result;
    }
    contentWorkPath = workPath;
    if (plan.hasLinuxPersistence()) {
      std::size_t configurationsRecognized = 0;
      std::size_t configurationsModified = 0;
      if (!prepareLinuxPersistenceFiles(
              preparedContent, sourceImage, workPath,
              plan.linuxPersistenceStyle_, configurationsRecognized,
              configurationsModified, result.error)) {
        return result;
      }
      if (configurationsRecognized != 0U && configurationsModified == 0U) {
        // This is valid for media whose boot entries already contain the
        // required option. The formatter and final verification still run.
      }
    }
  }
  if (!plan.windowsUnattendXml_.empty()) {
    const auto unattendedPath = contentWorkPath / "autounattend.xml";
    if (!writePreparedFile(unattendedPath, plan.windowsUnattendXml_, result.error)) {
      return result;
    }
    constexpr std::string_view deployedPath = "autounattend.xml";
    bool fileReplaced = false;
    for (auto& file : preparedContent.files) {
      if (asciiFold(file.path) == deployedPath) {
        file.sizeBytes = plan.windowsUnattendXml_.size();
        file.extents.clear();
        file.embeddedData.clear();
        file.externalPath = unattendedPath;
        fileReplaced = true;
      }
    }
    bool entryReplaced = false;
    for (auto& entry : preparedContent.entries) {
      if (!entry.directory && asciiFold(entry.path) == deployedPath) {
        entry.sizeBytes = plan.windowsUnattendXml_.size();
        entryReplaced = true;
      }
    }
    if (!fileReplaced) {
      preparedContent.files.push_back(
          {std::string(deployedPath), plan.windowsUnattendXml_.size(), {}, {},
           unattendedPath});
    }
    if (!entryReplaced) {
      preparedContent.entries.push_back(
          {std::string(deployedPath), plan.windowsUnattendXml_.size(), false});
    }
  }
  if (plan.splitWindowsImage_) {
    const auto largeWim = std::find_if(
        preparedContent.files.begin(), preparedContent.files.end(),
        [&](const detail::ImageFileRecord& file) {
          return file.sizeBytes > plan.options_.maximumFatFileBytes &&
                 isWindowsInstallImagePath(file.path);
        });
    if (largeWim == preparedContent.files.end()) {
      result.error = "The planned large Windows image is no longer present";
      return result;
    }
    std::uint64_t preparationBytes = 0;
    if (!checkedAdd(largeWim->sizeBytes, largeWim->sizeBytes,
                    preparationBytes) ||
        !checkedAdd(preparationBytes, largeWim->sizeBytes,
                    preparationBytes) ||
        !checkedAdd(preparationBytes, 64ULL * kMebibyte,
                    preparationBytes)) {
      result.error = "The Windows image is too large to stage safely";
      return result;
    }
    const auto space = std::filesystem::space(stagingParent, fileError);
    if (fileError) {
      result.error = "Unable to check free space for WIM splitting: " +
                     fileError.message();
      return result;
    }
    if (space.available < preparationBytes) {
      result.error =
          "Not enough temporary disk space to extract and prepare the Windows install image";
      return result;
    }
    auto workPath = outputPath;
    workPath += ".wim-work";
    bool cancelled = false;
    if (!prepareWindowsInstallImage(preparedContent, sourceImage, workPath,
                                    plan.wimSplitter_, plan.options_, onProgress,
                                    isCancelled, cancelled, result.error)) {
      result.cancelled = cancelled;
      return result;
    }
    if (!sourceUnchanged(plan, result.error)) {
      return result;
    }
  }

  if (plan.options_.runtimeUefiValidation != nullptr) {
    if (onProgress) {
      onProgress({IsoDeploymentStage::Planning, 0U, 0U,
                  "Creating runtime UEFI validation manifest"});
    }
    if (!prepareRuntimeUefiValidation(
            preparedContent, sourceImage,
            *plan.options_.runtimeUefiValidation, isCancelled, result.error)) {
      result.cancelled = isCancelled && isCancelled();
      return result;
    }
  }

  FatLayout layout;
  if (!makeLayout(plan.target(), plan.linuxPersistenceBytes_,
                  plan.partitionScheme(), plan.clusterSizeBytes(), layout,
                  result.error)) {
    return result;
  }
  FatNode root;
  root.path = "/";
  std::uint64_t totalFileBytes = 0;
  if (!buildTree(preparedContent, root, totalFileBytes, result.error) ||
      !assignShortNames(root, result.error)) {
    return result;
  }
  std::vector<FatNode*> directories;
  std::vector<FatNode*> files;
  std::uint32_t nextCluster = 2;
  std::unordered_set<std::uint32_t> chainEnds;
  if (!allocateNodes(root, layout, directories, files, nextCluster, chainEnds, result.error)) {
    return result;
  }
  const std::uint32_t allocatedClusters = nextCluster - 2U;
  const std::uint64_t fatStagedBytes = layout.clusterOffset(nextCluster - 1U) +
                                       layout.clusterBytes();
  const std::uint64_t stagedBytes =
      plan.requiresTrailingMetadataClear() ? fatStagedBytes
                                           : plan.target().capacityBytes;
  const std::uint64_t reservedTailBytes = std::min<std::uint64_t>(
      kMebibyte, plan.target().capacityBytes);
  const std::uint64_t usableDataEnd =
      plan.hasLinuxPersistence()
          ? static_cast<std::uint64_t>(layout.persistenceStart) *
                layout.sectorBytes
      : plan.partitionScheme() == PartitionScheme::Gpt
          ? (static_cast<std::uint64_t>(layout.partitionStart) +
             layout.partitionSectors) *
                layout.sectorBytes
      : plan.quickFormat() ? plan.target().capacityBytes - reservedTailBytes
                           : plan.target().capacityBytes;
  if (fatStagedBytes > usableDataEnd) {
    result.error =
        "The extracted ISO contents exceed the selected data-partition layout";
    return result;
  }
  const auto stagingSpace = std::filesystem::space(stagingParent, fileError);
  if (fileError) {
    result.error = "Unable to check free space for ISO staging: " + fileError.message();
    return result;
  }
  if (stagingSpace.available < fatStagedBytes) {
    result.error = "Not enough temporary disk space to stage the FAT32 deployment image";
    return result;
  }
  const auto label = volumeLabelBytes(plan.volumeLabel());

  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create the staged disk image";
    return result;
  }
  bool ownsOutput = true;
  auto fail = [&](std::string message, const bool cancelled = false) {
    output.close();
    std::error_code ignored;
    if (ownsOutput) {
      std::filesystem::remove(outputPath, ignored);
    }
    result.cancelled = cancelled;
    result.error = std::move(message);
    return result;
  };

  if (onProgress) {
    onProgress({IsoDeploymentStage::Formatting, 0, 0, {}});
  }
  if (!writeBootStructures(output, layout, label, allocatedClusters,
                           plan.legacyBiosBootstrap_, nullptr, result.error) ||
      !writeFatCopies(output, layout, nextCluster, chainEnds, isCancelled, result.error) ||
      !writeDirectories(output, layout, directories, label, isCancelled, result.error)) {
    return fail(result.error, isCancelled && isCancelled());
  }

  std::vector<unsigned char> buffer(kTransferBytes);
  std::uint64_t extracted = 0;
  for (const FatNode* file : files) {
    if (onProgress) {
      onProgress({IsoDeploymentStage::Extracting, extracted, totalFileBytes, file->path});
    }
    std::uint64_t offset = 0;
    while (offset < file->source->sizeBytes) {
      if (isCancelled && isCancelled()) {
        return fail("ISO deployment cancelled while extracting " + file->path, true);
      }
      const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
          buffer.size(), file->source->sizeBytes - offset));
      if (!detail::readImageFile(sourceImage, *file->source, offset, buffer.data(), amount,
                                 result.error) ||
          !writeAt(output, layout.clusterOffset(file->firstCluster) + offset,
                   buffer.data(), amount, result.error)) {
        return fail(result.error + " (while extracting " + file->path + ')');
      }
      offset += amount;
      extracted += amount;
      if (onProgress) {
        onProgress({IsoDeploymentStage::Extracting, extracted, totalFileBytes, file->path});
      }
    }
    ++result.filesExtracted;
  }
  detail::Ext2FormatOptions persistenceFormat;
  std::vector<RawWriteRange> initializedRanges{{0U, fatStagedBytes}};
  if (plan.hasLinuxPersistence()) {
    if (onProgress) {
      onProgress({IsoDeploymentStage::CreatingPersistence, 0,
                  plan.linuxPersistenceBytes_,
                  linuxPersistenceStyleName(plan.linuxPersistenceStyle_)});
    }
    persistenceFormat.offsetBytes =
        static_cast<std::uint64_t>(layout.persistenceStart) * layout.sectorBytes;
    persistenceFormat.sizeBytes =
        static_cast<std::uint64_t>(layout.persistenceSectors) * layout.sectorBytes;
    persistenceFormat.volumeLabel =
        plan.linuxPersistenceStyle_ == LinuxPersistenceStyle::Casper
            ? "casper-rw"
            : "persistence";
    persistenceFormat.createPersistenceConf =
        plan.linuxPersistenceStyle_ == LinuxPersistenceStyle::DebianLive;
    persistenceFormat.casperShutdownHook =
        plan.linuxPersistenceStyle_ == LinuxPersistenceStyle::Casper;
    if (!detail::writeExt2Filesystem(output, persistenceFormat, isCancelled,
                                     result.error, &initializedRanges)) {
      return fail(result.error, isCancelled && isCancelled());
    }
    if (onProgress) {
      onProgress({IsoDeploymentStage::CreatingPersistence,
                  plan.linuxPersistenceBytes_, plan.linuxPersistenceBytes_,
                  persistenceFormat.volumeLabel});
    }
  }
  output.flush();
  if (!output) {
    return fail("Unable to flush the staged FAT32 image");
  }
  output.close();

  std::filesystem::resize_file(outputPath, stagedBytes, fileError);
  if (fileError) {
    return fail("Unable to finalize the staged disk image: " + fileError.message());
  }

  std::ifstream staged(outputPath, std::ios::binary);
  if (!staged) {
    return fail("Unable to reopen the staged disk image for verification");
  }
  auto verificationFail = [&](std::string message, const bool cancelled = false) {
    staged.close();
    return fail(std::move(message), cancelled);
  };
  std::vector<unsigned char> partitionSector(layout.sectorBytes, 0U);
  std::vector<unsigned char> filesystemBoot(layout.sectorBytes, 0U);
  staged.read(reinterpret_cast<char*>(partitionSector.data()),
              static_cast<std::streamsize>(partitionSector.size()));
  staged.seekg(static_cast<std::streamoff>(layout.partitionStart) *
               layout.sectorBytes);
  staged.read(reinterpret_cast<char*>(filesystemBoot.data()),
              static_cast<std::streamsize>(filesystemBoot.size()));
  if (!staged || partitionSector[510U] != 0x55U ||
      partitionSector[511U] != 0xaaU ||
      filesystemBoot[510U] != 0x55U || filesystemBoot[511U] != 0xaaU ||
      get16(filesystemBoot.data() + 11U) != layout.sectorBytes ||
      filesystemBoot[13U] != layout.sectorsPerCluster) {
    return verificationFail(
        "Staged FAT32 partition or allocation-unit metadata verification failed");
  }
  if (layout.partitionScheme == PartitionScheme::Gpt) {
    std::vector<unsigned char> primaryHeader(layout.sectorBytes, 0U);
    std::vector<unsigned char> backupHeader(layout.sectorBytes, 0U);
    staged.clear();
    staged.seekg(static_cast<std::streamoff>(layout.sectorBytes));
    staged.read(reinterpret_cast<char*>(primaryHeader.data()),
                static_cast<std::streamsize>(primaryHeader.size()));
    staged.seekg(static_cast<std::streamoff>(plan.target().capacityBytes -
                                             layout.sectorBytes));
    staged.read(reinterpret_cast<char*>(backupHeader.data()),
                static_cast<std::streamsize>(backupHeader.size()));
    constexpr std::array<unsigned char, 8> gptSignature{
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    const std::uint32_t observedCrc = get32(primaryHeader.data() + 16U);
    put32(primaryHeader.data() + 16U, 0U);
    if (!staged || partitionSector[450U] != 0xeeU ||
        !std::equal(gptSignature.begin(), gptSignature.end(),
                    primaryHeader.begin()) ||
        !std::equal(gptSignature.begin(), gptSignature.end(),
                    backupHeader.begin()) ||
        crc32(primaryHeader.data(), 92U) != observedCrc) {
      return verificationFail("Staged GPT metadata verification failed");
    }
  } else if (partitionSector[450U] != 0x0cU) {
    return verificationFail("Staged MBR partition metadata verification failed");
  }
  std::vector<unsigned char> expected(kTransferBytes);
  std::vector<unsigned char> observed(kTransferBytes);
  std::uint64_t verified = 0;
  for (const FatNode* file : files) {
    std::uint64_t offset = 0;
    while (offset < file->source->sizeBytes) {
      if (isCancelled && isCancelled()) {
        return verificationFail("ISO deployment cancelled while verifying " + file->path,
                                true);
      }
      const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
          expected.size(), file->source->sizeBytes - offset));
      if (!detail::readImageFile(sourceImage, *file->source, offset, expected.data(), amount,
                                 result.error)) {
        return verificationFail(result.error + " (while verifying " + file->path + ')');
      }
      const std::uint64_t stagedOffset = layout.clusterOffset(file->firstCluster) + offset;
      if (stagedOffset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return verificationFail("Staged verification offset exceeds the host file API limit");
      }
      staged.seekg(static_cast<std::streamoff>(stagedOffset));
      staged.read(reinterpret_cast<char*>(observed.data()), static_cast<std::streamsize>(amount));
      if (!staged || !std::equal(expected.begin(), expected.begin() + amount,
                                 observed.begin())) {
        return verificationFail("Staged FAT32 verification failed for " + file->path);
      }
      offset += amount;
      verified += amount;
      if (onProgress) {
        onProgress({IsoDeploymentStage::Verifying, verified, totalFileBytes, file->path});
      }
    }
  }
  staged.close();
  if (plan.hasLinuxPersistence() &&
      !detail::verifyExt2Filesystem(outputPath, persistenceFormat, result.error)) {
    return fail(result.error);
  }
  if (!sourceUnchanged(plan, result.error)) {
    return fail(result.error);
  }

  ImageInfo stagedImage;
  stagedImage.path = outputPath.u8string();
  stagedImage.displayName = outputPath.filename().u8string();
  stagedImage.volumeLabel = plan.volumeLabel();
  stagedImage.sizeBytes = stagedBytes;
  stagedImage.expandedSizeBytes = stagedBytes;
  stagedImage.format = ImageFormat::Raw;
  stagedImage.partitionScheme = plan.partitionScheme();
  stagedImage.family = plan.image().family;
  stagedImage.architecture = plan.image().architecture;
  stagedImage.capabilities.rawWrite = true;
  stagedImage.capabilities.uefiBootable =
      plan.targetSystem() == IsoTargetSystem::Uefi ||
      plan.targetSystem() == IsoTargetSystem::BiosAndUefi;
  stagedImage.capabilities.biosBootable = plan.biosBootable();
  stagedImage.capabilities.validPartitionTable = true;
  stagedImage.bootable = stagedImage.capabilities.uefiBootable ||
                         stagedImage.capabilities.biosBootable;
  result.success = true;
  result.bytesExtracted = extracted;
  result.stagedImage = std::move(stagedImage);
  if (plan.quickFormat() && !plan.requiresTrailingMetadataClear()) {
    // Copy the final metadata area from the new image: this removes stale GPT
    // signatures for MBR while preserving new GPT tables or ext2 metadata.
    initializedRanges.push_back({plan.target().capacityBytes - reservedTailBytes,
                                 reservedTailBytes});
    std::sort(initializedRanges.begin(), initializedRanges.end(),
              [](const auto& left, const auto& right) {
                return left.offsetBytes < right.offsetBytes;
              });
    for (const auto& range : initializedRanges) {
      if (!result.writeRanges.empty() &&
          range.offsetBytes <= result.writeRanges.back().offsetBytes +
                                   result.writeRanges.back().sizeBytes) {
        auto& previous = result.writeRanges.back();
        previous.sizeBytes = std::max(previous.offsetBytes + previous.sizeBytes,
                                      range.offsetBytes + range.sizeBytes) - previous.offsetBytes;
      } else {
        result.writeRanges.push_back(range);
      }
    }
  }
  ownsOutput = false;
  if (onProgress) {
    onProgress({IsoDeploymentStage::Complete, totalFileBytes, totalFileBytes, {}});
  }
  return result;
}

namespace {
class DirectoryFileTree final : public IsoDeploymentFileTree {
 public:
  explicit DirectoryFileTree(std::filesystem::path root) : root_(std::move(root)) {}
  bool createDirectories(const std::string& path, std::string& error) override {
    std::filesystem::path current = root_;
    for (const auto& part : std::filesystem::u8path(path)) {
      current /= part;
      std::error_code ec;
      const auto status = std::filesystem::symlink_status(current, ec);
      if ((ec && ec != std::errc::no_such_file_or_directory) ||
          std::filesystem::is_symlink(status)) {
        error = "Refusing an inaccessible or symbolic-link staging directory: " + path;
        return false;
      }
      ec.clear();
      std::filesystem::create_directories(current, ec);
      if (ec) {
        error = "Unable to create staging directory " + path + ": " + ec.message();
        return false;
      }
    }
    return true;
  }
  bool createFile(const std::string& path, std::string& error) override {
    const auto relative = std::filesystem::u8path(path);
    if (!closeFile(error) || !createDirectories(relative.parent_path().generic_u8string(), error)) {
      return false;
    }
    const auto destination = root_ / relative;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(destination, ec);
    if ((ec && ec != std::errc::no_such_file_or_directory) ||
        std::filesystem::exists(status) || std::filesystem::is_symlink(status)) {
      error = "Refusing to replace an existing or inaccessible staging file: " + path;
      return false;
    }
    output_.clear();
    output_.open(destination, std::ios::binary | std::ios::trunc);
    if (!output_) {
      error = "Unable to create staging file: " + path;
      return false;
    }
    return true;
  }
  bool write(const unsigned char* data, const std::size_t size, std::string& error) override {
    output_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output_) {
      error = "Unable to write staging file";
      return false;
    }
    return true;
  }
  bool closeFile(std::string& error) override {
    if (output_.is_open()) {
      output_.flush();
      output_.close();
      if (!output_) {
        error = "Unable to flush or close staging file";
        return false;
      }
    }
    if (input_.is_open()) {
      input_.close();
    }
    return true;
  }
  bool reopenReadOnly(std::string& error) override { return closeFile(error); }
  bool openFileForRead(const std::string& path, const std::uint64_t expectedSize,
                       std::string& error) override {
    if (!closeFile(error)) {
      return false;
    }
    input_.clear();
    input_.open(root_ / std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!input_ || input_.tellg() < 0 ||
        static_cast<std::uint64_t>(input_.tellg()) != expectedSize) {
      error = "Unable to reopen staging file, or its size changed: " + path;
      return false;
    }
    input_.seekg(0);
    return static_cast<bool>(input_);
  }
  bool read(unsigned char* data, const std::size_t size, std::string& error) override {
    input_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!input_) {
      error = "Unable to read staging file";
      return false;
    }
    return true;
  }
 private:
  std::filesystem::path root_;
  std::ofstream output_;
  std::ifstream input_;
};
}  // namespace

IsoDeploymentResult IsoImageStager::extractToDirectory(
    const IsoDeploymentPlan& plan, const std::filesystem::path& outputRoot,
    const IsoDeploymentProgressCallback& onProgress,
    const IsoDeploymentCancelCallback& isCancelled) const {
  std::error_code ec;
  if (!std::filesystem::is_directory(outputRoot, ec) || ec) {
    IsoDeploymentResult result;
    result.error = "The ISO extraction root is not an accessible directory";
    return result;
  }
  DirectoryFileTree destination(outputRoot);
  return extractToFileTree(plan, destination, onProgress, isCancelled);
}

IsoDeploymentResult IsoImageStager::extractToFileTree(
    const IsoDeploymentPlan& plan, IsoDeploymentFileTree& tree,
    const IsoDeploymentProgressCallback& onProgress,
    const IsoDeploymentCancelCallback& isCancelled) const {
  IsoDeploymentResult result;
  if (plan.fileSystem() != IsoDeploymentFilesystem::Ntfs) {
    result.error = "Directory extraction requires an NTFS ISO deployment plan";
    return result;
  }
  if (plan.hasLinuxPersistence() || plan.splitsWindowsImage()) {
    result.error = "The NTFS extractor received an incompatible deployment transformation";
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "ISO deployment was cancelled before extraction began";
    return result;
  }
  if (!sourceUnchanged(plan, result.error)) {
    return result;
  }
  const auto sourcePath = std::filesystem::u8path(plan.image().path);
  std::ifstream sourceImage(sourcePath, std::ios::binary);
  if (!sourceImage) {
    result.error = "Unable to open the source ISO for deployment";
    return result;
  }
  std::set<std::string> normalizedPaths;
  std::vector<std::string> components;
  const auto checkedDestination = [&](const std::string& path,
                                      std::string& destination,
                                      std::string& error) {
    if (!splitPath(path, components, error) || components.empty()) {
      return false;
    }
    destination = path;
    for (const auto& component : components) {
      std::u16string ignored;
      if (!validateComponent(component, ignored, error)) {
        return false;
      }
    }
    const std::string key = asciiFold(path);
    if (!normalizedPaths.emplace(key).second) {
      error = "Optical image contains duplicate or case-colliding paths: " + path;
      return false;
    }
    return true;
  };

  for (const auto& entry : plan.content_->entries) {
    if (!entry.directory) {
      continue;
    }
    std::string destination;
    if (!checkedDestination(entry.path, destination, result.error)) {
      return result;
    }
    if (!tree.createDirectories(destination, result.error)) {
      return result;
    }
  }

  std::uint64_t totalBytes = 0;
  bool replacesUnattend = false;
  for (const auto& file : plan.content_->files) {
    const bool unattended = !plan.windowsUnattendXml().empty() &&
                            asciiFold(file.path) == "autounattend.xml";
    const std::uint64_t size = unattended
                                   ? plan.windowsUnattendXml().size()
                                   : file.sizeBytes;
    if (!checkedAdd(totalBytes, size, totalBytes)) {
      result.error = "The NTFS extraction size overflows";
      return result;
    }
    replacesUnattend = replacesUnattend || unattended;
  }
  if (!plan.windowsUnattendXml().empty() && !replacesUnattend &&
      !checkedAdd(totalBytes, plan.windowsUnattendXml().size(), totalBytes)) {
    result.error = "The NTFS extraction size overflows";
    return result;
  }

  std::vector<unsigned char> buffer(kTransferBytes);
  std::uint64_t extracted = 0;
  const auto writeOne = [&](const std::string& path,
                            const detail::ImageFileRecord* source,
                            const std::string_view embedded) {
    std::string destination;
    if (!checkedDestination(path, destination, result.error)) {
      return false;
    }
    if (!tree.createFile(destination, result.error)) {
      return false;
    }
    const std::uint64_t size = source == nullptr ? embedded.size() : source->sizeBytes;
    std::uint64_t offset = 0;
    while (offset < size) {
      if (isCancelled && isCancelled()) {
        result.cancelled = true;
        result.error = "ISO deployment cancelled while extracting " + path;
        return false;
      }
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uint64_t>(buffer.size(), size - offset));
      if (source == nullptr) {
        std::copy_n(reinterpret_cast<const unsigned char*>(embedded.data()) + offset,
                    amount, buffer.data());
      } else if (!detail::readImageFile(sourceImage, *source, offset,
                                        buffer.data(), amount, result.error)) {
        result.error += " (while extracting " + path + ')';
        return false;
      }
      if (!tree.write(buffer.data(), amount, result.error)) {
        return false;
      }
      offset += amount;
      extracted += amount;
      if (onProgress) {
        onProgress({IsoDeploymentStage::Extracting, extracted, totalBytes, path});
      }
    }
    if (!tree.closeFile(result.error)) {
      return false;
    }
    ++result.filesExtracted;
    return true;
  };

  for (const auto& file : plan.content_->files) {
    if (onProgress) {
      onProgress({IsoDeploymentStage::Extracting, extracted, totalBytes,
                  file.path});
    }
    const bool unattended = !plan.windowsUnattendXml().empty() &&
                            asciiFold(file.path) == "autounattend.xml";
    if (!writeOne(file.path, unattended ? nullptr : &file,
                  unattended ? std::string_view(plan.windowsUnattendXml())
                             : std::string_view{})) {
      return result;
    }
  }
  if (!plan.windowsUnattendXml().empty() && !replacesUnattend &&
      !writeOne("autounattend.xml", nullptr, plan.windowsUnattendXml())) {
    return result;
  }

  if (!tree.reopenReadOnly(result.error)) {
    return result;
  }
  std::uint64_t verified = 0;
  normalizedPaths.clear();
  const auto verifyOne = [&](const std::string& path,
                             const detail::ImageFileRecord* source,
                             const std::string_view embedded) {
    std::string destination;
    if (!checkedDestination(path, destination, result.error)) {
      return false;
    }
    const std::uint64_t size = source == nullptr ? embedded.size() : source->sizeBytes;
    if (!tree.openFileForRead(destination, size, result.error)) {
      return false;
    }
    std::uint64_t offset = 0;
    std::vector<unsigned char> observed(buffer.size());
    while (offset < size) {
      if (isCancelled && isCancelled()) {
        result.cancelled = true;
        result.error = "ISO deployment cancelled while verifying " + path;
        return false;
      }
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uint64_t>(buffer.size(), size - offset));
      if (source == nullptr) {
        std::copy_n(reinterpret_cast<const unsigned char*>(embedded.data()) + offset,
                    amount, buffer.data());
      } else if (!detail::readImageFile(sourceImage, *source, offset,
                                        buffer.data(), amount, result.error)) {
        result.error += " (while verifying " + path + ')';
        return false;
      }
      if (!tree.read(observed.data(), amount, result.error) ||
          !std::equal(buffer.begin(), buffer.begin() + amount,
                                observed.begin())) {
        result.error = "NTFS staging verification failed for " + path;
        return false;
      }
      offset += amount;
      verified += amount;
      if (onProgress) {
        onProgress({IsoDeploymentStage::Verifying, verified, totalBytes, path});
      }
    }
    return tree.closeFile(result.error);
  };
  for (const auto& file : plan.content_->files) {
    const bool unattended = !plan.windowsUnattendXml().empty() &&
                            asciiFold(file.path) == "autounattend.xml";
    if (!verifyOne(file.path, unattended ? nullptr : &file,
                   unattended ? std::string_view(plan.windowsUnattendXml())
                              : std::string_view{})) {
      return result;
    }
  }
  if (!plan.windowsUnattendXml().empty() && !replacesUnattend &&
      !verifyOne("autounattend.xml", nullptr, plan.windowsUnattendXml())) {
    return result;
  }
  if (!sourceUnchanged(plan, result.error)) {
    return result;
  }
  result.success = true;
  result.bytesExtracted = extracted;
  if (onProgress) {
    onProgress({IsoDeploymentStage::Complete, totalBytes, totalBytes, {}});
  }
  return result;
}

const char* isoDeploymentStageName(const IsoDeploymentStage stage) noexcept {
  switch (stage) {
    case IsoDeploymentStage::Planning:
      return "Planning ISO deployment";
    case IsoDeploymentStage::PreparingWindowsImage:
      return "Preparing Windows image";
    case IsoDeploymentStage::Formatting:
      return "Creating FAT32 layout";
    case IsoDeploymentStage::CreatingPersistence:
      return "Creating Linux persistence";
    case IsoDeploymentStage::Extracting:
      return "Extracting ISO files";
    case IsoDeploymentStage::Verifying:
      return "Verifying staged files";
    case IsoDeploymentStage::Complete:
      return "ISO staging complete";
  }
  return "ISO deployment";
}

StandaloneMediaResult stageFat32Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel, const StandaloneBootMode bootMode,
    std::vector<StandaloneMediaFile> mediaFiles,
    const StandaloneMediaProgressCallback& onProgress,
    const StandaloneMediaCancelCallback& isCancelled,
    const StandaloneFormatOptions options) {
  StandaloneMediaResult result;
  if (outputPath.empty()) {
    result.error = "The standalone-media staging path is empty";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(outputPath, fileError) || fileError) {
    result.error = fileError
                       ? "Unable to inspect the standalone-media staging path: " +
                             fileError.message()
                       : "The standalone-media staging path already exists";
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone formatting cancelled before staging";
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Planning, 0U, target.capacityBytes});
  }
  FatLayout layout;
  if (!makeLayout(target, 0U, PartitionScheme::Mbr,
                  options.clusterSizeBytes, layout, result.error)) {
    return result;
  }
  detail::IsoDeploymentContent content;
  content.entries.reserve(mediaFiles.size());
  content.files.reserve(mediaFiles.size());
  for (auto& mediaFile : mediaFiles) {
    detail::ImageFileRecord file;
    file.path = std::move(mediaFile.path);
    file.sizeBytes = mediaFile.data.size();
    file.embeddedData = std::move(mediaFile.data);
    content.entries.push_back({file.path, file.sizeBytes, false});
    content.files.push_back(std::move(file));
  }
  if (bootMode == StandaloneBootMode::Syslinux) {
    std::vector<unsigned char> loader(
        syslinux4_ldlinux_sys,
        syslinux4_ldlinux_sys + syslinux4_ldlinux_sys_len);
    const std::size_t advanceOffset = loader.size();
    loader.resize(loader.size() + 1024U, 0U);
    constexpr std::uint32_t kAdvanceMagic1 = 0x5a2d2fa5U;
    constexpr std::uint32_t kAdvanceMagic2 = 0xa3041767U;
    constexpr std::uint32_t kAdvanceMagic3 = 0xdd28bf64U;
    put32(loader.data() + advanceOffset, kAdvanceMagic1);
    put32(loader.data() + advanceOffset + 4U, kAdvanceMagic2);
    put32(loader.data() + advanceOffset + 508U, kAdvanceMagic3);
    std::copy_n(loader.begin() + static_cast<std::ptrdiff_t>(advanceOffset),
                512U, loader.begin() +
                          static_cast<std::ptrdiff_t>(advanceOffset + 512U));
    detail::ImageFileRecord loaderFile;
    loaderFile.path = "LDLINUX.SYS";
    loaderFile.sizeBytes = loader.size();
    loaderFile.embeddedData = std::move(loader);
    content.entries.push_back(
        {loaderFile.path, loaderFile.sizeBytes, false});
    content.files.push_back(std::move(loaderFile));

    constexpr std::string_view configuration =
        "DEFAULT prompt\r\nPROMPT 1\r\n"
        "SAY Syslinux media created by Rufus++\r\n";
    detail::ImageFileRecord configurationFile;
    configurationFile.path = "SYSLINUX.CFG";
    configurationFile.sizeBytes = configuration.size();
    configurationFile.embeddedData.assign(configuration.begin(),
                                           configuration.end());
    content.entries.push_back(
        {configurationFile.path, configurationFile.sizeBytes, false});
    content.files.push_back(std::move(configurationFile));
  }
  if (bootMode == StandaloneBootMode::FreeDos ||
      bootMode == StandaloneBootMode::MsDos) {
    const auto has = [&content](const std::string_view expected) {
      return std::any_of(content.files.begin(), content.files.end(),
                         [expected](const detail::ImageFileRecord& file) {
                           return asciiFold(file.path) == expected;
                         });
    };
    const bool requiredFilesPresent =
        bootMode == StandaloneBootMode::FreeDos
            ? has("kernel.sys") && has("command.com")
            : has("io.sys") && has("msdos.sys") && has("command.com");
    if (!requiredFilesPresent) {
      result.error = bootMode == StandaloneBootMode::FreeDos
                         ? "FreeDOS media requires KERNEL.SYS and COMMAND.COM payloads"
                         : "MS-DOS FAT32 media requires user-supplied IO.SYS, MSDOS.SYS, and COMMAND.COM payloads";
      return result;
    }
  } else if (bootMode == StandaloneBootMode::Grub4Dos) {
    const bool hasGrldr = std::any_of(
        content.files.begin(), content.files.end(),
        [](const detail::ImageFileRecord& file) {
          return asciiFold(file.path) == "grldr";
        });
    if (!hasGrldr) {
      result.error =
          "Grub4DOS media requires a user-supplied GRLDR loader";
      return result;
    }
  } else if (bootMode == StandaloneBootMode::ReactOs) {
    const bool hasFreeLoader = std::any_of(
        content.files.begin(), content.files.end(),
        [](const detail::ImageFileRecord& file) {
          return asciiFold(file.path) == "freeldr.sys";
        });
    if (!hasFreeLoader) {
      result.error =
          "ReactOS media requires a user-supplied FREELDR.SYS loader";
      return result;
    }
  } else if (bootMode == StandaloneBootMode::Uefi) {
    const bool hasFallbackLoader = std::any_of(
        content.files.begin(), content.files.end(),
        [](const detail::ImageFileRecord& file) {
          const std::string path =
              detail::normalizedPersistencePath(file.path);
          constexpr std::array<std::string_view, 6> supported{
              "efi/boot/bootx64.efi", "efi/boot/bootia32.efi",
              "efi/boot/bootaa64.efi", "efi/boot/bootarm.efi",
              "efi/boot/bootloongarch64.efi",
              "efi/boot/bootriscv64.efi"};
          return std::find(supported.begin(), supported.end(), path) !=
                 supported.end();
        });
    if (!hasFallbackLoader) {
      result.error =
          "UEFI media requires an application at a supported EFI/BOOT fallback path";
      return result;
    }
  }
  FatNode root;
  root.path = "/";
  std::uint64_t totalFileBytes = 0U;
  if (!buildTree(content, root, totalFileBytes, result.error) ||
      !assignShortNames(root, result.error)) {
    return result;
  }
  std::vector<FatNode*> directories;
  std::vector<FatNode*> fatFiles;
  std::uint32_t nextCluster = 2U;
  std::unordered_set<std::uint32_t> chainEnds;
  if (!allocateNodes(root, layout, directories, fatFiles, nextCluster, chainEnds,
                     result.error)) {
    return result;
  }
  std::vector<unsigned char> syslinuxBootSector;
  if (bootMode == StandaloneBootMode::Syslinux) {
    const auto loaderRecord = std::find_if(
        content.files.begin(), content.files.end(),
        [](const detail::ImageFileRecord& file) {
          return asciiFold(file.path) == "ldlinux.sys";
        });
    if (loaderRecord == content.files.end()) {
      result.error = "The staged Syslinux loader could not be found";
      return result;
    }
    const detail::ImageFileRecord* const loaderRecordAddress = &*loaderRecord;
    const auto loaderNode = std::find_if(
        fatFiles.begin(), fatFiles.end(),
        [loaderRecordAddress](const FatNode* node) {
          return node->source == loaderRecordAddress;
        });
    if (loaderNode == fatFiles.end()) {
      result.error = "The staged Syslinux loader could not be allocated";
      return result;
    }
    constexpr std::size_t kSectorBytes = 512U;
    const std::size_t originalBytes = syslinux4_ldlinux_sys_len;
    const std::size_t requiredSectors =
        (originalBytes + kSectorBytes - 1U) / kSectorBytes + 2U;
    if (layout.sectorBytes != kSectorBytes ||
        static_cast<std::uint64_t>((*loaderNode)->clusterCount) *
                layout.sectorsPerCluster <
            requiredSectors) {
      result.error = "The FAT32 allocation cannot hold the Syslinux sector map";
      return result;
    }
    const std::uint64_t firstSector =
        layout.dataStartSector - layout.partitionStart +
        static_cast<std::uint64_t>((*loaderNode)->firstCluster - 2U) *
            layout.sectorsPerCluster;
    std::vector<std::uint64_t> sectors(requiredSectors);
    for (std::size_t index = 0; index < sectors.size(); ++index) {
      sectors[index] = firstSector + index;
    }
    syslinuxBootSector.assign(
        syslinux4_ldlinux_bss,
        syslinux4_ldlinux_bss + syslinux4_ldlinux_bss_len);
    if (!patchSyslinux4(loaderRecord->embeddedData, originalBytes,
                        syslinuxBootSector, sectors, result.error)) {
      return result;
    }
  }
  const std::uint64_t initializedBytes =
      layout.clusterOffset(nextCluster - 1U) + layout.clusterBytes();
  const std::uint64_t stagedBytes =
      options.quickFormat ? initializedBytes : target.capacityBytes;
  const auto parent = outputPath.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : outputPath.parent_path();
  if (fileError) {
    result.error = "Unable to resolve the staging directory: " +
                   fileError.message();
    return result;
  }
  const auto space = std::filesystem::space(parent, fileError);
  if (fileError || space.available < initializedBytes) {
    result.error = fileError
                       ? "Unable to inspect staging free space: " +
                             fileError.message()
                       : "Not enough free space to stage the FAT32 format";
    return result;
  }
  const auto label = volumeLabelBytes(volumeLabel);
  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create the standalone FAT32 staging image";
    return result;
  }
  bool keepOutput = false;
  const auto cleanup = [&] {
    output.close();
    if (!keepOutput) {
      std::error_code ignored;
      std::filesystem::remove(outputPath, ignored);
    }
  };
  if (onProgress) {
    onProgress({StandaloneMediaStage::Formatting, 0U, stagedBytes});
  }
  if (!writeBootStructures(output, layout, label, nextCluster - 2U,
                           bootMode == StandaloneBootMode::FreeDos
                               ? LegacyBiosBootstrap::FreeDos
                           : bootMode == StandaloneBootMode::MsDos
                               ? LegacyBiosBootstrap::MsDos
                           : bootMode == StandaloneBootMode::Grub2
                               ? LegacyBiosBootstrap::Grub2
                           : bootMode == StandaloneBootMode::Grub4Dos
                               ? LegacyBiosBootstrap::Grub4Dos
                           : bootMode == StandaloneBootMode::ReactOs
                               ? LegacyBiosBootstrap::ReactOs
                           : bootMode == StandaloneBootMode::Syslinux
                               ? LegacyBiosBootstrap::Syslinux
                               : LegacyBiosBootstrap::None,
                           syslinuxBootSector.empty() ? nullptr
                                                      : &syslinuxBootSector,
                           result.error) ||
      !writeFatCopies(output, layout, nextCluster, chainEnds, isCancelled,
                      result.error) ||
      !writeDirectories(output, layout, directories, label, isCancelled,
                        result.error)) {
    result.cancelled = isCancelled && isCancelled();
    cleanup();
    return result;
  }
  for (const FatNode* file : fatFiles) {
    if (isCancelled && isCancelled()) {
      result.cancelled = true;
      result.error = "Standalone media creation cancelled while copying files";
      cleanup();
      return result;
    }
    if (!file->source->embeddedData.empty() &&
        !writeAt(output, layout.clusterOffset(file->firstCluster),
                 file->source->embeddedData, result.error)) {
      cleanup();
      return result;
    }
  }
  output.flush();
  if (!output) {
    result.error = "Unable to flush the standalone FAT32 staging image";
    cleanup();
    return result;
  }
  output.close();
  std::filesystem::resize_file(outputPath, stagedBytes, fileError);
  if (fileError) {
    result.error = "Unable to finalize the FAT32 staging image: " +
                   fileError.message();
    cleanup();
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Verifying, 0U, stagedBytes});
  }
  std::ifstream verify(outputPath, std::ios::binary);
  std::vector<unsigned char> mbr(layout.sectorBytes, 0U);
  std::vector<unsigned char> boot(layout.sectorBytes, 0U);
  verify.read(reinterpret_cast<char*>(mbr.data()),
              static_cast<std::streamsize>(mbr.size()));
  verify.seekg(static_cast<std::streamoff>(layout.partitionStart) *
               layout.sectorBytes);
  verify.read(reinterpret_cast<char*>(boot.data()),
              static_cast<std::streamsize>(boot.size()));
  if (!verify || mbr[510] != 0x55U || mbr[511] != 0xaaU ||
      mbr[450] != 0x0cU || boot[510] != 0x55U || boot[511] != 0xaaU ||
      get16(boot.data() + 11U) != layout.sectorBytes ||
      !std::equal(label.begin(), label.end(), boot.begin() + 71U)) {
    result.error = "Standalone FAT32 metadata verification failed";
    cleanup();
    return result;
  }
  for (const FatNode* file : fatFiles) {
    if (file->source->embeddedData.empty()) {
      continue;
    }
    std::vector<unsigned char> observed(file->source->embeddedData.size());
    verify.clear();
    verify.seekg(static_cast<std::streamoff>(
        layout.clusterOffset(file->firstCluster)));
    verify.read(reinterpret_cast<char*>(observed.data()),
                static_cast<std::streamsize>(observed.size()));
    if (!verify || observed != file->source->embeddedData) {
      result.error = "Standalone media file verification failed: " + file->path;
      cleanup();
      return result;
    }
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone formatting cancelled after staging";
    cleanup();
    return result;
  }
  ImageInfo image;
  image.path = outputPath.u8string();
  image.displayName = outputPath.filename().u8string();
  image.volumeLabel = std::move(volumeLabel);
  image.sizeBytes = stagedBytes;
  image.expandedSizeBytes = stagedBytes;
  image.format = ImageFormat::Raw;
  image.partitionScheme = PartitionScheme::Mbr;
  image.capabilities.rawWrite = true;
  image.capabilities.validPartitionTable = true;
  image.capabilities.biosBootable =
      bootMode == StandaloneBootMode::FreeDos ||
      bootMode == StandaloneBootMode::MsDos ||
      bootMode == StandaloneBootMode::Grub2 ||
      bootMode == StandaloneBootMode::Grub4Dos ||
      bootMode == StandaloneBootMode::ReactOs ||
      bootMode == StandaloneBootMode::Syslinux;
  image.capabilities.uefiBootable = bootMode == StandaloneBootMode::Uefi;
  image.bootable = image.capabilities.biosBootable ||
                   image.capabilities.uefiBootable;
  result.stagedImage = std::move(image);
  result.success = true;
  keepOutput = true;
  if (onProgress) {
    onProgress({StandaloneMediaStage::Complete, stagedBytes, stagedBytes});
  }
  return result;
}

StandaloneMediaResult stageBlankFat32Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel,
    const StandaloneMediaProgressCallback& onProgress,
    const StandaloneMediaCancelCallback& isCancelled,
    const StandaloneFormatOptions options) {
  return stageFat32Media(target, outputPath, std::move(volumeLabel),
                         StandaloneBootMode::None, {}, onProgress,
                         isCancelled, options);
}

StandaloneMediaResult stageBlankFat16Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel,
    const StandaloneMediaProgressCallback& onProgress,
    const StandaloneMediaCancelCallback& isCancelled,
    const StandaloneFormatOptions options) {
  StandaloneMediaResult result;
  constexpr std::uint32_t sectorBytes = 512U;
  constexpr std::uint32_t partitionStart = 2048U;
  constexpr std::uint32_t reservedSectors = 1U;
  constexpr std::uint32_t fatCopies = 2U;
  constexpr std::uint32_t rootEntries = 512U;
  constexpr std::uint32_t rootSectors = rootEntries * 32U / sectorBytes;
  if (outputPath.empty() || target.logicalSectorSize != sectorBytes ||
      target.capacityBytes < 16ULL * kMebibyte ||
      target.capacityBytes > 2ULL * kGibibyte ||
      target.capacityBytes % sectorBytes != 0U) {
    result.error =
        "Standalone FAT16 formatting requires a 512-byte-sector target from 16 MiB through 2 GiB";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(outputPath, fileError) || fileError) {
    result.error = fileError
                       ? "Unable to inspect the FAT16 staging path: " +
                             fileError.message()
                       : "The FAT16 staging path already exists";
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone FAT16 formatting was cancelled before staging";
    return result;
  }
  const std::uint64_t totalSectors64 = target.capacityBytes / sectorBytes;
  if (totalSectors64 <= partitionStart + reservedSectors + rootSectors ||
      totalSectors64 > std::numeric_limits<std::uint32_t>::max()) {
    result.error = "The target cannot be represented by the FAT16 MBR layout";
    return result;
  }
  const std::uint32_t totalSectors =
      static_cast<std::uint32_t>(totalSectors64);
  const std::uint32_t partitionSectors = totalSectors - partitionStart;
  std::uint32_t sectorsPerCluster = 0U;
  std::uint32_t fatSectors = 0U;
  std::uint32_t clusters = 0U;
  if (options.clusterSizeBytes != 0U &&
      (options.clusterSizeBytes < sectorBytes ||
       options.clusterSizeBytes > 32768U ||
       options.clusterSizeBytes % sectorBytes != 0U ||
       (options.clusterSizeBytes & (options.clusterSizeBytes - 1U)) != 0U)) {
    result.error =
        "FAT16 cluster size must be a power of two from 512 bytes through 32 KiB";
    return result;
  }
  const std::array<std::uint32_t, 7> automaticCandidates{
      1U, 2U, 4U, 8U, 16U, 32U, 64U};
  const std::array<std::uint32_t, 1> requestedCandidate{
      options.clusterSizeBytes / sectorBytes};
  const std::uint32_t* const candidates =
      options.clusterSizeBytes == 0U ? automaticCandidates.data()
                                    : requestedCandidate.data();
  const std::size_t candidateCount =
      options.clusterSizeBytes == 0U ? automaticCandidates.size()
                                    : requestedCandidate.size();
  for (std::size_t candidateIndex = 0U; candidateIndex < candidateCount;
       ++candidateIndex) {
    const std::uint32_t candidate = candidates[candidateIndex];
    std::uint32_t candidateFat = 1U;
    for (unsigned int iteration = 0; iteration < 16U; ++iteration) {
      const std::uint32_t overhead =
          reservedSectors + fatCopies * candidateFat + rootSectors;
      if (overhead >= partitionSectors) {
        break;
      }
      const std::uint32_t candidateClusters =
          (partitionSectors - overhead) / candidate;
      const std::uint32_t requiredFat = static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(candidateClusters + 2U) * 2U +
           sectorBytes - 1U) /
          sectorBytes);
      if (requiredFat == candidateFat) {
        if (candidateClusters >= 4085U && candidateClusters < 65525U) {
          sectorsPerCluster = candidate;
          fatSectors = candidateFat;
          clusters = candidateClusters;
        }
        break;
      }
      candidateFat = requiredFat;
    }
    if (sectorsPerCluster != 0U) {
      break;
    }
  }
  if (sectorsPerCluster == 0U) {
    result.error = "A standards-compliant FAT16 cluster layout could not be selected";
    return result;
  }
  const std::uint64_t partitionOffset =
      static_cast<std::uint64_t>(partitionStart) * sectorBytes;
  const std::uint64_t fatOffset = partitionOffset + sectorBytes;
  const std::uint64_t rootOffset =
      fatOffset + static_cast<std::uint64_t>(fatCopies) * fatSectors *
                      sectorBytes;
  const std::uint64_t initializedBytes = rootOffset +
      static_cast<std::uint64_t>(rootSectors) * sectorBytes;
  const std::uint64_t stagedBytes =
      options.quickFormat ? initializedBytes : target.capacityBytes;
  const auto parent = outputPath.parent_path().empty()
                          ? std::filesystem::current_path(fileError)
                          : outputPath.parent_path();
  const auto space = fileError ? std::filesystem::space_info{}
                               : std::filesystem::space(parent, fileError);
  if (fileError || space.available < initializedBytes) {
    result.error = fileError ? "Unable to inspect FAT16 staging space: " +
                                   fileError.message()
                             : "Not enough space to stage the FAT16 format";
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Planning, 0U, stagedBytes});
    onProgress({StandaloneMediaStage::Formatting, 0U, stagedBytes});
  }
  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create the FAT16 staging image";
    return result;
  }
  const auto cleanup = [&] {
    output.close();
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);
  };
  std::array<unsigned char, sectorBytes> mbr{};
  mbr[447] = 0xfeU;
  mbr[448] = 0xffU;
  mbr[449] = 0xffU;
  mbr[450] = 0x0eU;
  mbr[451] = 0xfeU;
  mbr[452] = 0xffU;
  mbr[453] = 0xffU;
  put32(mbr.data() + 454U, partitionStart);
  put32(mbr.data() + 458U, partitionSectors);
  mbr[510] = 0x55U;
  mbr[511] = 0xaaU;
  const auto label = volumeLabelBytes(volumeLabel);
  std::array<unsigned char, sectorBytes> boot{};
  boot[0] = 0xebU;
  boot[1] = 0x3cU;
  boot[2] = 0x90U;
  std::copy_n("RUFUS++ ", 8U, boot.begin() + 3U);
  put16(boot.data() + 11U, sectorBytes);
  boot[13] = static_cast<unsigned char>(sectorsPerCluster);
  put16(boot.data() + 14U, reservedSectors);
  boot[16] = static_cast<unsigned char>(fatCopies);
  put16(boot.data() + 17U, rootEntries);
  if (partitionSectors <= 65535U) {
    put16(boot.data() + 19U,
          static_cast<std::uint16_t>(partitionSectors));
  } else {
    put32(boot.data() + 32U, partitionSectors);
  }
  boot[21] = 0xf8U;
  put16(boot.data() + 22U, static_cast<std::uint16_t>(fatSectors));
  put16(boot.data() + 24U, 63U);
  put16(boot.data() + 26U, 255U);
  put32(boot.data() + 28U, partitionStart);
  boot[36] = 0x80U;
  boot[38] = 0x29U;
  put32(boot.data() + 39U,
        static_cast<std::uint32_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count()));
  std::copy(label.begin(), label.end(), boot.begin() + 43U);
  std::copy_n("FAT16   ", 8U, boot.begin() + 54U);
  boot[510] = 0x55U;
  boot[511] = 0xaaU;
  std::vector<unsigned char> fat(
      static_cast<std::size_t>(fatSectors) * sectorBytes, 0U);
  fat[0] = 0xf8U;
  fat[1] = 0xffU;
  fat[2] = 0xffU;
  fat[3] = 0xffU;
  std::vector<unsigned char> root(
      static_cast<std::size_t>(rootSectors) * sectorBytes, 0U);
  std::copy(label.begin(), label.end(), root.begin());
  root[11] = 0x08U;
  if (!writeAt(output, 0U, mbr.data(), mbr.size(), result.error) ||
      !writeAt(output, partitionOffset, boot.data(), boot.size(), result.error)) {
    cleanup();
    return result;
  }
  for (std::uint32_t copy = 0; copy < fatCopies; ++copy) {
    if (!writeAt(output,
                 fatOffset + static_cast<std::uint64_t>(copy) * fat.size(),
                 fat, result.error)) {
      cleanup();
      return result;
    }
  }
  if (!writeAt(output, rootOffset, root, result.error)) {
    cleanup();
    return result;
  }
  output.flush();
  output.close();
  std::filesystem::resize_file(outputPath, stagedBytes, fileError);
  if (fileError) {
    result.error = "Unable to finalize the FAT16 staging image: " +
                   fileError.message();
    cleanup();
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone FAT16 formatting was cancelled";
    cleanup();
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Verifying, 0U, stagedBytes});
  }
  std::ifstream verify(outputPath, std::ios::binary);
  std::array<unsigned char, sectorBytes> observed{};
  verify.seekg(static_cast<std::streamoff>(partitionOffset));
  verify.read(reinterpret_cast<char*>(observed.data()), observed.size());
  if (!verify || std::string_view(
                     reinterpret_cast<const char*>(observed.data() + 54U),
                     8U) != "FAT16   " ||
      !std::equal(label.begin(), label.end(), observed.begin() + 43U) ||
      clusters < 4085U || clusters >= 65525U) {
    result.error = "Standalone FAT16 metadata verification failed";
    cleanup();
    return result;
  }
  ImageInfo image;
  image.path = outputPath.string();
  image.displayName = outputPath.filename().string();
  image.volumeLabel = std::move(volumeLabel);
  image.sizeBytes = stagedBytes;
  image.expandedSizeBytes = stagedBytes;
  image.format = ImageFormat::Raw;
  image.partitionScheme = PartitionScheme::Mbr;
  image.capabilities.rawWrite = true;
  image.capabilities.validPartitionTable = true;
  result.stagedImage = std::move(image);
  result.success = true;
  if (onProgress) {
    onProgress({StandaloneMediaStage::Complete, stagedBytes, stagedBytes});
  }
  return result;
}

StandaloneMediaResult stageBlankExt2Media(
    const BlockDeviceInfo& target, const std::filesystem::path& outputPath,
    std::string volumeLabel,
    const StandaloneMediaProgressCallback& onProgress,
    const StandaloneMediaCancelCallback& isCancelled,
    const StandaloneFormatOptions options) {
  StandaloneMediaResult result;
  if (options.clusterSizeBytes != 0U) {
    result.error = "The portable ext2 formatter does not expose allocation-unit selection";
    return result;
  }
  if (outputPath.empty() || target.logicalSectorSize != 512U ||
      target.capacityBytes < 258ULL * 1024ULL * 1024ULL ||
      target.capacityBytes % target.logicalSectorSize != 0U) {
    result.error =
        "Standalone ext2 formatting requires a 512-byte-sector target of at least 258 MiB";
    return result;
  }
  const std::uint64_t totalSectors =
      target.capacityBytes / target.logicalSectorSize;
  constexpr std::uint32_t partitionStart = 2048U;
  if (totalSectors <= partitionStart ||
      totalSectors - partitionStart >
          std::numeric_limits<std::uint32_t>::max()) {
    result.error =
        "The portable MBR/ext2 formatter supports targets smaller than 2 TiB";
    return result;
  }
  std::error_code fileError;
  if (std::filesystem::exists(outputPath, fileError) || fileError) {
    result.error = fileError
                       ? "Unable to inspect the ext2 staging path: " +
                             fileError.message()
                       : "The ext2 staging path already exists";
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone ext2 formatting cancelled before staging";
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Planning, 0U, target.capacityBytes});
  }
  const std::uint32_t partitionSectors =
      static_cast<std::uint32_t>(totalSectors - partitionStart);
  const detail::Ext2FormatOptions ext2Options{
      static_cast<std::uint64_t>(partitionStart) * 512U,
      static_cast<std::uint64_t>(partitionSectors) * 512U,
      volumeLabel.substr(0U, 16U), false};
  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "Unable to create the standalone ext2 staging image";
    return result;
  }
  bool keepOutput = false;
  const auto cleanup = [&] {
    output.close();
    if (!keepOutput) {
      std::error_code ignored;
      std::filesystem::remove(outputPath, ignored);
    }
  };
  std::array<unsigned char, 512> mbr{};
  const auto nonce = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  put32(mbr.data() + 440U,
        static_cast<std::uint32_t>(nonce ^ (nonce >> 32U)));
  mbr[450] = 0x83U;
  mbr[451] = 0xfeU;
  mbr[452] = 0xffU;
  mbr[453] = 0xffU;
  put32(mbr.data() + 454U, partitionStart);
  put32(mbr.data() + 458U, partitionSectors);
  mbr[510] = 0x55U;
  mbr[511] = 0xaaU;
  if (!writeAt(output, 0U, mbr.data(), mbr.size(), result.error)) {
    cleanup();
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Formatting, 0U,
                target.capacityBytes});
  }
  if (!detail::writeExt2Filesystem(output, ext2Options, isCancelled,
                                   result.error)) {
    result.cancelled = isCancelled && isCancelled();
    cleanup();
    return result;
  }
  output.flush();
  output.close();
  std::filesystem::resize_file(outputPath, target.capacityBytes, fileError);
  if (fileError) {
    result.error = "Unable to finalize the sparse ext2 staging image: " +
                   fileError.message();
    cleanup();
    return result;
  }
  if (onProgress) {
    onProgress({StandaloneMediaStage::Verifying, 0U,
                target.capacityBytes});
  }
  std::ifstream verifyMbr(outputPath, std::ios::binary);
  std::array<unsigned char, 512> observedMbr{};
  verifyMbr.read(reinterpret_cast<char*>(observedMbr.data()),
                 static_cast<std::streamsize>(observedMbr.size()));
  if (!verifyMbr || observedMbr[450] != 0x83U ||
      observedMbr[510] != 0x55U || observedMbr[511] != 0xaaU ||
      !detail::verifyExt2Filesystem(outputPath, ext2Options, result.error)) {
    if (result.error.empty()) {
      result.error = "Standalone MBR/ext2 metadata verification failed";
    }
    cleanup();
    return result;
  }
  if (isCancelled && isCancelled()) {
    result.cancelled = true;
    result.error = "Standalone ext2 formatting cancelled after staging";
    cleanup();
    return result;
  }
  ImageInfo image;
  image.path = outputPath.u8string();
  image.displayName = outputPath.filename().u8string();
  image.volumeLabel = std::move(volumeLabel);
  image.sizeBytes = target.capacityBytes;
  image.expandedSizeBytes = target.capacityBytes;
  image.format = ImageFormat::Raw;
  image.partitionScheme = PartitionScheme::Mbr;
  image.capabilities.rawWrite = true;
  image.capabilities.validPartitionTable = true;
  result.stagedImage = std::move(image);
  result.success = true;
  keepOutput = true;
  if (onProgress) {
    onProgress({StandaloneMediaStage::Complete, target.capacityBytes,
                target.capacityBytes});
  }
  return result;
}

const char* standaloneMediaStageName(const StandaloneMediaStage stage) noexcept {
  switch (stage) {
    case StandaloneMediaStage::Planning:
      return "Planning standalone format";
    case StandaloneMediaStage::Formatting:
      return "Creating FAT32 filesystem";
    case StandaloneMediaStage::Verifying:
      return "Verifying FAT32 metadata";
    case StandaloneMediaStage::Complete:
      return "Standalone format staged";
  }
  return "Standalone format";
}

}  // namespace rufus::core
