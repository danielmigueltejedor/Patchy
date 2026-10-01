#include "ui-gnome/tools/path_controller.hpp"

#include "core/document_path.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lienzo::gnome {

namespace {

double distance_to_segment(
    double px,
    double py,
    double ax,
    double ay,
    double bx,
    double by) {
  const double dx =
      bx - ax;

  const double dy =
      by - ay;

  const double length_squared =
      dx * dx +
      dy * dy;

  if (length_squared <= 0.000001) {
    return
        std::hypot(
            px - ax,
            py - ay);
  }

  const double t =
      std::clamp(
          (
              (px - ax) * dx +
              (py - ay) * dy) /
              length_squared,
          0.0,
          1.0);

  return
      std::hypot(
          px -
              (ax + dx * t),
          py -
              (ay + dy * t));
}

std::pair<double, double>
cubic_point(
    const patchy::PathAnchor& a,
    const patchy::PathAnchor& b,
    double t) {
  const double u =
      1.0 - t;

  return {
      u * u * u *
          a.anchor_x +
      3.0 * u * u * t *
          a.out_x +
      3.0 * u * t * t *
          b.in_x +
      t * t * t *
          b.anchor_x,

      u * u * u *
          a.anchor_y +
      3.0 * u * u * t *
          a.out_y +
      3.0 * u * t * t *
          b.in_y +
      t * t * t *
          b.anchor_y};
}

void append_subpath(
    cairo_t* cr,
    const patchy::PathSubpath& subpath,
    double origin_x,
    double origin_y,
    double zoom) {
  if (subpath.anchors.empty()) {
    return;
  }

  const auto tx =
      [origin_x, zoom](
          double value) {
        return
            origin_x +
            value * zoom;
      };

  const auto ty =
      [origin_y, zoom](
          double value) {
        return
            origin_y +
            value * zoom;
      };

  const auto& first =
      subpath.anchors.front();

  cairo_move_to(
      cr,
      tx(first.anchor_x),
      ty(first.anchor_y));

  for (std::size_t i = 1;
       i < subpath.anchors.size();
       ++i) {
    const auto& previous =
        subpath.anchors[i - 1];

    const auto& current =
        subpath.anchors[i];

    cairo_curve_to(
        cr,
        tx(previous.out_x),
        ty(previous.out_y),
        tx(current.in_x),
        ty(current.in_y),
        tx(current.anchor_x),
        ty(current.anchor_y));
  }

  if (
      subpath.closed &&
      subpath.anchors.size() > 1) {
    const auto& last =
        subpath.anchors.back();

    cairo_curve_to(
        cr,
        tx(last.out_x),
        ty(last.out_y),
        tx(first.in_x),
        ty(first.in_y),
        tx(first.anchor_x),
        ty(first.anchor_y));

    cairo_close_path(cr);
  }
}

}  // namespace

PathController::PathController(
    patchy::Document& document,
    std::function<void()> checkpoint,
    std::function<void()> changed)
    : document_(&document),
      checkpoint_(
          std::move(
              checkpoint)),
      changed_(
          std::move(
              changed)) {}

void PathController::pen_begin(
    double x,
    double y,
    double zoom) {
  if (
      pen_active_ &&
      pen_anchors_.size() >= 3) {
    const auto& first =
        pen_anchors_.front();

    if (
        std::hypot(
            (
                x -
                first.anchor_x) *
                zoom,
            (
                y -
                first.anchor_y) *
                zoom) <= 8.0) {
      (void)commit_pen(
          true);

      return;
    }
  }

  patchy::PathAnchor anchor;

  anchor.anchor_x = x;
  anchor.anchor_y = y;

  anchor.in_x = x;
  anchor.in_y = y;

  anchor.out_x = x;
  anchor.out_y = y;

  anchor.smooth = false;

  pen_anchors_.push_back(
      anchor);

  pen_active_ = true;
  pen_dragging_ = true;

  hover_x_ = x;
  hover_y_ = y;
}

void PathController::pen_drag(
    double x,
    double y,
    double zoom) {
  if (
      !pen_active_ ||
      !pen_dragging_ ||
      pen_anchors_.empty()) {
    return;
  }

  auto& anchor =
      pen_anchors_.back();

  const double distance =
      std::hypot(
          x -
              anchor.anchor_x,
          y -
              anchor.anchor_y);

  if (
      distance *
          zoom <
      2.0) {
    return;
  }

  anchor.out_x = x;
  anchor.out_y = y;

  anchor.in_x =
      2.0 *
          anchor.anchor_x -
      x;

  anchor.in_y =
      2.0 *
          anchor.anchor_y -
      y;

  anchor.smooth = true;

  hover_x_ = x;
  hover_y_ = y;
}

void PathController::pen_end() {
  pen_dragging_ = false;
}

void PathController::set_hover(
    double x,
    double y) {
  hover_x_ = x;
  hover_y_ = y;
}

bool PathController::commit_open_pen() {
  return commit_pen(
      false);
}

void PathController::cancel_pen() {
  pen_anchors_.clear();

  pen_active_ = false;
  pen_dragging_ = false;
}

void PathController::delete_last_pen_anchor() {
  if (pen_anchors_.empty()) {
    return;
  }

  pen_anchors_.pop_back();

  if (pen_anchors_.empty()) {
    cancel_pen();
  }
}

bool PathController::commit_pen(
    bool closed) {
  const std::size_t minimum =
      closed
          ? 3U
          : 2U;

  if (pen_anchors_.size() < minimum) {
    cancel_pen();

    return false;
  }

  if (checkpoint_) {
    checkpoint_();
  }

  patchy::PathSubpath subpath;

  subpath.anchors =
      pen_anchors_;

  subpath.closed =
      closed;

  subpath.op =
      patchy::PathCombineOp::Add;

  patchy::VectorPath path;

  if (
      auto* work =
          document_->work_path();
      work != nullptr) {
    path =
        work->path();
  }

  subpath.shape_group =
      path.next_shape_group();

  path.subpaths.push_back(
      std::move(
          subpath));

  if (
      auto* work =
          document_->work_path();
      work != nullptr) {
    work->set_path(
        std::move(
            path));

  } else {
    document_->add_path(
        patchy::DocumentPath(
            document_->allocate_path_id(),
            "",
            patchy::DocumentPathKind::Work,
            std::move(
                path)));
  }

  cancel_pen();

  if (changed_) {
    changed_();
  }

  return true;
}

std::optional<std::size_t>
PathController::hit_subpath(
    double x,
    double y,
    double zoom) const {
  const auto* work =
      document_->work_path();

  if (work == nullptr) {
    return std::nullopt;
  }

  const auto& path =
      work->path();

  const double threshold =
      8.0 /
      std::max(
          0.02,
          zoom);

  double best =
      std::numeric_limits<double>::max();

  std::optional<std::size_t>
      best_index;

  for (std::size_t subpath_index = 0;
       subpath_index <
           path.subpaths.size();
       ++subpath_index) {
    const auto& subpath =
        path.subpaths[
            subpath_index];

    if (subpath.anchors.empty()) {
      continue;
    }

    for (const auto& anchor :
         subpath.anchors) {
      const double distance =
          std::hypot(
              x -
                  anchor.anchor_x,
              y -
                  anchor.anchor_y);

      if (distance < best) {
        best = distance;
        best_index =
            subpath_index;
      }
    }

    const std::size_t segment_count =
        subpath.closed
            ? subpath.anchors.size()
            : subpath.anchors.size() - 1;

    for (std::size_t segment = 0;
         segment < segment_count;
         ++segment) {
      const auto& a =
          subpath.anchors[
              segment];

      const auto& b =
          subpath.anchors[
              (segment + 1) %
              subpath.anchors.size()];

      auto previous =
          std::pair<double, double>{
              a.anchor_x,
              a.anchor_y};

      constexpr int samples = 16;

      for (int step = 1;
           step <= samples;
           ++step) {
        const auto current =
            cubic_point(
                a,
                b,
                static_cast<double>(
                    step) /
                    samples);

        const double distance =
            distance_to_segment(
                x,
                y,
                previous.first,
                previous.second,
                current.first,
                current.second);

        if (distance < best) {
          best = distance;
          best_index =
              subpath_index;
        }

        previous =
            current;
      }
    }
  }

  if (best <= threshold) {
    return best_index;
  }

  return std::nullopt;
}

bool PathController::begin_path_select(
    double x,
    double y,
    double zoom) {
  selected_subpath_ =
      hit_subpath(
          x,
          y,
          zoom);

  selection_dragging_ =
      selected_subpath_.has_value();

  selection_checkpointed_ =
      false;

  selection_last_x_ = x;
  selection_last_y_ = y;

  return
      selected_subpath_.has_value();
}

void PathController::drag_path_select(
    double x,
    double y) {
  if (
      !selection_dragging_ ||
      !selected_subpath_
           .has_value()) {
    return;
  }

  auto* work =
      document_->work_path();

  if (work == nullptr) {
    return;
  }

  const double dx =
      x -
      selection_last_x_;

  const double dy =
      y -
      selection_last_y_;

  if (
      std::abs(dx) < 0.000001 &&
      std::abs(dy) < 0.000001) {
    return;
  }

  auto path =
      work->path();

  if (
      *selected_subpath_ >=
      path.subpaths.size()) {
    return;
  }

  if (!selection_checkpointed_) {
    if (checkpoint_) {
      checkpoint_();
    }

    selection_checkpointed_ =
        true;
  }

  auto& subpath =
      path.subpaths[
          *selected_subpath_];

  for (auto& anchor :
       subpath.anchors) {
    anchor.anchor_x += dx;
    anchor.anchor_y += dy;

    anchor.in_x += dx;
    anchor.in_y += dy;

    anchor.out_x += dx;
    anchor.out_y += dy;
  }

  work->set_path(
      std::move(
          path));

  selection_last_x_ = x;
  selection_last_y_ = y;

  if (changed_) {
    changed_();
  }
}

void PathController::end_path_select() {
  selection_dragging_ = false;
  selection_checkpointed_ = false;
}

void PathController::draw_pen(
    cairo_t* cr,
    double origin_x,
    double origin_y,
    double zoom) const {
  if (
      !pen_active_ ||
      pen_anchors_.empty()) {
    return;
  }

  cairo_save(cr);

  patchy::PathSubpath temporary;

  temporary.anchors =
      pen_anchors_;

  temporary.closed =
      false;

  cairo_new_path(cr);

  append_subpath(
      cr,
      temporary,
      origin_x,
      origin_y,
      zoom);

  cairo_set_source_rgba(
      cr,
      0.30,
      0.65,
      1.0,
      1.0);

  cairo_set_line_width(
      cr,
      1.4);

  cairo_stroke(cr);

  const auto& last =
      pen_anchors_.back();

  cairo_new_path(cr);

  cairo_move_to(
      cr,
      origin_x +
          last.anchor_x *
              zoom,
      origin_y +
          last.anchor_y *
              zoom);

  cairo_curve_to(
      cr,
      origin_x +
          last.out_x *
              zoom,
      origin_y +
          last.out_y *
              zoom,
      origin_x +
          hover_x_ *
              zoom,
      origin_y +
          hover_y_ *
              zoom,
      origin_x +
          hover_x_ *
              zoom,
      origin_y +
          hover_y_ *
              zoom);

  cairo_set_dash(
      cr,
      nullptr,
      0,
      0.0);

  cairo_set_source_rgba(
      cr,
      0.30,
      0.65,
      1.0,
      0.65);

  cairo_set_line_width(
      cr,
      1.0);

  cairo_stroke(cr);

  for (const auto& anchor :
       pen_anchors_) {
    const double x =
        origin_x +
        anchor.anchor_x *
            zoom;

    const double y =
        origin_y +
        anchor.anchor_y *
            zoom;

    cairo_rectangle(
        cr,
        x - 3.0,
        y - 3.0,
        6.0,
        6.0);

    cairo_set_source_rgba(
        cr,
        0.30,
        0.65,
        1.0,
        1.0);

    cairo_fill(cr);
  }

  cairo_restore(cr);
}

void PathController::draw_path_selection(
    cairo_t* cr,
    double origin_x,
    double origin_y,
    double zoom) const {
  const auto* work =
      document_->work_path();

  if (work == nullptr) {
    return;
  }

  const auto& path =
      work->path();

  cairo_save(cr);

  for (std::size_t index = 0;
       index <
           path.subpaths.size();
       ++index) {
    const bool selected =
        selected_subpath_.has_value() &&
        *selected_subpath_ ==
            index;

    cairo_new_path(cr);

    append_subpath(
        cr,
        path.subpaths[index],
        origin_x,
        origin_y,
        zoom);

    cairo_set_source_rgba(
        cr,
        0.30,
        0.65,
        1.0,
        selected
            ? 1.0
            : 0.55);

    cairo_set_line_width(
        cr,
        selected
            ? 1.8
            : 1.0);

    cairo_stroke(cr);

    if (selected) {
      for (const auto& anchor :
           path.subpaths[index]
               .anchors) {
        const double x =
            origin_x +
            anchor.anchor_x *
                zoom;

        const double y =
            origin_y +
            anchor.anchor_y *
                zoom;

        cairo_rectangle(
            cr,
            x - 3.0,
            y - 3.0,
            6.0,
            6.0);

        cairo_set_source_rgba(
            cr,
            0.30,
            0.65,
            1.0,
            1.0);

        cairo_fill(cr);
      }
    }
  }

  cairo_restore(cr);
}

}  // namespace lienzo::gnome
