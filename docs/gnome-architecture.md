# Lienzo GNOME architecture

Lienzo on this branch is Patchy plus a second executable. `lienzo-gnome` (`cmake/LienzoGnome.cmake`) is a GTK 4 / libadwaita frontend. It links `patchy_core`, `patchy_render`, `patchy_psd`, and `patchy_formats`. It does not link `patchy_ui`. The Qt application in `src/ui` and `src/app` is still the shipping interface, the test surface, and the packaging source. Deleting it before the parity rows below are replaced, or explicitly waived, is not allowed.

The product id is `com.nodalix.lienzo` (desktop file, Flatpak, `QGuiApplication::setDesktopFileName`). The GNOME application id uses that same id.

## Dependency direction

```text
lienzo-gnome (src/ui-gnome)
        |
        v
patchy_core / patchy_render / patchy_psd / patchy_formats
        |
        v
document, pixels, compositor, codecs
```

`src/core`, `src/render`, `src/psd`, `src/formats`, `src/filters`, `src/color`, and `src/support` do not include Qt or GTK. Comments in `pixel_buffer.hpp`, `magnetic_lasso.hpp`, and `compositor.cpp` mention Qt only as a comparison. `src/app` is the Qt process entry and is not part of the engine.

Do not rename `patchy_*` CMake targets or move engine files to make the tree look like a new product. Public names stay Lienzo. Internal engine names stay aligned with Patchy so upstream commits still apply.

## Where a change goes

A new document, pixel, PSD, or compositor behavior goes in the existing engine directory (`src/core`, `src/render`, `src/psd`, `src/formats`, `src/filters`). The new function takes engine types (`Document`, `PixelBuffer`, `EditOptions`, `Rect`) and returns a result. It does not open a dialog or include GTK or Qt Widgets.

The GNOME interface calls that function. A gesture belongs in `canvas_input.cpp` or in `src/ui-gnome/tools/` when the tool already has a controller. A panel belongs in `inspector.cpp` or a new panel file next to it. A dialog belongs next to `new_document_dialog.cpp`. None of those files own a second copy of the pixel algorithm.

`src/ui` is the Qt application that still ships. New Lienzo product behavior does not start there.

Engine changes that already exist on this branch, and the rule for each:

- `Compositor::flatten_rgb8_region` is a toolkit-agnostic region flatten used by the GNOME canvas cache. It belongs on `Compositor`. It needs a core test before it is treated as pinned.
- The row copy in `expand_layer_to_include_rect` is a performance change on a geometry path. Keep it only while the existing geometry and tool-write canaries stay green. Do not restyle the rest of `pixel_tools.cpp`.
- The Adwaita Sans choice in `src/app/main.cpp` is Qt application chrome. It is not an engine API.

## Classification

### A. Pure engine

`src/core`, `src/render`, `src/psd`, `src/filters`, `src/color`, `src/support`, and the non-vendored part of `src/formats`. Vendored trees (`libheif`, `libraw`, `lcms2`, `miniz`, `stb`, `zstd`) stay untouched. `tests/core` pins this layer.

### B. Editor logic that still lives in Qt UI

These are not engine files, and they must not be moved into `src/core` just to share them with GNOME. Patchy keeps them in `src/ui`. Moving them into `src/core` would fork the upstream layout. When a second consumer needs one of them, add a concrete toolkit-agnostic type under a Lienzo-owned directory and leave `src/core` file names alone. Do not introduce interfaces, factories, or an event bus for that move.

Known piles:

- Selection state: `CanvasWidget` stores `QImage` masks (`canvas_widget.hpp`). Algorithms that already exist in core are `quick_select_segment`, `LiveWireEngine`, `color_within_tolerance`, and `trace_mask_outlines`. The GNOME `SelectionController` keeps its own mask. Quick Select stamps a footprint and calls `quick_select_segment` once on release, the same call Patchy makes. Magic Wand uses `color_within_tolerance`; the contiguous flood itself is still the controller's, because Patchy keeps that flood in `CanvasWidget` rather than in `src/core`.
- History policy: `MainWindow::DocumentSession` stores document snapshots, selection snapshots, labels, coalescing, and the memory budget. Whole-`Document` copies are already cheap for shared payloads (`document_memory.hpp`). The policy is UI. The GNOME canvas keeps a private `undo_stack` of bare `Document` values and does not restore selection.
- Text layout and the Photoshop text pipeline: `src/ui/text_layout.cpp` and the text code in `main_window.cpp`. Calibration lives in `docs/text-tool.md` and `docs/txt2.md`. The GNOME `TextController` shapes with Pango and writes its own layer metadata. That is a second text engine. Do not ship it as the document text model.
- Retouch: healing, spot healing, and patch live in `src/core` and are documented in `docs/healing.md`. `RetouchController` does not call those APIs. It paints from the composite buffer itself.
- File dialogs, scripting, and MCP are product/UI. Scripting and MCP stay required (class B in the matrix) but their implementation is Qt. Do not reimplement the script API in the GNOME layer.

### C. Shared infrastructure

Root `CMakeLists.txt`, presets, `tests/core`, translation catalogs, and `scripts/`. `src/plugins` is the Windows 8BF host. It is engine-adjacent and platform-specific. It is not a GNOME widget dependency.

### D. Qt UI

`src/ui` (about 189 `.cpp` files), `src/app`, `tests/ui`. This includes `MainWindow`, `CanvasWidget`, dialogs, theme, scripting host, and MCP session UI. It remains until the parity table says otherwise.

### E. GNOME UI

`src/ui-gnome`, built only when `UNIX AND NOT APPLE AND NOT EMSCRIPTEN` and GTK 4.14 / libadwaita 1.5 are present. There are no GNOME tests.

`CanvasState` and the functions shared across canvas translation units live in `canvas_internal.hpp`. Only `src/ui-gnome/canvas*.cpp` may include it. The split is by responsibility: `canvas.cpp` builds the widget, `canvas_input.cpp` dispatches gestures, `canvas_render.cpp` owns the composite cache, `canvas_overlay.cpp` draws overlays, `canvas_brush.cpp` strokes through `patchy::paint_brush_*`, `canvas_move.cpp` previews a move, `canvas_selection.cpp` syncs the selection and runs the magnetic lasso, `canvas_view.cpp` converts coordinates, and `canvas_session.cpp` owns history, clipboard, and crop commit. A new tool's pixel work goes through an existing `patchy::` function. Its gesture goes in `canvas_input.cpp` or a controller under `tools/`, not into a new copy of the brush loop. Controllers that already exist: selection, text, path, retouch.

`inspector.cpp` is the layers, channels, and paths panel, plus a history page that shows a single static row. `main_window.cpp` owns the welcome page, tabs, open/save/export, and autosave. Strings in this layer are hardcoded Spanish. New user-visible strings follow `docs/localization.md` once a surface is stable enough to extract. Do not add another catalog pass over prototype copy.

### F. Temporary compatibility

Two executables are the current compatibility state, not the end state. There is no adapter framework. `flatten_rgb8_region` is a real engine API, not an adapter.

### G. Dead code

Nothing in `src/ui` is dead while that executable still ships. The GNOME history page is a stub, not a second implementation to delete yet.

## Feature matrix

Status is what the code does today. "Engine" means a toolkit-free implementation already exists. "Tests" means an automated pin exists, almost always on the Qt or core suites, not on `lienzo-gnome`.

Class: A parity required, B Lienzo keeps it, C not decided and not a GNOME blocker yet, D engine exists and GNOME does not expose it.

| Feature | Qt | GNOME | Engine | Tests | Class |
|---|---|---|---|---|---|
| Open/save PSD/PSB | yes | yes | yes | core | A |
| Export flattened formats | yes | yes | yes | core | A |
| New document | yes | yes | yes | partial | A |
| Layers list, visibility, rename, group, mask, delete | yes | yes | yes | ui | A |
| Layer styles and effects | yes | no | yes | core | D |
| Blend-mode editing | yes | no | yes | core | D |
| Channels and quick mask | yes | partial | yes | ui | A |
| Paths panel | yes | list only | yes | ui | A |
| History (labels, selection, budget) | yes | document copies only, panel stub | snapshots are cheap; policy is UI | ui | A |
| Marquee, ellipse, lasso | yes | yes, local mask | outlines in core | ui | A |
| Magic wand | yes | flood in the controller, metric is `color_within_tolerance` | metric in core; the mask flood still lives in `CanvasWidget` | core | A |
| Quick select | yes | seed during the drag, `quick_select_segment` once on release | yes | core | A |
| Magnetic lasso | yes | uses `LiveWireEngine` | yes | core | A |
| Move | yes | preview in canvas | layer bounds | ui | A |
| Free transform and warp | yes | no | yes | core/ui | D |
| Crop | yes | yes, calls `crop_document` | yes | ui | A |
| Brush, flow, airbrush, tips | yes | partial, calls `paint_brush_*` | yes | core canary | A |
| Mixer brush | yes | tool id only | yes | core | D |
| Fill and gradient | yes | calls core draw helpers | yes | core | A |
| Clone, heal, spot heal, patch | yes | local painter, not core healing | yes | core | A |
| Blur, sharpen, dodge, burn, sponge, smudge | yes | local retouch modes | partial | mixed | A |
| Pen and vector paths | yes | `PathController` | yes | core | A |
| Shape layers | yes | drag preview | yes | core | A |
| Text (TySh/Txt2, calibrated layout) | yes | Pango preview and commit | layout is still in `src/ui` | ui, psd | A |
| Adjustment layers | yes | create only | yes | core | D |
| Smart objects | yes | no | yes | core | D |
| Smart filters | yes | no | yes | core | D |
| Filter gallery, liquify | yes | no | yes | core/ui | D |
| Preferences | yes | dialog | settings are Qt | ui | B |
| Autosave / recovery | yes | 30s timer | recovery is Qt | ui | B |
| Scripting | yes | no | host is Qt | ui | B |
| MCP | yes | no | host is Qt | python | B |
| Print, scanner, 8BF plugins | yes, platform-specific | no | partial | partial | C |
| Guides, alignment, palette mode | yes | no | partial | ui | D |
| Localization catalogs | yes | hardcoded Spanish | n/a | translation tests | A for shipping strings |

No feature in this table is class C because Lienzo has decided to drop it. Class C means the capability is platform tooling around the Qt app, and the GNOME editor does not need a copy of it to be the document editor. Revisit before deleting `src/ui`.

## Upstream sync

`.github/workflows/weekly-patchy-sync.yml` still cherry-picks every non-protected commit and keeps it when the Linux build and tests do not regress against Lienzo main. Path tags on the report (`engine`, `ui`, `mixed`, `review`) do not change that apply rule.

While `src/ui` ships, upstream Qt commits can still carry editor fixes (text, filters, tools) and must remain eligible. Ignoring all of `src/ui` now would drop those fixes.

When `lienzo-gnome` is the only interface, change the apply rule to:

- Auto: commits whose files are only under `src/core`, `src/render`, `src/psd`, `src/formats`, `src/filters`, `src/color`, `src/support`, `tests/core`.
- Manual: `mixed` commits, CMake structure, and anything that changes a public engine signature.
- Manual port or skip: commits that only touch `src/ui`, `src/app`, `tests/ui`, Patchy packaging, or Patchy branding.

Lienzo-owned paths (`src/ui-gnome`, `cmake/LienzoGnome.cmake`, this document) are not in Patchy. A cherry-pick does not overwrite them unless a Patchy commit touches the same path, which it does not.

Protected identity paths stay protected: packaging, README, `AGENTS.md`, release scripts, and the Lienzo desktop id.

## What not to do next

- Do not `rm -rf src/ui`.
- Do not rename engine files, reformat them, or retarget `patchy_*` libraries.
- Do not put GTK or `QWidget` types into `src/core`.
- Do not add a parallel selection, healing, or text algorithm in `src/ui-gnome` when `src/core` or the calibrated Qt text pipeline already has one.
- Do not put the next tool's pixel loop in `canvas_input.cpp`. That file only dispatches the gesture to a `patchy::` function or to a controller under `tools/`.

## Next boundary

Clone, healing, blur, sharpen, dodge, burn, and sponge are not core functions. Patchy implements them inside `CanvasWidget` (`canvas_widget_brush.cpp`, `canvas_widget_spot_healing.cpp`, `canvas_widget_patch_tool.cpp`). The GNOME `RetouchController` is a second painter and is not the performance path. Extracting those loops into the engine means moving the pixel math without changing it, then pointing both UIs at that function. Do that only with the tool-write canary running: the Qt brush path is byte-pinned. Text stays on the calibrated pipeline in `src/ui` until that pipeline can be called without a `QWidget`.
