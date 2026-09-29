#pragma once

#include "core/document.hpp"
#include "ui-gnome/tool_palette.hpp"

#include <gtk/gtk.h>

#include <functional>

namespace lienzo::gnome {

struct CanvasView {
  GtkWidget* widget{};
  std::function<void(Tool)> set_tool;
  std::function<void()> refresh;
};

CanvasView create_canvas_view(
    patchy::Document& document,
    Tool initial_tool);

}  // namespace lienzo::gnome
