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

// Zoom, pan, and document/widget coordinate conversion.


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

}  // namespace lienzo::gnome
