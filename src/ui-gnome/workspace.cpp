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

  const bool control =
      (modifiers &
       GDK_CONTROL_MASK) != 0;

  const bool shift =
      (modifiers &
       GDK_SHIFT_MASK) != 0;

  if (control) {
    if (
        keyval == GDK_KEY_z ||
        keyval == GDK_KEY_Z) {
      if (shift) {
        if (state->canvas.redo) {
          state->canvas.redo();
        }
      } else {
        if (state->canvas.undo) {
          state->canvas.undo();
        }
      }

      return TRUE;
    }

    if (
        keyval == GDK_KEY_y ||
        keyval == GDK_KEY_Y) {
      if (state->canvas.redo) {
        state->canvas.redo();
      }

      return TRUE;
    }

    if (
        keyval == GDK_KEY_c ||
        keyval == GDK_KEY_C) {
      if (state->canvas.copy_active) {
        state->canvas.copy_active();
      }

      return TRUE;
    }

    if (
        keyval == GDK_KEY_x ||
        keyval == GDK_KEY_X) {
      if (state->canvas.cut_active) {
        state->canvas.cut_active();
      }

      return TRUE;
    }

    if (
        keyval == GDK_KEY_v ||
        keyval == GDK_KEY_V) {
      if (state->canvas.paste) {
        state->canvas.paste();
      }

      return TRUE;
    }

    if (
        keyval == GDK_KEY_n ||
        keyval == GDK_KEY_N ||
        keyval == GDK_KEY_o ||
        keyval == GDK_KEY_O) {
      GtkRoot* root =
          gtk_widget_get_root(
              state->canvas.widget);

      if (
          root != nullptr &&
          G_IS_ACTION_GROUP(root)) {
        g_action_group_activate_action(
            G_ACTION_GROUP(root),
            keyval == GDK_KEY_n ||
                    keyval == GDK_KEY_N
                ? "new"
                : "open",
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
        delete static_cast<WorkspaceKeys*>(data);
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

  gtk_box_append(
      GTK_BOX(root),
      options.widget);

  gtk_box_append(
      GTK_BOX(root),
      gtk_separator_new(
          GTK_ORIENTATION_HORIZONTAL));

  GtkWidget* editor =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          0);

  GtkWidget* palette =
      create_tool_palette(
          current_tool,
          [canvas, options, tool_selected](
              Tool tool) mutable {
            if (canvas.set_tool) {
              canvas.set_tool(tool);
            }

            if (options.set_tool) {
              options.set_tool(tool);
            }

            if (tool_selected) {
              tool_selected(tool);
            }
          });

  gtk_box_append(
      GTK_BOX(editor),
      palette);

  gtk_box_append(
      GTK_BOX(editor),
      gtk_separator_new(
          GTK_ORIENTATION_VERTICAL));

  gtk_box_append(
      GTK_BOX(editor),
      canvas.widget);

  gtk_box_append(
      GTK_BOX(editor),
      gtk_separator_new(
          GTK_ORIENTATION_VERTICAL));

  GtkWidget* inspector =
      create_inspector(
          mutable_document,
          canvas);

  if (canvas.set_document_changed_callback) {
    canvas.set_document_changed_callback(
        [inspector] {
          refresh_inspector(
              inspector);
        });
  }

  gtk_box_append(
      GTK_BOX(editor),
      inspector);

  gtk_widget_set_hexpand(
      canvas.widget,
      TRUE);

  gtk_widget_set_vexpand(
      canvas.widget,
      TRUE);

  gtk_widget_set_vexpand(
      editor,
      TRUE);

  gtk_box_append(
      GTK_BOX(root),
      editor);

  return root;
}

}  // namespace lienzo::gnome
