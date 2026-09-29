#include "ui-gnome/application.hpp"

#include "ui-gnome/main_window.hpp"

#include <gio/gio.h>

namespace lienzo::gnome {

namespace {

void on_quit(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  g_application_quit(
      G_APPLICATION(data));
}

void on_activate(
    GApplication* application,
    gpointer) {
  GtkWindow* window =
      create_main_window(
          ADW_APPLICATION(application));

  gtk_window_present(window);
}

}  // namespace

AdwApplication* create_application() {
  AdwApplication* app =
      adw_application_new(
          "com.getnodalia.Lienzo",
          G_APPLICATION_DEFAULT_FLAGS);

  GSimpleAction* quit =
      g_simple_action_new(
          "quit",
          nullptr);

  g_signal_connect(
      quit,
      "activate",
      G_CALLBACK(on_quit),
      app);

  g_action_map_add_action(
      G_ACTION_MAP(app),
      G_ACTION(quit));

  g_object_unref(quit);

  const char* quit_accels[] = {
      "<Control>q",
      nullptr};

  gtk_application_set_accels_for_action(
      GTK_APPLICATION(app),
      "app.quit",
      quit_accels);

  g_signal_connect(
      app,
      "activate",
      G_CALLBACK(on_activate),
      nullptr);

  return app;
}

}  // namespace lienzo::gnome
