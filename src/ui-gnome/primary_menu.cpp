#include "ui-gnome/primary_menu.hpp"

namespace lienzo::gnome {

GMenuModel* create_primary_menu() {
  GMenu* menu = g_menu_new();

  GMenu* app_section = g_menu_new();
  g_menu_append(app_section, "Preferencias", "win.preferences");
  g_menu_append(app_section, "Acerca de Lienzo", "win.about");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(app_section));
  g_object_unref(app_section);

  GMenu* quit_section = g_menu_new();
  g_menu_append(quit_section, "Salir", "app.quit");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(quit_section));
  g_object_unref(quit_section);

  return G_MENU_MODEL(menu);
}

}
