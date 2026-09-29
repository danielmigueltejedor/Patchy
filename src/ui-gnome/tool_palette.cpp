#include "ui-gnome/tool_palette.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include <vector>

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


struct FlyoutEntry {
  const char* name;
  bool available;
};

std::vector<FlyoutEntry> tool_flyout_entries(
    Tool tool) {
  switch (tool) {
    case Tool::Marquee:
      return {
          {"Marco rectangular", true},
          {"Marco elíptico", false},
      };

    case Tool::Lasso:
      return {
          {"Lazo", true},
          {"Lazo magnético", false},
      };

    case Tool::MagicWand:
      return {
          {"Varita mágica", false},
          {"Selección rápida", false},
      };

    case Tool::Gradient:
      return {
          {"Degradado", true},
          {"Bote de pintura", false},
      };

    case Tool::Clone:
      return {
          {"Tampón de clonar", false},
          {"Tampón de motivo", false},
      };

    case Tool::Healing:
      return {
          {"Pincel corrector", false},
          {"Pincel corrector puntual", false},
          {"Parche", false},
      };

    case Tool::Smudge:
      return {
          {"Dedo", true},
          {"Pincel mezclador", false},
          {"Desenfocar", false},
          {"Enfocar", false},
      };

    case Tool::Dodge:
      return {
          {"Sobreexponer", false},
          {"Subexponer", false},
          {"Esponja", false},
      };

    case Tool::Pen:
      return {
          {"Pluma", false},
          {"Añadir punto de ancla", false},
          {"Eliminar punto de ancla", false},
          {"Convertir punto", false},
      };

    case Tool::PathSelect:
      return {
          {"Selección de trazado", false},
          {"Selección directa", false},
      };

    case Tool::Shape:
      return {
          {"Línea", false},
          {"Rectángulo", true},
          {"Elipse", false},
          {"Polígono", false},
          {"Forma personalizada", false},
      };

    default:
      return {};
  }
}

void present_tool_flyout(
    GtkWidget* owner,
    Tool tool) {
  const auto entries =
      tool_flyout_entries(tool);

  if (entries.empty()) {
    return;
  }

  GtkWidget* popover =
      gtk_popover_new();

  gtk_widget_set_parent(
      popover,
      owner);

  gtk_popover_set_autohide(
      GTK_POPOVER(popover),
      TRUE);

  gtk_popover_set_has_arrow(
      GTK_POPOVER(popover),
      TRUE);

  gtk_popover_set_position(
      GTK_POPOVER(popover),
      GTK_POS_RIGHT);

  GdkRectangle pointing{
      gtk_widget_get_width(owner) - 1,
      0,
      1,
      gtk_widget_get_height(owner)};

  gtk_popover_set_pointing_to(
      GTK_POPOVER(popover),
      &pointing);

  GtkWidget* box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          2);

  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);

  for (const auto& entry : entries) {
    GtkWidget* row =
        gtk_button_new_with_label(
            entry.name);

    gtk_widget_add_css_class(
        row,
        "flat");

    gtk_widget_set_sensitive(
        row,
        entry.available);

    GtkWidget* child =
        gtk_button_get_child(
            GTK_BUTTON(row));

    if (child != nullptr) {
      gtk_widget_set_halign(
          child,
          GTK_ALIGN_START);
    }

    gtk_box_append(
        GTK_BOX(box),
        row);
  }

  gtk_popover_set_child(
      GTK_POPOVER(popover),
      box);

  g_signal_connect_swapped(
      popover,
      "closed",
      G_CALLBACK(gtk_widget_unparent),
      popover);

  gtk_popover_popup(
      GTK_POPOVER(popover));
}

void palette_secondary_pressed(
    GtkGestureClick* gesture,
    int,
    double x,
    double y,
    gpointer data) {
  GtkWidget* palette =
      GTK_WIDGET(data);

  GtkWidget* picked =
      gtk_widget_pick(
          palette,
          x,
          y,
          static_cast<GtkPickFlags>(
              GTK_PICK_INSENSITIVE |
              GTK_PICK_NON_TARGETABLE));

  while (
      picked != nullptr &&
      picked != palette) {
    gpointer raw =
        g_object_get_data(
            G_OBJECT(picked),
            "lienzo-tool-id");

    if (raw != nullptr) {
      gtk_gesture_set_state(
          GTK_GESTURE(gesture),
          GTK_EVENT_SEQUENCE_CLAIMED);

      const auto tool =
          static_cast<Tool>(
              GPOINTER_TO_INT(raw) - 1);

      present_tool_flyout(
          picked,
          tool);

      return;
    }

    picked =
        gtk_widget_get_parent(
            picked);
  }
}

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

  GtkGesture* secondary =
      gtk_gesture_click_new();

  gtk_gesture_single_set_button(
      GTK_GESTURE_SINGLE(secondary),
      GDK_BUTTON_SECONDARY);

  gtk_gesture_single_set_exclusive(
      GTK_GESTURE_SINGLE(secondary),
      TRUE);

  gtk_event_controller_set_propagation_phase(
      GTK_EVENT_CONTROLLER(secondary),
      GTK_PHASE_CAPTURE);

  g_signal_connect(
      secondary,
      "pressed",
      G_CALLBACK(palette_secondary_pressed),
      palette);

  gtk_widget_add_controller(
      palette,
      GTK_EVENT_CONTROLLER(secondary));

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

    g_object_set_data(
        G_OBJECT(button),
        "lienzo-tool-id",
        GINT_TO_POINTER(
            static_cast<int>(
                definition.tool) + 1));

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
