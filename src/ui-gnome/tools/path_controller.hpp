#pragma once

#include "core/document.hpp"
#include "core/vector_shape.hpp"

#include <cairo.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace lienzo::gnome {

class PathController {
public:
  PathController(
      patchy::Document& document,
      std::function<void()> checkpoint,
      std::function<void()> changed);

  void pen_begin(
      double x,
      double y,
      double zoom);

  void pen_drag(
      double x,
      double y,
      double zoom);

  void pen_end();

  bool commit_open_pen();
  void cancel_pen();
  void delete_last_pen_anchor();

  void set_hover(
      double x,
      double y);

  bool begin_path_select(
      double x,
      double y,
      double zoom);

  void drag_path_select(
      double x,
      double y);

  void end_path_select();

  void draw_pen(
      cairo_t* cr,
      double origin_x,
      double origin_y,
      double zoom) const;

  void draw_path_selection(
      cairo_t* cr,
      double origin_x,
      double origin_y,
      double zoom) const;

private:
  bool commit_pen(
      bool closed);

  [[nodiscard]] std::optional<std::size_t>
  hit_subpath(
      double x,
      double y,
      double zoom) const;

  patchy::Document* document_{};

  std::function<void()>
      checkpoint_;

  std::function<void()>
      changed_;

  std::vector<patchy::PathAnchor>
      pen_anchors_;

  bool pen_active_{false};
  bool pen_dragging_{false};

  double hover_x_{0.0};
  double hover_y_{0.0};

  std::optional<std::size_t>
      selected_subpath_;

  bool selection_dragging_{false};
  bool selection_checkpointed_{false};

  double selection_last_x_{0.0};
  double selection_last_y_{0.0};
};

}  // namespace lienzo::gnome
