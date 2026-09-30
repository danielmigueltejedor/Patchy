#pragma once

#include "core/pixel_tools.hpp"

#include <gtk/gtk.h>

#include <functional>

namespace lienzo::gnome {

enum class Tool {
  Move,

  Marquee,
  EllipticalMarquee,

  Lasso,
  MagneticLasso,

  MagicWand,
  QuickSelect,

  Crop,

  Brush,
  MixerBrush,
  Eraser,

  Gradient,
  Fill,

  Clone,
  PatternStamp,

  Healing,
  SpotHealing,
  PatchTool,

  Smudge,
  BlurBrush,
  SharpenBrush,

  Dodge,
  Burn,
  Sponge,

  Pen,
  AddAnchor,
  DeleteAnchor,
  ConvertPoint,

  PathSelect,
  DirectSelect,

  Line,
  Rectangle,
  Ellipse,
  Polygon,
  CustomShape,

  Text,

  Eyedropper,
  Hand,
  Zoom
};

using ToolSelectedCallback =
    std::function<void(Tool)>;

struct ToolPaletteControls {
  std::function<patchy::EditColor()>
      foreground_color;

  std::function<patchy::EditColor()>
      background_color;

  std::function<void(patchy::EditColor)>
      set_foreground_color;

  std::function<void(patchy::EditColor)>
      set_background_color;

  std::function<void()> reset_colors;
  std::function<void()> swap_colors;

  std::function<bool()> quick_mask_enabled;
  std::function<void(bool)> set_quick_mask;
};

GtkWidget* create_tool_palette(
    Tool initial_tool,
    ToolSelectedCallback callback,
    ToolPaletteControls controls = {});

const char* tool_name(
    Tool tool);

}  // namespace lienzo::gnome
