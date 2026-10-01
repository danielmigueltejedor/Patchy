#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace lienzo::gnome {

enum class ExportKind {
  Png,
  Jpeg,
  Webp,
  Bmp,
  Pcx,
  Psd,
  Psb,
  Pxd
};

struct ExportSettings {
  ExportKind kind{ExportKind::Png};
  int quality{90};
  bool lossless{false};
  std::string extension{".png"};
  bool layered{false};
};

using ExportChosen =
    std::function<void(ExportSettings)>;

void present_export_dialog(
    GtkWidget* parent,
    const std::string& suggested_stem,
    ExportChosen chosen);

}  // namespace lienzo::gnome
