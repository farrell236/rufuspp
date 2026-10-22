/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
namespace rufus::tests {
inline void writeIsoFixture(const std::filesystem::path& path, const bool corruptBootCatalog = false,
                     const bool corruptWim = false,
                     const bool useInstallEsd = false) {
  constexpr std::uint32_t sectorSize = 2048;
  std::vector<unsigned char> bytes(28 * sectorSize, 0);

  auto writeBothEndian32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[offset + 2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[offset + 3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
    bytes[offset + 4] = static_cast<unsigned char>((value >> 24U) & 0xffU);
    bytes[offset + 5] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[offset + 6] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[offset + 7] = static_cast<unsigned char>(value & 0xffU);
  };

  auto writeBothEndian16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[offset + 2] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[offset + 3] = static_cast<unsigned char>(value & 0xffU);
  };

  auto writeLittleEndian16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
  };

  auto writeLittleEndian32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
      bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };

  auto writeLittleEndian64 = [&bytes](const std::size_t offset, const std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
      bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
    }
  };

  auto directoryRecord = [&bytes, &writeBothEndian32](
                             const std::size_t offset, const std::uint32_t extent,
                             const std::uint32_t size, const unsigned char flags,
                             const std::vector<unsigned char>& name) {
    const std::size_t padding = name.size() % 2U == 0 ? 1U : 0U;
    const std::size_t length = 33U + name.size() + padding;
    bytes[offset] = static_cast<unsigned char>(length);
    writeBothEndian32(offset + 2, extent);
    writeBothEndian32(offset + 10, size);
    bytes[offset + 25] = flags;
    bytes[offset + 28] = 1;
    bytes[offset + 31] = 1;
    bytes[offset + 32] = static_cast<unsigned char>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33));
    return length;
  };

  auto descriptor = [&](const std::size_t sector, const unsigned char type) {
    const std::size_t offset = sector * sectorSize;
    bytes[offset] = type;
    bytes[offset + 1] = 'C';
    bytes[offset + 2] = 'D';
    bytes[offset + 3] = '0';
    bytes[offset + 4] = '0';
    bytes[offset + 5] = '1';
    bytes[offset + 6] = 1;
    return offset;
  };

  const std::size_t boot = descriptor(16, 0);
  const std::string bootSystem = "EL TORITO SPECIFICATION";
  std::copy(bootSystem.begin(), bootSystem.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(boot + 7));
  bytes[boot + 71] = 26;
  const std::size_t primary = descriptor(17, 1);
  const std::string label = "RUFUSPP_QT_TEST";
  std::copy(label.begin(), label.end(), bytes.begin() + static_cast<std::ptrdiff_t>(primary + 40));
  writeBothEndian16(primary + 128, sectorSize);
  directoryRecord(primary + 156, 19, sectorSize, 0x02, {0});
  descriptor(18, 255);

  auto asciiName = [](const std::string& value) {
    return std::vector<unsigned char>(value.begin(), value.end());
  };
  auto writeDirectory = [&](
                            const std::size_t sector, const std::size_t parentSector,
                            const std::vector<std::tuple<std::string, std::uint32_t,
                                                         std::uint32_t, unsigned char>>& entries) {
    std::size_t offset = sector * sectorSize;
    offset += directoryRecord(offset, static_cast<std::uint32_t>(sector), sectorSize, 0x02, {0});
    offset += directoryRecord(offset, static_cast<std::uint32_t>(parentSector), sectorSize,
                              0x02, {1});
    for (const auto& [name, extent, size, flags] : entries) {
      offset += directoryRecord(offset, extent, size, flags, asciiName(name));
    }
  };

  writeDirectory(19, 19,
                 {{"BOOTMGR;1", 20, 4, 0},
                  {"SOURCES", 21, sectorSize, 0x02},
                  {"EFI", 23, sectorSize, 0x02}});
  const std::string wimXml =
      "<WIM><IMAGE INDEX=\"1\"><NAME>Windows Test</NAME>"
      "<DISPLAYNAME>Windows 11 Test &amp; Tools</DISPLAYNAME>"
      "<DESCRIPTION>Test edition metadata</DESCRIPTION><TOTALBYTES>7516192768</TOTALBYTES>"
      "<WINDOWS><ARCH>9</ARCH>"
      "<VERSION><MAJOR>10</MAJOR><MINOR>0</MINOR><BUILD>22621</BUILD></VERSION>"
      "</WINDOWS></IMAGE></WIM>";
  const std::uint32_t wimSize =
      static_cast<std::uint32_t>(208U + 2U + wimXml.size() * 2U);
  writeDirectory(21, 19,
                 {{useInstallEsd ? "INSTALL.ESD;1" : "INSTALL.WIM;1",
                   22, wimSize, 0}});
  writeDirectory(23, 19, {{"BOOT", 24, sectorSize, 0x02}});
  writeDirectory(24, 23, {{"BOOTX64.EFI;1", 25, 4, 0}});
  for (const std::size_t sector : {20U, 25U}) {
    bytes[sector * sectorSize] = 't';
    bytes[sector * sectorSize + 1] = 'e';
    bytes[sector * sectorSize + 2] = 's';
    bytes[sector * sectorSize + 3] = 't';
  }

  const std::size_t wim = 22 * sectorSize;
  const std::string wimMagic{"MSWIM\0\0\0", 8};
  std::copy(wimMagic.begin(), wimMagic.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(wim));
  writeLittleEndian32(wim + 8, 208);
  writeLittleEndian32(wim + 12, 0x00010d00U);
  writeLittleEndian16(wim + 40, 1);
  writeLittleEndian16(wim + 42, 1);
  writeLittleEndian32(wim + 44, 1);
  const std::uint64_t xmlSize = 2U + wimXml.size() * 2U;
  for (unsigned int index = 0; index < 7; ++index) {
    bytes[wim + 72 + index] =
        static_cast<unsigned char>((xmlSize >> (index * 8U)) & 0xffU);
  }
  bytes[wim + 79] = 0x02;
  writeLittleEndian64(wim + 80, 208);
  writeLittleEndian64(wim + 88, xmlSize);
  writeLittleEndian32(wim + 120, 1);
  bytes[wim + 208] = 0xff;
  bytes[wim + 209] = 0xfe;
  for (std::size_t index = 0; index < wimXml.size(); ++index) {
    bytes[wim + 210 + index * 2U] = static_cast<unsigned char>(wimXml[index]);
  }
  if (corruptWim) {
    bytes[wim] = 'X';
  }

  const std::size_t catalog = 26 * sectorSize;
  bytes[catalog] = 1;
  bytes[catalog + 28] = 0xaa;
  bytes[catalog + 29] = 0x55;
  bytes[catalog + 30] = 0x55;
  bytes[catalog + 31] = 0xaa;
  bytes[catalog + 32] = 0x88;
  bytes[catalog + 38] = 4;
  bytes[catalog + 40] = 27;
  bytes[27 * sectorSize] = 0xeb;
  if (corruptBootCatalog) {
    bytes[catalog + 28] ^= 1U;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("Unable to create ISO fixture");
}

}  // namespace rufus::tests
