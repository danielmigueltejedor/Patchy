<div align="center">
  <img src="src/ui/icons/lienzo-app.png" width="176" height="176" alt="Lienzo">
  <h1>Lienzo</h1>
  <p><strong>A modern, native image editor for layered creative work, PSD compatibility and local-first automation.</strong></p>

  <p>
    <img src="https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-5E81AC" alt="Windows, macOS and Linux">
    <img src="https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white" alt="Qt 6">
    <img src="https://img.shields.io/badge/C%2B%2B-native-00599C?logo=cplusplus&logoColor=white" alt="Native C++">
    <a href="./LICENSE"><img src="https://img.shields.io/github/license/danielmigueltejedor/Lienzo" alt="MIT license"></a>
    <img src="https://img.shields.io/github/last-commit/danielmigueltejedor/Lienzo?label=updated" alt="Last commit">
  </p>

  <p>
    <a href="#overview">Overview</a> ·
    <a href="#preview">Preview</a> ·
    <a href="#core-capabilities">Features</a> ·
    <a href="#psd-and-psb-compatibility">PSD compatibility</a> ·
    <a href="#local-ai-control-and-automation">AI control</a> ·
    <a href="#installation">Installation</a> ·
    <a href="#building-from-source">Build</a> ·
    <a href="#support-and-contributions">Contribute</a>
  </p>
</div>

---

## Overview

**Lienzo** is a native C++/Qt image editor built for serious layered raster and vector workflows. It focuses on editable documents, strong PSD/PSB interoperability, precise local processing and a desktop experience that remains fast, private and fully under the user’s control.

Lienzo builds on the open-source **Patchy** codebase originally created by Seth A. Robinson. The fork introduces its own identity, packaging, application ID (`com.nodalix.lienzo`), visual language and ongoing interface redesign while preserving the mature editing engine, compatibility work and regression coverage inherited from upstream.

> [!IMPORTANT]
> **Lienzo is under active development.** The editing engine already covers a wide surface, but the project is not a complete replacement for every Photoshop workflow. See [Project status](#project-status) for the main limitations.

## Why Lienzo

| Design | Editing | Automation |
|---|---|---|
| Native Qt desktop interface | Layered PSD/PSB workflows | Built-in JavaScript scripting |
| Cohesive dark visual language | Raster, vector and text editing | Native MCP connector for local AI tools |
| Responsive desktop-first controls | Smart Objects and Smart Filters | Headless and command-line workflows |
| Cross-platform packaging | Non-destructive adjustments | Scriptable documents, layers and exports |
| Local-first, no telemetry | Photoshop-oriented round trips | Preview, inspect, edit and save locally |

Lienzo is designed as a real desktop creative tool rather than a remote service. Documents stay on the machine, editing happens locally and the application does not require an account or cloud backend.

## Preview

<p align="center">
  <a href="docs/images/screenshots/levels.png"><img src="docs/images/screenshots/levels.png" width="31%" alt="Levels adjustment in Lienzo"></a>
  <a href="docs/images/screenshots/layer_styles.png"><img src="docs/images/screenshots/layer_styles.png" width="31%" alt="Layer styles in Lienzo"></a>
  <a href="docs/images/screenshots/smart_objects.png"><img src="docs/images/screenshots/smart_objects.png" width="31%" alt="Smart Objects in Lienzo"></a>
</p>

<p align="center">
  <a href="docs/images/screenshots/brush_dynamics.png"><img src="docs/images/screenshots/brush_dynamics.png" width="31%" alt="Brush dynamics in Lienzo"></a>
  <a href="docs/images/screenshots/vector_tools.png"><img src="docs/images/screenshots/vector_tools.png" width="31%" alt="Vector tools in Lienzo"></a>
  <a href="docs/images/screenshots/camera_raw.png"><img src="docs/images/screenshots/camera_raw.png" width="31%" alt="Camera Raw development in Lienzo"></a>
</p>

## Core capabilities

### Layered editing

- PSD and PSB opening with layers, groups, masks, clipping masks, blend modes and channels.
- Editable text layers with rich text, paragraph controls, tracking, leading and scaling.
- Layer styles, Fill Opacity, adjustment layers and Photoshop-compatible preset workflows.
- Embedded and linked Smart Objects with non-destructive transforms and editable contents.
- Native Smart Filter stacks with shared masks, per-filter opacity and blend controls.
- Multiple documents with tabs, floating windows, Tile/Cascade layouts and cross-document layer movement.

### Raster tools

- Brush, Eraser, Clone Stamp, Healing Brush, Spot Healing, Patch, Smudge, Blur, Sharpen, Dodge, Burn and Sponge.
- Marquee, lasso, magnetic lasso, quick selection, magic wand and Quick Mask workflows.
- Crop, transforms, warp transforms, gradients, fills and image/canvas sizing.
- Filter Gallery, Liquify, blur/sharpen filters and reusable effect stacks.
- Pressure-aware brush dynamics for size, opacity, flow, angle, scatter and colour.

### Vector and typography

- Pen paths, editable anchors and a dedicated Paths panel.
- Rectangle, ellipse, polygon, line and custom shape layers.
- Solid, gradient and pattern fills and strokes.
- SVG import as editable shapes and SVG export with vectors preserved.
- Trace Image to Shapes for converting raster artwork into editable vector layers.
- Warp Text with the supported Photoshop-style warp presets.

### Colour, patterns and game-art workflows

- Indexed/palette editing with built-in retro palettes and imported palette formats.
- Pattern, gradient and layer-style libraries with import/export support.
- Seamless texture authoring and live tile preview.
- Sprite-sheet and image-sequence import/export.
- Animated GIF import/export with per-frame timing.
- Pixel-art export with nearest-neighbour scaling and palette-constrained workflows.

### File formats and import workflows

Lienzo supports a broad set of raster and layered formats, including:

`PSD` · `PSB` · `PNG` · `JPEG` · `TIFF` · `WebP` · `BMP` · `TGA` · `GIF` · `PCX` · `IFF/LBM` · `ICO/CUR` · `Aseprite` · `JPEG XR` · `RTTEX` · `SVG`

Additional workflows include:

- Affinity Photo, Designer and Publisher document import as layered content where supported.
- Camera RAW development for formats such as CR2, CR3, NEF, ARW, RAF and DNG.
- HEIC/HEIF through available platform codecs.
- PDF import on supported desktop builds and PDF export.
- Printing, scanner import and camera import where the platform provides the required APIs.
- Classic Photoshop `.8bf` filter plug-ins on Windows through isolated helper processes.

## PSD and PSB compatibility

PSD compatibility is a central engineering goal rather than a checkbox feature. The repository contains dedicated regression tests and a documented compatibility benchmark against Photoshop.

The last upstream benchmark baseline inherited by Lienzo used a mixed 64-file PSD corpus and recorded:

| Baseline | Files opened | Perceptual render match | PSD saves rejected by Photoshop | Editable text after round trip |
|---|---:|---:|---:|---:|
| Patchy engine `879a3a8` | 64 / 64 | 98.83% (`n=63`) | 0 / 64 | 312 / 312 |

This is a **historical corpus-specific baseline**, not a claim that every PSD feature or every document is perfectly compatible with the current Lienzo build.

See the [PSD compatibility benchmark](docs/psd-compatibility-benchmark.md) for the full methodology, per-file results, tested versions and known limitations.

## Local AI control and automation

Lienzo includes a native **MCP connector** and a JavaScript scripting environment so local automation tools can work with editable documents instead of flattening everything to screenshots.

An assistant can:

- inspect the current document and layer structure;
- request fresh canvas previews;
- create and edit layers, text, paths and shapes;
- use pressure-aware brush strokes and reusable brush presets;
- work with selections, filters, colours and palettes;
- export or save editable documents;
- attach to the user’s open Lienzo workspace or operate in an isolated workspace.

The desktop application includes setup guidance under **Help → Set up AI Control**.

See the [AI control guide](docs/ai-control.md) and bundled [scripting guide](scripts/bundled/scripting-guide.md).

## Privacy

Lienzo is local-first by design:

- no telemetry;
- no advertising;
- no account requirement;
- no document uploads;
- no cloud-processing requirement;
- user settings and creative data remain local.

When update checks are enabled, the application contacts GitHub only to discover whether a newer release is available.

## Installation

Packaged builds are distributed through [GitHub Releases](https://github.com/danielmigueltejedor/Lienzo/releases).

| Platform | Package | Stable asset name |
|---|---|---|
| Windows 10/11 x64 | Installer | `LienzoWindowsInstaller.exe` |
| Windows 10/11 x64 | Portable ZIP | `LienzoWindowsNoInstaller.zip` |
| macOS | DMG | `LienzoMacOS.dmg` |
| Linux | Flatpak bundle | `LienzoLinux.flatpak` |

> [!NOTE]
> If no Lienzo-branded release has been published yet, build the current source tree using the instructions below. Historical releases inherited from the upstream repository may still use Patchy-era asset names.

### Linux Flatpak

Once a Lienzo release is available, the standalone bundle can be installed for the current user with:

```bash
curl -L \
  -o /tmp/LienzoLinux.flatpak \
  https://github.com/danielmigueltejedor/Lienzo/releases/latest/download/LienzoLinux.flatpak

flatpak install --user -y /tmp/LienzoLinux.flatpak
```

The application ID is:

```text
com.nodalix.lienzo
```

### WebAssembly

The source tree also contains a WebAssembly build and browser shell. Web builds execute the editor locally in the browser; document processing is not sent to a remote editing service.

Desktop builds remain the preferred environment for large documents and platform-specific functionality.

## Building from source

### Core and tests

```bash
cmake --preset dev -DPATCHY_BUILD_APP=OFF
cmake --build --preset dev
ctest --preset dev
```

### Qt desktop application

```bash
cmake --preset qt-local
cmake --build --preset qt-local
```

### macOS and Linux release builds

```bash
cmake --preset mac-release
cmake --build --preset mac-release
```

or:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

> [!NOTE]
> Some internal target names, environment variables and compatibility identifiers still use the historical `patchy` / `PATCHY_` naming while the rebrand is migrated safely. They remain implementation details and are being changed separately from the public Lienzo identity.

## Translations

Lienzo includes UI catalogues for English, German, Spanish, French, Italian, Japanese, Simplified Chinese and Traditional Chinese. Translation catalogues are validated during the build to detect stale or unbound UI strings.

## Repository layout

| Path | Contents |
|---|---|
| `src/app/` | Application entry points, MCP executable and platform resources |
| `src/ui/` | Qt interface, panels, dialogs, tools and visual system |
| `src/core/` | Editing and image-processing engine |
| `src/formats/` | File-format readers and writers |
| `scripts/` | Bundled scripts, tests and release automation |
| `packaging/` | Windows, macOS, Linux and web packaging |
| `translations/` | Application translation catalogues |
| `tests/` | Regression, UI, format and compatibility tests |
| `docs/` | Architecture, compatibility, workflow and developer documentation |
| `agent-kit/` | Local AI/MCP integration material |

## Project status

Current limitations include:

- no complete CMYK/Lab editing and export workflow;
- no full 16/32-bit editing pipeline;
- incomplete support for every Photoshop adjustment, Smart Filter and metadata block;
- layered PSB writing is not yet complete across the full Photoshop feature surface;
- some Affinity features import approximately or as preserved placeholders;
- no GPU-accelerated rendering pipeline yet;
- classic `.8bf` plug-ins are Windows-only;
- Photoshop Actions, UXP/JSX panels and Photoshop-specific scripts are not directly compatible.

Unsupported Photoshop data is preserved where practical so opening and resaving a document does not unnecessarily destroy information Lienzo cannot yet edit.

## Support and contributions

- Use [GitHub Issues](https://github.com/danielmigueltejedor/Lienzo/issues) for reproducible bugs and focused feature requests.
- Include the operating system, Lienzo version or commit, reproduction steps and relevant screenshots/logs.
- For PSD compatibility reports, attach a minimal reproducible document when licensing and privacy allow it.
- Never publish private documents, credentials or personal data in an issue.
- Keep pull requests focused and include tests for behaviour changes where practical.
- Run the relevant regression suite before submitting a change.

The repository contains extensive automated coverage for UI behaviour, file formats, compatibility and packaging. Changes that affect Photoshop round trips should preserve or extend the relevant compatibility tests.

## Credits and lineage

Lienzo is based on **[Patchy](https://github.com/SethRobinson/Patchy)**, originally created by **Seth A. Robinson**.

The fork preserves the MIT-licensed upstream work and its contributor history. Upstream code contributions include work from [mcapogna](https://github.com/mcapogna), [csbun](https://github.com/csbun) and [ifloppy](https://github.com/ifloppy).

**Lienzo** is redesigned and maintained by [Daniel Miguel Tejedor](https://github.com/danielmigueltejedor).

## License

Lienzo is released under the [MIT License](./LICENSE). Third-party runtime notices are tracked in [NOTICE-THIRD-PARTY.md](./NOTICE-THIRD-PARTY.md).

## Trademark notice

Adobe and Photoshop are trademarks or registered trademarks of Adobe in the United States and/or other countries.

Lienzo is an independent open-source project and is not affiliated with, authorized by, endorsed by or sponsored by Adobe. References to Photoshop, PSD/PSB, Smart Objects, Smart Filters and the 8BF plug-in format are used only to describe interoperability and compatibility.

<div align="center">
  <sub>Designed and maintained by <a href="https://github.com/danielmigueltejedor">Daniel Miguel Tejedor</a>.</sub>
  <br>
  <sub>Based on the open-source Patchy project originally created by Seth A. Robinson.</sub>
  <br><br>
  <a href="https://buymeacoffee.com/danielmigueltejedor"><img src="https://img.shields.io/badge/Support%20the%20project-Buy%20Me%20a%20Coffee-FFDD00?logo=buymeacoffee&logoColor=000" alt="Support Lienzo on Buy Me a Coffee"></a>
</div>
