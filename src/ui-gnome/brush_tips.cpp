#include "ui-gnome/brush_tips.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lienzo::gnome {

namespace {

constexpr int kTipSize = 64;

patchy::BrushTip make_tip(
    double spacing,
    const auto& coverage) {
  patchy::BrushTip tip;

  tip.width = kTipSize;
  tip.height = kTipSize;
  tip.default_spacing = spacing;

  tip.mask.resize(
      static_cast<std::size_t>(kTipSize) *
      static_cast<std::size_t>(kTipSize));

  for (int y = 0; y < kTipSize; ++y) {
    for (int x = 0; x < kTipSize; ++x) {
      const double nx =
          (x + 0.5 - kTipSize / 2.0) /
          (kTipSize / 2.0);

      const double ny =
          (y + 0.5 - kTipSize / 2.0) /
          (kTipSize / 2.0);

      const double value =
          std::clamp(
              coverage(nx, ny, x, y),
              0.0,
              1.0);

      tip.mask[
          static_cast<std::size_t>(y) *
              kTipSize +
          static_cast<std::size_t>(x)] =
          static_cast<std::uint8_t>(
              std::lround(
                  value * 255.0));
    }
  }

  return tip;
}

double hash_noise(
    int x,
    int y) {
  std::uint32_t value =
      static_cast<std::uint32_t>(
          x * 374761393u +
          y * 668265263u);

  value =
      (value ^ (value >> 13)) *
      1274126177u;

  return
      static_cast<double>(
          value & 0xffffu) /
      65535.0;
}

}  // namespace

const std::vector<BuiltinBrushTip>&
builtin_brush_tips() {
  static const std::vector<BuiltinBrushTip> tips = {
      {
          "Redonda dura",
          true,
          patchy::BrushShape::Round,
          {}},
      {
          "Cuadrada",
          true,
          patchy::BrushShape::Square,
          {}},
      {
          "Redonda suave",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.12,
              [](double x, double y, int, int) {
                const double d =
                    std::sqrt(x * x + y * y);

                return
                    std::clamp(
                        1.0 - d,
                        0.0,
                        1.0);
              })},
      {
          "Lápiz",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.10,
              [](double x, double y, int, int) {
                return
                    x * x + y * y <= 0.72
                        ? 1.0
                        : 0.0;
              })},
      {
          "Rotulador",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.12,
              [](double x, double y, int, int) {
                const double yy =
                    y * 1.8;

                return
                    x * x + yy * yy <= 0.9
                        ? 1.0
                        : 0.0;
              })},
      {
          "Caligrafía",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.10,
              [](double x, double y, int, int) {
                constexpr double c =
                    0.70710678118;

                const double rx =
                    c * x + c * y;

                const double ry =
                    -c * x + c * y;

                return
                    rx * rx +
                                ry * ry * 9.0 <=
                            0.9
                        ? 1.0
                        : 0.0;
              })},
      {
          "Tiza",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.18,
              [](double x, double y, int px, int py) {
                const double d =
                    std::sqrt(x * x + y * y);

                if (d > 1.0) {
                  return 0.0;
                }

                return
                    hash_noise(px, py) >
                            0.26
                        ? 0.85
                        : 0.05;
              })},
      {
          "Carboncillo",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.16,
              [](double x, double y, int px, int py) {
                const double d =
                    std::sqrt(
                        x * x +
                        y * y * 1.6);

                if (d > 1.0) {
                  return 0.0;
                }

                return
                    0.25 +
                    hash_noise(
                        px * 3,
                        py * 5) *
                        0.75;
              })},
      {
          "Spray",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.08,
              [](double x, double y, int px, int py) {
                const double d =
                    std::sqrt(x * x + y * y);

                if (d > 1.0) {
                  return 0.0;
                }

                return
                    hash_noise(
                        px * 17,
                        py * 23) >
                            0.72
                        ? 1.0
                        : 0.0;
              })},
      {
          "Cerdas",
          false,
          patchy::BrushShape::Round,
          make_tip(
              0.12,
              [](double x, double y, int px, int) {
                if (
                    std::abs(x) > 0.95 ||
                    std::abs(y) > 0.92) {
                  return 0.0;
                }

                const double stripe =
                    std::fmod(
                        std::abs(px * 0.37),
                        3.0);

                return
                    stripe < 1.15
                        ? 1.0
                        : 0.12;
              })},
  };

  return tips;
}

}  // namespace lienzo::gnome
