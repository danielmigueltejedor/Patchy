#include "ui-gnome/main_window.hpp"

#include "ui-gnome/primary_menu.hpp"

#include <gio/gio.h>

namespace lienzo::gnome {

namespace {

struct WindowContext {
  GtkWindow* window = nullptr;
  AdwToastOverlay* toast_overlay = nullptr;
};

void show_toast(WindowContext* context, const char* text) {
  auto* toast = adw_toast_new(text);
  adw_toast_overlay_add_toast(context->toast_overlay, toast);
}

void on_new_document(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context = static_cast<WindowContext*>(data);
  show_toast(context, "Nuevo documento: conexion con el editor pendiente");
}

void on_open_finished(
    GObject* source,
    GAsyncResult* result,
    gpointer data) {
  auto* context = static_cast<WindowContext*>(data);

  GError* error = nullptr;

  GFile* file = gtk_file_dialog_open_finish(
      GTK_FILE_DIALOG(source),
      result,
      &error);

  if (file == nullptr) {
    g_clear_error(&error);
    return;
  }

  char* path = g_file_get_parse_name(file);

  char* message = g_strdup_printf(
      "Abrir: %s",
      path);

  show_toast(context, message);

  g_free(message);
  g_free(path);
  g_object_unref(file);
}

void on_open(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context = static_cast<WindowContext*>(data);

  GtkFileDialog* dialog = gtk_file_dialog_new();

  gtk_file_dialog_set_title(
      dialog,
      "Abrir imagen");

  gtk_file_dialog_open(
      dialog,
      context->window,
      nullptr,
      on_open_finished,
      context);

  g_object_unref(dialog);
}

void on_preferences(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context = static_cast<WindowContext*>(data);

  show_toast(
      context,
      "Preferencias GNOME: siguiente fase");
}

void on_about(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context = static_cast<WindowContext*>(data);

  AdwDialog* dialog =
      adw_about_dialog_new();

  g_object_set(
      dialog,
      "application-name", "Lienzo",
      "application-icon", "image-x-generic-symbolic",
      "developer-name", "Daniel Miguel Tejedor",
      "version", "GNOME development frontend",
      "comments", "Editor de imagenes con frontend nativo GNOME",
      "website", "https://github.com/danielmigueltejedor/lienzo",
      nullptr);

  adw_dialog_present(
      dialog,
      GTK_WIDGET(context->window));
}

void add_window_action(
    GtkWidget* window,
    const char* name,
    GCallback callback,
    WindowContext* context) {
  GSimpleAction* action =
      g_simple_action_new(name, nullptr);

  g_signal_connect(
      action,
      "activate",
      callback,
      context);

  g_action_map_add_action(
      G_ACTION_MAP(window),
      G_ACTION(action));

  g_object_unref(action);
}

}  // namespace

GtkWindow* create_main_window(
    AdwApplication* app) {
  GtkWidget* window =
      adw_application_window_new(
          GTK_APPLICATION(app));

  gtk_window_set_title(
      GTK_WINDOW(window),
      "Lienzo");

  gtk_window_set_default_size(
      GTK_WINDOW(window),
      1200,
      760);

  auto* context = new WindowContext{
      GTK_WINDOW(window),
      nullptr};

  g_object_set_data_full(
      G_OBJECT(window),
      "lienzo-window-context",
      context,
      [](gpointer data) {
        delete static_cast<WindowContext*>(data);
      });

  GtkWidget* toolbar_view =
      adw_toolbar_view_new();

  GtkWidget* header_bar =
      adw_header_bar_new();

  GtkWidget* title =
      adw_window_title_new(
          "Lienzo",
          "");

  adw_header_bar_set_title_widget(
      ADW_HEADER_BAR(header_bar),
      title);

  GtkWidget* menu_button =
      gtk_menu_button_new();

  gtk_menu_button_set_icon_name(
      GTK_MENU_BUTTON(menu_button),
      "open-menu-symbolic");

  gtk_widget_set_tooltip_text(
      menu_button,
      "Menu principal");

  GMenuModel* primary_menu =
      create_primary_menu();

  gtk_menu_button_set_menu_model(
      GTK_MENU_BUTTON(menu_button),
      primary_menu);

  g_object_unref(primary_menu);

  adw_header_bar_pack_end(
      ADW_HEADER_BAR(header_bar),
      menu_button);

  adw_toolbar_view_add_top_bar(
      ADW_TOOLBAR_VIEW(toolbar_view),
      header_bar);

  GtkWidget* status_page =
      adw_status_page_new();

  adw_status_page_set_icon_name(
      ADW_STATUS_PAGE(status_page),
      "image-x-generic-symbolic");

  adw_status_page_set_title(
      ADW_STATUS_PAGE(status_page),
      "Lienzo");

  adw_status_page_set_description(
      ADW_STATUS_PAGE(status_page),
      "Editor de imagenes");

  GtkWidget* actions =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL,
          12);

  gtk_widget_set_halign(
      actions,
      GTK_ALIGN_CENTER);

  GtkWidget* new_button =
      gtk_button_new_with_label(
          "Nuevo documento");

  gtk_widget_add_css_class(
      new_button,
      "suggested-action");

  gtk_widget_add_css_class(
      new_button,
      "pill");

  gtk_actionable_set_action_name(
      GTK_ACTIONABLE(new_button),
      "win.new");

  GtkWidget* open_button =
      gtk_button_new_with_label(
          "Abrir");

  gtk_widget_add_css_class(
      open_button,
      "pill");

  gtk_actionable_set_action_name(
      GTK_ACTIONABLE(open_button),
      "win.open");

  gtk_box_append(
      GTK_BOX(actions),
      new_button);

  gtk_box_append(
      GTK_BOX(actions),
      open_button);

  adw_status_page_set_child(
      ADW_STATUS_PAGE(status_page),
      actions);

  GtkWidget* toast_overlay =
      adw_toast_overlay_new();

  context->toast_overlay =
      ADW_TOAST_OVERLAY(toast_overlay);

  adw_toast_overlay_set_child(
      context->toast_overlay,
      status_page);

  adw_toolbar_view_set_content(
      ADW_TOOLBAR_VIEW(toolbar_view),
      toast_overlay);

  adw_application_window_set_content(
      ADW_APPLICATION_WINDOW(window),
      toolbar_view);

  add_window_action(
      window,
      "new",
      G_CALLBACK(on_new_document),
      context);

  add_window_action(
      window,
      "open",
      G_CALLBACK(on_open),
      context);

  add_window_action(
      window,
      "preferences",
      G_CALLBACK(on_preferences),
      context);

  add_window_action(
      window,
      "about",
      G_CALLBACK(on_about),
      context);

  return GTK_WINDOW(window);
}

}  // namespace lienzo::gnome
