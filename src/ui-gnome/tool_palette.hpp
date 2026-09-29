#pragma once

#include <gtk/gtk.h>

#include <functional>

namespace lienzo::gnome {

enum class Tool {
  Move,
  Marquee,
  Lasso,
  MagicWand,
  Crop,

  Brush,
  Eraser,
  Gradient,

  Clone,
  Healing,
  Smudge,
  Dodge,

  Pen,
  PathSelect,
  Shape,
  Text,

  Eyedropper,
  Hand,
  Zoom
};

using ToolSelectedCallback =
    std::function<void(Tool)>;

GtkWidget* create_tool_palette(
    Tool initial_tool,
    ToolSelectedCallback callback);

const char* tool_name(Tool tool);

}  // namespace lienzo::gnome
