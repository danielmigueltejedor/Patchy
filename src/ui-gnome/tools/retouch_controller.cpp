#include "ui-gnome/tools/retouch_controller.hpp"

#include "core/rect_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace lienzo::gnome {

namespace {

std::uint8_t clamp_byte(
    double value) {
  return static_cast<std::uint8_t>(
      std::clamp(
          std::lround(value),
          0L,
          255L));
}

std::uint64_t pixel_key(
    int x,
    int y) {
  return
      (
          static_cast<std::uint64_t>(
              static_cast<std::uint32_t>(y))
          << 32U) |
      static_cast<std::uint32_t>(x);
}

float footprint(
    double distance,
    double radius,
    int softness) {
  if (distance > radius) {
    return 0.0F;
  }

  if (softness <= 0) {
    return 1.0F;
  }

  const double edge =
      std::max(
          0.5,
          radius *
              static_cast<double>(softness) /
              100.0);

  const double inner =
      std::max(
          0.0,
          radius - edge);

  if (distance <= inner) {
    return 1.0F;
  }

  const double t =
      std::clamp(
          (distance - inner) /
              edge,
          0.0,
          1.0);

  const double smooth =
      t * t * t *
      (
          t *
          (
              t * 6.0 -
              15.0) +
          10.0);

  return static_cast<float>(
      1.0 - smooth);
}

void blend_rgba(
    std::uint8_t* destination,
    std::uint16_t channels,
    const std::uint8_t* source,
    float amount) {
  amount =
      std::clamp(
          amount,
          0.0F,
          1.0F);

  if (amount <= 0.0F) {
    return;
  }

  const float source_alpha =
      source[3] /
      255.0F;

  if (channels < 4) {
    const float alpha =
        amount *
        source_alpha;

    for (int channel = 0;
         channel < 3;
         ++channel) {
      destination[channel] =
          clamp_byte(
              source[channel] *
                  alpha +
              destination[channel] *
                  (1.0F - alpha));
    }

    return;
  }

  const float destination_alpha =
      destination[3] /
      255.0F;

  const float effective_source_alpha =
      source_alpha *
      amount;

  const float output_alpha =
      effective_source_alpha +
      destination_alpha *
          (1.0F -
           effective_source_alpha);

  if (output_alpha <= 0.0F) {
    return;
  }

  for (int channel = 0;
       channel < 3;
       ++channel) {
    destination[channel] =
        clamp_byte(
            (
                source[channel] *
                    effective_source_alpha +
                destination[channel] *
                    destination_alpha *
                    (1.0F -
                     effective_source_alpha)) /
            output_alpha);
  }

  destination[3] =
      clamp_byte(
          output_alpha *
          255.0F);
}

}  // namespace

RetouchController::RetouchController(
    patchy::Document& document)
    : document_(&document) {}

void RetouchController::set_source(
    int x,
    int y) {
  if (
      x < 0 ||
      y < 0 ||
      x >= document_->width() ||
      y >= document_->height()) {
    return;
  }

  source_ =
      std::pair<int, int>{
          x,
          y};
}

bool RetouchController::source_set() const noexcept {
  return source_.has_value();
}

const std::uint8_t*
RetouchController::sample(
    int x,
    int y) const {
  if (
      x < 0 ||
      y < 0 ||
      x >= width_ ||
      y >= height_ ||
      snapshot_.empty()) {
    return nullptr;
  }

  return
      snapshot_.data() +
      static_cast<std::size_t>(y) *
          static_cast<std::size_t>(
              stride_) +
      static_cast<std::size_t>(x) *
          4U;
}

std::array<double, 3>
RetouchController::ring_tone(
    int x,
    int y,
    int radius) const {
  constexpr std::array<
      std::array<int, 2>,
      8>
      directions{{
          {{-1, -1}},
          {{0, -1}},
          {{1, -1}},
          {{-1, 0}},
          {{1, 0}},
          {{-1, 1}},
          {{0, 1}},
          {{1, 1}},
      }};

  std::array<double, 3> result{};
  double weight = 0.0;

  for (const auto& direction :
       directions) {
    const int px =
        std::clamp(
            x +
                direction[0] *
                    radius,
            0,
            width_ - 1);

    const int py =
        std::clamp(
            y +
                direction[1] *
                    radius,
            0,
            height_ - 1);

    const auto* pixel =
        sample(
            px,
            py);

    if (pixel == nullptr) {
      continue;
    }

    const double alpha =
        pixel[3] /
        255.0;

    weight += alpha;

    for (int channel = 0;
         channel < 3;
         ++channel) {
      result[channel] +=
          pixel[channel] *
          alpha;
    }
  }

  if (weight > 0.000001) {
    for (double& channel :
         result) {
      channel /= weight;
    }

    return result;
  }

  const auto* center =
      sample(
          std::clamp(
              x,
              0,
              width_ - 1),
          std::clamp(
              y,
              0,
              height_ - 1));

  if (center != nullptr) {
    return {
        static_cast<double>(
            center[0]),
        static_cast<double>(
            center[1]),
        static_cast<double>(
            center[2])};
  }

  return {};
}

std::array<std::uint8_t, 4>
RetouchController::healing_sample(
    int source_x,
    int source_y,
    int destination_x,
    int destination_y,
    int radius) const {
  const auto source_tone =
      ring_tone(
          source_x,
          source_y,
          radius);

  const auto destination_tone =
      ring_tone(
          destination_x,
          destination_y,
          radius);

  const auto* source =
      sample(
          source_x,
          source_y);

  if (source == nullptr) {
    return {};
  }

  std::array<std::uint8_t, 4>
      result{};

  for (int channel = 0;
       channel < 3;
       ++channel) {
    result[channel] =
        clamp_byte(
            destination_tone[channel] +
            source[channel] -
            source_tone[channel]);
  }

  result[3] =
      source[3];

  return result;
}

patchy::Rect RetouchController::begin_stroke(
    RetouchMode mode,
    double x,
    double y,
    const std::vector<std::uint8_t>& rgba,
    int width,
    int height,
    int stride,
    RetouchBrushSettings settings,
    std::function<float(int, int)> selection_coverage) {
  if (
      (
          mode ==
              RetouchMode::Clone ||
          mode ==
              RetouchMode::Healing) &&
      !source_.has_value()) {
    return {};
  }

  if (
      width <= 0 ||
      height <= 0 ||
      stride < width * 4 ||
      rgba.empty()) {
    return {};
  }

  mode_ = mode;
  settings_ = settings;

  snapshot_ = rgba;
  width_ = width;
  height_ = height;
  stride_ = stride;

  selection_coverage_ =
      std::move(
          selection_coverage);

  stroke_caps_.clear();

  active_ = true;

  last_x_ = x;
  last_y_ = y;

  if (source_.has_value()) {
    offset_x_ =
        source_->first -
        static_cast<int>(
            std::lround(x));

    offset_y_ =
        source_->second -
        static_cast<int>(
            std::lround(y));
  }

  return paint_segment(
      x,
      y,
      x,
      y);
}

patchy::Rect RetouchController::stroke_to(
    double x,
    double y) {
  if (!active_) {
    return {};
  }

  const auto dirty =
      paint_segment(
          last_x_,
          last_y_,
          x,
          y);

  last_x_ = x;
  last_y_ = y;

  return dirty;
}

void RetouchController::end_stroke() {
  active_ = false;
  stroke_caps_.clear();
  snapshot_.clear();
}

patchy::Rect RetouchController::paint_segment(
    double x0,
    double y0,
    double x1,
    double y1) {
  const auto active =
      document_->active_layer_id();

  if (!active.has_value()) {
    return {};
  }

  auto* layer =
      document_->find_layer(
          *active);

  if (
      layer == nullptr ||
      layer->kind() !=
          patchy::LayerKind::Pixel ||
      layer->pixels().empty() ||
      layer->pixels().format().bit_depth !=
          patchy::BitDepth::UInt8 ||
      layer->pixels().format().channels < 3) {
    return {};
  }

  const double radius =
      std::max(
          0.5,
          settings_.size *
              0.5);

  patchy::Rect region{
      static_cast<std::int32_t>(
          std::floor(
              std::min(x0, x1) -
              radius -
              1.0)),
      static_cast<std::int32_t>(
          std::floor(
              std::min(y0, y1) -
              radius -
              1.0)),
      static_cast<std::int32_t>(
          std::ceil(
              std::abs(x1 - x0) +
              radius * 2.0 +
              2.0)),
      static_cast<std::int32_t>(
          std::ceil(
              std::abs(y1 - y0) +
              radius * 2.0 +
              2.0))};

  region =
      patchy::intersect_rect(
          region,
          patchy::Rect::from_size(
              document_->width(),
              document_->height()));

  if (region.empty()) {
    return {};
  }

  patchy::expand_layer_to_include_rect(
      *layer,
      region);

  region =
      patchy::intersect_rect(
          region,
          layer->bounds());

  if (region.empty()) {
    return {};
  }

  auto& pixels =
      layer->pixels();

  const auto bounds =
      layer->bounds();

  const auto channels =
      pixels.format().channels;

  const double dx =
      x1 - x0;

  const double dy =
      y1 - y0;

  const double length_squared =
      dx * dx +
      dy * dy;

  patchy::Rect dirty{};

  const float opacity =
      std::clamp(
          settings_.opacity,
          1,
          100) /
      100.0F;

  for (int y = region.y;
       y < region.y + region.height;
       ++y) {
    for (int x = region.x;
         x < region.x + region.width;
         ++x) {
      double t = 0.0;

      if (length_squared >
          0.000001) {
        t =
            std::clamp(
                (
                    (x - x0) * dx +
                    (y - y0) * dy) /
                    length_squared,
                0.0,
                1.0);
      }

      const double closest_x =
          x0 +
          dx * t;

      const double closest_y =
          y0 +
          dy * t;

      const double distance =
          std::hypot(
              x - closest_x,
              y - closest_y);

      float coverage =
          footprint(
              distance,
              radius,
              settings_.softness);

      if (coverage <= 0.0F) {
        continue;
      }

      if (selection_coverage_) {
        coverage *=
            std::clamp(
                selection_coverage_(
                    x,
                    y),
                0.0F,
                1.0F);
      }

      if (coverage <= 0.0F) {
        continue;
      }

      const float target =
          opacity *
          coverage;

      auto& previous =
          stroke_caps_[
              pixel_key(
                  x,
                  y)];

      if (
          target <=
          previous +
              0.0005F) {
        continue;
      }

      const float incremental =
          std::clamp(
              (
                  target -
                  previous) /
                  std::max(
                      0.0005F,
                      1.0F -
                          previous),
              0.0F,
              1.0F);

      previous = target;

      auto* destination =
          pixels.pixel(
              x - bounds.x,
              y - bounds.y);

      if (
          mode_ == RetouchMode::Blur ||
          mode_ == RetouchMode::Sharpen ||
          mode_ == RetouchMode::Dodge ||
          mode_ == RetouchMode::Burn ||
          mode_ == RetouchMode::Sponge) {
        const auto* center =
            sample(
                x,
                y);

        if (center == nullptr) {
          continue;
        }

        std::array<std::uint8_t, 3>
            adjusted{
                center[0],
                center[1],
                center[2]};

        if (
            mode_ == RetouchMode::Blur ||
            mode_ == RetouchMode::Sharpen) {
          constexpr std::array<int, 3>
              gaussian{
                  1,
                  2,
                  1};

          std::array<double, 3>
              premultiplied{};

          double alpha_weight = 0.0;

          for (int oy = -1;
               oy <= 1;
               ++oy) {
            for (int ox = -1;
                 ox <= 1;
                 ++ox) {
              const auto* neighbour =
                  sample(
                      std::clamp(
                          x + ox,
                          0,
                          width_ - 1),
                      std::clamp(
                          y + oy,
                          0,
                          height_ - 1));

              if (neighbour == nullptr) {
                continue;
              }

              const double weight =
                  gaussian[
                      static_cast<std::size_t>(
                          ox + 1)] *
                  gaussian[
                      static_cast<std::size_t>(
                          oy + 1)];

              const double alpha =
                  neighbour[3] /
                  255.0;

              alpha_weight +=
                  weight *
                  alpha;

              for (int channel = 0;
                   channel < 3;
                   ++channel) {
                premultiplied[channel] +=
                    weight *
                    alpha *
                    neighbour[channel];
              }
            }
          }

          if (alpha_weight > 0.000001) {
            for (int channel = 0;
                 channel < 3;
                 ++channel) {
              const double blurred =
                  premultiplied[channel] /
                  alpha_weight;

              adjusted[channel] =
                  mode_ ==
                          RetouchMode::Blur
                      ? clamp_byte(
                            blurred)
                      : clamp_byte(
                            center[channel] *
                                2.0 -
                            blurred);
            }
          }

        } else {
          const double red =
              center[0];

          const double green =
              center[1];

          const double blue =
              center[2];

          const double lightness =
              (
                  54.0 * red +
                  183.0 * green +
                  19.0 * blue) /
              (256.0 * 255.0);

          if (
              mode_ == RetouchMode::Dodge ||
              mode_ == RetouchMode::Burn) {
            // Igual que el frontend Qt por defecto:
            // medios tonos + preservar tonos.
            const double range_weight =
                1.0 -
                std::abs(
                    lightness * 2.0 -
                    1.0);

            const double source_lightness =
                lightness *
                255.0;

            const double target_lightness =
                mode_ ==
                        RetouchMode::Dodge
                    ? source_lightness +
                          (
                              255.0 -
                              source_lightness) *
                              range_weight
                    : source_lightness *
                          (
                              1.0 -
                              range_weight);

            const double delta =
                target_lightness -
                source_lightness;

            for (int channel = 0;
                 channel < 3;
                 ++channel) {
              adjusted[channel] =
                  clamp_byte(
                      center[channel] +
                      delta);
            }

          } else if (
              mode_ ==
              RetouchMode::Sponge) {
            const double maximum =
                std::max({
                    static_cast<double>(
                        center[0]),
                    static_cast<double>(
                        center[1]),
                    static_cast<double>(
                        center[2])});

            const double minimum =
                std::min({
                    static_cast<double>(
                        center[0]),
                    static_cast<double>(
                        center[1]),
                    static_cast<double>(
                        center[2])});

            const double saturation =
                (
                    maximum -
                    minimum) /
                255.0;

            // Valor por defecto del frontend Qt:
            // Desaturar + Vibrance.
            const double vibrance =
                1.0 -
                saturation;

            const double chroma_scale =
                1.0 -
                vibrance;

            const double luma =
                lightness *
                255.0;

            for (int channel = 0;
                 channel < 3;
                 ++channel) {
              adjusted[channel] =
                  clamp_byte(
                      luma +
                      (
                          center[channel] -
                          luma) *
                          chroma_scale);
            }
          }
        }

        for (int channel = 0;
             channel < 3;
             ++channel) {
          destination[channel] =
              clamp_byte(
                  adjusted[channel] *
                      incremental +
                  destination[channel] *
                      (
                          1.0F -
                          incremental));
        }

      } else {
        const int source_x =
            x +
            offset_x_;

        const int source_y =
            y +
            offset_y_;

        const auto* source =
            sample(
                source_x,
                source_y);

        if (source == nullptr) {
          continue;
        }

        std::array<std::uint8_t, 4>
            healed{};

        if (mode_ ==
            RetouchMode::Healing) {
          healed =
              healing_sample(
                  source_x,
                  source_y,
                  x,
                  y,
                  std::max(
                      1,
                      settings_.size /
                          4));

          source =
              healed.data();
        }

        blend_rgba(
            destination,
            channels,
            source,
            incremental);
      }

      const patchy::Rect pixel_rect{
          x,
          y,
          1,
          1};

      dirty =
          dirty.empty()
              ? pixel_rect
              : patchy::unite_rect(
                    dirty,
                    pixel_rect);
    }
  }

  return dirty;
}

void RetouchController::draw_source_marker(
    cairo_t* cr,
    double origin_x,
    double origin_y,
    double zoom) const {
  if (!source_.has_value()) {
    return;
  }

  const double x =
      origin_x +
      source_->first *
          zoom;

  const double y =
      origin_y +
      source_->second *
          zoom;

  cairo_save(cr);

  cairo_set_source_rgba(
      cr,
      0.25,
      0.68,
      1.0,
      0.95);

  cairo_set_line_width(
      cr,
      1.2);

  cairo_move_to(
      cr,
      x - 6.0,
      y);

  cairo_line_to(
      cr,
      x + 6.0,
      y);

  cairo_move_to(
      cr,
      x,
      y - 6.0);

  cairo_line_to(
      cr,
      x,
      y + 6.0);

  cairo_stroke(cr);
  cairo_restore(cr);
}

}  // namespace lienzo::gnome
