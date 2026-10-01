#pragma once

#include "core/document.hpp"
#include "core/pixel_tools.hpp"

#include <cairo.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lienzo::gnome {

enum class RetouchMode {
  Clone,
  Healing,
  Blur,
  Sharpen,
  Dodge,
  Burn,
  Sponge
};

struct RetouchBrushSettings {
  int size{24};
  int softness{20};
  int opacity{100};
};

class RetouchController {
public:
  explicit RetouchController(
      patchy::Document& document);

  void set_source(
      int x,
      int y);

  [[nodiscard]] bool source_set() const noexcept;

  patchy::Rect begin_stroke(
      RetouchMode mode,
      double x,
      double y,
      const std::vector<std::uint8_t>& rgba,
      int width,
      int height,
      int stride,
      RetouchBrushSettings settings,
      std::function<float(int, int)> selection_coverage);

  patchy::Rect stroke_to(
      double x,
      double y);

  void end_stroke();

  void draw_source_marker(
      cairo_t* cr,
      double origin_x,
      double origin_y,
      double zoom) const;

private:
  patchy::Rect paint_segment(
      double x0,
      double y0,
      double x1,
      double y1);

  [[nodiscard]] const std::uint8_t* sample(
      int x,
      int y) const;

  [[nodiscard]] std::array<double, 3> ring_tone(
      int x,
      int y,
      int radius) const;

  [[nodiscard]] std::array<std::uint8_t, 4> healing_sample(
      int source_x,
      int source_y,
      int destination_x,
      int destination_y,
      int radius) const;

  patchy::Document* document_{};

  std::optional<std::pair<int, int>>
      source_;

  std::vector<std::uint8_t>
      snapshot_;

  int width_{0};
  int height_{0};
  int stride_{0};

  RetouchMode mode_{
      RetouchMode::Clone};

  RetouchBrushSettings settings_{};

  std::function<float(int, int)>
      selection_coverage_;

  bool active_{false};

  double last_x_{0.0};
  double last_y_{0.0};

  int offset_x_{0};
  int offset_y_{0};

  std::unordered_map<std::uint64_t, float>
      stroke_caps_;
};

}  // namespace lienzo::gnome
