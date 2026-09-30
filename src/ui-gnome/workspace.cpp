#include "ui-gnome/workspace.hpp"

#include "ui-gnome/canvas.hpp"
#include "ui-gnome/inspector.hpp"
#include "ui-gnome/tool_options_bar.hpp"

#include <utility>

namespace lienzo::gnome {

namespace {

struct WorkspaceKeys {
  CanvasView canvas;
};

gboolean workspace_key_pressed(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType modifiers,
    gpointer data) {
  auto* state =
      static_cast<WorkspaceKeys*>(data);

  const bool ctrl =
      (modifiers &
       GDK_CONTROL_MASK) != 0;

  const bool shift =
      (modifiers &
       GDK_SHIFT_MASK) != 0;

  if (ctrl) {
    if (
        keyval == GDK_KEY_a ||
        keyval == GDK_KEY_A) {
      if (state->canvas.select_all)
        state->canvas.select_all();

      return TRUE;
    }

    if (
        keyval == GDK_KEY_d ||
        keyval == GDK_KEY_D) {
      if (state->canvas.deselect)
        state->canvas.deselect();

      return TRUE;
    }

    if (
        shift &&
        (keyval == GDK_KEY_i ||
         keyval == GDK_KEY_I)) {
      if (state->canvas.invert_selection)
        state->canvas.invert_selection();

      return TRUE;
    }

    if (
        keyval == GDK_KEY_z ||
        keyval == GDK_KEY_Z) {
      if (shift) {
        if (state->canvas.redo)
          state->canvas.redo();
      } else {
        if (state->canvas.undo)
          state->canvas.undo();
      }

      return TRUE;
    }

    if (
        keyval == GDK_KEY_y ||
        keyval == GDK_KEY_Y) {
      if (state->canvas.redo)
        state->canvas.redo();

      return TRUE;
    }

    if (
        keyval == GDK_KEY_c ||
        keyval == GDK_KEY_C) {
      if (state->canvas.copy_active)
        state->canvas.copy_active();

      return TRUE;
    }

    if (
        keyval == GDK_KEY_x ||
        keyval == GDK_KEY_X) {
      if (state->canvas.cut_active)
        state->canvas.cut_active();

      return TRUE;
    }

    if (
        keyval == GDK_KEY_v ||
        keyval == GDK_KEY_V) {
      if (state->canvas.paste)
        state->canvas.paste();

      return TRUE;
    }

    GtkRoot* root =
        gtk_widget_get_root(
            state->canvas.widget);

    if (
        root != nullptr &&
        G_IS_ACTION_GROUP(root)) {
      if (
          keyval == GDK_KEY_s ||
          keyval == GDK_KEY_S) {
        g_action_group_activate_action(
            G_ACTION_GROUP(root),
            shift
                ? "save-as"
                : "save",
            nullptr);

        return TRUE;
      }

      if (
          keyval == GDK_KEY_n ||
          keyval == GDK_KEY_N) {
        g_action_group_activate_action(
            G_ACTION_GROUP(root),
            "new",
            nullptr);

        return TRUE;
      }

      if (
          keyval == GDK_KEY_o ||
          keyval == GDK_KEY_O) {
        g_action_group_activate_action(
            G_ACTION_GROUP(root),
            "open",
            nullptr);

        return TRUE;
      }
    }
  }

  if (
      keyval == GDK_KEY_Return ||
      keyval == GDK_KEY_KP_Enter) {
    if (state->canvas.commit_crop) {
      state->canvas.commit_crop();
      return TRUE;
    }
  }

  if (keyval == GDK_KEY_Escape) {
    if (state->canvas.cancel_crop) {
      state->canvas.cancel_crop();
      return TRUE;
    }
  }

  return FALSE;
}

}  // namespace

GtkWidget* create_workspace(
    const patchy::Document& document,
    Tool current_tool,
    ToolSelectedCallback tool_selected) {
  auto& mutable_document =
      const_cast<patchy::Document&>(
          document);

  CanvasView canvas =
      create_canvas_view(
          mutable_document,
          current_tool);

  ToolOptionsBar options =
      create_tool_options_bar(
          canvas);

  GtkWidget* root =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  auto* keys_state =
      new WorkspaceKeys{
          canvas};

  g_object_set_data_full(
      G_OBJECT(root),
      "lienzo-workspace-keys",
      keys_state,
      [](gpointer data) {
        delete static_cast<WorkspaceKeys*>(
            data);
      });

  GtkEventController* keys =
      gtk_event_controller_key_new();

  gtk_event_controller_set_propagation_phase(
      keys,
      GTK_PHASE_CAPTURE);

  g_signal_connect(
      keys,
      "key-pressed",
      G_CALLBACK(workspace_key_pressed),
      keys_state);

  gtk_widget_add_controller(
      root,
      keys);

  // Barra contextual horizontal desplazable en ventanas estrechas.
  GtkWidget* options_scroll =
      gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(options_scroll),
      GTK_POLICY_AUTOMATIC,
      GTK_POLICY_NEVER);

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(options_scroll),
      options.widget);

  gtk_box_append(
      GTK_BOX(root),
      options_scroll);

  gtk_box_append(
      GTK_BOX(root),
      gtk_separator_new(
          GTK_ORIENTATION_HORIZONTAL));

  GtkWidget* paned =
      gtk_paned_new(
          GTK_ORIENTATION_HORIZONTAL);

  gtk_widget_set_vexpand(
      paned,
      TRUE);

  gtk_paned_set_resize_start_child(
      GTK_PANED(paned),
      TRUE);

  gtk_paned_set_shrink_start_child(
      GTK_PANED(paned),
      TRUE);

  gtk_paned_set_resize_end_child(
      GTK_PANED(paned),
      FALSE);

  gtk_paned_set_shrink_end_child(
      GTK_PANED(paned),
      TRUE);

  // Zona editor: toolbar vertical + canvas.
  GtkWidget* editor =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          0);

  GtkWidget* palette_scroll =
      gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(palette_scroll),
      GTK_POLICY_NEVER,
      GTK_POLICY_AUTOMATIC);

  GtkWidget* palette =
      create_tool_palette(
          current_tool,
          [canvas, options, tool_selected](
              Tool tool) mutable {
            if (canvas.set_tool)
              canvas.set_tool(tool);

            if (options.set_tool)
              options.set_tool(tool);

            if (tool_selected)
              tool_selected(tool);
          },
          ToolPaletteControls{
              canvas.foreground_color,
              canvas.background_color,
              canvas.set_foreground_color,
              canvas.set_background_color,
              canvas.reset_colors,
              canvas.swap_colors,
              canvas.quick_mask_enabled,
              canvas.set_quick_mask});

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(palette_scroll),
      palette);

  gtk_box_append(
      GTK_BOX(editor),
      palette_scroll);

  gtk_box_append(
      GTK_BOX(editor),
      gtk_separator_new(
          GTK_ORIENTATION_VERTICAL));

  gtk_box_append(
      GTK_BOX(editor),
      canvas.widget);

  gtk_widget_set_hexpand(
      canvas.widget,
      TRUE);

  gtk_widget_set_vexpand(
      canvas.widget,
      TRUE);

  GtkWidget* inspector =
      create_inspector(
          mutable_document,
          canvas);

  // El inspector puede bajar hasta ~170px.
  gtk_widget_set_size_request(
      inspector,
      170,
      -1);

  GtkWidget* inspector_scroll =
      gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(inspector_scroll),
      GTK_POLICY_NEVER,
      GTK_POLICY_AUTOMATIC);

  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(inspector_scroll),
      inspector);

  if (canvas.set_document_changed_callback) {
    canvas.set_document_changed_callback(
        [inspector] {
          refresh_inspector(
              inspector);
        });
  }

  gtk_paned_set_start_child(
      GTK_PANED(paned),
      editor);

  gtk_paned_set_end_child(
      GTK_PANED(paned),
      inspector_scroll);

  gtk_box_append(
      GTK_BOX(root),
      paned);

  return root;
}

}  // namespace lienzo::gnome
