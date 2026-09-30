#include "ui-gnome/inspector.hpp"
#include "ui-gnome/layer_thumbnail.hpp"
#include "render/compositor.hpp"
#include "core/adjustment_layer.hpp"

#include <adwaita.h>

#include <cstdint>
#include <string>

namespace lienzo::gnome {

namespace {

struct InspectorState {
  patchy::Document* document{};
  CanvasView canvas;

  GtkListBox* layers{};
  GtkListBox* channels{};
  GtkListBox* paths{};
};

struct LayerBinding {
  InspectorState* state{};
  patchy::LayerId id{};
};

void clear_list(
    GtkListBox* list) {
  while (GtkWidget* child =
             gtk_widget_get_first_child(
                 GTK_WIDGET(list))) {
    gtk_list_box_remove(
        list,
        child);
  }
}

void visibility_changed(
    GtkCheckButton* button,
    gpointer data) {
  auto* binding =
      static_cast<LayerBinding*>(data);

  auto* layer =
      binding->state->document->find_layer(
          binding->id);

  if (layer == nullptr) {
    return;
  }

  layer->set_visible(
      gtk_check_button_get_active(
          button));

  if (binding->state->canvas.refresh) {
    binding->state->canvas.refresh();
  }
}

void add_layer_rows(
    InspectorState* state,
    const std::vector<patchy::Layer>& layers,
    int depth) {
  for (auto it = layers.rbegin();
       it != layers.rend();
       ++it) {
    const auto& layer = *it;

    GtkWidget* row =
        gtk_list_box_row_new();

    auto* id =
        new patchy::LayerId(
            layer.id());

    g_object_set_data_full(
        G_OBJECT(row),
        "lienzo-layer-id",
        id,
        [](gpointer data) {
          delete static_cast<patchy::LayerId*>(
              data);
        });

    GtkWidget* content =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            8);

    gtk_widget_set_margin_top(
        content,
        3);

    gtk_widget_set_margin_bottom(
        content,
        3);

    gtk_widget_set_margin_start(
        content,
        6 + depth * 12);

    gtk_widget_set_margin_end(
        content,
        6);

    gtk_widget_set_valign(
        content,
        GTK_ALIGN_CENTER);

    GtkWidget* visible =
        gtk_check_button_new();

    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(visible),
        layer.visible());

    auto* binding =
        new LayerBinding{
            state,
            layer.id()};

    g_signal_connect_data(
        visible,
        "toggled",
        G_CALLBACK(visibility_changed),
        binding,
        [](gpointer data, GClosure*) {
          delete static_cast<LayerBinding*>(
              data);
        },
        GConnectFlags(0));

    GtkWidget* icon =
        create_layer_thumbnail(
            layer,
            state->document->width(),
            state->document->height());

    GtkWidget* name =
        gtk_label_new(
            layer.name().c_str());

    gtk_label_set_xalign(
        GTK_LABEL(name),
        0.0F);

    gtk_label_set_ellipsize(
        GTK_LABEL(name),
        PANGO_ELLIPSIZE_END);

    gtk_label_set_lines(
        GTK_LABEL(name),
        1);

    gtk_widget_set_hexpand(
        name,
        TRUE);

    gtk_widget_set_halign(
        name,
        GTK_ALIGN_START);

    gtk_widget_set_valign(
        name,
        GTK_ALIGN_CENTER);

    gtk_box_append(
        GTK_BOX(content),
        visible);

    gtk_box_append(
        GTK_BOX(content),
        icon);

    if (layer.mask().has_value()) {
      GtkWidget* mask =
          create_mask_thumbnail(
              *layer.mask());

      gtk_widget_set_tooltip_text(
          mask,
          "Máscara de capa");

      gtk_widget_set_size_request(
          mask,
          24,
          24);

      gtk_box_append(
          GTK_BOX(content),
          mask);
    }

    gtk_box_append(
        GTK_BOX(content),
        name);

    gtk_list_box_row_set_child(
        GTK_LIST_BOX_ROW(row),
        content);

    gtk_list_box_append(
        state->layers,
        row);

    if (
        state->document->active_layer_id().has_value() &&
        *state->document->active_layer_id() ==
            layer.id()) {
      gtk_list_box_select_row(
          state->layers,
          GTK_LIST_BOX_ROW(row));
    }

    if (
        layer.kind() ==
        patchy::LayerKind::Group) {
      add_layer_rows(
          state,
          layer.children(),
          depth + 1);
    }
  }
}

void rebuild_layers(
    InspectorState* state) {
  clear_list(
      state->layers);

  add_layer_rows(
      state,
      std::as_const(
          *state->document).layers(),
      0);
}

std::uint8_t white_backed_component(
    std::uint8_t component,
    std::uint8_t alpha) {
  return static_cast<std::uint8_t>(
      (static_cast<int>(component) *
           static_cast<int>(alpha) +
       255 *
           (255 - static_cast<int>(alpha))) /
      255);
}

GtkWidget* make_channel_row(
    const char* name,
    GtkWidget* thumbnail,
    const char* detail) {
  GtkWidget* row =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          8);

  gtk_widget_set_margin_top(row, 3);
  gtk_widget_set_margin_bottom(row, 3);
  gtk_widget_set_margin_start(row, 6);
  gtk_widget_set_margin_end(row, 6);

  gtk_widget_set_size_request(
      thumbnail,
      42,
      30);

  gtk_box_append(
      GTK_BOX(row),
      thumbnail);

  GtkWidget* text =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  GtkWidget* title =
      gtk_label_new(name);

  gtk_label_set_xalign(
      GTK_LABEL(title),
      0.0F);

  gtk_label_set_ellipsize(
      GTK_LABEL(title),
      PANGO_ELLIPSIZE_END);

  gtk_box_append(
      GTK_BOX(text),
      title);

  if (
      detail != nullptr &&
      *detail != 0) {
    GtkWidget* subtitle =
        gtk_label_new(detail);

    gtk_label_set_xalign(
        GTK_LABEL(subtitle),
        0.0F);

    gtk_widget_add_css_class(
        subtitle,
        "dim-label");

    gtk_widget_add_css_class(
        subtitle,
        "caption");

    gtk_box_append(
        GTK_BOX(text),
        subtitle);
  }

  gtk_widget_set_hexpand(
      text,
      TRUE);

  gtk_box_append(
      GTK_BOX(row),
      text);

  return row;
}

void rebuild_channels(
    InspectorState* state) {
  clear_list(
      state->channels);

  const auto& document =
      std::as_const(
          *state->document);

  std::vector<std::uint8_t> alpha;

  const auto composite =
      patchy::Compositor{}.flatten_rgb8(
          document,
          &alpha);

  patchy::PixelBuffer composite_gray(
      document.width(),
      document.height(),
      patchy::PixelFormat::gray8());

  patchy::PixelBuffer red(
      document.width(),
      document.height(),
      patchy::PixelFormat::gray8());

  patchy::PixelBuffer green(
      document.width(),
      document.height(),
      patchy::PixelFormat::gray8());

  patchy::PixelBuffer blue(
      document.width(),
      document.height(),
      patchy::PixelFormat::gray8());

  for (
      int y = 0;
      y < document.height();
      ++y) {
    const auto source =
        composite.row(y);

    auto composite_dest =
        composite_gray.row(y);

    auto red_dest =
        red.row(y);

    auto green_dest =
        green.row(y);

    auto blue_dest =
        blue.row(y);

    for (
        int x = 0;
        x < document.width();
        ++x) {
      const std::size_t pixel_index =
          static_cast<std::size_t>(y) *
              static_cast<std::size_t>(
                  document.width()) +
          static_cast<std::size_t>(x);

      const auto* pixel =
          source.data() +
          static_cast<std::size_t>(x) * 3U;

      const auto a =
          pixel_index < alpha.size()
              ? alpha[pixel_index]
              : 255;

      const auto r =
          white_backed_component(
              pixel[0],
              a);

      const auto g =
          white_backed_component(
              pixel[1],
              a);

      const auto b =
          white_backed_component(
              pixel[2],
              a);

      red_dest[
          static_cast<std::size_t>(x)] = r;

      green_dest[
          static_cast<std::size_t>(x)] = g;

      blue_dest[
          static_cast<std::size_t>(x)] = b;

      composite_dest[
          static_cast<std::size_t>(x)] =
          static_cast<std::uint8_t>(
              (static_cast<int>(r) * 30 +
               static_cast<int>(g) * 59 +
               static_cast<int>(b) * 11) /
              100);
    }
  }

  gtk_list_box_append(
      state->channels,
      make_channel_row(
          "RGB",
          create_channel_thumbnail(
              composite_gray),
          "Compuesto"));

  gtk_list_box_append(
      state->channels,
      make_channel_row(
          "Rojo",
          create_channel_thumbnail(red),
          "Canal de componente"));

  gtk_list_box_append(
      state->channels,
      make_channel_row(
          "Verde",
          create_channel_thumbnail(green),
          "Canal de componente"));

  gtk_list_box_append(
      state->channels,
      make_channel_row(
          "Azul",
          create_channel_thumbnail(blue),
          "Canal de componente"));

  for (
      const auto& channel :
      document.channels()) {
    std::string detail =
        channel.kind() ==
                patchy::DocumentChannelKind::Spot
            ? "Tinta plana"
            : "Canal alfa";

    detail += " · ";
    detail += std::to_string(
        static_cast<int>(
            std::lround(
                channel.display_info().opacity *
                100.0F)));
    detail += "%";

    GtkWidget* row =
        make_channel_row(
            channel.name().c_str(),
            create_channel_thumbnail(
                channel.pixels()),
            detail.c_str());

    auto* id =
        new patchy::ChannelId(
            channel.id());

    g_object_set_data_full(
        G_OBJECT(row),
        "lienzo-channel-id",
        id,
        [](gpointer data) {
          delete static_cast<
              patchy::ChannelId*>(data);
        });

    gtk_list_box_append(
        state->channels,
        row);
  }
}

void rebuild_paths(
    InspectorState* state) {
  clear_list(
      state->paths);

  const auto& paths =
      std::as_const(
          *state->document).paths();

  if (paths.empty()) {
    GtkWidget* empty =
        gtk_label_new(
            "No hay trazados");

    gtk_widget_add_css_class(
        empty,
        "dim-label");

    gtk_widget_set_margin_top(
        empty,
        18);

    gtk_list_box_append(
        state->paths,
        empty);

    return;
  }

  for (const auto& path : paths) {
    std::string name =
        path.name();

    if (name.empty()) {
      name =
          path.kind() ==
                  patchy::DocumentPathKind::Work
              ? "Trazado de trabajo"
              : "Trazado";
    }

    GtkWidget* label =
        gtk_label_new(
            name.c_str());

    gtk_widget_set_margin_top(
        label,
        8);

    gtk_widget_set_margin_bottom(
        label,
        8);

    gtk_widget_set_margin_start(
        label,
        10);

    gtk_widget_set_halign(
        label,
        GTK_ALIGN_START);

    gtk_list_box_append(
        state->paths,
        label);
  }
}

void layer_selected(
    GtkListBox*,
    GtkListBoxRow* row,
    gpointer data) {
  if (row == nullptr) {
    return;
  }

  auto* state =
      static_cast<InspectorState*>(data);

  auto* id =
      static_cast<patchy::LayerId*>(
          g_object_get_data(
              G_OBJECT(row),
              "lienzo-layer-id"));

  if (id == nullptr) {
    return;
  }

  if (
      state->document->find_layer(
          *id) != nullptr) {
    state->document->set_active_layer(
        *id);
  }
}

void refresh_after_layer_change(
    InspectorState* state) {
  rebuild_layers(state);
  rebuild_channels(state);
  rebuild_paths(state);

  if (state->canvas.refresh) {
    state->canvas.refresh();
  }
}

struct RenameDialogContext {
  InspectorState* state{};
  GtkWidget* entry{};
  GtkWindow* dialog{};
  patchy::LayerId id{};
};

void apply_rename_clicked(
    GtkButton*,
    gpointer data) {
  auto* context =
      static_cast<RenameDialogContext*>(data);

  auto* layer =
      context->state->document->find_layer(
          context->id);

  if (layer != nullptr) {
    const char* value =
        gtk_editable_get_text(
            GTK_EDITABLE(context->entry));

    if (
        value != nullptr &&
        *value != 0) {
      layer->set_name(value);

      refresh_after_layer_change(
          context->state);
    }
  }

  gtk_window_destroy(
      context->dialog);
}

void destroy_rename_context(
    gpointer data,
    GClosure*) {
  delete static_cast<RenameDialogContext*>(
      data);
}

void rename_layer_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  auto* layer =
      state->document->find_layer(
          *active);

  if (layer == nullptr) {
    return;
  }

  GtkWidget* dialog =
      gtk_window_new();

  gtk_window_set_title(
      GTK_WINDOW(dialog),
      "Cambiar nombre");

  gtk_window_set_transient_for(
      GTK_WINDOW(dialog),
      GTK_WINDOW(
          gtk_widget_get_root(
              GTK_WIDGET(state->layers))));

  gtk_window_set_modal(
      GTK_WINDOW(dialog),
      TRUE);

  GtkWidget* box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          12);

  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  GtkWidget* entry =
      gtk_entry_new();

  gtk_editable_set_text(
      GTK_EDITABLE(entry),
      layer->name().c_str());

  gtk_editable_select_region(
      GTK_EDITABLE(entry),
      0,
      -1);

  GtkWidget* buttons =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          6);

  gtk_widget_set_halign(
      buttons,
      GTK_ALIGN_END);

  GtkWidget* cancel =
      gtk_button_new_with_label(
          "Cancelar");

  GtkWidget* apply =
      gtk_button_new_with_label(
          "Cambiar nombre");

  gtk_widget_add_css_class(
      apply,
      "suggested-action");

  gtk_box_append(
      GTK_BOX(buttons),
      cancel);

  gtk_box_append(
      GTK_BOX(buttons),
      apply);

  gtk_box_append(
      GTK_BOX(box),
      entry);

  gtk_box_append(
      GTK_BOX(box),
      buttons);

  gtk_window_set_child(
      GTK_WINDOW(dialog),
      box);

  g_signal_connect_swapped(
      cancel,
      "clicked",
      G_CALLBACK(gtk_window_destroy),
      dialog);

  auto* rename_context =
      new RenameDialogContext{
          state,
          entry,
          GTK_WINDOW(dialog),
          *active};

  g_signal_connect_data(
      apply,
      "clicked",
      G_CALLBACK(apply_rename_clicked),
      rename_context,
      destroy_rename_context,
      GConnectFlags(0));

  gtk_window_present(
      GTK_WINDOW(dialog));
}

void add_group_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  auto& document =
      *state->document;

  patchy::Layer group(
      document.allocate_layer_id(),
      "Carpeta",
      patchy::LayerKind::Group);

  group.set_blend_mode(
      patchy::BlendMode::PassThrough);

  document.add_layer(
      std::move(group));

  refresh_after_layer_change(state);
}

void add_mask_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  auto* layer =
      state->document->find_layer(
          *active);

  if (
      layer == nullptr ||
      layer->mask().has_value()) {
    return;
  }

  if (
      layer->kind() != patchy::LayerKind::Pixel &&
      layer->kind() != patchy::LayerKind::Adjustment &&
      layer->kind() != patchy::LayerKind::Group) {
    return;
  }

  patchy::PixelBuffer pixels(
      state->document->width(),
      state->document->height(),
      patchy::PixelFormat::gray8());

  pixels.clear(255);

  layer->set_mask(
      patchy::LayerMask{
          patchy::Rect{
              0,
              0,
              state->document->width(),
              state->document->height()},
          std::move(pixels),
          255,
          false});

  refresh_after_layer_change(state);
}

void add_adjustment(
    InspectorState* state,
    patchy::AdjustmentKind kind) {
  auto& document =
      *state->document;

  patchy::AdjustmentSettings settings;

  settings.kind = kind;

  patchy::Layer layer(
      document.allocate_layer_id(),
      patchy::adjustment_display_name(kind),
      patchy::LayerKind::Adjustment);

  patchy::configure_adjustment_layer(
      layer,
      settings);

  document.add_layer(
      std::move(layer));

  refresh_after_layer_change(state);
}

void adjustment_selected(
    GtkButton* button,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  const auto kind =
      GPOINTER_TO_INT(
          g_object_get_data(
              G_OBJECT(button),
              "adjustment-kind"));

  add_adjustment(
      state,
      static_cast<patchy::AdjustmentKind>(
          kind));

  GtkWidget* popover =
      gtk_widget_get_ancestor(
          GTK_WIDGET(button),
          GTK_TYPE_POPOVER);

  if (popover != nullptr) {
    gtk_popover_popdown(
        GTK_POPOVER(popover));
  }
}

void show_adjustment_menu(
    GtkButton* button,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  GtkWidget* popover =
      gtk_popover_new();

  gtk_widget_set_parent(
      popover,
      GTK_WIDGET(button));

  gtk_popover_set_position(
      GTK_POPOVER(popover),
      GTK_POS_TOP);

  GtkWidget* box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          2);

  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);

  struct Entry {
    const char* name;
    patchy::AdjustmentKind kind;
  };

  constexpr Entry entries[] = {
      {"Niveles", patchy::AdjustmentKind::Levels},
      {"Curvas", patchy::AdjustmentKind::Curves},
      {"Tono/Saturación", patchy::AdjustmentKind::HueSaturation},
      {"Equilibrio de color", patchy::AdjustmentKind::ColorBalance},
      {"Invertir", patchy::AdjustmentKind::Invert},
      {"Posterizar", patchy::AdjustmentKind::Posterize},
      {"Umbral", patchy::AdjustmentKind::Threshold},
      {"Brillo/Contraste", patchy::AdjustmentKind::BrightnessContrast},
  };

  for (const auto& entry : entries) {
    GtkWidget* row =
        gtk_button_new_with_label(
            entry.name);

    gtk_widget_add_css_class(
        row,
        "flat");

    g_object_set_data(
        G_OBJECT(row),
        "adjustment-kind",
        GINT_TO_POINTER(
            static_cast<int>(
                entry.kind)));

    g_signal_connect(
        row,
        "clicked",
        G_CALLBACK(adjustment_selected),
        state);

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

void add_layer_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  patchy::PixelBuffer pixels(
      state->document->width(),
      state->document->height(),
      patchy::PixelFormat::rgba8());

  pixels.clear(0);

  const std::string name =
      "Capa " +
      std::to_string(
          state->document->layers().size() + 1);

  state->document->add_pixel_layer(
      name,
      std::move(pixels));

  rebuild_layers(state);

  if (state->canvas.refresh) {
    state->canvas.refresh();
  }
}

void remove_layer_clicked(
    GtkButton*,
    gpointer data) {
  auto* state =
      static_cast<InspectorState*>(data);

  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  if (
      state->document->remove_layer(
          *active)) {
    rebuild_layers(state);

    if (state->canvas.refresh) {
      state->canvas.refresh();
    }
  }
}

}  // namespace

GtkWidget* create_inspector(
    patchy::Document& document,
    const CanvasView& canvas) {
  auto* state =
      new InspectorState;

  state->document =
      &document;

  state->canvas =
      canvas;

  GtkWidget* root =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  gtk_widget_set_size_request(
      root,
      170,
      -1);

  GtkWidget* stack =
      gtk_stack_new();

  gtk_stack_set_transition_type(
      GTK_STACK(stack),
      GTK_STACK_TRANSITION_TYPE_CROSSFADE);

  GtkWidget* switcher =
      gtk_stack_switcher_new();

  gtk_stack_switcher_set_stack(
      GTK_STACK_SWITCHER(switcher),
      GTK_STACK(stack));

  gtk_widget_set_halign(
      switcher,
      GTK_ALIGN_CENTER);

  gtk_widget_set_margin_top(
      switcher,
      4);

  gtk_widget_set_margin_bottom(
      switcher,
      4);

  state->layers =
      GTK_LIST_BOX(
          gtk_list_box_new());

  gtk_list_box_set_selection_mode(
      state->layers,
      GTK_SELECTION_SINGLE);

  g_signal_connect(
      state->layers,
      "row-selected",
      G_CALLBACK(layer_selected),
      state);

  GtkWidget* layers_scroll =
      gtk_scrolled_window_new();

  gtk_widget_set_vexpand(
      layers_scroll,
      TRUE);

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(layers_scroll),
      GTK_WIDGET(state->layers));

  GtkWidget* layers_page =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  gtk_box_append(
      GTK_BOX(layers_page),
      layers_scroll);

  GtkWidget* layer_actions =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          4);

  gtk_widget_set_margin_top(
      layer_actions,
      4);

  gtk_widget_set_margin_bottom(
      layer_actions,
      4);

  gtk_widget_set_margin_start(
      layer_actions,
      4);

  gtk_widget_set_margin_end(
      layer_actions,
      4);

  GtkWidget* rename =
      gtk_button_new_from_icon_name(
          "document-edit-symbolic");

  gtk_widget_set_tooltip_text(
      rename,
      "Cambiar nombre");

  GtkWidget* folder =
      gtk_button_new_from_icon_name(
          "folder-new-symbolic");

  gtk_widget_set_tooltip_text(
      folder,
      "Nueva carpeta de capas");

  GtkWidget* adjustment =
      gtk_button_new_from_icon_name(
          "image-adjust-color-symbolic");

  gtk_widget_set_tooltip_text(
      adjustment,
      "Nueva capa de ajuste");

  GtkWidget* mask =
      gtk_button_new_from_icon_name(
          "view-reveal-symbolic");

  gtk_widget_set_tooltip_text(
      mask,
      "Añadir máscara de capa");

  GtkWidget* add =
      gtk_button_new_from_icon_name(
          "list-add-symbolic");

  gtk_widget_set_tooltip_text(
      add,
      "Nueva capa");

  GtkWidget* remove =
      gtk_button_new_from_icon_name(
          "user-trash-symbolic");

  gtk_widget_set_tooltip_text(
      remove,
      "Eliminar capa");

  for (GtkWidget* button : {
           rename,
           folder,
           adjustment,
           mask,
           add,
           remove}) {
    gtk_widget_add_css_class(
        button,
        "flat");

    gtk_widget_set_size_request(
        button,
        28,
        28);
  }

  gtk_box_append(
      GTK_BOX(layer_actions),
      rename);

  gtk_box_append(
      GTK_BOX(layer_actions),
      folder);

  gtk_box_append(
      GTK_BOX(layer_actions),
      adjustment);

  gtk_box_append(
      GTK_BOX(layer_actions),
      mask);

  gtk_box_append(
      GTK_BOX(layer_actions),
      add);

  GtkWidget* spacer =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          0);

  gtk_widget_set_hexpand(
      spacer,
      TRUE);

  gtk_box_append(
      GTK_BOX(layer_actions),
      spacer);

  gtk_box_append(
      GTK_BOX(layer_actions),
      remove);

  gtk_box_append(
      GTK_BOX(layers_page),
      layer_actions);

  state->channels =
      GTK_LIST_BOX(
          gtk_list_box_new());

  GtkWidget* channels_scroll =
      gtk_scrolled_window_new();

  gtk_widget_set_vexpand(
      channels_scroll,
      TRUE);

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(channels_scroll),
      GTK_WIDGET(state->channels));

  GtkWidget* channels_page =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  gtk_box_append(
      GTK_BOX(channels_page),
      channels_scroll);

  GtkWidget* channel_actions =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          4);

  gtk_widget_set_margin_top(
      channel_actions,
      4);

  gtk_widget_set_margin_bottom(
      channel_actions,
      4);

  gtk_widget_set_margin_start(
      channel_actions,
      4);

  gtk_widget_set_margin_end(
      channel_actions,
      4);

  GtkWidget* add_channel =
      gtk_button_new_from_icon_name(
          "list-add-symbolic");

  gtk_widget_set_tooltip_text(
      add_channel,
      "Nuevo canal alfa");

  GtkWidget* channel_spacer =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          0);

  gtk_widget_set_hexpand(
      channel_spacer,
      TRUE);

  GtkWidget* delete_channel =
      gtk_button_new_from_icon_name(
          "user-trash-symbolic");

  gtk_widget_set_tooltip_text(
      delete_channel,
      "Eliminar canal");

  gtk_widget_add_css_class(
      add_channel,
      "flat");

  gtk_widget_add_css_class(
      delete_channel,
      "flat");

  gtk_box_append(
      GTK_BOX(channel_actions),
      add_channel);

  gtk_box_append(
      GTK_BOX(channel_actions),
      channel_spacer);

  gtk_box_append(
      GTK_BOX(channel_actions),
      delete_channel);

  gtk_box_append(
      GTK_BOX(channels_page),
      channel_actions);

  state->paths =
      GTK_LIST_BOX(
          gtk_list_box_new());

  GtkWidget* paths_scroll =
      gtk_scrolled_window_new();

  gtk_widget_set_vexpand(
      paths_scroll,
      TRUE);

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(paths_scroll),
      GTK_WIDGET(state->paths));

  GtkWidget* history_page =
      gtk_list_box_new();

  GtkWidget* history_initial =
      gtk_label_new(
          "Documento abierto");

  gtk_widget_set_margin_top(
      history_initial,
      10);

  gtk_widget_set_margin_bottom(
      history_initial,
      10);

  gtk_list_box_append(
      GTK_LIST_BOX(history_page),
      history_initial);

  GtkWidget* properties_page =
      adw_preferences_group_new();

  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(properties_page),
      "Propiedades de la capa activa");

  GtkWidget* opacity =
      adw_spin_row_new_with_range(
          0,
          100,
          1);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(opacity),
      "Opacidad");

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(properties_page),
      opacity);

  GtkWidget* fill_opacity =
      adw_spin_row_new_with_range(
          0,
          100,
          1);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(fill_opacity),
      "Opacidad de relleno");

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(properties_page),
      fill_opacity);

  GtkWidget* info_page =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          8);

  gtk_widget_set_margin_top(
      info_page,
      12);

  gtk_widget_set_margin_start(
      info_page,
      12);

  char info[256];

  g_snprintf(
      info,
      sizeof(info),
      "%d × %d px\n%.0f ppp\n%zu capas\n%zu canales\n%zu trazados",
      document.width(),
      document.height(),
      document.print_settings().horizontal_ppi,
      document.layers().size(),
      document.channels().size(),
      document.paths().size());

  GtkWidget* info_label =
      gtk_label_new(info);

  gtk_label_set_xalign(
      GTK_LABEL(info_label),
      0.0F);

  gtk_box_append(
      GTK_BOX(info_page),
      info_label);

  GtkWidget* palette_page =
      gtk_flow_box_new();

  gtk_flow_box_set_selection_mode(
      GTK_FLOW_BOX(palette_page),
      GTK_SELECTION_NONE);

  const auto add_palette =
      [palette_page](const auto& colors) {
        for (const auto& color : colors) {
          GtkWidget* swatch =
              gtk_drawing_area_new();

          gtk_widget_set_size_request(
              swatch,
              32,
              32);

          auto* stored =
              new patchy::RgbColor(color);

          gtk_drawing_area_set_draw_func(
              GTK_DRAWING_AREA(swatch),
              [](
                  GtkDrawingArea*,
                  cairo_t* cr,
                  int width,
                  int height,
                  gpointer data) {
                const auto* color =
                    static_cast<
                        patchy::RgbColor*>(
                            data);

                cairo_set_source_rgb(
                    cr,
                    color->red / 255.0,
                    color->green / 255.0,
                    color->blue / 255.0);

                cairo_rectangle(
                    cr,
                    0,
                    0,
                    width,
                    height);

                cairo_fill(cr);
              },
              stored,
              [](gpointer data) {
                delete static_cast<
                    patchy::RgbColor*>(
                        data);
              });

          gtk_flow_box_append(
              GTK_FLOW_BOX(palette_page),
              swatch);
        }
      };

  if (document.palette_editing().has_value()) {
    add_palette(
        document.palette_editing()->palette.colors);
  } else if (document.indexed_palette().has_value()) {
    add_palette(
        document.indexed_palette()->colors);
  }

  gtk_stack_add_titled(
      GTK_STACK(stack),
      layers_page,
      "layers",
      "Capas");

  gtk_stack_add_titled(
      GTK_STACK(stack),
      channels_page,
      "channels",
      "Canales");

  gtk_stack_add_titled(
      GTK_STACK(stack),
      paths_scroll,
      "paths",
      "Trazados");


  gtk_box_append(
      GTK_BOX(root),
      switcher);

  gtk_box_append(
      GTK_BOX(root),
      gtk_separator_new(
          GTK_ORIENTATION_HORIZONTAL));

  gtk_widget_set_vexpand(
      stack,
      TRUE);

  gtk_box_append(
      GTK_BOX(root),
      stack);

  const auto add_collapsible_panel =
      [root](
          const char* title,
          GtkWidget* child) {
        GtkWidget* expander =
            gtk_expander_new(title);

        gtk_expander_set_child(
            GTK_EXPANDER(expander),
            child);

        gtk_expander_set_expanded(
            GTK_EXPANDER(expander),
            FALSE);

        gtk_widget_add_css_class(
            expander,
            "lienzo-inspector-section");

        gtk_box_append(
            GTK_BOX(root),
            expander);
      };

  add_collapsible_panel(
      "Historia",
      history_page);

  add_collapsible_panel(
      "Propiedades",
      properties_page);

  add_collapsible_panel(
      "Información",
      info_page);

  add_collapsible_panel(
      "Paleta",
      palette_page);

  g_signal_connect(
      rename,
      "clicked",
      G_CALLBACK(rename_layer_clicked),
      state);

  g_signal_connect(
      folder,
      "clicked",
      G_CALLBACK(add_group_clicked),
      state);

  g_signal_connect(
      adjustment,
      "clicked",
      G_CALLBACK(show_adjustment_menu),
      state);

  g_signal_connect(
      mask,
      "clicked",
      G_CALLBACK(add_mask_clicked),
      state);

  g_signal_connect(
      add,
      "clicked",
      G_CALLBACK(add_layer_clicked),
      state);

  g_signal_connect(
      remove,
      "clicked",
      G_CALLBACK(remove_layer_clicked),
      state);

  g_object_set_data_full(
      G_OBJECT(root),
      "lienzo-inspector-state",
      state,
      [](gpointer data) {
        delete static_cast<InspectorState*>(
            data);
      });

  rebuild_layers(state);
  rebuild_channels(state);
  rebuild_paths(state);

  return root;
}

void refresh_inspector(
    GtkWidget* inspector) {
  if (inspector == nullptr) {
    return;
  }

  auto* state =
      static_cast<InspectorState*>(
          g_object_get_data(
              G_OBJECT(inspector),
              "lienzo-inspector-state"));

  if (state == nullptr) {
    return;
  }

  rebuild_layers(state);
  rebuild_channels(state);
  rebuild_paths(state);
}

}  // namespace lienzo::gnome
