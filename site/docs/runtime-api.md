# Runtime API

Astrogram no longer hides runtime diagnostics behind unlock gestures or repeated taps.

## Host info

`host->hostInfo()` always exposes runtime fields:

```cpp
const auto info = _host->hostInfo();

info.runtimeApiEnabled;
info.runtimeApiPort;
info.runtimeApiBaseUrl;
info.workingPath;
info.pluginsPath;
info.safeModeEnabled;
```

Even when the runtime API is disabled, these fields remain part of the public contract.

## Runtime diagnostics

Use these values to:

- show developer info in a plugin
- log the current runtime endpoint
- expose status in a diagnostics page
- decide whether a plugin can talk to an external helper

## System info

`host->systemInfo()` provides machine-level metadata:

- process ID
- total and available memory
- logical and physical CPU counts
- operating system details
- locale and UI language
- time zone
- user and host names

## Recommended check

```cpp
const auto host = _host->hostInfo();
if (host.runtimeApiEnabled && host.runtimeApiPort > 0) {
	_host->showToast(
		QString("Runtime at %1").arg(host.runtimeApiBaseUrl));
}
```
