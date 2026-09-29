#pragma once

#include "ui-gnome/canvas.hpp"
#include "ui-gnome/tool_palette.hpp"

#include <gtk/gtk.h>

#include <functional>

namespace lienzo::gnome {

struct ToolOptionsBar {
  GtkWidget* widget{};
  std::function<void(Tool)> set_tool;
};

ToolOptionsBar create_tool_options_bar(
    const CanvasView& canvas);

}
