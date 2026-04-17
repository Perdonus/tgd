/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/launcher.h"

#include "platform/platform_launcher.h"
#include "platform/platform_specific.h"
#include "base/options.h"
#include "base/platform/base_platform_info.h"
#include "base/platform/base_platform_file_utilities.h"
#include "ui/main_queue_processor.h"
#include "core/crash_reports.h"
#include "core/update_checker.h"
#include "core/sandbox.h"
#include "logs.h"
#include "base/concurrent_timer.h"
#include "base/options.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLibraryInfo>
#include <QtCore/QLoggingCategory>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Core {
namespace {

uint64 InstallationTag = 0;

base::options::toggle OptionHighDpiDownscale({
	.id = kOptionHighDpiDownscale,
	.name = "High DPI downscale",
	.description = "Follow system interface scale settings exactly"
		" (another approach, likely better quality).",
	.scope = [] {
		return !Platform::IsMac()
			&& QLibraryInfo::version() >= QVersionNumber(6, 4);
	},
	.restartRequired = true,
});

base::options::toggle OptionFreeType({
	.id = kOptionFreeType,
	.name = "FreeType font engine",
	.description = "Use the font engine from Linux instead of the system one.",
	.scope = base::options::windows | base::options::macos,
	.restartRequired = true,
});

class FilteredCommandLineArguments {
public:
	FilteredCommandLineArguments(int argc, char **argv);

	int &count();
	char **values();

private:
	static constexpr auto kForwardArgumentCount = 1;

	int _count = 0;
	std::vector<QByteArray> _owned;
	std::vector<char*> _arguments;

	void pushArgument(const char *text);

};

FilteredCommandLineArguments::FilteredCommandLineArguments(
	int argc,
	char **argv) {
	// For now just pass only the first argument, the executable path.
	for (auto i = 0; i != kForwardArgumentCount; ++i) {
		pushArgument(argv[i]);
	}

#if defined Q_OS_WIN || defined Q_OS_MAC
	if (OptionFreeType.value() || OptionHighDpiDownscale.value()) {
		pushArgument("-platform");
#ifdef Q_OS_WIN
		pushArgument("windows:fontengine=freetype");
#else // Q_OS_WIN
		pushArgument("cocoa:fontengine=freetype");
#endif // !Q_OS_WIN
	}
#endif // Q_OS_WIN || Q_OS_MAC

	pushArgument(nullptr);
}

int &FilteredCommandLineArguments::count() {
	_count = _arguments.size() - 1;
	return _count;
}

char **FilteredCommandLineArguments::values() {
	return _arguments.data();
}

void FilteredCommandLineArguments::pushArgument(const char *text) {
	_owned.emplace_back(text);
	_arguments.push_back(_owned.back().data());
}

QString DebugModeSettingPath() {
	return cWorkingDir() + u"tdata/withdebug"_q;
}

void WriteDebugModeSetting() {
	auto file = QFile(DebugModeSettingPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(Logs::DebugEnabled() ? "1" : "0");
	}
}

void ComputeDebugMode() {
	Logs::SetDebugEnabled(cAlphaVersion() != 0);
	const auto debugModeSettingPath = DebugModeSettingPath();
	auto file = QFile(debugModeSettingPath);
	if (file.exists() && file.open(QIODevice::ReadOnly)) {
		Logs::SetDebugEnabled(file.read(1) != "0");
#if defined _DEBUG
	} else {
		Logs::SetDebugEnabled(true);
#endif
	}
	if (cDebugMode()) {
		Logs::SetDebugEnabled(true);
	}
	if (Logs::DebugEnabled()) {
		QLoggingCategory::setFilterRules("qt.qpa.gl.debug=true");
	}
}

void ComputeExternalUpdater() {
	auto locations = QStandardPaths::standardLocations(
		QStandardPaths::AppDataLocation);
	if (locations.isEmpty()) {
		locations << QString();
	}
	locations[0] = QDir::cleanPath(cWorkingDir());
	locations << QDir::cleanPath(cExeDir());
	for (const auto &location : locations) {
		const auto dir = location + u"/externalupdater.d"_q;
		for (const auto &info : QDir(dir).entryInfoList(QDir::Files)) {
			QFile file(info.absoluteFilePath());
			if (file.open(QIODevice::ReadOnly)) {
				QTextStream fileStream(&file);
				while (!fileStream.atEnd()) {
					const auto path = fileStream.readLine();
					if (path == (cExeDir() + cExeName())) {
						SetUpdaterDisabledAtStartup();
						return;
					}
				}
			}
		}
	}
}

QString InstallBetaVersionsSettingPath() {
	return cWorkingDir() + u"tdata/devversion"_q;
}

void WriteInstallBetaVersionsSetting() {
	QFile f(InstallBetaVersionsSettingPath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write(cInstallBetaVersion() ? "1" : "0");
	}
}

void ComputeInstallBetaVersions() {
	const auto installBetaSettingPath = InstallBetaVersionsSettingPath();
	if (cAlphaVersion()) {
		cSetInstallBetaVersion(false);
	} else if (QFile::exists(installBetaSettingPath)) {
		QFile f(installBetaSettingPath);
		if (f.open(QIODevice::ReadOnly)) {
			cSetInstallBetaVersion(f.read(1) != "0");
		}
	} else if (AppBetaVersion) {
		WriteInstallBetaVersionsSetting();
	}
}

void ComputeInstallationTag() {
	InstallationTag = 0;
	auto file = QFile(cWorkingDir() + u"tdata/usertag"_q);
	if (file.open(QIODevice::ReadOnly)) {
		const auto result = file.read(
			reinterpret_cast<char*>(&InstallationTag),
			sizeof(uint64));
		if (result != sizeof(uint64)) {
			InstallationTag = 0;
		}
		file.close();
	}
	if (!InstallationTag) {
		auto generator = std::mt19937(std::random_device()());
		auto distribution = std::uniform_int_distribution<uint64>();
		do {
			InstallationTag = distribution(generator);
		} while (!InstallationTag);

		if (file.open(QIODevice::WriteOnly)) {
			file.write(
				reinterpret_cast<char*>(&InstallationTag),
				sizeof(uint64));
			file.close();
		}
	}
}

#ifdef Q_OS_WIN
struct RuntimeApiCommandState {
	bool enabled = false;
	int port = 37080;
};

struct RuntimeApiPathSyncResult {
	bool sessionPathUpdated = false;
	bool userPathUpdated = false;
	bool userPathContainsCommand = false;
};

QString NormalizePathKey(QString path) {
	if (path.isEmpty()) {
		return QString();
	}
	return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

QString EscapeBatchValue(QString value) {
	value.replace("\r\n", " ");
	value.replace('\r', ' ');
	value.replace('\n', ' ');
	value.replace(u'%', u"%%"_q);
	return value;
}

QString RuntimeApiUserBinDir() {
	auto base = QStandardPaths::writableLocation(
		QStandardPaths::AppLocalDataLocation);
	if (base.isEmpty()) {
		base = cWorkingDir() + u"tdata"_q;
	}
	return QDir::cleanPath(base + u"/bin"_q);
}

QStringList RuntimeApiInstallDirs() {
	auto result = QStringList{
		RuntimeApiUserBinDir(),
		QDir::cleanPath(cWorkingDir() + u"tdata/bin"_q),
	};
	result.removeDuplicates();
	return result;
}

QStringList RuntimeApiTemplateCandidates() {
	const auto exeDir = QDir(cExeDir());
	const auto workingDir = QDir(cWorkingDir());
	auto result = QStringList{
		exeDir.absoluteFilePath(u"tools/runtime_api_windows.bat"_q),
		exeDir.absoluteFilePath(u"../tools/runtime_api_windows.bat"_q),
		exeDir.absoluteFilePath(u"../../tools/runtime_api_windows.bat"_q),
		workingDir.absoluteFilePath(u"tools/runtime_api_windows.bat"_q),
	};
	result.removeDuplicates();
	return result;
}

QString NormalizeWindowsBatchLineEndings(QString script) {
	script.replace("\r\n", "\n");
	script.replace('\r', '\n');
	script.replace("\n", "\r\n");
	return script;
}

QString RuntimeApiFallbackScript() {
	return QString::fromLatin1(R"ASTROBAT(
@echo off
setlocal EnableExtensions

if exist "%~dp0astro-vars.bat" call "%~dp0astro-vars.bat"

if not defined ASTRO_RUNTIME_HOST set "ASTRO_RUNTIME_HOST=127.0.0.1"
if not defined ASTRO_RUNTIME_PORT set "ASTRO_RUNTIME_PORT=37080"
if not defined ASTRO_BASE_URL set "ASTRO_BASE_URL=http://%ASTRO_RUNTIME_HOST%:%ASTRO_RUNTIME_PORT%"
if not defined ASTRO_CLIENT_LOG if defined ASTRO_WORKING_DIR set "ASTRO_CLIENT_LOG=%ASTRO_WORKING_DIR%client.log"
if not defined ASTRO_PLUGINS_LOG if defined ASTRO_WORKING_DIR set "ASTRO_PLUGINS_LOG=%ASTRO_WORKING_DIR%tdata\plugins.log"
if not defined ASTRO_PLUGINS_CONFIG if defined ASTRO_WORKING_DIR set "ASTRO_PLUGINS_CONFIG=%ASTRO_WORKING_DIR%tdata\plugins.json"
if not defined ASTRO_COMMAND_DIR set "ASTRO_COMMAND_DIR=%~dp0"

if "%~1"=="" goto help
if /I "%~1"=="help" goto help
if /I "%~1"=="api" goto api
if /I "%~1"=="status" goto status
if /I "%~1"=="health" goto health
if /I "%~1"=="host" goto host
if /I "%~1"=="system" goto system
if /I "%~1"=="diagnostics" goto diagnostics
if /I "%~1"=="runtime" goto runtime
if /I "%~1"=="plugins" goto plugins
if /I "%~1"=="plugin" goto plugins
if /I "%~1"=="safe-mode" goto safe_mode
if /I "%~1"=="chats" goto chats
if /I "%~1"=="messages" goto messages
if /I "%~1"=="send" goto send
if /I "%~1"=="logs" goto logs

echo Unknown command: %~1
echo.
goto help

:api
shift
if "%~1"=="" goto api_root
set "ASTRO_METHOD=%~1"
if /I "%ASTRO_METHOD%"=="GET" goto api_method
if /I "%ASTRO_METHOD%"=="POST" goto api_method
if /I "%ASTRO_METHOD%"=="PUT" goto api_method
if /I "%ASTRO_METHOD%"=="PATCH" goto api_method
if /I "%ASTRO_METHOD%"=="DELETE" goto api_method
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=%~1"
set "ASTRO_RAW_BODY="
goto api_passthrough

:api_method
shift
if "%~1"=="" goto help_api
set "ASTRO_PATH=%~1"
shift
set "ASTRO_RAW_BODY=%*"
goto api_passthrough

:api_root
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1"
set "ASTRO_RAW_BODY="
goto api_passthrough

:api_passthrough
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$base = if ($env:ASTRO_BASE_URL) { $env:ASTRO_BASE_URL } else { 'http://127.0.0.1:37080' };" ^
  "$path = if ($env:ASTRO_PATH) { $env:ASTRO_PATH } else { '/v1' };" ^
  "if (-not $path.StartsWith('/')) { $path = '/' + $path.TrimStart('/') }" ^
  "$uri = $base + $path;" ^
  "$bodyText = if ($env:ASTRO_RAW_BODY) { $env:ASTRO_RAW_BODY.Trim() } else { '' };" ^
  "try { if ($bodyText) { $null = $bodyText | ConvertFrom-Json; $response = Invoke-RestMethod -Method $env:ASTRO_METHOD -Uri $uri -ContentType 'application/json; charset=utf-8' -Body $bodyText } else { $response = Invoke-RestMethod -Method $env:ASTRO_METHOD -Uri $uri }; $response | ConvertTo-Json -Depth 16 } catch { $payload = ''; if ($_.Exception.Response) { try { $reader = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd() } catch {} } if ($payload) { Write-Host $payload } else { if ($bodyText) { Write-Host 'If you pass a body to astro api, it must be valid JSON.' }; Write-Host ('Astrogram Runtime API is unavailable at ' + $base + '.'); Write-Host 'Enable Runtime API in Astrogram settings or use astro runtime on for the next launch.'; Write-Host $_.Exception.Message } exit 1 }"
exit /b %errorlevel%

:status
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$base = if ($env:ASTRO_BASE_URL) { $env:ASTRO_BASE_URL } else { 'http://127.0.0.1:37080' };" ^
  "$configPath = $env:ASTRO_PLUGINS_CONFIG;" ^
  "$configuredEnabled = $false;" ^
  "$configuredPort = if ($env:ASTRO_RUNTIME_PORT) { [int]$env:ASTRO_RUNTIME_PORT } else { 37080 };" ^
  "if ($configPath -and (Test-Path $configPath)) { try { $doc = Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json; if ($null -ne $doc.runtimeApi) { if ($null -ne $doc.runtimeApi.enabled) { $configuredEnabled = [bool]$doc.runtimeApi.enabled }; if ($doc.runtimeApi.port) { $configuredPort = [int]$doc.runtimeApi.port } } } catch {} }" ^
  "$reachable = $false; $health = $null;" ^
  "try { $health = Invoke-RestMethod -Method GET -Uri ($base + '/v1/health'); $reachable = $true } catch {}" ^
  "[pscustomobject]@{ runtimeReachable = $reachable; baseUrl = $base; configuredEnabled = $configuredEnabled; configuredPort = $configuredPort; workingDir = $env:ASTRO_WORKING_DIR; commandDir = $env:ASTRO_COMMAND_DIR; clientLog = $env:ASTRO_CLIENT_LOG; pluginsLog = $env:ASTRO_PLUGINS_LOG; pluginsConfig = $env:ASTRO_PLUGINS_CONFIG; health = $health } | ConvertTo-Json -Depth 16"
exit /b %errorlevel%

:health
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/health"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:host
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/host"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:system
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/system"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:diagnostics
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/diagnostics"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:runtime
shift
if "%~1"=="" goto help_runtime
if /I "%~1"=="on" goto runtime_on
if /I "%~1"=="off" goto runtime_off
goto help_runtime

:runtime_on
shift
if "%~1"=="" (
  set "ASTRO_ARG1=%ASTRO_RUNTIME_PORT%"
) else (
  set "ASTRO_ARG1=%~1"
)
set "ASTRO_METHOD=POST"
set "ASTRO_PATH=/v1/runtime"
set "ASTRO_BODY_KIND=runtime-on"
call :api_request
if not errorlevel 1 exit /b 0
set "ASTRO_RUNTIME_FALLBACK_ENABLED=1"
set "ASTRO_RUNTIME_FALLBACK_PORT=%ASTRO_ARG1%"
call :runtime_config_fallback
exit /b %errorlevel%

:runtime_off
set "ASTRO_METHOD=POST"
set "ASTRO_PATH=/v1/runtime"
set "ASTRO_BODY_KIND=runtime-off"
call :api_request
if not errorlevel 1 exit /b 0
set "ASTRO_RUNTIME_FALLBACK_ENABLED=0"
set "ASTRO_RUNTIME_FALLBACK_PORT="
call :runtime_config_fallback
exit /b %errorlevel%

:plugins
shift
if "%~1"=="" (
  set "ASTRO_METHOD=GET"
  set "ASTRO_PATH=/v1/plugins"
  set "ASTRO_BODY_KIND="
  call :api_request
  exit /b %errorlevel%
)
if /I "%~1"=="info" (
  shift
  if "%~1"=="" goto help_plugins
  set "ASTRO_METHOD=GET"
  set "ASTRO_PATH=/v1/plugins/%~1"
  set "ASTRO_RAW_BODY="
  goto api_passthrough
)
if /I "%~1"=="reload" (
  set "ASTRO_METHOD=POST"
  set "ASTRO_PATH=/v1/plugins/reload"
  set "ASTRO_BODY_KIND="
  call :api_request
  exit /b %errorlevel%
)
if /I "%~1"=="enable" (
  shift
  if "%~1"=="" goto help_plugins
  set "ASTRO_METHOD=POST"
  set "ASTRO_PATH=/v1/plugins/%~1/enable"
  set "ASTRO_BODY_KIND="
  call :api_request
  exit /b %errorlevel%
)
if /I "%~1"=="disable" (
  shift
  if "%~1"=="" goto help_plugins
  set "ASTRO_METHOD=POST"
  set "ASTRO_PATH=/v1/plugins/%~1/disable"
  set "ASTRO_BODY_KIND="
  call :api_request
  exit /b %errorlevel%
)
if /I "%~1"=="remove" (
  shift
  if "%~1"=="" goto help_plugins
  set "ASTRO_METHOD=DELETE"
  set "ASTRO_PATH=/v1/plugins/%~1"
  set "ASTRO_BODY_KIND="
  call :api_request
  exit /b %errorlevel%
)
goto help_plugins

:safe_mode
shift
if "%~1"=="" goto help_safe_mode
if /I "%~1"=="on" (
  set "ASTRO_ARG1=true"
) else (
  if /I "%~1"=="off" (
    set "ASTRO_ARG1=false"
  ) else (
    goto help_safe_mode
  )
)
set "ASTRO_METHOD=POST"
set "ASTRO_PATH=/v1/safe-mode"
set "ASTRO_BODY_KIND=safe-mode"
call :api_request
exit /b %errorlevel%

:chats
shift
if "%~1"=="" (
  set "ASTRO_ARG1=100"
) else (
  set "ASTRO_ARG1=%~1"
)
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/chats?limit=%ASTRO_ARG1%"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:messages
shift
if "%~1"=="" goto help_messages
set "ASTRO_ARG1=%~1"
shift
if "%~1"=="" (
  set "ASTRO_ARG2=50"
) else (
  set "ASTRO_ARG2=%~1"
)
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/chats/%ASTRO_ARG1%/messages?limit=%ASTRO_ARG2%"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:send
shift
if "%~1"=="" goto help_send
set "ASTRO_ARG1=%~1"
set "ASTRO_ARG2="
shift
:send_collect
if "%~1"=="" goto send_ready
if defined ASTRO_ARG2 (
  set "ASTRO_ARG2=%ASTRO_ARG2% %~1"
) else (
  set "ASTRO_ARG2=%~1"
)
shift
goto send_collect
:send_ready
if not defined ASTRO_ARG1 goto help_send
if not defined ASTRO_ARG2 goto help_send
set "ASTRO_METHOD=POST"
set "ASTRO_PATH=/v1/messages/send"
set "ASTRO_BODY_KIND=send"
call :api_request
exit /b %errorlevel%

:logs
shift
if "%~1"=="" (
  set "ASTRO_LOG_TARGET=plugins"
  set "ASTRO_LOG_LINES=200"
  goto logs_dispatch
)
if /I "%~1"=="paths" goto logs_paths
if /I "%~1"=="client" (
  shift
  set "ASTRO_LOG_TARGET=client"
  if "%~1"=="" (
    set "ASTRO_LOG_LINES=200"
  ) else (
    set "ASTRO_LOG_LINES=%~1"
  )
  goto logs_dispatch
)
if /I "%~1"=="plugins" (
  shift
  set "ASTRO_LOG_TARGET=plugins"
  if "%~1"=="" (
    set "ASTRO_LOG_LINES=200"
  ) else (
    set "ASTRO_LOG_LINES=%~1"
  )
  goto logs_dispatch
)
set "ASTRO_LOG_TARGET=plugins"
set "ASTRO_LOG_LINES=%~1"

goto logs_dispatch

:logs_dispatch
if /I "%ASTRO_LOG_TARGET%"=="client" goto logs_client
set "ASTRO_METHOD=GET"
set "ASTRO_PATH=/v1/logs?lines=%ASTRO_LOG_LINES%"
set "ASTRO_BODY_KIND="
call :api_request
exit /b %errorlevel%

:logs_client
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$path = $env:ASTRO_CLIENT_LOG;" ^
  "$lines = if ($env:ASTRO_LOG_LINES) { [int]$env:ASTRO_LOG_LINES } else { 200 };" ^
  "if (-not $path) { Write-Error 'ASTRO_CLIENT_LOG is not configured.'; exit 1 }" ^
  "if (-not (Test-Path $path)) { Write-Error ('client.log was not found: ' + $path); exit 1 }" ^
  "Get-Content -LiteralPath $path -Tail $lines"
exit /b %errorlevel%

:logs_paths
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "[pscustomobject]@{ commandDir = $env:ASTRO_COMMAND_DIR; workingDir = $env:ASTRO_WORKING_DIR; clientLog = $env:ASTRO_CLIENT_LOG; pluginsLog = $env:ASTRO_PLUGINS_LOG; pluginsConfig = $env:ASTRO_PLUGINS_CONFIG; baseUrl = $env:ASTRO_BASE_URL } | ConvertTo-Json -Depth 8"
exit /b %errorlevel%

:runtime_config_fallback
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$configPath = $env:ASTRO_PLUGINS_CONFIG;" ^
  "if (-not $configPath) { Write-Error 'ASTRO_PLUGINS_CONFIG is not configured.'; exit 1 }" ^
  "$dir = Split-Path -Parent $configPath; if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }" ^
  "$doc = $null;" ^
  "if (Test-Path $configPath) { try { $doc = Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json } catch {} }" ^
  "if ($null -eq $doc) { $doc = New-Object PSObject }" ^
  "$runtimeProperty = $doc.PSObject.Properties['runtimeApi'];" ^
  "if ($null -eq $runtimeProperty) { $runtimeObject = New-Object PSObject; $doc | Add-Member -NotePropertyName runtimeApi -NotePropertyValue $runtimeObject -Force } else { $runtimeObject = $runtimeProperty.Value; if ($null -eq $runtimeObject) { $runtimeObject = New-Object PSObject; $doc.runtimeApi = $runtimeObject } }" ^
  "$enabled = $env:ASTRO_RUNTIME_FALLBACK_ENABLED -eq '1';" ^
  "if ($null -eq $runtimeObject.PSObject.Properties['enabled']) { $runtimeObject | Add-Member -NotePropertyName enabled -NotePropertyValue $enabled -Force } else { $runtimeObject.enabled = $enabled }" ^
  "if ($enabled) { $port = if ($env:ASTRO_RUNTIME_FALLBACK_PORT) { [int]$env:ASTRO_RUNTIME_FALLBACK_PORT } else { [int]$env:ASTRO_RUNTIME_PORT }; if ($null -eq $runtimeObject.PSObject.Properties['port']) { $runtimeObject | Add-Member -NotePropertyName port -NotePropertyValue $port -Force } else { $runtimeObject.port = $port } }" ^
  "$encoding = New-Object System.Text.UTF8Encoding($false);" ^
  "$json = $doc | ConvertTo-Json -Depth 16;" ^
  "[System.IO.File]::WriteAllText($configPath, $json, $encoding);" ^
  "if ($enabled) { Write-Host ('Runtime API will be enabled on next Astrogram start. Port: ' + $runtimeObject.port) } else { Write-Host 'Runtime API will be disabled on next Astrogram start.' }"
exit /b %errorlevel%

:api_request
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$base = if ($env:ASTRO_BASE_URL) { $env:ASTRO_BASE_URL } else { 'http://127.0.0.1:37080' };" ^
  "$uri = $base + $env:ASTRO_PATH;" ^
  "$body = $null;" ^
  "switch ($env:ASTRO_BODY_KIND) { 'runtime-on' { $port = if ($env:ASTRO_ARG1) { [int]$env:ASTRO_ARG1 } else { [int]$env:ASTRO_RUNTIME_PORT }; $body = @{ enabled = $true; port = $port } } 'runtime-off' { $body = @{ enabled = $false } } 'safe-mode' { $body = @{ enabled = [System.Convert]::ToBoolean($env:ASTRO_ARG1) } } 'send' { $body = @{ peerId = $env:ASTRO_ARG1; text = $env:ASTRO_ARG2 } } default { } }" ^
  "try { if ($null -ne $body) { $json = $body | ConvertTo-Json -Compress -Depth 16; $response = Invoke-RestMethod -Method $env:ASTRO_METHOD -Uri $uri -ContentType 'application/json; charset=utf-8' -Body $json } else { $response = Invoke-RestMethod -Method $env:ASTRO_METHOD -Uri $uri }; $response | ConvertTo-Json -Depth 16 } catch { $payload = ''; if ($_.Exception.Response) { try { $reader = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd() } catch {} } if ($payload) { Write-Host $payload } else { Write-Host ('Astrogram Runtime API is unavailable at ' + $base + '.'); Write-Host 'Enable Runtime API in Astrogram settings or use astro runtime on for the next launch.'; Write-Host $_.Exception.Message } exit 1 }"
exit /b %errorlevel%

:help_api
echo Usage: astro api
echo        astro api ^<path^>
echo        astro api ^<METHOD^> ^<path^> [json-body]
exit /b 1

:help_runtime
echo Usage: astro runtime on [port]
echo        astro runtime off
exit /b 1

:help_plugins
echo Usage: astro plugins
echo        astro plugins info ^<id^>
echo        astro plugins reload
echo        astro plugins enable ^<id^>
echo        astro plugins disable ^<id^>
echo        astro plugins remove ^<id^>
exit /b 1

:help_safe_mode
echo Usage: astro safe-mode on
echo        astro safe-mode off
exit /b 1

:help_messages
echo Usage: astro messages ^<peerId^> [limit]
exit /b 1

:help_send
echo Usage: astro send ^<peerId^> ^<text...^>
exit /b 1

:help
echo Astrogram Runtime API CLI
echo.
echo   astro api
echo   astro api ^<path^>
echo   astro api ^<METHOD^> ^<path^> [json-body]
echo   astro status
echo   astro health
echo   astro host
echo   astro system
echo   astro diagnostics
echo   astro runtime on [port]
echo   astro runtime off
echo   astro plugins
echo   astro plugins info ^<id^>
echo   astro plugins reload
echo   astro plugins enable ^<id^>
echo   astro plugins disable ^<id^>
echo   astro plugins remove ^<id^>
echo   astro safe-mode on^|off
echo   astro chats [limit]
echo   astro messages ^<peerId^> [limit]
echo   astro send ^<peerId^> ^<text...^>
echo   astro logs [lines]
echo   astro logs client [lines]
echo   astro logs plugins [lines]
echo   astro logs paths
echo.
echo Runtime API commands work live when the client runtime server is enabled.
echo If it is currently offline, astro runtime on/off updates the saved config for the next Astrogram start.
exit /b 0

)ASTROBAT");
}

QString LoadRuntimeApiWindowsScript() {
	for (const auto &candidate : RuntimeApiTemplateCandidates()) {
		QFile file(candidate);
		if (file.open(QIODevice::ReadOnly)) {
			const auto script = QString::fromUtf8(file.readAll()).trimmed();
			if (!script.isEmpty()) {
				return NormalizeWindowsBatchLineEndings(script + u'\n');
			}
		}
	}
	return NormalizeWindowsBatchLineEndings(
		RuntimeApiFallbackScript().trimmed() + u'\n');
}

RuntimeApiCommandState ReadRuntimeApiCommandState() {
	auto result = RuntimeApiCommandState();
	QFile file(cWorkingDir() + u"tdata/plugins.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return result;
	}
	const auto runtimeValue = document.object().value(u"runtimeApi"_q);
	if (!runtimeValue.isObject()) {
		return result;
	}
	const auto runtime = runtimeValue.toObject();
	result.enabled = runtime.value(u"enabled"_q).toBool(false);
	const auto loadedPort = runtime.value(u"port"_q).toInt(result.port);
	if (loadedPort > 0 && loadedPort <= 65535) {
		result.port = loadedPort;
	}
	return result;
}

QString RuntimeApiEnvContents(const RuntimeApiCommandState &state) {
	auto workingDir = QDir::toNativeSeparators(QDir::cleanPath(cWorkingDir()));
	if (!workingDir.endsWith(QDir::separator()) && !workingDir.endsWith(u'/')) {
		workingDir += QDir::separator();
	}
	const auto clientLog = QDir::toNativeSeparators(
		QDir(cWorkingDir()).absoluteFilePath(u"client.log"_q));
	const auto pluginsLog = QDir::toNativeSeparators(
		QDir(cWorkingDir()).absoluteFilePath(u"tdata/plugins.log"_q));
	const auto pluginsConfig = QDir::toNativeSeparators(
		QDir(cWorkingDir()).absoluteFilePath(u"tdata/plugins.json"_q));
	const auto baseUrl = u"http://127.0.0.1:%1"_q.arg(state.port);
	const auto commandDir = QDir::toNativeSeparators(
		QDir(RuntimeApiUserBinDir()).absolutePath());
	return QString::fromLatin1("@echo off\r\n")
		+ u"set \"ASTRO_WORKING_DIR=%1\"\r\n"_q.arg(
			EscapeBatchValue(workingDir))
		+ u"set \"ASTRO_COMMAND_DIR=%1\"\r\n"_q.arg(
			EscapeBatchValue(commandDir))
		+ u"set \"ASTRO_RUNTIME_HOST=127.0.0.1\"\r\n"_q
		+ u"set \"ASTRO_RUNTIME_PORT=%1\"\r\n"_q.arg(state.port)
		+ u"set \"ASTRO_RUNTIME_ENABLED=%1\"\r\n"_q.arg(
			state.enabled ? u"1"_q : u"0"_q)
		+ u"set \"ASTRO_BASE_URL=%1\"\r\n"_q.arg(
			EscapeBatchValue(baseUrl))
		+ u"set \"ASTRO_CLIENT_LOG=%1\"\r\n"_q.arg(
			EscapeBatchValue(clientLog))
		+ u"set \"ASTRO_PLUGINS_LOG=%1\"\r\n"_q.arg(
			EscapeBatchValue(pluginsLog))
		+ u"set \"ASTRO_PLUGINS_CONFIG=%1\"\r\n"_q.arg(
			EscapeBatchValue(pluginsConfig));
}
bool WriteTextIfChanged(const QString &path, const QString &contents) {
	const auto data = contents.toUtf8();
	QFile existing(path);
	if (existing.open(QIODevice::ReadOnly)
		&& existing.readAll() == data) {
		return true;
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (file.write(data) != data.size()) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

void WriteLauncherClientLog(
		const QString &scope,
		const QString &message) {
	if (!Logs::started()) {
		return;
	}
	Logs::writeClient(u"[%1] %2"_q.arg(scope, message));
}

bool PathListContains(const QString &pathValue, const QString &entry) {
	const auto normalizedEntry = NormalizePathKey(entry);
	for (const auto &part : pathValue.split(';', Qt::SkipEmptyParts)) {
		if (NormalizePathKey(part) == normalizedEntry) {
			return true;
		}
	}
	return false;
}

void BroadcastEnvironmentChanged() {
	DWORD_PTR result = 0;
	SendMessageTimeoutW(
		HWND_BROADCAST,
		WM_SETTINGCHANGE,
		0,
		reinterpret_cast<LPARAM>(L"Environment"),
		SMTO_ABORTIFHUNG,
		5000,
		&result);
}

RuntimeApiPathSyncResult EnsureRuntimeApiPath(const QString &dir) {
	auto result = RuntimeApiPathSyncResult();
	if (dir.isEmpty()) {
		return result;
	}
	const auto nativeDir = QDir::toNativeSeparators(dir);
	const auto currentPath = qEnvironmentVariable("PATH");
	if (!PathListContains(currentPath, nativeDir)) {
		auto updated = currentPath;
		if (!updated.isEmpty() && !updated.endsWith(';')) {
			updated += ';';
		}
		updated += nativeDir;
		qputenv("PATH", updated.toUtf8());
		result.sessionPathUpdated = true;
	}
	QSettings settings(u"HKEY_CURRENT_USER\\Environment"_q, QSettings::NativeFormat);
	const auto userPath = settings.value(u"Path"_q).toString();
	if (!PathListContains(userPath, nativeDir)) {
		auto updated = userPath;
		if (!updated.isEmpty() && !updated.endsWith(';')) {
			updated += ';';
		}
		updated += nativeDir;
		settings.setValue(u"Path"_q, updated);
		settings.sync();
		result.userPathUpdated = true;
		BroadcastEnvironmentChanged();
	}
	result.userPathContainsCommand = PathListContains(
		settings.value(u"Path"_q).toString(),
		nativeDir);
	return result;
}

QString CanonicalWindowsUpdaterPathInDir(const QString &dir) {
	return QDir(dir).absoluteFilePath(u"Updater.exe"_q);
}

QString FindWindowsUpdaterAliasInDir(const QString &dir) {
	const auto candidates = {
		QDir(dir).absoluteFilePath(u"AstrogramUpdater.exe"_q),
		QDir(dir).absoluteFilePath(u"astrogram_updater.exe"_q),
	};
	for (const auto &candidate : candidates) {
		if (QFileInfo::exists(candidate)) {
			return candidate;
		}
	}
	return QString();
}

void EnsureCanonicalWindowsUpdaterBinary() {
	const auto canonical = CanonicalWindowsUpdaterPathInDir(cExeDir());
	const auto alias = FindWindowsUpdaterAliasInDir(cExeDir());
	if (alias.isEmpty()) {
		return;
	}
	const auto canonicalInfo = QFileInfo(canonical);
	const auto aliasInfo = QFileInfo(alias);
	const auto needsRestore = !canonicalInfo.exists()
		|| (canonicalInfo.size() != aliasInfo.size())
		|| (canonicalInfo.lastModified() < aliasInfo.lastModified());
	if (!needsRestore) {
		return;
	}
	QFile::remove(canonical);
	if (QFile::copy(alias, canonical)) {
		LOG(("Update Info: restored canonical Updater.exe from '%1'").arg(alias));
		WriteLauncherClientLog(
			u"updater"_q,
			u"restored canonical Updater.exe from '%1'"_q.arg(alias));
	} else {
		LOG(("Update Error: could not restore canonical Updater.exe from '%1'").arg(alias));
		WriteLauncherClientLog(
			u"updater"_q,
			u"failed to restore canonical Updater.exe from '%1'"_q.arg(alias));
	}
}

void EnsureRuntimeApiWindowsCommand() {
	const auto state = ReadRuntimeApiCommandState();
	const auto script = LoadRuntimeApiWindowsScript();
	const auto env = RuntimeApiEnvContents(state);
	auto writtenDirs = QStringList();
	auto failedPaths = QStringList();
	for (const auto &dir : RuntimeApiInstallDirs()) {
		if (!QDir().mkpath(dir)) {
			LOG(("Runtime API: could not create command dir '%1'").arg(dir));
			failedPaths.push_back(dir + u" (mkdir)"_q);
			continue;
		}
		const auto commandPath = QDir(dir).absoluteFilePath(u"astro.bat"_q);
		const auto commandAliasPath = QDir(dir).absoluteFilePath(u"astro.cmd"_q);
		const auto envPath = QDir(dir).absoluteFilePath(u"astro-vars.bat"_q);
		const auto legacyEnvPath = QDir(dir).absoluteFilePath(u"astro.env"_q);
		if (!WriteTextIfChanged(commandPath, script)) {
			LOG(("Runtime API: could not write '%1'").arg(commandPath));
			failedPaths.push_back(commandPath);
		}
		if (!WriteTextIfChanged(commandAliasPath, script)) {
			LOG(("Runtime API: could not write '%1'").arg(commandAliasPath));
			failedPaths.push_back(commandAliasPath);
		}
		if (!WriteTextIfChanged(envPath, env)) {
			LOG(("Runtime API: could not write '%1'").arg(envPath));
			failedPaths.push_back(envPath);
		} else {
			QFile::remove(legacyEnvPath);
			writtenDirs.push_back(dir);
		}
	}
	const auto pathResult = EnsureRuntimeApiPath(RuntimeApiUserBinDir());
	WriteLauncherClientLog(
		u"runtime-cli"_q,
		u"astro command prepared in %1 dir(s), path=session:%2 user:%3 contains:%4, runtimeEnabled:%5 port:%6"_q
			.arg(writtenDirs.size())
			.arg(pathResult.sessionPathUpdated ? u"1"_q : u"0"_q)
			.arg(pathResult.userPathUpdated ? u"1"_q : u"0"_q)
			.arg(pathResult.userPathContainsCommand ? u"1"_q : u"0"_q)
			.arg(state.enabled ? u"1"_q : u"0"_q)
			.arg(state.port));
	if (!failedPaths.isEmpty()) {
		WriteLauncherClientLog(
			u"runtime-cli"_q,
			u"astro command write issues: %1"_q.arg(
				failedPaths.join(u", "_q)));
	}
}
#endif // Q_OS_WIN

bool MoveLegacyAlphaFolder(const QString &folder, const QString &file) {
	const auto was = cExeDir() + folder;
	const auto now = cExeDir() + u"TelegramForcePortable"_q;
	if (QDir(was).exists() && !QDir(now).exists()) {
		const auto oldFile = was + "/tdata/" + file;
		const auto newFile = was + "/tdata/alpha";
		if (QFile::exists(oldFile) && !QFile::exists(newFile)) {
			if (!QFile(oldFile).copy(newFile)) {
				LOG(("FATAL: Could not copy '%1' to '%2'").arg(
					oldFile,
					newFile));
				return false;
			}
		}
		if (!QDir().rename(was, now)) {
			LOG(("FATAL: Could not rename '%1' to '%2'").arg(was, now));
			return false;
		}
	}
	return true;
}

bool MoveLegacyAlphaFolder() {
	if (!MoveLegacyAlphaFolder(u"TelegramAlpha_data"_q, u"alpha"_q)
		|| !MoveLegacyAlphaFolder(u"TelegramBeta_data"_q, u"beta"_q)) {
		return false;
	}
	return true;
}

bool CheckPortableVersionFolder() {
	if (!MoveLegacyAlphaFolder()) {
		return false;
	}

	const auto portable = cExeDir() + u"TelegramForcePortable"_q;
	QFile key(portable + u"/tdata/alpha"_q);
	if (cAlphaVersion()) {
		Assert(*AlphaPrivateKey != 0);

		cForceWorkingDir(portable);
		QDir().mkpath(cWorkingDir() + u"tdata"_q);
		cSetAlphaPrivateKey(QByteArray(AlphaPrivateKey));
		if (!key.open(QIODevice::WriteOnly)) {
			LOG(("FATAL: Could not open '%1' for writing private key!"
				).arg(key.fileName()));
			return false;
		}
		QDataStream dataStream(&key);
		dataStream.setVersion(QDataStream::Qt_5_3);
		dataStream << quint64(cRealAlphaVersion()) << cAlphaPrivateKey();
		return true;
	}
	if (!QDir(portable).exists()) {
		return true;
	}
	cForceWorkingDir(portable);
	if (!key.exists()) {
		return true;
	}

	if (!key.open(QIODevice::ReadOnly)) {
		LOG(("FATAL: could not open '%1' for reading private key. "
			"Delete it or reinstall private alpha version."
			).arg(key.fileName()));
		return false;
	}
	QDataStream dataStream(&key);
	dataStream.setVersion(QDataStream::Qt_5_3);

	quint64 v;
	QByteArray k;
	dataStream >> v >> k;
	if (dataStream.status() != QDataStream::Ok || k.isEmpty()) {
		LOG(("FATAL: '%1' is corrupted. "
			"Delete it or reinstall private alpha version."
			).arg(key.fileName()));
		return false;
	}
	cSetAlphaVersion(AppVersion * 1000ULL);
	cSetAlphaPrivateKey(k);
	cSetRealAlphaVersion(v);
	return true;
}

base::options::toggle OptionFractionalScalingEnabled({
	.id = kOptionFractionalScalingEnabled,
	.name = "Enable precise High DPI scaling",
	.description = "Follow system interface scale settings exactly.",
	.scope = base::options::windows | base::options::linux,
	.restartRequired = true,
});

} // namespace

const char kOptionFractionalScalingEnabled[] = "fractional-scaling-enabled";
const char kOptionHighDpiDownscale[] = "high-dpi-downscale";
const char kOptionFreeType[] = "freetype";

Launcher *Launcher::InstanceSetter::Instance = nullptr;

std::unique_ptr<Launcher> Launcher::Create(int argc, char *argv[]) {
	return std::make_unique<Platform::Launcher>(argc, argv);
}

Launcher::Launcher(int argc, char *argv[])
: _argc(argc)
, _argv(argv)
, _arguments(readArguments(_argc, _argv))
, _baseIntegration(_argc, _argv)
, _initialWorkingDir(QDir::currentPath() + '/') {
	crl::toggle_fp_exceptions(true);

	base::Integration::Set(&_baseIntegration);
}

Launcher::~Launcher() {
	InstanceSetter::Instance = nullptr;
}

void Launcher::init() {
	prepareSettings();
	initQtMessageLogging();

	QApplication::setApplicationName(u"Astrogram"_q);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	// fallback session management is useless for tdesktop since it doesn't have
	// any "are you sure you want to close this window?" dialogs
	// but it produces bugs like https://github.com/telegramdesktop/tdesktop/issues/5022
	// and https://github.com/telegramdesktop/tdesktop/issues/7549
	// and https://github.com/telegramdesktop/tdesktop/issues/948
	// more info: https://doc.qt.io/qt-5/qguiapplication.html#isFallbackSessionManagementEnabled
	QApplication::setFallbackSessionManagementEnabled(false);
#endif // Qt < 6.0.0

	initHook();
}

void Launcher::initHighDpi() {
#if QT_VERSION < QT_VERSION_CHECK(6, 2, 0)
	qputenv("QT_DPI_ADJUSTMENT_POLICY", "AdjustDpi");
#endif // Qt < 6.2.0

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
#endif // Qt < 6.0.0

	if (OptionHighDpiDownscale.value()) {
		qputenv("QT_WIDGETS_HIGHDPI_DOWNSCALE", "1");
		qputenv("QT_WIDGETS_RHI", "1");
		qputenv("QT_WIDGETS_RHI_BACKEND", "opengl");
	}

	if (OptionFractionalScalingEnabled.value()
			|| OptionHighDpiDownscale.value()) {
		QApplication::setHighDpiScaleFactorRoundingPolicy(
			Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
	} else {
		QApplication::setHighDpiScaleFactorRoundingPolicy(
			Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
	}
}

int Launcher::exec() {
	init();

	if (cLaunchMode() == LaunchModeFixPrevious) {
		return psFixPrevious();
	} else if (cLaunchMode() == LaunchModeCleanup) {
		return psCleanup();
	}

	// Must be started before Platform is started.
	Logs::start();
	base::options::init(cWorkingDir() + "tdata/experimental_options.json");

	// Must be called after options are inited.
	initHighDpi();

	if (Logs::DebugEnabled()) {
		const auto openalLogPath = QDir::toNativeSeparators(
			cWorkingDir() + u"DebugLogs/last_openal_log.txt"_q);

		qputenv("ALSOFT_LOGLEVEL", "3");

#ifdef Q_OS_WIN
		_wputenv_s(
			L"ALSOFT_LOGFILE",
			openalLogPath.toStdWString().c_str());
#else // Q_OS_WIN
		qputenv(
			"ALSOFT_LOGFILE",
			QFile::encodeName(openalLogPath));
#endif // !Q_OS_WIN
	}

	// Must be started before Sandbox is created.
	Platform::start();
	ThirdParty::start();
	auto result = executeApplication();

	DEBUG_LOG(("Astrogram finished, result: %1").arg(result));

	if (!UpdaterDisabled() && cRestartingUpdate()) {
		DEBUG_LOG(("Sandbox Info: executing updater to install update."));
		if (!launchUpdater(UpdaterLaunch::PerformUpdate)) {
			base::Platform::DeleteDirectory(cWorkingDir() + u"tupdates/temp"_q);
		}
	} else if (cRestarting()) {
		DEBUG_LOG(("Sandbox Info: executing Astrogram because of restart."));
		launchUpdater(UpdaterLaunch::JustRelaunch);
	}

	CrashReports::Finish();
	ThirdParty::finish();
	Platform::finish();
	Logs::finish();

	return result;
}

bool Launcher::validateCustomWorkingDir() {
	if (customWorkingDir()) {
		if (_customWorkingDir == cWorkingDir()) {
			_customWorkingDir = QString();
			return false;
		}
		cForceWorkingDir(_customWorkingDir);
		return true;
	}
	return false;
}

void Launcher::workingFolderReady() {
	srand((unsigned int)time(nullptr));

	ComputeDebugMode();
	ComputeExternalUpdater();
	ComputeInstallBetaVersions();
	ComputeInstallationTag();
#ifdef Q_OS_WIN
	EnsureCanonicalWindowsUpdaterBinary();
	EnsureRuntimeApiWindowsCommand();
#endif // Q_OS_WIN
}

void Launcher::writeDebugModeSetting() {
	WriteDebugModeSetting();
}

void Launcher::writeInstallBetaVersionsSetting() {
	WriteInstallBetaVersionsSetting();
}

bool Launcher::checkPortableVersionFolder() {
	return CheckPortableVersionFolder();
}

QStringList Launcher::readArguments(int argc, char *argv[]) const {
	Expects(argc >= 0);

	if (const auto native = readArgumentsHook(argc, argv)) {
		return *native;
	}

	auto result = QStringList();
	result.reserve(argc);
	for (auto i = 0; i != argc; ++i) {
		result.push_back(base::FromUtf8Safe(argv[i]));
	}
	return result;
}

const QStringList &Launcher::arguments() const {
	return _arguments;
}

QString Launcher::initialWorkingDir() const {
	return _initialWorkingDir;
}

bool Launcher::customWorkingDir() const {
	return !_customWorkingDir.isEmpty();
}

void Launcher::prepareSettings() {
	auto path = base::Platform::CurrentExecutablePath(_argc, _argv);
	LOG(("Executable path before check: %1").arg(path));
	if (cExeName().isEmpty()) {
		LOG(("WARNING: Could not compute executable path, some features will be disabled."));
	}

	processArguments();
}

void Launcher::initQtMessageLogging() {
	static QtMessageHandler OriginalMessageHandler = nullptr;
	OriginalMessageHandler = qInstallMessageHandler([](
			QtMsgType type,
			const QMessageLogContext &context,
			const QString &msg) {
		if (OriginalMessageHandler) {
			OriginalMessageHandler(type, context, msg);
		}
		if (Logs::DebugEnabled() || !Logs::started()) {
			if (!Logs::WritingEntry()) {
				// Sometimes Qt logs something inside our own logging.
				LOG((msg));
			}
		}
	});
}

uint64 Launcher::installationTag() const {
	return InstallationTag;
}

QByteArray Launcher::instanceHash() const {
	static const auto Result = [&] {
		QByteArray h(32, 0);
		if (customWorkingDir()) {
			const auto d = QFile::encodeName(
				QDir(cWorkingDir()).absolutePath());
			hashMd5Hex(d.constData(), d.size(), h.data());
		} else {
			const auto f = QFile::encodeName(cExeDir() + cExeName());
			hashMd5Hex(f.constData(), f.size(), h.data());
		}
		return h;
	}();
	return Result;
}

void Launcher::processArguments() {
	enum class KeyFormat {
		NoValues,
		OneValue,
		AllLeftValues,
	};
	auto parseMap = std::map<QByteArray, KeyFormat> {
		{ "-debug"          , KeyFormat::NoValues },
		{ "-key"            , KeyFormat::OneValue },
		{ "-autostart"      , KeyFormat::NoValues },
		{ "-fixprevious"    , KeyFormat::NoValues },
		{ "-cleanup"        , KeyFormat::NoValues },
		{ "-noupdate"       , KeyFormat::NoValues },
		{ "-tosettings"     , KeyFormat::NoValues },
		{ "-startintray"    , KeyFormat::NoValues },
		{ "-quit"           , KeyFormat::NoValues },
		{ "-workdir"        , KeyFormat::OneValue },
		{ "--"              , KeyFormat::AllLeftValues },
		{ "-scale"          , KeyFormat::OneValue },
	};
	auto parseResult = QMap<QByteArray, QStringList>();
	auto parsingKey = QByteArray();
	auto parsingFormat = KeyFormat::NoValues;
	for (auto i = _arguments.cbegin(); i != _arguments.cend(); ++i) {
		if (i == _arguments.cbegin()) {
			continue;
		}
		const auto &argument = *i;
		switch (parsingFormat) {
		case KeyFormat::OneValue: {
			parseResult[parsingKey] = QStringList(argument.mid(0, 8192));
			parsingFormat = KeyFormat::NoValues;
		} break;
		case KeyFormat::AllLeftValues: {
			parseResult[parsingKey].push_back(argument.mid(0, 8192));
		} break;
		case KeyFormat::NoValues: {
			parsingKey = argument.toLatin1();
			auto it = parseMap.find(parsingKey);
			if (it != parseMap.end()) {
				parsingFormat = it->second;
				parseResult[parsingKey] = QStringList();
				continue;
			}
			parseResult["--"].push_back(argument.mid(0, 8192));
		} break;
		}
	}

	static const auto RegExp = QRegularExpression("[^a-z0-9\\-_]");
	gDebugMode = parseResult.contains("-debug");
	gKeyFile = parseResult
		.value("-key", {})
		.join(QString())
		.toLower()
		.replace(RegExp, {});
	gLaunchMode = parseResult.contains("-autostart") ? LaunchModeAutoStart
		: parseResult.contains("-fixprevious") ? LaunchModeFixPrevious
		: parseResult.contains("-cleanup") ? LaunchModeCleanup
		: LaunchModeNormal;
	gNoStartUpdate = parseResult.contains("-noupdate");
	gStartToSettings = parseResult.contains("-tosettings");
	gStartInTray = parseResult.contains("-startintray");
	gQuit = parseResult.contains("-quit");
	_customWorkingDir = parseResult.value("-workdir", {}).join(QString());
	if (!_customWorkingDir.isEmpty()) {
		_customWorkingDir = QDir(_customWorkingDir).absolutePath() + '/';
	}

	const auto startUrls = parseResult.value("--", {});
	gStartUrls = startUrls | ranges::views::transform([&](const QString &url) {
		return QUrl::fromUserInput(url, _initialWorkingDir);
	}) | ranges::views::filter(&QUrl::isValid) | ranges::to<QList<QUrl>>;

	const auto scaleKey = parseResult.value("-scale", {});
	if (scaleKey.size() > 0) {
		using namespace style;
		const auto value = scaleKey[0].toInt();
		gConfigScale = ((value < kScaleMin) || (value > kScaleMax))
			? kScaleAuto
			: value;
	}
}

int Launcher::executeApplication() {
	FilteredCommandLineArguments arguments(_argc, _argv);
	Sandbox sandbox(arguments.count(), arguments.values());
	Ui::MainQueueProcessor processor;
	base::ConcurrentTimerEnvironment environment;
	return sandbox.start();
}

} // namespace Core
