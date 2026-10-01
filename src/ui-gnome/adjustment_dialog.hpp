#pragma once

#include "core/document.hpp"
#include "ui-gnome/canvas.hpp"

#include <functional>

#include <gtk/gtk.h>

namespace lienzo::gnome {

void present_adjustment_editor(
    patchy::Document& document,
    patchy::LayerId id,
    const CanvasView& canvas,
    GtkWidget* parent,
    std::function<void()> after_apply);

}  // namespace lienzo::gnome
