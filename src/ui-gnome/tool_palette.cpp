#include "ui-gnome/tool_palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    {Tool::Rectangle, "Forma", "tool-rect"},
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
  Tool tool;
  const char* name;
  const char* icon;
  bool available;
};

GtkWidget* tool_icon(
    const char* icon_name);

std::vector<FlyoutEntry> tool_flyout_entries(
    Tool tool) {
  switch (tool) {
    case Tool::Marquee:
    case Tool::EllipticalMarquee:
      return {
          {Tool::Marquee, "Marco rectangular", "tool-marquee", true},
          {Tool::EllipticalMarquee, "Marco elíptico", "tool-marquee-ellipse", true},
      };

    case Tool::Lasso:
    case Tool::MagneticLasso:
      return {
          {Tool::Lasso, "Lazo", "tool-lasso", true},
          {Tool::MagneticLasso, "Lazo magnético", "tool-magnetic-lasso", true},
      };

    case Tool::MagicWand:
    case Tool::QuickSelect:
      return {
          {Tool::MagicWand, "Varita mágica", "tool-wand", true},
          {Tool::QuickSelect, "Selección rápida", "tool-quick-select", true},
      };

    case Tool::Brush:
    case Tool::MixerBrush:
      return {
          {Tool::Brush, "Pincel", "tool-brush", true},
          {Tool::MixerBrush, "Pincel mezclador", "tool-mixer-brush", false},
      };

    case Tool::Gradient:
    case Tool::Fill:
      return {
          {Tool::Gradient, "Degradado", "tool-gradient", true},
          {Tool::Fill, "Bote de pintura", "tool-fill", true},
      };

    case Tool::Clone:
    case Tool::PatternStamp:
      return {
          {Tool::Clone, "Tampón de clonar", "tool-clone", true},
          {Tool::PatternStamp, "Tampón de motivo", "tool-pattern-stamp", false},
      };

    case Tool::Healing:
    case Tool::SpotHealing:
    case Tool::PatchTool:
      return {
          {Tool::Healing, "Pincel corrector", "tool-healing", true},
          {Tool::SpotHealing, "Pincel corrector puntual", "tool-spot-healing", false},
          {Tool::PatchTool, "Parche", "tool-patch", false},
      };

    case Tool::Smudge:
    case Tool::BlurBrush:
    case Tool::SharpenBrush:
      return {
          {Tool::Smudge, "Dedo", "tool-smudge", true},
          {Tool::MixerBrush, "Pincel mezclador", "tool-mixer-brush", false},
          {Tool::BlurBrush, "Desenfocar", "tool-blur", true},
          {Tool::SharpenBrush, "Enfocar", "tool-sharpen", true},
      };

    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Sponge:
      return {
          {Tool::Dodge, "Sobreexponer", "tool-dodge", true},
          {Tool::Burn, "Subexponer", "tool-burn", true},
          {Tool::Sponge, "Esponja", "tool-sponge", true},
      };

    case Tool::Pen:
    case Tool::AddAnchor:
    case Tool::DeleteAnchor:
    case Tool::ConvertPoint:
      return {
          {Tool::Pen, "Pluma", "tool-pen", true},
          {Tool::AddAnchor, "Añadir punto de ancla", "tool-add-anchor", false},
          {Tool::DeleteAnchor, "Eliminar punto de ancla", "tool-delete-anchor", false},
          {Tool::ConvertPoint, "Convertir punto", "tool-convert-point", false},
      };

    case Tool::PathSelect:
    case Tool::DirectSelect:
      return {
          {Tool::PathSelect, "Selección de trazado", "tool-path-select", true},
          {Tool::DirectSelect, "Selección directa", "tool-direct-select", false},
      };

    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
    case Tool::Circle:
    case Tool::Polygon:
    case Tool::CustomShape:
      return {
          {Tool::Line, "Línea", "tool-line", true},
          {Tool::Rectangle, "Rectángulo", "tool-rect", true},
          {Tool::Ellipse, "Elipse", "tool-ellipse", true},
          {Tool::Circle, "Círculo", "tool-ellipse", true},
          {Tool::Polygon, "Polígono", "tool-polygon", true},
          {Tool::CustomShape, "Forma personalizada", "tool-custom-shape", false},
      };

    default:
      return {};
  }
}

struct FlyoutChoiceBinding {
  GtkWidget* owner{};
  GtkPopover* popover{};
  ToolBinding* binding{};
  Tool tool{};
  const char* name{};
  const char* icon{};
};

void flyout_choice_clicked(
    GtkButton*,
    gpointer data) {
  auto* choice =
      static_cast<FlyoutChoiceBinding*>(
          data);

  if (
      choice == nullptr ||
      choice->binding == nullptr) {
    return;
  }

  choice->binding->tool =
      choice->tool;

  g_object_set_data(
      G_OBJECT(choice->owner),
      "lienzo-tool-id",
      GINT_TO_POINTER(
          static_cast<int>(
              choice->tool) + 1));

  gtk_button_set_child(
      GTK_BUTTON(choice->owner),
      tool_icon(choice->icon));

  gtk_widget_set_tooltip_text(
      choice->owner,
      choice->name);

  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(
          choice->owner),
      TRUE);

  if (choice->binding->callback) {
    choice->binding->callback(
        choice->tool);
  }

  gtk_popover_popdown(
      choice->popover);
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

    if (entry.available) {
      auto* owner_binding =
          static_cast<ToolBinding*>(
              g_object_get_data(
                  G_OBJECT(owner),
                  "lienzo-tool-binding"));

      auto* choice =
          new FlyoutChoiceBinding{
              owner,
              GTK_POPOVER(popover),
              owner_binding,
              entry.tool,
              entry.name,
              entry.icon};

      g_signal_connect_data(
          row,
          "clicked",
          G_CALLBACK(
              flyout_choice_clicked),
          choice,
          [](gpointer data, GClosure*) {
            delete static_cast<
                FlyoutChoiceBinding*>(
                    data);
          },
          GConnectFlags(0));
    }

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
      18);

  return image;
}

struct PaletteColorState {
  ToolPaletteControls controls;
  GtkColorButton* foreground{};
  GtkColorButton* background{};
  bool synchronizing{false};
};

GdkRGBA to_rgba(
    patchy::EditColor color) {
  return GdkRGBA{
      color.r / 255.0F,
      color.g / 255.0F,
      color.b / 255.0F,
      color.a / 255.0F};
}

patchy::EditColor from_rgba(
    const GdkRGBA& color) {
  const auto component =
      [](float value) {
        return static_cast<std::uint8_t>(
            std::clamp(
                static_cast<int>(
                    std::lround(
                        value * 255.0F)),
                0,
                255));
      };

  return patchy::EditColor{
      component(color.red),
      component(color.green),
      component(color.blue),
      component(color.alpha)};
}

void sync_palette_colors(
    PaletteColorState* state) {
  if (state == nullptr) {
    return;
  }

  state->synchronizing = true;

  if (
      state->controls.foreground_color &&
      state->foreground != nullptr) {
    const GdkRGBA rgba =
        to_rgba(
            state->controls.foreground_color());

    gtk_color_chooser_set_rgba(
        GTK_COLOR_CHOOSER(
            state->foreground),
        &rgba);
  }

  if (
      state->controls.background_color &&
      state->background != nullptr) {
    const GdkRGBA rgba =
        to_rgba(
            state->controls.background_color());

    gtk_color_chooser_set_rgba(
        GTK_COLOR_CHOOSER(
            state->background),
        &rgba);
  }

  state->synchronizing = false;
}

void foreground_color_changed(
    GtkColorButton* button,
    gpointer data) {
  auto* state =
      static_cast<PaletteColorState*>(
          data);

  if (
      state->synchronizing ||
      !state->controls.set_foreground_color) {
    return;
  }

  GdkRGBA rgba{};

  gtk_color_chooser_get_rgba(
      GTK_COLOR_CHOOSER(button),
      &rgba);

  state->controls.set_foreground_color(
      from_rgba(rgba));
}

void background_color_changed(
    GtkColorButton* button,
    gpointer data) {
  auto* state =
      static_cast<PaletteColorState*>(
          data);

  if (
      state->synchronizing ||
      !state->controls.set_background_color) {
    return;
  }

  GdkRGBA rgba{};

  gtk_color_chooser_get_rgba(
      GTK_COLOR_CHOOSER(button),
      &rgba);

  state->controls.set_background_color(
      from_rgba(rgba));
}

void quick_mask_toggled(
    GtkToggleButton* button,
    gpointer data) {
  auto* state =
      static_cast<PaletteColorState*>(
          data);

  if (state->controls.set_quick_mask) {
    state->controls.set_quick_mask(
        gtk_toggle_button_get_active(
            button));
  }
}

void default_colors_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<PaletteColorState*>(
          data);

  if (state->controls.reset_colors) {
    state->controls.reset_colors();
  }

  sync_palette_colors(state);
}

void swap_colors_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<PaletteColorState*>(
          data);

  if (state->controls.swap_colors) {
    state->controls.swap_colors();
  }

  sync_palette_colors(state);
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
    ToolSelectedCallback callback,
    ToolPaletteControls controls) {
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
      2);

  gtk_widget_set_margin_end(
      palette,
      2);

  static bool compact_palette_css_installed = false;

  if (!compact_palette_css_installed) {
    GtkCssProvider* provider =
        gtk_css_provider_new();

    gtk_css_provider_load_from_string(
        provider,
        ".lienzo-color-swatch {"
        "  min-width: 16px;"
        "  min-height: 16px;"
        "  padding: 0;"
        "  margin: 0;"
        "  border: none;"
        "  box-shadow: none;"
        "  background: transparent;"
        "  background-image: none;"
        "  border-radius: 4px;"
        "}"
        ".lienzo-color-swatch > button {"
        "  min-width: 16px;"
        "  min-height: 16px;"
        "  padding: 0;"
        "  margin: 0;"
        "  border: none;"
        "  box-shadow: none;"
        "  background: transparent;"
        "  background-image: none;"
        "  border-radius: 4px;"
        "}"
        ".lienzo-color-mini {"
        "  min-width: 18px;"
        "  min-height: 18px;"
        "  padding: 0;"
        "  margin: 0;"
        "  border-radius: 5px;"
        "  font-size: 9px;"
        "}");

    gtk_style_context_add_provider_for_display(
        gtk_widget_get_display(palette),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(provider);

    compact_palette_css_installed = true;
  }

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
        definition.tool == Tool::Marquee ||
        definition.tool == Tool::Lasso ||
        definition.tool == Tool::MagicWand ||
        definition.tool == Tool::Crop ||
        definition.tool == Tool::Brush ||
        definition.tool == Tool::Eraser ||
        definition.tool == Tool::Gradient ||
        definition.tool == Tool::Smudge ||
        definition.tool == Tool::Rectangle ||
        definition.tool == Tool::Text ||
        definition.tool == Tool::Eyedropper ||
        definition.tool == Tool::Hand ||
        definition.tool == Tool::Zoom ||
        definition.tool == Tool::Clone ||
        definition.tool == Tool::Healing ||
        definition.tool == Tool::Dodge ||
        definition.tool == Tool::Pen ||
        definition.tool == Tool::PathSelect;

    gtk_widget_set_sensitive(
        button,
        implemented);

    gtk_button_set_child(
        GTK_BUTTON(button),
        tool_icon(definition.icon));

    gtk_widget_set_size_request(
        button,
        34,
        34);

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

    g_object_set_data(
        G_OBJECT(button),
        "lienzo-tool-binding",
        binding);

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

  GtkWidget* spacer =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  gtk_widget_set_vexpand(
      spacer,
      TRUE);

  gtk_box_append(
      GTK_BOX(palette),
      spacer);

  gtk_box_append(
      GTK_BOX(palette),
      gtk_separator_new(
          GTK_ORIENTATION_HORIZONTAL));

  auto* color_state =
      new PaletteColorState{
          controls,
          nullptr,
          nullptr,
          false};

  g_object_set_data_full(
      G_OBJECT(palette),
      "lienzo-palette-color-state",
      color_state,
      [](gpointer data) {
        delete static_cast<
            PaletteColorState*>(data);
      });

  // Colores frontal/fondo: dos swatches superpuestos pero
  // desplazados para que ambos sean siempre visibles.
  GtkWidget* swatches =
      gtk_fixed_new();

  gtk_widget_set_size_request(
      swatches,
      34,
      32);

  gtk_widget_set_halign(
      swatches,
      GTK_ALIGN_CENTER);

  gtk_widget_set_margin_top(
      swatches,
      5);

  GtkWidget* background =
      gtk_color_button_new();

  color_state->background =
      GTK_COLOR_BUTTON(background);

  gtk_widget_set_size_request(
      background,
      16,
      16);

  gtk_widget_add_css_class(
      background,
      "lienzo-color-swatch");

  gtk_widget_add_css_class(
      background,
      "flat");

  gtk_widget_add_css_class(
      background,
      "lienzo-color-flat");

  gtk_widget_set_tooltip_text(
      background,
      "Color de fondo");

  gtk_fixed_put(
      GTK_FIXED(swatches),
      background,
      12.0,
      10.0);

  GtkWidget* foreground =
      gtk_color_button_new();

  color_state->foreground =
      GTK_COLOR_BUTTON(foreground);

  gtk_widget_set_size_request(
      foreground,
      16,
      16);

  gtk_widget_add_css_class(
      foreground,
      "lienzo-color-swatch");

  gtk_widget_add_css_class(
      foreground,
      "flat");

  gtk_widget_add_css_class(
      foreground,
      "lienzo-color-flat");

  gtk_widget_set_tooltip_text(
      foreground,
      "Color frontal");

  gtk_fixed_put(
      GTK_FIXED(swatches),
      foreground,
      0.0,
      0.0);

  gtk_box_append(
      GTK_BOX(palette),
      swatches);

  g_signal_connect(
      foreground,
      "color-set",
      G_CALLBACK(
          foreground_color_changed),
      color_state);

  g_signal_connect(
      background,
      "color-set",
      G_CALLBACK(
          background_color_changed),
      color_state);

  sync_palette_colors(
      color_state);

  GtkWidget* color_actions =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          2);

  gtk_widget_set_halign(
      color_actions,
      GTK_ALIGN_CENTER);

  GtkWidget* defaults =
      gtk_button_new();

  GtkWidget* defaults_icon =
      tool_icon(
          "default-colors");

  gtk_image_set_pixel_size(
      GTK_IMAGE(defaults_icon),
      15);

  gtk_button_set_child(
      GTK_BUTTON(defaults),
      defaults_icon);

  gtk_widget_add_css_class(
      defaults,
      "flat");

  gtk_widget_add_css_class(
      defaults,
      "lienzo-color-mini");

  gtk_widget_set_tooltip_text(
      defaults,
      "Colores por defecto (D)");

  gtk_widget_set_size_request(
      defaults,
      18,
      18);

  GtkWidget* swap =
      gtk_button_new();

  GtkWidget* swap_icon =
      tool_icon(
          "swap-colors");

  gtk_image_set_pixel_size(
      GTK_IMAGE(swap_icon),
      15);

  gtk_button_set_child(
      GTK_BUTTON(swap),
      swap_icon);

  gtk_widget_add_css_class(
      swap,
      "flat");

  gtk_widget_add_css_class(
      swap,
      "lienzo-color-mini");

  gtk_widget_set_tooltip_text(
      swap,
      "Intercambiar colores (X)");

  gtk_widget_set_size_request(
      swap,
      18,
      18);

  gtk_box_append(
      GTK_BOX(color_actions),
      defaults);

  gtk_box_append(
      GTK_BOX(color_actions),
      swap);

  gtk_box_append(
      GTK_BOX(palette),
      color_actions);

  g_signal_connect(
      defaults,
      "clicked",
      G_CALLBACK(
          default_colors_clicked),
      color_state);

  g_signal_connect(
      swap,
      "clicked",
      G_CALLBACK(
          swap_colors_clicked),
      color_state);

  gtk_widget_set_margin_bottom(
      color_actions,
      2);

  GtkWidget* quick_mask =
      gtk_toggle_button_new();

  GtkWidget* quick_mask_icon =
      tool_icon(
          "mask");

  gtk_image_set_pixel_size(
      GTK_IMAGE(quick_mask_icon),
      16);

  gtk_button_set_child(
      GTK_BUTTON(quick_mask),
      quick_mask_icon);

  gtk_widget_add_css_class(
      quick_mask,
      "flat");

  gtk_widget_add_css_class(
      quick_mask,
      "lienzo-color-mini");

  gtk_widget_set_size_request(
      quick_mask,
      18,
      18);

  gtk_widget_set_halign(
      quick_mask,
      GTK_ALIGN_CENTER);

  gtk_widget_set_tooltip_text(
      quick_mask,
      "Editar en modo Máscara rápida");

  if (controls.quick_mask_enabled) {
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(quick_mask),
        controls.quick_mask_enabled());
  }

  g_signal_connect(
      quick_mask,
      "toggled",
      G_CALLBACK(quick_mask_toggled),
      color_state);

  gtk_box_append(
      GTK_BOX(palette),
      quick_mask);

  return palette;
}

}  // namespace lienzo::gnome
