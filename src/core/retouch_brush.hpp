#pragma once

#include "core/pixel_tools.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace patchy {

// Straight RGBA8 view. `channels` is 3 or 4. Alpha is treated as opaque when
// channels is 3. Clone and healing sample a document-sized plane (origin 0, 0)
// and skip coordinates outside it. Local adjustment samples a layer snapshot
// and clamps into that plane, matching CanvasWidget.
struct RgbaPlane {
  const std::uint8_t* data{nullptr};
  std::int32_t origin_x{0};
  std::int32_t origin_y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  std::int32_t stride_bytes{0};
  std::uint16_t channels{4};
};

enum class LocalAdjustment : std::uint8_t { Blur, Sharpen, Dodge, Burn, Sponge };

enum class LocalToneRange : std::uint8_t { Shadows, Midtones, Highlights };

enum class SpongeMode : std::uint8_t { Saturate, Desaturate };

struct LocalAdjustmentSettings {
  LocalToneRange tone_range{LocalToneRange::Midtones};
  bool protect_tones{true};
  SpongeMode sponge_mode{SpongeMode::Desaturate};
  bool sponge_vibrance{true};
};

struct CloneStampSettings {
  bool healing{false};
  int tone_radius{1};
  std::int32_t offset_x{0};
  std::int32_t offset_y{0};
};

// Diffusion is Photoshop's 1..7 Healing Brush setting. The default in both
// canvases is 5.
[[nodiscard]] int healing_tone_radius(int brush_size, int diffusion) noexcept;

// Per-stroke opacity cap shared with CanvasWidget::capped_stroke_coverage.
// `amount` is the brush opacity or the adjustment strength, in 0..1.
[[nodiscard]] float capped_stroke_coverage(std::unordered_map<std::uint64_t, float>& caps, std::int32_t x,
                                           std::int32_t y, float coverage, float amount) noexcept;

[[nodiscard]] std::array<double, 3> healing_ring_tone(const RgbaPlane& snapshot, std::int32_t x, std::int32_t y,
                                                      int radius) noexcept;

// Classic frequency-separation healing: sampled detail carried into the
// destination's local tone. Fixed local samples only. Not patch search,
// synthesis, or a gradient-domain solve. See docs/healing.md.
[[nodiscard]] std::array<std::uint8_t, 4> healing_sample(const RgbaPlane& snapshot, std::int32_t source_x,
                                                         std::int32_t source_y, std::int32_t destination_x,
                                                         std::int32_t destination_y, int tone_radius) noexcept;

void blend_straight_rgba(std::uint8_t* dst, const std::uint8_t* src, float amount) noexcept;

// Fixed local color transform. The brush footprint alone chooses the pixel.
// Do not add edge ranking, patch matching, deconvolution, automatic boundary
// isolation, or stroke-start color classification here.
[[nodiscard]] std::array<std::uint8_t, 3> local_adjustment_rgb(const RgbaPlane& snapshot, std::int32_t x,
                                                               std::int32_t y, LocalAdjustment adjustment,
                                                               const LocalAdjustmentSettings& settings) noexcept;

// `strength` and clone `opacity` are 0..1. `coverage_caps` is the stroke's
// running cap; null skips the cap. `selection_coverage` multiplies the
// footprint; an empty function leaves the footprint unchanged.
[[nodiscard]] Rect local_adjustment_brush_segment(
    Document& document, LayerId layer_id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
    const EditOptions& options, const RgbaPlane& snapshot, LocalAdjustment adjustment,
    const LocalAdjustmentSettings& settings, float strength,
    std::unordered_map<std::uint64_t, float>* coverage_caps = nullptr,
    const std::function<float(std::int32_t, std::int32_t)>& selection_coverage = {});

[[nodiscard]] Rect clone_stamp_brush_segment(
    Document& document, LayerId layer_id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
    const EditOptions& options, const RgbaPlane& source, const CloneStampSettings& settings, float opacity,
    std::unordered_map<std::uint64_t, float>* coverage_caps = nullptr,
    const std::function<float(std::int32_t, std::int32_t)>& selection_coverage = {});

}  // namespace patchy
