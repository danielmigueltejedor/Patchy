#include "ui-gnome/main_window.hpp"

#include "ui-gnome/primary_menu.hpp"
#include "ui-gnome/preferences_dialog.hpp"
#include "ui-gnome/new_document_dialog.hpp"
#include "ui-gnome/tool_palette.hpp"
#include "ui-gnome/workspace.hpp"

#include "core/document.hpp"
#include "formats/bmp_document_io.hpp"
#include "formats/pcx_document_io.hpp"
#include "psd/psd_document_io.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lienzo::gnome {

namespace {

struct DocumentSession {
  std::unique_ptr<patchy::Document> document;
  std::filesystem::path path;
  std::string title;
  gint64 last_autosave_us{0};
};

struct WindowContext {
  GtkWindow* window{};
  AdwToastOverlay* toast_overlay{};
  AdwTabView* tab_view{};
  GtkStack* content_stack{};
  Tool current_tool{Tool::Brush};
  guint autosave_timer{0};

  ~WindowContext() {
    if (autosave_timer != 0) {
      g_source_remove(autosave_timer);
    }
  }
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

DocumentSession* session_for_page(
    AdwTabPage* page) {
  if (page == nullptr) {
    return nullptr;
  }

  GtkWidget* child =
      adw_tab_page_get_child(page);

  if (child == nullptr) {
    return nullptr;
  }

  return static_cast<DocumentSession*>(
      g_object_get_data(
          G_OBJECT(child),
          "lienzo-document-session"));
}


void save_session_to_path(
    DocumentSession* session,
    const std::filesystem::path& path) {
  if (
      session == nullptr ||
      session->document == nullptr) {
    return;
  }

  const std::string ext =
      extension_lower(path);

  if (ext == ".psd" || ext == ".psb") {
    patchy::psd::WriteOptions options;
    options.large_document =
        ext == ".psb";

    patchy::psd::DocumentIo::
        write_layered_rgb8_file(
            *session->document,
            path,
            options);

    return;
  }

  if (ext == ".bmp") {
    patchy::bmp::DocumentIo::write_file(
        *session->document,
        path);

    return;
  }

  if (ext == ".pcx") {
    patchy::pcx::DocumentIo::write_file(
        *session->document,
        path);

    return;
  }

  patchy::psd::DocumentIo::
      write_layered_rgb8_file(
          *session->document,
          path);
}

struct SaveRequest {
  WindowContext* context{};
  GtkWidget* workspace{};
};

void save_dialog_finished(
    GObject* source,
    GAsyncResult* result,
    gpointer data) {
  std::unique_ptr<SaveRequest> request(
      static_cast<SaveRequest*>(data));

  GError* error = nullptr;

  GFile* file =
      gtk_file_dialog_save_finish(
          GTK_FILE_DIALOG(source),
          result,
          &error);

  if (file == nullptr) {
    if (error != nullptr) {
      g_error_free(error);
    }

    g_object_unref(
        request->workspace);

    return;
  }

  char* raw_path =
      g_file_get_path(file);

  if (raw_path != nullptr) {
    try {
      auto* session =
          static_cast<DocumentSession*>(
              g_object_get_data(
                  G_OBJECT(
                      request->workspace),
                  "lienzo-document-session"));

      if (session != nullptr) {
        std::filesystem::path path(
            raw_path);

        if (path.extension().empty()) {
          path += ".psd";
        }

        save_session_to_path(
            session,
            path);

        session->path = path;
        session->title =
            path.filename().string();

        AdwTabPage* page =
            adw_tab_view_get_page(
                request->context->tab_view,
                request->workspace);

        if (page != nullptr) {
          adw_tab_page_set_title(
              page,
              session->title.c_str());
        }

        show_toast(
            request->context,
            "Documento guardado");
      }
    } catch (
        const std::exception& error) {
      show_toast(
          request->context,
          error.what());
    }

    g_free(raw_path);
  }

  g_object_unref(file);
  g_object_unref(
      request->workspace);
}

void save_active_document(
    WindowContext* context,
    bool save_as) {
  AdwTabPage* page =
      adw_tab_view_get_selected_page(
          context->tab_view);

  if (page == nullptr) {
    return;
  }

  GtkWidget* workspace =
      adw_tab_page_get_child(page);

  auto* session =
      session_for_page(page);

  if (
      session == nullptr ||
      session->document == nullptr) {
    return;
  }

  if (
      !save_as &&
      !session->path.empty()) {
    try {
      save_session_to_path(
          session,
          session->path);

      show_toast(
          context,
          "Documento guardado");
    } catch (
        const std::exception& error) {
      show_toast(
          context,
          error.what());
    }

    return;
  }

  GtkFileDialog* dialog =
      gtk_file_dialog_new();

  gtk_file_dialog_set_title(
      dialog,
      save_as
          ? "Guardar como"
          : "Guardar documento");

  gtk_file_dialog_set_initial_name(
      dialog,
      session->path.empty()
          ? "Sin título.psd"
          : session->path
                .filename()
                .string()
                .c_str());

  auto* request =
      new SaveRequest{
          context,
          GTK_WIDGET(
              g_object_ref(workspace))};

  gtk_file_dialog_save(
      dialog,
      context->window,
      nullptr,
      save_dialog_finished,
      request);

  g_object_unref(dialog);
}

gboolean autosave_tick(
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  if (!autosave_enabled()) {
    return G_SOURCE_CONTINUE;
  }

  const gint64 now =
      g_get_monotonic_time();

  const gint64 interval =
      static_cast<gint64>(
          autosave_interval_minutes()) *
      60 *
      G_USEC_PER_SEC;

  const int pages =
      adw_tab_view_get_n_pages(
          context->tab_view);

  for (int i = 0; i < pages; ++i) {
    AdwTabPage* page =
        adw_tab_view_get_nth_page(
            context->tab_view,
            i);

    auto* session =
        session_for_page(page);

    if (
        session == nullptr ||
        session->document == nullptr ||
        now - session->last_autosave_us <
            interval) {
      continue;
    }

    try {
      if (!session->path.empty()) {
        save_session_to_path(
            session,
            session->path);
      } else {
        std::filesystem::path directory =
            std::filesystem::path(
                g_get_user_config_dir()) /
            "lienzo" /
            "autosave";

        std::filesystem::create_directories(
            directory);

        const auto filename =
            std::string("Untitled-") +
            std::to_string(
                reinterpret_cast<
                    std::uintptr_t>(
                        session)) +
            ".psd";

        patchy::psd::DocumentIo::
            write_layered_rgb8_file(
                *session->document,
                directory / filename);
      }

      session->last_autosave_us =
          now;
    } catch (...) {
      // Autosave must never interrupt editing.
    }
  }

  return G_SOURCE_CONTINUE;
}

void present_document(
    WindowContext* context,
    std::unique_ptr<patchy::Document> document,
    const std::string& title,
    std::filesystem::path path = {}) {
  auto* session =
      new DocumentSession{
          std::move(document),
          std::move(path),
          title,
          g_get_monotonic_time()};

  GtkWidget* workspace =
      create_workspace(
          *session->document,
          context->current_tool,
          [context](Tool tool) {
            context->current_tool =
                tool;
          });

  g_object_set_data_full(
      G_OBJECT(workspace),
      "lienzo-document-session",
      session,
      [](gpointer data) {
        delete static_cast<
            DocumentSession*>(data);
      });

  AdwTabPage* page =
      adw_tab_view_append(
          context->tab_view,
          workspace);

  adw_tab_page_set_title(
      page,
      title.c_str());

  adw_tab_view_set_selected_page(
      context->tab_view,
      page);

  gtk_stack_set_visible_child_name(
      context->content_stack,
      "documents");

  gtk_window_set_title(
      context->window,
      "Lienzo");
}

void on_new_document(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  present_new_document_dialog(
      GTK_WIDGET(context->window),
      [context](const NewDocumentSettings& settings) {
        try {
          auto document =
              std::make_unique<patchy::Document>(
                  settings.width,
                  settings.height,
                  patchy::PixelFormat::rgb8());

          document->print_settings().horizontal_ppi =
              settings.resolution_ppi;

          document->print_settings().vertical_ppi =
              settings.resolution_ppi;

          patchy::PixelFormat background_format =
              patchy::PixelFormat::rgb8();

          std::uint8_t background_value = 255;

          if (
              settings.background ==
              NewDocumentBackground::Black) {
            background_value = 0;
          } else if (
              settings.background ==
              NewDocumentBackground::Transparent) {
            background_format =
                patchy::PixelFormat::rgba8();

            background_value = 0;
          }

          patchy::PixelBuffer background(
              settings.width,
              settings.height,
              background_format);

          background.clear(
              background_value);

          document->add_pixel_layer(
              "Background",
              std::move(background));

          patchy::PixelBuffer paint(
              settings.width,
              settings.height,
              patchy::PixelFormat::rgba8());

          paint.clear(0);

          document->add_pixel_layer(
              "Paint Layer",
              std::move(paint));

          present_document(
              context,
              std::move(document),
              "Sin título — Lienzo");
        } catch (const std::exception& error) {
          show_toast(
              context,
              error.what());
        }
      });
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
        path.filename().string(),
        path);
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

struct ExportRequest {
  WindowContext* context{};
  patchy::Document* document{};
};

void export_dialog_finished(
    GObject* source,
    GAsyncResult* result,
    gpointer data) {
  std::unique_ptr<ExportRequest> request(
      static_cast<ExportRequest*>(data));

  GError* error = nullptr;

  GFile* file =
      gtk_file_dialog_save_finish(
          GTK_FILE_DIALOG(source),
          result,
          &error);

  if (file == nullptr) {
    g_clear_error(&error);
    return;
  }

  char* raw_path =
      g_file_get_path(file);

  if (raw_path == nullptr) {
    g_object_unref(file);
    return;
  }

  try {
    std::filesystem::path path(
        raw_path);

    if (path.extension().empty()) {
      path += ".png";
    }

    std::vector<std::uint8_t> alpha;

    const auto flattened =
        patchy::Compositor{}.flatten_rgb8(
            *request->document,
            &alpha);

    const int width =
        request->document->width();

    const int height =
        request->document->height();

    const bool jpeg =
        extension_lower(path) == ".jpg" ||
        extension_lower(path) == ".jpeg";

    const int channels =
        jpeg ? 3 : 4;

    const std::size_t stride =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(channels);

    auto* pixels =
        static_cast<guchar*>(
            g_malloc(
                stride *
                static_cast<std::size_t>(
                    height)));

    for (int y = 0; y < height; ++y) {
      const auto row =
          flattened.row(y);

      for (int x = 0; x < width; ++x) {
        const auto src =
            row.data() +
            static_cast<std::size_t>(x) * 3;

        const std::size_t index =
            static_cast<std::size_t>(y) *
                static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x);

        auto* dst =
            pixels +
            static_cast<std::size_t>(y) *
                stride +
            static_cast<std::size_t>(x) *
                channels;

        const std::uint8_t a =
            index < alpha.size()
                ? alpha[index]
                : 255;

        if (jpeg) {
          // JPEG no tiene transparencia:
          // componer contra blanco.
          dst[0] =
              static_cast<guchar>(
                  (src[0] * a +
                   255 * (255 - a)) /
                  255);

          dst[1] =
              static_cast<guchar>(
                  (src[1] * a +
                   255 * (255 - a)) /
                  255);

          dst[2] =
              static_cast<guchar>(
                  (src[2] * a +
                   255 * (255 - a)) /
                  255);
        } else {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
          dst[3] = a;
        }
      }
    }

    GdkPixbuf* pixbuf =
        gdk_pixbuf_new_from_data(
            pixels,
            GDK_COLORSPACE_RGB,
            jpeg ? FALSE : TRUE,
            8,
            width,
            height,
            static_cast<int>(stride),
            [](guchar* data, gpointer) {
              g_free(data);
            },
            nullptr);

    GError* save_error = nullptr;

    const char* type =
        jpeg ? "jpeg" : "png";

    if (!gdk_pixbuf_save(
            pixbuf,
            path.string().c_str(),
            type,
            &save_error,
            nullptr)) {
      std::string message =
          save_error != nullptr
              ? save_error->message
              : "No se pudo exportar";

      g_clear_error(&save_error);

      show_toast(
          request->context,
          message.c_str());
    } else {
      show_toast(
          request->context,
          "Imagen exportada");
    }

    g_object_unref(pixbuf);
  } catch (
      const std::exception& error) {
    show_toast(
        request->context,
        error.what());
  }

  g_free(raw_path);
  g_object_unref(file);
}

void on_export_as(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  auto* session =
      session_for_page(
          adw_tab_view_get_selected_page(
              context->tab_view));

  if (
      session == nullptr ||
      session->document == nullptr) {
    return;
  }

  GtkFileDialog* dialog =
      gtk_file_dialog_new();

  gtk_file_dialog_set_title(
      dialog,
      "Exportar como");

  gtk_file_dialog_set_initial_name(
      dialog,
      "export.png");

  gtk_file_dialog_save(
      dialog,
      context->window,
      nullptr,
      export_dialog_finished,
      new ExportRequest{
          context,
          session->document.get()});

  g_object_unref(dialog);
}

void on_save(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  save_active_document(
      static_cast<WindowContext*>(data),
      false);
}

void on_save_as(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  save_active_document(
      static_cast<WindowContext*>(data),
      true);
}

void on_autosave_change_state(
    GSimpleAction* action,
    GVariant* value,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  const bool enabled =
      g_variant_get_boolean(value);

  set_autosave_enabled(enabled);

  g_simple_action_set_state(
      action,
      value);

  show_toast(
      context,
      enabled
          ? "Guardado automático activado"
          : "Guardado automático desactivado");
}

void on_preferences(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  present_preferences_dialog(
      GTK_WIDGET(context->window));
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
      "application-icon", "com.getnodalia.Lienzo",
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
      new WindowContext{};

  context->window =
      GTK_WINDOW(window);

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

  // Guardado: junto a los controles de ventana.
  GMenu* save_menu =
      g_menu_new();

  g_menu_append(
      save_menu,
      "Guardar",
      "win.save");

  g_menu_append(
      save_menu,
      "Guardar como…",
      "win.save-as");

  g_menu_append(
      save_menu,
      "Guardado automático",
      "win.autosave");

  GtkWidget* save_menu_button =
      gtk_menu_button_new();

  gtk_menu_button_set_icon_name(
      GTK_MENU_BUTTON(save_menu_button),
      "document-save-as-symbolic");

  gtk_widget_set_tooltip_text(
      save_menu_button,
      "Guardar");

  gtk_menu_button_set_menu_model(
      GTK_MENU_BUTTON(save_menu_button),
      G_MENU_MODEL(save_menu));

  gtk_widget_add_css_class(
      save_menu_button,
      "flat");

  g_object_unref(save_menu);

  adw_header_bar_pack_start(
      ADW_HEADER_BAR(header_bar),
      save_menu_button);

  // Nuevo / abrir / exportar.
  GMenu* document_menu =
      g_menu_new();

  g_menu_append(
      document_menu,
      "Documento nuevo…",
      "win.new");

  g_menu_append(
      document_menu,
      "Abrir documento…",
      "win.open");

  g_menu_append(
      document_menu,
      "Exportar como…",
      "win.export-as");

  GtkWidget* document_menu_button =
      gtk_menu_button_new();

  gtk_menu_button_set_icon_name(
      GTK_MENU_BUTTON(document_menu_button),
      "document-open-symbolic");

  gtk_widget_set_tooltip_text(
      document_menu_button,
      "Documento");

  gtk_menu_button_set_menu_model(
      GTK_MENU_BUTTON(document_menu_button),
      G_MENU_MODEL(document_menu));

  gtk_widget_add_css_class(
      document_menu_button,
      "flat");

  g_object_unref(document_menu);

  adw_header_bar_pack_start(
      ADW_HEADER_BAR(header_bar),
      document_menu_button);

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
      "com.getnodalia.Lienzo");

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

  GtkWidget* content_stack =
      gtk_stack_new();

  context->content_stack =
      GTK_STACK(content_stack);

  gtk_stack_add_named(
      context->content_stack,
      status_page,
      "welcome");

  GtkWidget* documents =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  AdwTabView* tab_view =
      adw_tab_view_new();

  context->tab_view =
      tab_view;

  AdwTabBar* tab_bar =
      adw_tab_bar_new();

  adw_tab_bar_set_view(
      tab_bar,
      context->tab_view);

  // En un editor gráfico las pestañas forman parte permanente
  // del workspace, incluso con un solo documento abierto.
  adw_tab_bar_set_autohide(
      tab_bar,
      FALSE);

  gtk_box_append(
      GTK_BOX(documents),
      GTK_WIDGET(tab_bar));

  gtk_box_append(
      GTK_BOX(documents),
      GTK_WIDGET(tab_view));

  gtk_widget_set_vexpand(
      GTK_WIDGET(tab_view),
      TRUE);

  gtk_stack_add_named(
      context->content_stack,
      documents,
      "documents");

  gtk_stack_set_visible_child_name(
      context->content_stack,
      "welcome");

  adw_toast_overlay_set_child(
      context->toast_overlay,
      content_stack);

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
      "export-as",
      G_CALLBACK(on_export_as),
      context);

  add_window_action(
      window,
      "save",
      G_CALLBACK(on_save),
      context);

  add_window_action(
      window,
      "save-as",
      G_CALLBACK(on_save_as),
      context);

  GSimpleAction* autosave_action =
      g_simple_action_new_stateful(
          "autosave",
          nullptr,
          g_variant_new_boolean(
              autosave_enabled()));

  g_signal_connect(
      autosave_action,
      "change-state",
      G_CALLBACK(on_autosave_change_state),
      context);

  g_action_map_add_action(
      G_ACTION_MAP(window),
      G_ACTION(autosave_action));

  g_object_unref(
      autosave_action);

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

  context->autosave_timer =
      g_timeout_add_seconds(
          30,
          autosave_tick,
          context);

  return GTK_WINDOW(window);
}

}  // namespace lienzo::gnome
