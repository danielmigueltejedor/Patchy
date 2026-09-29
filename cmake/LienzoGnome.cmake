
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  option(
    LIENZO_BUILD_GNOME_FRONTEND
    "Build the native GTK4/libadwaita frontend on Linux"
    ON
  )

  if(LIENZO_BUILD_GNOME_FRONTEND)
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
      pkg_check_modules(
        LIENZO_GTK4
        QUIET
        IMPORTED_TARGET
        gtk4>=4.14
      )

      pkg_check_modules(
        LIENZO_ADWAITA
        QUIET
        IMPORTED_TARGET
        libadwaita-1>=1.5
      )
    endif()

    if(
      TARGET PkgConfig::LIENZO_GTK4
      AND
      TARGET PkgConfig::LIENZO_ADWAITA
    )
      add_executable(
        lienzo_gnome
        src/ui-gnome/main.cpp
        src/ui-gnome/application.cpp
        src/ui-gnome/main_window.cpp
        src/ui-gnome/primary_menu.cpp
      )

      set_target_properties(
        lienzo_gnome
        PROPERTIES
          OUTPUT_NAME "lienzo-gnome"
      )

      target_include_directories(
        lienzo_gnome
        PRIVATE
          "${PROJECT_SOURCE_DIR}/src"
      )

      target_link_libraries(
        lienzo_gnome
        PRIVATE
          PkgConfig::LIENZO_GTK4
          PkgConfig::LIENZO_ADWAITA
          patchy_core
          patchy_render
      )

      patchy_configure_target(lienzo_gnome)

      message(
        STATUS
        "Lienzo native GNOME frontend enabled"
      )
    else()
      message(
        WARNING
        "GTK4/libadwaita not found: lienzo_gnome will not be built"
      )
    endif()
  endif()
endif()
