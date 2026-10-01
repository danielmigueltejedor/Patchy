#pragma once

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lienzo::gnome {

struct PortalFilter {
  std::string name;
  std::vector<std::string> globs;
};

using PortalChosen =
    std::function<void(std::optional<std::filesystem::path>)>;

// GNOME Files chooser through the desktop portal (Nautilus on GNOME).
// If the portal is not running, the GTK file dialog is used instead.
void present_portal_open(
    GtkWindow* window,
    const char* title,
    std::vector<PortalFilter> filters,
    PortalChosen chosen);

void present_portal_save(
    GtkWindow* window,
    const char* title,
    const char* initial_name,
    PortalChosen chosen);

}  // namespace lienzo::gnome
