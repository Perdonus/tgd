# Plugin Catalog

Versioned plugin sources and built `.tgd` packages live here.

Layout:
- `PluginCatalog/<plugin>/<version>/<plugin>.cpp`
- `PluginCatalog/<plugin>/<version>/<plugin>.tgd`

Rules:
- Keep only the latest version buildable in CI.
- Plugin-specific runners should build a single plugin via `TGD_CATALOG_PLUGIN`.
- Client-facing plugins currently targeted in this repo are:
  - `transparent_telegram`
  - `ai_chat`
  - `show_logs`
  - `ayu_safe`
  - `font_tuner`
  - `blur_telegram`
  - `accent_color`
