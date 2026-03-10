# Plugin Catalog

This branch stores versioned Telegram Desktop plugins outside the app sources.

Layout
- `PluginCatalog/<plugin-name>/<version>/<plugin-name>.cpp`
- `PluginCatalog/<plugin-name>/<version>/<plugin-name>.tgd`

Rules
- Each plugin gets its own folder.
- Each released version gets its own subfolder.
- The version folder keeps the source file and the compiled `.tgd` built from the same branch state.
- `.tgd` files are native binaries and must be rebuilt against the exact Telegram Desktop ABI they target.

CI
- The `Plugins` branch uses the `Build Plugin Catalog` workflow.
- It prepares only the minimal Windows stages needed for static Qt 5.15.18 and then builds `PluginCatalog/CMakeLists.txt`.
- Successful runs upload the built `.tgd` files as artifacts and commit refreshed binaries back into the matching version folders.

Local build
- Prepare a static Qt matching Telegram Desktop on your platform.
- Then run:
  `cmake -S PluginCatalog -B out/plugin-catalog -D CMAKE_PREFIX_PATH=/path/to/Qt`
  `cmake --build out/plugin-catalog --config Release --target plugin_catalog`
