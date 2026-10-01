#include "core/document.hpp"
#include "formats/format_registry.hpp"
#include "formats/pxd_document_io.hpp"

#include "test_harness.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace {

patchy::Document sample_document() {
  patchy::Document document(10, 8, patchy::PixelFormat::rgba8());
  patchy::PixelBuffer paint(2, 2, patchy::PixelFormat::rgba8());
  auto* pixel = paint.pixel(0, 0);
  pixel[0] = 240;
  pixel[3] = 128;
  pixel = paint.pixel(1, 1);
  pixel[1] = 200;
  pixel[3] = 255;
  patchy::Layer layer(1, "Paint", std::move(paint));
  layer.set_bounds(patchy::Rect{1, 3, 2, 2});
  layer.set_opacity(0.5f);
  layer.set_blend_mode(patchy::BlendMode::Multiply);
  layer.set_visible(false);
  document.add_layer(std::move(layer));

  patchy::Layer group(2, "Group", patchy::LayerKind::Group);
  patchy::PixelBuffer child_pixels(1, 1, patchy::PixelFormat::rgba8());
  auto* child_pixel = child_pixels.pixel(0, 0);
  child_pixel[2] = 255;
  child_pixel[3] = 255;
  patchy::Layer child(3, "Child", std::move(child_pixels));
  child.set_bounds(patchy::Rect{4, 1, 1, 1});
  group.children().push_back(std::move(child));
  document.add_layer(std::move(group));
  return document;
}

void expect_round_trip(const patchy::Document& read) {
  CHECK(read.width() == 10);
  CHECK(read.height() == 8);
  CHECK(read.layers().size() == 2);
  const auto& paint = read.layers()[0];
  CHECK(paint.name() == "Paint");
  CHECK(paint.visible() == false);
  CHECK(paint.opacity() > 0.49f && paint.opacity() < 0.51f);
  CHECK(paint.blend_mode() == patchy::BlendMode::Multiply);
  CHECK(paint.bounds().x == 1);
  CHECK(paint.bounds().y == 3);
  CHECK(paint.pixels().pixel(0, 0)[0] == 240);
  CHECK(paint.pixels().pixel(0, 0)[3] == 128);
  CHECK(paint.pixels().pixel(1, 1)[1] == 200);
  const auto& group = read.layers()[1];
  CHECK(group.kind() == patchy::LayerKind::Group);
  CHECK(group.name() == "Group");
  CHECK(group.children().size() == 1);
  CHECK(group.children()[0].name() == "Child");
  CHECK(group.children()[0].bounds().x == 4);
  CHECK(group.children()[0].bounds().y == 1);
  CHECK(group.children()[0].pixels().pixel(0, 0)[2] == 255);
}

void pxd_zip_round_trip_keeps_layers_and_pixels() {
  const auto document = sample_document();
  const auto bytes = patchy::pxd::DocumentIo::write(document);
  CHECK(patchy::pxd::DocumentIo::sniff(bytes));
  const auto* handler = patchy::builtin_format_registry().find_by_extension(".pxd");
  CHECK(handler != nullptr && handler->can_write());
  std::vector<std::string> notices;
  const auto read = patchy::pxd::DocumentIo::read(bytes, &notices);
  expect_round_trip(read);
  CHECK(notices.empty());
}

void pxd_directory_package_round_trip() {
  const auto directory = std::filesystem::path("pxd-round-trip.pxd");
  std::filesystem::remove_all(directory);
  std::filesystem::create_directory(directory);
  patchy::pxd::DocumentIo::write_file(sample_document(), directory);
  CHECK(std::filesystem::is_regular_file(directory / "metadata.info"));
  std::vector<std::string> notices;
  const auto read = patchy::pxd::DocumentIo::read_file(directory, &notices);
  expect_round_trip(read);
  CHECK(notices.empty());
  std::filesystem::remove_all(directory);
}

}  // namespace

std::vector<patchy::test::TestCase> pxd_tests() {
  return {
      {"pxd_zip_round_trip_keeps_layers_and_pixels", pxd_zip_round_trip_keeps_layers_and_pixels},
      {"pxd_directory_package_round_trip", pxd_directory_package_round_trip},
  };
}
