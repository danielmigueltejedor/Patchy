#include "ui-gnome/file_portal.hpp"

#include <gio/gio.h>

#include <utility>

namespace lienzo::gnome {

namespace {

struct PendingPortal {
  GtkWindow* window{};
  std::string title;
  std::string initial_name;
  std::vector<PortalFilter> filters;
  bool save{false};
  PortalChosen chosen;
  GDBusConnection* bus{};
  guint subscription{0};
  bool finished{false};
};

void finish(
    PendingPortal* pending,
    std::optional<std::filesystem::path> path) {
  if (
      pending->finished) {
    return;
  }

  pending->finished = true;

  if (
      pending->subscription != 0 &&
      pending->bus != nullptr) {
    g_dbus_connection_signal_unsubscribe(
        pending->bus,
        pending->subscription);
  }

  if (pending->bus != nullptr) {
    g_object_unref(pending->bus);
    pending->bus = nullptr;
  }

  auto chosen =
      std::move(pending->chosen);

  delete pending;

  if (chosen) {
    chosen(std::move(path));
  }
}

void fallback_finished(
    GObject* source,
    GAsyncResult* result,
    gpointer data) {
  auto* pending =
      static_cast<PendingPortal*>(data);

  GError* error = nullptr;

  GFile* file =
      pending->save
          ? gtk_file_dialog_save_finish(
                GTK_FILE_DIALOG(source),
                result,
                &error)
          : gtk_file_dialog_open_finish(
                GTK_FILE_DIALOG(source),
                result,
                &error);

  if (file == nullptr) {
    g_clear_error(&error);
    finish(pending, std::nullopt);
    return;
  }

  char* raw =
      g_file_get_path(file);

  g_object_unref(file);

  if (raw == nullptr) {
    finish(pending, std::nullopt);
    return;
  }

  std::filesystem::path path(raw);
  g_free(raw);
  finish(pending, std::move(path));
}

void fallback_gtk(PendingPortal* pending) {
  GtkFileDialog* dialog =
      gtk_file_dialog_new();

  gtk_file_dialog_set_title(
      dialog,
      pending->title.c_str());

  if (
      pending->save &&
      !pending->initial_name.empty()) {
    gtk_file_dialog_set_initial_name(
        dialog,
        pending->initial_name.c_str());
  }

  if (pending->save) {
    gtk_file_dialog_save(
        dialog,
        pending->window,
        nullptr,
        fallback_finished,
        pending);
  } else {
    gtk_file_dialog_open(
        dialog,
        pending->window,
        nullptr,
        fallback_finished,
        pending);
  }

  g_object_unref(dialog);
}

void on_response(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant* parameters,
    gpointer data) {
  auto* pending =
      static_cast<PendingPortal*>(data);

  guint32 response = 1;
  GVariant* results = nullptr;

  g_variant_get(
      parameters,
      "(u@a{sv})",
      &response,
      &results);

  std::optional<std::filesystem::path> path;

  if (
      response == 0 &&
      results != nullptr) {
    GVariant* uris =
        g_variant_lookup_value(
            results,
            "uris",
            G_VARIANT_TYPE("as"));

    if (
        uris != nullptr &&
        g_variant_n_children(uris) > 0) {
      const char* uri = nullptr;

      g_variant_get_child(
          uris,
          0,
          "&s",
          &uri);

      if (uri != nullptr) {
        char* filename =
            g_filename_from_uri(
                uri,
                nullptr,
                nullptr);

        if (filename != nullptr) {
          path = std::filesystem::path(filename);
          g_free(filename);
        }
      }

      g_variant_unref(uris);
    }
  }

  if (results != nullptr) {
    g_variant_unref(results);
  }

  finish(pending, std::move(path));
}

void on_portal_call(
    GObject* source,
    GAsyncResult* result,
    gpointer data) {
  auto* pending =
      static_cast<PendingPortal*>(data);

  GError* error = nullptr;

  GVariant* reply =
      g_dbus_connection_call_finish(
          G_DBUS_CONNECTION(source),
          result,
          &error);

  if (reply == nullptr) {
    g_clear_error(&error);

    if (
        pending->subscription != 0) {
      g_dbus_connection_signal_unsubscribe(
          pending->bus,
          pending->subscription);

      pending->subscription = 0;
    }

    fallback_gtk(pending);
    return;
  }

  g_variant_unref(reply);
}

void start(
    GtkWindow* window,
    const char* title,
    const char* initial_name,
    std::vector<PortalFilter> filters,
    bool save,
    PortalChosen chosen) {
  auto* pending =
      new PendingPortal;

  pending->window = window;
  pending->title = title != nullptr ? title : "";
  pending->initial_name =
      initial_name != nullptr ? initial_name : "";
  pending->filters = std::move(filters);
  pending->save = save;
  pending->chosen = std::move(chosen);

  GError* error = nullptr;

  pending->bus =
      g_bus_get_sync(
          G_BUS_TYPE_SESSION,
          nullptr,
          &error);

  if (pending->bus == nullptr) {
    g_clear_error(&error);
    fallback_gtk(pending);
    return;
  }

  const char* unique =
      g_dbus_connection_get_unique_name(
          pending->bus);

  std::string sender =
      unique != nullptr && unique[0] == ':'
          ? unique + 1
          : "lienzo";

  for (char& character : sender) {
    if (character == '.') {
      character = '_';
    }
  }

  const std::string token =
      "lienzo" +
      std::to_string(g_random_int());

  const std::string request_path =
      "/org/freedesktop/portal/desktop/request/" +
      sender + "/" + token;

  pending->subscription =
      g_dbus_connection_signal_subscribe(
          pending->bus,
          "org.freedesktop.portal.Desktop",
          "org.freedesktop.portal.Request",
          "Response",
          request_path.c_str(),
          nullptr,
          G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
          on_response,
          pending,
          nullptr);

  GVariantBuilder options;

  g_variant_builder_init(
      &options,
      G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add(
      &options,
      "{sv}",
      "handle_token",
      g_variant_new_string(token.c_str()));

  g_variant_builder_add(
      &options,
      "{sv}",
      "modal",
      g_variant_new_boolean(TRUE));

  if (
      save &&
      !pending->initial_name.empty()) {
    g_variant_builder_add(
        &options,
        "{sv}",
        "current_name",
        g_variant_new_string(
            pending->initial_name.c_str()));
  }

  if (!pending->filters.empty()) {
    GVariantBuilder filter_list;

    g_variant_builder_init(
        &filter_list,
        G_VARIANT_TYPE("a(sa(us))"));

    for (const auto& filter : pending->filters) {
      GVariantBuilder globs;

      g_variant_builder_init(
          &globs,
          G_VARIANT_TYPE("a(us)"));

      for (const auto& glob : filter.globs) {
        g_variant_builder_add(
            &globs,
            "(us)",
            0,
            glob.c_str());
      }

      g_variant_builder_add(
          &filter_list,
          "(sa(us))",
          filter.name.c_str(),
          &globs);
    }

    g_variant_builder_add(
        &options,
        "{sv}",
        "filters",
        g_variant_builder_end(&filter_list));
  }

  g_dbus_connection_call(
      pending->bus,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.FileChooser",
      save ? "SaveFile" : "OpenFile",
      g_variant_new(
          "(ss@a{sv})",
          "",
          pending->title.c_str(),
          g_variant_builder_end(&options)),
      G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      nullptr,
      on_portal_call,
      pending);
}

}  // namespace

void present_portal_open(
    GtkWindow* window,
    const char* title,
    std::vector<PortalFilter> filters,
    PortalChosen chosen) {
  start(
      window,
      title,
      nullptr,
      std::move(filters),
      false,
      std::move(chosen));
}

void present_portal_save(
    GtkWindow* window,
    const char* title,
    const char* initial_name,
    PortalChosen chosen) {
  start(
      window,
      title,
      initial_name,
      {},
      true,
      std::move(chosen));
}

}  // namespace lienzo::gnome
