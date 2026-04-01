# Plugin Catalog

This branch stores versioned Telegram Desktop plugins outside the app sources.

Layout
- `PluginCatalog/<plugin-name>/<version>/<plugin-name>.cpp`
- `PluginCatalog/<plugin-name>/<version>/<plugin-name>.tgd`

Rules
- Each plugin gets its own folder.
- Each released version gets its own subfolder.
- The version folder keeps the source file and the compiled `.tgd` built from the same branch state.
- Compatibility rebuilds should be published as a new version folder instead of rewriting older releases in place.
- `.tgd` files are native binaries and must be rebuilt against the exact Telegram Desktop ABI they target.
- This branch tracks the current plugin ABI from `Telegram/SourceFiles/plugins/plugins_api.h`.
- If `kApiVersion`, compiler ABI, platform, architecture, or Qt major/minor changes, the catalog binaries must be rebuilt.
- CI rebuilds only the latest version folder for each plugin; older folders stay as archived releases.

CI
- The `Plugins` branch uses one workflow per plugin.
- `build-plugin-transparent-telegram.yml`, `build-plugin-ai-chat.yml`, and `build-plugin-ayu-safe.yml` call the reusable `.github/workflows/build-plugins.yml`.
- Each workflow is path-filtered to its own plugin sources plus shared plugin build files.
- The Windows job restores the cached Qt/toolchain first and skips `prepare` when the expected toolchain is already present.
- Successful runs build only the selected plugin, upload that plugin's `.tgd` files as artifacts, and commit refreshed binaries back into the matching version folder.

Local build
- Prepare a static Qt matching Telegram Desktop on your platform.
- Then run:
  `cmake -S PluginCatalog -B out/plugin-catalog -D CMAKE_PREFIX_PATH=/path/to/Qt -D TGD_CATALOG_PLUGIN=transparent_telegram`
  `cmake --build out/plugin-catalog --config Release --target plugin_catalog`
- Omit `TGD_CATALOG_PLUGIN` only when you intentionally want to build every catalog entry present in the tree.
