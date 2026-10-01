#pragma once

#include "core/document.hpp"
#include "core/pixel_tools.hpp"
#include "core/retouch_brush.hpp"
#include "ui-gnome/tool_palette.hpp"
#include "ui-gnome/tools/text_controller.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
  std::function<void(bool)> set_fill_shapes;
  std::function<void(int)> set_polygon_sides;

  std::function<bool()> commit_crop;
  std::function<bool()> cancel_crop;

  std::function<bool()> commit_pen;
  std::function<void()> cancel_pen;

  std::function<void()> zoom_in;
  std::function<void()> zoom_out;
  std::function<void()> zoom_100;
  std::function<void()> zoom_fit;

  std::function<void(std::function<void()>)>
      set_document_changed_callback;

  std::function<void(std::function<void(
      const std::uint8_t*,
      int,
      int,
      int)>)>
      set_composite_callback;

  std::function<void()> publish_composite;

  std::function<void()> checkpoint;
  std::function<void()> undo;
  std::function<void()> redo;
  std::function<void()> copy_active;
  std::function<void()> cut_active;
  std::function<void()> paste;

  std::function<void(int)> set_brush_tip_index;

  std::function<void(patchy::GradientMethod)> set_gradient_method;
  std::function<void(int)> set_gradient_opacity;
  std::function<void(bool)> set_gradient_reverse;
  std::function<void(int)> set_flood_tolerance;
  std::function<void(bool)> set_flood_contiguous;
  std::function<void(int)> set_wand_tolerance;
  std::function<void(bool)> set_wand_contiguous;
  std::function<void(patchy::LocalToneRange)> set_tone_range;
  std::function<void(bool)> set_protect_tones;
  std::function<void(patchy::SpongeMode)> set_sponge_mode;
  std::function<void(int)> set_healing_diffusion;

  std::function<patchy::EditColor()> foreground_color;
  std::function<patchy::EditColor()> background_color;

  std::function<void(patchy::EditColor)>
      set_foreground_color;

  std::function<void(patchy::EditColor)>
      set_background_color;

  std::function<void()> reset_colors;
  std::function<void()> swap_colors;

  std::function<void()> select_all;
  std::function<void()> deselect;
  std::function<void()> invert_selection;

  std::function<bool()> quick_mask_enabled;
  std::function<void(bool)> set_quick_mask;
  std::function<void()> toggle_quick_mask;

  std::function<void(std::string)> set_text_family;
  std::function<void(int)> set_text_size;
  std::function<void(bool)> set_text_bold;
  std::function<void(bool)> set_text_italic;
  std::function<void(TextAlignment)> set_text_alignment;

  std::function<void()> commit_text;
  std::function<void()> cancel_text;
};

struct CanvasPreview {
  int width{0};
  int height{0};
  double scale_x{1.0};
  double scale_y{1.0};
  std::vector<std::uint8_t> rgba;
};

CanvasPreview build_canvas_preview(
    const patchy::Document& document);

CanvasView create_canvas_view(
    patchy::Document& document,
    Tool initial_tool,
    const CanvasPreview* prepared = nullptr);

}  // namespace lienzo::gnome
