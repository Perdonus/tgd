# Troubleshooting

## Plugin does not appear in the list

Check these first:

- the file ends with `.tgd`
- it is placed in `<working dir>/tdata/plugins`
- the client and plugin architectures match
- the plugin was built against the current Astrogram plugin API

## Plugin loads as metadata only

Astrogram can read preview metadata without fully executing plugin code.

Common reasons a plugin remains metadata-only:

- it is disabled
- it was disabled by recovery after a crash
- ABI mismatch
- entry point not found

## Safe mode turned on after a crash

When a tracked plugin operation crashes the client, Astrogram may:

1. enable safe mode
2. disable the implicated plugin
3. keep the plugin visible in the manager
4. record details in `plugins.log` and recovery state files

Check:

- `<working dir>/tdata/plugins.safe-mode`
- `<working dir>/tdata/plugins.recovery.json`
- `<working dir>/tdata/plugins.log`

## Common plugin loading failures

### "Cannot load library"

Usually means one of these:

- missing dependent DLL
- Qt version mismatch
- compiler runtime mismatch
- incorrect exported plugin entry point

### Plugin installs but does nothing

Typical causes:

- your plugin never registers observers or settings callbacks
- it expects a window/session too early during startup
- it only reacts to commands and none were invoked

### Client crashes when opening plugin settings

Prefer host-rendered settings pages over raw plugin-owned dialogs. Host-rendered controls are the stable UI path in Astrogram.

## What to include in a bug report

- client build identifier
- exact plugin version
- steps to reproduce
- the relevant tail of `plugins.log`
- whether the problem happens only after restart or immediately after install
