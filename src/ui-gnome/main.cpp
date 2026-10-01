#include "ui-gnome/application.hpp"

int main(
    int argc,
    char** argv) {
  AdwApplication* app =
      lienzo::gnome::create_application();

  const int result =
      g_application_run(
          G_APPLICATION(app),
          argc,
          argv);

  g_object_unref(app);

  return result;
}
