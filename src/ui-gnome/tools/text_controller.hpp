#pragma once

#include "core/document.hpp"
#include "core/pixel_tools.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

namespace lienzo::gnome {

enum class TextAlignment {
  Left,
  Center,
  Right
};

struct TextStyle {
  std::string family{"Sans"};
  int size{32};
  bool bold{false};
  bool italic{false};
  TextAlignment alignment{TextAlignment::Left};
  patchy::EditColor color{0, 0, 0, 255};
};

class TextController {
 public:
  TextController(
      patchy::Document& document,
      GtkOverlay* overlay,
      std::function<void()> before_commit,
      std::function<void()> after_commit);

  ~TextController();

  [[nodiscard]] bool active() const noexcept;

  void begin_point(
      int document_x,
      int document_y,
      double widget_x,
      double widget_y,
      double view_zoom,
      patchy::EditColor color);

  bool commit();
  void cancel();

  void set_family(std::string family);
  void set_size(int size);
  void set_bold(bool bold);
  void set_italic(bool italic);
  void set_alignment(TextAlignment alignment);
  void set_color(patchy::EditColor color);

  [[nodiscard]] const TextStyle& style() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lienzo::gnome
