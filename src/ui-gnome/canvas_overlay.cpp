#include "ui-gnome/canvas.hpp"
#include "ui-gnome/canvas_internal.hpp"
#include "ui-gnome/brush_tips.hpp"
#include "ui-gnome/tools/selection_controller.hpp"
#include "ui-gnome/tools/text_controller.hpp"
#include "ui-gnome/tools/path_controller.hpp"
#include "ui-gnome/tools/retouch_controller.hpp"

#include "core/layer_metadata.hpp"
#include "core/magnetic_lasso.hpp"
#include "core/pixel_tools.hpp"
#include "core/rect_utils.hpp"
#include "core/stroke_stabilizer.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lienzo::gnome {

// Overlays drawn on top of the cached composite.


bool shape_drag_tool(
    Tool tool) {
  return
      tool == Tool::Line ||
      tool == Tool::Rectangle ||
      tool == Tool::Ellipse ||
      tool == Tool::Circle ||
      tool == Tool::Polygon;
}

std::vector<CanvasPoint> polygon_vertices(
    CanvasPoint center,
    CanvasPoint edge,
    int sides) {
  sides =
      std::clamp(
          sides,
          3,
          32);

  const double dx =
      edge.x - center.x;

  const double dy =
      edge.y - center.y;

  const double radius =
      std::hypot(
          dx,
          dy);

  if (radius < 0.5) {
    return {};
  }

  const double start_angle =
      std::atan2(
          dy,
          dx);

  constexpr double pi =
      3.14159265358979323846;

  std::vector<CanvasPoint> points;

  points.reserve(
      static_cast<std::size_t>(
          sides));

  for (int i = 0;
       i < sides;
       ++i) {
    const double angle =
        start_angle +
        2.0 * pi *
            static_cast<double>(i) /
            static_cast<double>(sides);

    points.push_back(
        CanvasPoint{
            center.x +
                std::cos(angle) *
                    radius,
            center.y +
                std::sin(angle) *
                    radius});
  }

  return points;
}

void append_ellipse_path(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height) {
  constexpr double kappa =
      0.5522847498307936;

  const double rx =
      width * 0.5;

  const double ry =
      height * 0.5;

  const double cx =
      x + rx;

  const double cy =
      y + ry;

  const double ox =
      rx * kappa;

  const double oy =
      ry * kappa;

  cairo_move_to(
      cr,
      cx + rx,
      cy);

  cairo_curve_to(
      cr,
      cx + rx,
      cy + oy,
      cx + ox,
      cy + ry,
      cx,
      cy + ry);

  cairo_curve_to(
      cr,
      cx - ox,
      cy + ry,
      cx - rx,
      cy + oy,
      cx - rx,
      cy);

  cairo_curve_to(
      cr,
      cx - rx,
      cy - oy,
      cx - ox,
      cy - ry,
      cx,
      cy - ry);

  cairo_curve_to(
      cr,
      cx + ox,
      cy - ry,
      cx + rx,
      cy - oy,
      cx + rx,
      cy);

  cairo_close_path(cr);
}

void draw_shape_preview(
    CanvasState* state,
    cairo_t* cr) {
  if (
      !state->shape_preview_active ||
      !shape_drag_tool(
          state->tool)) {
    return;
  }

  const auto view =
      geometry(state);

  const CanvasPoint start{
      state->shape_preview_start_x,
      state->shape_preview_start_y};

  const CanvasPoint end{
      state->shape_preview_end_x,
      state->shape_preview_end_y};

  const auto widget_x =
      [&view](double value) {
        return
            view.x +
            value *
                view.zoom;
      };

  const auto widget_y =
      [&view](double value) {
        return
            view.y +
            value *
                view.zoom;
      };

  cairo_save(cr);
  cairo_new_path(cr);

  if (state->tool == Tool::Line) {
    cairo_move_to(
        cr,
        widget_x(start.x),
        widget_y(start.y));

    cairo_line_to(
        cr,
        widget_x(end.x),
        widget_y(end.y));

  } else if (
      state->tool ==
      Tool::Rectangle) {
    const double left =
        widget_x(
            std::min(
                start.x,
                end.x));

    const double top =
        widget_y(
            std::min(
                start.y,
                end.y));

    const double width =
        std::abs(
            end.x -
            start.x) *
        view.zoom;

    const double height =
        std::abs(
            end.y -
            start.y) *
        view.zoom;

    cairo_rectangle(
        cr,
        left,
        top,
        width,
        height);

  } else if (
      (
          state->tool ==
              Tool::Ellipse ||
          state->tool ==
              Tool::Circle)) {
    const double left =
        widget_x(
            std::min(
                start.x,
                end.x));

    const double top =
        widget_y(
            std::min(
                start.y,
                end.y));

    const double width =
        std::abs(
            end.x -
            start.x) *
        view.zoom;

    const double height =
        std::abs(
            end.y -
            start.y) *
        view.zoom;

    append_ellipse_path(
        cr,
        left,
        top,
        width,
        height);

  } else if (
      state->tool ==
      Tool::Polygon) {
    const auto points =
        polygon_vertices(
            start,
            end,
            state->polygon_sides);

    if (!points.empty()) {
      cairo_move_to(
          cr,
          widget_x(points.front().x),
          widget_y(points.front().y));

      for (std::size_t i = 1;
           i < points.size();
           ++i) {
        cairo_line_to(
            cr,
            widget_x(points[i].x),
            widget_y(points[i].y));
      }

      cairo_close_path(cr);
    }
  }

  const auto color =
      state->edit_options.primary;

  const double alpha =
      color.a / 255.0;

  if (
      state->edit_options.fill_shapes &&
      state->tool != Tool::Line) {
    // Filled shapes preview exactly as a filled shape.
    // Do not preserve the path and add an artificial outline.
    cairo_set_source_rgba(
        cr,
        color.r / 255.0,
        color.g / 255.0,
        color.b / 255.0,
        alpha);

    cairo_fill(cr);

  } else {
    // Line and outline-only shapes preview with the
    // actual foreground colour, opacity and stroke width.
    cairo_set_source_rgba(
        cr,
        color.r / 255.0,
        color.g / 255.0,
        color.b / 255.0,
        alpha);

    cairo_set_line_width(
        cr,
        std::max(
            1.0,
            state->edit_options.brush_size *
                view.zoom));

    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

patchy::Rect draw_polygon_shape(
    CanvasState* state,
    patchy::LayerId layer,
    CanvasPoint center,
    CanvasPoint edge) {
  const auto points =
      polygon_vertices(
          center,
          edge,
          state->polygon_sides);

  if (points.size() < 3) {
    return {};
  }

  patchy::Rect dirty{};

  const auto add_dirty =
      [&dirty](patchy::Rect rect) {
        if (rect.empty()) {
          return;
        }

        dirty =
            dirty.empty()
                ? rect
                : patchy::unite_rect(
                      dirty,
                      rect);
      };

  if (state->edit_options.fill_shapes) {
    double minimum_y =
        points.front().y;

    double maximum_y =
        points.front().y;

    for (const auto& point : points) {
      minimum_y =
          std::min(
              minimum_y,
              point.y);

      maximum_y =
          std::max(
              maximum_y,
              point.y);
    }

    const int first_y =
        std::max(
            0,
            static_cast<int>(
                std::floor(
                    minimum_y)));

    const int last_y =
        std::min(
            state->document->height() - 1,
            static_cast<int>(
                std::ceil(
                    maximum_y)));

    for (int y = first_y;
         y <= last_y;
         ++y) {
      const double scan_y =
          y + 0.5;

      std::vector<double>
          intersections;

      for (std::size_t i = 0;
           i < points.size();
           ++i) {
        const auto& a =
            points[i];

        const auto& b =
            points[
                (i + 1) %
                points.size()];

        const bool crosses =
            (a.y <= scan_y &&
             b.y > scan_y) ||
            (b.y <= scan_y &&
             a.y > scan_y);

        if (!crosses) {
          continue;
        }

        const double t =
            (scan_y - a.y) /
            (b.y - a.y);

        intersections.push_back(
            a.x +
            (b.x - a.x) *
                t);
      }

      std::sort(
          intersections.begin(),
          intersections.end());

      for (std::size_t i = 0;
           i + 1 < intersections.size();
           i += 2) {
        const int left =
            static_cast<int>(
                std::ceil(
                    intersections[i]));

        const int right =
            static_cast<int>(
                std::floor(
                    intersections[i + 1]));

        if (right < left) {
          continue;
        }

        add_dirty(
            patchy::fill_rect(
                *state->document,
                layer,
                patchy::Rect{
                    left,
                    y,
                    right - left + 1,
                    1},
                state->edit_options));
      }
    }
  }

  for (std::size_t i = 0;
       i < points.size();
       ++i) {
    const auto& a =
        points[i];

    const auto& b =
        points[
            (i + 1) %
            points.size()];

    add_dirty(
        patchy::draw_line(
            *state->document,
            layer,
            static_cast<int>(
                std::lround(a.x)),
            static_cast<int>(
                std::lround(a.y)),
            static_cast<int>(
                std::lround(b.x)),
            static_cast<int>(
                std::lround(b.y)),
            state->edit_options,
            false));
  }

  return dirty;
}


void draw_selection_overlay(
    CanvasState* state,
    cairo_t* cr) {
  const auto view =
      geometry(state);

  if (state->selection.quick_selecting()) {
    const auto& stroke =
        state->selection.quick_select_stroke();

    cairo_save(cr);
    cairo_set_source_rgba(
        cr,
        0.35,
        0.67,
        1.0,
        0.28);

    const double radius =
        std::max(
            1.5,
            static_cast<double>(
                std::max(
                    1,
                    state->edit_options
                        .brush_size)) *
                view.zoom *
                0.5);

    for (const auto& point : stroke) {
      cairo_arc(
          cr,
          view.x +
              (static_cast<double>(point.x) +
               0.5) *
                  view.zoom,
          view.y +
              (static_cast<double>(point.y) +
               0.5) *
                  view.zoom,
          radius,
          0.0,
          2.0 * 3.141592653589793);

      cairo_fill(cr);
    }

    cairo_restore(cr);
  }

  if (state->selection.quick_mask()) {
    cairo_save(cr);

    cairo_set_source_rgba(
        cr,
        0.90,
        0.08,
        0.16,
        0.38);

    const auto& mask =
        state->selection.mask();

    const int width =
        state->selection.width();

    const int height =
        state->selection.height();

    for (int y = 0;
         y < height;
         ++y) {
      int run_start = -1;

      for (int x = 0;
           x <= width;
           ++x) {
        const bool covered =
            x < width &&
            mask[
                static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(
                        width) +
                static_cast<std::size_t>(x)] <
                128;

        if (
            covered &&
            run_start < 0) {
          run_start = x;
        }

        if (
            !covered &&
            run_start >= 0) {
          cairo_rectangle(
              cr,
              view.x +
                  run_start *
                      view.zoom,
              view.y +
                  y *
                      view.zoom,
              (x - run_start) *
                  view.zoom,
              std::max(
                  1.0,
                  view.zoom));

          run_start = -1;
        }
      }
    }

    cairo_fill(cr);
    cairo_restore(cr);
  }

  const double phase =
      std::fmod(
          g_get_monotonic_time() /
              80000.0,
          8.0);

  const auto stroke_ants =
      [cr, phase]() {
        cairo_set_line_width(
            cr,
            1.0);

        cairo_set_line_cap(
            cr,
            CAIRO_LINE_CAP_BUTT);

        cairo_set_line_join(
            cr,
            CAIRO_LINE_JOIN_MITER);

        const double dash[] = {
            4.0,
            4.0};

        cairo_set_source_rgba(
            cr,
            0.05,
            0.05,
            0.05,
            0.95);

        cairo_set_dash(
            cr,
            dash,
            2,
            phase);

        cairo_stroke_preserve(cr);

        cairo_set_source_rgba(
            cr,
            0.98,
            0.98,
            0.98,
            0.98);

        cairo_set_dash(
            cr,
            dash,
            2,
            phase + 4.0);

        cairo_stroke(cr);

        cairo_set_dash(
            cr,
            nullptr,
            0,
            0.0);
      };

  const auto kind =
      state->selection.draft_kind();

  if (
      kind ==
          SelectionDraftKind::Rectangle ||
      kind ==
          SelectionDraftKind::Ellipse) {
    const auto rect =
        state->selection.draft_rect();

    const double x =
        view.x +
        rect.x *
            view.zoom;

    const double y =
        view.y +
        rect.y *
            view.zoom;

    const double width =
        rect.width *
        view.zoom;

    const double height =
        rect.height *
        view.zoom;

    cairo_save(cr);
    cairo_new_path(cr);

    if (
        kind ==
        SelectionDraftKind::Ellipse) {
      cairo_save(cr);

      cairo_translate(
          cr,
          x + width / 2.0,
          y + height / 2.0);

      cairo_scale(
          cr,
          std::max(
              0.5,
              width / 2.0),
          std::max(
              0.5,
              height / 2.0));

      cairo_arc(
          cr,
          0.0,
          0.0,
          1.0,
          0.0,
          2.0 * G_PI);

      cairo_restore(cr);

    } else {
      cairo_rectangle(
          cr,
          x,
          y,
          width,
          height);
    }

    stroke_ants();

    cairo_restore(cr);
  }

  if (
      kind ==
      SelectionDraftKind::Lasso) {
    const auto& points =
        state->selection.draft_points();

    if (
        points.size() >= 2) {
      cairo_save(cr);
      cairo_new_path(cr);

      cairo_move_to(
          cr,
          view.x +
              points.front().x *
                  view.zoom,
          view.y +
              points.front().y *
                  view.zoom);

      for (std::size_t i = 1;
           i < points.size();
           ++i) {
        cairo_line_to(
            cr,
            view.x +
                points[i].x *
                    view.zoom,
            view.y +
                points[i].y *
                    view.zoom);
      }

      stroke_ants();

      cairo_restore(cr);
    }
  }

  const auto& outlines =
      state->selection.outlines();

  if (outlines.empty()) {
    return;
  }

  cairo_save(cr);
  cairo_new_path(cr);

  for (const auto& loop :
       outlines) {
    if (loop.points.size() < 3) {
      continue;
    }

    const auto& first =
        loop.points.front();

    cairo_move_to(
        cr,
        view.x +
            first.x *
                view.zoom,
        view.y +
            first.y *
                view.zoom);

    for (std::size_t i = 1;
         i < loop.points.size();
         ++i) {
      cairo_line_to(
          cr,
          view.x +
              loop.points[i].x *
                  view.zoom,
          view.y +
              loop.points[i].y *
                  view.zoom);
    }

    cairo_close_path(cr);
  }

  stroke_ants();

  cairo_restore(cr);
}

void draw_magnetic_lasso_overlay(
    CanvasState* state,
    cairo_t* cr) {
  if (!state->magnetic_lasso_active) {
    return;
  }

  const auto view =
      geometry(state);

  cairo_save(cr);

  cairo_set_line_width(
      cr,
      1.3);

  cairo_set_line_join(
      cr,
      CAIRO_LINE_JOIN_ROUND);

  cairo_set_line_cap(
      cr,
      CAIRO_LINE_CAP_ROUND);

  cairo_new_path(cr);

  bool started = false;

  const auto append =
      [&](const std::vector<SelectionPoint>& path) {
        for (const auto& point : path) {
          const double x =
              view.x +
              point.x *
                  view.zoom;

          const double y =
              view.y +
              point.y *
                  view.zoom;

          if (!started) {
            cairo_move_to(
                cr,
                x,
                y);

            started = true;
          } else {
            cairo_line_to(
                cr,
                x,
                y);
          }
        }
      };

  append(
      state->magnetic_committed_path);

  if (
      state->magnetic_live_path.size() >
      1) {
    for (std::size_t i = 1;
         i <
         state->magnetic_live_path.size();
         ++i) {
      const auto& point =
          state->magnetic_live_path[i];

      cairo_line_to(
          cr,
          view.x +
              point.x *
                  view.zoom,
          view.y +
              point.y *
                  view.zoom);
    }
  }

  cairo_set_source_rgba(
      cr,
      0.20,
      0.58,
      1.0,
      0.95);

  cairo_stroke(cr);

  for (
      std::size_t i = 0;
      i <
      state->magnetic_committed_path.size();
      i +=
          std::max<std::size_t>(
              1,
              state->magnetic_committed_path.size() /
                  32)) {
    const auto& point =
        state->magnetic_committed_path[i];

    cairo_rectangle(
        cr,
        view.x +
            point.x *
                view.zoom -
            2.0,
        view.y +
            point.y *
                view.zoom -
            2.0,
        4.0,
        4.0);
  }

  cairo_set_source_rgba(
      cr,
      0.95,
      0.97,
      1.0,
      0.95);

  cairo_fill(cr);

  cairo_restore(cr);
}

void draw_brush_cursor_overlay(
    CanvasState* state,
    cairo_t* cr) {
  if (!state->hover_valid) {
    return;
  }

  if (
      state->tool != Tool::Brush &&
      state->tool != Tool::Eraser &&
      state->tool != Tool::Smudge) {
    return;
  }

  double document_x = 0.0;
  double document_y = 0.0;

  if (!document_position(
          state,
          state->hover_x,
          state->hover_y,
          &document_x,
          &document_y)) {
    return;
  }

  const auto view =
      geometry(state);

  const double diameter =
      std::max(
          4.0,
          state->edit_options.brush_size *
              view.zoom);

  const double radius =
      diameter * 0.5;

  // Halo oscuro + línea clara: permanece visible sobre cualquier imagen.
  cairo_set_line_width(cr, 3.0);

  cairo_set_source_rgba(
      cr,
      0.05,
      0.06,
      0.08,
      0.9);

  cairo_arc(
      cr,
      state->hover_x,
      state->hover_y,
      radius,
      0.0,
      2.0 * G_PI);

  cairo_stroke(cr);

  cairo_set_line_width(cr, 1.2);

  cairo_set_source_rgba(
      cr,
      0.96,
      0.97,
      0.99,
      0.95);

  cairo_arc(
      cr,
      state->hover_x,
      state->hover_y,
      radius,
      0.0,
      2.0 * G_PI);

  cairo_stroke(cr);

  // El borrador se distingue inmediatamente.
  if (state->tool == Tool::Eraser) {
    cairo_set_line_width(cr, 1.4);

    cairo_move_to(
        cr,
        state->hover_x - 4.0,
        state->hover_y - 4.0);

    cairo_line_to(
        cr,
        state->hover_x + 4.0,
        state->hover_y + 4.0);

    cairo_move_to(
        cr,
        state->hover_x + 4.0,
        state->hover_y - 4.0);

    cairo_line_to(
        cr,
        state->hover_x - 4.0,
        state->hover_y + 4.0);

    cairo_stroke(cr);
  }
}

void draw_crop_overlay(
    CanvasState* state,
    cairo_t* cr) {
  if (
      state->tool != Tool::Crop ||
      !state->crop_session_active ||
      state->crop_rect.empty()) {
    return;
  }

  const auto view =
      geometry(state);

  const double x =
      view.x +
      state->crop_rect.x *
          view.zoom;

  const double y =
      view.y +
      state->crop_rect.y *
          view.zoom;

  const double width =
      state->crop_rect.width *
      view.zoom;

  const double height =
      state->crop_rect.height *
      view.zoom;

  const int widget_width =
      gtk_widget_get_width(
          GTK_WIDGET(state->area));

  const int widget_height =
      gtk_widget_get_height(
          GTK_WIDGET(state->area));

  // Oscurecer todo lo que queda fuera del recorte.
  cairo_save(cr);

  cairo_set_fill_rule(
      cr,
      CAIRO_FILL_RULE_EVEN_ODD);

  cairo_rectangle(
      cr,
      0,
      0,
      widget_width,
      widget_height);

  cairo_rectangle(
      cr,
      x,
      y,
      width,
      height);

  cairo_set_source_rgba(
      cr,
      0.0,
      0.0,
      0.0,
      0.48);

  cairo_fill(cr);

  cairo_restore(cr);

  // Marco del crop.
  cairo_set_line_width(
      cr,
      1.0);

  cairo_set_source_rgba(
      cr,
      0.37,
      0.67,
      1.0,
      1.0);

  cairo_rectangle(
      cr,
      x + 0.5,
      y + 0.5,
      width - 1.0,
      height - 1.0);

  cairo_stroke(cr);

  // Regla de tercios.
  if (
      width >= 24.0 &&
      height >= 24.0) {
    cairo_set_source_rgba(
        cr,
        1.0,
        1.0,
        1.0,
        0.62);

    cairo_set_line_width(
        cr,
        1.0);

    for (int i = 1; i <= 2; ++i) {
      const double gx =
          x +
          width *
              i /
              3.0;

      const double gy =
          y +
          height *
              i /
              3.0;

      cairo_move_to(
          cr,
          gx,
          y);

      cairo_line_to(
          cr,
          gx,
          y + height);

      cairo_move_to(
          cr,
          x,
          gy);

      cairo_line_to(
          cr,
          x + width,
          gy);
    }

    cairo_stroke(cr);
  }

  // Handles visuales.
  constexpr double handle = 7.0;

  const double points[][2] = {
      {x, y},
      {x + width / 2.0, y},
      {x + width, y},
      {x + width, y + height / 2.0},
      {x + width, y + height},
      {x + width / 2.0, y + height},
      {x, y + height},
      {x, y + height / 2.0},
  };

  for (const auto& point : points) {
    cairo_rectangle(
        cr,
        point[0] - handle / 2.0,
        point[1] - handle / 2.0,
        handle,
        handle);

    cairo_set_source_rgb(
        cr,
        0.96,
        0.97,
        0.99);

    cairo_fill_preserve(cr);

    cairo_set_source_rgb(
        cr,
        0.08,
        0.09,
        0.11);

    cairo_stroke(cr);
  }
}


std::optional<patchy::EditColor>
eyedropper_hover_color(
    CanvasState* state) {
  if (
      state->tool != Tool::Eyedropper ||
      !state->hover_valid ||
      !canvas_cache_ready(state)) {
    return std::nullopt;
  }

  double document_x = 0.0;
  double document_y = 0.0;

  if (!document_position(
          state,
          state->hover_x,
          state->hover_y,
          &document_x,
          &document_y)) {
    return std::nullopt;
  }

  const int x =
      std::clamp(
          static_cast<int>(
              std::floor(document_x)),
          0,
          state->composite_width - 1);

  const int y =
      std::clamp(
          static_cast<int>(
              std::floor(document_y)),
          0,
          state->composite_height - 1);

  const auto* pixel =
      state->composite_rgba.data() +
      static_cast<std::size_t>(y) *
          static_cast<std::size_t>(
              state->composite_stride) +
      static_cast<std::size_t>(x) * 4U;

  return patchy::EditColor{
      pixel[0],
      pixel[1],
      pixel[2],
      pixel[3]};
}

void draw_eyedropper_overlay(
    CanvasState* state,
    cairo_t* cr) {
  const auto sampled =
      eyedropper_hover_color(
          state);

  if (!sampled.has_value()) {
    return;
  }

  const auto foreground =
      state->edit_options.primary;

  const double cx =
      state->hover_x;

  const double cy =
      state->hover_y;

  constexpr double radius =
      30.0;

  constexpr double halo_width =
      8.0;

  constexpr double color_width =
      5.0;

  constexpr double gap =
      0.18;

  constexpr double pi =
      3.14159265358979323846;

  cairo_save(cr);

  cairo_set_line_cap(
      cr,
      CAIRO_LINE_CAP_ROUND);

  // Halo oscuro del arco superior.
  cairo_new_path(cr);

  cairo_arc(
      cr,
      cx,
      cy,
      radius,
      pi + gap,
      2.0 * pi - gap);

  cairo_set_source_rgba(
      cr,
      0.0,
      0.0,
      0.0,
      0.88);

  cairo_set_line_width(
      cr,
      halo_width);

  cairo_stroke(cr);

  // Color muestreado: arco superior.
  cairo_new_path(cr);

  cairo_arc(
      cr,
      cx,
      cy,
      radius,
      pi + gap,
      2.0 * pi - gap);

  cairo_set_source_rgb(
      cr,
      sampled->r / 255.0,
      sampled->g / 255.0,
      sampled->b / 255.0);

  cairo_set_line_width(
      cr,
      color_width);

  cairo_stroke(cr);

  // Halo oscuro del arco inferior.
  cairo_new_path(cr);

  cairo_arc(
      cr,
      cx,
      cy,
      radius,
      gap,
      pi - gap);

  cairo_set_source_rgba(
      cr,
      0.0,
      0.0,
      0.0,
      0.0 + 0.88);

  cairo_set_line_width(
      cr,
      halo_width);

  cairo_stroke(cr);

  // Color frontal: arco inferior.
  cairo_new_path(cr);

  cairo_arc(
      cr,
      cx,
      cy,
      radius,
      gap,
      pi - gap);

  cairo_set_source_rgb(
      cr,
      foreground.r / 255.0,
      foreground.g / 255.0,
      foreground.b / 255.0);

  cairo_set_line_width(
      cr,
      color_width);

  cairo_stroke(cr);

  // Eyedropper precision reticle.
  // Four ticks indicate the exact sample point while
  // keeping the central pixels visible.
  cairo_new_path(cr);

  cairo_move_to(
      cr,
      cx - 10.0,
      cy);

  cairo_line_to(
      cr,
      cx - 4.0,
      cy);

  cairo_move_to(
      cr,
      cx + 4.0,
      cy);

  cairo_line_to(
      cr,
      cx + 10.0,
      cy);

  cairo_move_to(
      cr,
      cx,
      cy - 10.0);

  cairo_line_to(
      cr,
      cx,
      cy - 4.0);

  cairo_move_to(
      cr,
      cx,
      cy + 4.0);

  cairo_line_to(
      cr,
      cx,
      cy + 10.0);

  // Halo oscuro para contraste.
  cairo_set_source_rgba(
      cr,
      0.0,
      0.0,
      0.0,
      0.90);

  cairo_set_line_width(
      cr,
      3.0);

  cairo_stroke_preserve(cr);

  // Trazo interior claro.
  cairo_set_source_rgba(
      cr,
      1.0,
      1.0,
      1.0,
      0.95);

  cairo_set_line_width(
      cr,
      1.1);

  cairo_stroke(cr);

  cairo_restore(cr);
}

void draw_zoom_marquee_overlay(
    CanvasState* state,
    cairo_t* cr) {
  if (
      state->tool != Tool::Zoom ||
      !state->zoom_marquee_active) {
    return;
  }

  const double x =
      std::min(
          state->zoom_marquee_start_x,
          state->zoom_marquee_end_x);

  const double y =
      std::min(
          state->zoom_marquee_start_y,
          state->zoom_marquee_end_y);

  const double width =
      std::abs(
          state->zoom_marquee_end_x -
          state->zoom_marquee_start_x);

  const double height =
      std::abs(
          state->zoom_marquee_end_y -
          state->zoom_marquee_start_y);

  if (
      width < 1.0 ||
      height < 1.0) {
    return;
  }

  cairo_save(cr);

  cairo_rectangle(
      cr,
      x,
      y,
      width,
      height);

  cairo_set_source_rgba(
      cr,
      0.25,
      0.60,
      1.0,
      0.12);

  cairo_fill_preserve(cr);

  const double dash[] = {
      5.0,
      4.0};

  cairo_set_dash(
      cr,
      dash,
      2,
      0.0);

  cairo_set_line_width(
      cr,
      1.5);

  cairo_set_source_rgba(
      cr,
      0.35,
      0.68,
      1.0,
      0.95);

  cairo_stroke(cr);

  cairo_restore(cr);
}

}  // namespace lienzo::gnome
