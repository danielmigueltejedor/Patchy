#pragma once

#include "core/document.hpp"
#include "ui-gnome/canvas.hpp"

#include <gtk/gtk.h>

namespace lienzo::gnome {

GtkWidget* create_inspector(
    patchy::Document& document,
    const CanvasView& canvas);

void refresh_inspector(
    GtkWidget* inspector);

}
