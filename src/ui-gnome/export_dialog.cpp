#include "ui-gnome/export_dialog.hpp"

#include <adwaita.h>

#include <vector>

namespace lienzo::gnome {

namespace {

struct FormatRow {
  const char* label;
  ExportKind kind;
  const char* extension;
  bool layered;
  bool quality;
  bool lossless;
};

const FormatRow kFormats[] = {
    {"PNG", ExportKind::Png, ".png", false, false, false},
    {"JPEG", ExportKind::Jpeg, ".jpg", false, true, false},
    {"WebP", ExportKind::Webp, ".webp", false, true, true},
    {"BMP", ExportKind::Bmp, ".bmp", false, false, false},
    {"PCX", ExportKind::Pcx, ".pcx", false, false, false},
    {"PSD (con capas)", ExportKind::Psd, ".psd", true, false, false},
    {"PSB (con capas)", ExportKind::Psb, ".psb", true, false, false},
    {"PXD de Pixelmator (con capas)", ExportKind::Pxd, ".pxd", true, false, false},
};

struct DialogState {
  AdwDialog* dialog{};
  GtkDropDown* format{};
  GtkWidget* quality_box{};
  GtkScale* quality{};
  GtkWidget* lossless_row{};
  GtkSwitch* lossless{};
  std::string stem;
  ExportChosen chosen;
};

const FormatRow& selected_format(const DialogState* state) {
  const auto index =
      gtk_drop_down_get_selected(
          state->format);

  if (index >= G_N_ELEMENTS(kFormats)) {
    return kFormats[0];
  }

  return kFormats[index];
}

void refresh_options(DialogState* state) {
  const auto& format =
      selected_format(state);

  gtk_widget_set_visible(
      state->quality_box,
      format.quality);

  gtk_widget_set_visible(
      state->lossless_row,
      format.lossless);
}

void on_format_changed(
    GObject*,
    GParamSpec*,
    gpointer data) {
  refresh_options(
      static_cast<DialogState*>(data));
}

void on_cancel(GtkButton*, gpointer data) {
  auto* state =
      static_cast<DialogState*>(data);

  adw_dialog_close(state->dialog);
}

void on_export(GtkButton*, gpointer data) {
  auto* state =
      static_cast<DialogState*>(data);

  const auto& format =
      selected_format(state);

  ExportSettings settings;

  settings.kind = format.kind;
  settings.extension = format.extension;
  settings.layered = format.layered;
  settings.quality = static_cast<int>(
      gtk_range_get_value(
          GTK_RANGE(state->quality)));
  settings.lossless =
      gtk_switch_get_active(state->lossless);

  auto chosen =
      std::move(state->chosen);

  adw_dialog_close(state->dialog);

  if (chosen) {
    chosen(std::move(settings));
  }
}

}  // namespace

void present_export_dialog(
    GtkWidget* parent,
    const std::string& suggested_stem,
    ExportChosen chosen) {
  AdwDialog* dialog =
      ADW_DIALOG(adw_dialog_new());

  adw_dialog_set_title(
      dialog,
      "Exportar como");

  adw_dialog_set_content_width(
      dialog,
      460);

  adw_dialog_set_content_height(
      dialog,
      360);

  auto* state =
      new DialogState;

  state->dialog = dialog;
  state->stem = suggested_stem;
  state->chosen = std::move(chosen);

  g_object_set_data_full(
      G_OBJECT(dialog),
      "lienzo-export-state",
      state,
      [](gpointer data) {
        delete static_cast<DialogState*>(data);
      });

  GtkWidget* toolbar =
      adw_toolbar_view_new();

  GtkWidget* header =
      adw_header_bar_new();

  GtkWidget* cancel =
      gtk_button_new_with_label("Cancelar");

  GtkWidget* accept =
      gtk_button_new_with_label("Exportar");

  gtk_widget_add_css_class(
      accept,
      "suggested-action");

  adw_header_bar_pack_start(
      ADW_HEADER_BAR(header),
      cancel);

  adw_header_bar_pack_end(
      ADW_HEADER_BAR(header),
      accept);

  adw_toolbar_view_add_top_bar(
      ADW_TOOLBAR_VIEW(toolbar),
      header);

  GtkWidget* body =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          16);

  gtk_widget_set_margin_top(body, 16);
  gtk_widget_set_margin_bottom(body, 16);
  gtk_widget_set_margin_start(body, 16);
  gtk_widget_set_margin_end(body, 16);

  const char* labels[] = {
      kFormats[0].label,
      kFormats[1].label,
      kFormats[2].label,
      kFormats[3].label,
      kFormats[4].label,
      kFormats[5].label,
      kFormats[6].label,
      kFormats[7].label,
      nullptr};

  GtkWidget* format =
      gtk_drop_down_new_from_strings(labels);

  state->format =
      GTK_DROP_DOWN(format);

  gtk_drop_down_set_selected(
      state->format,
      0);

  GtkWidget* format_label =
      gtk_label_new("Formato");

  gtk_widget_set_halign(
      format_label,
      GTK_ALIGN_START);

  gtk_box_append(
      GTK_BOX(body),
      format_label);

  gtk_box_append(
      GTK_BOX(body),
      format);

  GtkWidget* note =
      gtk_label_new(
          "PNG, JPEG, WebP, BMP y PCX guardan la imagen plana. PSD, PSB y PXD conservan las capas.");

  gtk_label_set_wrap(
      GTK_LABEL(note),
      TRUE);

  gtk_widget_set_halign(
      note,
      GTK_ALIGN_START);

  gtk_widget_add_css_class(
      note,
      "dim-label");

  gtk_box_append(
      GTK_BOX(body),
      note);

  GtkWidget* quality_box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          6);

  state->quality_box = quality_box;

  gtk_box_append(
      GTK_BOX(quality_box),
      gtk_label_new("Calidad"));

  GtkWidget* quality =
      gtk_scale_new_with_range(
          GTK_ORIENTATION_HORIZONTAL,
          1,
          100,
          1);

  gtk_range_set_value(
      GTK_RANGE(quality),
      90);

  gtk_scale_set_draw_value(
      GTK_SCALE(quality),
      TRUE);

  state->quality =
      GTK_SCALE(quality);

  gtk_box_append(
      GTK_BOX(quality_box),
      quality);

  gtk_box_append(
      GTK_BOX(body),
      quality_box);

  GtkWidget* lossless_row =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          12);

  state->lossless_row = lossless_row;

  gtk_box_append(
      GTK_BOX(lossless_row),
      gtk_label_new("Sin pérdida"));

  GtkWidget* lossless =
      gtk_switch_new();

  gtk_widget_set_halign(
      lossless,
      GTK_ALIGN_END);

  gtk_widget_set_hexpand(lossless, TRUE);

  state->lossless =
      GTK_SWITCH(lossless);

  gtk_box_append(
      GTK_BOX(lossless_row),
      lossless);

  gtk_box_append(
      GTK_BOX(body),
      lossless_row);

  refresh_options(state);

  g_signal_connect(
      format,
      "notify::selected",
      G_CALLBACK(on_format_changed),
      state);

  g_signal_connect(
      cancel,
      "clicked",
      G_CALLBACK(on_cancel),
      state);

  g_signal_connect(
      accept,
      "clicked",
      G_CALLBACK(on_export),
      state);

  adw_toolbar_view_set_content(
      ADW_TOOLBAR_VIEW(toolbar),
      body);

  adw_dialog_set_child(
      dialog,
      toolbar);

  adw_dialog_present(
      dialog,
      parent);
}

}  // namespace lienzo::gnome
