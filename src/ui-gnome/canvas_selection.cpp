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

// Selection sync and the magnetic lasso session.


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

}  // namespace lienzo::gnome
