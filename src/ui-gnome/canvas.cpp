#include "ui-gnome/canvas.hpp"
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

namespace {

struct CanvasState {
  patchy::Document* document{};
  GtkDrawingArea* area{};

  cairo_surface_t* canvas_surface{};

  std::vector<std::uint8_t>
      composite_rgba;

  int composite_width{0};
  int composite_height{0};
  int composite_stride{0};

  struct MovePreviewState {
    bool active{false};
    bool fast_surface{false};

    patchy::LayerId layer_id{};
    patchy::Rect original_bounds{};
    patchy::Rect base_patch_bounds{};

    double anchor_x{0.0};
    double anchor_y{0.0};

    int dx{0};
    int dy{0};

    cairo_surface_t* base_patch_surface{};
    cairo_surface_t* layer_surface{};
  };

  MovePreviewState move_preview;

  Tool tool{Tool::Brush};

  double zoom{1.0};
  double pan_x{0.0};
  double pan_y{0.0};
  bool view_initialized{false};

  double drag_start_x{0.0};
  double drag_start_y{0.0};
  double last_x{0.0};
  double last_y{0.0};

  double start_pan_x{0.0};
  double start_pan_y{0.0};

  // Hover/cursor overlay.
  double hover_x{0.0};
  double hover_y{0.0};
  bool hover_valid{false};

  // Brush path interpolation.  The Qt frontend does the same basic
  // midpoint/quadratic reconstruction so sparse pointer events do not
  // produce a visibly polygonal stroke.
  bool brush_smoothing_active{false};
  bool brush_smoothing_had_movement{false};
  double brush_last_input_x{0.0};
  double brush_last_input_y{0.0};
  double brush_last_rendered_x{0.0};
  double brush_last_rendered_y{0.0};

  // Crop is a session, not an immediate destructive drag.
  bool crop_session_active{false};
  patchy::Rect crop_rect{};

  bool zoom_marquee_active{false};
  double zoom_marquee_start_x{0.0};
  double zoom_marquee_start_y{0.0};
  double zoom_marquee_end_x{0.0};
  double zoom_marquee_end_y{0.0};

  bool shape_preview_active{false};

  double shape_preview_start_x{0.0};
  double shape_preview_start_y{0.0};
  double shape_preview_end_x{0.0};
  double shape_preview_end_y{0.0};

  int polygon_sides{5};

  SelectionController selection;
  SelectionCombine selection_combine{
      SelectionCombine::Replace};

  patchy::LiveWireEngine magnetic_engine;

  bool magnetic_lasso_active{false};

  SelectionCombine magnetic_combine{
      SelectionCombine::Replace};

  std::vector<std::uint8_t>
      magnetic_source_rgba;

  std::vector<SelectionPoint>
      magnetic_committed_path;

  std::vector<SelectionPoint>
      magnetic_live_path;

  int magnetic_width{10};
  int magnetic_edge_contrast{10};
  int magnetic_frequency{57};

  patchy::EditOptions edit_options{};

  std::unique_ptr<TextController>
      text_controller;

  std::unique_ptr<RetouchController>
      retouch_controller;

  std::unique_ptr<PathController>
      path_controller;

  int brush_opacity{100};
  int brush_flow{100};
  int smoothing{20};
  bool airbrush{false};

  bool pointer_down{false};
  double pointer_document_x{0.0};
  double pointer_document_y{0.0};
  guint airbrush_timer{0};

  guint refresh_timer{0};
  bool refresh_pending{false};

  bool full_refresh_pending{false};

  std::optional<patchy::Rect>
      pending_dirty_rect;
  guint selection_animation_timer{0};

  patchy::StrokeStabilizer stroke_stabilizer{};

  std::function<void()>
      document_changed_callback;

  std::vector<patchy::Document>
      undo_stack;

  std::vector<patchy::Document>
      redo_stack;

  struct ClipboardLayer {
    patchy::PixelBuffer pixels;
    patchy::Rect bounds{};
    std::string name;
  };

  std::optional<ClipboardLayer>
      clipboard;

  int brush_tip_index{0};

  patchy::BrushTipMipChain
      brush_tip_mips;

  patchy::ScaledBrushTip
      scaled_brush_tip;

  ~CanvasState() {
    if (airbrush_timer != 0) {
      g_source_remove(airbrush_timer);
    }

    if (refresh_timer != 0) {
      g_source_remove(
          refresh_timer);
    }

    if (selection_animation_timer != 0) {
      g_source_remove(
          selection_animation_timer);
    }

    if (
        move_preview.base_patch_surface !=
        nullptr) {
      cairo_surface_destroy(
          move_preview.base_patch_surface);
    }

    if (
        move_preview.layer_surface !=
        nullptr) {
      cairo_surface_destroy(
          move_preview.layer_surface);
    }

    if (canvas_surface != nullptr) {
      cairo_surface_destroy(
          canvas_surface);
    }
  }
};

void sync_selection_to_edit_options(
    CanvasState* state);

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

void cancel_magnetic_lasso(
    CanvasState* state) {
  state->magnetic_lasso_active =
      false;

  state->magnetic_committed_path.clear();
  state->magnetic_live_path.clear();
  state->magnetic_source_rgba.clear();

  state->magnetic_engine.set_image(
      nullptr,
      0,
      0,
      0);

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

bool start_magnetic_lasso(
    CanvasState* state,
    int x,
    int y,
    SelectionCombine combine) {
  if (!canvas_cache_ready(state)) {
    return false;
  }

  const int width =
      state->composite_width;

  const int height =
      state->composite_height;

  const int target_stride =
      state->composite_stride;

  state->magnetic_source_rgba =
      state->composite_rgba;

  state->magnetic_engine.set_image(
      state->magnetic_source_rgba.data(),
      width,
      height,
      target_stride);

  patchy::MagneticLassoParams params;

  params.width =
      state->magnetic_width;

  params.edge_contrast =
      state->magnetic_edge_contrast;

  state->magnetic_engine.set_params(
      params);

  const auto snapped =
      state->magnetic_engine.snap(
          patchy::PointI32{x, y});

  state->magnetic_engine.set_anchor(
      snapped);

  state->magnetic_combine =
      combine;

  state->magnetic_committed_path.clear();
  state->magnetic_live_path.clear();

  state->magnetic_committed_path.push_back(
      SelectionPoint{
          snapped.x,
          snapped.y});

  state->magnetic_lasso_active =
      true;

  return true;
}

void update_magnetic_lasso(
    CanvasState* state,
    int x,
    int y) {
  if (!state->magnetic_lasso_active) {
    return;
  }

  const auto snapped =
      state->magnetic_engine.snap(
          patchy::PointI32{x, y});

  const auto path =
      state->magnetic_engine.path_to(
          snapped);

  state->magnetic_live_path.clear();

  state->magnetic_live_path.reserve(
      path.size());

  for (const auto& point : path) {
    state->magnetic_live_path.push_back(
        SelectionPoint{
            point.x,
            point.y});
  }

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

void add_magnetic_anchor(
    CanvasState* state) {
  if (
      !state->magnetic_lasso_active ||
      state->magnetic_live_path.size() <
          2) {
    return;
  }

  for (std::size_t i = 1;
       i <
       state->magnetic_live_path.size();
       ++i) {
    state->magnetic_committed_path.push_back(
        state->magnetic_live_path[i]);
  }

  const auto anchor =
      state->magnetic_committed_path.back();

  state->magnetic_engine.set_anchor(
      patchy::PointI32{
          anchor.x,
          anchor.y});

  state->magnetic_live_path.clear();
}

void finish_magnetic_lasso(
    CanvasState* state) {
  if (!state->magnetic_lasso_active) {
    return;
  }

  std::vector<SelectionPoint> polygon =
      state->magnetic_committed_path;

  for (std::size_t i = 1;
       i < state->magnetic_live_path.size();
       ++i) {
    polygon.push_back(
        state->magnetic_live_path[i]);
  }

  if (polygon.size() >= 3) {
    const auto last =
        polygon.back();

    const auto first =
        polygon.front();

    state->magnetic_engine.set_anchor(
        patchy::PointI32{
            last.x,
            last.y});

    const auto closing =
        state->magnetic_engine.path_to(
            patchy::PointI32{
                first.x,
                first.y});

    for (std::size_t i = 1;
         i + 1 < closing.size();
         ++i) {
      polygon.push_back(
          SelectionPoint{
              closing[i].x,
              closing[i].y});
    }

    state->selection.begin_lasso(
        polygon.front().x,
        polygon.front().y,
        state->magnetic_combine);

    for (std::size_t i = 1;
         i < polygon.size();
         ++i) {
      state->selection.append_lasso(
          polygon[i].x,
          polygon[i].y);
    }

    state->selection.commit_draft();

    sync_selection_to_edit_options(
        state);
  }

  cancel_magnetic_lasso(
      state);

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

SelectionCombine selection_combine_from_modifiers(
    GdkModifierType modifiers) {
  const bool shift =
      (modifiers &
       GDK_SHIFT_MASK) != 0;

  const bool alt =
      (modifiers &
       GDK_ALT_MASK) != 0;

  if (shift && alt) {
    return SelectionCombine::Intersect;
  }

  if (shift) {
    return SelectionCombine::Add;
  }

  if (alt) {
    return SelectionCombine::Subtract;
  }

  return SelectionCombine::Replace;
}

void sync_selection_to_edit_options(
    CanvasState* state) {
  if (state->selection.empty()) {
    state->edit_options.selection.reset();
    state->edit_options.selection_mask = {};
    state->edit_options.selection_coverage = {};
    return;
  }

  state->edit_options.selection =
      state->selection.bounds();

  state->edit_options.selection_mask =
      [state](
          std::int32_t x,
          std::int32_t y) {
        return
            state->selection.selected(
                x,
                y);
      };

  state->edit_options.selection_coverage =
      [state](
          std::int32_t x,
          std::int32_t y) {
        return
            state->selection.coverage(
                x,
                y);
      };
}

void apply_brush_tip(
    CanvasState* state,
    int index) {
  const auto& tips =
      builtin_brush_tips();

  if (tips.empty()) {
    return;
  }

  index =
      std::clamp(
          index,
          0,
          static_cast<int>(
              tips.size() - 1));

  const int previous_tip_index =
      state->brush_tip_index;

  state->brush_tip_index =
      index;

  // Un pincel de tamaño 1 siempre representa exactamente
  // un píxel del documento, independientemente del preset.
  if (state->edit_options.brush_size <= 1) {
    state->brush_tip_mips = {};
    state->scaled_brush_tip = {};

    state->edit_options.brush_tip =
        nullptr;

    state->edit_options.brush_shape =
        patchy::BrushShape::Square;

    state->edit_options.brush_tip_spacing =
        1.0;

    return;
  }

  const auto& preset =
      tips[
          static_cast<std::size_t>(
              index)];

  if (preset.procedural) {
    state->edit_options.brush_tip =
        nullptr;

    state->edit_options.brush_shape =
        preset.procedural_shape;

    state->edit_options.brush_tip_spacing =
        0.25;

    return;
  }

  if (
      previous_tip_index != index ||
      state->brush_tip_mips.empty()) {
    state->brush_tip_mips =
        patchy::build_brush_tip_mips(
            preset.tip);
  }

  state->scaled_brush_tip =
      patchy::make_scaled_brush_tip(
          state->brush_tip_mips,
          state->edit_options.brush_size);

  state->edit_options.brush_tip =
      &state->scaled_brush_tip;

  state->edit_options.brush_tip_spacing =
      preset.tip.default_spacing;
}

void update_brush_alpha(
    CanvasState* state) {
  const double opacity =
      std::clamp(
          state->brush_opacity,
          1,
          100) /
      100.0;

  const double flow =
      std::clamp(
          state->brush_flow,
          1,
          100) /
      100.0;

  state->edit_options.primary.a =
      static_cast<std::uint8_t>(
          std::clamp(
              std::lround(
                  255.0 *
                  opacity *
                  flow),
              1L,
              255L));
}

patchy::Layer* find_last_pixel_layer(
    std::vector<patchy::Layer>& layers) {
  for (auto it = layers.rbegin();
       it != layers.rend();
       ++it) {
    if (it->kind() == patchy::LayerKind::Group) {
      if (auto* child =
              find_last_pixel_layer(it->children());
          child != nullptr) {
        return child;
      }
    }

    if (it->kind() == patchy::LayerKind::Pixel) {
      return &*it;
    }
  }

  return nullptr;
}

std::optional<patchy::LayerId> editing_layer(
    CanvasState* state) {
  auto active =
      state->document->active_layer_id();

  if (active.has_value()) {
    auto* layer =
        state->document->find_layer(*active);

    if (
        layer != nullptr &&
        layer->kind() == patchy::LayerKind::Pixel) {
      return active;
    }
  }

  auto* layer =
      find_last_pixel_layer(
          state->document->layers());

  if (layer == nullptr) {
    return std::nullopt;
  }

  state->document->set_active_layer(
      layer->id());

  return layer->id();
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

gboolean flush_canvas_refresh(
    gpointer data);

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

void notify_document_changed(
    CanvasState* state) {
  if (state->document_changed_callback) {
    state->document_changed_callback();
  }
}

void push_history(
    CanvasState* state) {
  state->undo_stack.push_back(
      *state->document);

  constexpr std::size_t kMaxHistory = 32;

  if (
      state->undo_stack.size() >
      kMaxHistory) {
    state->undo_stack.erase(
        state->undo_stack.begin());
  }

  state->redo_stack.clear();
}


void move_active_layer(
    CanvasState* state,
    int dx,
    int dy);

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

void undo_document(
    CanvasState* state) {
  if (state->undo_stack.empty()) {
    return;
  }

  state->redo_stack.push_back(
      *state->document);

  *state->document =
      std::move(
          state->undo_stack.back());

  state->undo_stack.pop_back();

  state->view_initialized = false;

  refresh_canvas(state);
  notify_document_changed(state);
}

void redo_document(
    CanvasState* state) {
  if (state->redo_stack.empty()) {
    return;
  }

  state->undo_stack.push_back(
      *state->document);

  *state->document =
      std::move(
          state->redo_stack.back());

  state->redo_stack.pop_back();

  state->view_initialized = false;

  refresh_canvas(state);
  notify_document_changed(state);
}

void copy_active_layer(
    CanvasState* state) {
  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  const auto* layer =
      state->document->find_layer(
          *active);

  if (
      layer == nullptr ||
      layer->kind() !=
          patchy::LayerKind::Pixel ||
      layer->pixels().empty()) {
    return;
  }

  state->clipboard =
      CanvasState::ClipboardLayer{
          layer->pixels(),
          layer->bounds(),
          layer->name()};
}

void cut_active_layer(
    CanvasState* state) {
  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  copy_active_layer(state);

  if (!state->clipboard.has_value()) {
    return;
  }

  push_history(state);

  if (
      state->document->remove_layer(
          *active)) {
    refresh_canvas(state);
    notify_document_changed(state);
  }
}

void paste_layer(
    CanvasState* state) {
  if (!state->clipboard.has_value()) {
    return;
  }

  push_history(state);

  const auto& copied =
      *state->clipboard;

  patchy::Layer layer(
      state->document->allocate_layer_id(),
      copied.name + " copy",
      copied.pixels);

  layer.set_bounds(
      copied.bounds);

  state->document->add_layer(
      std::move(layer));

  refresh_canvas(state);
  notify_document_changed(state);
}


gboolean airbrush_tick(
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  if (
      !state->pointer_down ||
      !state->airbrush ||
      state->tool != Tool::Brush) {
    state->airbrush_timer = 0;
    return G_SOURCE_REMOVE;
  }

  const auto layer =
      editing_layer(state);

  if (!layer.has_value()) {
    return G_SOURCE_CONTINUE;
  }

  const auto dirty =
      patchy::paint_brush_dab(
          *state->document,
          *layer,
          state->pointer_document_x,
          state->pointer_document_y,
          state->edit_options,
          false);

  refresh_canvas(
      state,
      dirty);

  return G_SOURCE_CONTINUE;
}

void start_airbrush_timer(
    CanvasState* state) {
  if (
      !state->airbrush ||
      state->airbrush_timer != 0) {
    return;
  }

  state->airbrush_timer =
      g_timeout_add(
          55,
          airbrush_tick,
          state);
}

void stop_airbrush_timer(
    CanvasState* state) {
  state->pointer_down = false;

  if (state->airbrush_timer != 0) {
    g_source_remove(
        state->airbrush_timer);

    state->airbrush_timer = 0;
  }
}

void ensure_initial_view(
    CanvasState* state,
    int width,
    int height) {
  if (state->view_initialized) {
    return;
  }

  const double document_width =
      state->document->width();

  const double document_height =
      state->document->height();

  if (
      width <= 0 ||
      height <= 0 ||
      document_width <= 0 ||
      document_height <= 0) {
    return;
  }

  const double fit_x =
      std::max(
          0.01,
          (width - 48.0) /
              document_width);

  const double fit_y =
      std::max(
          0.01,
          (height - 48.0) /
              document_height);

  state->zoom =
      std::min(
          1.0,
          std::min(fit_x, fit_y));

  state->view_initialized = true;
}

struct ViewGeometry {
  double x{};
  double y{};
  double zoom{1.0};
};

ViewGeometry geometry(
    CanvasState* state) {
  const int width =
      gtk_widget_get_width(
          GTK_WIDGET(state->area));

  const int height =
      gtk_widget_get_height(
          GTK_WIDGET(state->area));

  ensure_initial_view(
      state,
      width,
      height);

  const double rendered_width =
      state->document->width() *
      state->zoom;

  const double rendered_height =
      state->document->height() *
      state->zoom;

  return {
      (width - rendered_width) / 2.0 +
          state->pan_x,
      (height - rendered_height) / 2.0 +
          state->pan_y,
      state->zoom};
}

bool document_position(
    CanvasState* state,
    double widget_x,
    double widget_y,
    double* x,
    double* y) {
  const auto view =
      geometry(state);

  *x =
      (widget_x - view.x) /
      view.zoom;

  *y =
      (widget_y - view.y) /
      view.zoom;

  return
      *x >= 0.0 &&
      *y >= 0.0 &&
      *x < state->document->width() &&
      *y < state->document->height();
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


struct CanvasPoint {
  double x{0.0};
  double y{0.0};
};

double point_distance(
    CanvasPoint a,
    CanvasPoint b) {
  return std::hypot(
      b.x - a.x,
      b.y - a.y);
}

CanvasPoint midpoint(
    CanvasPoint a,
    CanvasPoint b) {
  return {
      (a.x + b.x) * 0.5,
      (a.y + b.y) * 0.5};
}

CanvasPoint quadratic_point(
    CanvasPoint start,
    CanvasPoint control,
    CanvasPoint end,
    double t) {
  const double inverse =
      1.0 - t;

  return {
      inverse * inverse * start.x +
          2.0 * inverse * t * control.x +
          t * t * end.x,
      inverse * inverse * start.y +
          2.0 * inverse * t * control.y +
          t * t * end.y};
}

void set_tool_cursor(
    CanvasState* state) {
  const char* name =
      "default";

  switch (state->tool) {
    case Tool::Move:
      name = "move";
      break;

    case Tool::Hand:
      name = "grab";
      break;

    case Tool::Zoom:
      name = "zoom-in";
      break;

    case Tool::Text:
      name = "text";
      break;

    case Tool::Marquee:
    case Tool::EllipticalMarquee:
    case Tool::Lasso:
    case Tool::MagneticLasso:
    case Tool::MagicWand:
    case Tool::QuickSelect:
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Smudge:
    case Tool::Gradient:
    case Tool::Fill:
    case Tool::Crop:
    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
    case Tool::Circle:
      name = "crosshair";
      break;

    case Tool::Eyedropper:
      name = "none";
      break;

    default:
      name = "default";
      break;
  }

  gtk_widget_set_cursor_from_name(
      GTK_WIDGET(state->area),
      name);
}

patchy::Rect normalized_document_rect(
    double x0,
    double y0,
    double x1,
    double y1) {
  const auto left =
      static_cast<std::int32_t>(
          std::floor(
              std::min(x0, x1)));

  const auto top =
      static_cast<std::int32_t>(
          std::floor(
              std::min(y0, y1)));

  const auto right =
      static_cast<std::int32_t>(
          std::ceil(
              std::max(x0, x1)));

  const auto bottom =
      static_cast<std::int32_t>(
          std::ceil(
              std::max(y0, y1)));

  return {
      left,
      top,
      std::max<std::int32_t>(
          1,
          right - left),
      std::max<std::int32_t>(
          1,
          bottom - top)};
}

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

void paint_brush_segment_raw(
    CanvasState* state,
    CanvasPoint from,
    CanvasPoint to,
    bool erase) {
  if (state->selection.quick_mask()) {
    state->selection.paint_mask_segment(
        from.x,
        from.y,
        to.x,
        to.y,
        state->edit_options.brush_size *
            0.5,
        !erase);

    sync_selection_to_edit_options(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  const auto layer =
      editing_layer(state);

  if (!layer.has_value()) {
    return;
  }

  const auto dirty =
      patchy::paint_brush_segment(
          *state->document,
          *layer,
          from.x,
          from.y,
          to.x,
          to.y,
          state->edit_options,
          erase);

  refresh_canvas(
      state,
      dirty);
}

void paint_quadratic_curve(
    CanvasState* state,
    CanvasPoint start,
    CanvasPoint control,
    CanvasPoint end,
    bool erase) {
  const double length =
      std::max(
          point_distance(start, end),
          point_distance(start, control) +
              point_distance(control, end));

  const double step_length =
      std::max(
          1.0,
          state->edit_options.brush_size *
              0.125);

  const int steps =
      std::max(
          1,
          static_cast<int>(
              std::ceil(
                  length /
                  step_length)));

  CanvasPoint previous =
      start;

  for (int step = 1;
       step <= steps;
       ++step) {
    const double t =
        static_cast<double>(step) /
        static_cast<double>(steps);

    const auto current =
        quadratic_point(
            start,
            control,
            end,
            t);

    paint_brush_segment_raw(
        state,
        previous,
        current,
        erase);

    previous =
        current;
  }
}

void begin_smoothed_brush(
    CanvasState* state,
    double x,
    double y) {
  state->brush_smoothing_active = true;
  state->brush_smoothing_had_movement = false;

  state->brush_last_input_x = x;
  state->brush_last_input_y = y;

  state->brush_last_rendered_x = x;
  state->brush_last_rendered_y = y;

  patchy::StrokeStabilizerConfig config;

  config.leash_radius =
      static_cast<double>(
          std::clamp(
              state->smoothing,
              0,
              100));

  config.pulled_string = false;
  config.catch_up = true;
  config.catch_up_on_end = true;

  state->stroke_stabilizer.begin(
      x,
      y,
      config);
}

void advance_smoothed_brush(
    CanvasState* state,
    double x,
    double y,
    bool erase) {
  if (!state->brush_smoothing_active) {
    begin_smoothed_brush(
        state,
        x,
        y);

    return;
  }

  CanvasPoint current{x, y};

  if (state->smoothing > 0) {
    const auto stable =
        state->stroke_stabilizer.move(
            x,
            y);

    current = {
        stable.x,
        stable.y};
  }

  const CanvasPoint input{
      state->brush_last_input_x,
      state->brush_last_input_y};

  const CanvasPoint rendered{
      state->brush_last_rendered_x,
      state->brush_last_rendered_y};

  if (
      point_distance(
          input,
          current) <= 0.01) {
    return;
  }

  if (state->smoothing == 0) {
    paint_brush_segment_raw(
        state,
        rendered,
        current,
        erase);

    state->brush_last_input_x =
        current.x;

    state->brush_last_input_y =
        current.y;

    state->brush_last_rendered_x =
        current.x;

    state->brush_last_rendered_y =
        current.y;

    return;
  }

  const CanvasPoint end =
      midpoint(
          input,
          current);

  paint_quadratic_curve(
      state,
      rendered,
      input,
      end,
      erase);

  state->brush_smoothing_had_movement = true;

  state->brush_last_input_x =
      current.x;

  state->brush_last_input_y =
      current.y;

  state->brush_last_rendered_x =
      end.x;

  state->brush_last_rendered_y =
      end.y;

}

void finish_smoothed_brush(
    CanvasState* state,
    double x,
    double y,
    bool erase) {
  if (!state->brush_smoothing_active) {
    return;
  }

  CanvasPoint end{x, y};

  if (state->smoothing > 0) {
    const auto stable =
        state->stroke_stabilizer.finish(
            x,
            y);

    end = {
        stable.x,
        stable.y};
  }

  const CanvasPoint rendered{
      state->brush_last_rendered_x,
      state->brush_last_rendered_y};

  const CanvasPoint control{
      state->brush_last_input_x,
      state->brush_last_input_y};

  if (
      point_distance(
          rendered,
          end) > 0.01) {
    if (state->smoothing > 0) {
      paint_quadratic_curve(
          state,
          rendered,
          control,
          end,
          erase);
    } else {
      paint_brush_segment_raw(
          state,
          rendered,
          end,
          erase);
    }
  }

  state->brush_smoothing_active = false;

  // Every rendered segment already schedules its own dirty-region
  // refresh. A full-document refresh here defeats that optimization.
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

void motion_changed(
    GtkEventControllerMotion*,
    double x,
    double y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  state->hover_x = x;
  state->hover_y = y;
  state->hover_valid = true;

  if (
      state->tool == Tool::Pen &&
      state->path_controller) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (document_position(
            state,
            x,
            y,
            &document_x,
            &document_y)) {
      state->path_controller
          ->set_hover(
              document_x,
              document_y);
    }
  }

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

void magnetic_motion_changed(
    GtkEventControllerMotion*,
    double x,
    double y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(
          data);

  if (
      !state->magnetic_lasso_active ||
      state->tool !=
          Tool::MagneticLasso) {
    return;
  }

  double document_x = 0.0;
  double document_y = 0.0;

  if (!document_position(
          state,
          x,
          y,
          &document_x,
          &document_y)) {
    return;
  }

  update_magnetic_lasso(
      state,
      static_cast<int>(
          std::lround(document_x)),
      static_cast<int>(
          std::lround(document_y)));
}

void motion_left(
    GtkEventControllerMotion*,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  state->hover_valid = false;

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

bool commit_crop(
    CanvasState* state) {
  if (
      state->tool != Tool::Crop ||
      !state->crop_session_active ||
      state->crop_rect.empty()) {
    return false;
  }

  push_history(state);

  if (
      !patchy::crop_document(
          *state->document,
          state->crop_rect)) {
    return false;
  }

  state->crop_session_active = false;
  state->view_initialized = false;

  refresh_canvas(state);
  notify_document_changed(state);

  return true;
}

void cancel_crop(
    CanvasState* state) {
  if (!state->crop_session_active) {
    return;
  }

  state->crop_session_active = false;

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

gboolean key_pressed(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType modifiers,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  // TEXT_KEYBOARD_SESSION
  if (
      state->tool == Tool::Text &&
      state->text_controller &&
      state->text_controller->active()) {
    if (keyval == GDK_KEY_Escape) {
      state->text_controller
          ->cancel();

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      return TRUE;
    }

    if (
        (
            keyval == GDK_KEY_Return ||
            keyval == GDK_KEY_KP_Enter) &&
        (modifiers &
         GDK_CONTROL_MASK) != 0) {
      state->text_controller
          ->commit();

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      return TRUE;
    }
  }

  if (
      state->tool == Tool::Pen &&
      state->path_controller) {
    if (
        keyval == GDK_KEY_Return ||
        keyval == GDK_KEY_KP_Enter) {
      const bool committed =
          state->path_controller
              ->commit_open_pen();

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      (void)committed;
      return TRUE;
    }

    if (keyval == GDK_KEY_Escape) {
      state->path_controller
          ->cancel_pen();

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      return TRUE;
    }

    if (
        keyval == GDK_KEY_BackSpace ||
        keyval == GDK_KEY_Delete) {
      state->path_controller
          ->delete_last_pen_anchor();

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      return TRUE;
    }
  }

  if (
      state->move_preview.active &&
      keyval == GDK_KEY_Escape) {
    cancel_move_preview(state);
    return TRUE;
  }

  if (
      state->zoom_marquee_active &&
      keyval == GDK_KEY_Escape) {
    state->zoom_marquee_active =
        false;

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return TRUE;
  }

  if (state->magnetic_lasso_active) {
    if (
        keyval == GDK_KEY_Return ||
        keyval == GDK_KEY_KP_Enter) {
      finish_magnetic_lasso(
          state);

      return TRUE;
    }

    if (keyval == GDK_KEY_Escape) {
      cancel_magnetic_lasso(
          state);

      return TRUE;
    }
  }

  if (
      state->tool == Tool::Crop &&
      state->crop_session_active) {
    if (
        keyval == GDK_KEY_Return ||
        keyval == GDK_KEY_KP_Enter) {
      return commit_crop(state)
                 ? TRUE
                 : FALSE;
    }

    if (keyval == GDK_KEY_Escape) {
      cancel_crop(state);
      return TRUE;
    }
  }

  return FALSE;
}

gboolean selection_animation_tick(
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(
          data);

  if (
      !state->selection.empty() ||
      state->selection.draft_kind() !=
          SelectionDraftKind::None) {
    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));
  }

  return G_SOURCE_CONTINUE;
}

void draw_zoom_marquee_overlay(
    CanvasState* state,
    cairo_t* cr);


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

void paint_at(
    CanvasState* state,
    double x0,
    double y0,
    double x1,
    double y1,
    bool erase) {
  if (state->selection.quick_mask()) {
    state->selection.paint_mask_segment(
        x0,
        y0,
        x1,
        y1,
        state->edit_options.brush_size *
            0.5,
        !erase);

    sync_selection_to_edit_options(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  const auto layer =
      editing_layer(state);

  if (!layer.has_value()) {
    return;
  }

  const auto dirty =
      patchy::paint_brush_segment(
          *state->document,
          *layer,
          x0,
          y0,
          x1,
          y1,
          state->edit_options,
          erase);

  refresh_canvas(
      state,
      dirty);
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

bool is_retouch_tool(
    Tool tool) {
  switch (tool) {
    case Tool::Clone:
    case Tool::Healing:
    case Tool::BlurBrush:
    case Tool::SharpenBrush:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Sponge:
      return true;

    default:
      return false;
  }
}

RetouchMode retouch_mode_for(
    Tool tool) {
  switch (tool) {
    case Tool::Healing:
      return RetouchMode::Healing;

    case Tool::BlurBrush:
      return RetouchMode::Blur;

    case Tool::SharpenBrush:
      return RetouchMode::Sharpen;

    case Tool::Dodge:
      return RetouchMode::Dodge;

    case Tool::Burn:
      return RetouchMode::Burn;

    case Tool::Sponge:
      return RetouchMode::Sponge;

    default:
      return RetouchMode::Clone;
  }
}

void zoom_around(
    CanvasState* state,
    double widget_x,
    double widget_y,
    double factor);

void zoom_to_widget_rect(
    CanvasState* state,
    double x0,
    double y0,
    double x1,
    double y1) {
  const auto old_view =
      geometry(state);

  const double left =
      std::min(x0, x1);

  const double top =
      std::min(y0, y1);

  const double right =
      std::max(x0, x1);

  const double bottom =
      std::max(y0, y1);

  double document_left =
      (left - old_view.x) /
      old_view.zoom;

  double document_top =
      (top - old_view.y) /
      old_view.zoom;

  double document_right =
      (right - old_view.x) /
      old_view.zoom;

  double document_bottom =
      (bottom - old_view.y) /
      old_view.zoom;

  document_left =
      std::clamp(
          document_left,
          0.0,
          static_cast<double>(
              state->document->width()));

  document_top =
      std::clamp(
          document_top,
          0.0,
          static_cast<double>(
              state->document->height()));

  document_right =
      std::clamp(
          document_right,
          0.0,
          static_cast<double>(
              state->document->width()));

  document_bottom =
      std::clamp(
          document_bottom,
          0.0,
          static_cast<double>(
              state->document->height()));

  const double selection_width =
      document_right -
      document_left;

  const double selection_height =
      document_bottom -
      document_top;

  if (
      selection_width < 1.0 ||
      selection_height < 1.0) {
    return;
  }

  const int viewport_width =
      gtk_widget_get_width(
          GTK_WIDGET(
              state->area));

  const int viewport_height =
      gtk_widget_get_height(
          GTK_WIDGET(
              state->area));

  const double new_zoom =
      std::clamp(
          std::min(
              (viewport_width - 48.0) /
                  selection_width,
              (viewport_height - 48.0) /
                  selection_height),
          0.02,
          32.0);

  const double center_x =
      (
          document_left +
          document_right) *
      0.5;

  const double center_y =
      (
          document_top +
          document_bottom) *
      0.5;

  state->zoom =
      new_zoom;

  state->pan_x =
      (
          state->document->width() *
              0.5 -
          center_x) *
      new_zoom;

  state->pan_y =
      (
          state->document->height() *
              0.5 -
          center_y) *
      new_zoom;

  gtk_widget_queue_draw(
      GTK_WIDGET(
          state->area));
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

void drag_begin(
    GtkGestureDrag* gesture,
    double x,
    double y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  // CANVAS_FORCE_FOCUS
  gtk_widget_grab_focus(
      GTK_WIDGET(
          state->area));

  state->drag_start_x = x;
  state->drag_start_y = y;
  state->last_x = x;
  state->last_y = y;

  state->start_pan_x =
      state->pan_x;

  state->start_pan_y =
      state->pan_y;

  double dx = 0.0;
  double dy = 0.0;

  if (!document_position(
          state,
          x,
          y,
          &dx,
          &dy)) {
    return;
  }

  gtk_widget_grab_focus(
      GTK_WIDGET(state->area));

  if (state->tool == Tool::Zoom) {
    state->zoom_marquee_active =
        true;

    state->zoom_marquee_start_x =
        x;

    state->zoom_marquee_start_y =
        y;

    state->zoom_marquee_end_x =
        x;

    state->zoom_marquee_end_y =
        y;

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  const auto modifiers =
      gtk_event_controller_get_current_event_state(
          GTK_EVENT_CONTROLLER(gesture));

  state->selection_combine =
      selection_combine_from_modifiers(
          modifiers);

  if (
      state->tool == Tool::Pen &&
      state->path_controller) {
    state->path_controller
        ->pen_begin(
            dx,
            dy,
            state->zoom);

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  if (
      state->tool == Tool::PathSelect &&
      state->path_controller) {
    state->path_controller
        ->begin_path_select(
            dx,
            dy,
            state->zoom);

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  if (
      is_retouch_tool(
          state->tool) &&
      state->retouch_controller) {
    if (
        (
            state->tool == Tool::Clone ||
            state->tool == Tool::Healing) &&
        (modifiers & GDK_ALT_MASK) != 0) {
      return;
    }

    if (
        (
            state->tool == Tool::Clone ||
            state->tool == Tool::Healing) &&
        !state->retouch_controller
             ->source_set()) {
      return;
    }

    if (
        !canvas_cache_ready(state) ||
        !editing_layer(state)
             .has_value()) {
      return;
    }

    push_history(state);

    const RetouchMode mode =
        retouch_mode_for(
            state->tool);

    const auto dirty =
        state->retouch_controller
            ->begin_stroke(
                mode,
                dx,
                dy,
                state->composite_rgba,
                state->composite_width,
                state->composite_height,
                state->composite_stride,
                RetouchBrushSettings{
                    state->edit_options
                        .brush_size,
                    state->edit_options
                        .brush_softness,
                    state->brush_opacity},
                [state](
                    int px,
                    int py) {
                  if (
                      state->selection
                          .empty()) {
                    return 1.0F;
                  }

                  return
                      state->selection
                          .coverage(
                              px,
                              py);
                });

    refresh_canvas(
        state,
        dirty);

    return;
  }

  if (
      state->tool == Tool::Marquee ||
      state->tool ==
          Tool::EllipticalMarquee) {
    state->selection.begin_rectangle(
        static_cast<int>(
            std::lround(dx)),
        static_cast<int>(
            std::lround(dy)),
        state->tool ==
            Tool::EllipticalMarquee,
        state->selection_combine);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));
  }

  if (state->tool == Tool::Lasso) {
    state->selection.begin_lasso(
        static_cast<int>(
            std::lround(dx)),
        static_cast<int>(
            std::lround(dy)),
        state->selection_combine);
  }

  if (state->tool == Tool::QuickSelect) {
    if (
        state->selection_combine ==
        SelectionCombine::Replace) {
      state->selection.clear();
    }

    if (canvas_cache_ready(state)) {
      state->selection.quick_select_rgba(
          state->composite_rgba.data(),
          state->composite_stride,
          static_cast<int>(
              std::lround(dx)),
          static_cast<int>(
              std::lround(dy)),
          std::max(
              4,
              state->edit_options.brush_size),
          state->selection_combine ==
                  SelectionCombine::Subtract
              ? SelectionCombine::Subtract
              : SelectionCombine::Add);

      sync_selection_to_edit_options(
          state);

      gtk_widget_queue_draw(
          GTK_WIDGET(state->area));
    }
  }

  if (state->tool == Tool::Move) {
    begin_move_preview(
        state,
        dx,
        dy);
  }

  if (
      state->tool == Tool::Brush ||
      state->tool == Tool::Eraser ||
      state->tool == Tool::Smudge ||
      state->tool == Tool::Gradient ||
      state->tool == Tool::Line ||
      state->tool == Tool::Rectangle ||
      state->tool == Tool::Ellipse ||
      state->tool == Tool::Polygon) {
    push_history(state);
  }

  if (
      shape_drag_tool(
          state->tool)) {
    state->shape_preview_active =
        true;

    state->shape_preview_start_x =
        dx;

    state->shape_preview_start_y =
        dy;

    state->shape_preview_end_x =
        dx;

    state->shape_preview_end_y =
        dy;

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));
  }

  state->pointer_down = true;
  state->pointer_document_x = dx;
  state->pointer_document_y = dy;

  if (
      state->tool == Tool::Brush &&
      state->airbrush) {
    start_airbrush_timer(state);
  }

  if (
      state->tool == Tool::Brush ||
      state->tool == Tool::Eraser) {
    begin_smoothed_brush(
        state,
        dx,
        dy);

    paint_at(
        state,
        dx,
        dy,
        dx,
        dy,
        state->tool == Tool::Eraser);
  }
}

void drag_update(
    GtkGestureDrag*,
    double offset_x,
    double offset_y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  const double x =
      state->drag_start_x +
      offset_x;

  const double y =
      state->drag_start_y +
      offset_y;

  if (
      state->tool == Tool::Zoom &&
      state->zoom_marquee_active) {
    state->zoom_marquee_end_x =
        x;

    state->zoom_marquee_end_y =
        y;

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  if (state->tool == Tool::Hand) {
    state->pan_x =
        state->start_pan_x +
        offset_x;

    state->pan_y =
        state->start_pan_y +
        offset_y;

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  if (
      state->tool == Tool::Move &&
      state->move_preview.active) {
    update_move_preview(
        state,
        offset_x,
        offset_y);

    state->last_x = x;
    state->last_y = y;

    return;
  }

  double old_doc_x = 0.0;
  double old_doc_y = 0.0;
  double new_doc_x = 0.0;
  double new_doc_y = 0.0;

  const bool old_inside =
      document_position(
          state,
          state->last_x,
          state->last_y,
          &old_doc_x,
          &old_doc_y);

  const bool new_inside =
      document_position(
          state,
          x,
          y,
          &new_doc_x,
          &new_doc_y);

  if (new_inside) {
    state->pointer_document_x =
        new_doc_x;

    state->pointer_document_y =
        new_doc_y;
  }

  if (old_inside && new_inside) {
    if (
      is_retouch_tool(
          state->tool) &&
      state->retouch_controller) {
      const auto dirty =
          state->retouch_controller
              ->stroke_to(
                  new_doc_x,
                  new_doc_y);

      refresh_canvas(
          state,
          dirty);

      state->last_x = x;
      state->last_y = y;

      return;
    }

    if (
        state->tool == Tool::Pen &&
        state->path_controller) {
      state->path_controller
          ->pen_drag(
              new_doc_x,
              new_doc_y,
              state->zoom);

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      state->last_x = x;
      state->last_y = y;

      return;
    }

    if (
        state->tool == Tool::PathSelect &&
        state->path_controller) {
      state->path_controller
          ->drag_path_select(
              new_doc_x,
              new_doc_y);

      gtk_widget_queue_draw(
          GTK_WIDGET(
              state->area));

      state->last_x = x;
      state->last_y = y;

      return;
    }
    if (
        state->tool == Tool::Marquee ||
        state->tool ==
            Tool::EllipticalMarquee) {
      state->selection.update_rectangle(
          static_cast<int>(
              std::lround(new_doc_x)),
          static_cast<int>(
              std::lround(new_doc_y)));

      gtk_widget_queue_draw(
          GTK_WIDGET(state->area));

    } else if (
        state->tool == Tool::Lasso) {
      state->selection.append_lasso(
          static_cast<int>(
              std::lround(new_doc_x)),
          static_cast<int>(
              std::lround(new_doc_y)));

      gtk_widget_queue_draw(
          GTK_WIDGET(state->area));

    } else if (
        state->tool ==
        Tool::QuickSelect) {
      if (canvas_cache_ready(state)) {
        state->selection.quick_select_rgba(
            state->composite_rgba.data(),
            state->composite_stride,
            static_cast<int>(
                std::lround(new_doc_x)),
            static_cast<int>(
                std::lround(new_doc_y)),
            std::max(
                4,
                state->edit_options.brush_size),
            state->selection_combine ==
                    SelectionCombine::Subtract
                ? SelectionCombine::Subtract
                : SelectionCombine::Add);

        sync_selection_to_edit_options(
            state);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      }

    } else if (
        state->tool == Tool::Brush ||
        state->tool == Tool::Eraser) {
      advance_smoothed_brush(
          state,
          new_doc_x,
          new_doc_y,
          state->tool == Tool::Eraser);
    } else if (
        shape_drag_tool(
            state->tool)) {
      state->shape_preview_end_x =
          new_doc_x;

      state->shape_preview_end_y =
          new_doc_y;

      if (state->tool == Tool::Circle) {
        const double dx =
            new_doc_x -
            state->shape_preview_start_x;

        const double dy =
            new_doc_y -
            state->shape_preview_start_y;

        const double side =
            std::max(
                std::abs(dx),
                std::abs(dy));

        state->shape_preview_end_x =
            state->shape_preview_start_x +
            std::copysign(
                side,
                dx == 0.0
                    ? 1.0
                    : dx);

        state->shape_preview_end_y =
            state->shape_preview_start_y +
            std::copysign(
                side,
                dy == 0.0
                    ? 1.0
                    : dy);
      }

      gtk_widget_queue_draw(
          GTK_WIDGET(state->area));

    } else if (state->tool == Tool::Crop) {
      double anchor_x = 0.0;
      double anchor_y = 0.0;

      if (document_position(
              state,
              state->drag_start_x,
              state->drag_start_y,
              &anchor_x,
              &anchor_y)) {
        state->crop_rect =
            normalized_document_rect(
                anchor_x,
                anchor_y,
                new_doc_x,
                new_doc_y);

        state->crop_session_active = true;

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      }
    } else if (state->tool == Tool::Smudge) {
      const auto layer =
          editing_layer(state);

      if (layer.has_value()) {
        const auto dirty =
            patchy::smudge_brush_segment(
                *state->document,
                *layer,
                static_cast<int>(
                    std::lround(old_doc_x)),
                static_cast<int>(
                    std::lround(old_doc_y)),
                static_cast<int>(
                    std::lround(new_doc_x)),
                static_cast<int>(
                    std::lround(new_doc_y)),
                state->edit_options);

        refresh_canvas(
            state,
            dirty);
      }
    }
  }

  state->last_x = x;
  state->last_y = y;
}

void drag_end(
    GtkGestureDrag*,
    double offset_x,
    double offset_y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  stop_airbrush_timer(state);

  if (
      is_retouch_tool(
          state->tool) &&
      state->retouch_controller) {
    state->retouch_controller
        ->end_stroke();

    notify_document_changed(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  if (
      state->tool == Tool::Pen &&
      state->path_controller) {
    state->path_controller
        ->pen_end();

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  if (
      state->tool == Tool::PathSelect &&
      state->path_controller) {
    state->path_controller
        ->end_path_select();

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  gtk_widget_grab_focus(
      GTK_WIDGET(state->area));

  const double end_x =
      state->drag_start_x +
      offset_x;

  const double end_y =
      state->drag_start_y +
      offset_y;

  if (
      state->tool == Tool::Zoom &&
      state->zoom_marquee_active) {
    state->zoom_marquee_active =
        false;

    const double distance =
        std::hypot(
            end_x -
                state->zoom_marquee_start_x,
            end_y -
                state->zoom_marquee_start_y);

    if (distance < 6.0) {
      zoom_around(
          state,
          state->zoom_marquee_start_x,
          state->zoom_marquee_start_y,
          1.25);

    } else {
      zoom_to_widget_rect(
          state,
          state->zoom_marquee_start_x,
          state->zoom_marquee_start_y,
          end_x,
          end_y);
    }

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }

  if (state->tool == Tool::Move) {
    if (state->move_preview.active) {
      const double safe_zoom =
          std::max(
              0.0001,
              state->zoom);

      state->move_preview.dx =
          static_cast<int>(
              std::lround(
                  offset_x /
                  safe_zoom));

      state->move_preview.dy =
          static_cast<int>(
              std::lround(
                  offset_y /
                  safe_zoom));

      commit_move_preview(state);
    }

    return;
  }

  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;

  if (
      !document_position(
          state,
          state->drag_start_x,
          state->drag_start_y,
          &x0,
          &y0) ||
      !document_position(
          state,
          end_x,
          end_y,
          &x1,
          &y1)) {
    return;
  }

  if (
      state->tool == Tool::Marquee ||
      state->tool ==
          Tool::EllipticalMarquee) {
    state->selection.update_rectangle(
        static_cast<int>(
            std::lround(x1)),
        static_cast<int>(
            std::lround(y1)));

    state->selection.commit_draft();

    sync_selection_to_edit_options(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  if (state->tool == Tool::Lasso) {
    state->selection.append_lasso(
        static_cast<int>(
            std::lround(x1)),
        static_cast<int>(
            std::lround(y1)));

    state->selection.commit_draft();

    sync_selection_to_edit_options(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  if (state->tool == Tool::QuickSelect) {
    sync_selection_to_edit_options(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  if (
      state->tool == Tool::Brush ||
      state->tool == Tool::Eraser) {
    finish_smoothed_brush(
        state,
        x1,
        y1,
        state->tool == Tool::Eraser);

    notify_document_changed(state);

    return;
  }

  const auto layer =
      editing_layer(state);

  if (state->tool == Tool::Gradient) {
    if (layer.has_value()) {
      const auto dirty =
          patchy::draw_linear_gradient(
              *state->document,
              *layer,
              static_cast<int>(
                  std::lround(x0)),
              static_cast<int>(
                  std::lround(y0)),
              static_cast<int>(
                  std::lround(x1)),
              static_cast<int>(
                  std::lround(y1)),
              state->edit_options);

      refresh_canvas(
          state,
          dirty);
      notify_document_changed(state);
    }

    return;
  }

  // Circle uses a square drag box.
  if (state->tool == Tool::Circle) {
    const double dx =
        x1 - x0;

    const double dy =
        y1 - y0;

    const double side =
        std::max(
            std::abs(dx),
            std::abs(dy));

    x1 =
        x0 +
        std::copysign(
            side,
            dx == 0.0
                ? 1.0
                : dx);

    y1 =
        y0 +
        std::copysign(
            side,
            dy == 0.0
                ? 1.0
                : dy);
  }

  if (
      shape_drag_tool(
          state->tool)) {
    state->shape_preview_active =
        false;

    if (layer.has_value()) {
      patchy::Rect dirty{};

      const int ix0 =
          static_cast<int>(
              std::lround(x0));

      const int iy0 =
          static_cast<int>(
              std::lround(y0));

      const int ix1 =
          static_cast<int>(
              std::lround(x1));

      const int iy1 =
          static_cast<int>(
              std::lround(y1));

      if (state->tool == Tool::Line) {
        dirty =
            patchy::draw_line(
                *state->document,
                *layer,
                ix0,
                iy0,
                ix1,
                iy1,
                state->edit_options,
                false);

      } else if (
          state->tool ==
          Tool::Polygon) {
        dirty =
            draw_polygon_shape(
                state,
                *layer,
                CanvasPoint{x0, y0},
                CanvasPoint{x1, y1});

      } else {
        patchy::Rect rect{
            static_cast<std::int32_t>(
                std::floor(
                    std::min(
                        x0,
                        x1))),
            static_cast<std::int32_t>(
                std::floor(
                    std::min(
                        y0,
                        y1))),
            std::max(
                1,
                static_cast<int>(
                    std::ceil(
                        std::abs(
                            x1 - x0)))),
            std::max(
                1,
                static_cast<int>(
                    std::ceil(
                        std::abs(
                            y1 - y0))))};

        if (
            (
                state->tool ==
                    Tool::Ellipse ||
                state->tool ==
                    Tool::Circle)) {
          dirty =
              patchy::draw_ellipse(
                  *state->document,
                  *layer,
                  rect,
                  state->edit_options,
                  false);

        } else {
          dirty =
              patchy::draw_rectangle(
                  *state->document,
                  *layer,
                  rect,
                  state->edit_options,
                  false);
        }
      }

      if (!dirty.empty()) {
        refresh_canvas(
            state,
            dirty);

        notify_document_changed(
            state);
      }
    }

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  if (state->tool == Tool::Crop) {
    state->crop_rect =
        normalized_document_rect(
            x0,
            y0,
            x1,
            y1);

    state->crop_session_active = true;

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }
}

void zoom_around(
    CanvasState* state,
    double widget_x,
    double widget_y,
    double factor) {
  double document_x = 0.0;
  double document_y = 0.0;

  if (!document_position(
          state,
          widget_x,
          widget_y,
          &document_x,
          &document_y)) {
    return;
  }

  const double new_zoom =
      std::clamp(
          state->zoom * factor,
          0.02,
          32.0);

  const int width =
      gtk_widget_get_width(
          GTK_WIDGET(state->area));

  const int height =
      gtk_widget_get_height(
          GTK_WIDGET(state->area));

  state->zoom =
      new_zoom;

  state->pan_x =
      widget_x -
      (width -
       state->document->width() *
           new_zoom) /
          2.0 -
      document_x *
          new_zoom;

  state->pan_y =
      widget_y -
      (height -
       state->document->height() *
           new_zoom) /
          2.0 -
      document_y *
          new_zoom;

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

void click_pressed(
    GtkGestureClick* gesture,
    int n_press,
    double x,
    double y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  // CANVAS_FORCE_FOCUS
  gtk_widget_grab_focus(
      GTK_WIDGET(
          state->area));

  // CANVAS_CLICK_FOCUS
  gtk_widget_grab_focus(
      GTK_WIDGET(
          state->area));

  if (
      state->retouch_controller &&
      (
          state->tool == Tool::Clone ||
          state->tool == Tool::Healing)) {
    const auto modifiers =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(
                gesture));

    const guint button =
        gtk_gesture_single_get_current_button(
            GTK_GESTURE_SINGLE(
                gesture));

    if (
        button == GDK_BUTTON_PRIMARY &&
        (modifiers & GDK_ALT_MASK) != 0) {
      double document_x = 0.0;
      double document_y = 0.0;

      if (document_position(
              state,
              x,
              y,
              &document_x,
              &document_y)) {
        state->retouch_controller
            ->set_source(
                static_cast<int>(
                    std::lround(
                        document_x)),
                static_cast<int>(
                    std::lround(
                        document_y)));

        gtk_widget_queue_draw(
            GTK_WIDGET(
                state->area));
      }

      return;
    }
  }

  if (state->tool == Tool::Text) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (!document_position(
            state,
            x,
            y,
            &document_x,
            &document_y)) {
      return;
    }

    if (state->text_controller) {
      state->text_controller->begin_point(
          static_cast<int>(
              std::lround(document_x)),
          static_cast<int>(
              std::lround(document_y)),
          x,
          y,
          state->zoom,
          state->edit_options.primary);
    }

    return;
  }

  if (
      state->tool ==
      Tool::MagneticLasso) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (!document_position(
            state,
            x,
            y,
            &document_x,
            &document_y)) {
      return;
    }

    const auto modifiers =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(
                gesture));

    if (
        !state->magnetic_lasso_active) {
      start_magnetic_lasso(
          state,
          static_cast<int>(
              std::lround(document_x)),
          static_cast<int>(
              std::lround(document_y)),
          selection_combine_from_modifiers(
              modifiers));

    } else {
      update_magnetic_lasso(
          state,
          static_cast<int>(
              std::lround(document_x)),
          static_cast<int>(
              std::lround(document_y)));

      if (n_press >= 2) {
        finish_magnetic_lasso(
            state);
      } else {
        add_magnetic_anchor(
            state);
      }
    }

    return;
  }

  if (state->tool == Tool::Zoom) {
    const guint button =
        gtk_gesture_single_get_current_button(
            GTK_GESTURE_SINGLE(
                gesture));

    if (
        button ==
        GDK_BUTTON_SECONDARY) {
      zoom_around(
          state,
          x,
          y,
          0.8);
    }

    return;
  }

  if (state->tool == Tool::MagicWand) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (
        !canvas_cache_ready(state) ||
        !document_position(
            state,
            x,
            y,
            &document_x,
            &document_y)) {
      return;
    }

    const auto modifiers =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(
                gesture));

    state->selection.magic_wand_rgba(
        state->composite_rgba.data(),
        state->composite_stride,
        static_cast<int>(
            std::lround(document_x)),
        static_cast<int>(
            std::lround(document_y)),
        32,
        true,
        selection_combine_from_modifiers(
            modifiers));

    sync_selection_to_edit_options(
        state);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

    return;
  }

  if (state->tool == Tool::Fill) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (!document_position(
            state,
            x,
            y,
            &document_x,
            &document_y)) {
      return;
    }

    const auto layer =
        editing_layer(state);

    if (!layer.has_value()) {
      return;
    }

    push_history(state);

    const auto dirty =
        patchy::flood_fill(
            *state->document,
            *layer,
            static_cast<int>(
                std::lround(document_x)),
            static_cast<int>(
                std::lround(document_y)),
            state->edit_options);

    refresh_canvas(
        state,
        dirty);
    notify_document_changed(state);

    return;
  }

  if (state->tool == Tool::Eyedropper) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (
        !canvas_cache_ready(state) ||
        !document_position(
            state,
            x,
            y,
            &document_x,
            &document_y)) {
      return;
    }

    const int px =
        std::clamp(
            static_cast<int>(
                std::floor(document_x)),
            0,
            state->composite_width - 1);

    const int py =
        std::clamp(
            static_cast<int>(
                std::floor(document_y)),
            0,
            state->composite_height - 1);

    const auto* sample =
        state->composite_rgba.data() +
        static_cast<std::size_t>(py) *
            static_cast<std::size_t>(
                state->composite_stride) +
        static_cast<std::size_t>(px) *
            4U;

    state->edit_options.primary =
        patchy::EditColor{
            sample[0],
            sample[1],
            sample[2],
            sample[3]};

    gtk_widget_queue_draw(
        GTK_WIDGET(
            state->area));

    return;
  }
}

gboolean scroll_canvas(
    GtkEventControllerScroll* controller,
    double dx,
    double dy,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

  const GdkModifierType modifiers =
      gtk_event_controller_get_current_event_state(
          GTK_EVENT_CONTROLLER(
              controller));

  const bool zoom =
      state->tool == Tool::Zoom ||
      (modifiers & GDK_CONTROL_MASK) != 0;

  if (zoom) {
    double x =
        gtk_widget_get_width(
            GTK_WIDGET(
                state->area)) *
        0.5;

    double y =
        gtk_widget_get_height(
            GTK_WIDGET(
                state->area)) *
        0.5;

    GdkEvent* event =
        gtk_event_controller_get_current_event(
            GTK_EVENT_CONTROLLER(
                controller));

    if (event != nullptr) {
      double event_x = 0.0;
      double event_y = 0.0;

      if (gdk_event_get_position(
              event,
              &event_x,
              &event_y)) {
        x = event_x;
        y = event_y;
      }
    }

    double delta = dy;

    if (
        std::abs(delta) <
        std::abs(dx)) {
      delta = dx;
    }

    if (
        std::abs(delta) <
        0.00001) {
      return TRUE;
    }

    const double factor =
        std::exp(
            -delta * 0.18);

    zoom_around(
        state,
        x,
        y,
        std::clamp(
            factor,
            0.70,
            1.43));

    return TRUE;
  }

  state->pan_x -=
      dx * 36.0;

  state->pan_y -=
      dy * 36.0;

  gtk_widget_queue_draw(
      GTK_WIDGET(
          state->area));

  return TRUE;
}

}  // namespace

CanvasView create_canvas_view(
    patchy::Document& document,
    Tool initial_tool) {
  GtkWidget* area =
      gtk_drawing_area_new();

  GtkWidget* overlay =
      gtk_overlay_new();

  gtk_overlay_set_child(
      GTK_OVERLAY(overlay),
      area);

  gtk_widget_set_hexpand(
      overlay,
      TRUE);

  gtk_widget_set_vexpand(
      overlay,
      TRUE);

  gtk_widget_set_hexpand(
      area,
      TRUE);

  gtk_widget_set_vexpand(
      area,
      TRUE);

  gtk_widget_set_focusable(
      area,
      TRUE);

  gtk_widget_set_focus_on_click(
      area,
      TRUE);

  gtk_widget_set_focusable(
      overlay,
      TRUE);

  auto* state =
      new CanvasState;

  state->document =
      &document;

  state->area =
      GTK_DRAWING_AREA(area);

  state->tool =
      initial_tool;

  state->selection.resize(
      document.width(),
      document.height());

  sync_selection_to_edit_options(
      state);

  state->edit_options.primary =
      patchy::EditColor{
          0,
          0,
          0,
          255};

  state->edit_options.secondary =
      patchy::EditColor{
          255,
          255,
          255,
          255};

  state->edit_options.brush_size = 24;
  state->edit_options.brush_softness = 20;
  state->edit_options.brush_shape =
      patchy::BrushShape::Round;

  state->brush_opacity = 100;
  state->brush_flow = 100;
  state->smoothing = 20;
  state->airbrush = false;

  update_brush_alpha(state);

  apply_brush_tip(
      state,
      0);

  rebuild_canvas_cache(state);

  state->text_controller =
      std::make_unique<TextController>(
          document,
          GTK_OVERLAY(overlay),
          [state] {
            push_history(state);
          },
          [state] {
            refresh_canvas(state);
            notify_document_changed(state);
          });


  state->retouch_controller =
      std::make_unique<RetouchController>(
          document);

  state->path_controller =
      std::make_unique<PathController>(
          document,
          [state] {
            push_history(state);
          },
          [state] {
            notify_document_changed(state);

            gtk_widget_queue_draw(
                GTK_WIDGET(
                    state->area));
          });

  g_object_set_data_full(
      G_OBJECT(area),
      "lienzo-canvas-state",
      state,
      [](gpointer data) {
        delete static_cast<CanvasState*>(data);
      });

  gtk_drawing_area_set_draw_func(
      GTK_DRAWING_AREA(area),
      draw_canvas,
      state,
      nullptr);

  state->selection_animation_timer =
      g_timeout_add(
          80,
          selection_animation_tick,
          state);

  GtkEventController* motion =
      gtk_event_controller_motion_new();

  g_signal_connect(
      motion,
      "motion",
      G_CALLBACK(motion_changed),
      state);

  g_signal_connect(
      motion,
      "leave",
      G_CALLBACK(motion_left),
      state);

  gtk_widget_add_controller(
      area,
      motion);

  GtkEventController* magnetic_motion =
      gtk_event_controller_motion_new();

  g_signal_connect(
      magnetic_motion,
      "motion",
      G_CALLBACK(
          magnetic_motion_changed),
      state);

  gtk_widget_add_controller(
      area,
      magnetic_motion);

  GtkEventController* keys =
      gtk_event_controller_key_new();

  gtk_event_controller_set_propagation_phase(
      keys,
      GTK_PHASE_CAPTURE);

  g_signal_connect(
      keys,
      "key-pressed",
      G_CALLBACK(key_pressed),
      state);

  gtk_widget_add_controller(
      overlay,
      keys);

  GtkGesture* drag =
      gtk_gesture_drag_new();

  gtk_gesture_single_set_button(
      GTK_GESTURE_SINGLE(drag),
      GDK_BUTTON_PRIMARY);

  g_signal_connect(
      drag,
      "drag-begin",
      G_CALLBACK(drag_begin),
      state);

  g_signal_connect(
      drag,
      "drag-update",
      G_CALLBACK(drag_update),
      state);

  g_signal_connect(
      drag,
      "drag-end",
      G_CALLBACK(drag_end),
      state);

  gtk_widget_add_controller(
      area,
      GTK_EVENT_CONTROLLER(drag));

  GtkGesture* click =
      gtk_gesture_click_new();

  gtk_gesture_single_set_button(
      GTK_GESTURE_SINGLE(click),
      0);

  g_signal_connect(
      click,
      "pressed",
      G_CALLBACK(click_pressed),
      state);

  gtk_widget_add_controller(
      area,
      GTK_EVENT_CONTROLLER(click));

  GtkEventController* scroll =
      gtk_event_controller_scroll_new(
          static_cast<GtkEventControllerScrollFlags>(
              GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES));

  gtk_event_controller_set_propagation_phase(
      GTK_EVENT_CONTROLLER(
          scroll),
      GTK_PHASE_CAPTURE);

  gtk_event_controller_set_propagation_phase(
      GTK_EVENT_CONTROLLER(
          scroll),
      GTK_PHASE_CAPTURE);

  g_signal_connect(
      scroll,
      "scroll",
      G_CALLBACK(scroll_canvas),
      state);

  gtk_widget_add_controller(
      overlay,
      scroll);

  set_tool_cursor(state);

  CanvasView result;

  result.widget = overlay;

  result.set_tool =
      [state](Tool tool) {
        if (
            state->text_controller &&
            state->text_controller->active() &&
            tool != Tool::Text) {
          state->text_controller->commit();
        }
        if (
            state->magnetic_lasso_active &&
            tool != Tool::MagneticLasso) {
          cancel_magnetic_lasso(
              state);
        }

        state->shape_preview_active =
            false;

        if (
            state->path_controller &&
            state->tool == Tool::Pen &&
            tool != Tool::Pen) {
          state->path_controller
              ->cancel_pen();
        }

        if (
            state->path_controller &&
            state->tool ==
                Tool::PathSelect &&
            tool !=
                Tool::PathSelect) {
          state->path_controller
              ->end_path_select();
        }

        if (
            state->retouch_controller &&
            state->tool != tool) {
          state->retouch_controller
              ->end_stroke();
        }

        if (
            state->move_preview.active &&
            state->tool == Tool::Move &&
            tool != Tool::Move) {
          cancel_move_preview(state);
        }

        state->tool = tool;

        if (tool != Tool::Crop) {
          state->crop_session_active = false;
        }

        set_tool_cursor(state);

        // CANVAS_TOOL_FOCUS
        gtk_widget_grab_focus(
            GTK_WIDGET(
                state->area));

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.refresh =
      [state] {
        refresh_canvas(state);
      };

  result.reset_brush_options =
      [state] {
        state->edit_options.brush_size = 24;
        state->edit_options.brush_softness = 20;
        state->edit_options.brush_shape =
            patchy::BrushShape::Round;

        state->brush_opacity = 100;
        state->brush_flow = 100;
        state->smoothing = 20;
        state->airbrush = false;

        update_brush_alpha(state);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.set_brush_size =
      [state](int size) {
        state->edit_options.brush_size =
            std::clamp(
                size,
                1,
                5000);

        apply_brush_tip(
            state,
            state->brush_tip_index);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.set_brush_opacity =
      [state](int value) {
        state->brush_opacity =
            std::clamp(value, 1, 100);

        update_brush_alpha(state);
      };

  result.set_brush_softness =
      [state](int value) {
        state->edit_options.brush_softness =
            std::clamp(value, 0, 100);
      };

  result.set_brush_flow =
      [state](int value) {
        state->brush_flow =
            std::clamp(value, 1, 100);

        update_brush_alpha(state);
      };

  result.set_airbrush =
      [state](bool enabled) {
        state->airbrush = enabled;

        if (!enabled) {
          stop_airbrush_timer(state);
        }
      };

  result.set_smoothing =
      [state](int value) {
        state->smoothing =
            std::clamp(value, 0, 100);
      };

  result.set_brush_shape =
      [state](patchy::BrushShape shape) {
        state->edit_options.brush_shape =
            shape;
      };

  result.set_fill_shapes =
      [state](bool enabled) {
        state->edit_options.fill_shapes =
            enabled;

        gtk_widget_queue_draw(
            GTK_WIDGET(
                state->area));
      };

  result.set_polygon_sides =
      [state](int sides) {
        state->polygon_sides =
            std::clamp(
                sides,
                3,
                32);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.commit_crop =
      [state] {
        (void)commit_crop(state);
      };

  result.cancel_crop =
      [state] {
        cancel_crop(state);
      };

  result.commit_pen =
      [state] {
        if (!state->path_controller) {
          return false;
        }

        const bool committed =
            state->path_controller
                ->commit_open_pen();

        gtk_widget_queue_draw(
            GTK_WIDGET(
                state->area));

        return committed;
      };

  result.cancel_pen =
      [state] {
        if (state->path_controller) {
          state->path_controller
              ->cancel_pen();
        }

        gtk_widget_queue_draw(
            GTK_WIDGET(
                state->area));
      };

  result.zoom_in =
      [state] {
        const double x =
            gtk_widget_get_width(
                GTK_WIDGET(
                    state->area)) *
            0.5;

        const double y =
            gtk_widget_get_height(
                GTK_WIDGET(
                    state->area)) *
            0.5;

        zoom_around(
            state,
            x,
            y,
            1.25);
      };

  result.zoom_out =
      [state] {
        const double x =
            gtk_widget_get_width(
                GTK_WIDGET(
                    state->area)) *
            0.5;

        const double y =
            gtk_widget_get_height(
                GTK_WIDGET(
                    state->area)) *
            0.5;

        zoom_around(
            state,
            x,
            y,
            0.8);
      };

  result.zoom_100 =
      [state] {
        state->zoom = 1.0;
        state->pan_x = 0.0;
        state->pan_y = 0.0;
        state->view_initialized = true;

        gtk_widget_queue_draw(
            GTK_WIDGET(
                state->area));
      };

  result.zoom_fit =
      [state] {
        state->zoom = 1.0;
        state->pan_x = 0.0;
        state->pan_y = 0.0;
        state->view_initialized = false;

        gtk_widget_queue_draw(
            GTK_WIDGET(
                state->area));
      };

  result.set_document_changed_callback =
      [state](std::function<void()> callback) {
        state->document_changed_callback =
            std::move(callback);
      };

  result.checkpoint =
      [state] {
        push_history(state);
      };

  result.undo =
      [state] {
        undo_document(state);
      };

  result.redo =
      [state] {
        redo_document(state);
      };

  result.copy_active =
      [state] {
        copy_active_layer(state);
      };

  result.cut_active =
      [state] {
        cut_active_layer(state);
      };

  result.paste =
      [state] {
        paste_layer(state);
      };

  result.set_brush_tip_index =
      [state](int index) {
        apply_brush_tip(
            state,
            index);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.foreground_color =
      [state] {
        return state->edit_options.primary;
      };

  result.background_color =
      [state] {
        return state->edit_options.secondary;
      };

  result.set_foreground_color =
      [state](patchy::EditColor color) {
        state->edit_options.primary =
            color;

        if (
            state->text_controller &&
            state->text_controller->active()) {
          state->text_controller->set_color(
              color);
        }
      };

  result.set_background_color =
      [state](patchy::EditColor color) {
        state->edit_options.secondary =
            color;
      };

  result.reset_colors =
      [state] {
        state->edit_options.primary =
            patchy::EditColor{
                0,
                0,
                0,
                255};

        state->edit_options.secondary =
            patchy::EditColor{
                255,
                255,
                255,
                255};
      };

  result.swap_colors =
      [state] {
        std::swap(
            state->edit_options.primary,
            state->edit_options.secondary);
      };

  result.select_all =
      [state] {
        state->selection.select_all();

        sync_selection_to_edit_options(
            state);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.deselect =
      [state] {
        state->selection.clear();

        sync_selection_to_edit_options(
            state);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.invert_selection =
      [state] {
        state->selection.invert();

        sync_selection_to_edit_options(
            state);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.quick_mask_enabled =
      [state] {
        return
            state->selection.quick_mask();
      };

  result.set_quick_mask =
      [state](bool enabled) {
        state->selection.set_quick_mask(
            enabled);

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.toggle_quick_mask =
      [state] {
        state->selection.toggle_quick_mask();

        gtk_widget_queue_draw(
            GTK_WIDGET(state->area));
      };

  result.set_text_family =
      [state](std::string family) {
        if (state->text_controller) {
          state->text_controller->set_family(
              std::move(family));
        }
      };

  result.set_text_size =
      [state](int size) {
        if (state->text_controller) {
          state->text_controller->set_size(
              size);
        }
      };

  result.set_text_bold =
      [state](bool bold) {
        if (state->text_controller) {
          state->text_controller->set_bold(
              bold);
        }
      };

  result.set_text_italic =
      [state](bool italic) {
        if (state->text_controller) {
          state->text_controller->set_italic(
              italic);
        }
      };

  result.set_text_alignment =
      [state](TextAlignment alignment) {
        if (state->text_controller) {
          state->text_controller->set_alignment(
              alignment);
        }
      };

  result.commit_text =
      [state] {
        if (state->text_controller) {
          state->text_controller->commit();
        }
      };

  result.cancel_text =
      [state] {
        if (state->text_controller) {
          state->text_controller->cancel();
        }
      };

  return result;
}

}  // namespace lienzo::gnome
