# File Utilities

## Paths you can rely on

Use `hostInfo().workingPath` and `hostInfo().pluginsPath` as the base for plugin-owned files.

For example:

```cpp
const auto host = _host->hostInfo();
const auto pluginsDir = host.pluginsPath;
const auto workingDir = host.workingPath;
```

## Logs written by the client

Astrogram writes plugin-related diagnostics to:

- `tdata/plugins.log`
- `tdata/plugins.trace.jsonl`
- `tdata/plugins.recovery.json`
- `tdata/plugins.safe-mode`

## Recovery and safe mode

If Astrogram detects a crash in a tracked plugin operation, it can:

- disable the suspected plugin
- switch into safe mode
- show the plugin list in metadata-only mode
- queue a recovery notice for the next launch

## Plugin package behavior

Astrogram uses preview metadata before load, so install/update can inspect a `.tgd` package without executing plugin code.

That is why `TGD_PLUGIN_PREVIEW(...)` is strongly recommended.

## Packaging advice

- Keep plugin-owned data inside the plugin folder or a stable subfolder under `workingPath`.
- Use absolute paths for imported assets after copying them.
- Avoid assuming the process working directory outside the host API.
