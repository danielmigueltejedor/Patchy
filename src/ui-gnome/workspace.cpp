#include "ui-gnome/workspace.hpp"

#include "ui-gnome/canvas.hpp"

#include <utility>

namespace lienzo::gnome {

GtkWidget* create_workspace(
    const patchy::Document& document,
    Tool current_tool,
    ToolSelectedCallback tool_selected) {
  auto& mutable_document =
      const_cast<patchy::Document&>(
          document);

  GtkWidget* workspace =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          0);

  CanvasView canvas =
      create_canvas_view(
          mutable_document,
          current_tool);

  GtkWidget* palette =
      create_tool_palette(
          current_tool,
          [canvas, tool_selected](Tool tool) {
            canvas.set_tool(tool);

            if (tool_selected) {
              tool_selected(tool);
            }
          });

  gtk_box_append(
      GTK_BOX(workspace),
      palette);

  gtk_box_append(
      GTK_BOX(workspace),
      gtk_separator_new(
          GTK_ORIENTATION_VERTICAL));

  gtk_box_append(
      GTK_BOX(workspace),
      canvas.widget);

  return workspace;
}

}  // namespace lienzo::gnome
