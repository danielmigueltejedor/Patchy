#include "ui-gnome/preferences_dialog.hpp"

#include <adwaita.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <string>

namespace lienzo::gnome {

namespace {

std::string preferences_path() {
  gchar* directory =
      g_build_filename(
          g_get_user_config_dir(),
          "lienzo",
          nullptr);

  g_mkdir_with_parents(
      directory,
      0700);

  gchar* path =
      g_build_filename(
          directory,
          "preferences.ini",
          nullptr);

  std::string result(path);

  g_free(path);
  g_free(directory);

  return result;
}

GKeyFile* load_preferences() {
  GKeyFile* file =
      g_key_file_new();

  const auto path =
      preferences_path();

  g_key_file_load_from_file(
      file,
      path.c_str(),
      G_KEY_FILE_KEEP_COMMENTS,
      nullptr);

  return file;
}

void save_preferences(
    GKeyFile* file) {
  const auto path =
      preferences_path();

  g_key_file_save_to_file(
      file,
      path.c_str(),
      nullptr);
}

bool get_bool(
    const char* group,
    const char* key,
    bool fallback) {
  GKeyFile* file =
      load_preferences();

  GError* error = nullptr;

  const gboolean value =
      g_key_file_get_boolean(
          file,
          group,
          key,
          &error);

  g_key_file_unref(file);

  if (error != nullptr) {
    g_error_free(error);
    return fallback;
  }

  return value;
}

std::string get_string(
    const char* group,
    const char* key,
    const char* fallback) {
  GKeyFile* file =
      load_preferences();

  gchar* value =
      g_key_file_get_string(
          file,
          group,
          key,
          nullptr);

  g_key_file_unref(file);

  if (value == nullptr) {
    return fallback;
  }

  std::string result(value);

  g_free(value);

  return result;
}

int get_int(
    const char* group,
    const char* key,
    int fallback) {
  GKeyFile* file =
      load_preferences();

  GError* error = nullptr;

  const int value =
      g_key_file_get_integer(
          file,
          group,
          key,
          &error);

  g_key_file_unref(file);

  if (error != nullptr) {
    g_error_free(error);
    return fallback;
  }

  return value;
}

void set_int(
    const char* group,
    const char* key,
    int value) {
  GKeyFile* file =
      load_preferences();

  g_key_file_set_integer(
      file,
      group,
      key,
      value);

  save_preferences(file);
  g_key_file_unref(file);
}

void set_bool(
    const char* group,
    const char* key,
    bool value) {
  GKeyFile* file =
      load_preferences();

  g_key_file_set_boolean(
      file,
      group,
      key,
      value);

  save_preferences(file);

  g_key_file_unref(file);
}

void set_string(
    const char* group,
    const char* key,
    const char* value) {
  GKeyFile* file =
      load_preferences();

  g_key_file_set_string(
      file,
      group,
      key,
      value);

  save_preferences(file);

  g_key_file_unref(file);
}

struct BoolBinding {
  const char* group{};
  const char* key{};
};

void switch_changed(
    GObject* object,
    GParamSpec*,
    gpointer data) {
  auto* binding =
      static_cast<BoolBinding*>(data);

  set_bool(
      binding->group,
      binding->key,
      adw_switch_row_get_active(
          ADW_SWITCH_ROW(object)));
}

GtkWidget* preference_switch(
    const char* title,
    const char* subtitle,
    const char* group,
    const char* key,
    bool fallback) {
  GtkWidget* row =
      adw_switch_row_new();

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(row),
      title);

  if (subtitle != nullptr) {
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(row),
        subtitle);
  }

  adw_switch_row_set_active(
      ADW_SWITCH_ROW(row),
      get_bool(
          group,
          key,
          fallback));

  auto* binding =
      new BoolBinding{
          group,
          key};

  g_signal_connect_data(
      row,
      "notify::active",
      G_CALLBACK(switch_changed),
      binding,
      [](gpointer data, GClosure*) {
        delete static_cast<BoolBinding*>(
            data);
      },
      GConnectFlags(0));

  return row;
}

void autosave_interval_changed(
    GtkSpinButton* spin,
    gpointer) {
  set_int(
      "autosave",
      "intervalMinutes",
      gtk_spin_button_get_value_as_int(
          spin));
}

void scheme_changed(
    GObject* object,
    GParamSpec*,
    gpointer) {
  auto* row =
      ADW_COMBO_ROW(object);

  const guint selected =
      adw_combo_row_get_selected(row);

  const char* token = "system";

  AdwColorScheme scheme =
      ADW_COLOR_SCHEME_DEFAULT;

  if (selected == 1) {
    token = "dark";
    scheme =
        ADW_COLOR_SCHEME_FORCE_DARK;
  } else if (selected == 2) {
    token = "light";
    scheme =
        ADW_COLOR_SCHEME_FORCE_LIGHT;
  }

  set_string(
      "application",
      "colorScheme",
      token);

  adw_style_manager_set_color_scheme(
      adw_style_manager_get_default(),
      scheme);
}

}  // namespace

void present_preferences_dialog(
    GtkWidget* parent) {
  AdwPreferencesDialog* dialog =
      ADW_PREFERENCES_DIALOG(
          adw_preferences_dialog_new());

  adw_dialog_set_title(
      ADW_DIALOG(dialog),
      "Preferencias");

  GtkWidget* application =
      adw_preferences_page_new();

  adw_preferences_page_set_title(
      ADW_PREFERENCES_PAGE(application),
      "Aplicación");

  adw_preferences_page_set_icon_name(
      ADW_PREFERENCES_PAGE(application),
      "preferences-system-symbolic");

  GtkWidget* appearance =
      adw_preferences_group_new();

  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(appearance),
      "Apariencia");

  GtkWidget* scheme =
      adw_combo_row_new();

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(scheme),
      "Esquema de color");

  const char* schemes[] = {
      "Seguir el sistema",
      "Oscuro",
      "Claro",
      nullptr};

  GtkStringList* scheme_model =
      gtk_string_list_new(schemes);

  adw_combo_row_set_model(
      ADW_COMBO_ROW(scheme),
      G_LIST_MODEL(scheme_model));

  g_object_unref(scheme_model);

  const auto saved_scheme =
      get_string(
          "application",
          "colorScheme",
          "system");

  adw_combo_row_set_selected(
      ADW_COMBO_ROW(scheme),
      saved_scheme == "dark"
          ? 1
          : saved_scheme == "light"
                ? 2
                : 0);

  g_signal_connect(
      scheme,
      "notify::selected",
      G_CALLBACK(scheme_changed),
      nullptr);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(appearance),
      scheme);

  GtkWidget* files =
      adw_preferences_group_new();

  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(files),
      "Archivos y recuperación");

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(files),
      preference_switch(
          "Buscar actualizaciones al iniciar",
          nullptr,
          "updates",
          "checkOnStartup",
          true));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(files),
      preference_switch(
          "Guardado automático",
          "Guarda periódicamente los documentos abiertos",
          "autosave",
          "enabled",
          true));

  GtkWidget* autosave_interval =
      adw_spin_row_new_with_range(
          1,
          60,
          1);

  adw_preferences_row_set_title(
      ADW_PREFERENCES_ROW(autosave_interval),
      "Intervalo de guardado automático");

  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(autosave_interval),
      "Minutos entre guardados");

  adw_spin_row_set_value(
      ADW_SPIN_ROW(autosave_interval),
      get_int(
          "autosave",
          "intervalMinutes",
          5));

  g_signal_connect(
      autosave_interval,
      "value-changed",
      G_CALLBACK(autosave_interval_changed),
      nullptr);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(files),
      autosave_interval);

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(files),
      preference_switch(
          "Recuperación automática",
          "Mantiene copias temporales de documentos modificados",
          "recovery",
          "enabled",
          true));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(files),
      preference_switch(
          "Mostrar avisos al importar PSD",
          nullptr,
          "imports",
          "showPsdWarningsAndInfo",
          false));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(files),
      preference_switch(
          "Mostrar revelado al abrir RAW",
          nullptr,
          "imports",
          "showRawDevelopDialog",
          true));

  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(application),
      ADW_PREFERENCES_GROUP(appearance));

  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(application),
      ADW_PREFERENCES_GROUP(files));

  GtkWidget* pen =
      adw_preferences_page_new();

  adw_preferences_page_set_title(
      ADW_PREFERENCES_PAGE(pen),
      "Lápiz");

  adw_preferences_page_set_icon_name(
      ADW_PREFERENCES_PAGE(pen),
      "input-tablet-symbolic");

  GtkWidget* pen_group =
      adw_preferences_group_new();

  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(pen_group),
      "Entrada de lápiz");

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(pen_group),
      preference_switch(
          "Activar entrada de lápiz",
          nullptr,
          "pen",
          "enabled",
          true));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(pen_group),
      preference_switch(
          "Presión controla el tamaño",
          nullptr,
          "pen",
          "pressureSize",
          true));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(pen_group),
      preference_switch(
          "Presión controla la opacidad",
          nullptr,
          "pen",
          "pressureOpacity",
          false));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(pen_group),
      preference_switch(
          "Usar la punta de borrador",
          nullptr,
          "pen",
          "eraserTip",
          true));

  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(pen),
      ADW_PREFERENCES_GROUP(pen_group));

  GtkWidget* canvas =
      adw_preferences_page_new();

  adw_preferences_page_set_title(
      ADW_PREFERENCES_PAGE(canvas),
      "Cuadrícula y guías");

  adw_preferences_page_set_icon_name(
      ADW_PREFERENCES_PAGE(canvas),
      "view-grid-symbolic");

  GtkWidget* canvas_group =
      adw_preferences_group_new();

  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(canvas_group),
      "Ayudas del lienzo");

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(canvas_group),
      preference_switch(
          "Mostrar reglas",
          nullptr,
          "view",
          "showRulers",
          false));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(canvas_group),
      preference_switch(
          "Mostrar cuadrícula",
          nullptr,
          "view",
          "showGrid",
          false));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(canvas_group),
      preference_switch(
          "Mostrar guías",
          nullptr,
          "view",
          "showGuides",
          true));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(canvas_group),
      preference_switch(
          "Bloquear guías",
          nullptr,
          "view",
          "lockGuides",
          false));

  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(canvas_group),
      preference_switch(
          "Ajustar",
          nullptr,
          "view",
          "snap",
          true));

  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(canvas),
      ADW_PREFERENCES_GROUP(canvas_group));

  adw_preferences_dialog_add(
      dialog,
      ADW_PREFERENCES_PAGE(application));

  adw_preferences_dialog_add(
      dialog,
      ADW_PREFERENCES_PAGE(pen));

  adw_preferences_dialog_add(
      dialog,
      ADW_PREFERENCES_PAGE(canvas));

  adw_dialog_present(
      ADW_DIALOG(dialog),
      parent);
}

bool autosave_enabled() {
  return get_bool(
      "autosave",
      "enabled",
      true);
}

void set_autosave_enabled(
    bool enabled) {
  set_bool(
      "autosave",
      "enabled",
      enabled);
}

int autosave_interval_minutes() {
  return std::clamp(
      get_int(
          "autosave",
          "intervalMinutes",
          5),
      1,
      60);
}

}  // namespace lienzo::gnome
