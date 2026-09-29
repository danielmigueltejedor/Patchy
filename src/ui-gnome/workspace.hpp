#pragma once

#include "core/document.hpp"
#include "ui-gnome/tool_palette.hpp"

#include <gtk/gtk.h>

#include <functional>

namespace lienzo::gnome {

GtkWidget* create_workspace(
    const patchy::Document& document,
    Tool current_tool,
    ToolSelectedCallback tool_selected);

}  // namespace lienzo::gnome
