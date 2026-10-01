#include "ui-gnome/layer_style_dialog.hpp"

#include <adwaita.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace lienzo::gnome {
namespace {

const char* const blend_mode_names[] = {
    "Atravesar",
    "Normal",
    "Multiplicar",
    "Trama",
    "Superponer",
    "Oscurecer",
    "Aclarar",
    "Sobreexponer color",
    "Subexponer color",
    "Luz fuerte",
    "Luz suave",
    "Diferencia",
    "Subexposición lineal",
    "Luz focal",
    "Saturación",
    "Luminosidad",
    "Exclusión",
    "Tono",
    "Color",
    "Sobreexposición lineal",
    "Restar",
    "Dividir",
    "Luz intensa",
    "Luz lineal",
    "Mezcla definida",
    "Color más oscuro",
    "Color más claro",
    "Disolver",
    nullptr};

const char* const bevel_styles[] = {
    "Bisel interior",
    "Bisel exterior",
    "Relieve",
    "Relieve acolchado",
    "Relieve del trazo",
    nullptr};

const char* const bevel_techniques[] = {
    "Suavizar",
    "Cincel duro",
    "Cincel suave",
    nullptr};

const char* const glow_techniques[] = {
    "Más suave",
    "Preciso",
    nullptr};

const char* const glow_sources[] = {
    "Centro",
    "Borde",
    nullptr};

const char* const stroke_positions[] = {
    "Exterior",
    "Interior",
    "Centro",
    nullptr};

const char* const gradient_types[] = {
    "Lineal",
    "Radial",
    "Angular",
    "Reflejado",
    "Diamante",
    "Contorno",
    nullptr};

struct DialogState {
  DialogState() = default;
  ~DialogState() {
    g_weak_ref_clear(&parent);
  }

  DialogState(const DialogState&) = delete;
  DialogState& operator=(const DialogState&) = delete;

  GWeakRef parent{};
  patchy::Document* document{};
  patchy::LayerId id{};
  CanvasView canvas;
  std::function<void()> after_apply;
  std::vector<std::string> pattern_ids;

  GtkWidget* name{};
  GtkWidget* opacity{};
  GtkWidget* fill{};
  GtkWidget* blend{};
  GtkWidget* visible{};
  GtkWidget* clipped{};
  GtkWidget* locked{};
  GtkWidget* effects_visible{};
  GtkWidget* mask_hides{};
  GtkWidget* blend_interior{};
  GtkWidget* blend_clipped{};
  GtkWidget* blend_if_this_black{};
  GtkWidget* blend_if_this_white{};
  GtkWidget* blend_if_under_black{};
  GtkWidget* blend_if_under_white{};

  GtkWidget* drop_on{};
  GtkWidget* drop_blend{};
  GtkWidget* drop_color{};
  GtkWidget* drop_opacity{};
  GtkWidget* drop_angle{};
  GtkWidget* drop_distance{};
  GtkWidget* drop_spread{};
  GtkWidget* drop_size{};
  GtkWidget* drop_global{};
  GtkWidget* drop_conceals{};

  GtkWidget* inner_shadow_on{};
  GtkWidget* inner_shadow_blend{};
  GtkWidget* inner_shadow_color{};
  GtkWidget* inner_shadow_opacity{};
  GtkWidget* inner_shadow_angle{};
  GtkWidget* inner_shadow_distance{};
  GtkWidget* inner_shadow_choke{};
  GtkWidget* inner_shadow_size{};
  GtkWidget* inner_shadow_global{};

  GtkWidget* outer_glow_on{};
  GtkWidget* outer_glow_blend{};
  GtkWidget* outer_glow_color{};
  GtkWidget* outer_glow_opacity{};
  GtkWidget* outer_glow_spread{};
  GtkWidget* outer_glow_size{};
  GtkWidget* outer_glow_range{};
  GtkWidget* outer_glow_technique{};

  GtkWidget* inner_glow_on{};
  GtkWidget* inner_glow_blend{};
  GtkWidget* inner_glow_color{};
  GtkWidget* inner_glow_opacity{};
  GtkWidget* inner_glow_choke{};
  GtkWidget* inner_glow_size{};
  GtkWidget* inner_glow_range{};
  GtkWidget* inner_glow_technique{};
  GtkWidget* inner_glow_source{};

  GtkWidget* bevel_on{};
  GtkWidget* bevel_style{};
  GtkWidget* bevel_technique{};
  GtkWidget* bevel_depth{};
  GtkWidget* bevel_size{};
  GtkWidget* bevel_soften{};
  GtkWidget* bevel_angle{};
  GtkWidget* bevel_altitude{};
  GtkWidget* bevel_up{};
  GtkWidget* bevel_global{};
  GtkWidget* bevel_highlight_blend{};
  GtkWidget* bevel_highlight_color{};
  GtkWidget* bevel_highlight_opacity{};
  GtkWidget* bevel_shadow_blend{};
  GtkWidget* bevel_shadow_color{};
  GtkWidget* bevel_shadow_opacity{};

  GtkWidget* satin_on{};
  GtkWidget* satin_blend{};
  GtkWidget* satin_color{};
  GtkWidget* satin_opacity{};
  GtkWidget* satin_angle{};
  GtkWidget* satin_distance{};
  GtkWidget* satin_size{};
  GtkWidget* satin_invert{};

  GtkWidget* color_on{};
  GtkWidget* color_blend{};
  GtkWidget* color_color{};
  GtkWidget* color_opacity{};

  GtkWidget* gradient_on{};
  GtkWidget* gradient_blend{};
  GtkWidget* gradient_opacity{};
  GtkWidget* gradient_angle{};
  GtkWidget* gradient_scale{};
  GtkWidget* gradient_type{};
  GtkWidget* gradient_reverse{};
  GtkWidget* gradient_start{};
  GtkWidget* gradient_end{};

  GtkWidget* pattern_on{};
  GtkWidget* pattern_blend{};
  GtkWidget* pattern_opacity{};
  GtkWidget* pattern_scale{};
  GtkWidget* pattern_angle{};
  GtkWidget* pattern_choice{};

  GtkWidget* stroke_on{};
  GtkWidget* stroke_blend{};
  GtkWidget* stroke_color{};
  GtkWidget* stroke_opacity{};
  GtkWidget* stroke_size{};
  GtkWidget* stroke_position{};
  GtkWidget* stroke_overprint{};
};

guint blend_index(patchy::BlendMode mode) {
  const auto index = static_cast<guint>(mode);

  if (index >= static_cast<guint>(patchy::BlendMode::Dissolve) + 1U) {
    return static_cast<guint>(patchy::BlendMode::Normal);
  }

  return index;
}

int choice_index(GtkWidget* button) {
  const gpointer stored =
      g_object_get_data(
          G_OBJECT(button),
          "lienzo-choice-index");

  if (stored == nullptr) {
    return 0;
  }

  return std::max(0, GPOINTER_TO_INT(stored) - 1);
}

patchy::BlendMode blend_from(GtkWidget* button) {
  const int index = choice_index(button);

  if (
      index < 0 ||
      index > static_cast<int>(patchy::BlendMode::Dissolve)) {
    return patchy::BlendMode::Normal;
  }

  return static_cast<patchy::BlendMode>(index);
}

float range_of(GtkWidget* row) {
  return static_cast<float>(
      adw_spin_row_get_value(ADW_SPIN_ROW(row)));
}

bool switch_of(GtkWidget* toggle) {
  if (ADW_IS_SWITCH_ROW(toggle)) {
    return adw_switch_row_get_active(ADW_SWITCH_ROW(toggle));
  }

  return gtk_switch_get_active(GTK_SWITCH(toggle));
}

guint choice_of(GtkWidget* button) {
  return static_cast<guint>(choice_index(button));
}

void on_choice_activated(
    GtkListBox*,
    GtkListBoxRow* row,
    gpointer data) {
  auto* button = GTK_MENU_BUTTON(data);
  const int index =
      GPOINTER_TO_INT(
          g_object_get_data(
              G_OBJECT(row),
              "lienzo-choice-index"));
  const char* label =
      static_cast<const char*>(
          g_object_get_data(
              G_OBJECT(row),
              "lienzo-choice-label"));

  g_object_set_data(
      G_OBJECT(button),
      "lienzo-choice-index",
      GINT_TO_POINTER(index));

  if (label != nullptr) {
    gtk_menu_button_set_label(button, label);
  }

  GtkPopover* popover =
      gtk_menu_button_get_popover(button);

  if (popover != nullptr) {
    gtk_popover_popdown(popover);
  }
}

patchy::RgbColor color_of(GtkWidget* button) {
  const GdkRGBA* rgba =
      gtk_color_dialog_button_get_rgba(
          GTK_COLOR_DIALOG_BUTTON(button));

  const auto channel = [](double value) {
    return static_cast<std::uint8_t>(
        std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
  };

  return {
      channel(rgba->red),
      channel(rgba->green),
      channel(rgba->blue)};
}

void set_color(
    GtkWidget* button,
    patchy::RgbColor color) {
  GdkRGBA rgba{};
  rgba.red = color.red / 255.0;
  rgba.green = color.green / 255.0;
  rgba.blue = color.blue / 255.0;
  rgba.alpha = 1.0;
  gtk_color_dialog_button_set_rgba(
      GTK_COLOR_DIALOG_BUTTON(button),
      &rgba);
}

void append_row(GtkWidget* parent, GtkWidget* row) {
  if (ADW_IS_EXPANDER_ROW(parent)) {
    adw_expander_row_add_row(ADW_EXPANDER_ROW(parent), row);
    return;
  }

  adw_preferences_group_add(ADW_PREFERENCES_GROUP(parent), row);
}

GtkWidget* add_scale(
    GtkWidget* parent,
    const char* label,
    double min,
    double max,
    double step,
    double value) {
  GtkWidget* row =
      adw_spin_row_new_with_range(min, max, step);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
  adw_spin_row_set_digits(ADW_SPIN_ROW(row), 0);
  adw_spin_row_set_value(
      ADW_SPIN_ROW(row),
      std::clamp(value, min, max));
  append_row(parent, row);
  return row;
}

GtkWidget* add_switch(
    GtkWidget* parent,
    const char* label,
    bool active) {
  if (
      ADW_IS_EXPANDER_ROW(parent) &&
      std::string_view(label) == "Activar") {
    GtkWidget* toggle = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(toggle), active);
    gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(parent), toggle);
    return toggle;
  }

  GtkWidget* row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
  append_row(parent, row);
  return row;
}

GtkWidget* add_choice(
    GtkWidget* parent,
    const char* label,
    const char* const* items,
    guint selected) {
  GtkWidget* row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);

  GtkWidget* button = gtk_menu_button_new();
  gtk_menu_button_set_always_show_arrow(
      GTK_MENU_BUTTON(button),
      TRUE);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(button, "flat");
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);

  GtkWidget* list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_list_box_set_selection_mode(
      GTK_LIST_BOX(list),
      GTK_SELECTION_NONE);
  guint count = 0;
  const char* selected_label = items[0];

  for (guint index = 0; items[index] != nullptr; ++index) {
    GtkWidget* item = gtk_list_box_row_new();
    GtkWidget* text = gtk_label_new(items[index]);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0F);
    gtk_widget_set_margin_start(text, 10);
    gtk_widget_set_margin_end(text, 10);
    gtk_widget_set_margin_top(text, 6);
    gtk_widget_set_margin_bottom(text, 6);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(item), text);
    g_object_set_data(
        G_OBJECT(item),
        "lienzo-choice-index",
        GINT_TO_POINTER(static_cast<int>(index) + 1));
    g_object_set_data_full(
        G_OBJECT(item),
        "lienzo-choice-label",
        g_strdup(items[index]),
        g_free);
    gtk_list_box_append(GTK_LIST_BOX(list), item);
    ++count;

    if (index == selected) {
      selected_label = items[index];
    }
  }

  g_object_set_data(
      G_OBJECT(button),
      "lienzo-choice-index",
      GINT_TO_POINTER(
          static_cast<int>(
              selected_label == nullptr ? 0 : selected) +
          1));
  gtk_menu_button_set_label(
      GTK_MENU_BUTTON(button),
      selected_label != nullptr ? selected_label : "");

  g_signal_connect(
      list,
      "row-activated",
      G_CALLBACK(on_choice_activated),
      button);

  GtkWidget* scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(scroller),
      GTK_POLICY_NEVER,
      count > 8 ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scroller),
      TRUE);
  gtk_scrolled_window_set_max_content_height(
      GTK_SCROLLED_WINDOW(scroller),
      280);
  gtk_scrolled_window_set_min_content_width(
      GTK_SCROLLED_WINDOW(scroller),
      220);
  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(scroller),
      list);

  GtkWidget* popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(popover), scroller);
  gtk_menu_button_set_popover(
      GTK_MENU_BUTTON(button),
      popover);
  append_row(parent, row);
  return button;
}

GtkWidget* add_color(
    GtkWidget* parent,
    const char* label,
    patchy::RgbColor color) {
  GtkWidget* row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
  GtkWidget* button = gtk_color_dialog_button_new(nullptr);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  set_color(button, color);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
  append_row(parent, row);
  return button;
}

GtkWidget* add_section(
    GtkWidget* page,
    const char* title,
    bool expanded = false) {
  GtkWidget* group = adw_preferences_group_new();
  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(page),
      ADW_PREFERENCES_GROUP(group));

  if (expanded) {
    adw_preferences_group_set_title(
        ADW_PREFERENCES_GROUP(group),
        title);
    return group;
  }

  GtkWidget* row = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(row), FALSE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  return row;
}

template <typename T, typename Fill>
void write_effect(
    std::vector<T>& items,
    bool enabled,
    Fill fill) {
  if (!enabled && items.empty()) {
    return;
  }

  if (items.empty()) {
    items.emplace_back();
  }

  items.front().enabled = enabled;
  fill(items.front());
}

void ensure_gradient(
    patchy::LayerStyleGradient& gradient,
    patchy::RgbColor start,
    patchy::RgbColor end) {
  if (gradient.color_stops.empty()) {
    gradient.color_stops.push_back(
        {0.0F, start, 0.5F, patchy::GradientColorStop::Kind::User});
    gradient.color_stops.push_back(
        {1.0F, end, 0.5F, patchy::GradientColorStop::Kind::User});
  } else if (gradient.color_stops.size() == 1) {
    gradient.color_stops.front().color = start;
    gradient.color_stops.front().location = 0.0F;
    gradient.color_stops.push_back(
        {1.0F, end, 0.5F, patchy::GradientColorStop::Kind::User});
  } else {
    gradient.color_stops.front().color = start;
    gradient.color_stops.back().color = end;
  }

  if (gradient.alpha_stops.size() < 2) {
    gradient.alpha_stops = {
        {0.0F, 1.0F, 0.5F},
        {1.0F, 1.0F, 0.5F}};
  }
}

GtkWidget* dialog_widget(const DialogState* dialog) {
  return GTK_WIDGET(gtk_widget_get_ancestor(
      dialog->name,
      ADW_TYPE_DIALOG));
}

void close_layer_dialog_later(GtkWidget* dialog) {
  if (dialog == nullptr) {
    return;
  }

  g_object_ref(dialog);

  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* widget = GTK_WIDGET(data);

        if (ADW_IS_DIALOG(widget)) {
          adw_dialog_close(ADW_DIALOG(widget));
        }

        g_object_unref(widget);
        return G_SOURCE_REMOVE;
      },
      dialog);
}

void apply_layer_settings(
    GtkButton*,
    gpointer data) {
  auto* dialog = static_cast<DialogState*>(data);
  GtkWidget* alive =
      GTK_WIDGET(g_weak_ref_get(&dialog->parent));
  GtkWidget* widget = dialog_widget(dialog);

  if (alive == nullptr) {
    close_layer_dialog_later(widget);
    return;
  }

  g_object_unref(alive);

  auto* layer = dialog->document->find_layer(dialog->id);

  if (layer == nullptr) {
    close_layer_dialog_later(widget);
    return;
  }

  if (dialog->canvas.checkpoint) {
    dialog->canvas.checkpoint();
  }

  const char* name =
      gtk_editable_get_text(GTK_EDITABLE(dialog->name));

  layer->set_name(
      name != nullptr && name[0] != '\0' ? name : "Capa");
  layer->set_opacity(range_of(dialog->opacity) / 100.0F);
  layer->set_fill_opacity(range_of(dialog->fill) / 100.0F);
  layer->set_blend_mode(blend_from(dialog->blend));
  layer->set_visible(switch_of(dialog->visible));
  layer->set_clipped(switch_of(dialog->clipped));
  layer->set_lock_flags(
      switch_of(dialog->locked)
          ? patchy::kLayerLockAll
          : patchy::kLayerLockNone);

  patchy::LayerBlendIf blend_if = layer->blend_if();
  auto& gray =
      blend_if.channels[static_cast<std::size_t>(
          patchy::BlendIfChannel::Gray)];
  const auto this_black = static_cast<std::uint8_t>(
      std::lround(range_of(dialog->blend_if_this_black)));
  const auto this_white = static_cast<std::uint8_t>(
      std::lround(range_of(dialog->blend_if_this_white)));
  const auto under_black = static_cast<std::uint8_t>(
      std::lround(range_of(dialog->blend_if_under_black)));
  const auto under_white = static_cast<std::uint8_t>(
      std::lround(range_of(dialog->blend_if_under_white)));
  gray.this_layer.black_low = this_black;
  gray.this_layer.black_high = this_black;
  gray.this_layer.white_low = this_white;
  gray.this_layer.white_high = this_white;
  gray.underlying_layer.black_low = under_black;
  gray.underlying_layer.black_high = under_black;
  gray.underlying_layer.white_low = under_white;
  gray.underlying_layer.white_high = under_white;
  (void)layer->set_blend_if(blend_if, true);

  patchy::LayerStyle style =
      std::as_const(*layer).layer_style();
  style.effects_visible = switch_of(dialog->effects_visible);
  style.layer_mask_hides_effects = switch_of(dialog->mask_hides);
  style.blend_interior_elements = switch_of(dialog->blend_interior);
  style.blend_clipped_elements = switch_of(dialog->blend_clipped);

  {
    write_effect(
        style.drop_shadows,
        switch_of(dialog->drop_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->drop_blend);
    effect.color = color_of(dialog->drop_color);
    effect.opacity = range_of(dialog->drop_opacity) / 100.0F;
    effect.angle_degrees = range_of(dialog->drop_angle);
    effect.distance = range_of(dialog->drop_distance);
    effect.spread = range_of(dialog->drop_spread);
    effect.size = range_of(dialog->drop_size);
    effect.use_global_light = switch_of(dialog->drop_global);
    effect.layer_conceals = switch_of(dialog->drop_conceals);
        });
  }

  {
    write_effect(
        style.inner_shadows,
        switch_of(dialog->inner_shadow_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->inner_shadow_blend);
    effect.color = color_of(dialog->inner_shadow_color);
    effect.opacity = range_of(dialog->inner_shadow_opacity) / 100.0F;
    effect.angle_degrees = range_of(dialog->inner_shadow_angle);
    effect.distance = range_of(dialog->inner_shadow_distance);
    effect.choke = range_of(dialog->inner_shadow_choke);
    effect.size = range_of(dialog->inner_shadow_size);
    effect.use_global_light = switch_of(dialog->inner_shadow_global);
        });
  }

  {
    write_effect(
        style.outer_glows,
        switch_of(dialog->outer_glow_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->outer_glow_blend);
    effect.color = color_of(dialog->outer_glow_color);
    effect.opacity = range_of(dialog->outer_glow_opacity) / 100.0F;
    effect.spread = range_of(dialog->outer_glow_spread);
    effect.size = range_of(dialog->outer_glow_size);
    effect.range = range_of(dialog->outer_glow_range);
    effect.technique = static_cast<patchy::LayerGlowTechnique>(
        choice_of(dialog->outer_glow_technique));
        });
  }

  {
    write_effect(
        style.inner_glows,
        switch_of(dialog->inner_glow_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->inner_glow_blend);
    effect.color = color_of(dialog->inner_glow_color);
    effect.opacity = range_of(dialog->inner_glow_opacity) / 100.0F;
    effect.choke = range_of(dialog->inner_glow_choke);
    effect.size = range_of(dialog->inner_glow_size);
    effect.range = range_of(dialog->inner_glow_range);
    effect.technique = static_cast<patchy::LayerGlowTechnique>(
        choice_of(dialog->inner_glow_technique));
    effect.source = static_cast<patchy::LayerInnerGlowSource>(
        choice_of(dialog->inner_glow_source));
        });
  }

  {
    write_effect(
        style.bevels,
        switch_of(dialog->bevel_on),
        [&](auto& effect) {
    effect.style = static_cast<patchy::BevelEmbossStyleKind>(
        choice_of(dialog->bevel_style));
    effect.technique = static_cast<patchy::BevelTechnique>(
        choice_of(dialog->bevel_technique));
    effect.depth = range_of(dialog->bevel_depth) / 100.0F;
    effect.size = range_of(dialog->bevel_size);
    effect.soften = range_of(dialog->bevel_soften);
    effect.angle_degrees = range_of(dialog->bevel_angle);
    effect.altitude_degrees = range_of(dialog->bevel_altitude);
    effect.direction_up = switch_of(dialog->bevel_up);
    effect.use_global_light = switch_of(dialog->bevel_global);
    effect.highlight_blend_mode =
        blend_from(dialog->bevel_highlight_blend);
    effect.highlight_color = color_of(dialog->bevel_highlight_color);
    effect.highlight_opacity =
        range_of(dialog->bevel_highlight_opacity) / 100.0F;
    effect.shadow_blend_mode =
        blend_from(dialog->bevel_shadow_blend);
    effect.shadow_color = color_of(dialog->bevel_shadow_color);
    effect.shadow_opacity =
        range_of(dialog->bevel_shadow_opacity) / 100.0F;
        });
  }

  {
    write_effect(
        style.satins,
        switch_of(dialog->satin_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->satin_blend);
    effect.color = color_of(dialog->satin_color);
    effect.opacity = range_of(dialog->satin_opacity) / 100.0F;
    effect.angle_degrees = range_of(dialog->satin_angle);
    effect.distance = range_of(dialog->satin_distance);
    effect.size = range_of(dialog->satin_size);
    effect.invert = switch_of(dialog->satin_invert);
        });
  }

  {
    write_effect(
        style.color_overlays,
        switch_of(dialog->color_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->color_blend);
    effect.color = color_of(dialog->color_color);
    effect.opacity = range_of(dialog->color_opacity) / 100.0F;
        });
  }

  {
    write_effect(
        style.gradient_fills,
        switch_of(dialog->gradient_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->gradient_blend);
    effect.opacity = range_of(dialog->gradient_opacity) / 100.0F;
    effect.gradient.angle_degrees = range_of(dialog->gradient_angle);
    effect.gradient.scale = range_of(dialog->gradient_scale) / 100.0F;
    effect.gradient.type = static_cast<patchy::LayerStyleGradientType>(
        choice_of(dialog->gradient_type));
    effect.gradient.reverse = switch_of(dialog->gradient_reverse);
    ensure_gradient(
        effect.gradient,
        color_of(dialog->gradient_start),
        color_of(dialog->gradient_end));
        });
  }

  {
    const guint pattern = choice_of(dialog->pattern_choice);
    const bool pattern_enabled =
        switch_of(dialog->pattern_on) &&
        pattern > 0 &&
        pattern < dialog->pattern_ids.size();
    write_effect(
        style.pattern_overlays,
        pattern_enabled,
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->pattern_blend);
    effect.opacity = range_of(dialog->pattern_opacity) / 100.0F;
    effect.scale = range_of(dialog->pattern_scale) / 100.0F;
    effect.angle_degrees = range_of(dialog->pattern_angle);

    if (
        pattern > 0 &&
        pattern < dialog->pattern_ids.size()) {
      effect.pattern_id = dialog->pattern_ids[pattern];

      if (pattern < dialog->document->metadata().patterns.patterns.size() + 1) {
        effect.pattern_name =
            dialog->document->metadata().patterns.patterns[pattern - 1].name;
      }
    }
        });
  }

  {
    write_effect(
        style.strokes,
        switch_of(dialog->stroke_on),
        [&](auto& effect) {
    effect.blend_mode = blend_from(dialog->stroke_blend);
    effect.color = color_of(dialog->stroke_color);
    effect.opacity = range_of(dialog->stroke_opacity) / 100.0F;
    effect.size = range_of(dialog->stroke_size);
    effect.position = static_cast<patchy::LayerStrokePosition>(
        choice_of(dialog->stroke_position));
    effect.overprint = switch_of(dialog->stroke_overprint);
        });
  }

  layer->layer_style() = std::move(style);

  auto after = std::move(dialog->after_apply);

  if (after) {
    after();
  }

  close_layer_dialog_later(widget);
}

}  // namespace

void present_layer_settings(
    patchy::Document& document,
    patchy::LayerId id,
    const CanvasView& canvas,
    GtkWidget* parent,
    std::function<void()> after_apply) {
  const auto* layer =
      std::as_const(document).find_layer(id);

  if (layer == nullptr) {
    return;
  }

  const patchy::LayerStyle style =
      std::as_const(*layer).layer_style();
  const patchy::LayerBlendIf blend_if = layer->blend_if();
  const auto& gray =
      blend_if.channels[static_cast<std::size_t>(
          patchy::BlendIfChannel::Gray)];

  auto* dialog_state = new DialogState{};
  g_weak_ref_init(&dialog_state->parent, parent);
  dialog_state->document = &document;
  dialog_state->id = id;
  dialog_state->canvas = canvas;
  dialog_state->after_apply = std::move(after_apply);

  GtkWidget* content = adw_preferences_page_new();

  GtkWidget* general = add_section(content, "General", true);
  dialog_state->name = adw_entry_row_new();
  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(dialog_state->name),
      "Nombre");
  gtk_editable_set_text(
      GTK_EDITABLE(dialog_state->name),
      layer->name().c_str());
  append_row(general, dialog_state->name);
  dialog_state->opacity = add_scale(
      general,
      "Opacidad",
      0,
      100,
      1,
      layer->opacity() * 100.0);
  dialog_state->fill = add_scale(
      general,
      "Relleno",
      0,
      100,
      1,
      layer->fill_opacity() * 100.0);
  dialog_state->blend = add_choice(
      general,
      "Modo de fusión",
      blend_mode_names,
      blend_index(layer->blend_mode()));
  dialog_state->visible = add_switch(
      general,
      "Visible",
      layer->visible());
  dialog_state->clipped = add_switch(
      general,
      "Máscara de recorte",
      layer->clipped());
  dialog_state->locked = add_switch(
      general,
      "Bloquear capa",
      layer->lock_flags() != patchy::kLayerLockNone);

  GtkWidget* blending = add_section(content, "Opciones de fusión");
  dialog_state->effects_visible = add_switch(
      blending,
      "Efectos visibles",
      style.effects_visible);
  dialog_state->mask_hides = add_switch(
      blending,
      "La máscara oculta los efectos",
      style.layer_mask_hides_effects);
  dialog_state->blend_interior = add_switch(
      blending,
      "Fusionar efectos interiores como grupo",
      style.blend_interior_elements);
  dialog_state->blend_clipped = add_switch(
      blending,
      "Fusionar capas recortadas como grupo",
      style.blend_clipped_elements);
  dialog_state->blend_if_this_black = add_scale(
      blending,
      "Esta capa, negro",
      0,
      255,
      1,
      gray.this_layer.black_low);
  dialog_state->blend_if_this_white = add_scale(
      blending,
      "Esta capa, blanco",
      0,
      255,
      1,
      gray.this_layer.white_high);
  dialog_state->blend_if_under_black = add_scale(
      blending,
      "Capa subyacente, negro",
      0,
      255,
      1,
      gray.underlying_layer.black_low);
  dialog_state->blend_if_under_white = add_scale(
      blending,
      "Capa subyacente, blanco",
      0,
      255,
      1,
      gray.underlying_layer.white_high);

  const patchy::LayerDropShadow drop =
      style.drop_shadows.empty()
          ? patchy::LayerDropShadow{}
          : style.drop_shadows.front();
  GtkWidget* drop_box = add_section(content, "Sombra paralela");
  dialog_state->drop_on = add_switch(drop_box, "Activar", drop.enabled);
  dialog_state->drop_blend = add_choice(
      drop_box,
      "Modo",
      blend_mode_names,
      blend_index(drop.blend_mode));
  dialog_state->drop_color = add_color(drop_box, "Color", drop.color);
  dialog_state->drop_opacity = add_scale(
      drop_box, "Opacidad", 0, 100, 1, drop.opacity * 100.0);
  dialog_state->drop_angle = add_scale(
      drop_box, "Ángulo", 0, 360, 1, drop.angle_degrees);
  dialog_state->drop_distance = add_scale(
      drop_box, "Distancia", 0, 250, 1, drop.distance);
  dialog_state->drop_spread = add_scale(
      drop_box, "Extensión", 0, 100, 1, drop.spread);
  dialog_state->drop_size = add_scale(
      drop_box, "Tamaño", 0, 250, 1, drop.size);
  dialog_state->drop_global = add_switch(
      drop_box, "Usar luz global", drop.use_global_light);
  dialog_state->drop_conceals = add_switch(
      drop_box, "La capa tapa la sombra", drop.layer_conceals);

  const patchy::LayerInnerShadow inner_shadow =
      style.inner_shadows.empty()
          ? patchy::LayerInnerShadow{}
          : style.inner_shadows.front();
  GtkWidget* inner_shadow_box = add_section(content, "Sombra interior");
  dialog_state->inner_shadow_on =
      add_switch(inner_shadow_box, "Activar", inner_shadow.enabled);
  dialog_state->inner_shadow_blend = add_choice(
      inner_shadow_box,
      "Modo",
      blend_mode_names,
      blend_index(inner_shadow.blend_mode));
  dialog_state->inner_shadow_color =
      add_color(inner_shadow_box, "Color", inner_shadow.color);
  dialog_state->inner_shadow_opacity = add_scale(
      inner_shadow_box,
      "Opacidad",
      0,
      100,
      1,
      inner_shadow.opacity * 100.0);
  dialog_state->inner_shadow_angle = add_scale(
      inner_shadow_box, "Ángulo", 0, 360, 1, inner_shadow.angle_degrees);
  dialog_state->inner_shadow_distance = add_scale(
      inner_shadow_box, "Distancia", 0, 250, 1, inner_shadow.distance);
  dialog_state->inner_shadow_choke = add_scale(
      inner_shadow_box, "Retraer", 0, 100, 1, inner_shadow.choke);
  dialog_state->inner_shadow_size = add_scale(
      inner_shadow_box, "Tamaño", 0, 250, 1, inner_shadow.size);
  dialog_state->inner_shadow_global = add_switch(
      inner_shadow_box,
      "Usar luz global",
      inner_shadow.use_global_light);

  const patchy::LayerOuterGlow outer_glow =
      style.outer_glows.empty()
          ? patchy::LayerOuterGlow{}
          : style.outer_glows.front();
  GtkWidget* outer_glow_box = add_section(content, "Resplandor exterior");
  dialog_state->outer_glow_on =
      add_switch(outer_glow_box, "Activar", outer_glow.enabled);
  dialog_state->outer_glow_blend = add_choice(
      outer_glow_box,
      "Modo",
      blend_mode_names,
      blend_index(outer_glow.blend_mode));
  dialog_state->outer_glow_color =
      add_color(outer_glow_box, "Color", outer_glow.color);
  dialog_state->outer_glow_opacity = add_scale(
      outer_glow_box,
      "Opacidad",
      0,
      100,
      1,
      outer_glow.opacity * 100.0);
  dialog_state->outer_glow_spread = add_scale(
      outer_glow_box, "Extensión", 0, 100, 1, outer_glow.spread);
  dialog_state->outer_glow_size = add_scale(
      outer_glow_box, "Tamaño", 0, 250, 1, outer_glow.size);
  dialog_state->outer_glow_range = add_scale(
      outer_glow_box, "Rango", 1, 100, 1, outer_glow.range);
  dialog_state->outer_glow_technique = add_choice(
      outer_glow_box,
      "Técnica",
      glow_techniques,
      static_cast<guint>(outer_glow.technique));

  const patchy::LayerInnerGlow inner_glow =
      style.inner_glows.empty()
          ? patchy::LayerInnerGlow{}
          : style.inner_glows.front();
  GtkWidget* inner_glow_box = add_section(content, "Resplandor interior");
  dialog_state->inner_glow_on =
      add_switch(inner_glow_box, "Activar", inner_glow.enabled);
  dialog_state->inner_glow_blend = add_choice(
      inner_glow_box,
      "Modo",
      blend_mode_names,
      blend_index(inner_glow.blend_mode));
  dialog_state->inner_glow_color =
      add_color(inner_glow_box, "Color", inner_glow.color);
  dialog_state->inner_glow_opacity = add_scale(
      inner_glow_box,
      "Opacidad",
      0,
      100,
      1,
      inner_glow.opacity * 100.0);
  dialog_state->inner_glow_choke = add_scale(
      inner_glow_box, "Retraer", 0, 100, 1, inner_glow.choke);
  dialog_state->inner_glow_size = add_scale(
      inner_glow_box, "Tamaño", 0, 250, 1, inner_glow.size);
  dialog_state->inner_glow_range = add_scale(
      inner_glow_box, "Rango", 1, 100, 1, inner_glow.range);
  dialog_state->inner_glow_technique = add_choice(
      inner_glow_box,
      "Técnica",
      glow_techniques,
      static_cast<guint>(inner_glow.technique));
  dialog_state->inner_glow_source = add_choice(
      inner_glow_box,
      "Origen",
      glow_sources,
      static_cast<guint>(inner_glow.source));

  const patchy::LayerBevelEmboss bevel =
      style.bevels.empty()
          ? patchy::LayerBevelEmboss{}
          : style.bevels.front();
  GtkWidget* bevel_box = add_section(content, "Bisel y relieve");
  dialog_state->bevel_on = add_switch(bevel_box, "Activar", bevel.enabled);
  dialog_state->bevel_style = add_choice(
      bevel_box,
      "Estilo",
      bevel_styles,
      static_cast<guint>(bevel.style));
  dialog_state->bevel_technique = add_choice(
      bevel_box,
      "Técnica",
      bevel_techniques,
      static_cast<guint>(bevel.technique));
  dialog_state->bevel_depth = add_scale(
      bevel_box, "Profundidad", 0, 1000, 1, bevel.depth * 100.0);
  dialog_state->bevel_size = add_scale(
      bevel_box, "Tamaño", 0, 250, 1, bevel.size);
  dialog_state->bevel_soften = add_scale(
      bevel_box, "Suavizar", 0, 50, 1, bevel.soften);
  dialog_state->bevel_angle = add_scale(
      bevel_box, "Ángulo", 0, 360, 1, bevel.angle_degrees);
  dialog_state->bevel_altitude = add_scale(
      bevel_box, "Altitud", 0, 90, 1, bevel.altitude_degrees);
  dialog_state->bevel_up = add_switch(
      bevel_box, "Hacia arriba", bevel.direction_up);
  dialog_state->bevel_global = add_switch(
      bevel_box, "Usar luz global", bevel.use_global_light);
  dialog_state->bevel_highlight_blend = add_choice(
      bevel_box,
      "Modo de luz",
      blend_mode_names,
      blend_index(bevel.highlight_blend_mode));
  dialog_state->bevel_highlight_color =
      add_color(bevel_box, "Color de luz", bevel.highlight_color);
  dialog_state->bevel_highlight_opacity = add_scale(
      bevel_box,
      "Opacidad de luz",
      0,
      100,
      1,
      bevel.highlight_opacity * 100.0);
  dialog_state->bevel_shadow_blend = add_choice(
      bevel_box,
      "Modo de sombra",
      blend_mode_names,
      blend_index(bevel.shadow_blend_mode));
  dialog_state->bevel_shadow_color =
      add_color(bevel_box, "Color de sombra", bevel.shadow_color);
  dialog_state->bevel_shadow_opacity = add_scale(
      bevel_box,
      "Opacidad de sombra",
      0,
      100,
      1,
      bevel.shadow_opacity * 100.0);

  const patchy::LayerSatin satin =
      style.satins.empty() ? patchy::LayerSatin{} : style.satins.front();
  GtkWidget* satin_box = add_section(content, "Satinado");
  dialog_state->satin_on = add_switch(satin_box, "Activar", satin.enabled);
  dialog_state->satin_blend = add_choice(
      satin_box,
      "Modo",
      blend_mode_names,
      blend_index(satin.blend_mode));
  dialog_state->satin_color = add_color(satin_box, "Color", satin.color);
  dialog_state->satin_opacity = add_scale(
      satin_box, "Opacidad", 0, 100, 1, satin.opacity * 100.0);
  dialog_state->satin_angle = add_scale(
      satin_box, "Ángulo", 0, 360, 1, satin.angle_degrees);
  dialog_state->satin_distance = add_scale(
      satin_box, "Distancia", 0, 250, 1, satin.distance);
  dialog_state->satin_size = add_scale(
      satin_box, "Tamaño", 0, 250, 1, satin.size);
  dialog_state->satin_invert = add_switch(
      satin_box, "Invertir", satin.invert);

  const patchy::LayerColorOverlay color_overlay =
      style.color_overlays.empty()
          ? patchy::LayerColorOverlay{}
          : style.color_overlays.front();
  GtkWidget* color_box = add_section(content, "Superposición de color");
  dialog_state->color_on =
      add_switch(color_box, "Activar", color_overlay.enabled);
  dialog_state->color_blend = add_choice(
      color_box,
      "Modo",
      blend_mode_names,
      blend_index(color_overlay.blend_mode));
  dialog_state->color_color =
      add_color(color_box, "Color", color_overlay.color);
  dialog_state->color_opacity = add_scale(
      color_box,
      "Opacidad",
      0,
      100,
      1,
      color_overlay.opacity * 100.0);

  const patchy::LayerGradientFill gradient_fill =
      style.gradient_fills.empty()
          ? patchy::LayerGradientFill{}
          : style.gradient_fills.front();
  const patchy::RgbColor gradient_start =
      gradient_fill.gradient.color_stops.empty()
          ? patchy::RgbColor{0, 0, 0}
          : gradient_fill.gradient.color_stops.front().color;
  const patchy::RgbColor gradient_end =
      gradient_fill.gradient.color_stops.size() < 2
          ? patchy::RgbColor{255, 255, 255}
          : gradient_fill.gradient.color_stops.back().color;
  GtkWidget* gradient_box =
      add_section(content, "Superposición de degradado");
  dialog_state->gradient_on =
      add_switch(gradient_box, "Activar", gradient_fill.enabled);
  dialog_state->gradient_blend = add_choice(
      gradient_box,
      "Modo",
      blend_mode_names,
      blend_index(gradient_fill.blend_mode));
  dialog_state->gradient_opacity = add_scale(
      gradient_box,
      "Opacidad",
      0,
      100,
      1,
      gradient_fill.opacity * 100.0);
  dialog_state->gradient_type = add_choice(
      gradient_box,
      "Tipo",
      gradient_types,
      static_cast<guint>(gradient_fill.gradient.type));
  dialog_state->gradient_angle = add_scale(
      gradient_box,
      "Ángulo",
      0,
      360,
      1,
      gradient_fill.gradient.angle_degrees);
  dialog_state->gradient_scale = add_scale(
      gradient_box,
      "Escala",
      1,
      1000,
      1,
      gradient_fill.gradient.scale * 100.0);
  dialog_state->gradient_reverse = add_switch(
      gradient_box,
      "Invertir",
      gradient_fill.gradient.reverse);
  dialog_state->gradient_start =
      add_color(gradient_box, "Color inicial", gradient_start);
  dialog_state->gradient_end =
      add_color(gradient_box, "Color final", gradient_end);

  const patchy::LayerPatternOverlay pattern =
      style.pattern_overlays.empty()
          ? patchy::LayerPatternOverlay{}
          : style.pattern_overlays.front();
  dialog_state->pattern_ids.push_back({});
  std::vector<const char*> pattern_labels{"Sin motivo"};

  for (const auto& resource : std::as_const(document).metadata().patterns.patterns) {
    dialog_state->pattern_ids.push_back(resource.id);
    pattern_labels.push_back(resource.name.c_str());
  }

  pattern_labels.push_back(nullptr);
  guint pattern_index = 0;

  for (std::size_t i = 1; i < dialog_state->pattern_ids.size(); ++i) {
    if (dialog_state->pattern_ids[i] == pattern.pattern_id) {
      pattern_index = static_cast<guint>(i);
      break;
    }
  }

  GtkWidget* pattern_box = add_section(content, "Superposición de motivo");
  dialog_state->pattern_on =
      add_switch(pattern_box, "Activar", pattern.enabled);
  dialog_state->pattern_choice = add_choice(
      pattern_box,
      "Motivo",
      pattern_labels.data(),
      pattern_index);
  dialog_state->pattern_blend = add_choice(
      pattern_box,
      "Modo",
      blend_mode_names,
      blend_index(pattern.blend_mode));
  dialog_state->pattern_opacity = add_scale(
      pattern_box, "Opacidad", 0, 100, 1, pattern.opacity * 100.0);
  dialog_state->pattern_scale = add_scale(
      pattern_box, "Escala", 1, 1000, 1, pattern.scale * 100.0);
  dialog_state->pattern_angle = add_scale(
      pattern_box, "Ángulo", 0, 360, 1, pattern.angle_degrees);

  const patchy::LayerStroke stroke =
      style.strokes.empty() ? patchy::LayerStroke{} : style.strokes.front();
  GtkWidget* stroke_box = add_section(content, "Trazo");
  dialog_state->stroke_on =
      add_switch(stroke_box, "Activar", stroke.enabled);
  dialog_state->stroke_blend = add_choice(
      stroke_box,
      "Modo",
      blend_mode_names,
      blend_index(stroke.blend_mode));
  dialog_state->stroke_color =
      add_color(stroke_box, "Color", stroke.color);
  dialog_state->stroke_opacity = add_scale(
      stroke_box, "Opacidad", 0, 100, 1, stroke.opacity * 100.0);
  dialog_state->stroke_size = add_scale(
      stroke_box, "Tamaño", 0, 250, 1, stroke.size);
  dialog_state->stroke_position = add_choice(
      stroke_box,
      "Posición",
      stroke_positions,
      static_cast<guint>(stroke.position));
  dialog_state->stroke_overprint = add_switch(
      stroke_box, "Sobreimprimir", stroke.overprint);

  GtkWidget* cancel = gtk_button_new_with_label("Cancelar");
  GtkWidget* apply = gtk_button_new_with_label("Aplicar");
  gtk_widget_add_css_class(apply, "suggested-action");

  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), apply);

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);

  GtkWidget* dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_title(ADW_DIALOG(dialog), "Ajustes de capa");
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 480);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 640);
  adw_dialog_set_child(ADW_DIALOG(dialog), toolbar);
  adw_dialog_set_default_widget(ADW_DIALOG(dialog), apply);
  g_object_set_data_full(
      G_OBJECT(dialog),
      "lienzo-layer-settings",
      dialog_state,
      [](gpointer data) {
        delete static_cast<DialogState*>(data);
      });

  GtkWidget* host =
      GTK_WIDGET(gtk_widget_get_ancestor(
          parent,
          GTK_TYPE_WINDOW));

  if (host != nullptr) {
    g_object_set_data(
        G_OBJECT(host),
        "lienzo-block-close",
        GINT_TO_POINTER(1));
    g_object_set_data(
        G_OBJECT(dialog),
        "lienzo-block-close-host",
        host);
  }

  g_signal_connect(
      dialog,
      "closed",
      G_CALLBACK(+[](AdwDialog* self, gpointer) {
        gpointer blocked =
            g_object_get_data(
                G_OBJECT(self),
                "lienzo-block-close-host");

        if (blocked != nullptr) {
          g_object_set_data(
              G_OBJECT(blocked),
              "lienzo-block-close",
              nullptr);
        }
      }),
      nullptr);

  g_signal_connect(
      cancel,
      "clicked",
      G_CALLBACK(+[](GtkButton*, gpointer data) {
        close_layer_dialog_later(GTK_WIDGET(data));
      }),
      dialog);
  g_signal_connect(
      apply,
      "clicked",
      G_CALLBACK(apply_layer_settings),
      dialog_state);

  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

}  // namespace lienzo::gnome
