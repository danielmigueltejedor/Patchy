#include "ui-gnome/main_window.hpp"

#include "ui-gnome/lienzo_version.hpp"

#include "ui-gnome/export_dialog.hpp"
#include "ui-gnome/file_portal.hpp"
#include "ui-gnome/primary_menu.hpp"
#include "ui-gnome/preferences_dialog.hpp"
#include "ui-gnome/new_document_dialog.hpp"
#include "ui-gnome/tool_palette.hpp"
#include "ui-gnome/workspace.hpp"

#include "core/document.hpp"
#include "formats/bmp_document_io.hpp"
#include "formats/pcx_document_io.hpp"
#include "formats/pxd_document_io.hpp"
#include "psd/psd_document_io.hpp"
#include "render/compositor.hpp"

#include "support/atomic_file_write.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lienzo::gnome {

namespace {

struct DocumentSession {
  std::unique_ptr<patchy::Document> document;
  std::filesystem::path path;
  std::string title;
  gint64 last_autosave_us{0};
};

struct RecentThumbJob {
  int generation{0};
  std::filesystem::path path;
  GWeakRef picture{};
  GWeakRef window{};
  int width{0};
  int height{0};
  std::vector<std::uint8_t> rgba;
};

struct WindowContext {
  GtkWindow* window{};
  AdwToastOverlay* toast_overlay{};
  AdwTabView* tab_view{};
  GtkStack* content_stack{};
  Tool current_tool{Tool::Brush};
  guint autosave_timer{0};
  bool allow_close{false};
  GtkWidget* recent_box{};
  GtkListBox* recent_list{};
  int recent_generation{0};
  bool recent_thumb_busy{false};
  std::vector<RecentThumbJob*> recent_thumb_queue{};

  ~WindowContext() {
    if (autosave_timer != 0) {
      g_source_remove(autosave_timer);
    }

    for (auto* job : recent_thumb_queue) {
      g_weak_ref_clear(&job->picture);
      g_weak_ref_clear(&job->window);
      delete job;
    }
  }
};

void start_open(
    WindowContext* context,
    std::filesystem::path path);

void refresh_recent_documents(WindowContext* context);

void note_recent_document(
    WindowContext* context,
    const std::filesystem::path& path) {
  remember_recent_document(path);
  refresh_recent_documents(context);
}

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

  if (ext == ".pxd") {
    std::vector<std::string> notices;

    auto document =
        std::make_unique<patchy::Document>(
            patchy::pxd::DocumentIo::read_file(
                path,
                &notices));

    (void)notices;

    return document;
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

  if (ext == ".pxd") {
    patchy::pxd::DocumentIo::write_file(
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
  bool close_page_after{false};
  bool close_window_after{false};
  AdwTabPage* page{};
};

gboolean* document_dirty_flag(GtkWidget* workspace) {
  return static_cast<gboolean*>(
      g_object_get_data(
          G_OBJECT(workspace),
          "lienzo-dirty"));
}

void mark_document_clean(GtkWidget* workspace) {
  if (auto* dirty = document_dirty_flag(workspace)) {
    *dirty = FALSE;
  }
}

bool document_is_dirty(GtkWidget* workspace) {
  const auto* dirty =
      document_dirty_flag(workspace);

  return dirty != nullptr && *dirty;
}

void save_dirty_then_close_window(WindowContext* context);

void complete_save_request(
    SaveRequest* request,
    bool saved) {
  if (saved) {
    mark_document_clean(request->workspace);
  }

  if (request->close_window_after && saved) {
    auto* context = request->context;
    g_object_unref(request->workspace);
    delete request;
    save_dirty_then_close_window(context);
    return;
  }

  if (
      request->close_page_after &&
      request->page != nullptr) {
    adw_tab_view_close_page_finish(
        request->context->tab_view,
        request->page,
        saved);
  }

  g_object_unref(request->workspace);
  delete request;
}

void begin_save(
    WindowContext* context,
    GtkWidget* workspace,
    AdwTabPage* page,
    bool save_as,
    bool close_page_after,
    bool close_window_after) {
  auto* session =
      static_cast<DocumentSession*>(
          g_object_get_data(
              G_OBJECT(workspace),
              "lienzo-document-session"));

  if (
      session == nullptr ||
      session->document == nullptr) {
    return;
  }

  if (
      !save_as &&
      !session->path.empty()) {
    bool saved = false;

    try {
      save_session_to_path(
          session,
          session->path);

      mark_document_clean(workspace);
      saved = true;
      note_recent_document(context, session->path);

      show_toast(
          context,
          "Documento guardado");
    } catch (
        const std::exception& error) {
      show_toast(
          context,
          error.what());
    }

    if (close_window_after && saved) {
      save_dirty_then_close_window(context);
      return;
    }

    if (close_page_after && page != nullptr) {
      adw_tab_view_close_page_finish(
          context->tab_view,
          page,
          saved);
    }

    return;
  }

  auto* request =
      new SaveRequest{
          context,
          GTK_WIDGET(g_object_ref(workspace)),
          close_page_after,
          close_window_after,
          page};

  const std::string initial_name =
      session->path.empty()
          ? "Sin título.psd"
          : session->path.filename().string();

  present_portal_save(
      context->window,
      save_as
          ? "Guardar como"
          : "Guardar documento",
      initial_name.c_str(),
      [request](std::optional<std::filesystem::path> chosen) {
        if (!chosen.has_value()) {
          complete_save_request(request, false);
          return;
        }

        bool saved = false;

        try {
          auto path = *chosen;

          if (path.extension().empty()) {
            path += ".psd";
          }

          auto* session =
              static_cast<DocumentSession*>(
                  g_object_get_data(
                      G_OBJECT(request->workspace),
                      "lienzo-document-session"));

          if (session != nullptr) {
            save_session_to_path(session, path);
            session->path = path;
            session->title = path.filename().string();

            AdwTabPage* saved_page =
                adw_tab_view_get_page(
                    request->context->tab_view,
                    request->workspace);

            if (saved_page != nullptr) {
              adw_tab_page_set_title(
                  saved_page,
                  session->title.c_str());
            }

            show_toast(
                request->context,
                "Documento guardado");

            note_recent_document(
                request->context,
                path);
            saved = true;
          }
        } catch (const std::exception& error) {
          show_toast(
              request->context,
              error.what());
        }

        complete_save_request(request, saved);
      });
}

void save_dirty_then_close_window(
    WindowContext* context) {
  const int pages =
      adw_tab_view_get_n_pages(context->tab_view);

  for (int i = 0; i < pages; ++i) {
    AdwTabPage* page =
        adw_tab_view_get_nth_page(
            context->tab_view,
            i);

    GtkWidget* workspace =
        adw_tab_page_get_child(page);

    auto* session = session_for_page(page);

    if (
        session == nullptr ||
        workspace == nullptr ||
        !document_is_dirty(workspace) ||
        session->path.empty()) {
      continue;
    }

    try {
      save_session_to_path(
          session,
          session->path);

      mark_document_clean(workspace);
      note_recent_document(context, session->path);
    } catch (const std::exception& error) {
      show_toast(context, error.what());
      return;
    }
  }

  for (int i = 0; i < pages; ++i) {
    AdwTabPage* page =
        adw_tab_view_get_nth_page(
            context->tab_view,
            i);

    GtkWidget* workspace =
        adw_tab_page_get_child(page);

    auto* session = session_for_page(page);

    if (
        session == nullptr ||
        workspace == nullptr ||
        !document_is_dirty(workspace) ||
        !session->path.empty()) {
      continue;
    }

    begin_save(
        context,
        workspace,
        page,
        true,
        false,
        true);

    return;
  }

  context->allow_close = true;
  gtk_window_destroy(GTK_WINDOW(context->window));
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

  begin_save(
      context,
      adw_tab_page_get_child(page),
      page,
      save_as,
      false,
      false);
}

struct ClosePrompt {
  WindowContext* context{};
  AdwTabPage* page{};
};

bool any_document_dirty(WindowContext* context) {
  const int pages =
      adw_tab_view_get_n_pages(context->tab_view);

  for (int i = 0; i < pages; ++i) {
    AdwTabPage* page =
        adw_tab_view_get_nth_page(
            context->tab_view,
            i);

    if (document_is_dirty(
            adw_tab_page_get_child(page))) {
      return true;
    }
  }

  return false;
}

void on_close_response(
    AdwAlertDialog*,
    char* response,
    gpointer data) {
  auto* prompt =
      static_cast<ClosePrompt*>(data);

  const bool discard =
      response != nullptr &&
      std::strcmp(response, "discard") == 0;

  const bool save =
      response != nullptr &&
      std::strcmp(response, "save") == 0;

  if (prompt->page != nullptr && !discard && !save) {
    adw_tab_view_close_page_finish(
        prompt->context->tab_view,
        prompt->page,
        FALSE);
  } else if (discard && prompt->page != nullptr) {
    adw_tab_view_close_page_finish(
        prompt->context->tab_view,
        prompt->page,
        TRUE);
  } else if (discard) {
    prompt->context->allow_close = true;
    gtk_window_destroy(
        GTK_WINDOW(prompt->context->window));
  } else if (save && prompt->page != nullptr) {
    GtkWidget* workspace =
        adw_tab_page_get_child(prompt->page);

    auto* session =
        session_for_page(prompt->page);

    begin_save(
        prompt->context,
        workspace,
        prompt->page,
        session == nullptr || session->path.empty(),
        true,
        false);
  } else if (save) {
    save_dirty_then_close_window(prompt->context);
  }

  delete prompt;
}

void present_close_prompt(
    WindowContext* context,
    AdwTabPage* page) {
  const bool dirty =
      page != nullptr
          ? document_is_dirty(
                adw_tab_page_get_child(page))
          : any_document_dirty(context);

  AdwDialog* dialog =
      adw_alert_dialog_new(
          page != nullptr
              ? "¿Cerrar el documento?"
              : "¿Cerrar Lienzo?",
          dirty
              ? "Hay cambios sin guardar. Puedes guardarlos, cerrar sin guardar o seguir editando."
              : "Puedes seguir editando o cerrar ahora.");

  if (dirty) {
    adw_alert_dialog_add_responses(
        ADW_ALERT_DIALOG(dialog),
        "cancel", "Cancelar",
        "discard", "Cerrar sin guardar",
        "save", "Guardar",
        nullptr);

    adw_alert_dialog_set_response_appearance(
        ADW_ALERT_DIALOG(dialog),
        "save",
        ADW_RESPONSE_SUGGESTED);
  } else {
    adw_alert_dialog_add_responses(
        ADW_ALERT_DIALOG(dialog),
        "cancel", "Cancelar",
        "discard", "Cerrar",
        nullptr);
  }

  adw_alert_dialog_set_response_appearance(
      ADW_ALERT_DIALOG(dialog),
      "discard",
      ADW_RESPONSE_DESTRUCTIVE);

  adw_alert_dialog_set_default_response(
      ADW_ALERT_DIALOG(dialog),
      "cancel");

  adw_alert_dialog_set_close_response(
      ADW_ALERT_DIALOG(dialog),
      "cancel");

  auto* prompt =
      new ClosePrompt{context, page};

  g_signal_connect(
      dialog,
      "response",
      G_CALLBACK(on_close_response),
      prompt);

  adw_dialog_present(
      dialog,
      GTK_WIDGET(context->window));
}

gboolean on_window_close_request(
    GtkWindow* window,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  if (
      g_object_get_data(
          G_OBJECT(window),
          "lienzo-block-close") != nullptr) {
    return TRUE;
  }

  if (context->allow_close) {
    return FALSE;
  }

  present_close_prompt(context, nullptr);
  return TRUE;
}

gboolean on_tab_close_page(
    AdwTabView*,
    AdwTabPage* page,
    gpointer data) {
  present_close_prompt(
      static_cast<WindowContext*>(data),
      page);

  return TRUE;
}

void on_page_detached(
    AdwTabView* view,
    AdwTabPage*,
    gint,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  if (adw_tab_view_get_n_pages(view) == 0) {
    refresh_recent_documents(context);
    gtk_stack_set_visible_child_name(
        context->content_stack,
        "welcome");
  }
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

        mark_document_clean(
            adw_tab_page_get_child(page));
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
    std::filesystem::path path = {},
    const CanvasPreview* prepared = nullptr) {
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
          },
          prepared);

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

struct OpenJob {
  WindowContext* context{};
  GWeakRef window{};
  std::filesystem::path path;
  std::unique_ptr<patchy::Document> document;
  CanvasPreview preview;
  std::string error;
};

gboolean finish_open(gpointer data) {
  std::unique_ptr<OpenJob> job(
      static_cast<OpenJob*>(data));

  auto* window = static_cast<GtkWindow*>(
      g_weak_ref_get(&job->window));

  if (window == nullptr) {
    g_weak_ref_clear(&job->window);
    return G_SOURCE_REMOVE;
  }

  g_object_unref(window);

  if (!job->error.empty()) {
    show_toast(
        job->context,
        job->error.c_str());
  } else if (job->document) {
    try {
      const CanvasPreview* prepared =
          job->preview.width > 0
              ? &job->preview
              : nullptr;

      present_document(
          job->context,
          std::move(job->document),
          job->path.filename().string(),
          job->path,
          prepared);

      note_recent_document(
          job->context,
          job->path);
    } catch (const std::exception& error) {
      show_toast(
          job->context,
          error.what());
    }
  }

  g_weak_ref_clear(&job->window);
  return G_SOURCE_REMOVE;
}

bool select_open_document(
    WindowContext* context,
    const std::filesystem::path& path) {
  const int pages =
      adw_tab_view_get_n_pages(context->tab_view);

  for (int i = 0; i < pages; ++i) {
    AdwTabPage* page =
        adw_tab_view_get_nth_page(
            context->tab_view,
            i);

    auto* session = session_for_page(page);

    if (
        session == nullptr ||
        session->path.empty()) {
      continue;
    }

    std::error_code error;
    const bool same_file =
        std::filesystem::equivalent(
            session->path,
            path,
            error);

    const bool same_text =
        session->path.lexically_normal() ==
        path.lexically_normal();

    if (!same_file && !same_text) {
      continue;
    }

    adw_tab_view_set_selected_page(
        context->tab_view,
        page);

    gtk_stack_set_visible_child_name(
        context->content_stack,
        "documents");

    return true;
  }

  return false;
}

void start_open(
    WindowContext* context,
    std::filesystem::path path) {
  if (path.filename() == "metadata.info") {
    path = path.parent_path();
  }

  if (select_open_document(context, path)) {
    return;
  }

  std::error_code error;

  if (!std::filesystem::exists(path, error)) {
    show_toast(
        context,
        "Ese archivo ya no está");
    refresh_recent_documents(context);
    return;
  }

  show_toast(
      context,
      "Abriendo documento…");

  auto* job = new OpenJob;
  job->context = context;
  job->path = std::move(path);
  g_weak_ref_init(
      &job->window,
      context->window);

  std::thread([job] {
    try {
      job->document =
          load_document(job->path);

      if (job->document) {
        try {
          job->preview =
              build_canvas_preview(
                  *job->document);
        } catch (...) {
          job->preview = {};
        }
      }
    } catch (const std::exception& error) {
      job->error = error.what();
    } catch (...) {
      job->error =
          "No se pudo abrir el documento";
    }

    g_idle_add(finish_open, job);
  }).detach();
}

std::filesystem::path path_from_stored(
    const std::string& stored) {
  gchar* filename =
      g_filename_from_utf8(
          stored.c_str(),
          -1,
          nullptr,
          nullptr,
          nullptr);

  if (filename == nullptr) {
    return std::filesystem::path(stored);
  }

  std::filesystem::path path(filename);
  g_free(filename);
  return path;
}

std::string filename_for_display(
    const std::filesystem::path& path) {
  gchar* utf8 =
      g_filename_to_utf8(
          path.filename().string().c_str(),
          -1,
          nullptr,
          nullptr,
          nullptr);

  if (utf8 == nullptr) {
    return path.filename().string();
  }

  std::string result(utf8);
  g_free(utf8);
  return result;
}

std::string directory_for_display(
    const std::filesystem::path& path) {
  gchar* utf8 =
      g_filename_to_utf8(
          path.parent_path().string().c_str(),
          -1,
          nullptr,
          nullptr,
          nullptr);

  if (utf8 == nullptr) {
    return path.parent_path().string();
  }

  std::string result(utf8);
  g_free(utf8);
  return result;
}

void open_recent_row(
    WindowContext* context,
    GtkListBoxRow* row) {
  if (row == nullptr) {
    return;
  }

  const char* stored =
      static_cast<const char*>(
          g_object_get_data(
              G_OBJECT(row),
              "lienzo-recent-path"));

  if (stored == nullptr) {
    return;
  }

  start_open(
      context,
      path_from_stored(stored));
}

void on_recent_activated(
    GtkListBox*,
    GtkListBoxRow* row,
    gpointer data) {
  open_recent_row(
      static_cast<WindowContext*>(data),
      row);
}

std::uint8_t recent_checker(
    int x,
    int y) {
  return (((x / 8) ^ (y / 8)) & 1) != 0 ? 236 : 214;
}

void fit_recent_preview(
    CanvasPreview& preview) {
  constexpr int kMaxEdge = 128;

  if (
      preview.width <= 0 ||
      preview.height <= 0 ||
      preview.rgba.size() <
          static_cast<std::size_t>(preview.width) *
              static_cast<std::size_t>(preview.height) * 4) {
    preview = {};
    return;
  }

  int target_width = preview.width;
  int target_height = preview.height;
  const int longest =
      std::max(preview.width, preview.height);

  if (longest > kMaxEdge) {
    target_width = std::max(
        1,
        preview.width * kMaxEdge / longest);
    target_height = std::max(
        1,
        preview.height * kMaxEdge / longest);
  }

  std::vector<std::uint8_t> fitted(
      static_cast<std::size_t>(target_width) *
      static_cast<std::size_t>(target_height) * 4);

  for (int y = 0; y < target_height; ++y) {
    const int source_y = std::min(
        preview.height - 1,
        y * preview.height / target_height);

    for (int x = 0; x < target_width; ++x) {
      const int source_x = std::min(
          preview.width - 1,
          x * preview.width / target_width);
      const auto* src =
          preview.rgba.data() +
          (static_cast<std::size_t>(source_y) *
               static_cast<std::size_t>(preview.width) +
           static_cast<std::size_t>(source_x)) *
              4;
      auto* dst =
          fitted.data() +
          (static_cast<std::size_t>(y) *
               static_cast<std::size_t>(target_width) +
           static_cast<std::size_t>(x)) *
              4;
      const int coverage = src[3];
      const int under = recent_checker(x, y);
      dst[0] = static_cast<std::uint8_t>(
          (src[0] * coverage + under * (255 - coverage)) / 255);
      dst[1] = static_cast<std::uint8_t>(
          (src[1] * coverage + under * (255 - coverage)) / 255);
      dst[2] = static_cast<std::uint8_t>(
          (src[2] * coverage + under * (255 - coverage)) / 255);
      dst[3] = 255;
    }
  }

  preview.width = target_width;
  preview.height = target_height;
  preview.rgba = std::move(fitted);
}

std::uint64_t recent_path_hash(
    const std::filesystem::path& path) {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffset;

  for (const unsigned char byte : path.generic_string()) {
    hash ^= byte;
    hash *= kPrime;
  }

  return hash;
}

std::filesystem::path recent_cache_directory() {
  const char* root = g_get_user_cache_dir();
  std::filesystem::path directory =
      root != nullptr && root[0] != '\0'
          ? std::filesystem::path(root)
          : std::filesystem::temp_directory_path();
  directory /= "lienzo";
  directory /= "recent-previews";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return directory;
}

std::optional<std::filesystem::path> recent_cache_file(
    const std::filesystem::path& path) {
  std::error_code error;

  if (!std::filesystem::exists(path, error) || error) {
    return std::nullopt;
  }

  const auto stamp =
      std::filesystem::last_write_time(path, error)
          .time_since_epoch()
          .count();

  if (error) {
    return std::nullopt;
  }

  const auto bytes =
      std::filesystem::file_size(path, error);

  if (error) {
    return std::nullopt;
  }

  char name[96];
  std::snprintf(
      name,
      sizeof(name),
      "%016llx-%lld-%llu.png",
      static_cast<unsigned long long>(
          recent_path_hash(path)),
      static_cast<long long>(stamp),
      static_cast<unsigned long long>(bytes));
  return recent_cache_directory() / name;
}

bool show_cached_recent_thumb(
    GtkWidget* picture,
    const std::filesystem::path& path) {
  const auto file = recent_cache_file(path);

  if (!file.has_value()) {
    return false;
  }

  std::error_code error;

  if (!std::filesystem::exists(*file, error) || error) {
    return false;
  }

  GError* load_error = nullptr;
  GdkTexture* texture =
      gdk_texture_new_from_filename(
          file->c_str(),
          &load_error);

  if (texture == nullptr) {
    g_clear_error(&load_error);
    return false;
  }

  gtk_picture_set_paintable(
      GTK_PICTURE(picture),
      GDK_PAINTABLE(texture));
  g_object_unref(texture);
  return true;
}

void store_recent_thumb(
    const std::filesystem::path& path,
    GdkTexture* texture) {
  const auto file = recent_cache_file(path);

  if (!file.has_value() || texture == nullptr) {
    return;
  }

  GBytes* png = gdk_texture_save_to_png_bytes(texture);

  if (png == nullptr) {
    return;
  }

  gsize length = 0;
  const auto* data = static_cast<const std::uint8_t*>(
      g_bytes_get_data(png, &length));
  const std::vector<std::uint8_t> bytes(data, data + length);
  g_bytes_unref(png);

  try {
    patchy::write_file_bytes_atomically(
        *file,
        bytes,
        "No se pudo crear la miniatura",
        "No se pudo guardar la miniatura");
  } catch (const std::exception&) {
  }
}

void prune_recent_cache(
    const std::set<std::filesystem::path>& keep) {
  const auto directory = recent_cache_directory();
  std::error_code error;

  for (const auto& entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (error || !entry.is_regular_file()) {
      continue;
    }

    if (!keep.contains(entry.path())) {
      std::filesystem::remove(entry.path(), error);
    }
  }
}

void discard_pending_recent_thumbs(
    WindowContext* context) {
  for (auto* job : context->recent_thumb_queue) {
    g_weak_ref_clear(&job->picture);
    g_weak_ref_clear(&job->window);
    delete job;
  }

  context->recent_thumb_queue.clear();
}

void start_next_recent_thumb(WindowContext* context);

gboolean apply_recent_thumb(gpointer data) {
  std::unique_ptr<RecentThumbJob> job(
      static_cast<RecentThumbJob*>(data));
  auto* window = static_cast<GtkWindow*>(
      g_weak_ref_get(&job->window));
  g_weak_ref_clear(&job->window);

  if (window == nullptr) {
    g_weak_ref_clear(&job->picture);
    return G_SOURCE_REMOVE;
  }

  auto* context =
      static_cast<WindowContext*>(
          g_object_get_data(
              G_OBJECT(window),
              "lienzo-window-context"));
  g_object_unref(window);

  if (context == nullptr) {
    g_weak_ref_clear(&job->picture);
    return G_SOURCE_REMOVE;
  }

  context->recent_thumb_busy = false;
  auto* picture = static_cast<GtkWidget*>(
      g_weak_ref_get(&job->picture));
  g_weak_ref_clear(&job->picture);

  if (
      picture != nullptr &&
      context->recent_generation == job->generation &&
      job->width > 0 &&
      job->height > 0 &&
      !job->rgba.empty()) {
    GBytes* bytes =
        g_bytes_new(
            job->rgba.data(),
            job->rgba.size());
    GdkTexture* texture =
        gdk_memory_texture_new(
            job->width,
            job->height,
            GDK_MEMORY_R8G8B8A8,
            bytes,
            static_cast<gsize>(job->width) * 4);
    g_bytes_unref(bytes);
    gtk_picture_set_paintable(
        GTK_PICTURE(picture),
        GDK_PAINTABLE(texture));
    store_recent_thumb(job->path, texture);
    g_object_unref(texture);
  }

  if (picture != nullptr) {
    g_object_unref(picture);
  }

  start_next_recent_thumb(context);
  return G_SOURCE_REMOVE;
}

void start_next_recent_thumb(
    WindowContext* context) {
  if (
      context->recent_thumb_busy ||
      context->recent_thumb_queue.empty()) {
    return;
  }

  RecentThumbJob* job =
      context->recent_thumb_queue.front();
  context->recent_thumb_queue.erase(
      context->recent_thumb_queue.begin());
  context->recent_thumb_busy = true;

  std::thread([job] {
    try {
      const auto document = load_document(job->path);

      if (document) {
        auto preview = build_canvas_preview(*document);
        fit_recent_preview(preview);
        job->width = preview.width;
        job->height = preview.height;
        job->rgba = std::move(preview.rgba);
      }
    } catch (...) {
      job->rgba.clear();
    }

    g_idle_add(apply_recent_thumb, job);
  }).detach();
}

void queue_recent_thumb(
    WindowContext* context,
    GtkWidget* picture,
    const std::filesystem::path& path) {
  auto* job = new RecentThumbJob;
  job->generation = context->recent_generation;
  job->path = path;
  g_weak_ref_init(&job->picture, picture);
  g_weak_ref_init(&job->window, context->window);
  context->recent_thumb_queue.push_back(job);
  start_next_recent_thumb(context);
}

gboolean on_recent_key(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType,
    gpointer data) {
  if (
      keyval != GDK_KEY_Return &&
      keyval != GDK_KEY_KP_Enter) {
    return FALSE;
  }

  auto* context = static_cast<WindowContext*>(data);
  open_recent_row(
      context,
      gtk_list_box_get_selected_row(
          context->recent_list));
  return TRUE;
}

void refresh_recent_documents(
    WindowContext* context) {
  if (
      context->recent_list == nullptr ||
      context->recent_box == nullptr) {
    return;
  }

  ++context->recent_generation;
  discard_pending_recent_thumbs(context);

  while (
      GtkWidget* child = gtk_widget_get_first_child(
          GTK_WIDGET(context->recent_list))) {
    gtk_list_box_remove(
        context->recent_list,
        child);
  }

  int shown = 0;
  std::set<std::filesystem::path> cached;

  for (const auto& stored : recent_documents()) {
    const auto path = path_from_stored(stored);
    std::error_code error;

    if (
        path.empty() ||
        !std::filesystem::exists(path, error)) {
      continue;
    }

    GtkWidget* row = gtk_list_box_row_new();
    GtkWidget* box =
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    GtkWidget* thumb = gtk_picture_new();
    gtk_widget_set_size_request(thumb, 72, 54);
    gtk_widget_set_can_target(thumb, FALSE);
    gtk_picture_set_can_shrink(GTK_PICTURE(thumb), FALSE);
    gtk_picture_set_content_fit(
        GTK_PICTURE(thumb),
        GTK_CONTENT_FIT_CONTAIN);

    GtkWidget* text =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

    GtkWidget* title =
        gtk_label_new(filename_for_display(path).c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_label_set_ellipsize(
        GTK_LABEL(title),
        PANGO_ELLIPSIZE_END);
    gtk_widget_set_can_target(title, FALSE);

    GtkWidget* subtitle =
        gtk_label_new(directory_for_display(path).c_str());
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0F);
    gtk_label_set_ellipsize(
        GTK_LABEL(subtitle),
        PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_widget_set_can_target(subtitle, FALSE);

    gtk_box_append(GTK_BOX(text), title);
    gtk_box_append(GTK_BOX(text), subtitle);
    gtk_box_append(GTK_BOX(box), thumb);
    gtk_box_append(GTK_BOX(box), text);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(
        row,
        "Doble clic para abrir");

    g_object_set_data(
        G_OBJECT(row),
        "lienzo-recent-picture",
        thumb);
    g_object_set_data_full(
        G_OBJECT(row),
        "lienzo-recent-path",
        g_strdup(stored.c_str()),
        g_free);

    gtk_list_box_append(
        context->recent_list,
        row);

    if (const auto cached_file = recent_cache_file(path)) {
      cached.insert(*cached_file);
    }

    if (!show_cached_recent_thumb(thumb, path)) {
      queue_recent_thumb(context, thumb, path);
    }

    ++shown;
  }

  gtk_widget_set_visible(
      context->recent_box,
      shown > 0);

  if (shown > 0) {
    gtk_list_box_select_row(
        context->recent_list,
        gtk_list_box_get_row_at_index(
            context->recent_list,
            0));
  }

  prune_recent_cache(cached);
}

void on_open(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  present_portal_open(
      context->window,
      "Abrir documento",
      {
          {"Documentos de Lienzo",
           {"*.psd", "*.psb", "*.pxd", "*.bmp", "*.pcx"}},
          {"Pixelmator Pro", {"*.pxd"}},
          {"Photoshop", {"*.psd", "*.psb"}},
      },
      [context](std::optional<std::filesystem::path> chosen) {
        if (!chosen.has_value()) {
          return;
        }

        start_open(context, *chosen);
      });
}

void export_flat_pixbuf(
    const patchy::Document& document,
    const std::filesystem::path& path,
    const char* type,
    int quality,
    bool jpeg,
    bool use_quality) {
  std::vector<std::uint8_t> alpha;

  const auto flattened =
      patchy::Compositor{}.flatten_rgb8(
          document,
          &alpha);

  const int width = document.width();
  const int height = document.height();
  const int channels = jpeg ? 3 : 4;

  const std::size_t stride =
      static_cast<std::size_t>(width) *
      static_cast<std::size_t>(channels);

  auto* pixels =
      static_cast<guchar*>(
          g_malloc(
              stride *
              static_cast<std::size_t>(height)));

  for (int y = 0; y < height; ++y) {
    const auto row = flattened.row(y);

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
          static_cast<std::size_t>(y) * stride +
          static_cast<std::size_t>(x) * channels;

      const std::uint8_t coverage =
          index < alpha.size() ? alpha[index] : 255;

      if (jpeg) {
        dst[0] = static_cast<guchar>(
            (src[0] * coverage + 255 * (255 - coverage)) / 255);
        dst[1] = static_cast<guchar>(
            (src[1] * coverage + 255 * (255 - coverage)) / 255);
        dst[2] = static_cast<guchar>(
            (src[2] * coverage + 255 * (255 - coverage)) / 255);
      } else {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = coverage;
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
          [](guchar* data, gpointer) { g_free(data); },
          nullptr);

  GError* save_error = nullptr;
  const std::string quality_text = std::to_string(quality);
  const bool saved =
      use_quality
          ? gdk_pixbuf_save(
                pixbuf,
                path.string().c_str(),
                type,
                &save_error,
                "quality",
                quality_text.c_str(),
                nullptr)
          : gdk_pixbuf_save(
                pixbuf,
                path.string().c_str(),
                type,
                &save_error,
                nullptr);

  g_object_unref(pixbuf);

  if (!saved) {
    const std::string message =
        save_error != nullptr ? save_error->message : "No se pudo exportar";
    g_clear_error(&save_error);
    throw std::runtime_error(message);
  }
}

void write_export(
    const patchy::Document& document,
    const std::filesystem::path& path,
    const ExportSettings& settings) {
  switch (settings.kind) {
    case ExportKind::Png:
      export_flat_pixbuf(document, path, "png", settings.quality, false, false);
      return;
    case ExportKind::Jpeg:
      export_flat_pixbuf(document, path, "jpeg", settings.quality, true, true);
      return;
    case ExportKind::Webp:
      export_flat_pixbuf(
          document,
          path,
          "webp",
          settings.lossless ? 100 : settings.quality,
          false,
          true);
      return;
    case ExportKind::Bmp:
      patchy::bmp::DocumentIo::write_file(document, path);
      return;
    case ExportKind::Pcx:
      patchy::pcx::DocumentIo::write_file(document, path);
      return;
    case ExportKind::Psd:
    case ExportKind::Psb: {
      patchy::psd::WriteOptions options;
      options.large_document = settings.kind == ExportKind::Psb;
      patchy::psd::DocumentIo::write_layered_rgb8_file(document, path, options);
      return;
    }
    case ExportKind::Pxd:
      patchy::pxd::DocumentIo::write_file(document, path);
      return;
  }
}

void on_export_as(
    GSimpleAction*,
    GVariant*,
    gpointer data) {
  auto* context =
      static_cast<WindowContext*>(data);

  auto* session =
      session_for_page(
          adw_tab_view_get_selected_page(context->tab_view));

  if (
      session == nullptr ||
      session->document == nullptr) {
    return;
  }

  const std::string stem =
      session->path.empty()
          ? "export"
          : session->path.stem().string();

  present_export_dialog(
      GTK_WIDGET(context->window),
      stem,
      [context, stem](ExportSettings settings) {
        GtkWidget* workspace =
            adw_tab_view_get_selected_page(context->tab_view) != nullptr
                ? adw_tab_page_get_child(
                      adw_tab_view_get_selected_page(context->tab_view))
                : nullptr;

        if (workspace == nullptr) {
          return;
        }

        g_object_ref(workspace);

        const std::string initial =
            stem + settings.extension;

        present_portal_save(
            context->window,
            "Exportar",
            initial.c_str(),
            [context, workspace, settings](
                std::optional<std::filesystem::path> chosen) {
              if (!chosen.has_value()) {
                g_object_unref(workspace);
                return;
              }

              try {
                auto path = *chosen;

                if (path.extension().empty()) {
                  path += settings.extension;
                }

                auto* session =
                    static_cast<DocumentSession*>(
                        g_object_get_data(
                            G_OBJECT(workspace),
                            "lienzo-document-session"));

                if (
                    session != nullptr &&
                    session->document != nullptr) {
                  write_export(
                      *session->document,
                      path,
                      settings);
                  show_toast(context, "Imagen exportada");
                }
              } catch (const std::exception& error) {
                show_toast(context, error.what());
              }

              g_object_unref(workspace);
            });
      });
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
      "application-icon", "com.nodalix.lienzo",
      "developer-name", "Daniel Miguel Tejedor",
      "version", kLienzoVersion,
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
      "com.nodalix.lienzo");

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

  GtkWidget* welcome =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          0);

  gtk_box_append(
      GTK_BOX(welcome),
      actions);

  GtkWidget* recent_box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL,
          8);

  context->recent_box = recent_box;
  gtk_widget_set_margin_top(recent_box, 28);
  gtk_widget_set_size_request(recent_box, 480, -1);
  gtk_widget_set_halign(recent_box, GTK_ALIGN_CENTER);

  GtkWidget* recent_heading =
      gtk_label_new("Abiertos recientemente");

  gtk_widget_add_css_class(recent_heading, "heading");
  gtk_label_set_xalign(GTK_LABEL(recent_heading), 0.0F);
  gtk_box_append(GTK_BOX(recent_box), recent_heading);

  GtkWidget* recent_hint =
      gtk_label_new("Doble clic para abrir");
  gtk_label_set_xalign(GTK_LABEL(recent_hint), 0.0F);
  gtk_widget_add_css_class(recent_hint, "dim-label");
  gtk_box_append(GTK_BOX(recent_box), recent_hint);

  GtkWidget* recent_list = gtk_list_box_new();
  context->recent_list = GTK_LIST_BOX(recent_list);
  gtk_list_box_set_selection_mode(
      context->recent_list,
      GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(
      context->recent_list,
      FALSE);
  gtk_widget_add_css_class(recent_list, "boxed-list");

  g_signal_connect(
      recent_list,
      "row-activated",
      G_CALLBACK(on_recent_activated),
      context);

  GtkEventController* recent_keys =
      gtk_event_controller_key_new();
  g_signal_connect(
      recent_keys,
      "key-pressed",
      G_CALLBACK(on_recent_key),
      context);
  gtk_widget_add_controller(recent_list, recent_keys);

  GtkWidget* recent_scroll =
      gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(recent_scroll),
      GTK_POLICY_NEVER,
      GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(recent_scroll),
      TRUE);
  gtk_scrolled_window_set_max_content_height(
      GTK_SCROLLED_WINDOW(recent_scroll),
      320);
  gtk_scrolled_window_set_min_content_width(
      GTK_SCROLLED_WINDOW(recent_scroll),
      420);
  gtk_widget_set_hexpand(recent_scroll, TRUE);
  gtk_scrolled_window_set_child(
      GTK_SCROLLED_WINDOW(recent_scroll),
      recent_list);

  gtk_box_append(GTK_BOX(recent_box), recent_scroll);
  gtk_box_append(GTK_BOX(welcome), recent_box);
  refresh_recent_documents(context);

  adw_status_page_set_child(
      ADW_STATUS_PAGE(status_page),
      welcome);

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

  g_signal_connect(
      window,
      "close-request",
      G_CALLBACK(on_window_close_request),
      context);

  g_signal_connect(
      context->tab_view,
      "close-page",
      G_CALLBACK(on_tab_close_page),
      context);

  g_signal_connect(
      context->tab_view,
      "page-detached",
      G_CALLBACK(on_page_detached),
      context);

  return GTK_WINDOW(window);
}

}  // namespace lienzo::gnome
