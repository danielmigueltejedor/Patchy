#include "ui-gnome/canvas.hpp"
#include "ui-gnome/canvas_internal.hpp"
#include "ui-gnome/brush_tips.hpp"
#include "ui-gnome/tools/selection_controller.hpp"
#include "ui-gnome/tools/text_controller.hpp"
#include "ui-gnome/tools/path_controller.hpp"
#include "ui-gnome/tools/retouch_controller.hpp"

#include "core/layer_metadata.hpp"
#include "core/magnetic_lasso.hpp"
#include "core/pixel_tools.hpp"
#include "core/rect_utils.hpp"
#include "core/stroke_stabilizer.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lienzo::gnome {

// History, clipboard, crop commit, and the active editable layer.


patchy::Layer* find_last_pixel_layer(
    std::vector<patchy::Layer>& layers) {
  for (auto it = layers.rbegin();
       it != layers.rend();
       ++it) {
    if (it->kind() == patchy::LayerKind::Group) {
      if (auto* child =
              find_last_pixel_layer(it->children());
          child != nullptr) {
        return child;
      }
    }

    if (it->kind() == patchy::LayerKind::Pixel) {
      return &*it;
    }
  }

  return nullptr;
}

std::optional<patchy::LayerId> editing_layer(
    CanvasState* state) {
  auto active =
      state->document->active_layer_id();

  if (active.has_value()) {
    auto* layer =
        state->document->find_layer(*active);

    if (
        layer != nullptr &&
        layer->kind() == patchy::LayerKind::Pixel) {
      return active;
    }
  }

  auto* layer =
      find_last_pixel_layer(
          state->document->layers());

  if (layer == nullptr) {
    return std::nullopt;
  }

  state->document->set_active_layer(
      layer->id());

  return layer->id();
}

void notify_document_changed(
    CanvasState* state) {
  if (state->document_changed_callback) {
    state->document_changed_callback();
  }
}

void push_history(
    CanvasState* state) {
  state->undo_stack.push_back(
      *state->document);

  constexpr std::size_t kMaxHistory = 32;

  if (
      state->undo_stack.size() >
      kMaxHistory) {
    state->undo_stack.erase(
        state->undo_stack.begin());
  }

  state->redo_stack.clear();
}

void undo_document(
    CanvasState* state) {
  if (state->undo_stack.empty()) {
    return;
  }

  state->redo_stack.push_back(
      *state->document);

  *state->document =
      std::move(
          state->undo_stack.back());

  state->undo_stack.pop_back();

  state->view_initialized = false;

  refresh_canvas(state);
  notify_document_changed(state);
}

void redo_document(
    CanvasState* state) {
  if (state->redo_stack.empty()) {
    return;
  }

  state->undo_stack.push_back(
      *state->document);

  *state->document =
      std::move(
          state->redo_stack.back());

  state->redo_stack.pop_back();

  state->view_initialized = false;

  refresh_canvas(state);
  notify_document_changed(state);
}

void copy_active_layer(
    CanvasState* state) {
  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  const auto* layer =
      state->document->find_layer(
          *active);

  if (
      layer == nullptr ||
      layer->kind() !=
          patchy::LayerKind::Pixel ||
      layer->pixels().empty()) {
    return;
  }

  state->clipboard =
      CanvasState::ClipboardLayer{
          layer->pixels(),
          layer->bounds(),
          layer->name()};
}

void cut_active_layer(
    CanvasState* state) {
  const auto active =
      state->document->active_layer_id();

  if (!active.has_value()) {
    return;
  }

  copy_active_layer(state);

  if (!state->clipboard.has_value()) {
    return;
  }

  push_history(state);

  if (
      state->document->remove_layer(
          *active)) {
    refresh_canvas(state);
    notify_document_changed(state);
  }
}

void paste_layer(
    CanvasState* state) {
  if (!state->clipboard.has_value()) {
    return;
  }

  push_history(state);

  const auto& copied =
      *state->clipboard;

  patchy::Layer layer(
      state->document->allocate_layer_id(),
      copied.name + " copy",
      copied.pixels);

  layer.set_bounds(
      copied.bounds);

  state->document->add_layer(
      std::move(layer));

  refresh_canvas(state);
  notify_document_changed(state);
}

bool commit_crop(
    CanvasState* state) {
  if (
      state->tool != Tool::Crop ||
      !state->crop_session_active ||
      state->crop_rect.empty()) {
    return false;
  }

  push_history(state);

  if (
      !patchy::crop_document(
          *state->document,
          state->crop_rect)) {
    return false;
  }

  state->crop_session_active = false;
  state->view_initialized = false;

  refresh_canvas(state);
  notify_document_changed(state);

  return true;
}

void cancel_crop(
    CanvasState* state) {
  if (!state->crop_session_active) {
    return;
  }

  state->crop_session_active = false;

  gtk_widget_queue_draw(
      GTK_WIDGET(state->area));
}

}  // namespace lienzo::gnome
