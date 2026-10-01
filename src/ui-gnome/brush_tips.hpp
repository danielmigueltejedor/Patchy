#pragma once

#include "core/brush_tip.hpp"
#include "core/pixel_tools.hpp"

#include <vector>

namespace lienzo::gnome {

struct BuiltinBrushTip {
  const char* name{};
  bool procedural{false};
  patchy::BrushShape procedural_shape{
      patchy::BrushShape::Round};
  patchy::BrushTip tip{};
};

const std::vector<BuiltinBrushTip>&
builtin_brush_tips();

}
