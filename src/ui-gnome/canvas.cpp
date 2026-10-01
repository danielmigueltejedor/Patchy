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


CanvasView create_canvas_view(
    patchy::Document& document,
    Tool initial_tool,
    const CanvasPreview* prepared) {
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

  state->composite_slot =
      std::make_shared<std::function<void(
          const std::uint8_t*,
          int,
          int,
          int)>>();

  if (
      prepared != nullptr &&
      prepared->width > 0 &&
      prepared->height > 0 &&
      !prepared->rgba.empty()) {
    upload_canvas_preview(
        state,
        *prepared);
  } else {
    state->full_refresh_pending = true;
    schedule_canvas_refresh(state);
  }

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
        auto* canvas_state =
            static_cast<CanvasState*>(data);

        if (canvas_state->refresh_timer != 0) {
          g_source_remove(
              canvas_state->refresh_timer);
          canvas_state->refresh_timer = 0;
        }

        if (
            canvas_state
                ->selection_animation_timer !=
            0) {
          g_source_remove(
              canvas_state
                  ->selection_animation_timer);
          canvas_state
              ->selection_animation_timer = 0;
        }

        if (canvas_state->airbrush_timer != 0) {
          g_source_remove(
              canvas_state->airbrush_timer);
          canvas_state->airbrush_timer = 0;
        }

        delete canvas_state;
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

  result.set_gradient_method =
      [state](patchy::GradientMethod method) {
        state->gradient_method = method;
        gtk_widget_queue_draw(GTK_WIDGET(state->area));
      };

  result.set_gradient_opacity =
      [state](int value) {
        state->gradient_opacity =
            std::clamp(value, 1, 100) / 100.0F;
        gtk_widget_queue_draw(GTK_WIDGET(state->area));
      };

  result.set_gradient_reverse =
      [state](bool reverse) {
        state->gradient_reverse = reverse;
        gtk_widget_queue_draw(GTK_WIDGET(state->area));
      };

  result.set_flood_tolerance =
      [state](int value) {
        state->edit_options.flood_tolerance =
            std::clamp(value, 0, 255);
      };

  result.set_flood_contiguous =
      [state](bool contiguous) {
        state->edit_options.flood_contiguous = contiguous;
      };

  result.set_wand_tolerance =
      [state](int value) {
        state->wand_tolerance = std::clamp(value, 0, 255);
      };

  result.set_wand_contiguous =
      [state](bool contiguous) {
        state->wand_contiguous = contiguous;
      };

  result.set_tone_range =
      [state](patchy::LocalToneRange range) {
        state->local_adjustment.tone_range = range;

        if (state->retouch_controller) {
          state->retouch_controller->set_adjustment_settings(
              state->local_adjustment);
        }
      };

  result.set_protect_tones =
      [state](bool protect) {
        state->local_adjustment.protect_tones = protect;

        if (state->retouch_controller) {
          state->retouch_controller->set_adjustment_settings(
              state->local_adjustment);
        }
      };

  result.set_sponge_mode =
      [state](patchy::SpongeMode mode) {
        state->local_adjustment.sponge_mode = mode;

        if (state->retouch_controller) {
          state->retouch_controller->set_adjustment_settings(
              state->local_adjustment);
        }
      };

  result.set_healing_diffusion =
      [state](int diffusion) {
        state->healing_diffusion = std::clamp(diffusion, 1, 7);

        if (state->retouch_controller) {
          state->retouch_controller->set_healing_diffusion(
              state->healing_diffusion);
        }
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
        return commit_crop(state);
      };

  result.cancel_crop =
      [state] {
        return cancel_crop(state);
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

  result.set_composite_callback =
      [slot = state->composite_slot](
          std::function<void(
              const std::uint8_t*,
              int,
              int,
              int)> callback) {
        if (slot) {
          *slot = std::move(callback);
        }
      };

  result.publish_composite =
      [state] {
        publish_canvas_composite(state);
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
