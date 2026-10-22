/*
 * Rufus++: Cross-platform boot-media utility
 * Copyright (c) 2026 Rufus++ contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ext2_formatter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace rufus::core::detail {

namespace {

constexpr std::uint32_t kBlockBytes = 4096;
constexpr std::uint32_t kBlocksPerGroup = kBlockBytes * 8U;
constexpr std::uint32_t kInodesPerGroup = 8192;
constexpr std::uint16_t kInodeBytes = 256;
constexpr std::uint32_t kInodeTableBlocks =
    kInodesPerGroup * kInodeBytes / kBlockBytes;
constexpr std::uint32_t kRootInode = 2;
constexpr std::uint32_t kFirstRegularInode = 11;
constexpr std::uint16_t kExt2Magic = 0xef53;
constexpr std::string_view kPersistenceConfName = "persistence.conf";
constexpr std::string_view kPersistenceConfData = "/ union\n";

void put16(unsigned char* output, const std::uint16_t value) {
  output[0] = static_cast<unsigned char>(value & 0xffU);
  output[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put32(unsigned char* output, const std::uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index) {
    output[index] = static_cast<unsigned char>((value >> (8U * index)) & 0xffU);
  }
}

std::uint16_t get16(const unsigned char* input) {
  return static_cast<std::uint16_t>(input[0]) |
         static_cast<std::uint16_t>(input[1]) << 8U;
}

std::uint32_t get32(const unsigned char* input) {
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (8U * index);
  }
  return value;
}

bool writeAt(std::ofstream& output, const std::uint64_t offset,
             const unsigned char* bytes, const std::size_t size,
             std::string& error) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    error = "The ext2 filesystem exceeds the host file API limit";
    return false;
  }
  output.seekp(static_cast<std::streamoff>(offset));
  output.write(reinterpret_cast<const char*>(bytes),
               static_cast<std::streamsize>(size));
  if (!output) {
    error = "Unable to write the Linux persistence filesystem";
    return false;
  }
  return true;
}

bool writeAt(std::ofstream& output, const std::uint64_t offset,
             const std::vector<unsigned char>& bytes, std::string& error) {
  return writeAt(output, offset, bytes.data(), bytes.size(), error);
}

bool readAt(std::ifstream& input, const std::uint64_t offset,
            unsigned char* bytes, const std::size_t size) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset));
  input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
  return static_cast<bool>(input);
}

void setBit(std::vector<unsigned char>& bitmap, const std::uint32_t bit) {
  bitmap[bit / 8U] |= static_cast<unsigned char>(1U << (bit % 8U));
}

bool isPowerOf(std::uint32_t value, const std::uint32_t base) {
  if (value < 1U) {
    return false;
  }
  while (value % base == 0U) {
    value /= base;
  }
  return value == 1U;
}

bool hasSparseSuperblock(const std::uint32_t group) {
  return group == 0U || group == 1U || isPowerOf(group, 3U) ||
         isPowerOf(group, 5U) || isPowerOf(group, 7U);
}

std::uint32_t unixTimeNow() {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return seconds <= 0 ? 1U : static_cast<std::uint32_t>(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(seconds),
                              std::numeric_limits<std::uint32_t>::max()));
}

std::array<unsigned char, 16> makeUuid() {
  std::array<unsigned char, 16> uuid{};
  std::random_device random;
  for (auto& byte : uuid) {
    byte = static_cast<unsigned char>(random());
  }
  uuid[6] = static_cast<unsigned char>((uuid[6] & 0x0fU) | 0x40U);
  uuid[8] = static_cast<unsigned char>((uuid[8] & 0x3fU) | 0x80U);
  return uuid;
}

struct GroupLayout final {
  std::uint32_t firstBlock{};
  std::uint32_t blockCount{};
  std::uint32_t blockBitmap{};
  std::uint32_t inodeBitmap{};
  std::uint32_t inodeTable{};
  std::uint32_t firstDataBlock{};
  std::uint16_t freeBlocks{};
  std::uint16_t freeInodes{};
  bool backupSuperblock{};
};

struct InitialNode final {
  std::string name;
  std::size_t parent{};
  std::uint32_t inode{};
  std::uint16_t mode{};
  std::string contents;
  bool directory() const { return (mode & 0170000U) == 0040000U; }
};

std::string uuidText(const std::array<unsigned char, 16>& uuid) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) result += '-';
    result += hex[uuid[index] >> 4U];
    result += hex[uuid[index] & 15U];
  }
  return result;
}

std::string casperShutdownScript(const std::array<unsigned char, 16>& uuid) {
  // Only the generated persistence filesystem can be selected. Duplicate UUIDs
  // fail closed; an installed OS or non-persistent live boot does nothing.
  return std::string(
      "#!/bin/sh\n"
      "# Rufus++ Casper persistence finalization. No filesystem repair is performed.\n"
      "set -eu\n"
      "export PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
      "case \"$1\" in poweroff|halt|reboot|kexec) ;; *) exit 0;; esac\n"
      "grep -Eq '(^|[[:space:]])persistent([[:space:]]|$)' /proc/cmdline || exit 0\n"
      "[ \"$(findmnt -n -o FSTYPE /)\" = overlay ] || exit 0\n"
      "uuid='") + uuidText(uuid) +
      "'\n"
      "device=$(blkid -c /dev/null -t \"UUID=$uuid\" -o device)\n"
      "case \"$device\" in ''|*'\n'*) exit 1;; esac\n"
      "[ -b \"$device\" ]\n"
      "[ \"$(blkid -s UUID -o value \"$device\")\" = \"$uuid\" ]\n"
      "[ \"$(blkid -s TYPE -o value \"$device\")\" = ext2 ]\n"
      "point=/run/rufuspp-persistence-finalize\n"
      "umask 077\n"
      "mkdir \"$point\"\n"
      "mounted=0\n"
      "trap 'if [ \"$mounted\" = 1 ]; then umount \"$point\" || :; fi; rmdir \"$point\" || :' EXIT\n"
      "mount -t ext2 -o rw,nodev,nosuid \"$device\" \"$point\"\n"
      "mounted=1\n"
      "sync\n"
      "mount -o remount,ro \"$point\"\n"
      "umount \"$point\"\n"
      "mounted=0\n";
}

std::vector<InitialNode> initialNodes(
    const Ext2FormatOptions& options, const std::array<unsigned char, 16>& uuid) {
  std::vector<InitialNode> nodes{{"", 0U, kRootInode, 0040755U, ""}};
  const auto add = [&](const std::string& name, const std::size_t parent,
                       const std::uint16_t mode, std::string contents) {
    nodes.push_back({name, parent,
                     kFirstRegularInode + static_cast<std::uint32_t>(nodes.size() - 1U),
                     mode, std::move(contents)});
    return nodes.size() - 1U;
  };
  if (options.createPersistenceConf) {
    add(std::string(kPersistenceConfName), 0U, 0100644U,
        std::string(kPersistenceConfData));
  }
  if (options.casperShutdownHook) {
    std::size_t parent = 0U;
    for (const char* name : {"upper", "usr", "lib", "systemd", "system-shutdown"}) {
      parent = add(name, parent, 0040755U, "");
    }
    add("90-rufuspp-persistence", parent, 0100755U, casperShutdownScript(uuid));
  }
  return nodes;
}

bool makeGroups(const std::uint32_t requestedBlocks,
                std::uint32_t& filesystemBlocks,
                std::uint32_t& descriptorBlocks,
                std::vector<GroupLayout>& groups,
                const std::uint32_t initialNodeCount, std::string& error) {
  filesystemBlocks = requestedBlocks;
  for (;;) {
    const std::uint32_t groupCount =
        (filesystemBlocks + kBlocksPerGroup - 1U) / kBlocksPerGroup;
    descriptorBlocks = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(groupCount) * 32U + kBlockBytes - 1U) /
        kBlockBytes);
    if (groupCount == 0U || descriptorBlocks == 0U) {
      error = "The Linux persistence partition is too small";
      return false;
    }
    const std::uint32_t lastFirst = (groupCount - 1U) * kBlocksPerGroup;
    const std::uint32_t lastBlocks = filesystemBlocks - lastFirst;
    const bool lastHasSuper = hasSparseSuperblock(groupCount - 1U);
    const std::uint32_t lastMetadata = (lastHasSuper ? 1U + descriptorBlocks : 0U) +
                                       2U + kInodeTableBlocks;
    if (lastBlocks > lastMetadata + 8U || groupCount == 1U) {
      break;
    }
    filesystemBlocks = lastFirst;
  }

  const std::uint32_t groupCount =
      (filesystemBlocks + kBlocksPerGroup - 1U) / kBlocksPerGroup;
  groups.clear();
  groups.reserve(groupCount);
  for (std::uint32_t group = 0; group < groupCount; ++group) {
    GroupLayout layout;
    layout.firstBlock = group * kBlocksPerGroup;
    layout.blockCount = std::min(kBlocksPerGroup, filesystemBlocks - layout.firstBlock);
    layout.backupSuperblock = hasSparseSuperblock(group);
    std::uint32_t cursor = layout.firstBlock;
    if (layout.backupSuperblock) {
      cursor += 1U + descriptorBlocks;
    }
    layout.blockBitmap = cursor++;
    layout.inodeBitmap = cursor++;
    layout.inodeTable = cursor;
    cursor += kInodeTableBlocks;
    layout.firstDataBlock = cursor;
    std::uint32_t usedBlocks = cursor - layout.firstBlock;
    if (group == 0U) {
      usedBlocks += initialNodeCount;
    }
    if (usedBlocks >= layout.blockCount ||
        layout.blockCount - usedBlocks > std::numeric_limits<std::uint16_t>::max()) {
      error = "The Linux persistence filesystem group layout is invalid";
      return false;
    }
    layout.freeBlocks = static_cast<std::uint16_t>(layout.blockCount - usedBlocks);
    const std::uint32_t usedInodes = group == 0U
                                         ? (kFirstRegularInode - 1U) + initialNodeCount - 1U
                                         : 0U;
    layout.freeInodes = static_cast<std::uint16_t>(kInodesPerGroup - usedInodes);
    groups.push_back(layout);
  }
  return true;
}

std::vector<unsigned char> makeSuperblock(
    const std::uint32_t filesystemBlocks, const std::uint32_t freeBlocks,
    const std::uint32_t groupCount, const std::uint32_t freeInodes,
    const std::uint16_t blockGroup, const std::string& label,
    const std::array<unsigned char, 16>& uuid, const std::uint32_t timestamp) {
  std::vector<unsigned char> super(1024, 0);
  put32(super.data(), groupCount * kInodesPerGroup);
  put32(super.data() + 4, filesystemBlocks);
  put32(super.data() + 8, filesystemBlocks / 20U);
  put32(super.data() + 12, freeBlocks);
  put32(super.data() + 16, freeInodes);
  put32(super.data() + 20, 0);
  put32(super.data() + 24, 2);
  put32(super.data() + 28, 2);
  put32(super.data() + 32, kBlocksPerGroup);
  put32(super.data() + 36, kBlocksPerGroup);
  put32(super.data() + 40, kInodesPerGroup);
  put32(super.data() + 48, timestamp);
  put16(super.data() + 52, 0);
  put16(super.data() + 54, 20);
  put16(super.data() + 56, kExt2Magic);
  put16(super.data() + 58, 1);
  put16(super.data() + 60, 1);
  put16(super.data() + 62, 0);
  put32(super.data() + 64, timestamp);
  put32(super.data() + 68, 0);
  put32(super.data() + 72, 0);
  put32(super.data() + 76, 1);
  put16(super.data() + 80, 0);
  put16(super.data() + 82, 0);
  put32(super.data() + 84, kFirstRegularInode);
  put16(super.data() + 88, kInodeBytes);
  put16(super.data() + 90, blockGroup);
  put32(super.data() + 92, 0);
  put32(super.data() + 96, 0x00000002U);
  put32(super.data() + 100, 0x00000003U);
  std::copy(uuid.begin(), uuid.end(), super.begin() + 104);
  const std::size_t labelBytes = std::min<std::size_t>(16, label.size());
  std::copy_n(label.begin(), labelBytes, super.begin() + 120);
  super[252] = 1;
  put16(super.data() + 254, 32);
  put32(super.data() + 264, timestamp);
  return super;
}

std::vector<unsigned char> makeGroupDescriptors(
    const std::vector<GroupLayout>& groups, const std::uint32_t descriptorBlocks,
    const std::uint16_t initialDirectories) {
  std::vector<unsigned char> descriptors(
      static_cast<std::size_t>(descriptorBlocks) * kBlockBytes, 0);
  for (std::size_t index = 0; index < groups.size(); ++index) {
    unsigned char* descriptor = descriptors.data() + index * 32U;
    put32(descriptor, groups[index].blockBitmap);
    put32(descriptor + 4, groups[index].inodeBitmap);
    put32(descriptor + 8, groups[index].inodeTable);
    put16(descriptor + 12, groups[index].freeBlocks);
    put16(descriptor + 14, groups[index].freeInodes);
    put16(descriptor + 16, index == 0U ? initialDirectories : 0U);
  }
  return descriptors;
}

std::vector<unsigned char> makeInode(const std::uint16_t mode,
                                     const std::uint32_t size,
                                     const std::uint16_t links,
                                     const std::uint32_t dataBlock,
                                     const std::uint32_t timestamp) {
  std::vector<unsigned char> inode(kInodeBytes, 0);
  put16(inode.data(), mode);
  put32(inode.data() + 4, size);
  put32(inode.data() + 8, timestamp);
  put32(inode.data() + 12, timestamp);
  put32(inode.data() + 16, timestamp);
  put16(inode.data() + 26, links);
  put32(inode.data() + 28, kBlockBytes / 512U);
  put32(inode.data() + 40, dataBlock);
  return inode;
}

void writeDirectoryEntry(unsigned char* entry, const std::uint32_t inode,
                         const std::uint16_t recordLength,
                         const std::string_view name, const unsigned char type) {
  put32(entry, inode);
  put16(entry + 4, recordLength);
  entry[6] = static_cast<unsigned char>(name.size());
  entry[7] = type;
  std::copy(name.begin(), name.end(), entry + 8);
}

std::vector<unsigned char> nodeData(const std::vector<InitialNode>& nodes,
                                    const std::size_t index) {
  const auto& node = nodes[index];
  std::vector<unsigned char> data(kBlockBytes, 0);
  if (!node.directory()) {
    std::copy(node.contents.begin(), node.contents.end(), data.begin());
    return data;
  }
  struct Entry { std::uint32_t inode; std::string name; unsigned char type; };
  std::vector<Entry> entries{{node.inode, ".", 2},
                             {nodes[node.parent].inode, "..", 2}};
  for (std::size_t child = 1U; child < nodes.size(); ++child) {
    if (nodes[child].parent == index) {
      entries.push_back({nodes[child].inode, nodes[child].name,
                         static_cast<unsigned char>(nodes[child].directory() ? 2U : 1U)});
    }
  }
  std::size_t offset = 0U;
  for (std::size_t entry = 0U; entry < entries.size(); ++entry) {
    const auto& value = entries[entry];
    const auto bytes = static_cast<std::uint16_t>(
        entry + 1U == entries.size() ? kBlockBytes - offset
                                     : (8U + value.name.size() + 3U) & ~3U);
    writeDirectoryEntry(data.data() + offset, value.inode, bytes, value.name, value.type);
    offset += bytes;
  }
  return data;
}

std::vector<unsigned char> nodeInode(const std::vector<InitialNode>& nodes,
                                     const std::size_t index,
                                     const std::uint32_t dataBlock,
                                     const std::uint32_t timestamp) {
  const auto& node = nodes[index];
  std::uint16_t links = node.directory() ? 2U : 1U;
  if (node.directory()) {
    for (std::size_t child = 1U; child < nodes.size(); ++child) {
      if (nodes[child].parent == index && nodes[child].directory()) ++links;
    }
  }
  return makeInode(node.mode, node.directory() ? kBlockBytes
                    : static_cast<std::uint32_t>(node.contents.size()),
                   links, dataBlock, timestamp);
}

}  // namespace

bool writeExt2Filesystem(std::ofstream& output, const Ext2FormatOptions& options,
                         const Ext2CancelCallback& isCancelled,
                         std::string& error,
                         std::vector<RawWriteRange>* initializedRanges) {
  if (options.offsetBytes % kBlockBytes != 0U ||
      options.sizeBytes < 256ULL * 1024ULL * 1024ULL) {
    error = "Linux persistence requires a 4 KiB-aligned partition of at least 256 MiB";
    return false;
  }
  const std::uint64_t requestedBlocks64 = options.sizeBytes / kBlockBytes;
  if (requestedBlocks64 > std::numeric_limits<std::uint32_t>::max()) {
    error = "The Linux persistence partition exceeds the ext2 block-count limit";
    return false;
  }

  std::uint32_t filesystemBlocks = 0;
  std::uint32_t descriptorBlocks = 0;
  std::vector<GroupLayout> groups;
  const auto uuid = makeUuid();
  const auto nodes = initialNodes(options, uuid);
  for (const auto& node : nodes) {
    if (node.contents.size() > kBlockBytes) {
      error = "Initial persistence file exceeds the formatter block limit";
      return false;
    }
  }
  if (!makeGroups(static_cast<std::uint32_t>(requestedBlocks64), filesystemBlocks,
                  descriptorBlocks, groups, static_cast<std::uint32_t>(nodes.size()), error)) {
    return false;
  }
  std::uint64_t freeBlocks64 = 0;
  std::uint64_t freeInodes64 = 0;
  for (const auto& group : groups) {
    freeBlocks64 += group.freeBlocks;
    freeInodes64 += group.freeInodes;
  }
  if (freeBlocks64 > std::numeric_limits<std::uint32_t>::max() ||
      freeInodes64 > std::numeric_limits<std::uint32_t>::max()) {
    error = "The Linux persistence filesystem counters overflow";
    return false;
  }

  const std::uint32_t timestamp = unixTimeNow();
  const auto initialDirectories = static_cast<std::uint16_t>(std::count_if(
      nodes.begin(), nodes.end(), [](const InitialNode& node) { return node.directory(); }));
  const auto descriptors = makeGroupDescriptors(groups, descriptorBlocks, initialDirectories);
  for (std::uint32_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
    if (isCancelled && isCancelled()) {
      error = "Linux persistence formatting was cancelled";
      return false;
    }
    const auto& group = groups[groupIndex];
    if (group.backupSuperblock) {
      const auto super = makeSuperblock(
          filesystemBlocks, static_cast<std::uint32_t>(freeBlocks64),
          static_cast<std::uint32_t>(groups.size()),
          static_cast<std::uint32_t>(freeInodes64),
          static_cast<std::uint16_t>(groupIndex), options.volumeLabel, uuid, timestamp);
      const std::uint64_t superOffset = options.offsetBytes +
          (groupIndex == 0U ? 1024ULL
                            : static_cast<std::uint64_t>(group.firstBlock) * kBlockBytes);
      if (!writeAt(output, superOffset, super, error) ||
          !writeAt(output, options.offsetBytes +
                               static_cast<std::uint64_t>(group.firstBlock + 1U) *
                                   kBlockBytes,
                   descriptors, error)) {
        return false;
      }
    }

    std::vector<unsigned char> blockBitmap(kBlockBytes, 0);
    for (std::uint32_t block = 0;
         block < group.firstDataBlock - group.firstBlock; ++block) {
      setBit(blockBitmap, block);
    }
    if (groupIndex == 0U) {
      for (std::size_t index = 0U; index < nodes.size(); ++index) {
        setBit(blockBitmap, group.firstDataBlock - group.firstBlock +
                                static_cast<std::uint32_t>(index));
      }
    }
    for (std::uint32_t block = group.blockCount; block < kBlocksPerGroup; ++block) {
      setBit(blockBitmap, block);
    }
    if (!writeAt(output, options.offsetBytes +
                             static_cast<std::uint64_t>(group.blockBitmap) * kBlockBytes,
                 blockBitmap, error)) {
      return false;
    }

    std::vector<unsigned char> inodeBitmap(kBlockBytes, 0);
    if (groupIndex == 0U) {
      const std::uint32_t used = (kFirstRegularInode - 1U) +
                                 static_cast<std::uint32_t>(nodes.size() - 1U);
      for (std::uint32_t inode = 0; inode < used; ++inode) {
        setBit(inodeBitmap, inode);
      }
    }
    for (std::uint32_t inode = kInodesPerGroup;
         inode < kBlockBytes * 8U; ++inode) {
      setBit(inodeBitmap, inode);
    }
    if (!writeAt(output, options.offsetBytes +
                             static_cast<std::uint64_t>(group.inodeBitmap) * kBlockBytes,
                 inodeBitmap, error)) {
      return false;
    }
  }

  const auto& first = groups.front();
  const std::uint64_t inodeTableOffset = options.offsetBytes +
      static_cast<std::uint64_t>(first.inodeTable) * kBlockBytes;
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    const auto block = first.firstDataBlock + static_cast<std::uint32_t>(index);
    const auto inode = nodeInode(nodes, index, block, timestamp);
    const auto data = nodeData(nodes, index);
    if (!writeAt(output, inodeTableOffset +
                             static_cast<std::uint64_t>(nodes[index].inode - 1U) * kInodeBytes,
                 inode, error) ||
        !writeAt(output, options.offsetBytes +
                             static_cast<std::uint64_t>(block) * kBlockBytes,
                 data, error)) {
      return false;
    }
  }
  if (initializedRanges != nullptr) {
    // Include whole metadata prefixes, including all zero-filled inode tables
    // and reserved blocks that exist as holes in the fresh staging file.
    // Skipping merely zero-valued writes would retain stale allocated metadata.
    for (std::size_t index = 0U; index < groups.size(); ++index) {
      const auto& group = groups[index];
      const auto initializedBlocks = group.firstDataBlock - group.firstBlock +
          (index == 0U ? static_cast<std::uint32_t>(nodes.size()) : 0U);
      initializedRanges->push_back({
          options.offsetBytes + static_cast<std::uint64_t>(group.firstBlock) * kBlockBytes,
          static_cast<std::uint64_t>(initializedBlocks) * kBlockBytes});
    }
  }
  return true;
}

bool verifyExt2Filesystem(const std::filesystem::path& imagePath,
                          const Ext2FormatOptions& options, std::string& error) {
  std::ifstream input(imagePath, std::ios::binary);
  if (!input) {
    error = "Unable to reopen the Linux persistence filesystem for verification";
    return false;
  }
  std::array<unsigned char, 1024> super{};
  if (!readAt(input, options.offsetBytes + 1024U, super.data(), super.size()) ||
      get16(super.data() + 56) != kExt2Magic || get32(super.data() + 24) != 2U ||
      get32(super.data() + 32) != kBlocksPerGroup ||
      get16(super.data() + 88) != kInodeBytes ||
      get16(super.data() + 52) != 0U || get16(super.data() + 54) != 20U ||
      get16(super.data() + 58) != 1U ||
      get32(super.data() + 48) != get32(super.data() + 64)) {
    error = "Linux persistence superblock verification failed";
    return false;
  }
  std::string observedLabel(reinterpret_cast<const char*>(super.data() + 120), 16);
  observedLabel.erase(std::find(observedLabel.begin(), observedLabel.end(), '\0'),
                      observedLabel.end());
  if (observedLabel != options.volumeLabel.substr(0, 16)) {
    error = "Linux persistence volume label verification failed";
    return false;
  }

  std::array<unsigned char, 16> uuid{};
  std::copy_n(super.begin() + 104U, uuid.size(), uuid.begin());
  const auto nodes = initialNodes(options, uuid);
  std::uint32_t filesystemBlocks = 0U, descriptorBlocks = 0U;
  std::vector<GroupLayout> groups;
  if (options.sizeBytes / kBlockBytes > std::numeric_limits<std::uint32_t>::max() ||
      !makeGroups(static_cast<std::uint32_t>(options.sizeBytes / kBlockBytes),
                  filesystemBlocks, descriptorBlocks, groups,
                  static_cast<std::uint32_t>(nodes.size()), error)) return false;
  std::uint64_t freeBlocks = 0U, freeInodes = 0U;
  for (const auto& group : groups) {
    freeBlocks += group.freeBlocks;
    freeInodes += group.freeInodes;
  }
  const auto initialDirectories = static_cast<std::uint16_t>(std::count_if(
      nodes.begin(), nodes.end(), [](const InitialNode& node) { return node.directory(); }));
  const auto descriptors = makeGroupDescriptors(groups, descriptorBlocks, initialDirectories);
  std::vector<unsigned char> observedDescriptors(descriptors.size());
  if (get32(super.data() + 4U) != filesystemBlocks ||
      get32(super.data() + 12U) != freeBlocks || get32(super.data() + 16U) != freeInodes ||
      !readAt(input, options.offsetBytes + kBlockBytes,
              observedDescriptors.data(), observedDescriptors.size()) ||
      observedDescriptors != descriptors) {
    error = "Linux persistence allocation counter/descriptor verification failed";
    return false;
  }
  const auto& first = groups.front();
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    const auto block = first.firstDataBlock + static_cast<std::uint32_t>(index);
    const auto inode = nodeInode(nodes, index, block, get32(super.data() + 48U));
    const auto data = nodeData(nodes, index);
    std::vector<unsigned char> observedInode(inode.size()), observedData(data.size());
    if (!readAt(input, options.offsetBytes +
                           static_cast<std::uint64_t>(first.inodeTable) * kBlockBytes +
                           static_cast<std::uint64_t>(nodes[index].inode - 1U) * kInodeBytes,
                observedInode.data(), observedInode.size()) ||
        observedInode != inode ||
        !readAt(input, options.offsetBytes + static_cast<std::uint64_t>(block) * kBlockBytes,
                observedData.data(), observedData.size()) || observedData != data) {
      error = "Linux persistence initial file/directory verification failed: " + nodes[index].name;
      return false;
    }
  }
  return true;
}

}  // namespace rufus::core::detail
