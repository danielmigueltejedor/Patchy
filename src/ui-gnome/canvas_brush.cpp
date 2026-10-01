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

// Brush strokes. Pixel writes go through patchy::paint_brush_*.


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

}  // namespace lienzo::gnome
