#pragma once

#include "core/document.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace patchy::pxd {

// Pixelmator Pro packages (.pxd). The published layout is a directory (or a zip of that
// directory) with a SQLite `metadata.info` and a `data/` folder of raster tiles. Lienzo
// reads that package, keeps the layer tree, and decodes tiles that are PNG. Tiles in
// Pixelmator's private codec stay empty and are reported in `notices`. Files Lienzo
// writes are a zip with PNG tiles, or an updated directory package when the path is
// already one, so a later open round-trips the pixels.

class DocumentIo {
public:
  [[nodiscard]] static bool sniff(std::span<const std::uint8_t> bytes) noexcept;

  [[nodiscard]] static Document read(std::span<const std::uint8_t> bytes,
                                     std::vector<std::string>* notices = nullptr);

  [[nodiscard]] static Document read_file(const std::filesystem::path& path,
                                          std::vector<std::string>* notices = nullptr);

  [[nodiscard]] static std::vector<std::uint8_t> write(const Document& document);

  static void write_file(const Document& document, const std::filesystem::path& path);
};

}  // namespace patchy::pxd
