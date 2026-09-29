#include "ui-gnome/main_window.hpp"

#include "ui-gnome/primary_menu.hpp"

#include "core/document.hpp"
#include "formats/bmp_document_io.hpp"
#include "formats/pcx_document_io.hpp"
#include "psd/psd_document_io.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lienzo::gnome {

namespace {

struct WindowContext {
  GtkWindow* window = nullptr;
  AdwToastOverlay* toast_overlay = nullptr;
  std::unique_ptr<patchy::Document> document;
};

void show_toast(WindowContext* context, const char* text) {
  adw_toast_overlay_add_toast(
      context->toast_overlay,
      adw_toast_new(text));
}

std::string extension_lower(const std::filesystem::path& path) {
  std::string ext = path.extension().string();

  std::transform(
      ext.begin(),
      ext.end(),
      ext.begin(),
      [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });

  return ext;
}

std::unique_ptr<patchy::Document> load_document(
    const std::filesystem::path& path) {
  const std::string ext = extension_lower(path);

  if (ext == ".psd" || ext == ".psb") {
    return std::make_unique<patchy::Document>(
        patchy::psd::DocumentIo::read_file(path));
  }

  if (ext == ".bmp") {
    return std::make_unique<patchy::Document>(
        patchy::bmp::DocumentIo::read_file(path));
  }

  if (ext == ".pcx") {
    return std::make_unique<patchy::Document>(
        patchy::pcx::DocumentIo::read_file(path));
  }

  throw std::runtime_error(
      "Formato todavía no conectado al frontend GNOME");
}

GtkWidget* create_document_view(
    const patchy::Document& document) {
  std::vector<std::uint8_t> alpha;

  patchy::PixelBuffer rgb =
      patchy::Compositor{}.flatten_rgb8(
          document,
          &alpha);

  if (rgb.empty()) {
    throw std::runtime_error(
        "El documento no produjo una imagen renderizable");
  }

  const int width = rgb.width();
  const int height = rgb.height();

  std::vector<std::uint8_t> rgba(
      static_cast<std::size_t>(width) *
      static_cast<std::size_t>(height) *
      4);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto* source = rgb.pixel(x, y);

      const std::size_t pixel_index =
          static_cast<std::size_t>(y) *
              static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);

      const std::size_t target =
          pixel_index * 4;

      rgba[target + 0] = source[0];
      rgba[target + 1] = source[1];
      rgba[target + 2] = source[2];

      rgba[target + 3] =
          alpha.size() > pixel_index
              ? alpha[pixel_index]
              : 255;
    }
  }

  GBytes* bytes =
      g_bytes_new(
          rgba.data(),
          rgba.size());

  GdkTexture* texture =
      gdk_memory_texture_new(
          width,
          height,
          GDK_MEMORY_R8G8B8A8,
          bytes,
          static_cast<gsize>(width) * 4);

  g_bytes_unref(bytes);

  GtkWidget* picture =
      gtk_picture_new_for_paintable(
          GDK_PAINTABLE(texture));

  g_object_unref(texture);

  gtk_picture_set_content_fit(
      GTK_PICTURE(picture),
      GTK_CONTENT_FIT_CONTAIN);

  gtk_widget_set_hexpand(picture, TRUE);
  gtk_widget_set_vexpand(picture, TRUE);

  return picture;
}

void present_document(
    WindowContext* context,
    std::unique_ptr<patchy::Document> document,
    const std::string& title) {
  GtkWidget* view =
      create_document_view(*document);

  context->document =
      std::move(document);

  adw_toast_overlay_set_child(
      context->toast_overlay,
      view);

  gtk_window_set_title(
      context->window,
      title.c_str());
}

void on_new_document(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  try {
    auto document =
        std::make_unique<patchy::Document>(
            1600,
            900,
            patchy::PixelFormat::rgb8());

    patchy::PixelBuffer pixels(
        1600,
        900,
        patchy::PixelFormat::rgb8());

    pixels.clear(255);

    document->add_pixel_layer(
        "Fondo",
        std::move(pixels));

    present_document(
        context,
        std::move(document),
        "Sin título — Lienzo");
  } catch (const std::exception& error) {
    show_toast(
        context,
        error.what());
  }
}

void on_open_finished(
    GObject* source,
    GAsyncResult* result,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  GError* error = nullptr;

  GFile* file =
      gtk_file_dialog_open_finish(
          GTK_FILE_DIALOG(source),
          result,
          &error);

  if (file == nullptr) {
    if (error != nullptr) {
      if (!g_error_matches(
              error,
              G_IO_ERROR,
              G_IO_ERROR_CANCELLED)) {
        show_toast(
            context,
            error->message);
      }

      g_error_free(error);
    }

    return;
  }

  char* raw_path =
      g_file_get_path(file);

  if (raw_path == nullptr) {
    show_toast(
        context,
        "Solo se admiten archivos locales por ahora");

    g_object_unref(file);
    return;
  }

  try {
    const std::filesystem::path path(
        raw_path);

    auto document =
        load_document(path);

    present_document(
        context,
        std::move(document),
        path.filename().string());
  } catch (const std::exception& error) {
    show_toast(
        context,
        error.what());
  }

  g_free(raw_path);
  g_object_unref(file);
}

void on_open(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  GtkFileDialog* dialog =
      gtk_file_dialog_new();

  gtk_file_dialog_set_title(
      dialog,
      "Abrir documento");

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
  show_toast(
      static_cast<WindowContext*>(data),
      "Preferencias: pendiente de migrar");
}

void on_about(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  AdwDialog* dialog =
      adw_about_dialog_new();

  g_object_set(
      dialog,
      "application-name", "Lienzo",
      "application-icon", "image-x-generic-symbolic",
      "developer-name", "Daniel Miguel Tejedor",
      "version", "GNOME development frontend",
      "comments", "Editor de imágenes con frontend nativo GNOME",
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
      g_simple_action_new(
          name,
          nullptr);

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

  auto* context =
      new WindowContext{
          GTK_WINDOW(window),
          nullptr,
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

  gtk_menu_button_set_menu_model(
      GTK_MENU_BUTTON(menu_button),
      create_primary_menu());

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
      "Editor de imágenes");

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
