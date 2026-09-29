#include "ui-gnome/tool_palette.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace lienzo::gnome {

namespace {

struct ToolDefinition {
  Tool tool;
  const char* name;
  const char* icon;
};

constexpr ToolDefinition kTools[] = {
    {Tool::Move, "Mover", "tool-move"},
    {Tool::Marquee, "Marco rectangular", "tool-marquee"},
    {Tool::Lasso, "Lazo", "tool-lasso"},
    {Tool::MagicWand, "Varita mágica", "tool-wand"},
    {Tool::Crop, "Recortar", "tool-crop"},

    {Tool::Brush, "Pincel", "tool-brush"},
    {Tool::Eraser, "Borrador", "tool-eraser"},
    {Tool::Gradient, "Degradado", "tool-gradient"},

    {Tool::Clone, "Clonar", "tool-clone"},
    {Tool::Healing, "Pincel corrector", "tool-healing"},
    {Tool::Smudge, "Dedo", "tool-smudge"},
    {Tool::Dodge, "Sobreexponer", "tool-dodge"},

    {Tool::Pen, "Pluma", "tool-pen"},
    {Tool::PathSelect, "Selección de trazado", "tool-path-select"},
    {Tool::Shape, "Forma", "tool-rect"},
    {Tool::Text, "Texto", "tool-text"},

    {Tool::Eyedropper, "Cuentagotas", "tool-eyedropper"},
    {Tool::Hand, "Mano", "tool-pan"},
    {Tool::Zoom, "Zoom", "tool-zoom"},
};

struct ToolBinding {
  Tool tool{};
  ToolSelectedCallback callback;
};

void tool_toggled(
    GtkToggleButton* button,
    gpointer data) {
  if (!gtk_toggle_button_get_active(button)) {
    return;
  }

  auto* binding =
      static_cast<ToolBinding*>(data);

  if (binding->callback) {
    binding->callback(
        binding->tool);
  }
}

GtkWidget* tool_icon(
    const char* icon_name) {
  const std::filesystem::path path =
      std::filesystem::path(LIENZO_SOURCE_DIR) /
      "src" /
      "ui" /
      "icons" /
      (std::string(icon_name) + ".svg");

  GtkWidget* image =
      gtk_image_new_from_file(
          path.string().c_str());

  gtk_image_set_pixel_size(
      GTK_IMAGE(image),
      20);

  return image;
}

}  // namespace

const char* tool_name(
    Tool tool) {
  for (const auto& definition : kTools) {
    if (definition.tool == tool) {
      return definition.name;
    }
  }

  return "Herramienta";
}

GtkWidget* create_tool_palette(
    Tool initial_tool,
    ToolSelectedCallback callback) {
  GtkWidget* palette =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          2);

  gtk_widget_set_margin_top(
      palette,
      6);

  gtk_widget_set_margin_bottom(
      palette,
      6);

  gtk_widget_set_margin_start(
      palette,
      4);

  gtk_widget_set_margin_end(
      palette,
      4);

  GtkToggleButton* first = nullptr;

  int index = 0;

  for (const auto& definition : kTools) {
    if (
        index == 5 ||
        index == 8 ||
        index == 12 ||
        index == 16) {
      gtk_box_append(
          GTK_BOX(palette),
          gtk_separator_new(
              GTK_ORIENTATION_HORIZONTAL));
    }

    GtkWidget* button =
        gtk_toggle_button_new();

    gtk_widget_add_css_class(
        button,
        "flat");

    gtk_widget_set_tooltip_text(
        button,
        definition.name);

    const bool implemented =
        definition.tool == Tool::Move ||
        definition.tool == Tool::Crop ||
        definition.tool == Tool::Brush ||
        definition.tool == Tool::Eraser ||
        definition.tool == Tool::Gradient ||
        definition.tool == Tool::Smudge ||
        definition.tool == Tool::Shape ||
        definition.tool == Tool::Eyedropper ||
        definition.tool == Tool::Hand ||
        definition.tool == Tool::Zoom;

    gtk_widget_set_sensitive(
        button,
        implemented);

    gtk_button_set_child(
        GTK_BUTTON(button),
        tool_icon(definition.icon));

    gtk_widget_set_size_request(
        button,
        38,
        38);

    if (first == nullptr) {
      first =
          GTK_TOGGLE_BUTTON(button);
    } else {
      gtk_toggle_button_set_group(
          GTK_TOGGLE_BUTTON(button),
          first);
    }

    auto* binding =
        new ToolBinding{
            definition.tool,
            callback};

    g_signal_connect_data(
        button,
        "toggled",
        G_CALLBACK(tool_toggled),
        binding,
        [](gpointer data, GClosure*) {
          delete static_cast<ToolBinding*>(data);
        },
        GConnectFlags(0));

    gtk_box_append(
        GTK_BOX(palette),
        button);

    if (definition.tool == initial_tool) {
      gtk_toggle_button_set_active(
          GTK_TOGGLE_BUTTON(button),
          TRUE);
    }

    ++index;
  }

  return palette;
}

}  // namespace lienzo::gnome
