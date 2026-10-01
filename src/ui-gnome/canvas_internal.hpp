#pragma once

// Canvas session state and the functions shared by the canvas translation units.
// Include this header only from src/ui-gnome/canvas*.cpp. It is not a public API.
//
// canvas.cpp            widget construction and the public CanvasView callbacks
// canvas_input.cpp      pointer, keyboard, and scroll dispatch
// canvas_render.cpp     composite cache and paint
// canvas_overlay.cpp    overlays on top of the cache
// canvas_brush.cpp      brush strokes through patchy::paint_brush_*
// canvas_move.cpp       move preview
// canvas_selection.cpp  selection sync and magnetic lasso
// canvas_view.cpp       zoom, pan, and coordinate conversion
// canvas_session.cpp    history, clipboard, crop commit, active layer

#include "core/brush_tip.hpp"
#include "core/document.hpp"
#include "core/magnetic_lasso.hpp"
#include "core/pixel_tools.hpp"
#include "core/stroke_stabilizer.hpp"
#include "ui-gnome/tool_palette.hpp"
#include "ui-gnome/tools/path_controller.hpp"
#include "ui-gnome/tools/retouch_controller.hpp"
#include "ui-gnome/tools/selection_controller.hpp"
#include "ui-gnome/tools/text_controller.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lienzo::gnome {

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


struct ViewGeometry {
  double x{};
  double y{};
  double zoom{1.0};
};
struct CanvasPoint {
  double x{0.0};
  double y{0.0};
};

void sync_selection_to_edit_options(
    CanvasState* state);

bool canvas_cache_ready(
    const CanvasState* state);

void cancel_magnetic_lasso(
    CanvasState* state);

bool start_magnetic_lasso(
    CanvasState* state,
    int x,
    int y,
    SelectionCombine combine);

void update_magnetic_lasso(
    CanvasState* state,
    int x,
    int y);

void add_magnetic_anchor(
    CanvasState* state);

void finish_magnetic_lasso(
    CanvasState* state);

SelectionCombine selection_combine_from_modifiers(
    GdkModifierType modifiers);

void apply_brush_tip(
    CanvasState* state,
    int index);

void update_brush_alpha(
    CanvasState* state);

patchy::Layer* find_last_pixel_layer(
    std::vector<patchy::Layer>& layers);

std::optional<patchy::LayerId> editing_layer(
    CanvasState* state);

void ensure_canvas_storage(
    CanvasState* state,
    int width,
    int height);

std::uint8_t premultiply_channel(
    std::uint8_t value,
    std::uint8_t alpha);

cairo_surface_t*
make_move_preview_composite_surface(
    const patchy::PixelBuffer& rgb,
    const std::vector<std::uint8_t>& alpha);

cairo_surface_t*
make_move_preview_layer_surface(
    const patchy::Layer& layer);

void write_composite_region(
    CanvasState* state,
    const patchy::PixelBuffer& rgb,
    const std::vector<std::uint8_t>& alpha,
    patchy::Rect region);

void rebuild_canvas_cache(
    CanvasState* state);

void rebuild_canvas_cache_region(
    CanvasState* state,
    patchy::Rect dirty);

gboolean flush_canvas_refresh(
    gpointer data);

void schedule_canvas_refresh(
    CanvasState* state);

void refresh_canvas(
    CanvasState* state);

void refresh_canvas(
    CanvasState* state,
    patchy::Rect dirty);

void notify_document_changed(
    CanvasState* state);

void push_history(
    CanvasState* state);

void move_active_layer(
    CanvasState* state,
    int dx,
    int dy);

void clear_move_preview(
    CanvasState* state);

void cancel_move_preview(
    CanvasState* state);

bool move_layer_supports_fast_preview(
    const patchy::Layer& layer);

bool begin_move_preview(
    CanvasState* state,
    double document_x,
    double document_y);

void update_move_preview(
    CanvasState* state,
    double widget_offset_x,
    double widget_offset_y);

void commit_move_preview(
    CanvasState* state);

void undo_document(
    CanvasState* state);

void redo_document(
    CanvasState* state);

void copy_active_layer(
    CanvasState* state);

void cut_active_layer(
    CanvasState* state);

void paste_layer(
    CanvasState* state);

gboolean airbrush_tick(
    gpointer data);

void start_airbrush_timer(
    CanvasState* state);

void stop_airbrush_timer(
    CanvasState* state);

void ensure_initial_view(
    CanvasState* state,
    int width,
    int height);

ViewGeometry geometry(
    CanvasState* state);

bool document_position(
    CanvasState* state,
    double widget_x,
    double widget_y,
    double* x,
    double* y);

void draw_checkerboard(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height);

double point_distance(
    CanvasPoint a,
    CanvasPoint b);

CanvasPoint midpoint(
    CanvasPoint a,
    CanvasPoint b);

CanvasPoint quadratic_point(
    CanvasPoint start,
    CanvasPoint control,
    CanvasPoint end,
    double t);

void set_tool_cursor(
    CanvasState* state);

patchy::Rect normalized_document_rect(
    double x0,
    double y0,
    double x1,
    double y1);

bool shape_drag_tool(
    Tool tool);

std::vector<CanvasPoint> polygon_vertices(
    CanvasPoint center,
    CanvasPoint edge,
    int sides);

void append_ellipse_path(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height);

void draw_shape_preview(
    CanvasState* state,
    cairo_t* cr);

patchy::Rect draw_polygon_shape(
    CanvasState* state,
    patchy::LayerId layer,
    CanvasPoint center,
    CanvasPoint edge);

void draw_selection_overlay(
    CanvasState* state,
    cairo_t* cr);

void draw_magnetic_lasso_overlay(
    CanvasState* state,
    cairo_t* cr);

void draw_pixel_grid_overlay(
    CanvasState* state,
    cairo_t* cr);

void draw_brush_cursor_overlay(
    CanvasState* state,
    cairo_t* cr);

void draw_crop_overlay(
    CanvasState* state,
    cairo_t* cr);

void paint_brush_segment_raw(
    CanvasState* state,
    CanvasPoint from,
    CanvasPoint to,
    bool erase);

void paint_quadratic_curve(
    CanvasState* state,
    CanvasPoint start,
    CanvasPoint control,
    CanvasPoint end,
    bool erase);

void begin_smoothed_brush(
    CanvasState* state,
    double x,
    double y);

void advance_smoothed_brush(
    CanvasState* state,
    double x,
    double y,
    bool erase);

void finish_smoothed_brush(
    CanvasState* state,
    double x,
    double y,
    bool erase);

std::optional<patchy::EditColor>
eyedropper_hover_color(
    CanvasState* state);

void draw_eyedropper_overlay(
    CanvasState* state,
    cairo_t* cr);

void motion_changed(
    GtkEventControllerMotion*,
    double x,
    double y,
    gpointer data);

void magnetic_motion_changed(
    GtkEventControllerMotion*,
    double x,
    double y,
    gpointer data);

void motion_left(
    GtkEventControllerMotion*,
    gpointer data);

bool commit_crop(
    CanvasState* state);

void cancel_crop(
    CanvasState* state);

gboolean key_pressed(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType modifiers,
    gpointer data);

gboolean selection_animation_tick(
    gpointer data);

void draw_zoom_marquee_overlay(
    CanvasState* state,
    cairo_t* cr);

void set_preview_surface_source(
    cairo_t* cr,
    cairo_surface_t* surface,
    double x,
    double y,
    double zoom);

void draw_move_preview_outline(
    CanvasState* state,
    cairo_t* cr,
    const ViewGeometry& view);

void draw_cached_document(
    CanvasState* state,
    cairo_t* cr,
    const ViewGeometry& view);

void draw_canvas(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer data);

void paint_at(
    CanvasState* state,
    double x0,
    double y0,
    double x1,
    double y1,
    bool erase);

bool is_retouch_tool(
    Tool tool);

RetouchMode retouch_mode_for(
    Tool tool);

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
    double y1);

void drag_begin(
    GtkGestureDrag* gesture,
    double x,
    double y,
    gpointer data);

void drag_update(
    GtkGestureDrag*,
    double offset_x,
    double offset_y,
    gpointer data);

void drag_end(
    GtkGestureDrag*,
    double offset_x,
    double offset_y,
    gpointer data);

void click_pressed(
    GtkGestureClick* gesture,
    int n_press,
    double x,
    double y,
    gpointer data);

gboolean scroll_canvas(
    GtkEventControllerScroll* controller,
    double dx,
    double dy,
    gpointer data);

}  // namespace lienzo::gnome
