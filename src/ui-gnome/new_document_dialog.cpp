#include "ui-gnome/new_document_dialog.hpp"

#include <array>
#include <utility>

namespace lienzo::gnome {

namespace {

struct Preset {
  const char* name;
  int width;
  int height;
  double ppi;
};

constexpr std::array<Preset, 8> kScreenPresets{{
    {"Predeterminado", 1024, 768, 72.0},
    {"720p", 1280, 720, 72.0},
    {"1080p", 1920, 1080, 72.0},
    {"4K", 3840, 2160, 72.0},
    {"Cuadrado", 2048, 2048, 72.0},
    {"Publicación social", 1080, 1080, 72.0},
    {"Historia social", 1080, 1920, 72.0},
    {"Foto 3:2", 3000, 2000, 72.0},
}};

constexpr std::array<Preset, 7> kPrintPresets{{
    {"A5", 1748, 2480, 300.0},
    {"A4", 2480, 3508, 300.0},
    {"A3", 3508, 4961, 300.0},
    {"US Letter", 2550, 3300, 300.0},
    {"US Legal", 2550, 4200, 300.0},
    {"5 × 7 in", 1500, 2100, 300.0},
    {"8 × 10 in", 2400, 3000, 300.0},
}};

struct DialogState {
  AdwDialog* dialog{};
  GtkFlowBox* presets{};
  AdwSpinRow* width{};
  AdwSpinRow* height{};
  AdwSpinRow* resolution{};
  AdwComboRow* background{};
  NewDocumentCallback callback;
};

struct PresetBinding {
  DialogState* state{};
  const Preset* preset{};
};

void apply_preset(
    GtkButton*,
    gpointer data) {
  auto* binding =
      static_cast<PresetBinding*>(data);

  const Preset& preset =
      *binding->preset;

  adw_spin_row_set_value(
      binding->state->width,
      preset.width);

  adw_spin_row_set_value(
      binding->state->height,
      preset.height);

  adw_spin_row_set_value(
      binding->state->resolution,
      preset.ppi);
}

struct PreviewData {
  int width{};
  int height{};
};

void draw_preset_preview(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer data) {
  const auto* preview =
      static_cast<PreviewData*>(data);

  const double ratio =
      static_cast<double>(preview->width) /
      static_cast<double>(preview->height);

  double page_width =
      width - 12.0;

  double page_height =
      page_width / ratio;

  if (page_height > height - 8.0) {
    page_height =
        height - 8.0;

    page_width =
        page_height * ratio;
  }

  const double x =
      (width - page_width) / 2.0;

  const double y =
      (height - page_height) / 2.0;

  cairo_set_source_rgba(
      cr,
      0.15,
      0.15,
      0.17,
      0.18);

  cairo_rectangle(
      cr,
      x + 2,
      y + 3,
      page_width,
      page_height);

  cairo_fill(cr);

  cairo_set_source_rgb(
      cr,
      0.96,
      0.96,
      0.97);

  cairo_rectangle(
      cr,
      x,
      y,
      page_width,
      page_height);

  cairo_fill(cr);

  cairo_pattern_t* gradient =
      cairo_pattern_create_linear(
          x,
          y,
          x + page_width,
          y + page_height);

  cairo_pattern_add_color_stop_rgb(
      gradient,
      0.0,
      0.20,
      0.52,
      0.89);

  cairo_pattern_add_color_stop_rgb(
      gradient,
      1.0,
      0.55,
      0.28,
      0.78);

  cairo_rectangle(
      cr,
      x + page_width * 0.08,
      y + page_height * 0.10,
      page_width * 0.84,
      page_height * 0.55);

  cairo_set_source(
      cr,
      gradient);

  cairo_fill(cr);

  cairo_pattern_destroy(
      gradient);

  cairo_set_source_rgba(
      cr,
      0.12,
      0.12,
      0.14,
      0.72);

  cairo_rectangle(
      cr,
      x + page_width * 0.08,
      y + page_height * 0.73,
      page_width * 0.54,
      std::max(
          2.0,
          page_height * 0.07));

  cairo_fill(cr);

  cairo_set_source_rgba(
      cr,
      0.12,
      0.12,
      0.14,
      0.35);

  cairo_rectangle(
      cr,
      x + page_width * 0.08,
      y + page_height * 0.84,
      page_width * 0.75,
      std::max(
          1.0,
          page_height * 0.035));

  cairo_fill(cr);
}

GtkWidget* create_preset_preview(
    const Preset* preset) {
  GtkWidget* preview =
      gtk_drawing_area_new();

  gtk_widget_set_size_request(
      preview,
      108,
      68);

  auto* data =
      new PreviewData{
          preset->width,
          preset->height};

  gtk_drawing_area_set_draw_func(
      GTK_DRAWING_AREA(preview),
      draw_preset_preview,
      data,
      [](gpointer pointer) {
        delete static_cast<PreviewData*>(
            pointer);
      });

  return preview;
}

GtkWidget* create_preset_card(
    DialogState* state,
    const Preset* preset) {
  GtkWidget* button =
      gtk_button_new();

  gtk_widget_add_css_class(
      button,
      "card");

  GtkWidget* box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          4);

  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 14);
  gtk_widget_set_margin_end(box, 14);

  GtkWidget* preview =
      create_preset_preview(
          preset);

  GtkWidget* title =
      gtk_label_new(preset->name);

  gtk_widget_add_css_class(
      title,
      "heading");

  char details[96];

  g_snprintf(
      details,
      sizeof(details),
      "%d × %d px\n%.0f ppp",
      preset->width,
      preset->height,
      preset->ppi);

  GtkWidget* subtitle =
      gtk_label_new(details);

  gtk_widget_add_css_class(
      subtitle,
      "dim-label");

  gtk_label_set_justify(
      GTK_LABEL(subtitle),
      GTK_JUSTIFY_CENTER);

  gtk_box_append(
      GTK_BOX(box),
      preview);

  gtk_box_append(
      GTK_BOX(box),
      title);

  gtk_box_append(
      GTK_BOX(box),
      subtitle);

  gtk_button_set_child(
      GTK_BUTTON(button),
      box);

  auto* binding =
      new PresetBinding{
          state,
          preset};

  g_signal_connect_data(
      button,
      "clicked",
      G_CALLBACK(apply_preset),
      binding,
      [](gpointer data, GClosure*) {
        delete static_cast<PresetBinding*>(data);
      },
      GConnectFlags(0));

  return button;
}

template <std::size_t N>
void populate_presets(
    DialogState* state,
    const std::array<Preset, N>& presets) {
  gtk_flow_box_remove_all(
      state->presets);

  for (const auto& preset : presets) {
    gtk_flow_box_append(
        state->presets,
        create_preset_card(
            state,
            &preset));
  }
}

void show_screen_presets(
    GtkToggleButton* button,
    gpointer data) {
  if (!gtk_toggle_button_get_active(button)) {
    return;
  }

  populate_presets(
      static_cast<DialogState*>(data),
      kScreenPresets);
}

void show_print_presets(
    GtkToggleButton* button,
    gpointer data) {
  if (!gtk_toggle_button_get_active(button)) {
    return;
  }

  populate_presets(
      static_cast<DialogState*>(data),
      kPrintPresets);
}

void swap_dimensions(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<DialogState*>(data);

  const double width =
      adw_spin_row_get_value(state->width);

  const double height =
      adw_spin_row_get_value(state->height);

  adw_spin_row_set_value(
      state->width,
      height);

  adw_spin_row_set_value(
      state->height,
      width);
}

void cancel_dialog(
    GtkButton*,
    gpointer data) {
  adw_dialog_close(
      static_cast<AdwDialog*>(data));
}

void create_document(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<DialogState*>(data);

  NewDocumentSettings settings;

  settings.width =
      static_cast<std::int32_t>(
          adw_spin_row_get_value(
              state->width));

  settings.height =
      static_cast<std::int32_t>(
          adw_spin_row_get_value(
              state->height));

  settings.resolution_ppi =
      adw_spin_row_get_value(
          state->resolution);

  switch (
      adw_combo_row_get_selected(
          state->background)) {
    case 1:
      settings.background =
          NewDocumentBackground::Black;
      break;

    case 2:
      settings.background =
          NewDocumentBackground::Transparent;
      break;

    default:
      settings.background =
          NewDocumentBackground::White;
      break;
  }

  if (state->callback) {
    state->callback(settings);
  }

  adw_dialog_close(
      state->dialog);
}

}  // namespace

void present_new_document_dialog(
    GtkWidget* parent,
    NewDocumentCallback callback) {
  AdwDialog* dialog =
      ADW_DIALOG(adw_dialog_new());

  adw_dialog_set_title(
      dialog,
      "Nuevo documento");

  adw_dialog_set_content_width(
      dialog,
      760);

  adw_dialog_set_content_height(
      dialog,
      560);

  auto* state =
      new DialogState;

  state->dialog = dialog;
  state->callback =
      std::move(callback);

  g_object_set_data_full(
      G_OBJECT(dialog),
      "lienzo-new-document-state",
      state,
      [](gpointer data) {
        delete static_cast<DialogState*>(data);
      });

  GtkWidget* toolbar =
      adw_toolbar_view_new();

  GtkWidget* header =
      adw_header_bar_new();

  GtkWidget* cancel =
      gtk_button_new_with_label(
          "Cancelar");

  adw_header_bar_pack_start(
      ADW_HEADER_BAR(header),
      cancel);

  GtkWidget* create =
      gtk_button_new_with_label(
          "Crear");

  gtk_widget_add_css_class(
      create,
      "suggested-action");

  adw_header_bar_pack_end(
      ADW_HEADER_BAR(header),
      create);

  adw_toolbar_view_add_top_bar(
      ADW_TOOLBAR_VIEW(toolbar),
      header);

  GtkWidget* body =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          24);

  gtk_widget_set_margin_top(body, 18);
  gtk_widget_set_margin_bottom(body, 18);
  gtk_widget_set_margin_start(body, 18);
  gtk_widget_set_margin_end(body, 18);

  GtkWidget* left =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          12);

  gtk_widget_set_hexpand(
      left,
      TRUE);

  GtkWidget* categories =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          6);

  GtkWidget* screen =
      gtk_toggle_button_new_with_label(
          "Pantalla");

  GtkWidget* print =
      gtk_toggle_button_new_with_label(
          "Impresión");

  gtk_toggle_button_set_group(
      GTK_TOGGLE_BUTTON(print),
      GTK_TOGGLE_BUTTON(screen));

  gtk_widget_add_css_class(
      screen,
      "pill");

  gtk_widget_add_css_class(
      print,
      "pill");

  gtk_box_append(
      GTK_BOX(categories),
      screen);

  gtk_box_append(
      GTK_BOX(categories),
      print);

  gtk_box_append(
      GTK_BOX(left),
      categories);

  GtkWidget* scroller =
      gtk_scrolled_window_new();

  gtk_widget_set_hexpand(
      scroller,
      TRUE);

  gtk_widget_set_vexpand(
      scroller,
      TRUE);

  GtkWidget* flow =
      gtk_flow_box_new();

  state->presets =
      GTK_FLOW_BOX(flow);

  gtk_flow_box_set_selection_mode(
      state->presets,
      GTK_SELECTION_NONE);

  gtk_flow_box_set_max_children_per_line(
      state->presets,
      3);

  gtk_flow_box_set_min_children_per_line(
      state->presets,
      1);

  gtk_flow_box_set_column_spacing(
      state->presets,
      10);

  gtk_flow_box_set_row_spacing(
      state->presets,
      10);

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(scroller),
      flow);

  gtk_box_append(
      GTK_BOX(left),
      scroller);

  GtkWidget* details =
      adw_preferences_group_new();

  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(details),
      "Detalles");

  GtkWidget* width =
      adw_spin_row_new_with_range(
          1,
          100000,
          1);

  state->width =
      ADW_SPIN_ROW(width);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(width),
      "Anchura");

  GtkWidget* height =
      adw_spin_row_new_with_range(
          1,
          100000,
          1);

  state->height =
      ADW_SPIN_ROW(height);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(height),
      "Altura");

  GtkWidget* resolution =
      adw_spin_row_new_with_range(
          1,
          9999,
          1);

  state->resolution =
      ADW_SPIN_ROW(resolution);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(resolution),
      "Resolución (ppp)");

  GtkWidget* background =
      adw_combo_row_new();

  state->background =
      ADW_COMBO_ROW(background);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(background),
      "Fondo");

  const char* backgrounds[] = {
      "Blanco",
      "Negro",
      "Transparente",
      nullptr};

  GtkStringList* background_model =
      gtk_string_list_new(backgrounds);

  adw_combo_row_set_model(
      state->background,
      G_LIST_MODEL(background_model));

  g_object_unref(
      background_model);

  GtkWidget* orientation =
      adw_action_row_new();

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(orientation),
      "Orientación");

  GtkWidget* swap =
      gtk_button_new_from_icon_name(
          "object-rotate-right-symbolic");

  gtk_widget_set_tooltip_text(
      swap,
      "Intercambiar anchura y altura");

  gtk_widget_add_css_class(
      swap,
      "flat");

  adw_action_row_add_suffix(
      ADW_ACTION_ROW(orientation),
      swap);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(details),
      width);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(details),
      height);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(details),
      resolution);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(details),
      orientation);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(details),
      background);

  gtk_widget_set_size_request(
      details,
      260,
      -1);

  gtk_box_append(
      GTK_BOX(body),
      left);

  gtk_box_append(
      GTK_BOX(body),
      details);

  adw_toolbar_view_set_content(
      ADW_TOOLBAR_VIEW(toolbar),
      body);

  adw_dialog_set_child(
      dialog,
      toolbar);

  adw_dialog_set_default_widget(
      dialog,
      create);

  g_signal_connect(
      cancel,
      "clicked",
      G_CALLBACK(cancel_dialog),
      dialog);

  g_signal_connect(
      create,
      "clicked",
      G_CALLBACK(create_document),
      state);

  g_signal_connect(
      swap,
      "clicked",
      G_CALLBACK(swap_dimensions),
      state);

  g_signal_connect(
      screen,
      "toggled",
      G_CALLBACK(show_screen_presets),
      state);

  g_signal_connect(
      print,
      "toggled",
      G_CALLBACK(show_print_presets),
      state);

  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(screen),
      TRUE);

  adw_spin_row_set_value(
      state->width,
      1024);

  adw_spin_row_set_value(
      state->height,
      768);

  adw_spin_row_set_value(
      state->resolution,
      72);

  adw_dialog_present(
      dialog,
      parent);
}

}  // namespace lienzo::gnome
