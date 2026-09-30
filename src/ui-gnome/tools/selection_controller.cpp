#include "ui-gnome/tools/selection_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace lienzo::gnome {

namespace {

std::int32_t clamp_coord(
    std::int32_t value,
    std::int32_t maximum) {
  return std::clamp(
      value,
      std::int32_t{0},
      std::max(
          std::int32_t{0},
          maximum - 1));
}

bool color_matches(
    const std::uint8_t* a,
    const std::uint8_t* b,
    int tolerance) {
  tolerance =
      std::clamp(
          tolerance,
          0,
          255);

  std::int64_t distance = 0;

  for (int channel = 0;
       channel < 4;
       ++channel) {
    const int delta =
        static_cast<int>(a[channel]) -
        static_cast<int>(b[channel]);

    distance +=
        static_cast<std::int64_t>(
            delta * delta);
  }

  const std::int64_t limit =
      4LL *
      tolerance *
      tolerance;

  return distance <= limit;
}

}  // namespace

void SelectionController::resize(
    std::int32_t width,
    std::int32_t height) {
  width_ =
      std::max<std::int32_t>(
          0,
          width);

  height_ =
      std::max<std::int32_t>(
          0,
          height);

  mask_.assign(
      static_cast<std::size_t>(width_) *
          static_cast<std::size_t>(height_),
      0);

  boundary_.clear();
  bounds_.reset();
  cancel_draft();
}

std::int32_t SelectionController::width() const noexcept {
  return width_;
}

std::int32_t SelectionController::height() const noexcept {
  return height_;
}

std::size_t SelectionController::index(
    std::int32_t x,
    std::int32_t y) const noexcept {
  return
      static_cast<std::size_t>(y) *
          static_cast<std::size_t>(width_) +
      static_cast<std::size_t>(x);
}

bool SelectionController::empty() const noexcept {
  return !bounds_.has_value();
}

bool SelectionController::selected(
    std::int32_t x,
    std::int32_t y) const noexcept {
  if (
      x < 0 ||
      y < 0 ||
      x >= width_ ||
      y >= height_) {
    return false;
  }

  return
      mask_[index(x, y)] != 0;
}

float SelectionController::coverage(
    std::int32_t x,
    std::int32_t y) const noexcept {
  if (
      x < 0 ||
      y < 0 ||
      x >= width_ ||
      y >= height_) {
    return 0.0F;
  }

  return
      mask_[index(x, y)] /
      255.0F;
}

const std::vector<std::uint8_t>&
SelectionController::mask() const noexcept {
  return mask_;
}

const std::vector<SelectionPoint>&
SelectionController::boundary() const noexcept {
  return boundary_;
}

const std::vector<patchy::MaskLoop>&
SelectionController::outlines() const noexcept {
  return outlines_;
}

std::optional<patchy::Rect>
SelectionController::bounds() const noexcept {
  return bounds_;
}

void SelectionController::rebuild_cache() {
  boundary_.clear();
  outlines_.clear();
  bounds_.reset();

  if (
      width_ <= 0 ||
      height_ <= 0) {
    return;
  }

  std::int32_t min_x =
      std::numeric_limits<std::int32_t>::max();

  std::int32_t min_y =
      std::numeric_limits<std::int32_t>::max();

  std::int32_t max_x = -1;
  std::int32_t max_y = -1;

  for (std::int32_t y = 0;
       y < height_;
       ++y) {
    for (std::int32_t x = 0;
         x < width_;
         ++x) {
      if (
          mask_[index(x, y)] <
          128) {
        continue;
      }

      min_x =
          std::min(min_x, x);

      min_y =
          std::min(min_y, y);

      max_x =
          std::max(max_x, x);

      max_y =
          std::max(max_y, y);
    }
  }

  if (
      max_x < min_x ||
      max_y < min_y) {
    return;
  }

  bounds_ =
      patchy::Rect{
          min_x,
          min_y,
          max_x - min_x + 1,
          max_y - min_y + 1};

  const std::size_t stride =
      static_cast<std::size_t>(
          width_ + 2);

  std::vector<std::uint8_t>
      padded(
          stride *
              static_cast<std::size_t>(
                  height_ + 2),
          0);

  for (std::int32_t y = 0;
       y < height_;
       ++y) {
    auto* destination =
        padded.data() +
        static_cast<std::size_t>(y + 1) *
            stride +
        1;

    for (std::int32_t x = 0;
         x < width_;
         ++x) {
      destination[x] =
          mask_[index(x, y)] >= 128
              ? 1
              : 0;
    }
  }

  outlines_ =
      patchy::trace_mask_outlines(
          padded.data(),
          width_,
          height_,
          stride);
}

void SelectionController::clear() {
  std::fill(
      mask_.begin(),
      mask_.end(),
      0);

  rebuild_cache();
}

void SelectionController::select_all() {
  std::fill(
      mask_.begin(),
      mask_.end(),
      255);

  rebuild_cache();
}

void SelectionController::invert() {
  for (auto& value : mask_) {
    value =
        static_cast<std::uint8_t>(
            255 - value);
  }

  rebuild_cache();
}

void SelectionController::apply_generated(
    const std::vector<std::uint8_t>& generated,
    SelectionCombine combine) {
  if (generated.size() != mask_.size()) {
    return;
  }

  for (std::size_t i = 0;
       i < mask_.size();
       ++i) {
    switch (combine) {
      case SelectionCombine::Replace:
        mask_[i] =
            generated[i];
        break;

      case SelectionCombine::Add:
        mask_[i] =
            std::max(
                mask_[i],
                generated[i]);
        break;

      case SelectionCombine::Subtract:
        if (generated[i] != 0) {
          mask_[i] = 0;
        }
        break;

      case SelectionCombine::Intersect:
        if (generated[i] == 0) {
          mask_[i] = 0;
        }
        break;
    }
  }

  rebuild_cache();
}

void SelectionController::begin_rectangle(
    std::int32_t x,
    std::int32_t y,
    bool ellipse,
    SelectionCombine combine) {
  draft_kind_ =
      ellipse
          ? SelectionDraftKind::Ellipse
          : SelectionDraftKind::Rectangle;

  draft_combine_ = combine;

  draft_start_ = {
      clamp_coord(x, width_),
      clamp_coord(y, height_)};

  draft_end_ =
      draft_start_;

  draft_points_.clear();
}

void SelectionController::update_rectangle(
    std::int32_t x,
    std::int32_t y) {
  if (
      draft_kind_ !=
          SelectionDraftKind::Rectangle &&
      draft_kind_ !=
          SelectionDraftKind::Ellipse) {
    return;
  }

  draft_end_ = {
      clamp_coord(x, width_),
      clamp_coord(y, height_)};
}

void SelectionController::begin_lasso(
    std::int32_t x,
    std::int32_t y,
    SelectionCombine combine) {
  draft_kind_ =
      SelectionDraftKind::Lasso;

  draft_combine_ =
      combine;

  draft_points_.clear();

  draft_points_.push_back(
      SelectionPoint{
          clamp_coord(x, width_),
          clamp_coord(y, height_)});
}

void SelectionController::append_lasso(
    std::int32_t x,
    std::int32_t y) {
  if (
      draft_kind_ !=
      SelectionDraftKind::Lasso) {
    return;
  }

  SelectionPoint point{
      clamp_coord(x, width_),
      clamp_coord(y, height_)};

  if (
      !draft_points_.empty()) {
    const auto& previous =
        draft_points_.back();

    const auto dx =
        point.x - previous.x;

    const auto dy =
        point.y - previous.y;

    if (
        dx * dx +
            dy * dy <
        4) {
      return;
    }
  }

  draft_points_.push_back(point);
}

patchy::Rect
SelectionController::draft_rect() const noexcept {
  const auto left =
      std::min(
          draft_start_.x,
          draft_end_.x);

  const auto top =
      std::min(
          draft_start_.y,
          draft_end_.y);

  const auto right =
      std::max(
          draft_start_.x,
          draft_end_.x);

  const auto bottom =
      std::max(
          draft_start_.y,
          draft_end_.y);

  return {
      left,
      top,
      right - left + 1,
      bottom - top + 1};
}

SelectionDraftKind
SelectionController::draft_kind() const noexcept {
  return draft_kind_;
}

const std::vector<SelectionPoint>&
SelectionController::draft_points() const noexcept {
  return draft_points_;
}

void SelectionController::commit_draft() {
  std::vector<std::uint8_t> generated(
      mask_.size(),
      0);

  if (
      draft_kind_ ==
          SelectionDraftKind::Rectangle ||
      draft_kind_ ==
          SelectionDraftKind::Ellipse) {
    const auto rect =
        draft_rect();

    const double cx =
        rect.x +
        rect.width * 0.5;

    const double cy =
        rect.y +
        rect.height * 0.5;

    const double rx =
        std::max(
            0.5,
            rect.width * 0.5);

    const double ry =
        std::max(
            0.5,
            rect.height * 0.5);

    for (std::int32_t y = rect.y;
         y < rect.y + rect.height;
         ++y) {
      for (std::int32_t x = rect.x;
           x < rect.x + rect.width;
           ++x) {
        if (
            x < 0 ||
            y < 0 ||
            x >= width_ ||
            y >= height_) {
          continue;
        }

        if (
            draft_kind_ ==
            SelectionDraftKind::Ellipse) {
          constexpr int samples = 4;
          int inside_samples = 0;

          for (int sy = 0;
               sy < samples;
               ++sy) {
            for (int sx = 0;
                 sx < samples;
                 ++sx) {
              const double sample_x =
                  x +
                  (sx + 0.5) /
                      samples;

              const double sample_y =
                  y +
                  (sy + 0.5) /
                      samples;

              const double nx =
                  (sample_x - cx) /
                  rx;

              const double ny =
                  (sample_y - cy) /
                  ry;

              if (
                  nx * nx +
                      ny * ny <=
                  1.0) {
                ++inside_samples;
              }
            }
          }

          generated[index(x, y)] =
              static_cast<std::uint8_t>(
                  std::lround(
                      255.0 *
                      inside_samples /
                      (samples * samples)));

        } else {
          generated[index(x, y)] =
              255;
        }
      }
    }
  } else if (
      draft_kind_ ==
          SelectionDraftKind::Lasso &&
      draft_points_.size() >= 3) {
    std::int32_t min_y = height_ - 1;
    std::int32_t max_y = 0;

    for (const auto& point :
         draft_points_) {
      min_y =
          std::min(
              min_y,
              point.y);

      max_y =
          std::max(
              max_y,
              point.y);
    }

    for (std::int32_t y = min_y;
         y <= max_y;
         ++y) {
      const double scan_y =
          y + 0.5;

      std::vector<double>
          intersections;

      for (std::size_t i = 0;
           i < draft_points_.size();
           ++i) {
        const auto a =
            draft_points_[i];

        const auto b =
            draft_points_[
                (i + 1) %
                draft_points_.size()];

        if (a.y == b.y) {
          continue;
        }

        const double y0 =
            static_cast<double>(a.y);

        const double y1 =
            static_cast<double>(b.y);

        if (
            scan_y <
                std::min(y0, y1) ||
            scan_y >=
                std::max(y0, y1)) {
          continue;
        }

        const double t =
            (scan_y - y0) /
            (y1 - y0);

        intersections.push_back(
            a.x +
            t *
                (b.x - a.x));
      }

      std::sort(
          intersections.begin(),
          intersections.end());

      for (std::size_t i = 0;
           i + 1 <
               intersections.size();
           i += 2) {
        const auto left =
            static_cast<std::int32_t>(
                std::ceil(
                    intersections[i]));

        const auto right =
            static_cast<std::int32_t>(
                std::floor(
                    intersections[i + 1]));

        for (std::int32_t x = left;
             x <= right;
             ++x) {
          if (
              x >= 0 &&
              x < width_ &&
              y >= 0 &&
              y < height_) {
            generated[index(x, y)] =
                255;
          }
        }
      }
    }
  }

  apply_generated(
      generated,
      draft_combine_);

  cancel_draft();
}

void SelectionController::cancel_draft() {
  draft_kind_ =
      SelectionDraftKind::None;

  draft_points_.clear();
}

void SelectionController::magic_wand_rgba(
    const std::uint8_t* rgba,
    std::ptrdiff_t stride,
    std::int32_t x,
    std::int32_t y,
    int tolerance,
    bool contiguous,
    SelectionCombine combine) {
  if (
      rgba == nullptr ||
      width_ <= 0 ||
      height_ <= 0 ||
      x < 0 ||
      y < 0 ||
      x >= width_ ||
      y >= height_) {
    return;
  }

  std::vector<std::uint8_t> generated(
      mask_.size(),
      0);

  const auto* reference =
      rgba +
      static_cast<std::ptrdiff_t>(y) *
          stride +
      static_cast<std::ptrdiff_t>(x) *
          4;

  if (!contiguous) {
    for (std::int32_t py = 0;
         py < height_;
         ++py) {
      const auto* row =
          rgba +
          static_cast<std::ptrdiff_t>(py) *
              stride;

      for (std::int32_t px = 0;
           px < width_;
           ++px) {
        if (
            color_matches(
                row +
                    static_cast<std::ptrdiff_t>(px) *
                        4,
                reference,
                tolerance)) {
          generated[index(px, py)] =
              255;
        }
      }
    }
  } else {
    std::vector<std::uint8_t> visited(
        mask_.size(),
        0);

    std::queue<SelectionPoint> queue;

    queue.push({x, y});

    while (!queue.empty()) {
      const auto point =
          queue.front();

      queue.pop();

      if (
          point.x < 0 ||
          point.y < 0 ||
          point.x >= width_ ||
          point.y >= height_) {
        continue;
      }

      const auto position =
          index(
              point.x,
              point.y);

      if (visited[position] != 0) {
        continue;
      }

      visited[position] = 1;

      const auto* sample =
          rgba +
          static_cast<std::ptrdiff_t>(
              point.y) *
              stride +
          static_cast<std::ptrdiff_t>(
              point.x) *
              4;

      if (
          !color_matches(
              sample,
              reference,
              tolerance)) {
        continue;
      }

      generated[position] = 255;

      queue.push(
          {point.x - 1, point.y});

      queue.push(
          {point.x + 1, point.y});

      queue.push(
          {point.x, point.y - 1});

      queue.push(
          {point.x, point.y + 1});
    }
  }

  apply_generated(
      generated,
      combine);
}

void SelectionController::quick_select_rgba(
    const std::uint8_t* rgba,
    std::ptrdiff_t stride,
    std::int32_t x,
    std::int32_t y,
    int radius,
    SelectionCombine combine) {
  if (
      rgba == nullptr ||
      x < 0 ||
      y < 0 ||
      x >= width_ ||
      y >= height_) {
    return;
  }

  radius =
      std::clamp(
          radius,
          2,
          256);

  const auto* reference =
      rgba +
      static_cast<std::ptrdiff_t>(y) *
          stride +
      static_cast<std::ptrdiff_t>(x) *
          4;

  std::vector<std::uint8_t> generated(
      mask_.size(),
      0);

  const int radius_squared =
      radius * radius;

  for (int py =
           std::max(0, y - radius);
       py <=
           std::min(
               height_ - 1,
               y + radius);
       ++py) {
    const auto* row =
        rgba +
        static_cast<std::ptrdiff_t>(py) *
            stride;

    for (int px =
             std::max(0, x - radius);
         px <=
             std::min(
                 width_ - 1,
                 x + radius);
         ++px) {
      const int dx = px - x;
      const int dy = py - y;

      if (
          dx * dx +
              dy * dy >
          radius_squared) {
        continue;
      }

      const auto* sample =
          row +
          static_cast<std::ptrdiff_t>(px) *
              4;

      if (
          color_matches(
              sample,
              reference,
              42)) {
        generated[index(px, py)] =
            255;
      }
    }
  }

  apply_generated(
      generated,
      combine);
}

void SelectionController::paint_mask_segment(
    double x0,
    double y0,
    double x1,
    double y1,
    double radius,
    bool add) {
  radius =
      std::max(
          0.5,
          radius);

  const double distance =
      std::hypot(
          x1 - x0,
          y1 - y0);

  const int steps =
      std::max(
          1,
          static_cast<int>(
              std::ceil(
                  distance /
                  std::max(
                      1.0,
                      radius * 0.35))));

  const int iradius =
      static_cast<int>(
          std::ceil(radius));

  for (int step = 0;
       step <= steps;
       ++step) {
    const double t =
        static_cast<double>(step) /
        steps;

    const double cx =
        x0 +
        (x1 - x0) * t;

    const double cy =
        y0 +
        (y1 - y0) * t;

    for (int py =
             static_cast<int>(cy) -
             iradius;
         py <=
             static_cast<int>(cy) +
             iradius;
         ++py) {
      for (int px =
               static_cast<int>(cx) -
               iradius;
           px <=
               static_cast<int>(cx) +
               iradius;
           ++px) {
        if (
            px < 0 ||
            py < 0 ||
            px >= width_ ||
            py >= height_) {
          continue;
        }

        const double dx =
            px + 0.5 - cx;

        const double dy =
            py + 0.5 - cy;

        if (
            dx * dx +
                dy * dy >
            radius * radius) {
          continue;
        }

        mask_[index(px, py)] =
            add
                ? 255
                : 0;
      }
    }
  }

  rebuild_cache();
}

void SelectionController::set_quick_mask(
    bool enabled) noexcept {
  quick_mask_ = enabled;
}

void SelectionController::toggle_quick_mask() noexcept {
  quick_mask_ =
      !quick_mask_;
}

bool SelectionController::quick_mask() const noexcept {
  return quick_mask_;
}

}  // namespace lienzo::gnome
