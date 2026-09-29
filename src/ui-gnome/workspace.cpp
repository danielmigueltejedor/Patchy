#include "ui-gnome/workspace.hpp"

#include "ui-gnome/canvas.hpp"
#include "ui-gnome/inspector.hpp"
#include "ui-gnome/tool_options_bar.hpp"

#include <utility>

namespace lienzo::gnome {

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
