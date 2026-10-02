#include "ui-gnome/adjustment_dialog.hpp"

#include "core/adjustment_layer.hpp"

#include <adwaita.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace lienzo::gnome {
namespace {

struct Editor {
  patchy::Document* document{};
  patchy::LayerId id{};
  CanvasView canvas;
  std::function<void()> after_apply;
  patchy::AdjustmentSettings settings{};
  patchy::CurvesChannel curves_channel{
      patchy::CurvesChannel::Rgb};
  patchy::LevelsChannel levels_channel{
      patchy::LevelsChannel::Rgb};
  GtkWidget* graph{};
  GtkWidget* gradient{};
  int drag_index{-1};
  bool updating{false};

  GtkWidget* black_input{};
  GtkWidget* white_input{};
  GtkWidget* gamma{};
  GtkWidget* black_output{};
  GtkWidget* white_output{};
  GtkWidget* brightness{};
  GtkWidget* contrast{};
  GtkWidget* hue{};
  GtkWidget* saturation{};
  GtkWidget* lightness{};
  GtkWidget* colorize{};
  GtkWidget* colorize_hue{};
  GtkWidget* colorize_saturation{};
  GtkWidget* colorize_lightness{};
  GtkWidget* cyan_red{};
  GtkWidget* magenta_green{};
  GtkWidget* yellow_blue{};
  GtkWidget* posterize{};
  GtkWidget* threshold{};
};

patchy::CurveControlPoints curve_points(const Editor* editor) {
  return patchy::curve_points_for_channel(
      editor->settings.curves,
      editor->curves_channel);
}

void set_curve_points(
    Editor* editor,
    patchy::CurveControlPoints points) {
  patchy::set_curve_points_for_channel(
      editor->settings.curves,
      editor->curves_channel,
      std::move(points));
}

int graph_input(double x, int width) {
  if (width <= 1) {
    return 0;
  }

  return std::clamp(
      static_cast<int>(
          std::lround(x * 255.0 / (width - 1))),
      0,
      255);
}

int graph_output(double y, int height) {
  if (height <= 1) {
    return 0;
  }

  return std::clamp(
      static_cast<int>(
          std::lround(
              255.0 - y * 255.0 / (height - 1))),
      0,
      255);
}

double graph_x(int input, int width) {
  return input * (width - 1) / 255.0;
}

double graph_y(int output, int height) {
  return (255 - output) * (height - 1) / 255.0;
}

int nearest_point(
    const patchy::CurveControlPoints& points,
    double x,
    double y,
    int width,
    int height) {
  int found = -1;
  double best = 12.0 * 12.0;

  for (int index = 0; index < static_cast<int>(points.size()); ++index) {
    const double dx = x - graph_x(points[index].input, width);
    const double dy = y - graph_y(points[index].output, height);
    const double distance = dx * dx + dy * dy;

    if (distance <= best) {
      best = distance;
      found = index;
    }
  }

  return found;
}

void redraw(Editor* editor) {
  if (editor->graph != nullptr) {
    gtk_widget_queue_draw(editor->graph);
  }

  if (editor->gradient != nullptr) {
    gtk_widget_queue_draw(editor->gradient);
  }
}

void move_curve_point(
    Editor* editor,
    int index,
    int input,
    int output) {
  auto points = curve_points(editor);

  if (index < 0 || index >= static_cast<int>(points.size())) {
    return;
  }

  const bool endpoint =
      index == 0 || index + 1 == static_cast<int>(points.size());

  if (endpoint) {
    input = points[index].input;
  } else {
    const int lower = points[index - 1].input + 1;
    const int upper = points[index + 1].input - 1;

    if (lower > upper) {
      return;
    }

    input = std::clamp(input, lower, upper);
  }

  points[index].input = input;
  points[index].output = std::clamp(output, 0, 255);
  set_curve_points(editor, std::move(points));
  redraw(editor);
}

int scale_value(GtkWidget* row) {
  return static_cast<int>(
      std::lround(adw_spin_row_get_value(ADW_SPIN_ROW(row))));
}

void read_controls(Editor* editor) {
  if (editor->updating) {
    return;
  }

  switch (editor->settings.kind) {
    case patchy::AdjustmentKind::Levels: {
      patchy::LevelsRecord record =
          patchy::levels_record_for_channel(
              editor->settings.levels,
              editor->levels_channel);
      record.black_input = scale_value(editor->black_input);
      record.white_input = scale_value(editor->white_input);
      record.gamma_percent = scale_value(editor->gamma);
      record.black_output = scale_value(editor->black_output);
      record.white_output = scale_value(editor->white_output);
      record = patchy::clamp_levels_record(record);
      patchy::set_levels_record_for_channel(
          editor->settings.levels,
          editor->levels_channel,
          record);
      editor->updating = true;
      adw_spin_row_set_value(ADW_SPIN_ROW(editor->black_input), record.black_input);
      adw_spin_row_set_value(ADW_SPIN_ROW(editor->white_input), record.white_input);
      adw_spin_row_set_value(ADW_SPIN_ROW(editor->gamma), record.gamma_percent);
      adw_spin_row_set_value(ADW_SPIN_ROW(editor->black_output), record.black_output);
      adw_spin_row_set_value(ADW_SPIN_ROW(editor->white_output), record.white_output);
      editor->updating = false;
      break;
    }
    case patchy::AdjustmentKind::BrightnessContrast:
      editor->settings.brightness_contrast.brightness =
          scale_value(editor->brightness);
      editor->settings.brightness_contrast.contrast =
          scale_value(editor->contrast);
      editor->settings.brightness_contrast =
          patchy::clamp_brightness_contrast(
              editor->settings.brightness_contrast);
      break;
    case patchy::AdjustmentKind::HueSaturation:
      editor->settings.hue_saturation.hue_shift =
          scale_value(editor->hue);
      editor->settings.hue_saturation.saturation_delta =
          scale_value(editor->saturation);
      editor->settings.hue_saturation.lightness_delta =
          scale_value(editor->lightness);
      editor->settings.hue_saturation.colorize =
          adw_switch_row_get_active(ADW_SWITCH_ROW(editor->colorize));
      editor->settings.hue_saturation.colorize_hue =
          scale_value(editor->colorize_hue);
      editor->settings.hue_saturation.colorize_saturation =
          scale_value(editor->colorize_saturation);
      editor->settings.hue_saturation.colorize_lightness =
          scale_value(editor->colorize_lightness);
      break;
    case patchy::AdjustmentKind::ColorBalance:
      editor->settings.color_balance.cyan_red =
          scale_value(editor->cyan_red);
      editor->settings.color_balance.magenta_green =
          scale_value(editor->magenta_green);
      editor->settings.color_balance.yellow_blue =
          scale_value(editor->yellow_blue);
      break;
    case patchy::AdjustmentKind::Posterize:
      editor->settings.posterize.levels =
          scale_value(editor->posterize);
      break;
    case patchy::AdjustmentKind::Threshold:
      editor->settings.threshold.level =
          scale_value(editor->threshold);
      break;
    case patchy::AdjustmentKind::Curves:
    case patchy::AdjustmentKind::Invert:
      break;
  }

  redraw(editor);
}

void scale_changed(GObject*, GParamSpec*, gpointer data) {
  read_controls(static_cast<Editor*>(data));
}

void colorize_changed(GObject*, GParamSpec*, gpointer data) {
  read_controls(static_cast<Editor*>(data));
}

GtkWidget* add_scale(
    GtkWidget* parent,
    const char* caption,
    double minimum,
    double maximum,
    double value,
    Editor* editor) {
  GtkWidget* row =
      adw_spin_row_new_with_range(minimum, maximum, 1.0);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), caption);
  adw_spin_row_set_digits(ADW_SPIN_ROW(row), 0);
  adw_spin_row_set_value(ADW_SPIN_ROW(row), value);
  g_signal_connect(row, "notify::value", G_CALLBACK(scale_changed), editor);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(parent), row);
  return row;
}

void paint_grid(cairo_t* cr, int width, int height, const GdkRGBA& color) {
  cairo_set_source_rgba(
      cr,
      color.red,
      color.green,
      color.blue,
      0.18);
  cairo_set_line_width(cr, 1.0);

  for (int step = 1; step < 4; ++step) {
    const double x = width * step / 4.0;
    const double y = height * step / 4.0;
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, height);
    cairo_move_to(cr, 0, y);
    cairo_line_to(cr, width, y);
  }

  cairo_stroke(cr);
}

void paint_transfer(
    cairo_t* cr,
    int width,
    int height,
    const std::array<std::uint8_t, 256>& lut,
    double red,
    double green,
    double blue) {
  cairo_set_source_rgba(cr, red, green, blue, 1.0);
  cairo_set_line_width(cr, 1.6);
  cairo_move_to(
      cr,
      graph_x(0, width),
      graph_y(lut[0], height));

  for (int input = 1; input < 256; ++input) {
    cairo_line_to(
        cr,
        graph_x(input, width),
        graph_y(lut[input], height));
  }

  cairo_stroke(cr);
}

void draw_graph(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  GdkRGBA color;
  gtk_widget_get_color(editor->graph, &color);
  paint_grid(cr, width, height, color);

  if (editor->settings.kind == patchy::AdjustmentKind::Curves) {
    const auto points = curve_points(editor);
    const auto lut = patchy::build_curve_lut(points);
    double red = color.red;
    double green = color.green;
    double blue = color.blue;

    if (editor->curves_channel == patchy::CurvesChannel::Red) {
      red = 0.86;
      green = 0.25;
      blue = 0.22;
    } else if (editor->curves_channel == patchy::CurvesChannel::Green) {
      red = 0.20;
      green = 0.62;
      blue = 0.28;
    } else if (editor->curves_channel == patchy::CurvesChannel::Blue) {
      red = 0.22;
      green = 0.42;
      blue = 0.86;
    }

    paint_transfer(cr, width, height, lut, red, green, blue);
    cairo_set_source_rgba(cr, red, green, blue, 1.0);

    for (const auto& point : points) {
      cairo_arc(
          cr,
          graph_x(point.input, width),
          graph_y(point.output, height),
          4.0,
          0.0,
          2.0 * G_PI);
      cairo_fill(cr);
    }

    return;
  }

  if (editor->settings.kind == patchy::AdjustmentKind::HueSaturation) {
    cairo_pattern_t* rainbow =
        cairo_pattern_create_linear(0, height * 0.45, width, height * 0.45);
    const double stops[][3] = {
        {1, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
        {0, 1, 1},
        {0, 0, 1},
        {1, 0, 1},
        {1, 0, 0},
    };

    for (int index = 0; index < 7; ++index) {
      cairo_pattern_add_color_stop_rgb(
          rainbow,
          index / 6.0,
          stops[index][0],
          stops[index][1],
          stops[index][2]);
    }

    cairo_set_source(cr, rainbow);
    cairo_rectangle(cr, 0, height * 0.38, width, height * 0.16);
    cairo_fill(cr);
    cairo_pattern_destroy(rainbow);
    return;
  }

  const auto lut = patchy::build_adjustment_lut(editor->settings);

  if (!lut.has_value()) {
    return;
  }

  const auto& series =
      editor->settings.kind == patchy::AdjustmentKind::Levels &&
              editor->levels_channel == patchy::LevelsChannel::Green
          ? lut->green
          : editor->settings.kind == patchy::AdjustmentKind::Levels &&
                    editor->levels_channel == patchy::LevelsChannel::Blue
                ? lut->blue
                : editor->settings.kind == patchy::AdjustmentKind::Levels &&
                          editor->levels_channel == patchy::LevelsChannel::Red
                      ? lut->red
                      : lut->red;
  paint_transfer(cr, width, height, series, color.red, color.green, color.blue);

  if (editor->settings.kind == patchy::AdjustmentKind::ColorBalance) {
    paint_transfer(cr, width, height, lut->green, 0.20, 0.62, 0.28);
    paint_transfer(cr, width, height, lut->blue, 0.22, 0.42, 0.86);
  }
}

void draw_gradient(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  cairo_pattern_t* pattern =
      cairo_pattern_create_linear(0, 0, width, 0);
  double end_red = 1;
  double end_green = 1;
  double end_blue = 1;

  if (
      (editor->settings.kind == patchy::AdjustmentKind::Curves &&
       editor->curves_channel == patchy::CurvesChannel::Red) ||
      (editor->settings.kind == patchy::AdjustmentKind::Levels &&
       editor->levels_channel == patchy::LevelsChannel::Red)) {
    end_green = 0;
    end_blue = 0;
  } else if (
      (editor->settings.kind == patchy::AdjustmentKind::Curves &&
       editor->curves_channel == patchy::CurvesChannel::Green) ||
      (editor->settings.kind == patchy::AdjustmentKind::Levels &&
       editor->levels_channel == patchy::LevelsChannel::Green)) {
    end_red = 0;
    end_blue = 0;
  } else if (
      (editor->settings.kind == patchy::AdjustmentKind::Curves &&
       editor->curves_channel == patchy::CurvesChannel::Blue) ||
      (editor->settings.kind == patchy::AdjustmentKind::Levels &&
       editor->levels_channel == patchy::LevelsChannel::Blue)) {
    end_red = 0;
    end_green = 0;
  }

  cairo_pattern_add_color_stop_rgb(pattern, 0, 0, 0, 0);
  cairo_pattern_add_color_stop_rgb(
      pattern,
      1,
      end_red,
      end_green,
      end_blue);
  cairo_set_source(cr, pattern);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);
  cairo_pattern_destroy(pattern);
}

void curve_drag_begin(
    GtkGestureDrag* gesture,
    double start_x,
    double start_y,
    gpointer data) {
  auto* editor = static_cast<Editor*>(data);

  if (editor->settings.kind != patchy::AdjustmentKind::Curves) {
    return;
  }

  const int width = gtk_widget_get_width(editor->graph);
  const int height = gtk_widget_get_height(editor->graph);
  auto points = curve_points(editor);
  int index = nearest_point(points, start_x, start_y, width, height);

  if (index < 0 && points.size() < 19) {
    const int input = graph_input(start_x, width);
    const int output = graph_output(start_y, height);
    bool occupied = false;

    for (const auto& point : points) {
      if (std::abs(point.input - input) < 2) {
        occupied = true;
        break;
      }
    }

    if (!occupied) {
      points.push_back({input, output});
      set_curve_points(editor, std::move(points));
      points = curve_points(editor);

      for (int candidate = 0; candidate < static_cast<int>(points.size()); ++candidate) {
        if (points[candidate].input == input) {
          index = candidate;
          break;
        }
      }
    }
  }

  editor->drag_index = index;
  gtk_gesture_set_state(
      GTK_GESTURE(gesture),
      index >= 0 ? GTK_EVENT_SEQUENCE_CLAIMED : GTK_EVENT_SEQUENCE_DENIED);
  redraw(editor);
}

void curve_drag_update(
    GtkGestureDrag* gesture,
    double offset_x,
    double offset_y,
    gpointer data) {
  auto* editor = static_cast<Editor*>(data);

  if (editor->drag_index < 0) {
    return;
  }

  double start_x = 0;
  double start_y = 0;
  gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
  const int width = gtk_widget_get_width(editor->graph);
  const int height = gtk_widget_get_height(editor->graph);
  move_curve_point(
      editor,
      editor->drag_index,
      graph_input(start_x + offset_x, width),
      graph_output(start_y + offset_y, height));
}

void curve_drag_end(GtkGestureDrag*, double, double, gpointer data) {
  static_cast<Editor*>(data)->drag_index = -1;
}

void curve_delete(
    GtkGestureClick* gesture,
    int,
    double x,
    double y,
    gpointer data) {
  auto* editor = static_cast<Editor*>(data);

  if (editor->settings.kind != patchy::AdjustmentKind::Curves) {
    return;
  }

  const int width = gtk_widget_get_width(editor->graph);
  const int height = gtk_widget_get_height(editor->graph);
  auto points = curve_points(editor);
  const int index = nearest_point(points, x, y, width, height);

  if (index <= 0 || index + 1 >= static_cast<int>(points.size())) {
    return;
  }

  points.erase(points.begin() + index);
  set_curve_points(editor, std::move(points));
  gtk_gesture_set_state(
      GTK_GESTURE(gesture),
      GTK_EVENT_SEQUENCE_CLAIMED);
  redraw(editor);
}

void load_level_scales(Editor* editor) {
  const auto record = patchy::levels_record_for_channel(
      editor->settings.levels,
      editor->levels_channel);
  editor->updating = true;
  adw_spin_row_set_value(ADW_SPIN_ROW(editor->black_input), record.black_input);
  adw_spin_row_set_value(ADW_SPIN_ROW(editor->white_input), record.white_input);
  adw_spin_row_set_value(ADW_SPIN_ROW(editor->gamma), record.gamma_percent);
  adw_spin_row_set_value(ADW_SPIN_ROW(editor->black_output), record.black_output);
  adw_spin_row_set_value(ADW_SPIN_ROW(editor->white_output), record.white_output);
  editor->updating = false;
  redraw(editor);
}

void channel_chosen(GtkToggleButton* button, gpointer data) {
  if (!gtk_toggle_button_get_active(button)) {
    return;
  }

  auto* editor = static_cast<Editor*>(data);
  const int index = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(button), "lienzo-channel"));

  if (editor->settings.kind == patchy::AdjustmentKind::Curves) {
    editor->curves_channel = static_cast<patchy::CurvesChannel>(index);
  } else {
    editor->levels_channel = static_cast<patchy::LevelsChannel>(index);
    load_level_scales(editor);
  }

  redraw(editor);
}

GtkWidget* channel_row(Editor* editor, bool curves) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  const char* labels[] = {"RGB", "Rojo", "Verde", "Azul"};
  GtkWidget* group = nullptr;

  for (int index = 0; index < 4; ++index) {
    GtkWidget* button = gtk_toggle_button_new_with_label(labels[index]);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(group));
    gtk_widget_add_css_class(button, "pill");
    gtk_widget_add_css_class(button, "flat");
    group = button;
    g_object_set_data(
        G_OBJECT(button),
        "lienzo-channel",
        GINT_TO_POINTER(index));
    if (index == 0) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    }
    g_signal_connect(button, "toggled", G_CALLBACK(channel_chosen), editor);
    gtk_box_append(GTK_BOX(box), button);
    (void)curves;
  }

  return box;
}

void close_editor_later(GtkWidget* dialog) {
  if (dialog == nullptr) {
    return;
  }

  g_object_ref(dialog);
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* widget = GTK_WIDGET(data);

        if (ADW_IS_DIALOG(widget)) {
          adw_dialog_close(ADW_DIALOG(widget));
        }

        g_object_unref(widget);
        return G_SOURCE_REMOVE;
      },
      dialog);
}

void apply_editor(GtkButton*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  read_controls(editor);

  if (editor->canvas.checkpoint) {
    editor->canvas.checkpoint();
  }

  auto* layer = editor->document->find_layer(editor->id);

  if (layer != nullptr) {
    patchy::configure_adjustment_layer(*layer, editor->settings);
  }

  if (editor->after_apply) {
    editor->after_apply();
  }

  GtkWidget* dialog = gtk_widget_get_ancestor(
      editor->graph,
      ADW_TYPE_DIALOG);

  close_editor_later(dialog);
}

void close_editor(GtkButton*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  GtkWidget* dialog = gtk_widget_get_ancestor(
      editor->graph,
      ADW_TYPE_DIALOG);
  close_editor_later(dialog);
}

}  // namespace

void present_adjustment_editor(
    patchy::Document& document,
    patchy::LayerId id,
    const CanvasView& canvas,
    GtkWidget* parent,
    std::function<void()> after_apply) {
  const auto* layer = std::as_const(document).find_layer(id);

  if (layer == nullptr || !patchy::layer_is_adjustment(*layer)) {
    return;
  }

  auto settings = patchy::adjustment_settings_from_layer(*layer);

  if (!settings.has_value()) {
    return;
  }

  auto* editor = new Editor;
  editor->document = &document;
  editor->id = id;
  editor->canvas = canvas;
  editor->after_apply = std::move(after_apply);
  editor->settings = *settings;

  GtkWidget* page = adw_preferences_page_new();
  GtkWidget* graph_group = adw_preferences_group_new();
  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(graph_group),
      "Gráfica");
  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(page),
      ADW_PREFERENCES_GROUP(graph_group));
  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 6);
  gtk_widget_set_margin_bottom(content, 6);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(graph_group), content);
  GtkWidget* values = adw_preferences_group_new();
  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(values),
      "Valores");
  adw_preferences_page_add(
      ADW_PREFERENCES_PAGE(page),
      ADW_PREFERENCES_GROUP(values));

  const bool curves =
      editor->settings.kind == patchy::AdjustmentKind::Curves;
  const bool levels =
      editor->settings.kind == patchy::AdjustmentKind::Levels;

  if (curves || levels) {
    gtk_box_append(GTK_BOX(content), channel_row(editor, curves));
  }

  editor->graph = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(
      GTK_DRAWING_AREA(editor->graph),
      280);
  gtk_drawing_area_set_content_height(
      GTK_DRAWING_AREA(editor->graph),
      curves || levels ? 280 : 160);
  gtk_drawing_area_set_draw_func(
      GTK_DRAWING_AREA(editor->graph),
      draw_graph,
      editor,
      nullptr);
  gtk_widget_set_halign(editor->graph, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(editor->graph, "card");
  gtk_box_append(GTK_BOX(content), editor->graph);

  if (curves) {
    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(curve_drag_begin), editor);
    g_signal_connect(drag, "drag-update", G_CALLBACK(curve_drag_update), editor);
    g_signal_connect(drag, "drag-end", G_CALLBACK(curve_drag_end), editor);
    gtk_widget_add_controller(editor->graph, GTK_EVENT_CONTROLLER(drag));

    GtkGesture* remove = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(remove), 3);
    g_signal_connect(remove, "pressed", G_CALLBACK(curve_delete), editor);
    gtk_widget_add_controller(editor->graph, GTK_EVENT_CONTROLLER(remove));

    GtkWidget* hint = gtk_label_new(
        "Clic para añadir un punto. Arrastra para moverlo. Clic derecho para quitarlo.");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(hint, "caption");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(content), hint);
  }

  if (editor->settings.kind != patchy::AdjustmentKind::HueSaturation) {
    editor->gradient = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(
        GTK_DRAWING_AREA(editor->gradient),
        280);
    gtk_drawing_area_set_content_height(
        GTK_DRAWING_AREA(editor->gradient),
        18);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(editor->gradient),
        draw_gradient,
        editor,
        nullptr);
    gtk_widget_set_halign(editor->gradient, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(content), editor->gradient);
  }

  if (levels) {
    const auto record = patchy::levels_record_for_channel(
        editor->settings.levels,
        editor->levels_channel);
    editor->black_input = add_scale(
        values, "Negro de entrada", 0, 254, record.black_input, editor);
    editor->gamma = add_scale(
        values, "Gamma", 10, 999, record.gamma_percent, editor);
    editor->white_input = add_scale(
        values, "Blanco de entrada", 1, 255, record.white_input, editor);
    editor->black_output = add_scale(
        values, "Negro de salida", 0, 255, record.black_output, editor);
    editor->white_output = add_scale(
        values, "Blanco de salida", 0, 255, record.white_output, editor);
  } else if (editor->settings.kind == patchy::AdjustmentKind::BrightnessContrast) {
    const auto& value = editor->settings.brightness_contrast;
    const int brightness_range =
        value.use_legacy
            ? patchy::kBrightnessContrastLegacyRange
            : patchy::kModernBrightnessRange;
    const int contrast_low =
        value.use_legacy
            ? -patchy::kBrightnessContrastLegacyRange
            : patchy::kModernContrastMin;
    const int contrast_high =
        value.use_legacy
            ? patchy::kBrightnessContrastLegacyRange
            : patchy::kModernContrastMax;
    editor->brightness = add_scale(
        values,
        "Brillo",
        -brightness_range,
        brightness_range,
        value.brightness,
        editor);
    editor->contrast = add_scale(
        values,
        "Contraste",
        contrast_low,
        contrast_high,
        value.contrast,
        editor);
  } else if (editor->settings.kind == patchy::AdjustmentKind::HueSaturation) {
    const auto& value = editor->settings.hue_saturation;
    editor->hue = add_scale(values, "Tono", -180, 180, value.hue_shift, editor);
    editor->saturation = add_scale(
        values, "Saturación", -100, 100, value.saturation_delta, editor);
    editor->lightness = add_scale(
        values, "Luminosidad", -100, 100, value.lightness_delta, editor);
    editor->colorize = adw_switch_row_new();
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(editor->colorize),
        "Colorear");
    adw_switch_row_set_active(ADW_SWITCH_ROW(editor->colorize), value.colorize);
    g_signal_connect(
        editor->colorize,
        "notify::active",
        G_CALLBACK(colorize_changed),
        editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(values), editor->colorize);
    editor->colorize_hue = add_scale(
        values, "Tono de coloreado", 0, 360, value.colorize_hue, editor);
    editor->colorize_saturation = add_scale(
        values,
        "Saturación de coloreado",
        0,
        100,
        value.colorize_saturation,
        editor);
    editor->colorize_lightness = add_scale(
        values,
        "Luminosidad de coloreado",
        -100,
        100,
        value.colorize_lightness,
        editor);
  } else if (editor->settings.kind == patchy::AdjustmentKind::ColorBalance) {
    const auto& value = editor->settings.color_balance;
    editor->cyan_red = add_scale(
        values, "Cian / Rojo", -100, 100, value.cyan_red, editor);
    editor->magenta_green = add_scale(
        values, "Magenta / Verde", -100, 100, value.magenta_green, editor);
    editor->yellow_blue = add_scale(
        values, "Amarillo / Azul", -100, 100, value.yellow_blue, editor);
  } else if (editor->settings.kind == patchy::AdjustmentKind::Posterize) {
    editor->posterize = add_scale(
        values,
        "Niveles",
        2,
        255,
        editor->settings.posterize.levels,
        editor);
  } else if (editor->settings.kind == patchy::AdjustmentKind::Threshold) {
    editor->threshold = add_scale(
        values,
        "Umbral",
        1,
        255,
        editor->settings.threshold.level,
        editor);
  }

  if (editor->settings.kind == patchy::AdjustmentKind::Invert) {
    gtk_widget_set_visible(values, FALSE);
  }

  GtkWidget* cancel = gtk_button_new_with_label("Cancelar");
  GtkWidget* apply = gtk_button_new_with_label("Aplicar");
  gtk_widget_add_css_class(apply, "suggested-action");
  g_signal_connect(cancel, "clicked", G_CALLBACK(close_editor), editor);
  g_signal_connect(apply, "clicked", G_CALLBACK(apply_editor), editor);

  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), apply);
  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);

  GtkWidget* dialog = GTK_WIDGET(adw_dialog_new());
  const char* title = "Ajuste";

  switch (editor->settings.kind) {
    case patchy::AdjustmentKind::Levels:
      title = "Niveles";
      break;
    case patchy::AdjustmentKind::Curves:
      title = "Curvas";
      break;
    case patchy::AdjustmentKind::HueSaturation:
      title = "Tono/Saturación";
      break;
    case patchy::AdjustmentKind::ColorBalance:
      title = "Equilibrio de color";
      break;
    case patchy::AdjustmentKind::Invert:
      title = "Invertir";
      break;
    case patchy::AdjustmentKind::Posterize:
      title = "Posterizar";
      break;
    case patchy::AdjustmentKind::Threshold:
      title = "Umbral";
      break;
    case patchy::AdjustmentKind::BrightnessContrast:
      title = "Brillo/Contraste";
      break;
  }

  adw_dialog_set_title(ADW_DIALOG(dialog), title);
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 440);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 640);
  adw_dialog_set_child(ADW_DIALOG(dialog), toolbar);
  adw_dialog_set_default_widget(ADW_DIALOG(dialog), apply);
  g_object_set_data_full(
      G_OBJECT(dialog),
      "lienzo-adjustment-editor",
      editor,
      [](gpointer data) { delete static_cast<Editor*>(data); });

  GtkWidget* host =
      GTK_WIDGET(gtk_widget_get_ancestor(parent, GTK_TYPE_WINDOW));

  if (host != nullptr) {
    g_object_set_data(
        G_OBJECT(host),
        "lienzo-block-close",
        GINT_TO_POINTER(1));
    g_object_set_data(
        G_OBJECT(dialog),
        "lienzo-block-close-host",
        host);
  }

  g_signal_connect(
      dialog,
      "closed",
      G_CALLBACK(+[](AdwDialog* self, gpointer) {
        gpointer blocked = g_object_get_data(
            G_OBJECT(self),
            "lienzo-block-close-host");

        if (blocked != nullptr) {
          g_object_set_data(
              G_OBJECT(blocked),
              "lienzo-block-close",
              nullptr);
        }
      }),
      nullptr);

  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

}  // namespace lienzo::gnome
