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
- The `Plugins` branch builds the `tgd_catalog_plugins` target on GitHub Actions.
- Successful runs upload the built `.tgd` files as artifacts and commit refreshed binaries back into the matching version folders.
