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

// Composite cache and the canvas paint entry.


bool canvas_cache_ready(
    const CanvasState* state) {
  return
      state->canvas_surface != nullptr &&
      state->composite_width > 0 &&
      state->composite_height > 0 &&
      state->composite_stride ==
          state->composite_width * 4 &&
      state->composite_rgba.size() >=
          static_cast<std::size_t>(
              state->composite_stride) *
              static_cast<std::size_t>(
                  state->composite_height);
}

void ensure_canvas_storage(
    CanvasState* state,
    int width,
    int height) {
  if (
      width <= 0 ||
      height <= 0) {
    return;
  }

  const bool correct_size =
      state->canvas_surface != nullptr &&
      state->composite_width == width &&
      state->composite_height == height;

  if (correct_size) {
    return;
  }

  if (state->canvas_surface != nullptr) {
    cairo_surface_destroy(
        state->canvas_surface);

    state->canvas_surface = nullptr;
  }

  state->composite_width =
      width;

  state->composite_height =
      height;

  state->composite_stride =
      width * 4;

  state->composite_rgba.assign(
      static_cast<std::size_t>(
          state->composite_stride) *
          static_cast<std::size_t>(
              height),
      0);

  state->canvas_surface =
      cairo_image_surface_create(
          CAIRO_FORMAT_ARGB32,
          width,
          height);
}

std::uint8_t premultiply_channel(
    std::uint8_t value,
    std::uint8_t alpha) {
  return static_cast<std::uint8_t>(
      (
          static_cast<unsigned int>(
              value) *
              static_cast<unsigned int>(
                  alpha) +
          127U) /
      255U);
}

void write_composite_region(
    CanvasState* state,
    const patchy::PixelBuffer& rgb,
    const std::vector<std::uint8_t>& alpha,
    patchy::Rect region) {
  if (
      !canvas_cache_ready(state) ||
      rgb.empty()) {
    return;
  }

  cairo_surface_flush(
      state->canvas_surface);

  auto* surface_data =
      cairo_image_surface_get_data(
          state->canvas_surface);

  const int surface_stride =
      cairo_image_surface_get_stride(
          state->canvas_surface);

  for (int y = 0;
       y < rgb.height();
       ++y) {
    auto* rgba_row =
        state->composite_rgba.data() +
        static_cast<std::size_t>(
            region.y + y) *
            static_cast<std::size_t>(
                state->composite_stride) +
        static_cast<std::size_t>(
            region.x) *
            4;

    auto* surface_row =
        reinterpret_cast<std::uint32_t*>(
            surface_data +
            static_cast<std::size_t>(
                region.y + y) *
                static_cast<std::size_t>(
                    surface_stride));

    for (int x = 0;
         x < rgb.width();
         ++x) {
      const auto* source =
          rgb.pixel(x, y);

      const std::size_t index =
          static_cast<std::size_t>(y) *
              static_cast<std::size_t>(
                  rgb.width()) +
          static_cast<std::size_t>(x);

      const std::uint8_t a =
          index < alpha.size()
              ? alpha[index]
              : 255;

      const std::uint8_t r =
          source[0];

      const std::uint8_t g =
          source[1];

      const std::uint8_t b =
          source[2];

      rgba_row[x * 4 + 0] = r;
      rgba_row[x * 4 + 1] = g;
      rgba_row[x * 4 + 2] = b;
      rgba_row[x * 4 + 3] = a;

      const std::uint8_t pr =
          premultiply_channel(
              r,
              a);

      const std::uint8_t pg =
          premultiply_channel(
              g,
              a);

      const std::uint8_t pb =
          premultiply_channel(
              b,
              a);

      surface_row[
          region.x + x] =
          (
              static_cast<std::uint32_t>(a)
                  << 24U) |
          (
              static_cast<std::uint32_t>(pr)
                  << 16U) |
          (
              static_cast<std::uint32_t>(pg)
                  << 8U) |
          static_cast<std::uint32_t>(pb);
    }
  }

  cairo_surface_mark_dirty_rectangle(
      state->canvas_surface,
      region.x,
      region.y,
      rgb.width(),
      rgb.height());
}

void rebuild_canvas_cache(
    CanvasState* state) {
  std::vector<std::uint8_t> alpha;

  patchy::PixelBuffer rgb =
      patchy::Compositor{}.flatten_rgb8(
          *state->document,
          &alpha);

  ensure_canvas_storage(
      state,
      rgb.width(),
      rgb.height());

  write_composite_region(
      state,
      rgb,
      alpha,
      patchy::Rect{
          0,
          0,
          rgb.width(),
          rgb.height()});
}

void rebuild_canvas_cache_region(
    CanvasState* state,
    patchy::Rect dirty) {
  const patchy::Rect canvas =
      patchy::Rect::from_size(
          state->document->width(),
          state->document->height());

  const patchy::Rect clip =
      patchy::intersect_rect(
          dirty,
          canvas);

  if (clip.empty()) {
    return;
  }

  if (
      !canvas_cache_ready(state) ||
      state->composite_width !=
          state->document->width() ||
      state->composite_height !=
          state->document->height()) {
    rebuild_canvas_cache(
        state);

    return;
  }

  std::vector<std::uint8_t> alpha;

  patchy::PixelBuffer rgb =
      patchy::Compositor{}
          .flatten_rgb8_region(
              *state->document,
              clip,
              &alpha);

  write_composite_region(
      state,
      rgb,
      alpha,
      clip);
}

void schedule_canvas_refresh(
    CanvasState* state) {
  state->refresh_pending = true;

  if (state->refresh_timer != 0) {
    return;
  }

  state->refresh_timer =
      g_timeout_add(
          16,
          flush_canvas_refresh,
          state);
}

void refresh_canvas(
    CanvasState* state) {
  state->full_refresh_pending =
      true;

  state->pending_dirty_rect.reset();

  schedule_canvas_refresh(
      state);
}

void refresh_canvas(
    CanvasState* state,
    patchy::Rect dirty) {
  if (dirty.empty()) {
    return;
  }

  if (!state->full_refresh_pending) {
    if (
        state->pending_dirty_rect
            .has_value()) {
      state->pending_dirty_rect =
          patchy::unite_rect(
              *state->pending_dirty_rect,
              dirty);
    } else {
      state->pending_dirty_rect =
          dirty;
    }
  }

  schedule_canvas_refresh(
      state);
}

gboolean flush_canvas_refresh(
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(
          data);

  state->refresh_timer = 0;
  state->refresh_pending = false;

  if (
      state->full_refresh_pending ||
      !state->pending_dirty_rect
           .has_value()) {
    rebuild_canvas_cache(
        state);
  } else {
    rebuild_canvas_cache_region(
        state,
        *state->pending_dirty_rect);
  }

  state->full_refresh_pending =
      false;

  state->pending_dirty_rect.reset();

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));

  return G_SOURCE_REMOVE;
}

void draw_checkerboard(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height) {
  constexpr int cell = 12;

  cairo_save(cr);

  cairo_rectangle(
      cr,
      x,
      y,
      width,
      height);

  cairo_clip(cr);

  for (int py = 0;
       py < static_cast<int>(height);
       py += cell) {
    for (int px = 0;
         px < static_cast<int>(width);
         px += cell) {
      const bool dark =
          ((px / cell) +
           (py / cell)) %
              2 ==
          0;

      if (dark) {
        cairo_set_source_rgb(
            cr,
            0.72,
            0.72,
            0.72);
      } else {
        cairo_set_source_rgb(
            cr,
            0.88,
            0.88,
            0.88);
      }

      cairo_rectangle(
          cr,
          x + px,
          y + py,
          cell,
          cell);

      cairo_fill(cr);
    }
  }

  cairo_restore(cr);
}

void draw_pixel_grid_overlay(
    CanvasState* state,
    cairo_t* cr) {
  if (state->zoom < 12.0) {
    return;
  }

  const auto view =
      geometry(state);

  const int widget_width =
      gtk_widget_get_width(
          GTK_WIDGET(state->area));

  const int widget_height =
      gtk_widget_get_height(
          GTK_WIDGET(state->area));

  const int document_width =
      state->document->width();

  const int document_height =
      state->document->height();

  const int first_x =
      std::clamp(
          static_cast<int>(
              std::floor(
                  -view.x /
                  view.zoom)),
          0,
          document_width);

  const int last_x =
      std::clamp(
          static_cast<int>(
              std::ceil(
                  (widget_width -
                   view.x) /
                  view.zoom)),
          0,
          document_width);

  const int first_y =
      std::clamp(
          static_cast<int>(
              std::floor(
                  -view.y /
                  view.zoom)),
          0,
          document_height);

  const int last_y =
      std::clamp(
          static_cast<int>(
              std::ceil(
                  (widget_height -
                   view.y) /
                  view.zoom)),
          0,
          document_height);
  const int visible_columns =
      last_x - first_x;

  const int visible_rows =
      last_y - first_y;

  // Si hay demasiadas líneas visibles,
  // la cuadrícula deja de aportar y cuesta bastante.
  if (
      visible_columns > 512 ||
      visible_rows > 512) {
    return;
  }

  cairo_save(cr);

  cairo_set_line_width(
      cr,
      1.0);

  cairo_set_source_rgba(
      cr,
      0.08,
      0.08,
      0.08,
      0.22);

  for (int x = first_x;
       x <= last_x;
       ++x) {
    const double screen_x =
        std::floor(
            view.x +
            x * view.zoom) +
        0.5;

    cairo_move_to(
        cr,
        screen_x,
        std::max(
            0.0,
            view.y));

    cairo_line_to(
        cr,
        screen_x,
        std::min(
            static_cast<double>(
                widget_height),
            view.y +
                document_height *
                    view.zoom));
  }

  for (int y = first_y;
       y <= last_y;
       ++y) {
    const double screen_y =
        std::floor(
            view.y +
            y * view.zoom) +
        0.5;

    cairo_move_to(
        cr,
        std::max(
            0.0,
            view.x),
        screen_y);

    cairo_line_to(
        cr,
        std::min(
            static_cast<double>(
                widget_width),
            view.x +
                document_width *
                    view.zoom),
        screen_y);
  }

  cairo_stroke(cr);
  cairo_restore(cr);
}


void set_preview_surface_source(
    cairo_t* cr,
    cairo_surface_t* surface,
    double x,
    double y,
    double zoom) {
  cairo_set_source_surface(
      cr,
      surface,
      x,
      y);

  cairo_pattern_t* pattern =
      cairo_get_source(cr);

  cairo_pattern_set_filter(
      pattern,
      zoom >= 1.0
          ? CAIRO_FILTER_NEAREST
          : CAIRO_FILTER_BILINEAR);

  cairo_pattern_set_extend(
      pattern,
      CAIRO_EXTEND_NONE);
}

void draw_cached_document(
    CanvasState* state,
    cairo_t* cr,
    const ViewGeometry& view) {
  cairo_save(cr);

  cairo_translate(
      cr,
      view.x,
      view.y);

  cairo_scale(
      cr,
      view.zoom,
      view.zoom);

  cairo_rectangle(
      cr,
      0.0,
      0.0,
      state->document->width(),
      state->document->height());

  cairo_clip(cr);

  const auto paint_canvas =
      [&] {
        set_preview_surface_source(
            cr,
            state->canvas_surface,
            0.0,
            0.0,
            view.zoom);

        cairo_paint(cr);
      };

  const auto& preview =
      state->move_preview;

  if (
      !preview.active ||
      !preview.fast_surface ||
      preview.layer_surface == nullptr) {
    paint_canvas();

    if (preview.active) {
      draw_move_preview_outline(
          state,
          cr,
          view);
    }

    cairo_restore(cr);
    return;
  }

  const auto hole =
      preview.base_patch_bounds;

  if (
      !hole.empty() &&
      preview.base_patch_surface !=
          nullptr) {
    // Paint the normal cached document everywhere except
    // the layer's original rectangle.
    cairo_save(cr);

    cairo_new_path(cr);

    cairo_rectangle(
        cr,
        0.0,
        0.0,
        state->document->width(),
        state->document->height());

    cairo_rectangle(
        cr,
        hole.x,
        hole.y,
        hole.width,
        hole.height);

    cairo_set_fill_rule(
        cr,
        CAIRO_FILL_RULE_EVEN_ODD);

    cairo_clip(cr);

    paint_canvas();

    cairo_restore(cr);

    // Fill the vacated rectangle with the one-time composite
    // rendered with the moving layer hidden.
    set_preview_surface_source(
        cr,
        preview.base_patch_surface,
        hole.x,
        hole.y,
        view.zoom);

    cairo_paint(cr);

  } else {
    paint_canvas();
  }

  // The hot path: from here on a mouse move only changes dx/dy.
  set_preview_surface_source(
      cr,
      preview.layer_surface,
      preview.original_bounds.x +
          preview.dx,
      preview.original_bounds.y +
          preview.dy,
      view.zoom);

  cairo_paint(cr);

  cairo_restore(cr);
}

void draw_canvas(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  ensure_initial_view(
      state,
      width,
      height);

  cairo_set_source_rgb(
      cr,
      0.18,
      0.18,
      0.18);

  cairo_paint(cr);

  if (!canvas_cache_ready(state)) {
    return;
  }

  const auto view =
      geometry(state);

  const double document_width =
      state->document->width() *
      view.zoom;

  const double document_height =
      state->document->height() *
      view.zoom;

  draw_checkerboard(
      cr,
      view.x,
      view.y,
      document_width,
      document_height);

  draw_cached_document(
      state,
      cr,
      view);

  draw_shape_preview(
      state,
      cr);

  draw_zoom_marquee_overlay(
      state,
      cr);

  draw_crop_overlay(
      state,
      cr);

  draw_pixel_grid_overlay(
      state,
      cr);

  draw_selection_overlay(
      state,
      cr);

  draw_magnetic_lasso_overlay(
      state,
      cr);

  if (state->path_controller) {
    if (state->tool == Tool::Pen) {
      // Keep already committed work paths visible while
      // continuing to work with the Pen tool.
      state->path_controller
          ->draw_path_selection(
              cr,
              view.x,
              view.y,
              view.zoom);

      // Draw the currently active, not-yet-committed
      // pen subpath on top.
      state->path_controller
          ->draw_pen(
              cr,
              view.x,
              view.y,
              view.zoom);

    } else if (
        state->tool ==
            Tool::PathSelect) {
      state->path_controller
          ->draw_path_selection(
              cr,
              view.x,
              view.y,
              view.zoom);
    }
  }

  if (
      state->retouch_controller &&
      (
          state->tool == Tool::Clone ||
          state->tool == Tool::Healing)) {
    state->retouch_controller
        ->draw_source_marker(
            cr,
            view.x,
            view.y,
            view.zoom);
  }

  draw_eyedropper_overlay(
      state,
      cr);

  draw_brush_cursor_overlay(
      state,
      cr);
}

}  // namespace lienzo::gnome
