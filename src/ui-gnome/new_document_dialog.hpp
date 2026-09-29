#pragma once

#include <adwaita.h>

#include <cstdint>
#include <functional>

namespace lienzo::gnome {

enum class NewDocumentBackground {
  White,
  Black,
  Transparent
};

struct NewDocumentSettings {
  std::int32_t width{1024};
  std::int32_t height{768};
  double resolution_ppi{72.0};
  NewDocumentBackground background{NewDocumentBackground::White};
};

using NewDocumentCallback =
    std::function<void(const NewDocumentSettings&)>;

void present_new_document_dialog(
    GtkWidget* parent,
    NewDocumentCallback callback);

}  // namespace lienzo::gnome
