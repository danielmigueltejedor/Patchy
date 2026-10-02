#include "ui-gnome/tools/retouch_controller.hpp"

#include "render/compositor.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lienzo::gnome {

namespace {

patchy::LocalAdjustment adjustment_for(
    RetouchMode mode) {
  switch (mode) {
    case RetouchMode::Sharpen:
      return patchy::LocalAdjustment::Sharpen;

    case RetouchMode::Dodge:
      return patchy::LocalAdjustment::Dodge;

    case RetouchMode::Burn:
      return patchy::LocalAdjustment::Burn;

    case RetouchMode::Sponge:
      return patchy::LocalAdjustment::Sponge;

    case RetouchMode::Blur:
    default:
      return patchy::LocalAdjustment::Blur;
  }
}

bool is_adjustment(
    RetouchMode mode) {
  return
      mode == RetouchMode::Blur ||
      mode == RetouchMode::Sharpen ||
      mode == RetouchMode::Dodge ||
      mode == RetouchMode::Burn ||
      mode == RetouchMode::Sponge;
}

}  // namespace

RetouchController::RetouchController(
    patchy::Document& document)
    : document_(&document) {}

void RetouchController::set_adjustment_settings(
    const patchy::LocalAdjustmentSettings& settings) {
  adjustment_ = settings;
}

void RetouchController::set_healing_diffusion(
    int diffusion) {
  healing_diffusion_ = std::clamp(diffusion, 1, 7);
}

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

  aligned_offset_set_ = false;
}

bool RetouchController::source_set() const noexcept {
  return source_.has_value();
}

patchy::RgbaPlane RetouchController::plane() const noexcept {
  patchy::RgbaPlane view;

  view.data = snapshot_.data();
  view.origin_x = origin_x_;
  view.origin_y = origin_y_;
  view.width = width_;
  view.height = height_;
  view.stride_bytes = stride_;
  view.channels = channels_;

  return view;
}

bool RetouchController::copy_layer_snapshot() {
  if (
      !document_->active_layer_id()
           .has_value()) {
    return false;
  }

  auto* layer =
      document_->find_layer(
          *document_->active_layer_id());

  if (
      layer == nullptr ||
      layer->kind() !=
          patchy::LayerKind::Pixel) {
    return false;
  }

  const auto& pixels =
      std::as_const(*layer)
          .pixels();

  if (
      pixels.empty() ||
      pixels.format().bit_depth !=
          patchy::BitDepth::UInt8 ||
      pixels.format().channels < 3) {
    return false;
  }

  const auto bytes =
      pixels.data();

  snapshot_.assign(
      bytes.begin(),
      bytes.end());

  const auto bounds =
      layer->bounds();

  origin_x_ = bounds.x;
  origin_y_ = bounds.y;
  width_ = pixels.width();
  height_ = pixels.height();
  stride_ =
      static_cast<int>(
          pixels.stride_bytes());
  channels_ =
      pixels.format().channels;

  return
      !snapshot_.empty() &&
      stride_ > 0;
}

bool RetouchController::copy_document_composite() {
  std::vector<std::uint8_t> alpha;
  const auto rgb =
      patchy::Compositor{}
          .flatten_rgb8(
              std::as_const(*document_),
              &alpha);

  if (
      rgb.empty() ||
      rgb.format().channels < 3) {
    return false;
  }

  const int width = rgb.width();
  const int height = rgb.height();
  snapshot_.assign(
      static_cast<std::size_t>(width) *
          static_cast<std::size_t>(height) *
          4,
      0);

  for (int y = 0; y < height; ++y) {
    const auto row =
        std::as_const(rgb).row(y);
    auto* dst =
        snapshot_.data() +
        static_cast<std::size_t>(y) *
            static_cast<std::size_t>(width) *
            4;

    for (int x = 0; x < width; ++x) {
      const auto* src =
          row.data() +
          static_cast<std::size_t>(x) * 3;
      const std::size_t index =
          static_cast<std::size_t>(y) *
              static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);
      dst[x * 4] = src[0];
      dst[x * 4 + 1] = src[1];
      dst[x * 4 + 2] = src[2];
      dst[x * 4 + 3] =
          index < alpha.size() ? alpha[index] : 255;
    }
  }

  origin_x_ = 0;
  origin_y_ = 0;
  width_ = width;
  height_ = height;
  stride_ = width * 4;
  channels_ = 4;
  return !snapshot_.empty();
}

patchy::Rect RetouchController::begin_stroke(
    RetouchMode mode,
    double x,
    double y,
    RetouchBrushSettings settings,
    std::function<float(int, int)> selection_coverage) {
  if (
      (
          mode == RetouchMode::Clone ||
          mode == RetouchMode::Healing) &&
      !source_.has_value()) {
    return {};
  }

  mode_ = mode;
  settings_ = settings;
  selection_coverage_ =
      std::move(selection_coverage);
  stroke_caps_.clear();
  origin_x_ = 0;
  origin_y_ = 0;

  if (is_adjustment(mode)) {
    if (!copy_layer_snapshot()) {
      return {};
    }
  } else if (!copy_document_composite()) {
    return {};
  }

  active_ = true;
  last_x_ = x;
  last_y_ = y;

  if (
      !is_adjustment(mode) &&
      source_.has_value() &&
      !aligned_offset_set_) {
    offset_x_ =
        source_->first -
        static_cast<int>(
            std::lround(x));

    offset_y_ =
        source_->second -
        static_cast<int>(
            std::lround(y));

    aligned_offset_set_ = true;
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
  selection_coverage_ = nullptr;
}

patchy::Rect RetouchController::paint_segment(
    double x0,
    double y0,
    double x1,
    double y1) {
  if (
      document_ == nullptr ||
      !document_->active_layer_id()
           .has_value() ||
      snapshot_.empty()) {
    return {};
  }

  patchy::EditOptions options;
  options.brush_size = settings_.size;
  options.brush_softness = settings_.softness;
  options.brush_roundness = settings_.roundness;
  options.brush_angle_degrees =
      settings_.angle_degrees;

  const auto amount =
      static_cast<float>(
          std::clamp(
              settings_.opacity,
              1,
              100)) /
      100.0F;

  const auto layer_id =
      *document_->active_layer_id();

  const auto ix0 =
      static_cast<std::int32_t>(
          std::lround(x0));

  const auto iy0 =
      static_cast<std::int32_t>(
          std::lround(y0));

  const auto ix1 =
      static_cast<std::int32_t>(
          std::lround(x1));

  const auto iy1 =
      static_cast<std::int32_t>(
          std::lround(y1));

  const auto selection =
      [this](
          std::int32_t x,
          std::int32_t y) {
        if (!selection_coverage_) {
          return 1.0F;
        }

        return selection_coverage_(
            x,
            y);
      };

  if (is_adjustment(mode_)) {
    return patchy::local_adjustment_brush_segment(
        *document_,
        layer_id,
        ix0,
        iy0,
        ix1,
        iy1,
        options,
        plane(),
        adjustment_for(mode_),
        adjustment_,
        amount,
        &stroke_caps_,
        selection);
  }

  patchy::CloneStampSettings stamp;
  stamp.healing =
      mode_ == RetouchMode::Healing;
  stamp.tone_radius =
      patchy::healing_tone_radius(
          settings_.size,
          healing_diffusion_);
  stamp.offset_x = offset_x_;
  stamp.offset_y = offset_y_;

  return patchy::clone_stamp_brush_segment(
      *document_,
      layer_id,
      ix0,
      iy0,
      ix1,
      iy1,
      options,
      plane(),
      stamp,
      amount,
      &stroke_caps_,
      selection);
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
