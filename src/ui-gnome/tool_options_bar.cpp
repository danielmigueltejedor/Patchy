#include "ui-gnome/tool_options_bar.hpp"

#include <adwaita.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <string>
#include <vector>

namespace lienzo::gnome {

namespace {

struct State {
  CanvasView canvas;

  GtkWidget* root{};
  GtkWidget* paint_options{};
  GtkWidget* crop_options{};
  GtkWidget* text_options{};

  GtkDropDown* text_family{};
  GtkSpinButton* text_size{};
  GtkCheckButton* text_bold{};
  GtkCheckButton* text_italic{};
  GtkDropDown* text_alignment{};

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

  if (state->canvas.set_brush_tip_index) {
    state->canvas.set_brush_tip_index(
        static_cast<int>(selected));
  }
}

void text_family_changed(
    GObject* object,
    GParamSpec*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (!state->canvas.set_text_family) {
    return;
  }

  GObject* selected =
      static_cast<GObject*>(
          gtk_drop_down_get_selected_item(
              GTK_DROP_DOWN(object)));

  if (
      selected == nullptr ||
      !GTK_IS_STRING_OBJECT(selected)) {
    return;
  }

  state->canvas.set_text_family(
      gtk_string_object_get_string(
          GTK_STRING_OBJECT(selected)));
}

void text_size_changed(
    GtkSpinButton* spin,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_text_size) {
    state->canvas.set_text_size(
        gtk_spin_button_get_value_as_int(
            spin));
  }
}

void text_bold_changed(
    GtkCheckButton* button,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_text_bold) {
    state->canvas.set_text_bold(
        gtk_check_button_get_active(
            button));
  }
}

void text_italic_changed(
    GtkCheckButton* button,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.set_text_italic) {
    state->canvas.set_text_italic(
        gtk_check_button_get_active(
            button));
  }
}

void text_alignment_changed(
    GObject* object,
    GParamSpec*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (!state->canvas.set_text_alignment) {
    return;
  }

  const guint selected =
      gtk_drop_down_get_selected(
          GTK_DROP_DOWN(object));

  switch (selected) {
    case 1:
      state->canvas.set_text_alignment(
          TextAlignment::Center);
      break;

    case 2:
      state->canvas.set_text_alignment(
          TextAlignment::Right);
      break;

    default:
      state->canvas.set_text_alignment(
          TextAlignment::Left);
      break;
  }
}

void text_commit_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.commit_text) {
    state->canvas.commit_text();
  }
}

void text_cancel_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<State*>(data);

  if (state->canvas.cancel_text) {
    state->canvas.cancel_text();
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
      "Redonda dura",
      "Cuadrada",
      "Redonda suave",
      "Lápiz",
      "Rotulador",
      "Caligrafía",
      "Tiza",
      "Carboncillo",
      "Spray",
      "Aerógrafo suave",
      "Salpicadura",
      "Pincel seco",
      "Plano",
      "Abanico",
      "Cerdas",
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

  GtkWidget* text =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          6);

  state->text_options = text;

  gtk_box_append(
      GTK_BOX(text),
      label("Fuente"));

  GtkStringList* font_names =
      gtk_string_list_new(nullptr);

  PangoFontFamily** families = nullptr;
  int family_count = 0;

  pango_font_map_list_families(
      pango_cairo_font_map_get_default(),
      &families,
      &family_count);

  std::vector<std::string> names;

  names.reserve(
      static_cast<std::size_t>(
          std::max(
              0,
              family_count)));

  for (int i = 0;
       i < family_count;
       ++i) {
    const char* name =
        pango_font_family_get_name(
            families[i]);

    if (
        name != nullptr &&
        *name != 0) {
      names.emplace_back(name);
    }
  }

  g_free(families);

  std::sort(
      names.begin(),
      names.end());

  names.erase(
      std::unique(
          names.begin(),
          names.end()),
      names.end());

  guint sans_index = 0;

  for (guint i = 0;
       i < names.size();
       ++i) {
    gtk_string_list_append(
        font_names,
        names[i].c_str());

    if (names[i] == "Sans") {
      sans_index = i;
    }
  }

  state->text_family =
      GTK_DROP_DOWN(
          gtk_drop_down_new(
              G_LIST_MODEL(font_names),
              nullptr));

  g_object_unref(font_names);

  gtk_drop_down_set_enable_search(
      state->text_family,
      TRUE);

  gtk_drop_down_set_selected(
      state->text_family,
      sans_index);

  gtk_widget_set_size_request(
      GTK_WIDGET(
          state->text_family),
      180,
      -1);

  gtk_widget_set_tooltip_text(
      GTK_WIDGET(
          state->text_family),
      "Seleccionar o buscar tipografía");

  gtk_box_append(
      GTK_BOX(text),
      GTK_WIDGET(
          state->text_family));

  gtk_box_append(
      GTK_BOX(text),
      label("Tamaño"));

  state->text_size =
      GTK_SPIN_BUTTON(
          spin(
              1,
              4096,
              32));

  gtk_box_append(
      GTK_BOX(text),
      GTK_WIDGET(
          state->text_size));

  state->text_bold =
      GTK_CHECK_BUTTON(
          gtk_check_button_new_with_label(
              "Negrita"));

  gtk_box_append(
      GTK_BOX(text),
      GTK_WIDGET(
          state->text_bold));

  state->text_italic =
      GTK_CHECK_BUTTON(
          gtk_check_button_new_with_label(
              "Cursiva"));

  gtk_box_append(
      GTK_BOX(text),
      GTK_WIDGET(
          state->text_italic));

  const char* text_alignments[] = {
      "Izquierda",
      "Centro",
      "Derecha",
      nullptr};

  state->text_alignment =
      GTK_DROP_DOWN(
          gtk_drop_down_new_from_strings(
              text_alignments));

  gtk_widget_set_size_request(
      GTK_WIDGET(
          state->text_alignment),
      110,
      -1);

  gtk_box_append(
      GTK_BOX(text),
      GTK_WIDGET(
          state->text_alignment));

  GtkWidget* text_cancel =
      gtk_button_new_with_label(
          "Cancelar");

  gtk_widget_add_css_class(
      text_cancel,
      "flat");

  gtk_box_append(
      GTK_BOX(text),
      text_cancel);

  GtkWidget* text_apply =
      gtk_button_new_with_label(
          "Aplicar");

  gtk_widget_add_css_class(
      text_apply,
      "suggested-action");

  gtk_box_append(
      GTK_BOX(text),
      text_apply);

  gtk_box_append(
      GTK_BOX(root),
      text);

  gtk_widget_set_visible(
      text,
      FALSE);

  g_signal_connect(
      state->text_family,
      "notify::selected",
      G_CALLBACK(
          text_family_changed),
      state);

  g_signal_connect(
      state->text_size,
      "value-changed",
      G_CALLBACK(
          text_size_changed),
      state);

  g_signal_connect(
      state->text_bold,
      "toggled",
      G_CALLBACK(
          text_bold_changed),
      state);

  g_signal_connect(
      state->text_italic,
      "toggled",
      G_CALLBACK(
          text_italic_changed),
      state);

  g_signal_connect(
      state->text_alignment,
      "notify::selected",
      G_CALLBACK(
          text_alignment_changed),
      state);

  g_signal_connect(
      text_apply,
      "clicked",
      G_CALLBACK(
          text_commit_clicked),
      state);

  g_signal_connect(
      text_cancel,
      "clicked",
      G_CALLBACK(
          text_cancel_clicked),
      state);

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

        const bool text =
            tool == Tool::Text;

        gtk_widget_set_visible(
            state->paint_options,
            paint);

        gtk_widget_set_visible(
            state->crop_options,
            crop);

        gtk_widget_set_visible(
            state->text_options,
            text);

        gtk_widget_set_visible(
            state->root,
            paint || crop || text);
      };

  result.set_tool(
      Tool::Brush);

  return result;
}

}  // namespace lienzo::gnome
