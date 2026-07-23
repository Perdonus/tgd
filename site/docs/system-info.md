# System Info

`Plugins::SystemInfo` gives plugins a stable, read-only view of the current environment.

## Structure

```cpp
struct SystemInfo {
	quint64 processId;
	quint64 totalMemoryBytes;
	quint64 availableMemoryBytes;
	int logicalCpuCores;
	int physicalCpuCores;
	QString productType;
	QString productVersion;
	QString prettyProductName;
	QString kernelType;
	QString kernelVersion;
	QString architecture;
	QString buildAbi;
	QString hostName;
	QString userName;
	QString locale;
	QString uiLanguage;
	QString timeZone;
};
```

## Example

```cpp
const auto system = _host->systemInfo();
const auto line = QString("%1 • %2 • %3")
	.arg(system.prettyProductName)
	.arg(system.architecture)
	.arg(system.timeZone);
_host->showToast(line);
```

## Example: structured diagnostics dump

```cpp
const auto system = _host->systemInfo();
QStringList lines;
lines.push_back(QString("OS: %1").arg(system.prettyProductName));
lines.push_back(QString("Kernel: %1 %2")
	.arg(system.kernelType)
	.arg(system.kernelVersion));
lines.push_back(QString("CPU: %1 logical / %2 physical")
	.arg(system.logicalCpuCores)
	.arg(system.physicalCpuCores));
lines.push_back(QString("Memory: %1 / %2 MiB free")
	.arg(system.availableMemoryBytes / 1024 / 1024)
	.arg(system.totalMemoryBytes / 1024 / 1024));
lines.push_back(QString("Locale: %1 (%2)")
	.arg(system.locale)
	.arg(system.uiLanguage));

_host->log(lines.join('\n'));
```

## Good use cases

- diagnostics plugins
- environment-aware feature switches
- debug pages
- plugin bug reports

## Bad use cases

- fingerprinting users without consent
- sending system data to remote services silently
- assuming the values never change during the app lifetime
