#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#include "formats/pxd_document_io.hpp"

#include "formats/format_file_io.hpp"
#include "formats/miniz/miniz.h"
#include "formats/pdf_png_writer.hpp"
#include "formats/stb/stb_image.h"
#include "support/path_utils.hpp"

#include "lcms2.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(PATCHY_HAS_SQLITE)
#include <sqlite3.h>
#endif

namespace patchy::pxd {

namespace {

constexpr char kBlobMagic[4] = {'4', '-', 't', 'P'};

enum LayerFlag : std::uint64_t {
  kVisible = 1ull << 0,
  kLocked = 1ull << 1,
  kClipping = 1ull << 3,
  kMask = 1ull << 4,
  kRaster = 1ull << 6,
  kOriginalContent = 1ull << 9,
};

struct RawLayer {
  int id{0};
  int type{1};
  int index{0};
  std::string uuid;
  std::string parent;
  bool top_level{false};
  std::string name{"Layer"};
  int opacity{100};
  std::uint64_t flags{kVisible | kRaster};
  std::string blend{"norm"};
  double center_x{0.0};
  double center_y{0.0};
  double width{0.0};
  double height{0.0};
  // Points in the file are pixels divided by this. Pixelmator retina documents use 2.
  // A missing value is 2 so older Lienzo files, which stored half-pixel points, still line up.
  double backing_scale{2.0};
  bool has_geometry{false};
  bool canvas_fill{false};
  std::uint8_t fill_r{0};
  std::uint8_t fill_g{0};
  std::uint8_t fill_b{0};
  std::uint8_t fill_a{255};
  std::vector<std::uint8_t> encoded;
};

void append_le(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
}

void append_be_f64(std::vector<std::uint8_t>& out, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>(bits >> shift));
  }
}

std::uint32_t read_le_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_le_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  }
  return value;
}

std::int64_t read_le_i64(const std::uint8_t* data) {
  return static_cast<std::int64_t>(read_le_u64(data));
}

double read_be_f64(const std::uint8_t* data) {
  std::uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) {
    bits = (bits << 8) | data[i];
  }
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::string reversed_tag(std::string_view tag) {
  std::string out(tag);
  if (out.size() < 4) {
    out.append(4 - out.size(), ' ');
  }
  std::reverse(out.begin(), out.begin() + 4);
  return out.substr(0, 4);
}

std::vector<std::uint8_t> make_blob(std::string_view tag, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> blob;
  blob.insert(blob.end(), kBlobMagic, kBlobMagic + 4);
  const auto type = reversed_tag(tag);
  blob.insert(blob.end(), type.begin(), type.end());
  append_le(blob, payload.size(), 4);
  blob.insert(blob.end(), payload.begin(), payload.end());
  return blob;
}

std::vector<std::uint8_t> blob_strn(std::string_view text) {
  std::vector<std::uint8_t> payload;
  append_le(payload, text.size(), 4);
  payload.insert(payload.end(), text.begin(), text.end());
  while (payload.size() % 4 != 0) {
    payload.push_back(0);
  }
  return make_blob("Strn", payload);
}

std::vector<std::uint8_t> blob_u16(std::string_view tag, std::uint16_t value) {
  std::vector<std::uint8_t> payload;
  append_le(payload, value, 2);
  return make_blob(tag, payload);
}

std::vector<std::uint8_t> blob_f64(std::string_view tag, double value) {
  std::vector<std::uint8_t> payload;
  append_be_f64(payload, value);
  return make_blob(tag, payload);
}

std::vector<std::uint8_t> blob_u64(std::uint64_t value) {
  std::vector<std::uint8_t> payload;
  append_le(payload, value, 8);
  return make_blob("UI64", payload);
}

std::vector<std::uint8_t> blob_bdsz(std::int64_t width, std::int64_t height) {
  std::vector<std::uint8_t> payload;
  append_le(payload, static_cast<std::uint64_t>(width), 8);
  append_le(payload, static_cast<std::uint64_t>(height), 8);
  return make_blob("BDSz", payload);
}

std::vector<std::uint8_t> blob_point(std::string_view tag, double x, double y) {
  std::vector<std::uint8_t> payload;
  append_be_f64(payload, x);
  append_be_f64(payload, y);
  return make_blob(tag, payload);
}

std::vector<std::uint8_t> blob_blend(std::string code) {
  if (code.size() < 4) {
    code.append(4 - code.size(), ' ');
  }
  code.resize(4);
  std::reverse(code.begin(), code.end());
  return make_blob("Blnd", std::vector<std::uint8_t>(code.begin(), code.end()));
}

struct ParsedBlob {
  std::string tag;
  std::vector<std::uint8_t> payload;
};

std::optional<ParsedBlob> parse_blob(const std::uint8_t* data, int size) {
  if (data == nullptr || size < 12 || std::memcmp(data, kBlobMagic, 4) != 0) {
    return std::nullopt;
  }
  std::string tag(reinterpret_cast<const char*>(data + 4), 4);
  std::reverse(tag.begin(), tag.end());
  const auto length = static_cast<int>(read_le_u32(data + 8));
  if (length < 0 || 12 + length > size) {
    return std::nullopt;
  }
  return ParsedBlob{tag, std::vector<std::uint8_t>(data + 12, data + 12 + length)};
}

std::optional<std::string> blob_as_string(const ParsedBlob& blob) {
  if (blob.tag != "Strn" || blob.payload.size() < 4) {
    return std::nullopt;
  }
  const auto length = read_le_u32(blob.payload.data());
  if (4u + length > blob.payload.size()) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(blob.payload.data() + 4), length);
}

std::optional<std::pair<double, double>> blob_as_point(const ParsedBlob& blob) {
  if ((blob.tag != "PTPt" && blob.tag != "PTSz") || blob.payload.size() < 16) {
    return std::nullopt;
  }
  return std::pair<double, double>{read_be_f64(blob.payload.data()),
                                   read_be_f64(blob.payload.data() + 8)};
}

std::string blend_code(BlendMode mode) {
  switch (mode) {
    case BlendMode::PassThrough:
      return "pass";
    case BlendMode::Multiply:
      return "mul ";
    case BlendMode::Screen:
      return "scrn";
    case BlendMode::Overlay:
      return "over";
    case BlendMode::Darken:
      return "dark";
    case BlendMode::Lighten:
      return "lite";
    case BlendMode::ColorDodge:
      return "div ";
    case BlendMode::ColorBurn:
      return "idiv";
    case BlendMode::HardLight:
      return "hLit";
    case BlendMode::SoftLight:
      return "sLit";
    case BlendMode::Difference:
      return "diff";
    case BlendMode::LinearBurn:
      return "lbrn";
    case BlendMode::PinLight:
      return "pLit";
    case BlendMode::Saturation:
      return "sat ";
    case BlendMode::Luminosity:
      return "lum ";
    case BlendMode::Exclusion:
      return "smud";
    case BlendMode::Hue:
      return "hue ";
    case BlendMode::Color:
      return "colr";
    case BlendMode::LinearDodge:
      return "lddg";
    case BlendMode::Subtract:
      return "fmud";
    case BlendMode::Divide:
      return "fdiv";
    case BlendMode::VividLight:
      return "vLit";
    case BlendMode::LinearLight:
      return "lLit";
    case BlendMode::HardMix:
      return "hMix";
    case BlendMode::DarkerColor:
      return "dkCl";
    case BlendMode::LighterColor:
      return "lgCl";
    case BlendMode::Normal:
    case BlendMode::Dissolve:
      return "norm";
  }
  return "norm";
}

BlendMode blend_mode_from_code(std::string code, bool* known) {
  if (code.size() < 4) {
    code.append(4 - code.size(), ' ');
  }
  code.resize(4);
  *known = true;
  if (code == "pass") return BlendMode::PassThrough;
  if (code == "norm") return BlendMode::Normal;
  if (code == "mul ") return BlendMode::Multiply;
  if (code == "scrn") return BlendMode::Screen;
  if (code == "over") return BlendMode::Overlay;
  if (code == "dark") return BlendMode::Darken;
  if (code == "lite") return BlendMode::Lighten;
  if (code == "div ") return BlendMode::ColorDodge;
  if (code == "idiv") return BlendMode::ColorBurn;
  if (code == "hLit") return BlendMode::HardLight;
  if (code == "sLit") return BlendMode::SoftLight;
  if (code == "diff") return BlendMode::Difference;
  if (code == "lbrn") return BlendMode::LinearBurn;
  if (code == "pLit") return BlendMode::PinLight;
  if (code == "sat ") return BlendMode::Saturation;
  if (code == "lum ") return BlendMode::Luminosity;
  if (code == "smud") return BlendMode::Exclusion;
  if (code == "hue ") return BlendMode::Hue;
  if (code == "colr") return BlendMode::Color;
  if (code == "lddg") return BlendMode::LinearDodge;
  if (code == "fmud") return BlendMode::Subtract;
  if (code == "fdiv") return BlendMode::Divide;
  if (code == "vLit") return BlendMode::VividLight;
  if (code == "lLit") return BlendMode::LinearLight;
  if (code == "hMix") return BlendMode::HardMix;
  if (code == "dkCl") return BlendMode::DarkerColor;
  if (code == "lgCl") return BlendMode::LighterColor;
  *known = false;
  return BlendMode::Normal;
}

std::string new_uuid() {
  std::random_device device;
  std::uniform_int_distribution<int> hex(0, 15);
  std::string id(36, '0');
  int position = 0;
  for (int i = 0; i < 32; ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) {
      id[static_cast<std::size_t>(position++)] = '-';
    }
    const int nibble = hex(device);
    id[static_cast<std::size_t>(position++)] =
        static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
  }
  id[14] = '4';
  return id;
}

struct RgbaImage {
  int width{0};
  int height{0};
  std::vector<std::uint8_t> rgba;
};

RgbaImage layer_rgba(const Layer& layer) {
  if (layer.kind() != LayerKind::Pixel || layer.pixels().empty()) {
    return {};
  }
  const auto& pixels = layer.pixels();
  const auto channels = pixels.format().channels;
  RgbaImage image;
  image.width = pixels.width();
  image.height = pixels.height();
  image.rgba.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4);
  for (std::int32_t y = 0; y < image.height; ++y) {
    const auto row = pixels.row(y);
    for (std::int32_t x = 0; x < image.width; ++x) {
      const auto* src = row.data() + static_cast<std::size_t>(x) * channels;
      auto* dst = image.rgba.data() +
                  (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
      const auto channel = [&](std::uint16_t index, std::uint8_t fallback) {
        return index < channels ? src[index] : fallback;
      };
      dst[0] = channel(0, 0);
      dst[1] = channel(1, dst[0]);
      dst[2] = channel(2, dst[0]);
      dst[3] = channels >= 4 ? src[3] : 255;
    }
  }
  return image;
}

struct EncodedTile {
  std::vector<std::uint8_t> bytes;
  const char* uti{"public.png"};
};

EncodedTile encode_original_content(const Layer& layer) {
  const RgbaImage image = layer_rgba(layer);
  if (image.rgba.empty()) {
    return {};
  }
  return {formats::encode_png_rgba8(image.rgba, image.width, image.height), "public.png"};
}

bool png_signature(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
}

PixelBuffer pixels_from_png(const std::vector<std::uint8_t>& png) {
  int width = 0;
  int height = 0;
  int components = 0;
  stbi_uc* decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height,
                                           &components, 4);
  if (decoded == nullptr || width <= 0 || height <= 0) {
    if (decoded != nullptr) {
      stbi_image_free(decoded);
    }
    return {};
  }
  PixelBuffer pixels(width, height, PixelFormat::rgba8());
  const auto detached = pixels.data();
  std::memcpy(detached.data(), decoded, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  stbi_image_free(decoded);
  return pixels;
}

PixelBuffer pixels_from_encoded(const std::vector<std::uint8_t>& bytes) {
  if (png_signature(bytes)) {
    return pixels_from_png(bytes);
  }
  return {};
}

std::vector<std::uint8_t> apple_date_bytes() {
  const auto unix_seconds =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  const double apple_seconds = unix_seconds - 978307200.0;
  std::vector<std::uint8_t> payload;
  append_be_f64(payload, apple_seconds);
  return payload;
}

std::vector<std::uint8_t> version_blob() {
  std::vector<std::uint8_t> payload;
  append_le(payload, 2, 2);
  append_le(payload, 27, 2);
  append_le(payload, 26, 2);
  append_le(payload, 0, 2);
  const auto add = [&](std::string_view text) {
    const auto blob = blob_strn(text);
    payload.insert(payload.end(), blob.begin(), blob.end());
  };
  add("Lienzo");
  add("0.1");
  add("");
  add("");
  add("");
  payload.insert(payload.end(), 16, 0);
  append_le(payload, 4, 4);
  return make_blob("DcVr", payload);
}

std::vector<std::uint8_t> srgb_format_blob() {
  cmsHPROFILE profile = cmsCreate_sRGBProfile();
  cmsUInt32Number size = 0;
  cmsSaveProfileToMem(profile, nullptr, &size);
  std::vector<std::uint8_t> icc(size);
  if (profile == nullptr || size == 0 || cmsSaveProfileToMem(profile, icc.data(), &size) == 0) {
    if (profile != nullptr) {
      cmsCloseProfile(profile);
    }
    return {};
  }
  cmsCloseProfile(profile);
  icc.resize(size);
  std::vector<std::uint8_t> color;
  append_le(color, 2, 2);
  append_le(color, 2, 2);
  append_le(color, icc.size(), 4);
  color.insert(color.end(), icc.begin(), icc.end());
  append_le(color, 0x600c, 4);
  const auto colorspace = make_blob("ClrS", color);
  std::vector<std::uint8_t> payload{0x01, 0x00, 0x08, 0x12, 0x04, 0x00};
  payload.insert(payload.end(), colorspace.begin(), colorspace.end());
  return make_blob("Frmt", payload);
}

std::vector<std::uint8_t> identity_transform_blob() {
  std::vector<std::uint8_t> payload;
  const double values[] = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
  for (double value : values) {
    append_be_f64(payload, value);
  }
  return make_blob("Trns", payload);
}

#if defined(PATCHY_HAS_SQLITE)

void check_sqlite(int status, sqlite3* db, const char* what) {
  if (status == SQLITE_OK || status == SQLITE_DONE || status == SQLITE_ROW) {
    return;
  }
  const char* message = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(status);
  throw std::runtime_error(std::string("Could not ") + what + " PXD file: " +
                           (message != nullptr ? message : "sqlite error"));
}

void exec_sql(sqlite3* db, const char* sql) {
  char* error = nullptr;
  const int status = sqlite3_exec(db, sql, nullptr, nullptr, &error);
  if (status != SQLITE_OK) {
    std::string message = error != nullptr ? error : "sqlite error";
    sqlite3_free(error);
    throw std::runtime_error("Could not write PXD file: " + message);
  }
}

void bind_blob(sqlite3_stmt* statement, int index, const std::vector<std::uint8_t>& bytes) {
  sqlite3_bind_blob(statement, index, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
}

struct TileFile {
  std::string name;
  std::vector<std::uint8_t> png;
};

void insert_info(sqlite3* db, int layer_id, const char* key, const std::vector<std::uint8_t>& value) {
  sqlite3_stmt* statement = nullptr;
  check_sqlite(sqlite3_prepare_v2(db, "INSERT INTO layer_info(layer_id, key, value) VALUES(?, ?, ?)", -1,
                                  &statement, nullptr),
               db, "write");
  sqlite3_bind_int(statement, 1, layer_id);
  sqlite3_bind_text(statement, 2, key, -1, SQLITE_TRANSIENT);
  bind_blob(statement, 3, value);
  check_sqlite(sqlite3_step(statement), db, "write");
  sqlite3_finalize(statement);
}

void write_layer_rows(sqlite3* db, const std::vector<Layer>& layers, const std::string& parent, int* next_id,
                      std::vector<TileFile>* tiles, std::int32_t document_height) {
  for (std::size_t i = 0; i < layers.size(); ++i) {
    const Layer& layer = layers[i];
    const int id = (*next_id)++;
    const std::string uuid = new_uuid();
    const int index = static_cast<int>(layers.size() - 1 - i);
    const int type = layer.kind() == LayerKind::Group ? 4 : 1;
    sqlite3_stmt* statement = nullptr;
    check_sqlite(sqlite3_prepare_v2(db,
                                    "INSERT INTO document_layers(id, identifier, parent_identifier, "
                                    "index_at_parent, type) VALUES(?, ?, ?, ?, ?)",
                                    -1, &statement, nullptr),
                 db, "write");
    sqlite3_bind_int(statement, 1, id);
    sqlite3_bind_text(statement, 2, uuid.c_str(), -1, SQLITE_TRANSIENT);
    if (parent.empty()) {
      sqlite3_bind_null(statement, 3);
    } else {
      sqlite3_bind_text(statement, 3, parent.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(statement, 4, index);
    sqlite3_bind_int(statement, 5, type);
    check_sqlite(sqlite3_step(statement), db, "write");
    sqlite3_finalize(statement);

    insert_info(db, id, "name", blob_strn(layer.name()));
    const int opacity = std::clamp(static_cast<int>(std::lround(layer.opacity() * 100.0f)), 0, 100);
    insert_info(db, id, "opacity", blob_u16("LOpc", static_cast<std::uint16_t>(opacity)));
    std::uint64_t flags = layer.visible() ? static_cast<std::uint64_t>(kVisible) : 0;
    if (layer.clipped()) {
      flags |= kClipping;
    }
    if (layer.kind() == LayerKind::Pixel) {
      flags |= kRaster | kOriginalContent;
    }
    insert_info(db, id, "flags", blob_u64(flags));
    insert_info(db, id, "blendMode", blob_blend(blend_code(layer.blend_mode())));

    const Rect bounds = layer.bounds();
    const double width = layer.kind() == LayerKind::Pixel ? layer.pixels().width() : bounds.width;
    const double height = layer.kind() == LayerKind::Pixel ? layer.pixels().height() : bounds.height;
    const double center_x = bounds.x + width / 2.0;
    const double center_y = static_cast<double>(document_height) - (bounds.y + height / 2.0);
    // backingScale 1: one stored point is one pixel. Scale stays 1 (not halved).
    // Anchor 0.5, 0.5 is the layer center, matching Pixelmator Pro.
    insert_info(db, id, "position", blob_point("PTPt", center_x, center_y));
    insert_info(db, id, "size", blob_point("PTSz", width, height));
    insert_info(db, id, "anchorPoint", blob_point("PTPt", 0.5, 0.5));
    insert_info(db, id, "angle", blob_f64("PTFl", 0.0));
    insert_info(db, id, "backingScale", blob_f64("PTFl", 1.0));
    insert_info(db, id, "scale", blob_point("PTPt", 1.0, 1.0));
    insert_info(db, id, "transform", identity_transform_blob());
    insert_info(db, id, "opct-nrm", blob_f64("LDOp", 1.0));

    if (layer.kind() == LayerKind::Pixel) {
      const EncodedTile encoded = encode_original_content(layer);
      if (!encoded.bytes.empty()) {
        const std::string filename = uuid + "-OriginalContentSource";
        tiles->push_back(TileFile{filename, encoded.bytes});
        const auto timestamp = apple_date_bytes();
        sqlite3_stmt* stored = nullptr;
        check_sqlite(sqlite3_prepare_v2(db,
                                        "INSERT INTO storable_info(identifier, timestamp, content_uti, "
                                        "layer_identifier, user_data, options) VALUES(?, ?, ?, ?, ?, ?)",
                                        -1, &stored, nullptr),
                     db, "write");
        sqlite3_bind_text(stored, 1, "OriginalContentSource", -1, SQLITE_STATIC);
        sqlite3_bind_blob(stored, 2, timestamp.data(), static_cast<int>(timestamp.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stored, 3, encoded.uti, -1, SQLITE_STATIC);
        sqlite3_bind_text(stored, 4, uuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_null(stored, 5);
        sqlite3_bind_int(stored, 6, 0);
        check_sqlite(sqlite3_step(stored), db, "write");
        sqlite3_finalize(stored);
      }
    }

    if (layer.kind() == LayerKind::Group) {
      write_layer_rows(db, layer.children(), uuid, next_id, tiles, document_height);
    }
  }
}

std::vector<std::uint8_t> database_bytes(const Document& document, std::vector<TileFile>* tiles) {
  sqlite3* db = nullptr;
  check_sqlite(sqlite3_open(":memory:", &db), db, "write");
  try {
    exec_sql(db,
             "CREATE TABLE document_info (key TEXT NOT NULL ON CONFLICT IGNORE UNIQUE ON CONFLICT REPLACE "
             "DEFAULT NULL, value BLOB NOT NULL ON CONFLICT IGNORE DEFAULT NULL);"
             "CREATE TABLE document_layers (id INTEGER PRIMARY KEY ON CONFLICT FAIL AUTOINCREMENT NOT NULL ON "
             "CONFLICT FAIL UNIQUE ON CONFLICT FAIL DEFAULT 0, identifier TEXT UNIQUE ON CONFLICT FAIL NOT NULL "
             "ON CONFLICT FAIL DEFAULT NULL, parent_identifier TEXT DEFAULT NULL, index_at_parent INTEGER NOT NULL "
             "ON CONFLICT FAIL DEFAULT 0, type INTEGER NOT NULL ON CONFLICT FAIL DEFAULT 0);"
             "CREATE TABLE document_meta (key TEXT NOT NULL ON CONFLICT IGNORE UNIQUE ON CONFLICT REPLACE "
             "DEFAULT NULL, value BLOB NOT NULL ON CONFLICT IGNORE DEFAULT NULL);"
             "CREATE TABLE layer_info (layer_id INTEGER NOT NULL ON CONFLICT IGNORE REFERENCES "
             "document_layers(id) ON UPDATE RESTRICT ON DELETE RESTRICT, key TEXT NOT NULL ON CONFLICT IGNORE "
             "DEFAULT NULL, value BLOB NOT NULL ON CONFLICT IGNORE DEFAULT NULL, UNIQUE (layer_id, key) ON "
             "CONFLICT REPLACE);"
             "CREATE TABLE layer_tiles (layer_id INTEGER NOT NULL ON CONFLICT FAIL DEFAULT 0 REFERENCES "
             "document_layers(id) ON UPDATE RESTRICT ON DELETE RESTRICT, identifier BLOB NOT NULL ON CONFLICT "
             "FAIL DEFAULT NULL, timestamp BLOB NOT NULL ON CONFLICT FAIL DEFAULT NULL, size BLOB NOT NULL ON "
             "CONFLICT FAIL DEFAULT NULL, format BLOB NOT NULL ON CONFLICT FAIL DEFAULT NULL, metadata BLOB "
             "DEFAULT NULL, UNIQUE (layer_id, identifier) ON CONFLICT FAIL);"
             "CREATE TABLE storable_info (identifier TEXT NOT NULL ON CONFLICT FAIL DEFAULT NULL, timestamp BLOB "
             "NOT NULL ON CONFLICT FAIL DEFAULT NULL, content_uti TEXT DEFAULT NULL, layer_identifier TEXT "
             "DEFAULT NULL, user_data BLOB DEFAULT NULL, options INTEGER DEFAULT 0, UNIQUE (identifier, "
             "layer_identifier) ON CONFLICT FAIL);"
             "CREATE TABLE ql_info (key TEXT NOT NULL ON CONFLICT IGNORE UNIQUE ON CONFLICT REPLACE DEFAULT NULL, "
             "value TEXT NOT NULL ON CONFLICT IGNORE DEFAULT NULL);");
    const auto insert_document_info = [&](const char* key, const std::vector<std::uint8_t>& value) {
      if (value.empty()) {
        return;
      }
      sqlite3_stmt* info = nullptr;
      check_sqlite(sqlite3_prepare_v2(db, "INSERT INTO document_info(key, value) VALUES(?, ?)", -1, &info, nullptr),
                   db, "write");
      sqlite3_bind_text(info, 1, key, -1, SQLITE_STATIC);
      bind_blob(info, 2, value);
      check_sqlite(sqlite3_step(info), db, "write");
      sqlite3_finalize(info);
    };
    insert_document_info("size", blob_bdsz(document.width(), document.height()));
    insert_document_info("version", version_blob());
    insert_document_info("date", make_blob("Date", apple_date_bytes()));
    insert_document_info("format", srgb_format_blob());
    const auto timestamp = apple_date_bytes();
    const std::string writer =
        "[1,{\"v\":\"0.1\",\"o\":\"\",\"d\":\"\",\"b\":\"\",\"p\":\"linux\",\"r\":1,\"a\":\"lienzo\"}]";
    sqlite3_stmt* stored = nullptr;
    check_sqlite(sqlite3_prepare_v2(db,
                                    "INSERT INTO storable_info(identifier, timestamp, content_uti, "
                                    "layer_identifier, user_data, options) VALUES(?, ?, NULL, NULL, ?, ?)",
                                    -1, &stored, nullptr),
                 db, "write");
    sqlite3_bind_text(stored, 1, "com.pixelmatorteam.ptfoundation.content-writer", -1, SQLITE_STATIC);
    sqlite3_bind_blob(stored, 2, timestamp.data(), static_cast<int>(timestamp.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stored, 3, writer.data(), static_cast<int>(writer.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(stored, 4, 3);
    check_sqlite(sqlite3_step(stored), db, "write");
    sqlite3_finalize(stored);
    int next_id = 1;
    write_layer_rows(db, document.layers(), "", &next_id, tiles, document.height());
    sqlite3_int64 size = 0;
    unsigned char* serialized = sqlite3_serialize(db, "main", &size, 0);
    if (serialized == nullptr || size <= 0) {
      sqlite3_close(db);
      throw std::runtime_error("Could not write PXD file: sqlite serialize failed");
    }
    std::vector<std::uint8_t> bytes(serialized, serialized + size);
    sqlite3_free(serialized);
    sqlite3_close(db);
    return bytes;
  } catch (...) {
    sqlite3_close(db);
    throw;
  }
}

std::vector<std::uint8_t> zip_bytes(const std::vector<std::uint8_t>& database, const std::vector<TileFile>& tiles) {
  mz_zip_archive zip{};
  if (mz_zip_writer_init_heap(&zip, 0, database.size() + 64) == MZ_FALSE) {
    throw std::runtime_error("Could not write PXD file");
  }
  auto fail = [&]() {
    mz_zip_writer_end(&zip);
    throw std::runtime_error("Could not write PXD file");
  };
  if (mz_zip_writer_add_mem(&zip, "metadata.info", database.data(), database.size(), MZ_NO_COMPRESSION) == MZ_FALSE) {
    fail();
  }
  for (const auto& tile : tiles) {
    const std::string name = "data/" + tile.name;
    if (mz_zip_writer_add_mem(&zip, name.c_str(), tile.png.data(), tile.png.size(), MZ_NO_COMPRESSION) == MZ_FALSE) {
      fail();
    }
  }
  void* archive = nullptr;
  std::size_t archive_size = 0;
  if (mz_zip_writer_finalize_heap_archive(&zip, &archive, &archive_size) == MZ_FALSE) {
    fail();
  }
  mz_zip_writer_end(&zip);
  std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(archive),
                                  static_cast<std::uint8_t*>(archive) + archive_size);
  mz_free(archive);
  return bytes;
}

struct PackageFiles {
  std::vector<std::uint8_t> database;
  std::map<std::string, std::vector<std::uint8_t>> files;
};

PackageFiles read_zip_package(std::span<const std::uint8_t> bytes) {
  mz_zip_archive zip{};
  if (mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0) == MZ_FALSE) {
    throw std::runtime_error("Could not open PXD file");
  }
  PackageFiles package;
  const int count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
  for (int i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(i), &stat) == MZ_FALSE) {
      continue;
    }
    std::string name = stat.m_filename;
    if (name.empty() || name.back() == '/') {
      continue;
    }
    const auto slash = name.find_last_of('/');
    const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    std::size_t size = 0;
    void* extracted = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(i), &size, 0);
    if (extracted == nullptr) {
      continue;
    }
    std::vector<std::uint8_t> file(static_cast<std::uint8_t*>(extracted), static_cast<std::uint8_t*>(extracted) + size);
    mz_free(extracted);
    if (base == "metadata.info") {
      package.database = std::move(file);
    } else {
      package.files.emplace(base, std::move(file));
    }
  }
  mz_zip_reader_end(&zip);
  if (package.database.empty()) {
    throw std::runtime_error("Could not open PXD file: missing metadata.info");
  }
  return package;
}

PackageFiles read_directory_package(const std::filesystem::path& path) {
  PackageFiles package;
  package.database = formats::read_file_bytes(path / "metadata.info", "PXD");
  const auto data = path / "data";
  if (std::filesystem::is_directory(data)) {
    for (const auto& entry : std::filesystem::directory_iterator(data)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      package.files.emplace(path_to_utf8(entry.path().filename()),
                            formats::read_file_bytes(entry.path(), "PXD"));
    }
  }
  return package;
}

std::vector<std::uint8_t> column_bytes(sqlite3_stmt* statement, int column) {
  const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, column));
  const int size = sqlite3_column_bytes(statement, column);
  if (data == nullptr || size <= 0) {
    return {};
  }
  return std::vector<std::uint8_t>(data, data + size);
}

int base64_value(unsigned char character) {
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
}

std::vector<std::uint8_t> decode_base64(std::string_view text) {
  std::vector<std::uint8_t> out;
  int value = 0;
  int bits = 0;
  for (unsigned char character : text) {
    if (character == '=' || character == '"') {
      break;
    }
    const int digit = base64_value(character);
    if (digit < 0) {
      continue;
    }
    value = (value << 6) | digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
    }
  }
  return out;
}

float big_endian_f32(const std::uint8_t* bytes) {
  const std::uint32_t bits = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                             (static_cast<std::uint32_t>(bytes[1]) << 16) |
                             (static_cast<std::uint32_t>(bytes[2]) << 8) |
                             static_cast<std::uint32_t>(bytes[3]);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint8_t unit_byte(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<std::uint8_t>(std::lround(std::clamp(static_cast<double>(value), 0.0, 1.0) * 255.0));
}

bool color_fill_from_effects(const std::vector<std::uint8_t>& value, std::uint8_t* red, std::uint8_t* green,
                             std::uint8_t* blue, std::uint8_t* alpha) {
  const std::string text(value.begin(), value.end());
  const auto marker = text.find("CIFilterNSCodingData\":\"");
  if (marker == std::string::npos) {
    return false;
  }
  const auto start = marker + std::string("CIFilterNSCodingData\":\"").size();
  const auto end = text.find('"', start);
  if (end == std::string::npos || end <= start) {
    return false;
  }
  const auto plist = decode_base64(std::string_view(text).substr(start, end - start));
  const auto red_key = std::search(plist.begin(), plist.end(), std::begin("red"), std::end("red") - 1);
  if (red_key == plist.end()) {
    return false;
  }
  const auto alpha_key = std::search(red_key, plist.end(), std::begin("alpha"), std::end("alpha") - 1);
  const auto blue_key = alpha_key == plist.end()
                            ? plist.end()
                            : std::search(alpha_key, plist.end(), std::begin("blue"), std::end("blue") - 1);
  const auto green_key = blue_key == plist.end()
                             ? plist.end()
                             : std::search(blue_key, plist.end(), std::begin("green"), std::end("green") - 1);
  if (green_key == plist.end()) {
    return false;
  }
  std::vector<float> components;
  for (auto cursor = green_key; cursor + 5 <= plist.end() && components.size() < 4; ++cursor) {
    if (*cursor == 0x22) {
      components.push_back(big_endian_f32(&*cursor + 1));
      cursor += 4;
    }
  }
  if (components.size() < 4) {
    return false;
  }
  *red = unit_byte(components[0]);
  *alpha = unit_byte(components[1]);
  *blue = unit_byte(components[2]);
  *green = unit_byte(components[3]);
  return true;
}

void apply_info(RawLayer* layer, const std::string& key, const std::vector<std::uint8_t>& value) {
  const auto blob = parse_blob(value.data(), static_cast<int>(value.size()));
  if (!blob.has_value()) {
    return;
  }
  if (key == "name") {
    if (const auto text = blob_as_string(*blob); text.has_value() && !text->empty()) {
      layer->name = *text;
    }
  } else if (key == "opacity" && blob->tag == "LOpc" && blob->payload.size() >= 2) {
    layer->opacity = std::clamp(static_cast<int>(blob->payload[0] | (blob->payload[1] << 8)), 0, 100);
  } else if (key == "flags" && blob->tag == "UI64" && blob->payload.size() >= 8) {
    layer->flags = read_le_u64(blob->payload.data());
  } else if (key == "blendMode" && blob->tag == "Blnd" && blob->payload.size() >= 4) {
    layer->blend.assign(reinterpret_cast<const char*>(blob->payload.data()), 4);
    std::reverse(layer->blend.begin(), layer->blend.end());
  } else if (key == "position") {
    if (const auto point = blob_as_point(*blob); point.has_value()) {
      layer->center_x = point->first;
      layer->center_y = point->second;
      layer->has_geometry = true;
    }
  } else if (key == "size") {
    if (const auto point = blob_as_point(*blob); point.has_value()) {
      layer->width = point->first;
      layer->height = point->second;
      layer->has_geometry = true;
    }
  } else if (key == "backingScale" && blob->tag == "PTFl" && blob->payload.size() >= 8) {
    const double scale = read_be_f64(blob->payload.data());
    if (scale > 0.0) {
      layer->backing_scale = scale;
    }
  }
}

Document document_from_package(const PackageFiles& package, std::vector<std::string>* notices) {
  sqlite3* db = nullptr;
  check_sqlite(sqlite3_open(":memory:", &db), db, "open");
  try {
    check_sqlite(sqlite3_deserialize(db, "main", const_cast<unsigned char*>(package.database.data()),
                                     static_cast<sqlite3_int64>(package.database.size()),
                                     static_cast<sqlite3_int64>(package.database.size()),
                                     SQLITE_DESERIALIZE_READONLY),
                 db, "open");
    std::int32_t width = 0;
    std::int32_t height = 0;
    sqlite3_stmt* size_stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db, "SELECT value FROM document_info WHERE key = 'size'", -1, &size_stmt,
                                    nullptr),
                 db, "open");
    if (sqlite3_step(size_stmt) == SQLITE_ROW) {
      const auto bytes = column_bytes(size_stmt, 0);
      const auto blob = parse_blob(bytes.data(), static_cast<int>(bytes.size()));
      if (blob.has_value() && blob->tag == "BDSz" && blob->payload.size() >= 16) {
        width = static_cast<std::int32_t>(read_le_i64(blob->payload.data()));
        height = static_cast<std::int32_t>(read_le_i64(blob->payload.data() + 8));
      }
    }
    sqlite3_finalize(size_stmt);
    if (width <= 0 || height <= 0) {
      throw std::runtime_error("Could not open PXD file: missing document size");
    }

    std::map<int, RawLayer> layers;
    sqlite3_stmt* layer_stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db,
                                    "SELECT id, identifier, parent_identifier, index_at_parent, type FROM "
                                    "document_layers",
                                    -1, &layer_stmt, nullptr),
                 db, "open");
    while (sqlite3_step(layer_stmt) == SQLITE_ROW) {
      RawLayer layer;
      layer.id = sqlite3_column_int(layer_stmt, 0);
      const auto* uuid = sqlite3_column_text(layer_stmt, 1);
      layer.uuid = uuid != nullptr ? reinterpret_cast<const char*>(uuid) : "";
      layer.top_level = sqlite3_column_type(layer_stmt, 2) == SQLITE_NULL;
      const auto* parent = sqlite3_column_text(layer_stmt, 2);
      layer.parent = parent != nullptr ? reinterpret_cast<const char*>(parent) : "";
      layer.index = sqlite3_column_int(layer_stmt, 3);
      layer.type = sqlite3_column_int(layer_stmt, 4);
      layers.emplace(layer.id, std::move(layer));
    }
    sqlite3_finalize(layer_stmt);

    sqlite3_stmt* info_stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db, "SELECT layer_id, key, value FROM layer_info", -1, &info_stmt, nullptr),
                 db, "open");
    while (sqlite3_step(info_stmt) == SQLITE_ROW) {
      const auto found = layers.find(sqlite3_column_int(info_stmt, 0));
      if (found == layers.end()) {
        continue;
      }
      const auto* key = sqlite3_column_text(info_stmt, 1);
      if (key == nullptr) {
        continue;
      }
      const auto info_bytes = column_bytes(info_stmt, 2);
      const std::string info_key = reinterpret_cast<const char*>(key);
      if (info_key == "effects-data") {
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
        std::uint8_t alpha = 255;
        if (color_fill_from_effects(info_bytes, &red, &green, &blue, &alpha)) {
          found->second.canvas_fill = true;
          found->second.fill_r = red;
          found->second.fill_g = green;
          found->second.fill_b = blue;
          found->second.fill_a = alpha;
        }
      }
      apply_info(&found->second, info_key, info_bytes);
    }
    sqlite3_finalize(info_stmt);

    sqlite3_stmt* tile_stmt = nullptr;
    check_sqlite(
        sqlite3_prepare_v2(db, "SELECT layer_id, identifier, format FROM layer_tiles", -1, &tile_stmt, nullptr), db,
        "open");
    while (sqlite3_step(tile_stmt) == SQLITE_ROW) {
      const auto found = layers.find(sqlite3_column_int(tile_stmt, 0));
      if (found == layers.end()) {
        continue;
      }
      const auto identifier_bytes = column_bytes(tile_stmt, 1);
      const auto format_bytes = column_bytes(tile_stmt, 2);
      const auto identifier = parse_blob(identifier_bytes.data(), static_cast<int>(identifier_bytes.size()));
      const auto format = parse_blob(format_bytes.data(), static_cast<int>(format_bytes.size()));
      const auto name = identifier.has_value() ? blob_as_string(*identifier) : std::nullopt;
      const auto codec = format.has_value() ? blob_as_string(*format) : std::nullopt;
      if (!name.has_value()) {
        continue;
      }
      const auto file = package.files.find(*name);
      if (file == package.files.end()) {
        continue;
      }
      const bool png_name = name->size() >= 4 && name->compare(name->size() - 4, 4, ".png") == 0;
      const bool png_codec = codec.has_value() && *codec == "png";
      if (png_name || png_codec || file->second.size() >= 8) {
        found->second.encoded = file->second;
      }
    }
    sqlite3_finalize(tile_stmt);

    sqlite3_stmt* stored_stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT identifier, layer_identifier FROM storable_info WHERE identifier = "
                           "'OriginalContentSource'",
                           -1, &stored_stmt, nullptr) == SQLITE_OK) {
      while (sqlite3_step(stored_stmt) == SQLITE_ROW) {
        const auto* identifier = sqlite3_column_text(stored_stmt, 0);
        const auto* layer_identifier = sqlite3_column_text(stored_stmt, 1);
        if (identifier == nullptr || layer_identifier == nullptr) {
          continue;
        }
        const std::string filename = std::string(reinterpret_cast<const char*>(layer_identifier)) + "-" +
                                     reinterpret_cast<const char*>(identifier);
        const auto file = package.files.find(filename);
        if (file == package.files.end()) {
          continue;
        }
        for (auto& [id, layer] : layers) {
          (void)id;
          if (layer.uuid == reinterpret_cast<const char*>(layer_identifier)) {
            layer.encoded = file->second;
          }
        }
      }
      sqlite3_finalize(stored_stmt);
    }

    Document document(width, height, PixelFormat::rgba8());
    std::map<std::string, std::vector<Layer>> children;
    std::vector<Layer> top;
    std::vector<RawLayer> ordered;
    ordered.reserve(layers.size());
    for (auto& [id, layer] : layers) {
      (void)id;
      ordered.push_back(std::move(layer));
    }
    std::sort(ordered.begin(), ordered.end(), [](const RawLayer& a, const RawLayer& b) {
      if (a.parent != b.parent) {
        return a.parent < b.parent;
      }
      return a.index > b.index;
    });

    LayerId next_layer = 1;
    // Children are built after a post-order walk: highest Pixelmator index (bottom) first.
    std::vector<RawLayer*> post;
    const auto visit = [&](auto&& self, const std::string& parent) -> void {
      for (auto& layer : ordered) {
        const bool match = parent.empty() ? layer.top_level : layer.parent == parent;
        if (match) {
          self(self, layer.uuid);
          post.push_back(&layer);
        }
      }
    };
    visit(visit, "");
    for (RawLayer* raw : post) {
      bool known_blend = false;
      const BlendMode blend = blend_mode_from_code(raw->blend, &known_blend);
      if (!known_blend && notices != nullptr) {
        notices->push_back("PXD blend mode \"" + raw->blend + "\" was imported as Normal");
      }
      if ((raw->flags & kLocked) != 0 && notices != nullptr) {
        notices->push_back("Locked PXD layer \"" + raw->name + "\" was imported unlocked");
      }
      Layer layer;
      if (raw->type == 4) {
        layer = Layer(next_layer++, raw->name, LayerKind::Group);
        layer.children() = std::move(children[raw->uuid]);
      } else {
        PixelBuffer pixels = pixels_from_encoded(raw->encoded);
        if (raw->canvas_fill && pixels.empty()) {
          pixels = PixelBuffer(width, height, PixelFormat::rgba8());
          pixels.clear(0);
          for (std::int32_t y = 0; y < height; ++y) {
            auto row = pixels.row(y);
            for (std::int32_t x = 0; x < width; ++x) {
              auto* pixel = row.data() + static_cast<std::size_t>(x) * 4;
              pixel[0] = raw->fill_r;
              pixel[1] = raw->fill_g;
              pixel[2] = raw->fill_b;
              pixel[3] = raw->fill_a;
            }
          }
          raw->has_geometry = false;
        }
        if (pixels.empty()) {
          const auto w = raw->type == 1
                             ? std::max(1, static_cast<int>(std::lround(raw->width * raw->backing_scale)))
                             : 1;
          const auto h = raw->type == 1
                             ? std::max(1, static_cast<int>(std::lround(raw->height * raw->backing_scale)))
                             : 1;
          pixels = PixelBuffer(w, h, PixelFormat::rgba8());
          pixels.clear(0);
          if (raw->type != 1 && notices != nullptr) {
            notices->push_back("PXD layer \"" + raw->name + "\" is not a raster layer; it was imported empty");
          } else if (!raw->encoded.empty() && notices != nullptr) {
            notices->push_back("PXD raster tile for \"" + raw->name +
                               "\" could not be decoded; the layer was imported empty");
          } else if (raw->encoded.empty() && raw->type == 1 && notices != nullptr) {
            notices->push_back("PXD raster tile for \"" + raw->name + "\" is missing; the layer was imported empty");
          }
        }
        const double pixel_w = pixels.width();
        const double pixel_h = pixels.height();
        layer = Layer(next_layer++, raw->name, std::move(pixels));
        if (raw->has_geometry) {
          const double center_x = raw->center_x * raw->backing_scale;
          const double center_y = raw->center_y * raw->backing_scale;
          const double center_y_top = static_cast<double>(height) - center_y;
          layer.set_bounds(Rect{static_cast<std::int32_t>(std::lround(center_x - pixel_w / 2.0)),
                                static_cast<std::int32_t>(std::lround(center_y_top - pixel_h / 2.0)),
                                static_cast<std::int32_t>(pixel_w), static_cast<std::int32_t>(pixel_h)});
        }
      }
      layer.set_visible((raw->flags & kVisible) != 0);
      layer.set_clipped((raw->flags & kClipping) != 0);
      layer.set_opacity(static_cast<float>(raw->opacity) / 100.0f);
      layer.set_blend_mode(blend);
      if ((raw->flags & kMask) != 0 && notices != nullptr) {
        notices->push_back("PXD mask \"" + raw->name + "\" was imported as its own layer");
      }
      if (raw->top_level) {
        document.add_layer(std::move(layer));
      } else {
        children[raw->parent].push_back(std::move(layer));
      }
    }
    if (!document.layers().empty()) {
      document.set_active_layer(document.layers().back().id());
    }
    sqlite3_close(db);
    return document;
  } catch (...) {
    sqlite3_close(db);
    throw;
  }
}

void write_directory_package(const Document& document, const std::filesystem::path& path) {
  std::vector<TileFile> tiles;
  const auto database = database_bytes(document, &tiles);
  auto scratch = path;
  scratch += ".lienzo-tmp";
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(scratch / "data");
  formats::write_file_bytes(scratch / "metadata.info", database, "PXD");
  for (const auto& tile : tiles) {
    formats::write_file_bytes(scratch / "data" / tile.name, tile.png, "PXD");
  }
  auto backup = path;
  backup += ".lienzo-old";
  std::filesystem::remove_all(backup);
  if (std::filesystem::exists(path)) {
    std::filesystem::rename(path, backup);
  }
  try {
    std::filesystem::rename(scratch, path);
  } catch (...) {
    if (std::filesystem::exists(backup) && !std::filesystem::exists(path)) {
      std::filesystem::rename(backup, path);
    }
    std::filesystem::remove_all(scratch);
    throw;
  }
  std::filesystem::remove_all(backup);
}

#else

[[noreturn]] void sqlite_missing() {
  throw std::runtime_error("Could not open PXD file: this build has no sqlite3");
}

#endif

}  // namespace

bool DocumentIo::sniff(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.size() < 4 || bytes[0] != 'P' || bytes[1] != 'K') {
    return false;
  }
  mz_zip_archive zip{};
  if (mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0) == MZ_FALSE) {
    return false;
  }
  const bool found = mz_zip_reader_locate_file(&zip, "metadata.info", nullptr, 0) >= 0;
  mz_zip_reader_end(&zip);
  return found;
}

Document DocumentIo::read(std::span<const std::uint8_t> bytes, std::vector<std::string>* notices) {
#if defined(PATCHY_HAS_SQLITE)
  return document_from_package(read_zip_package(bytes), notices);
#else
  (void)bytes;
  (void)notices;
  sqlite_missing();
#endif
}

Document DocumentIo::read_file(const std::filesystem::path& path, std::vector<std::string>* notices) {
#if defined(PATCHY_HAS_SQLITE)
  if (std::filesystem::is_directory(path)) {
    return document_from_package(read_directory_package(path), notices);
  }
  return read(formats::read_file_bytes(path, "PXD"), notices);
#else
  (void)path;
  (void)notices;
  sqlite_missing();
#endif
}

std::vector<std::uint8_t> DocumentIo::write(const Document& document) {
#if defined(PATCHY_HAS_SQLITE)
  std::vector<TileFile> tiles;
  const auto database = database_bytes(document, &tiles);
  return zip_bytes(database, tiles);
#else
  (void)document;
  sqlite_missing();
#endif
}

void DocumentIo::write_file(const Document& document, const std::filesystem::path& path) {
#if defined(PATCHY_HAS_SQLITE)
  if (std::filesystem::is_directory(path)) {
    write_directory_package(document, path);
    return;
  }
  formats::write_file_bytes(path, write(document), "PXD");
#else
  (void)document;
  (void)path;
  sqlite_missing();
#endif
}

}  // namespace patchy::pxd
