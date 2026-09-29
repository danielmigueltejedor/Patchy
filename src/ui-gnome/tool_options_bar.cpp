#include "ui-gnome/tool_options_bar.hpp"

#include <adwaita.h>

namespace lienzo::gnome {

namespace {

struct State {
  CanvasView canvas;

  GtkWidget* root{};
  GtkWidget* paint_options{};
  GtkWidget* crop_options{};

  GtkSpinButton* size{};
  GtkSpinButton* opacity{};
  GtkSpinButton* softness{};
  GtkSpinButton* flow{};
  GtkSpinButton* smoothing{};

  GtkCheckButton* airbrush{};
  GtkDropDown* tip{};
};

GtkWidget* label(
    const char* text) {
  GtkWidget* widget =
      gtk_label_new(text);

  gtk_widget_add_css_class(
      widget,
      "dim-label");

  return widget;
}

GtkWidget* spin(
    double minimum,
    double maximum,
    double value) {
  GtkWidget* widget =
      gtk_spin_button_new_with_range(
          minimum,
          maximum,
          1.0);

  gtk_spin_button_set_value(
      GTK_SPIN_BUTTON(widget),
      value);

  gtk_widget_set_size_request(
      widget,
      74,
      -1);

  return widget;
}

void reset_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  gtk_spin_button_set_value(
      state->size,
      24);

  gtk_spin_button_set_value(
      state->opacity,
      100);

  gtk_spin_button_set_value(
      state->softness,
      20);

  gtk_spin_button_set_value(
      state->flow,
      100);

  gtk_spin_button_set_value(
      state->smoothing,
      20);

  gtk_check_button_set_active(
      state->airbrush,
      FALSE);

  gtk_drop_down_set_selected(
      state->tip,
      0);

  if (state->canvas.reset_brush_options) {
    state->canvas.reset_brush_options();
  }
}

void size_changed(
    GtkSpinButton* spin,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_brush_size) {
    state->canvas.set_brush_size(
        gtk_spin_button_get_value_as_int(spin));
  }
}

void opacity_changed(
    GtkSpinButton* spin,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_brush_opacity) {
    state->canvas.set_brush_opacity(
        gtk_spin_button_get_value_as_int(spin));
  }
}

void softness_changed(
    GtkSpinButton* spin,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_brush_softness) {
    state->canvas.set_brush_softness(
        gtk_spin_button_get_value_as_int(spin));
  }
}

void flow_changed(
    GtkSpinButton* spin,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_brush_flow) {
    state->canvas.set_brush_flow(
        gtk_spin_button_get_value_as_int(spin));
  }
}

void smoothing_changed(
    GtkSpinButton* spin,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_smoothing) {
    state->canvas.set_smoothing(
        gtk_spin_button_get_value_as_int(spin));
  }
}

void airbrush_changed(
    GtkCheckButton* button,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_airbrush) {
    state->canvas.set_airbrush(
        gtk_check_button_get_active(button));
  }
}

void tip_changed(
    GObject* object,
    GParamSpec*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  const guint selected =
      gtk_drop_down_get_selected(
          GTK_DROP_DOWN(object));

  if (state->canvas.set_brush_shape) {
    state->canvas.set_brush_shape(
        selected == 1
            ? patchy::BrushShape::Square
            : patchy::BrushShape::Round);
  }
}

void commit_crop_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.commit_crop) {
    state->canvas.commit_crop();
  }
}

void cancel_crop_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.cancel_crop) {
    state->canvas.cancel_crop();
  }
}

}  // namespace

ToolOptionsBar create_tool_options_bar(
    const CanvasView& canvas) {
  auto* state =
      new State;

  state->canvas = canvas;

  GtkWidget* root =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          6);

  state->root = root;

  gtk_widget_set_margin_top(root, 5);
  gtk_widget_set_margin_bottom(root, 5);
  gtk_widget_set_margin_start(root, 8);
  gtk_widget_set_margin_end(root, 8);

  gtk_widget_add_css_class(
      root,
      "toolbar");

  GtkWidget* paint =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          6);

  state->paint_options = paint;

  GtkWidget* defaults =
      gtk_button_new_with_label(
          "Predeterminado");

  gtk_widget_add_css_class(
      defaults,
      "flat");

  gtk_box_append(
      GTK_BOX(paint),
      defaults);

  gtk_box_append(
      GTK_BOX(paint),
      gtk_separator_new(
          GTK_ORIENTATION_VERTICAL));

  gtk_box_append(
      GTK_BOX(paint),
      label("Tamaño"));

  state->size =
      GTK_SPIN_BUTTON(
          spin(1, 5000, 24));

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->size));

  gtk_box_append(
      GTK_BOX(paint),
      label("Opacidad"));

  state->opacity =
      GTK_SPIN_BUTTON(
          spin(1, 100, 100));

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->opacity));

  gtk_box_append(
      GTK_BOX(paint),
      label("Suavidad"));

  state->softness =
      GTK_SPIN_BUTTON(
          spin(0, 100, 20));

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->softness));

  gtk_box_append(
      GTK_BOX(paint),
      label("Flujo"));

  state->flow =
      GTK_SPIN_BUTTON(
          spin(1, 100, 100));

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->flow));

  state->airbrush =
      GTK_CHECK_BUTTON(
          gtk_check_button_new_with_label(
              "Aerógrafo"));

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->airbrush));

  gtk_box_append(
      GTK_BOX(paint),
      label("Suavizado"));

  state->smoothing =
      GTK_SPIN_BUTTON(
          spin(0, 100, 20));

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->smoothing));

  gtk_box_append(
      GTK_BOX(paint),
      label("Punta"));

  const char* tips[] = {
      "Redonda",
      "Cuadrada",
      nullptr};

  state->tip =
      GTK_DROP_DOWN(
          gtk_drop_down_new_from_strings(
              tips));

  gtk_widget_set_size_request(
      GTK_WIDGET(state->tip),
      110,
      -1);

  gtk_box_append(
      GTK_BOX(paint),
      GTK_WIDGET(state->tip));

  gtk_box_append(
      GTK_BOX(root),
      paint);

  GtkWidget* crop =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          8);

  state->crop_options = crop;

  GtkWidget* crop_title =
      gtk_label_new(
          "Recorte");

  gtk_widget_add_css_class(
      crop_title,
      "heading");

  gtk_box_append(
      GTK_BOX(crop),
      crop_title);

  GtkWidget* crop_hint =
      gtk_label_new(
          "Arrastra para definir la zona · Enter aplica · Esc cancela");

  gtk_widget_add_css_class(
      crop_hint,
      "dim-label");

  gtk_box_append(
      GTK_BOX(crop),
      crop_hint);

  GtkWidget* cancel =
      gtk_button_new_with_label(
          "Cancelar");

  gtk_box_append(
      GTK_BOX(crop),
      cancel);

  GtkWidget* apply =
      gtk_button_new_with_label(
          "Aplicar");

  gtk_widget_add_css_class(
      apply,
      "suggested-action");

  gtk_box_append(
      GTK_BOX(crop),
      apply);

  gtk_box_append(
      GTK_BOX(root),
      crop);

  gtk_widget_set_visible(
      crop,
      FALSE);

  g_signal_connect(
      defaults,
      "clicked",
      G_CALLBACK(reset_clicked),
      state);

  g_signal_connect(
      state->size,
      "value-changed",
      G_CALLBACK(size_changed),
      state);

  g_signal_connect(
      state->opacity,
      "value-changed",
      G_CALLBACK(opacity_changed),
      state);

  g_signal_connect(
      state->softness,
      "value-changed",
      G_CALLBACK(softness_changed),
      state);

  g_signal_connect(
      state->flow,
      "value-changed",
      G_CALLBACK(flow_changed),
      state);

  g_signal_connect(
      state->smoothing,
      "value-changed",
      G_CALLBACK(smoothing_changed),
      state);

  g_signal_connect(
      state->airbrush,
      "toggled",
      G_CALLBACK(airbrush_changed),
      state);

  g_signal_connect(
      state->tip,
      "notify::selected",
      G_CALLBACK(tip_changed),
      state);

  g_signal_connect(
      apply,
      "clicked",
      G_CALLBACK(commit_crop_clicked),
      state);

  g_signal_connect(
      cancel,
      "clicked",
      G_CALLBACK(cancel_crop_clicked),
      state);

  g_object_set_data_full(
      G_OBJECT(root),
      "lienzo-tool-options-state",
      state,
      [](gpointer data) {
        delete static_cast<State*>(data);
      });

  ToolOptionsBar result;

  result.widget = root;

  result.set_tool =
      [state](Tool tool) {
        const bool paint =
            tool == Tool::Brush ||
            tool == Tool::Eraser ||
            tool == Tool::Smudge;

        const bool crop =
            tool == Tool::Crop;

        gtk_widget_set_visible(
            state->paint_options,
            paint);

        gtk_widget_set_visible(
            state->crop_options,
            crop);

        gtk_widget_set_visible(
            state->root,
            paint || crop);
      };

  result.set_tool(
      Tool::Brush);

  return result;
}

}  // namespace lienzo::gnome
