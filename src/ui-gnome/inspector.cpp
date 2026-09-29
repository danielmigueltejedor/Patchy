#include "ui-gnome/inspector.hpp"

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
        6);

    gtk_widget_set_margin_bottom(
        content,
        6);

    gtk_widget_set_margin_start(
        content,
        8 + depth * 14);

    gtk_widget_set_margin_end(
        content,
        8);

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
        gtk_image_new_from_icon_name(
            layer.kind() == patchy::LayerKind::Group
                ? "folder-symbolic"
                : "image-x-generic-symbolic");

    GtkWidget* name =
        gtk_label_new(
            layer.name().c_str());

    gtk_label_set_xalign(
        GTK_LABEL(name),
        0.0F);

    gtk_widget_set_hexpand(
        name,
        TRUE);

    gtk_box_append(
        GTK_BOX(content),
        visible);

    gtk_box_append(
        GTK_BOX(content),
        icon);

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

void rebuild_channels(
    InspectorState* state) {
  clear_list(
      state->channels);

  GtkWidget* composite =
      gtk_label_new(
          "RGB compuesto");

  gtk_widget_set_margin_top(
      composite,
      8);

  gtk_widget_set_margin_bottom(
      composite,
      8);

  gtk_widget_set_margin_start(
      composite,
      10);

  gtk_widget_set_halign(
      composite,
      GTK_ALIGN_START);

  gtk_list_box_append(
      state->channels,
      composite);

  for (const auto& channel :
       std::as_const(
           *state->document).channels()) {
    GtkWidget* label =
        gtk_label_new(
            channel.name().c_str());

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
        state->channels,
        label);
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
      280,
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
      6);

  gtk_widget_set_margin_bottom(
      switcher,
      6);

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
      6);

  gtk_widget_set_margin_bottom(
      layer_actions,
      6);

  gtk_widget_set_margin_start(
      layer_actions,
      6);

  gtk_widget_set_margin_end(
      layer_actions,
      6);

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

  gtk_box_append(
      GTK_BOX(layer_actions),
      add);

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

  gtk_stack_add_titled(
      GTK_STACK(stack),
      layers_page,
      "layers",
      "Capas");

  gtk_stack_add_titled(
      GTK_STACK(stack),
      channels_scroll,
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

  gtk_box_append(
      GTK_BOX(root),
      stack);

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

}  // namespace lienzo::gnome
