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

// Move-tool preview. The committed move uses the document layer bounds.



cairo_surface_t*
make_move_preview_composite_surface(
    const patchy::PixelBuffer& rgb,
    const std::vector<std::uint8_t>& alpha) {
  if (rgb.empty()) {
    return nullptr;
  }

  cairo_surface_t* surface =
      cairo_image_surface_create(
          CAIRO_FORMAT_ARGB32,
          rgb.width(),
          rgb.height());

  if (
      surface == nullptr ||
      cairo_surface_status(surface) !=
          CAIRO_STATUS_SUCCESS) {
    if (surface != nullptr) {
      cairo_surface_destroy(surface);
    }

    return nullptr;
  }

  cairo_surface_flush(surface);

  auto* data =
      cairo_image_surface_get_data(
          surface);

  if (data == nullptr) {
    cairo_surface_destroy(surface);
    return nullptr;
  }

  const int stride =
      cairo_image_surface_get_stride(
          surface);

  for (int y = 0;
       y < rgb.height();
       ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(
            data +
            static_cast<std::size_t>(y) *
                static_cast<std::size_t>(
                    stride));

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

      const auto r =
          premultiply_channel(
              source[0],
              a);

      const auto g =
          premultiply_channel(
              source[1],
              a);

      const auto b =
          premultiply_channel(
              source[2],
              a);

      row[x] =
          (
              static_cast<std::uint32_t>(a)
                  << 24U) |
          (
              static_cast<std::uint32_t>(r)
                  << 16U) |
          (
              static_cast<std::uint32_t>(g)
                  << 8U) |
          static_cast<std::uint32_t>(b);
    }
  }

  cairo_surface_mark_dirty(surface);

  return surface;
}

cairo_surface_t*
make_move_preview_layer_surface(
    const patchy::Layer& layer) {
  const auto& pixels =
      layer.pixels();

  if (pixels.empty()) {
    return nullptr;
  }

  const auto format =
      pixels.format();

  if (
      format.color_mode !=
          patchy::ColorMode::RGB ||
      format.bit_depth !=
          patchy::BitDepth::UInt8 ||
      (
          format.channels != 3 &&
          format.channels != 4)) {
    return nullptr;
  }

  cairo_surface_t* surface =
      cairo_image_surface_create(
          CAIRO_FORMAT_ARGB32,
          pixels.width(),
          pixels.height());

  if (
      surface == nullptr ||
      cairo_surface_status(surface) !=
          CAIRO_STATUS_SUCCESS) {
    if (surface != nullptr) {
      cairo_surface_destroy(surface);
    }

    return nullptr;
  }

  cairo_surface_flush(surface);

  auto* destination =
      cairo_image_surface_get_data(
          surface);

  if (destination == nullptr) {
    cairo_surface_destroy(surface);
    return nullptr;
  }

  const int destination_stride =
      cairo_image_surface_get_stride(
          surface);

  const auto* source =
      pixels.data().data();

  const std::size_t source_stride =
      pixels.stride_bytes();

  const auto channels =
      static_cast<std::size_t>(
          format.channels);

  const float layer_alpha =
      std::clamp(
          layer.opacity() *
              layer.fill_opacity(),
          0.0F,
          1.0F);

  for (int y = 0;
       y < pixels.height();
       ++y) {
    const auto* source_row =
        source +
        static_cast<std::size_t>(y) *
            source_stride;

    auto* destination_row =
        reinterpret_cast<std::uint32_t*>(
            destination +
            static_cast<std::size_t>(y) *
                static_cast<std::size_t>(
                    destination_stride));

    for (int x = 0;
         x < pixels.width();
         ++x) {
      const auto* pixel =
          source_row +
          static_cast<std::size_t>(x) *
              channels;

      const std::uint8_t source_alpha =
          format.channels >= 4
              ? pixel[3]
              : 255;

      const std::uint8_t a =
          static_cast<std::uint8_t>(
              std::clamp(
                  std::lround(
                      static_cast<double>(
                          source_alpha) *
                      layer_alpha),
                  0L,
                  255L));

      const auto r =
          premultiply_channel(
              pixel[0],
              a);

      const auto g =
          premultiply_channel(
              pixel[1],
              a);

      const auto b =
          premultiply_channel(
              pixel[2],
              a);

      destination_row[x] =
          (
              static_cast<std::uint32_t>(a)
                  << 24U) |
          (
              static_cast<std::uint32_t>(r)
                  << 16U) |
          (
              static_cast<std::uint32_t>(g)
                  << 8U) |
          static_cast<std::uint32_t>(b);
    }
  }

  cairo_surface_mark_dirty(surface);

  return surface;
}

void clear_move_preview(
    CanvasState* state) {
  auto& preview =
      state->move_preview;

  if (
      preview.base_patch_surface !=
      nullptr) {
    cairo_surface_destroy(
        preview.base_patch_surface);
  }

  if (
      preview.layer_surface !=
      nullptr) {
    cairo_surface_destroy(
        preview.layer_surface);
  }

  preview =
      CanvasState::MovePreviewState{};
}

void cancel_move_preview(
    CanvasState* state) {
  if (!state->move_preview.active) {
    return;
  }

  clear_move_preview(state);

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

bool move_layer_supports_fast_preview(
    const patchy::Layer& layer) {
  if (
      !layer.visible() ||
      (
          layer.kind() !=
              patchy::LayerKind::Pixel &&
          layer.kind() !=
              patchy::LayerKind::Text)) {
    return false;
  }

  const auto& pixels =
      layer.pixels();

  if (pixels.empty()) {
    return false;
  }

  const auto format =
      pixels.format();

  const auto bounds =
      layer.bounds();

  return
      format.color_mode ==
          patchy::ColorMode::RGB &&
      format.bit_depth ==
          patchy::BitDepth::UInt8 &&
      (
          format.channels == 3 ||
          format.channels == 4) &&
      pixels.width() ==
          bounds.width &&
      pixels.height() ==
          bounds.height &&
      layer.blend_mode() ==
          patchy::BlendMode::Normal &&
      !layer.clipped() &&
      !layer.mask().has_value() &&
      layer.vector_mask() == nullptr &&
      layer.layer_style().empty() &&
      layer.smart_filter_stack() == nullptr;
}

bool begin_move_preview(
    CanvasState* state,
    double document_x,
    double document_y) {
  clear_move_preview(state);

  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return false;
  }

  if (
      patchy::layer_effectively_locks_position(
          state->document->layers(),
          *active)) {
    return false;
  }

  const auto* layer =
      state->document->find_layer(
          *active);

  if (layer == nullptr) {
    return false;
  }

  auto& preview =
      state->move_preview;

  preview.active = true;
  preview.layer_id = *active;
  preview.original_bounds =
      layer->bounds();
  preview.anchor_x = document_x;
  preview.anchor_y = document_y;

  // Complex layers still use a deferred move, but get a
  // lightweight outline instead of recompositing at pointer rate.
  if (
      !canvas_cache_ready(state) ||
      !move_layer_supports_fast_preview(
          *layer)) {
    return true;
  }

  cairo_surface_t* layer_surface =
      make_move_preview_layer_surface(
          *layer);

  if (layer_surface == nullptr) {
    return true;
  }

  const patchy::Rect canvas =
      patchy::Rect::from_size(
          state->document->width(),
          state->document->height());

  const patchy::Rect old_clip =
      patchy::intersect_rect(
          preview.original_bounds,
          canvas);

  cairo_surface_t* base_surface =
      nullptr;

  if (!old_clip.empty()) {
    // Document/PixelBuffer copies are COW. We can hide the
    // active layer in a temporary snapshot without touching the
    // live Document or copying all pixel payloads.
    patchy::Document base_document =
        *state->document;

    auto* base_layer =
        base_document.find_layer(
            *active);

    if (base_layer == nullptr) {
      cairo_surface_destroy(
          layer_surface);

      return true;
    }

    base_layer->set_visible(false);

    std::vector<std::uint8_t> alpha;

    const auto rgb =
        patchy::Compositor{}
            .flatten_rgb8_region(
                base_document,
                old_clip,
                &alpha);

    base_surface =
        make_move_preview_composite_surface(
            rgb,
            alpha);

    if (base_surface == nullptr) {
      cairo_surface_destroy(
          layer_surface);

      return true;
    }
  }

  preview.layer_surface =
      layer_surface;

  preview.base_patch_surface =
      base_surface;

  preview.base_patch_bounds =
      old_clip;

  preview.fast_surface =
      true;

  return true;
}

void update_move_preview(
    CanvasState* state,
    double widget_offset_x,
    double widget_offset_y) {
  auto& preview =
      state->move_preview;

  if (!preview.active) {
    return;
  }

  const double safe_zoom =
      std::max(
          0.0001,
          state->zoom);

  const int dx =
      static_cast<int>(
          std::lround(
              widget_offset_x /
              safe_zoom));

  const int dy =
      static_cast<int>(
          std::lround(
              widget_offset_y /
              safe_zoom));

  if (
      dx == preview.dx &&
      dy == preview.dy) {
    return;
  }

  preview.dx = dx;
  preview.dy = dy;

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

void commit_move_preview(
    CanvasState* state) {
  if (!state->move_preview.active) {
    return;
  }

  const auto preview =
      state->move_preview;

  if (
      preview.dx == 0 &&
      preview.dy == 0) {
    cancel_move_preview(state);
    return;
  }

  const auto active =
      state->document->active_layer_id();

  if (
      !active.has_value() ||
      *active != preview.layer_id ||
      patchy::layer_effectively_locks_position(
          state->document->layers(),
          preview.layer_id)) {
    cancel_move_preview(state);
    return;
  }

  if (
      state->document->find_layer(
          preview.layer_id) == nullptr) {
    cancel_move_preview(state);
    return;
  }

  // History is created only for an actual committed move:
  // click-without-motion no longer consumes an undo slot.
  push_history(state);

  const int dx =
      preview.dx;

  const int dy =
      preview.dy;

  // Remove the transient overlay before scheduling the real
  // compositor refresh. Until that refresh fires we deliberately
  // do not queue an intermediate frame, avoiding a one-frame snap
  // back to the original cached position.
  clear_move_preview(state);

  move_active_layer(
      state,
      dx,
      dy);

  notify_document_changed(state);
}

void draw_move_preview_outline(
    CanvasState* state,
    cairo_t* cr,
    const ViewGeometry& view) {
  if (!state->move_preview.active) {
    return;
  }

  auto bounds =
      state->move_preview.original_bounds;

  bounds.x +=
      state->move_preview.dx;

  bounds.y +=
      state->move_preview.dy;

  const double scale =
      std::max(
          0.0001,
          view.zoom);

  const double dash[] = {
      6.0 / scale,
      4.0 / scale};

  cairo_save(cr);

  cairo_rectangle(
      cr,
      bounds.x,
      bounds.y,
      bounds.width,
      bounds.height);

  cairo_set_dash(
      cr,
      dash,
      2,
      0.0);

  cairo_set_line_width(
      cr,
      1.5 / scale);

  cairo_set_source_rgba(
      cr,
      1.0,
      1.0,
      1.0,
      0.95);

  cairo_stroke(cr);

  cairo_restore(cr);
}

void move_active_layer(
    CanvasState* state,
    int dx,
    int dy) {
  if (dx == 0 && dy == 0) {
    return;
  }

  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  if (
      patchy::layer_effectively_locks_position(
          state->document->layers(),
          *active)) {
    return;
  }

  auto* layer =
      state->document->find_layer(
          *active);

  if (layer == nullptr) {
    return;
  }

  const auto old_bounds =
      layer->bounds();

  auto bounds =
      old_bounds;

  bounds.x += dx;
  bounds.y += dy;

  layer->set_bounds(bounds);

  patchy::translate_moved_layer_metadata(
      *layer,
      dx,
      dy,
      state->document->width(),
      state->document->height());

  refresh_canvas(
      state,
      patchy::unite_rect(
          old_bounds,
          bounds));
}

}  // namespace lienzo::gnome
