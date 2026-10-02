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

// Pointer, keyboard, and scroll gestures. Dispatch only.


void set_tool_cursor(
    CanvasState* state) {
  // Cada herramienta dibuja su propio puntero encima del lienzo.
  const char* name = "none";

  gtk_widget_set_cursor_from_name(
      GTK_WIDGET(state->area),
      name);
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
                RetouchBrushSettings{
                    state->edit_options
                        .brush_size,
                    state->edit_options
                        .brush_softness,
                    state->brush_opacity,
                    state->edit_options
                        .brush_roundness,
                    state->edit_options
                        .brush_angle_degrees},
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
    state->selection.begin_quick_select(
        static_cast<int>(
            std::lround(dx)),
        static_cast<int>(
            std::lround(dy)),
        std::max(
            1,
            state->edit_options.brush_size),
        state->selection_combine);

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));
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
          state->tool) ||
      state->tool == Tool::Gradient) {
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
      state->selection.extend_quick_select(
          static_cast<int>(
              std::lround(new_doc_x)),
          static_cast<int>(
              std::lround(new_doc_y)),
          std::max(
              1,
              state->edit_options.brush_size));

      gtk_widget_queue_draw(
          GTK_WIDGET(state->area));

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
            state->tool) ||
        state->tool == Tool::Gradient) {
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

  if (
      state->tool == Tool::Gradient &&
      state->shape_preview_active) {
    double doc_x = 0.0;
    double doc_y = 0.0;

    widget_to_document(
        state,
        x,
        y,
        &doc_x,
        &doc_y);

    state->shape_preview_end_x = doc_x;
    state->shape_preview_end_y = doc_y;

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));
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

  if (state->tool == Tool::Gradient) {
    double gradient_x0 = 0.0;
    double gradient_y0 = 0.0;
    double gradient_x1 = 0.0;
    double gradient_y1 = 0.0;

    widget_to_document(
        state,
        state->drag_start_x,
        state->drag_start_y,
        &gradient_x0,
        &gradient_y0);

    widget_to_document(
        state,
        end_x,
        end_y,
        &gradient_x1,
        &gradient_y1);

    state->shape_preview_active = false;

    const auto gradient_layer =
        editing_layer(state);

    if (gradient_layer.has_value()) {
      auto options = state->edit_options;
      options.primary.a = 255;
      options.secondary.a = 255;

      patchy::GradientOptions gradient;
      gradient.method = state->gradient_method;
      gradient.opacity = state->gradient_opacity;
      gradient.reverse = state->gradient_reverse;

      const auto dirty =
          patchy::draw_gradient(
              *state->document,
              *gradient_layer,
              static_cast<int>(
                  std::lround(gradient_x0)),
              static_cast<int>(
                  std::lround(gradient_y0)),
              static_cast<int>(
                  std::lround(gradient_x1)),
              static_cast<int>(
                  std::lround(gradient_y1)),
              options,
              gradient);

      refresh_canvas(state, dirty);
      notify_document_changed(state);
    }

    gtk_widget_queue_draw(
        GTK_WIDGET(state->area));

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
    if (state->tool == Tool::QuickSelect) {
      if (canvas_cache_ready(state)) {
        state->selection.finish_quick_select(
            state->composite_rgba.data(),
            state->composite_stride,
            std::max(
                1,
                state->edit_options.brush_size),
            false);
        sync_selection_to_edit_options(state);
      } else {
        state->selection.cancel_quick_select();
      }
      gtk_widget_queue_draw(
          GTK_WIDGET(state->area));
    }
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
    if (canvas_cache_ready(state)) {
      state->selection.finish_quick_select(
          state->composite_rgba.data(),
          state->composite_stride,
          std::max(
              1,
              state->edit_options.brush_size),
          false);
    } else {
      state->selection.cancel_quick_select();
    }

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
        state->wand_tolerance,
        state->wand_contiguous,
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

}  // namespace lienzo::gnome
