#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <gtk/gtk.h>

namespace lienzo::gnome {

void present_preferences_dialog(
    GtkWidget* parent);

bool autosave_enabled();
void set_autosave_enabled(bool enabled);
int autosave_interval_minutes();

std::vector<std::string> recent_documents();
void remember_recent_document(
    const std::filesystem::path& path);

}
