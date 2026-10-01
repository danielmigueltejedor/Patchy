#pragma once

#include "core/layer.hpp"
#include "core/mask_outline.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lienzo::gnome {

enum class SelectionCombine {
  Replace,
  Add,
  Subtract,
  Intersect
};

struct SelectionPoint {
  std::int32_t x{};
  std::int32_t y{};
};

enum class SelectionDraftKind {
  None,
  Rectangle,
  Ellipse,
  Lasso
};

class SelectionController {
 public:
  void resize(
      std::int32_t width,
      std::int32_t height);

  [[nodiscard]] std::int32_t width() const noexcept;
  [[nodiscard]] std::int32_t height() const noexcept;

  [[nodiscard]] bool empty() const noexcept;

  [[nodiscard]] bool selected(
      std::int32_t x,
      std::int32_t y) const noexcept;

  [[nodiscard]] float coverage(
      std::int32_t x,
      std::int32_t y) const noexcept;

  [[nodiscard]] const std::vector<std::uint8_t>&
  mask() const noexcept;

  [[nodiscard]] const std::vector<SelectionPoint>&
  boundary() const noexcept;

  [[nodiscard]] const std::vector<patchy::MaskLoop>&
  outlines() const noexcept;

  [[nodiscard]] std::optional<patchy::Rect>
  bounds() const noexcept;

  void clear();
  void select_all();
  void invert();

  void begin_rectangle(
      std::int32_t x,
      std::int32_t y,
      bool ellipse,
      SelectionCombine combine);

  void update_rectangle(
      std::int32_t x,
      std::int32_t y);

  void begin_lasso(
      std::int32_t x,
      std::int32_t y,
      SelectionCombine combine);

  void append_lasso(
      std::int32_t x,
      std::int32_t y);

  void commit_draft();
  void cancel_draft();

  [[nodiscard]] SelectionDraftKind
  draft_kind() const noexcept;

  [[nodiscard]] patchy::Rect
  draft_rect() const noexcept;

  [[nodiscard]] const std::vector<SelectionPoint>&
  draft_points() const noexcept;

  void magic_wand_rgba(
      const std::uint8_t* rgba,
      std::ptrdiff_t stride,
      std::int32_t x,
      std::int32_t y,
      int tolerance,
      bool contiguous,
      SelectionCombine combine);

  // Quick Select matches the engine contract: the drag only stamps a brush
  // footprint. quick_select_segment() runs once, when the gesture ends.
  void begin_quick_select(
      std::int32_t x,
      std::int32_t y,
      int diameter,
      SelectionCombine combine);

  void extend_quick_select(
      std::int32_t x,
      std::int32_t y,
      int diameter);

  void finish_quick_select(
      const std::uint8_t* rgba,
      std::ptrdiff_t stride,
      int diameter,
      bool enhance_edge);

  void cancel_quick_select() noexcept;

  [[nodiscard]] bool quick_selecting() const noexcept;

  [[nodiscard]] const std::vector<SelectionPoint>&
  quick_select_stroke() const noexcept;

  void paint_mask_segment(
      double x0,
      double y0,
      double x1,
      double y1,
      double radius,
      bool add);

  void set_quick_mask(bool enabled) noexcept;
  void toggle_quick_mask() noexcept;

  [[nodiscard]] bool
  quick_mask() const noexcept;

 private:
  [[nodiscard]] std::size_t index(
      std::int32_t x,
      std::int32_t y) const noexcept;

  void apply_generated(
      const std::vector<std::uint8_t>& generated,
      SelectionCombine combine);

  void rebuild_cache();

  void stamp_quick_select_seed(
      std::int32_t x0,
      std::int32_t y0,
      std::int32_t x1,
      std::int32_t y1,
      int diameter);

  bool quick_selecting_{false};
  SelectionCombine quick_combine_{
      SelectionCombine::Replace};
  std::vector<std::uint8_t> quick_seed_;
  patchy::Rect quick_seed_bounds_{};
  std::vector<SelectionPoint> quick_stroke_;
  std::int32_t quick_last_x_{0};
  std::int32_t quick_last_y_{0};

  std::int32_t width_{0};
  std::int32_t height_{0};

  std::vector<std::uint8_t> mask_;
  std::vector<SelectionPoint> boundary_;
  std::vector<patchy::MaskLoop> outlines_;

  std::optional<patchy::Rect> bounds_;

  SelectionDraftKind draft_kind_{
      SelectionDraftKind::None};

  SelectionCombine draft_combine_{
      SelectionCombine::Replace};

  SelectionPoint draft_start_{};
  SelectionPoint draft_end_{};

  std::vector<SelectionPoint>
      draft_points_;

  bool quick_mask_{false};
};

}  // namespace lienzo::gnome
