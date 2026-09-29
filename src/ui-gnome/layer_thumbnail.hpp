#pragma once

#include "core/layer.hpp"

#include <gtk/gtk.h>

namespace lienzo::gnome {

GtkWidget* create_layer_thumbnail(
    const patchy::Layer& layer,
    int document_width,
    int document_height);

GtkWidget* create_mask_thumbnail(
    const patchy::LayerMask& mask);

}
