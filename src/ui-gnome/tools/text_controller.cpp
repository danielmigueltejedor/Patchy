#include "ui-gnome/tools/text_controller.hpp"

#include "core/layer_metadata.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace lienzo::gnome {

namespace {

struct RenderedText {
  patchy::PixelBuffer pixels;
  patchy::Rect bounds{};
};

std::string metadata_value(
    const patchy::Layer& layer,
    const char* key,
    std::string fallback = {}) {
  const auto found =
      layer.metadata().find(key);

  return
      found == layer.metadata().end()
          ? std::move(fallback)
          : found->second;
}

bool metadata_bool(
    const patchy::Layer& layer,
    const char* key) {
  return
      metadata_value(
          layer,
          key) == "true";
}

patchy::EditColor parse_color(
    const std::string& text,
    patchy::EditColor fallback) {
  if (
      text.size() != 7 ||
      text[0] != '#') {
    return fallback;
  }

  const auto parse =
      [&text](std::size_t offset) {
        char buffer[3]{
            text[offset],
            text[offset + 1],
            0};

        return static_cast<std::uint8_t>(
            std::strtoul(
                buffer,
                nullptr,
                16));
      };

  return {
      parse(1),
      parse(3),
      parse(5),
      255};
}

std::string color_hex(
    patchy::EditColor color) {
  char text[8]{};

  std::snprintf(
      text,
      sizeof(text),
      "#%02X%02X%02X",
      color.r,
      color.g,
      color.b);

  return text;
}

PangoFontDescription* make_text_font(
    const TextStyle& style,
    double scale = 1.0) {
  PangoFontDescription* font =
      pango_font_description_new();

  pango_font_description_set_family(
      font,
      style.family.c_str());

  pango_font_description_set_absolute_size(
      font,
      std::max(
          1.0,
          static_cast<double>(
              style.size) *
              scale) *
          PANGO_SCALE);

  pango_font_description_set_weight(
      font,
      style.bold
          ? PANGO_WEIGHT_BOLD
          : PANGO_WEIGHT_NORMAL);

  pango_font_description_set_style(
      font,
      style.italic
          ? PANGO_STYLE_ITALIC
          : PANGO_STYLE_NORMAL);

  return font;
}

PangoAlignment pango_alignment(
    TextAlignment alignment) {
  switch (alignment) {
    case TextAlignment::Center:
      return PANGO_ALIGN_CENTER;

    case TextAlignment::Right:
      return PANGO_ALIGN_RIGHT;

    case TextAlignment::Left:
    default:
      return PANGO_ALIGN_LEFT;
  }
}

RenderedText render_text(
    const std::string& text,
    int anchor_x,
    int anchor_y,
    const TextStyle& style,
    PangoContext* pango_context) {
  if (pango_context == nullptr) {
    return {};
  }

  PangoLayout* layout =
      pango_layout_new(
          pango_context);

  PangoFontDescription* font =
      make_text_font(
          style);

  pango_layout_set_font_description(
      layout,
      font);

  pango_layout_set_alignment(
      layout,
      pango_alignment(
          style.alignment));

  pango_layout_set_text(
      layout,
      text.c_str(),
      -1);

  PangoRectangle ink{};
  PangoRectangle logical{};

  pango_layout_get_pixel_extents(
      layout,
      &ink,
      &logical);

  constexpr int padding = 4;

  const int left =
      std::min(
          ink.x,
          logical.x);

  const int top =
      std::min(
          ink.y,
          logical.y);

  const int right =
      std::max(
          ink.x + ink.width,
          logical.x + logical.width);

  const int bottom =
      std::max(
          ink.y + ink.height,
          logical.y + logical.height);

  const int width =
      std::max(
          1,
          right - left +
              padding * 2);

  const int height =
      std::max(
          1,
          bottom - top +
              padding * 2);

  cairo_surface_t* surface =
      cairo_image_surface_create(
          CAIRO_FORMAT_ARGB32,
          width,
          height);

  cairo_t* cr =
      cairo_create(surface);

  cairo_set_operator(
      cr,
      CAIRO_OPERATOR_SOURCE);

  cairo_set_source_rgba(
      cr,
      0,
      0,
      0,
      0);

  cairo_paint(cr);

  cairo_set_operator(
      cr,
      CAIRO_OPERATOR_OVER);

  cairo_set_source_rgba(
      cr,
      static_cast<float>(style.color.r) / 255.0F,
      static_cast<float>(style.color.g) / 255.0F,
      static_cast<float>(style.color.b) / 255.0F,
      static_cast<float>(style.color.a) / 255.0F);

  cairo_move_to(
      cr,
      padding - left,
      padding - top);

  pango_cairo_show_layout(
      cr,
      layout);

  cairo_surface_flush(surface);

  patchy::PixelBuffer pixels(
      width,
      height,
      patchy::PixelFormat::rgba8());

  const auto* source =
      reinterpret_cast<const std::uint32_t*>(
          cairo_image_surface_get_data(
              surface));

  const int source_stride =
      cairo_image_surface_get_stride(
          surface) /
      4;

  for (int y = 0;
       y < height;
       ++y) {
    auto destination =
        pixels.row(y);

    for (int x = 0;
         x < width;
         ++x) {
      const std::uint32_t argb =
          source[
              static_cast<std::size_t>(y) *
                  source_stride +
              x];

      const auto a =
          static_cast<std::uint8_t>(
              argb >> 24);

      const auto premul_r =
          static_cast<std::uint8_t>(
              argb >> 16);

      const auto premul_g =
          static_cast<std::uint8_t>(
              argb >> 8);

      const auto premul_b =
          static_cast<std::uint8_t>(
              argb);

      const auto unpremultiply =
          [a](std::uint8_t value) {
            if (a == 0) {
              return std::uint8_t{0};
            }

            return static_cast<std::uint8_t>(
                std::min(
                    255,
                    (static_cast<int>(value) *
                         255 +
                     a / 2) /
                        a));
          };

      const auto offset =
          static_cast<std::size_t>(x) *
          4;

      destination[offset] =
          unpremultiply(
              premul_r);

      destination[offset + 1] =
          unpremultiply(
              premul_g);

      destination[offset + 2] =
          unpremultiply(
              premul_b);

      destination[offset + 3] =
          a;
    }
  }

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  g_object_unref(layout);
  pango_font_description_free(font);

  RenderedText result;

  result.pixels =
      std::move(pixels);

  result.bounds =
      patchy::Rect{
          anchor_x + left - padding,
          anchor_y + top - padding,
          width,
          height};

  return result;
}

patchy::Layer* find_text_layer_at(
    std::vector<patchy::Layer>& layers,
    int x,
    int y) {
  for (
      auto it = layers.rbegin();
      it != layers.rend();
      ++it) {
    if (
        auto* child =
            find_text_layer_at(
                it->children(),
                x,
                y);
        child != nullptr) {
      return child;
    }

    if (
        patchy::layer_is_text(*it) &&
        it->bounds().contains(
            x,
            y)) {
      return &*it;
    }
  }

  return nullptr;
}

}  // namespace

struct TextController::Impl {
  patchy::Document* document{};
  GtkOverlay* overlay{};

  std::function<void()>
      before_commit;

  std::function<void()>
      after_commit;

  TextStyle style;

  GtkWidget* editor_frame{};
  GtkTextView* editor{};
  GtkCssProvider* css{};
  GtkTextTag* editor_style_tag{};

  std::optional<patchy::LayerId>
      editing_layer;

  int anchor_x{};
  int anchor_y{};

  double editor_zoom{1.0};

  void apply_editor_style_tag() {
    if (editor == nullptr) {
      return;
    }

    GtkTextBuffer* buffer =
        gtk_text_view_get_buffer(
            editor);

    if (editor_style_tag == nullptr) {
      editor_style_tag =
          gtk_text_buffer_create_tag(
              buffer,
              "lienzo-text-preview",
              nullptr);
    }

    GdkRGBA rgba{
        static_cast<float>(style.color.r) / 255.0F,
        static_cast<float>(style.color.g) / 255.0F,
        static_cast<float>(style.color.b) / 255.0F,
        static_cast<float>(style.color.a) / 255.0F};

    PangoFontDescription* font =
        make_text_font(
            style,
            editor_zoom);

    g_object_set(
        editor_style_tag,
        "font-desc",
        font,
        "foreground-rgba",
        &rgba,
        nullptr);

    pango_font_description_free(
        font);

    GtkTextIter begin{};
    GtkTextIter finish{};

    gtk_text_buffer_get_bounds(
        buffer,
        &begin,
        &finish);

    gtk_text_buffer_remove_tag(
        buffer,
        editor_style_tag,
        &begin,
        &finish);

    gtk_text_buffer_apply_tag(
        buffer,
        editor_style_tag,
        &begin,
        &finish);


    char* preview_text =
        gtk_text_buffer_get_text(
            buffer,
            &begin,
            &finish,
            FALSE);

    PangoLayout* preview_layout =
        gtk_widget_create_pango_layout(
            GTK_WIDGET(editor),
            preview_text);

    PangoFontDescription* preview_font =
        make_text_font(
            style,
            editor_zoom);

    pango_layout_set_font_description(
        preview_layout,
        preview_font);

    pango_layout_set_alignment(
        preview_layout,
        pango_alignment(
            style.alignment));

    int preview_width = 0;
    int preview_height = 0;

    pango_layout_get_pixel_size(
        preview_layout,
        &preview_width,
        &preview_height);

    gtk_widget_set_size_request(
        GTK_WIDGET(editor),
        std::max(
            24,
            preview_width + 2),
        std::max(
            24,
            preview_height + 2));

    pango_font_description_free(
        preview_font);

    g_object_unref(
        preview_layout);

    g_free(
        preview_text);
  }

  static void editor_buffer_changed(
      GtkTextBuffer*,
      gpointer data) {
    static_cast<Impl*>(data)
        ->apply_editor_style_tag();
  }

  void refresh_editor_style() {
    if (
        editor == nullptr ||
        css == nullptr) {
      return;
    }

    const std::string css_text =
        ".lienzo-inline-text {"
        "background-color: transparent;"
        "border: none;"
        "padding: 0;"
        "}"
        ".lienzo-inline-text text {"
        "background-color: transparent;"
        "}"
        ".lienzo-inline-text-frame {"
        "background-color: transparent;"
        "border: 1px solid alpha(@accent_color,0.75);"
        "border-radius: 2px;"
        "outline: none;"
        "}";

    gtk_css_provider_load_from_string(
        css,
        css_text.c_str());

    apply_editor_style_tag();
  }

  void destroy_editor() {
    if (editor_frame != nullptr) {
      gtk_overlay_remove_overlay(
          overlay,
          editor_frame);
    }

    editor_frame = nullptr;
    editor = nullptr;
    editor_style_tag = nullptr;
    editing_layer.reset();
  }

  std::string editor_text() const {
    if (editor == nullptr) {
      return {};
    }

    GtkTextBuffer* buffer =
        gtk_text_view_get_buffer(
            editor);

    GtkTextIter start{};
    GtkTextIter end{};

    gtk_text_buffer_get_bounds(
        buffer,
        &start,
        &end);

    char* raw =
        gtk_text_buffer_get_text(
            buffer,
            &start,
            &end,
            FALSE);

    std::string result =
        raw != nullptr
            ? raw
            : "";

    g_free(raw);

    return result;
  }
};

gboolean text_key_pressed(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType modifiers,
    gpointer data) {
  auto* controller =
      static_cast<TextController*>(
          data);

  if (keyval == GDK_KEY_Escape) {
    controller->cancel();
    return TRUE;
  }

  if (
      (modifiers &
       GDK_CONTROL_MASK) != 0 &&
      (keyval == GDK_KEY_Return ||
       keyval == GDK_KEY_KP_Enter)) {
    controller->commit();
    return TRUE;
  }

  return FALSE;
}

TextController::TextController(
    patchy::Document& document,
    GtkOverlay* overlay,
    std::function<void()> before_commit,
    std::function<void()> after_commit)
    : impl_(
          std::make_unique<Impl>()) {
  impl_->document =
      &document;

  impl_->overlay =
      overlay;

  impl_->before_commit =
      std::move(
          before_commit);

  impl_->after_commit =
      std::move(
          after_commit);

  impl_->css =
      gtk_css_provider_new();
}

TextController::~TextController() {
  if (
      impl_ != nullptr &&
      impl_->css != nullptr) {
    g_object_unref(
        impl_->css);
  }
}

bool TextController::active() const noexcept {
  return
      impl_->editor != nullptr;
}

void TextController::begin_point(
    int document_x,
    int document_y,
    double widget_x,
    double widget_y,
    double view_zoom,
    patchy::EditColor color) {
  if (active()) {
    commit();
  }

  impl_->style.color =
      color;

  impl_->editor_zoom =
      std::clamp(
          view_zoom,
          0.02,
          64.0);

  patchy::Layer* existing =
      find_text_layer_at(
          impl_->document->layers(),
          document_x,
          document_y);

  std::string initial_text;

  if (existing != nullptr) {
    impl_->editing_layer =
        existing->id();

    impl_->document->set_active_layer(
        existing->id());

    initial_text =
        metadata_value(
            *existing,
            patchy::kLayerMetadataText);

    impl_->style.family =
        metadata_value(
            *existing,
            patchy::kLayerMetadataTextFont,
            "Sans");

    try {
      impl_->style.size =
          std::clamp(
              std::stoi(
                  metadata_value(
                      *existing,
                      patchy::kLayerMetadataTextSize,
                      "32")),
              1,
              4096);
    } catch (...) {
      impl_->style.size = 32;
    }

    impl_->style.bold =
        metadata_bool(
            *existing,
            patchy::kLayerMetadataTextBold);

    impl_->style.italic =
        metadata_bool(
            *existing,
            patchy::kLayerMetadataTextItalic);

    impl_->style.color =
        parse_color(
            metadata_value(
                *existing,
                patchy::kLayerMetadataTextColor),
            color);

    if (
        const auto transform =
            patchy::parse_layer_affine_transform(
                metadata_value(
                    *existing,
                    patchy::kLayerMetadataTextTransform));
        transform.has_value()) {
      impl_->anchor_x =
          static_cast<int>(
              std::lround(
                  (*transform)[4]));

      impl_->anchor_y =
          static_cast<int>(
              std::lround(
                  (*transform)[5]));
    } else {
      impl_->anchor_x =
          existing->bounds().x;

      impl_->anchor_y =
          existing->bounds().y;
    }

  } else {
    impl_->editing_layer.reset();

    impl_->anchor_x =
        document_x;

    impl_->anchor_y =
        document_y;
  }

  impl_->editor_frame =
      gtk_frame_new(nullptr);

  gtk_widget_add_css_class(
      impl_->editor_frame,
      "lienzo-inline-text-frame");

  impl_->editor =
      GTK_TEXT_VIEW(
          gtk_text_view_new());

  gtk_widget_add_css_class(
      GTK_WIDGET(impl_->editor),
      "lienzo-inline-text");

  gtk_text_view_set_wrap_mode(
      impl_->editor,
      GTK_WRAP_NONE);

  gtk_text_view_set_left_margin(
      impl_->editor,
      0);

  gtk_text_view_set_right_margin(
      impl_->editor,
      0);

  gtk_text_view_set_top_margin(
      impl_->editor,
      0);

  gtk_text_view_set_bottom_margin(
      impl_->editor,
      0);


  gtk_frame_set_child(
      GTK_FRAME(
          impl_->editor_frame),
      GTK_WIDGET(
          impl_->editor));

  gtk_widget_set_halign(
      impl_->editor_frame,
      GTK_ALIGN_START);

  gtk_widget_set_valign(
      impl_->editor_frame,
      GTK_ALIGN_START);

  gtk_widget_set_margin_start(
      impl_->editor_frame,
      std::max(
          0,
          static_cast<int>(
              std::lround(
                  widget_x))));

  gtk_widget_set_margin_top(
      impl_->editor_frame,
      std::max(
          0,
          static_cast<int>(
              std::lround(
                  widget_y))));

  GtkTextBuffer* buffer =
      gtk_text_view_get_buffer(
          impl_->editor);


  g_signal_connect(
      buffer,
      "changed",
      G_CALLBACK(
          Impl::editor_buffer_changed),
      impl_.get());

  gtk_text_buffer_set_text(
      buffer,
      initial_text.c_str(),
      -1);

  GtkEventController* keys =
      gtk_event_controller_key_new();

  g_signal_connect(
      keys,
      "key-pressed",
      G_CALLBACK(
          text_key_pressed),
      this);

  gtk_widget_add_controller(
      GTK_WIDGET(impl_->editor),
      keys);

  gtk_overlay_add_overlay(
      impl_->overlay,
      impl_->editor_frame);

  gtk_style_context_add_provider(
      gtk_widget_get_style_context(
          GTK_WIDGET(
              impl_->editor)),
      GTK_STYLE_PROVIDER(
          impl_->css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  gtk_style_context_add_provider(
      gtk_widget_get_style_context(
          impl_->editor_frame),
      GTK_STYLE_PROVIDER(
          impl_->css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  impl_->refresh_editor_style();

  gtk_widget_grab_focus(
      GTK_WIDGET(
          impl_->editor));

  GtkTextIter end{};

  gtk_text_buffer_get_end_iter(
      buffer,
      &end);

  gtk_text_buffer_place_cursor(
      buffer,
      &end);
}

bool TextController::commit() {
  if (!active()) {
    return false;
  }

  const std::string text =
      impl_->editor_text();

  if (text.empty()) {
    if (
        impl_->editing_layer.has_value()) {
      if (impl_->before_commit) {
        impl_->before_commit();
      }

      impl_->document->remove_layer(
          *impl_->editing_layer);

      if (impl_->after_commit) {
        impl_->after_commit();
      }
    }

    impl_->destroy_editor();
    return true;
  }

  if (impl_->before_commit) {
    impl_->before_commit();
  }

  PangoContext* pango_context =
      gtk_widget_get_pango_context(
          GTK_WIDGET(
              impl_->editor));

  auto rendered =
      render_text(
          text,
          impl_->anchor_x,
          impl_->anchor_y,
          impl_->style,
          pango_context);

  patchy::Layer* layer = nullptr;

  if (
      impl_->editing_layer.has_value()) {
    layer =
        impl_->document->find_layer(
            *impl_->editing_layer);
  }

  if (layer == nullptr) {
    patchy::Layer created(
        impl_->document->allocate_layer_id(),
        "Texto",
        patchy::LayerKind::Pixel);

    layer =
        &impl_->document->add_layer(
            std::move(created));
  }

  layer->set_pixels(
      std::move(
          rendered.pixels));

  layer->set_bounds(
      rendered.bounds);

  auto& metadata =
      layer->metadata();

  metadata[
      patchy::kLayerMetadataText] =
      text;

  metadata[
      patchy::kLayerMetadataTextFont] =
      impl_->style.family;

  metadata[
      patchy::kLayerMetadataTextSize] =
      std::to_string(
          impl_->style.size);

  metadata[
      patchy::kLayerMetadataTextColor] =
      color_hex(
          impl_->style.color);

  metadata[
      patchy::kLayerMetadataTextBold] =
      impl_->style.bold
          ? "true"
          : "false";

  metadata[
      patchy::kLayerMetadataTextItalic] =
      impl_->style.italic
          ? "true"
          : "false";

  metadata[
      patchy::kLayerMetadataTextFlow] =
      "point";

  metadata[
      patchy::kLayerMetadataTextAntiAlias] =
      "3";

  metadata[
      patchy::kLayerMetadataTextRasterStatus] =
      "patchy_raster";

  patchy::LayerAffineTransform transform{
      1.0,
      0.0,
      0.0,
      1.0,
      static_cast<double>(
          impl_->anchor_x),
      static_cast<double>(
          impl_->anchor_y)};

  metadata[
      patchy::kLayerMetadataTextTransform] =
      patchy::serialize_layer_affine_transform(
          transform);

  impl_->document->set_active_layer(
      layer->id());

  impl_->destroy_editor();

  if (impl_->after_commit) {
    impl_->after_commit();
  }

  return true;
}

void TextController::cancel() {
  if (!active()) {
    return;
  }

  impl_->destroy_editor();
}

void TextController::set_family(
    std::string family) {
  if (!family.empty()) {
    impl_->style.family =
        std::move(family);

    impl_->refresh_editor_style();
  }
}

void TextController::set_size(
    int size) {
  impl_->style.size =
      std::clamp(
          size,
          1,
          4096);

  impl_->refresh_editor_style();
}

void TextController::set_bold(
    bool bold) {
  impl_->style.bold = bold;
  impl_->refresh_editor_style();
}

void TextController::set_italic(
    bool italic) {
  impl_->style.italic = italic;
  impl_->refresh_editor_style();
}

void TextController::set_alignment(
    TextAlignment alignment) {
  impl_->style.alignment =
      alignment;

  if (impl_->editor != nullptr) {
    switch (alignment) {
      case TextAlignment::Center:
        gtk_text_view_set_justification(
            impl_->editor,
            GTK_JUSTIFY_CENTER);
        break;

      case TextAlignment::Right:
        gtk_text_view_set_justification(
            impl_->editor,
            GTK_JUSTIFY_RIGHT);
        break;

      case TextAlignment::Left:
      default:
        gtk_text_view_set_justification(
            impl_->editor,
            GTK_JUSTIFY_LEFT);
        break;
    }
  }
}

void TextController::set_color(
    patchy::EditColor color) {
  impl_->style.color = color;
  impl_->refresh_editor_style();
}

const TextStyle&
TextController::style() const noexcept {
  return impl_->style;
}

}  // namespace lienzo::gnome
