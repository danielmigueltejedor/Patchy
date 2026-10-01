#include "core/retouch_brush.hpp"

#include "core/blend_math.hpp"
#include "core/palette.hpp"
#include "core/rect_utils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace patchy {
namespace {

constexpr std::array<std::array<int, 2>, 8> kRingDirections{{
    {{-1, -1}}, {{0, -1}}, {{1, -1}}, {{-1, 0}}, {{1, 0}}, {{-1, 1}}, {{0, 1}}, {{1, 1}},
}};

[[nodiscard]] bool plane_usable(const RgbaPlane& plane) noexcept {
  return plane.data != nullptr && plane.width > 0 && plane.height > 0 && plane.stride_bytes > 0 &&
         plane.channels >= 3;
}

[[nodiscard]] std::array<std::uint8_t, 4> read_pixel(const RgbaPlane& plane, std::int32_t local_x,
                                                     std::int32_t local_y) noexcept {
  const auto* pixel = plane.data + static_cast<std::size_t>(local_y) * static_cast<std::size_t>(plane.stride_bytes) +
                      static_cast<std::size_t>(local_x) * plane.channels;
  return {pixel[0], pixel[1], pixel[2], plane.channels >= 4 ? pixel[3] : std::uint8_t{255}};
}

[[nodiscard]] std::array<std::uint8_t, 4> sample_clamped(const RgbaPlane& plane, std::int32_t x,
                                                         std::int32_t y) noexcept {
  if (!plane_usable(plane)) {
    return {0, 0, 0, 0};
  }
  const auto left = plane.origin_x;
  const auto top = plane.origin_y;
  const auto right = left + plane.width - 1;
  const auto bottom = top + plane.height - 1;
  return read_pixel(plane, std::clamp(x, left, right) - left, std::clamp(y, top, bottom) - top);
}

[[nodiscard]] const std::uint8_t* sample_inside(const RgbaPlane& plane, std::int32_t x, std::int32_t y) noexcept {
  if (!plane_usable(plane) || x < plane.origin_x || y < plane.origin_y || x >= plane.origin_x + plane.width ||
      y >= plane.origin_y + plane.height) {
    return nullptr;
  }
  return plane.data + static_cast<std::size_t>(y - plane.origin_y) * static_cast<std::size_t>(plane.stride_bytes) +
         static_cast<std::size_t>(x - plane.origin_x) * plane.channels;
}

[[nodiscard]] float round_brush_coverage(double distance_squared, int radius, int softness) noexcept {
  if (radius <= 0) {
    return distance_squared <= 0.0 ? 1.0F : 0.0F;
  }
  const auto radius_squared = static_cast<double>(radius) * static_cast<double>(radius);
  if (distance_squared > radius_squared) {
    return 0.0F;
  }
  softness = std::clamp(softness, 0, 100);
  if (softness <= 0) {
    return 1.0F;
  }
  const auto edge_width = std::max(1.0, static_cast<double>(radius) * static_cast<double>(softness) / 100.0);
  const auto inner_radius = std::max(0.0, static_cast<double>(radius) - edge_width);
  const auto distance = std::sqrt(distance_squared);
  if (distance <= inner_radius) {
    return 1.0F;
  }
  const auto t = std::clamp((distance - inner_radius) / edge_width, 0.0, 1.0);
  const auto smooth = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
  return static_cast<float>(1.0 - smooth);
}

// Procedural round footprint used by CanvasWidget's clone and local-adjustment
// loops. Square tips are not consulted; those loops never were.
[[nodiscard]] float retouch_footprint(double distance_x, double distance_y, int radius, const EditOptions& options) noexcept {
  const auto roundness = std::clamp(options.brush_roundness, 1, 100);
  if (radius <= 0 || roundness >= 99) {
    return round_brush_coverage(distance_x * distance_x + distance_y * distance_y, radius, options.brush_softness);
  }
  const auto major_radius = static_cast<double>(radius);
  const auto minor_radius = std::max(0.5, major_radius * static_cast<double>(roundness) / 100.0);
  constexpr double kPi = 3.14159265358979323846;
  const auto angle = options.brush_angle_degrees * kPi / 180.0;
  const auto c = std::cos(angle);
  const auto s = std::sin(angle);
  const auto major_axis = c * distance_x + s * distance_y;
  const auto minor_axis = -s * distance_x + c * distance_y;
  const auto normalized_distance_squared = (major_axis * major_axis) / (major_radius * major_radius) +
                                           (minor_axis * minor_axis) / (minor_radius * minor_radius);
  return round_brush_coverage(normalized_distance_squared * major_radius * major_radius, radius,
                              options.brush_softness);
}

[[nodiscard]] Layer* retouch_layer(Document& document, LayerId layer_id) noexcept {
  auto* layer = document.find_layer(layer_id);
  if (layer == nullptr || layer->kind() != LayerKind::Pixel) {
    return nullptr;
  }
  const auto& pixels = std::as_const(*layer).pixels();
  if (pixels.empty() || pixels.format().bit_depth != BitDepth::UInt8 || pixels.format().channels < 3) {
    return nullptr;
  }
  return layer;
}

[[nodiscard]] Rect document_rect(const Document& document) noexcept {
  return Rect{0, 0, document.width(), document.height()};
}

[[nodiscard]] Rect segment_bounds(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) noexcept {
  const auto left = std::min(x0, x1);
  const auto top = std::min(y0, y1);
  return Rect{left, top, std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1};
}

template <typename Callback>
void visit_pixel_line(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, Callback&& callback) {
  const auto dx = std::abs(x1 - x0);
  const auto sx = x0 < x1 ? 1 : -1;
  const auto dy = -std::abs(y1 - y0);
  const auto sy = y0 < y1 ? 1 : -1;
  auto error = dx + dy;
  while (true) {
    callback(x0, y0);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const auto doubled_error = error * 2;
    if (doubled_error >= dy) {
      error += dy;
      x0 += sx;
    }
    if (doubled_error <= dx) {
      error += dx;
      y0 += sy;
    }
  }
}

[[nodiscard]] float selection_factor(
    const std::function<float(std::int32_t, std::int32_t)>& selection_coverage, std::int32_t x, std::int32_t y) {
  if (!selection_coverage) {
    return 1.0F;
  }
  return std::clamp(selection_coverage(x, y), 0.0F, 1.0F);
}

[[nodiscard]] float capped_coverage(std::unordered_map<std::uint64_t, float>* caps, std::int32_t x, std::int32_t y,
                                    float coverage, float amount) noexcept {
  if (caps == nullptr) {
    return coverage;
  }
  return capped_stroke_coverage(*caps, x, y, coverage, amount);
}

}  // namespace

int healing_tone_radius(int brush_size, int diffusion) noexcept {
  diffusion = std::clamp(diffusion, 1, 7);
  return std::max(1, (brush_size * (9 - diffusion) + 15) / 16);
}

float capped_stroke_coverage(std::unordered_map<std::uint64_t, float>& caps, std::int32_t x, std::int32_t y,
                             float coverage, float amount) noexcept {
  amount = std::clamp(amount, 1.0F / 255.0F, 1.0F);
  const auto target_alpha = std::clamp(amount * std::clamp(coverage, 0.0F, 1.0F), 0.0F, 1.0F);
  if (target_alpha <= 0.0F) {
    return 0.0F;
  }

  const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32U) | static_cast<std::uint32_t>(x);
  auto& previous_alpha = caps[key];
  if (target_alpha <= previous_alpha + 0.0005F) {
    return 0.0F;
  }

  const auto incremental_alpha = (target_alpha - previous_alpha) / std::max(0.0005F, 1.0F - previous_alpha);
  previous_alpha = target_alpha;
  return std::clamp(incremental_alpha / amount, 0.0F, 1.0F);
}

std::array<double, 3> healing_ring_tone(const RgbaPlane& snapshot, std::int32_t x, std::int32_t y,
                                        int radius) noexcept {
  std::array<double, 3> sum{};
  double alpha_weight = 0.0;
  for (const auto& direction : kRingDirections) {
    const auto sample = sample_clamped(snapshot, x + direction[0] * radius, y + direction[1] * radius);
    const auto alpha = static_cast<double>(sample[3]) / 255.0;
    alpha_weight += alpha;
    for (std::size_t channel = 0; channel < sum.size(); ++channel) {
      sum[channel] += static_cast<double>(sample[channel]) * alpha;
    }
  }
  if (alpha_weight > std::numeric_limits<double>::epsilon()) {
    for (auto& channel : sum) {
      channel /= alpha_weight;
    }
    return sum;
  }
  const auto center = sample_clamped(snapshot, x, y);
  return {static_cast<double>(center[0]), static_cast<double>(center[1]), static_cast<double>(center[2])};
}

std::array<std::uint8_t, 4> healing_sample(const RgbaPlane& snapshot, std::int32_t source_x, std::int32_t source_y,
                                           std::int32_t destination_x, std::int32_t destination_y,
                                           int tone_radius) noexcept {
  const auto* source_pixel = sample_inside(snapshot, source_x, source_y);
  if (source_pixel == nullptr) {
    return {};
  }
  const auto source_tone = healing_ring_tone(snapshot, source_x, source_y, tone_radius);
  const auto destination_tone = healing_ring_tone(snapshot, destination_x, destination_y, tone_radius);
  std::array<std::uint8_t, 4> result{};
  for (std::size_t channel = 0; channel < 3; ++channel) {
    result[channel] = clamp_byte(static_cast<float>(destination_tone[channel] + static_cast<double>(source_pixel[channel]) -
                                                    source_tone[channel]));
  }
  result[3] = snapshot.channels >= 4 ? source_pixel[3] : std::uint8_t{255};
  return result;
}

void blend_straight_rgba(std::uint8_t* dst, const std::uint8_t* src, float amount) noexcept {
  amount = std::clamp(amount, 0.0F, 1.0F);
  if (amount <= 0.0F || dst == nullptr || src == nullptr) {
    return;
  }
  if (amount >= 0.999F) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    return;
  }

  const auto source_alpha = static_cast<float>(src[3]) / 255.0F;
  const auto destination_alpha = static_cast<float>(dst[3]) / 255.0F;
  const auto out_alpha = source_alpha * amount + destination_alpha * (1.0F - amount);
  if (out_alpha <= 0.0F) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = 0;
    return;
  }

  for (int channel = 0; channel < 3; ++channel) {
    const auto source_premultiplied = static_cast<float>(src[channel]) * source_alpha;
    const auto destination_premultiplied = static_cast<float>(dst[channel]) * destination_alpha;
    dst[channel] =
        clamp_byte((source_premultiplied * amount + destination_premultiplied * (1.0F - amount)) / out_alpha);
  }
  dst[3] = clamp_byte(out_alpha * 255.0F);
}

std::array<std::uint8_t, 3> local_adjustment_rgb(const RgbaPlane& snapshot, std::int32_t x, std::int32_t y,
                                                 LocalAdjustment adjustment,
                                                 const LocalAdjustmentSettings& settings) noexcept {
  const auto center = sample_clamped(snapshot, x, y);
  std::array<std::uint8_t, 3> result{center[0], center[1], center[2]};

  if (adjustment == LocalAdjustment::Blur || adjustment == LocalAdjustment::Sharpen) {
    constexpr std::array<int, 3> kGaussian{1, 2, 1};
    std::array<double, 3> premultiplied{};
    double alpha_weight = 0.0;
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
      for (int offset_x = -1; offset_x <= 1; ++offset_x) {
        const auto sample = sample_clamped(snapshot, x + offset_x, y + offset_y);
        const auto weight = static_cast<double>(kGaussian[static_cast<std::size_t>(offset_x + 1)] *
                                                kGaussian[static_cast<std::size_t>(offset_y + 1)]);
        const auto alpha = static_cast<double>(sample[3]) / 255.0;
        alpha_weight += weight * alpha;
        for (std::size_t channel = 0; channel < premultiplied.size(); ++channel) {
          premultiplied[channel] += weight * alpha * static_cast<double>(sample[channel]);
        }
      }
    }
    if (alpha_weight <= std::numeric_limits<double>::epsilon()) {
      return result;
    }
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
      const auto blurred = premultiplied[channel] / alpha_weight;
      result[channel] = adjustment == LocalAdjustment::Blur
                            ? clamp_byte(static_cast<float>(blurred))
                            : clamp_byte(static_cast<float>(static_cast<double>(center[channel]) * 2.0 - blurred));
    }
    return result;
  }

  const auto red = static_cast<double>(center[0]);
  const auto green = static_cast<double>(center[1]);
  const auto blue = static_cast<double>(center[2]);
  const auto lightness = (54.0 * red + 183.0 * green + 19.0 * blue) / (256.0 * 255.0);
  if (adjustment == LocalAdjustment::Dodge || adjustment == LocalAdjustment::Burn) {
    double range_weight = 1.0;
    switch (settings.tone_range) {
      case LocalToneRange::Shadows:
        range_weight = 1.0 - lightness;
        break;
      case LocalToneRange::Midtones:
        range_weight = 1.0 - std::abs(lightness * 2.0 - 1.0);
        break;
      case LocalToneRange::Highlights:
        range_weight = lightness;
        break;
    }
    const auto source_lightness = lightness * 255.0;
    const auto target_lightness = adjustment == LocalAdjustment::Dodge
                                      ? source_lightness + (255.0 - source_lightness) * range_weight
                                      : source_lightness * (1.0 - range_weight);
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
      const auto value = static_cast<double>(center[channel]);
      const auto adjusted = settings.protect_tones ? value + (target_lightness - source_lightness)
                                                   : (adjustment == LocalAdjustment::Dodge
                                                          ? value + (255.0 - value) * range_weight
                                                          : value * (1.0 - range_weight));
      result[channel] = clamp_byte(static_cast<float>(adjusted));
    }
    return result;
  }

  if (adjustment == LocalAdjustment::Sponge) {
    const auto maximum = static_cast<double>(std::max({center[0], center[1], center[2]}));
    const auto minimum = static_cast<double>(std::min({center[0], center[1], center[2]}));
    const auto saturation = (maximum - minimum) / 255.0;
    const auto vibrance_scale = settings.sponge_vibrance ? 1.0 - saturation : 1.0;
    const auto luma = lightness * 255.0;
    const auto chroma_scale =
        settings.sponge_mode == SpongeMode::Saturate ? 1.0 + vibrance_scale : 1.0 - vibrance_scale;
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
      result[channel] = clamp_byte(static_cast<float>(luma + (static_cast<double>(center[channel]) - luma) * chroma_scale));
    }
  }
  return result;
}

Rect local_adjustment_brush_segment(Document& document, LayerId layer_id, std::int32_t x0, std::int32_t y0,
                                    std::int32_t x1, std::int32_t y1, const EditOptions& options,
                                    const RgbaPlane& snapshot, LocalAdjustment adjustment,
                                    const LocalAdjustmentSettings& settings, float strength,
                                    std::unordered_map<std::uint64_t, float>* coverage_caps,
                                    const std::function<float(std::int32_t, std::int32_t)>& selection_coverage) {
  auto* layer = retouch_layer(document, layer_id);
  if (layer == nullptr || !plane_usable(snapshot)) {
    return {};
  }

  const auto radius = std::max(1, options.brush_size) / 2;
  const auto snapshot_rect = Rect{snapshot.origin_x, snapshot.origin_y, snapshot.width, snapshot.height};
  Rect stroke_rect = segment_bounds(x0, y0, x1, y1);
  if (radius > 0) {
    const auto left = std::min(x0, x1) - radius;
    const auto top = std::min(y0, y1) - radius;
    const auto right = std::max(x0, x1) + radius + 1;
    const auto bottom = std::max(y0, y1) + radius + 1;
    stroke_rect = Rect{left, top, right - left, bottom - top};
  }
  stroke_rect = intersect_rect(intersect_rect(stroke_rect, document_rect(document)), snapshot_rect);
  if (stroke_rect.empty()) {
    return {};
  }

  auto& pixels = layer->pixels();
  const auto detached = pixels.data();
  (void)detached;
  const auto channels = pixels.format().channels;
  const auto* palette_snap = options.palette_snap;
  Rect dirty;
  const auto adjust_pixel = [&](std::int32_t x, std::int32_t y, float coverage) {
    if (!stroke_rect.contains(x, y)) {
      return;
    }
    coverage *= selection_factor(selection_coverage, x, y);
    if (coverage <= 0.0F) {
      return;
    }
    if (palette_snap != nullptr) {
      if (coverage < palette_snap->coverage_threshold) {
        return;
      }
      coverage = 1.0F;
    }
    coverage = capped_coverage(coverage_caps, x, y, coverage, strength);
    if (coverage <= 0.0F) {
      return;
    }

    const auto local_x = x - snapshot.origin_x;
    const auto local_y = y - snapshot.origin_y;
    if (local_x < 0 || local_y < 0 || local_x >= pixels.width() || local_y >= pixels.height()) {
      return;
    }
    auto* destination = pixels.pixel(local_x, local_y);
    if (channels >= 4 && destination[3] == 0) {
      return;
    }
    const auto adjusted = local_adjustment_rgb(snapshot, x, y, adjustment, settings);
    const auto amount = strength * coverage;
    const auto before_r = destination[0];
    const auto before_g = destination[1];
    const auto before_b = destination[2];
    for (std::size_t channel = 0; channel < adjusted.size(); ++channel) {
      destination[channel] = clamp_byte(static_cast<float>(adjusted[channel]) * amount +
                                        static_cast<float>(destination[channel]) * (1.0F - amount));
    }
    if (palette_snap != nullptr) {
      snap_pixel_to_palette(destination, channels, *palette_snap);
    }
    if (destination[0] != before_r || destination[1] != before_g || destination[2] != before_b) {
      dirty = unite_rect(dirty, Rect{x, y, 1, 1});
    }
  };

  if (radius == 0) {
    visit_pixel_line(x0, y0, x1, y1, [&](std::int32_t x, std::int32_t y) { adjust_pixel(x, y, 1.0F); });
    return dirty;
  }

  const auto dx = static_cast<double>(x1 - x0);
  const auto dy = static_cast<double>(y1 - y0);
  const auto segment_length_squared = dx * dx + dy * dy;
  for (std::int32_t y = stroke_rect.y; y < stroke_rect.y + stroke_rect.height; ++y) {
    for (std::int32_t x = stroke_rect.x; x < stroke_rect.x + stroke_rect.width; ++x) {
      const auto along = segment_length_squared <= std::numeric_limits<double>::epsilon()
                             ? 0.0
                             : std::clamp((static_cast<double>(x - x0) * dx + static_cast<double>(y - y0) * dy) /
                                              segment_length_squared,
                                          0.0, 1.0);
      const auto closest_x = static_cast<double>(x0) + dx * along;
      const auto closest_y = static_cast<double>(y0) + dy * along;
      adjust_pixel(x, y,
                   retouch_footprint(static_cast<double>(x) - closest_x, static_cast<double>(y) - closest_y, radius,
                                     options));
    }
    if (options.progress_callback) {
      options.progress_callback();
    }
  }
  return dirty;
}

Rect clone_stamp_brush_segment(Document& document, LayerId layer_id, std::int32_t x0, std::int32_t y0,
                               std::int32_t x1, std::int32_t y1, const EditOptions& options, const RgbaPlane& source,
                               const CloneStampSettings& settings, float opacity,
                               std::unordered_map<std::uint64_t, float>* coverage_caps,
                               const std::function<float(std::int32_t, std::int32_t)>& selection_coverage) {
  auto* layer = retouch_layer(document, layer_id);
  if (layer == nullptr || !plane_usable(source)) {
    return {};
  }

  const auto radius = std::max(1, options.brush_size) / 2;
  const auto* palette_snap = options.palette_snap;
  const auto lock_transparent_pixels = options.lock_transparent_pixels;

  const auto paint_at = [&](PixelBuffer& pixels, const Rect& bounds, std::uint16_t channels, std::int32_t x,
                            std::int32_t y, float coverage, Rect& dirty) {
    if (!document_rect(document).contains(x, y) || !bounds.contains(x, y)) {
      return;
    }
    coverage *= selection_factor(selection_coverage, x, y);
    if (coverage <= 0.0F) {
      return;
    }
    if (palette_snap != nullptr) {
      if (coverage < palette_snap->coverage_threshold) {
        return;
      }
      coverage = 1.0F;
    }

    const auto source_x = x + settings.offset_x;
    const auto source_y = y + settings.offset_y;
    const auto* src = sample_inside(source, source_x, source_y);
    if (src == nullptr) {
      return;
    }

    auto* dst = pixels.pixel(x - bounds.x, y - bounds.y);
    if (lock_transparent_pixels && channels >= 4 && dst[3] == 0) {
      return;
    }
    coverage = capped_coverage(coverage_caps, x, y, coverage, opacity);
    if (coverage <= 0.0F) {
      return;
    }

    std::array<std::uint8_t, 4> healed{};
    if (settings.healing) {
      healed = healing_sample(source, source_x, source_y, x, y, settings.tone_radius);
      src = healed.data();
    }
    const auto covered_opacity = opacity * coverage;
    if (channels >= 4 && !lock_transparent_pixels) {
      blend_straight_rgba(dst, src, covered_opacity);
    } else {
      const auto source_alpha = source.channels >= 4 || settings.healing ? src[3] : std::uint8_t{255};
      const auto effective_opacity = covered_opacity * (static_cast<float>(source_alpha) / 255.0F);
      if (effective_opacity <= 0.0F) {
        return;
      }
      dst[0] = clamp_byte(static_cast<float>(src[0]) * effective_opacity +
                          static_cast<float>(dst[0]) * (1.0F - effective_opacity));
      dst[1] = clamp_byte(static_cast<float>(src[1]) * effective_opacity +
                          static_cast<float>(dst[1]) * (1.0F - effective_opacity));
      dst[2] = clamp_byte(static_cast<float>(src[2]) * effective_opacity +
                          static_cast<float>(dst[2]) * (1.0F - effective_opacity));
    }
    if (palette_snap != nullptr) {
      snap_pixel_to_palette(dst, channels, *palette_snap);
    }
    dirty = unite_rect(dirty, Rect{x, y, 1, 1});
  };

  if (radius == 0) {
    auto stroke_rect = intersect_rect(segment_bounds(x0, y0, x1, y1), document_rect(document));
    if (stroke_rect.empty()) {
      return {};
    }
    if (!lock_transparent_pixels) {
      expand_layer_to_include_rect(*layer, stroke_rect);
    }
    auto& pixels = layer->pixels();
    const auto detached = pixels.data();
    (void)detached;
    const auto bounds = layer->bounds();
    const auto channels = pixels.format().channels;
    Rect dirty;
    visit_pixel_line(x0, y0, x1, y1, [&](std::int32_t x, std::int32_t y) {
      paint_at(pixels, bounds, channels, x, y, 1.0F, dirty);
    });
    return dirty;
  }

  const auto left = std::min(x0, x1) - radius;
  const auto top = std::min(y0, y1) - radius;
  const auto right = std::max(x0, x1) + radius + 1;
  const auto bottom = std::max(y0, y1) + radius + 1;
  auto stroke_rect = intersect_rect(Rect{left, top, right - left, bottom - top}, document_rect(document));
  if (stroke_rect.empty()) {
    return {};
  }
  if (!lock_transparent_pixels) {
    expand_layer_to_include_rect(*layer, stroke_rect);
  }
  auto& pixels = layer->pixels();
  const auto detached = pixels.data();
  (void)detached;
  const auto bounds = layer->bounds();
  const auto channels = pixels.format().channels;
  stroke_rect = intersect_rect(stroke_rect, bounds);
  if (stroke_rect.empty()) {
    return {};
  }

  const auto dx = x1 - x0;
  const auto dy = y1 - y0;
  const auto segment_length_squared = static_cast<double>(dx) * static_cast<double>(dx) +
                                      static_cast<double>(dy) * static_cast<double>(dy);
  Rect dirty;
  for (std::int32_t y = stroke_rect.y; y < stroke_rect.y + stroke_rect.height; ++y) {
    for (std::int32_t x = stroke_rect.x; x < stroke_rect.x + stroke_rect.width; ++x) {
      const auto along =
          segment_length_squared <= 0.0
              ? 0.0
              : std::clamp((static_cast<double>(x - x0) * static_cast<double>(dx) +
                            static_cast<double>(y - y0) * static_cast<double>(dy)) /
                               segment_length_squared,
                           0.0, 1.0);
      const auto closest_x = static_cast<double>(x0) + static_cast<double>(dx) * along;
      const auto closest_y = static_cast<double>(y0) + static_cast<double>(dy) * along;
      const auto coverage = retouch_footprint(static_cast<double>(x) - closest_x, static_cast<double>(y) - closest_y,
                                              radius, options);
      paint_at(pixels, bounds, channels, x, y, coverage, dirty);
    }
    if (options.progress_callback) {
      options.progress_callback();
    }
  }
  return dirty;
}

}  // namespace patchy
