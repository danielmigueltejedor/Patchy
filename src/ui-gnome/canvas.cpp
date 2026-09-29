#include "ui-gnome/canvas.hpp"

#include "core/layer_metadata.hpp"
#include "core/pixel_tools.hpp"
#include "core/stroke_stabilizer.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
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

  patchy::EditOptions edit_options{};

  int brush_opacity{100};
  int brush_flow{100};
  int smoothing{20};
  bool airbrush{false};

  bool pointer_down{false};
  double pointer_document_x{0.0};
  double pointer_document_y{0.0};
  guint airbrush_timer{0};

  patchy::StrokeStabilizer stroke_stabilizer{};

  ~CanvasState() {
    if (airbrush_timer != 0) {
      g_source_remove(airbrush_timer);
    }

    if (pixbuf != nullptr) {
      g_object_unref(pixbuf);
    }
  }
};

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

void rebuild_pixbuf(
    CanvasState* state) {
  std::vector<std::uint8_t> alpha;

  patchy::PixelBuffer rgb =
      patchy::Compositor{}.flatten_rgb8(
          *state->document,
          &alpha);

  const int width =
      rgb.width();

  const int height =
      rgb.height();

  std::vector<std::uint8_t> rgba(
      static_cast<std::size_t>(width) *
      static_cast<std::size_t>(height) *
      4);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index =
          static_cast<std::size_t>(y) *
              static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);

      const auto* pixel =
          rgb.pixel(x, y);

      rgba[index * 4 + 0] = pixel[0];
      rgba[index * 4 + 1] = pixel[1];
      rgba[index * 4 + 2] = pixel[2];
      rgba[index * 4 + 3] =
          index < alpha.size()
              ? alpha[index]
              : 255;
    }
  }

  GBytes* bytes =
      g_bytes_new(
          rgba.data(),
          rgba.size());

  GdkPixbuf* pixbuf =
      gdk_pixbuf_new_from_bytes(
          bytes,
          GDK_COLORSPACE_RGB,
          TRUE,
          8,
          width,
          height,
          width * 4);

  g_bytes_unref(bytes);

  if (state->pixbuf != nullptr) {
    g_object_unref(state->pixbuf);
  }

  state->pixbuf = pixbuf;
}

void refresh_canvas(
    CanvasState* state) {
  rebuild_pixbuf(state);

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
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

  (void)patchy::paint_brush_dab(
      *state->document,
      *layer,
      state->pointer_document_x,
      state->pointer_document_y,
      state->edit_options,
      false);

  refresh_canvas(state);

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

    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Smudge:
    case Tool::Gradient:
    case Tool::Crop:
    case Tool::Shape:
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
  const auto layer =
      editing_layer(state);

  if (!layer.has_value()) {
    return;
  }

  (void)patchy::paint_brush_segment(
      *state->document,
      *layer,
      from.x,
      from.y,
      to.x,
      to.y,
      state->edit_options,
      erase);
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

    refresh_canvas(state);
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

  refresh_canvas(state);
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

  if (
      !patchy::crop_document(
          *state->document,
          state->crop_rect)) {
    return false;
  }

  state->crop_session_active = false;
  state->view_initialized = false;

  refresh_canvas(state);

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

  cairo_pattern_set_filter(
      cairo_get_source(cr),
      CAIRO_FILTER_BILINEAR);

  cairo_paint(cr);

  cairo_restore(cr);

  draw_crop_overlay(
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
  const auto layer =
      editing_layer(state);

  if (!layer.has_value()) {
    return;
  }

  (void)patchy::paint_brush_segment(
      *state->document,
      *layer,
      x0,
      y0,
      x1,
      y1,
      state->edit_options,
      erase);

  refresh_canvas(state);
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
    GtkGestureDrag*,
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
        (void)patchy::smudge_brush_segment(
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

        refresh_canvas(state);
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
      state->tool == Tool::Brush ||
      state->tool == Tool::Eraser) {
    finish_smoothed_brush(
        state,
        x1,
        y1,
        state->tool == Tool::Eraser);

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
    }

    return;
  }

  if (state->tool == Tool::Shape) {
    if (layer.has_value()) {
      patchy::Rect rect{
          static_cast<std::int32_t>(
              std::floor(std::min(x0, x1))),
          static_cast<std::int32_t>(
              std::floor(std::min(y0, y1))),
          std::max(
              1,
              static_cast<int>(
                  std::ceil(std::abs(x1 - x0)))),
          std::max(
              1,
              static_cast<int>(
                  std::ceil(std::abs(y1 - y0))))};

      (void)patchy::draw_rectangle(
          *state->document,
          *layer,
          rect,
          state->edit_options,
          false);

      refresh_canvas(state);
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
    int,
    double x,
    double y,
    gpointer data) {
  auto* state =
      static_cast<CanvasState*>(data);

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

  rebuild_pixbuf(state);

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

  result.widget = area;

  result.set_tool =
      [state](Tool tool) {
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
      [state](int value) {
        state->edit_options.brush_size =
            std::clamp(value, 1, 5000);

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

  return result;
}

}  // namespace lienzo::gnome
