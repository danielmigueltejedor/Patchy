#include "ui-gnome/layer_thumbnail.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lienzo::gnome {

namespace {

constexpr int kThumbSize = 40;

GtkWidget* fallback_icon(
    const char* name) {
  GtkWidget* image =
      gtk_image_new_from_icon_name(name);

  gtk_image_set_pixel_size(
      GTK_IMAGE(image),
      22);

  gtk_widget_set_size_request(
      image,
      kThumbSize,
      kThumbSize);

  return image;
}

GtkWidget* picture_from_rgba(
    std::vector<std::uint8_t> rgba,
    int width,
    int height) {
  if (
      width <= 0 ||
      height <= 0 ||
      rgba.empty()) {
    return fallback_icon(
        "image-x-generic-symbolic");
  }

  GBytes* bytes =
      g_bytes_new(
          rgba.data(),
          rgba.size());

  GdkTexture* texture =
      gdk_memory_texture_new(
          width,
          height,
          GDK_MEMORY_R8G8B8A8,
          bytes,
          static_cast<gsize>(width) * 4);

  g_bytes_unref(bytes);

  GtkWidget* picture =
      gtk_picture_new_for_paintable(
          GDK_PAINTABLE(texture));

  g_object_unref(texture);

  gtk_picture_set_can_shrink(
      GTK_PICTURE(picture),
      TRUE);

  gtk_picture_set_content_fit(
      GTK_PICTURE(picture),
      GTK_CONTENT_FIT_CONTAIN);

  gtk_widget_set_size_request(
      picture,
      kThumbSize,
      kThumbSize);

  return picture;
}

GtkWidget* picture_from_pixels(
    const patchy::PixelBuffer& pixels,
    bool grayscale) {
  if (
      pixels.empty() ||
      pixels.format().bit_depth !=
          patchy::BitDepth::UInt8) {
    return fallback_icon(
        "image-x-generic-symbolic");
  }

  const int src_w = pixels.width();
  const int src_h = pixels.height();

  if (src_w <= 0 || src_h <= 0) {
    return fallback_icon(
        "image-x-generic-symbolic");
  }

  const int channels =
      pixels.format().channels;

  if (channels <= 0) {
    return fallback_icon(
        "image-x-generic-symbolic");
  }

  std::vector<std::uint8_t> rgba(
      static_cast<std::size_t>(kThumbSize) *
      static_cast<std::size_t>(kThumbSize) *
      4);

  for (int y = 0; y < kThumbSize; ++y) {
    const int sy = std::clamp(
        y * src_h / kThumbSize,
        0,
        src_h - 1);

    const auto row = pixels.row(sy);

    for (int x = 0; x < kThumbSize; ++x) {
      const int sx = std::clamp(
          x * src_w / kThumbSize,
          0,
          src_w - 1);

      const std::size_t source_index =
          static_cast<std::size_t>(sx) *
          static_cast<std::size_t>(channels);

      if (
          source_index +
              static_cast<std::size_t>(channels) >
          row.size()) {
        continue;
      }

      const auto* source =
          row.data() + source_index;

      const std::size_t i =
          (static_cast<std::size_t>(y) *
               static_cast<std::size_t>(kThumbSize) +
           static_cast<std::size_t>(x)) *
          4;

      if (grayscale) {
        const std::uint8_t value = source[0];

        rgba[i + 0] = value;
        rgba[i + 1] = value;
        rgba[i + 2] = value;
        rgba[i + 3] = 255;
      } else {
        const std::uint8_t alpha =
            channels >= 4 ? source[3] : 255;

        const bool checker_dark =
            ((sx / 8) + (sy / 8)) % 2 == 0;

        const std::uint8_t checker =
            checker_dark ? 180 : 230;

        const double coverage = alpha / 255.0;

        rgba[i + 0] = static_cast<std::uint8_t>(
            source[0] * coverage +
            checker * (1.0 - coverage));

        rgba[i + 1] = static_cast<std::uint8_t>(
            (channels >= 2 ? source[1] : source[0]) *
                coverage +
            checker * (1.0 - coverage));

        rgba[i + 2] = static_cast<std::uint8_t>(
            (channels >= 3 ? source[2] : source[0]) *
                coverage +
            checker * (1.0 - coverage));

        rgba[i + 3] = 255;
      }
    }
  }

  return picture_from_rgba(
      rgba,
      kThumbSize,
      kThumbSize);
}

}  // namespace

GtkWidget* create_layer_thumbnail(
    const patchy::Layer& layer,
    int,
    int) {
  if (layer.kind() == patchy::LayerKind::Group) {
    return fallback_icon(
        "folder-symbolic");
  }

  if (layer.kind() == patchy::LayerKind::Adjustment) {
    return fallback_icon(
        "image-adjust-color-symbolic");
  }

  if (
      layer.kind() == patchy::LayerKind::Text ||
      layer.kind() == patchy::LayerKind::Vector) {
    return fallback_icon(
        "insert-text-symbolic");
  }

  return picture_from_pixels(
      layer.pixels(),
      false);
}

GtkWidget* create_mask_thumbnail(
    const patchy::LayerMask& mask) {
  return picture_from_pixels(
      mask.pixels,
      true);
}

GtkWidget* create_channel_thumbnail(
    const patchy::PixelBuffer& pixels) {
  return picture_from_pixels(
      pixels,
      true);
}

}  // namespace lienzo::gnome
