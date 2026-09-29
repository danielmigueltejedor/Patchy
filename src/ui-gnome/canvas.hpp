#pragma once

#include "core/document.hpp"
#include "core/pixel_tools.hpp"
#include "ui-gnome/tool_palette.hpp"

#include <gtk/gtk.h>

#include <functional>

namespace lienzo::gnome {

struct CanvasView {
  GtkWidget* widget{};

  std::function<void(Tool)> set_tool;
  std::function<void()> refresh;

  std::function<void()> reset_brush_options;
  std::function<void(int)> set_brush_size;
  std::function<void(int)> set_brush_opacity;
  std::function<void(int)> set_brush_softness;
  std::function<void(int)> set_brush_flow;
  std::function<void(bool)> set_airbrush;
  std::function<void(int)> set_smoothing;
  std::function<void(patchy::BrushShape)> set_brush_shape;

  std::function<void()> commit_crop;
  std::function<void()> cancel_crop;

  std::function<void(std::function<void()>)>
      set_document_changed_callback;

  std::function<void()> checkpoint;
  std::function<void()> undo;
  std::function<void()> redo;
  std::function<void()> copy_active;
  std::function<void()> cut_active;
  std::function<void()> paste;

  std::function<void(int)> set_brush_tip_index;
};

CanvasView create_canvas_view(
    patchy::Document& document,
    Tool initial_tool);

}  // namespace lienzo::gnome
