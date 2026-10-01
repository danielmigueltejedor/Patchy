
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
        src/ui-gnome/new_document_dialog.cpp
        src/ui-gnome/tool_palette.cpp
        src/ui-gnome/workspace.cpp
        src/ui-gnome/canvas.cpp
        src/ui-gnome/canvas_render.cpp
        src/ui-gnome/canvas_view.cpp
        src/ui-gnome/canvas_input.cpp
        src/ui-gnome/canvas_overlay.cpp
        src/ui-gnome/canvas_brush.cpp
        src/ui-gnome/canvas_move.cpp
        src/ui-gnome/canvas_selection.cpp
        src/ui-gnome/canvas_session.cpp
        src/ui-gnome/tools/selection_controller.cpp
        src/ui-gnome/tools/text_controller.cpp
        src/ui-gnome/tools/retouch_controller.cpp
        src/ui-gnome/tools/path_controller.cpp
        src/ui-gnome/brush_tips.cpp
        src/ui-gnome/tool_options_bar.cpp
        src/ui-gnome/inspector.cpp
        src/ui-gnome/layer_thumbnail.cpp
        src/ui-gnome/preferences_dialog.cpp
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

      set(
        LIENZO_GNOME_ICON_ROOT
        "${CMAKE_BINARY_DIR}/lienzo-gnome-icons"
      )

      file(
        MAKE_DIRECTORY
        "${LIENZO_GNOME_ICON_ROOT}/hicolor/256x256/apps"
      )

      configure_file(
        "${PROJECT_SOURCE_DIR}/src/ui/icons/lienzo-app.png"
        "${LIENZO_GNOME_ICON_ROOT}/hicolor/256x256/apps/com.nodalix.lienzo.png"
        COPYONLY
      )

      target_compile_definitions(
        lienzo_gnome
        PRIVATE
          LIENZO_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
          LIENZO_GNOME_ICON_ROOT="${LIENZO_GNOME_ICON_ROOT}"
      )

      target_link_libraries(
        lienzo_gnome
        PRIVATE
          PkgConfig::LIENZO_GTK4
          PkgConfig::LIENZO_ADWAITA
          patchy_core
          patchy_render
          patchy_psd
          patchy_formats
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
