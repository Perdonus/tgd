# Setup

## Requirements

Astrogram plugins must be compiled against the same native environment as the client build:

- same platform
- same pointer size
- same compiler ABI
- same Qt major/minor version
- same Astrogram plugin API version

If any of these differ, the plugin manager will reject the binary as an ABI mismatch.

## Important paths

- Plugins folder: `<working dir>/tdata/plugins`
- Manual enable/disable state: `<working dir>/tdata/plugins.json`
- Safe mode flag: `<working dir>/tdata/plugins.safe-mode`
- Main plugin log: `<working dir>/tdata/plugins.log`
- Recovery state: `<working dir>/tdata/plugins.recovery.json`
- Trace log: `<working dir>/tdata/plugins.trace.jsonl`

## Source files you need

- API header: `Telegram/SourceFiles/plugins/plugins_api.h`
- Example plugins: `Telegram/Plugins/Examples`
- Catalog versions: `PluginCatalog/<plugin>/<version>`

## Build expectations

At minimum:

- `QtCore`
- for UI plugins: `QtWidgets`
- for network plugins: `QtNetwork`

Astrogram ships selective GitHub Actions workflows for plugin builds, so small plugin edits do not need to rebuild the whole plugin catalog every time.

## Installing a plugin

1. Build a shared library.
2. Rename the output to `.tgd`.
3. Copy it into `<working dir>/tdata/plugins`.
4. Open `Settings > Astrogram > Plugins`.

The client reloads plugins automatically after install or update.

## Preview metadata

Use `TGD_PLUGIN_PREVIEW(...)` so Astrogram can show install/update metadata before running any plugin code.

This is how in-chat plugin installation can show:

- plugin name
- version
- author
- description
- website
- optional icon reference

without loading the binary itself.
