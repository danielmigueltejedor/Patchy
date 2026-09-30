#include "ui-gnome/canvas.hpp"
#include "ui-gnome/brush_tips.hpp"
#include "ui-gnome/tools/selection_controller.hpp"
#include "ui-gnome/tools/text_controller.hpp"

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
  GdkPixbuf* pixbuf{};

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

    if (pixbuf != nullptr) {
      g_object_unref(pixbuf);
    }
  }
};

void sync_selection_to_edit_options(
    CanvasState* state);

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
  if (
      state->pixbuf == nullptr ||
      gdk_pixbuf_get_n_channels(
          state->pixbuf) != 4) {
    return false;
  }

  const int width =
      gdk_pixbuf_get_width(
          state->pixbuf);

  const int height =
      gdk_pixbuf_get_height(
          state->pixbuf);

  const int source_stride =
      gdk_pixbuf_get_rowstride(
          state->pixbuf);

  const int target_stride =
      width * 4;

  state->magnetic_source_rgba.resize(
      static_cast<std::size_t>(
          target_stride) *
      static_cast<std::size_t>(
          height));

  const auto* source =
      gdk_pixbuf_get_pixels(
          state->pixbuf);

  for (int row = 0;
       row < height;
       ++row) {
    std::memcpy(
        state->magnetic_source_rgba.data() +
            static_cast<std::size_t>(row) *
                target_stride,
        source +
            static_cast<std::size_t>(row) *
                source_stride,
        static_cast<std::size_t>(
            target_stride));
  }

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

void ensure_pixbuf_storage(
    CanvasState* state,
    int width,
    int height) {
  const bool correct_size =
      state->pixbuf != nullptr &&
      gdk_pixbuf_get_width(
          state->pixbuf) == width &&
      gdk_pixbuf_get_height(
          state->pixbuf) == height;

  if (correct_size) {
    return;
  }

  if (state->pixbuf != nullptr) {
    g_object_unref(
        state->pixbuf);

    state->pixbuf = nullptr;
  }

  state->pixbuf =
      gdk_pixbuf_new(
          GDK_COLORSPACE_RGB,
          TRUE,
          8,
          width,
          height);
}

void write_composite_region(
    CanvasState* state,
    const patchy::PixelBuffer& rgb,
    const std::vector<std::uint8_t>& alpha,
    patchy::Rect region) {
  if (
      state->pixbuf == nullptr ||
      rgb.empty()) {
    return;
  }

  auto* destination =
      gdk_pixbuf_get_pixels(
          state->pixbuf);

  const int destination_stride =
      gdk_pixbuf_get_rowstride(
          state->pixbuf);

  for (int y = 0;
       y < rgb.height();
       ++y) {
    auto* row =
        destination +
        static_cast<std::size_t>(
            region.y + y) *
            destination_stride +
        static_cast<std::size_t>(
            region.x) *
            4;

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

      row[x * 4 + 0] =
          source[0];

      row[x * 4 + 1] =
          source[1];

      row[x * 4 + 2] =
          source[2];

      row[x * 4 + 3] =
          index < alpha.size()
              ? alpha[index]
              : 255;
    }
  }
}

void rebuild_pixbuf(
    CanvasState* state) {
  std::vector<std::uint8_t> alpha;

  patchy::PixelBuffer rgb =
      patchy::Compositor{}.flatten_rgb8(
          *state->document,
          &alpha);

  ensure_pixbuf_storage(
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

void rebuild_pixbuf_region(
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
      state->pixbuf == nullptr ||
      gdk_pixbuf_get_width(
          state->pixbuf) !=
          state->document->width() ||
      gdk_pixbuf_get_height(
          state->pixbuf) !=
          state->document->height()) {
    rebuild_pixbuf(state);
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
    rebuild_pixbuf(
        state);
  } else {
    rebuild_pixbuf_region(
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
    case Tool::Eyedropper:
      name = "crosshair";
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

  refresh_canvas(state);
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
    GdkModifierType,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

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

  if (state->pixbuf == nullptr) {
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

  cairo_save(cr);

  cairo_translate(
      cr,
      view.x,
      view.y);

  cairo_scale(
      cr,
      view.zoom,
      view.zoom);

  gdk_cairo_set_source_pixbuf(
      cr,
      state->pixbuf,
      0,
      0);

  cairo_pattern_t* canvas_pattern =
      cairo_get_source(cr);

  cairo_pattern_set_filter(
      canvas_pattern,
      state->zoom >= 1.0
          ? CAIRO_FILTER_NEAREST
          : CAIRO_FILTER_BILINEAR);

  cairo_pattern_set_extend(
      canvas_pattern,
      CAIRO_EXTEND_NONE);

  cairo_paint(cr);

  cairo_restore(cr);

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

  auto bounds =
      layer->bounds();

  bounds.x += dx;
  bounds.y += dy;

  layer->set_bounds(bounds);

  patchy::translate_moved_layer_metadata(
      *layer,
      dx,
      dy,
      state->document->width(),
      state->document->height());

  refresh_canvas(state);
}

void drag_begin(
    GtkGestureDrag* gesture,
    double x,
    double y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

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

  const auto modifiers =
      gtk_event_controller_get_current_event_state(
          GTK_EVENT_CONTROLLER(gesture));

  state->selection_combine =
      selection_combine_from_modifiers(
          modifiers);

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

    if (state->pixbuf != nullptr) {
      state->selection.quick_select_rgba(
          gdk_pixbuf_get_pixels(
              state->pixbuf),
          gdk_pixbuf_get_rowstride(
              state->pixbuf),
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

  if (
      state->tool == Tool::Brush ||
      state->tool == Tool::Eraser ||
      state->tool == Tool::Smudge ||
      state->tool == Tool::Move ||
      state->tool == Tool::Gradient ||
      state->tool == Tool::Line ||
      state->tool == Tool::Rectangle ||
      state->tool == Tool::Ellipse) {
    push_history(state);
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
      if (state->pixbuf != nullptr) {
        state->selection.quick_select_rgba(
            gdk_pixbuf_get_pixels(
                state->pixbuf),
            gdk_pixbuf_get_rowstride(
                state->pixbuf),
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
    } else if (state->tool == Tool::Move) {
      const int dx =
          static_cast<int>(
              std::lround(
                  new_doc_x -
                  old_doc_x));

      const int dy =
          static_cast<int>(
              std::lround(
                  new_doc_y -
                  old_doc_y));

      move_active_layer(
          state,
          dx,
          dy);
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

  gtk_widget_grab_focus(
      GTK_WIDGET(state->area));

  const double end_x =
      state->drag_start_x +
      offset_x;

  const double end_y =
      state->drag_start_y +
      offset_y;

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
      (void)patchy::draw_linear_gradient(
          *state->document,
          *layer,
          static_cast<int>(std::lround(x0)),
          static_cast<int>(std::lround(y0)),
          static_cast<int>(std::lround(x1)),
          static_cast<int>(std::lround(y1)),
          state->edit_options);

      refresh_canvas(state);
      notify_document_changed(state);
    }

    return;
  }

  if (
      state->tool == Tool::Line ||
      state->tool == Tool::Rectangle ||
      state->tool == Tool::Ellipse) {
    if (layer.has_value()) {
      const auto ix0 =
          static_cast<int>(
              std::lround(x0));

      const auto iy0 =
          static_cast<int>(
              std::lround(y0));

      const auto ix1 =
          static_cast<int>(
              std::lround(x1));

      const auto iy1 =
          static_cast<int>(
              std::lround(y1));

      if (state->tool == Tool::Line) {
        (void)patchy::draw_line(
            *state->document,
            *layer,
            ix0,
            iy0,
            ix1,
            iy1,
            state->edit_options,
            false);
      } else {
        patchy::Rect rect{
            static_cast<std::int32_t>(
                std::floor(
                    std::min(x0, x1))),
            static_cast<std::int32_t>(
                std::floor(
                    std::min(y0, y1))),
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
            state->tool ==
            Tool::Ellipse) {
          (void)patchy::draw_ellipse(
              *state->document,
              *layer,
              rect,
              state->edit_options,
              false);
        } else {
          (void)patchy::draw_rectangle(
              *state->document,
              *layer,
              rect,
              state->edit_options,
              false);
        }
      }

      refresh_canvas(state);
      notify_document_changed(state);
    }

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
            GTK_GESTURE_SINGLE(gesture));

    zoom_around(
        state,
        x,
        y,
        button == GDK_BUTTON_SECONDARY
            ? 0.8
            : 1.25);

    return;
  }

  if (state->tool == Tool::MagicWand) {
    double document_x = 0.0;
    double document_y = 0.0;

    if (
        state->pixbuf == nullptr ||
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
        gdk_pixbuf_get_pixels(
            state->pixbuf),
        gdk_pixbuf_get_rowstride(
            state->pixbuf),
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

    (void)patchy::flood_fill(
        *state->document,
        *layer,
        static_cast<int>(
            std::lround(document_x)),
        static_cast<int>(
            std::lround(document_y)),
        state->edit_options);

    refresh_canvas(state);
    notify_document_changed(state);

    return;
  }

  if (state->tool == Tool::Eyedropper) {
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

    auto image =
        patchy::Compositor{}.flatten_rgb8(
            *state->document);

    const int px =
        std::clamp(
            static_cast<int>(
                document_x),
            0,
            image.width() - 1);

    const int py =
        std::clamp(
            static_cast<int>(
                document_y),
            0,
            image.height() - 1);

    const auto* sample =
        image.pixel(px, py);

    state->edit_options.primary =
        patchy::EditColor{
            sample[0],
            sample[1],
            sample[2],
            255};
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
          GTK_EVENT_CONTROLLER(controller));

  const bool zoom =
      state->tool == Tool::Zoom ||
      (modifiers & GDK_CONTROL_MASK) != 0;

  if (zoom) {
    double x = 0.0;
    double y = 0.0;

    GdkEvent* event =
        gtk_event_controller_get_current_event(
            GTK_EVENT_CONTROLLER(controller));

    if (
        event != nullptr &&
        gdk_event_get_position(
            event,
            &x,
            &y)) {
      zoom_around(
          state,
          x,
          y,
          dy < 0.0
              ? 1.15
              : 1.0 / 1.15);
    }

    return TRUE;
  }

  state->pan_x -=
      dx * 36.0;

  state->pan_y -=
      dy * 36.0;

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));

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

  gtk_widget_set_can_focus(
      area,
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

  rebuild_pixbuf(state);

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
      area,
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

  g_signal_connect(
      scroll,
      "scroll",
      G_CALLBACK(scroll_canvas),
      state);

  gtk_widget_add_controller(
      area,
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

        state->tool = tool;

        if (tool != Tool::Crop) {
          state->crop_session_active = false;
        }

        set_tool_cursor(state);

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

  result.commit_crop =
      [state] {
        (void)commit_crop(state);
      };

  result.cancel_crop =
      [state] {
        cancel_crop(state);
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
