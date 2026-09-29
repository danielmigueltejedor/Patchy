#include "ui-gnome/canvas.hpp"

#include "core/layer_metadata.hpp"
#include "core/pixel_tools.hpp"
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

  patchy::EditOptions edit_options{};

  ~CanvasState() {
    if (pixbuf != nullptr) {
      g_object_unref(pixbuf);
    }
  }
};

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

  if (state->tool == Tool::Brush) {
    paint_at(
        state,
        dx,
        dy,
        dx,
        dy,
        false);
  }

  if (state->tool == Tool::Eraser) {
    paint_at(
        state,
        dx,
        dy,
        dx,
        dy,
        true);
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

  if (old_inside && new_inside) {
    if (state->tool == Tool::Brush) {
      paint_at(
          state,
          old_doc_x,
          old_doc_y,
          new_doc_x,
          new_doc_y,
          false);
    } else if (state->tool == Tool::Eraser) {
      paint_at(
          state,
          old_doc_x,
          old_doc_y,
          new_doc_x,
          new_doc_y,
          true);
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

    if (
        patchy::crop_document(
            *state->document,
            rect)) {
      state->view_initialized = false;

      refresh_canvas(state);
    }
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

  CanvasView result;

  result.widget = area;

  result.set_tool =
      [state](Tool tool) {
        state->tool = tool;
      };

  result.refresh =
      [state] {
        refresh_canvas(state);
      };

  return result;
}

}  // namespace lienzo::gnome
