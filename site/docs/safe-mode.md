# Safe Mode & Recovery

Astrogram can switch into safe mode when a plugin crashes during a tracked operation such as install, update, load or settings entry.

## Main recovery files

- `tdata/plugins.log`
- `tdata/plugins.trace.jsonl`
- `tdata/plugins.json`
- `tdata/plugins.recovery.json`
- `tdata/plugins.safe-mode`

## Recovery model

1. The plugin manager records the operation that is about to enter risky code.
2. If the process crashes before the operation is cleared, recovery state remains on disk.
3. On the next launch Astrogram can disable the implicated plugin set and enter safe mode.
4. The plugin remains visible in the manager so the user can inspect and remove it.

## Best practices for plugin authors

- keep `onLoad()` small and deterministic
- avoid long blocking work on the UI thread
- prefer host-rendered settings pages over ad-hoc raw dialogs
- log important transitions into `plugins.log`
- unregister handlers, timers and observers on unload

## Best practices for bug reports

Collect:

- client build / commit
- plugin name and version
- `log.txt`
- `tdata/plugins.log`
- `tdata/plugins.trace.jsonl`
- whether the crash happened during install, restart or settings open
