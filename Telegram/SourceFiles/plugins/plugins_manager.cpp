/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_manager.h"

#include "boxes/abstract_box.h"
#include "api/api_common.h"
#include "core/application.h"
#include "core/launcher.h"
#include "data/data_changes.h"
#include "data/data_history_messages.h"
#include "history/history_item.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "logs.h"
#include "storage/storage_sparse_ids_list.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QDateTime>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QLibrary>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QAbstractSocket>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif // _WIN32

namespace Plugins {
namespace {

constexpr auto kPluginsFolder = "tdata/plugins";
constexpr auto kConfigFile = "tdata/plugins.json";
constexpr auto kLogFile = "tdata/plugins.log";
constexpr auto kTraceFile = "tdata/plugins.trace.jsonl";
constexpr auto kSafeModeFile = "tdata/plugins.safe-mode";
constexpr auto kRecoveryFile = "tdata/plugins.recovery.json";
constexpr auto kBinaryInfoName = "TgdPluginBinaryInfo";
constexpr auto kPreviewInfoName = "TgdPluginPreviewInfo";
constexpr auto kEntryName = "TgdPluginEntry";
constexpr auto kNoPluginsArgument = "-noplugins";
constexpr auto kRecoveryDuckSize = 72;
constexpr auto kMaxPluginLogBytes = qsizetype(25 * 1024 * 1024);
constexpr auto kMaxPluginLogBackups = 5;

#if defined(_WIN32)
constexpr auto kRuntimeCliFolder = "tdata/bin";
constexpr auto kRuntimeCliHelperName = "astro.bat";

[[nodiscard]] QString NormalizePathForComparison(QString path) {
	path = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
	while (path.endsWith(u'/')) {
		path.chop(1);
	}
	return path.toLower();
}

[[nodiscard]] QString RuntimeCliDirectoryPath() {
	return QDir(cWorkingDir()).filePath(QString::fromLatin1(kRuntimeCliFolder));
}

[[nodiscard]] QString RuntimeCliHelperPath() {
	return QDir(RuntimeCliDirectoryPath()).filePath(
		QString::fromLatin1(kRuntimeCliHelperName));
}

[[nodiscard]] QString ReadUserEnvironmentPath() {
	HKEY key = nullptr;
	const auto status = RegOpenKeyExW(
		HKEY_CURRENT_USER,
		L"Environment",
		0,
		KEY_QUERY_VALUE,
		&key);
	if (status != ERROR_SUCCESS || !key) {
		return QString();
	}
	DWORD type = 0;
	DWORD size = 0;
	auto result = QString();
	const auto query = RegQueryValueExW(
		key,
		L"Path",
		nullptr,
		&type,
		nullptr,
		&size);
	if ((query == ERROR_SUCCESS)
		&& (type == REG_SZ || type == REG_EXPAND_SZ)
		&& (size >= sizeof(wchar_t))) {
		auto buffer = std::wstring(size / sizeof(wchar_t), L'\0');
		if (RegQueryValueExW(
				key,
				L"Path",
				nullptr,
				&type,
				reinterpret_cast<LPBYTE>(buffer.data()),
				&size) == ERROR_SUCCESS) {
			while (!buffer.empty() && buffer.back() == L'\0') {
				buffer.pop_back();
			}
			result = QString::fromStdWString(buffer);
		}
	}
	RegCloseKey(key);
	return result;
}

bool WriteUserEnvironmentPath(const QString &value) {
	HKEY key = nullptr;
	const auto status = RegCreateKeyExW(
		HKEY_CURRENT_USER,
		L"Environment",
		0,
		nullptr,
		0,
		KEY_SET_VALUE,
		nullptr,
		&key,
		nullptr);
	if (status != ERROR_SUCCESS || !key) {
		return false;
	}
	const auto wide = value.toStdWString();
	const auto ok = (RegSetValueExW(
		key,
		L"Path",
		0,
		REG_EXPAND_SZ,
		reinterpret_cast<const BYTE*>(wide.c_str()),
		DWORD((wide.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
	RegCloseKey(key);
	return ok;
}

[[nodiscard]] bool PathContainsDirectory(
		const QString &pathValue,
		const QString &directory) {
	const auto wanted = NormalizePathForComparison(directory);
	for (const auto &entry : pathValue.split(u';', Qt::SkipEmptyParts)) {
		if (NormalizePathForComparison(entry) == wanted) {
			return true;
		}
	}
	return false;
}

bool EnsureUserPathContainsDirectory(const QString &directory) {
	const auto absolute = QDir(directory).absolutePath();
	const auto native = QDir::toNativeSeparators(absolute);
	auto registryPath = ReadUserEnvironmentPath();
	auto changed = false;
	if (!PathContainsDirectory(registryPath, absolute)) {
		if (!registryPath.trimmed().isEmpty() && !registryPath.endsWith(u';')) {
			registryPath += u';';
		}
		registryPath += native;
		if (!WriteUserEnvironmentPath(registryPath)) {
			return false;
		}
		changed = true;
	}

	auto processPath = qEnvironmentVariable("PATH");
	if (!PathContainsDirectory(processPath, absolute)) {
		if (!processPath.trimmed().isEmpty() && !processPath.endsWith(u';')) {
			processPath += u';';
		}
		processPath += native;
		qputenv("PATH", processPath.toUtf8());
	}

	if (changed) {
		SendMessageTimeoutW(
			HWND_BROADCAST,
			WM_SETTINGCHANGE,
			0,
			reinterpret_cast<LPARAM>(L"Environment"),
			SMTO_ABORTIFHUNG,
			2000,
			nullptr);
	}
	return true;
}

[[nodiscard]] QString RuntimeApiCliScript() {
	auto script = QString::fromLatin1(R"BATCH(@echo off
setlocal EnableExtensions

set "TDATA=%~dp0.."
for %%I in ("%TDATA%") do set "TDATA=%%~fI"
set "CFG=%TDATA%\plugins.json"
set "DEFAULT_PORT=37080"
set "ENABLED=False"
set "PORT=%DEFAULT_PORT%"
set "BASE=http://127.0.0.1:%DEFAULT_PORT%"

if "%~1"=="" goto :help
set "CMD=%~1"
shift

if /I "%CMD%"=="help" goto :help
if /I "%CMD%"=="status" goto :status
if /I "%CMD%"=="enable" goto :enable
if /I "%CMD%"=="disable" goto :disable
if /I "%CMD%"=="health" goto :health
if /I "%CMD%"=="host" goto :host
if /I "%CMD%"=="system" goto :system
if /I "%CMD%"=="plugins" goto :plugins
if /I "%CMD%"=="actions" goto :actions
if /I "%CMD%"=="panels" goto :panels
if /I "%CMD%"=="settings-pages" goto :settings_pages
if /I "%CMD%"=="action" goto :action
if /I "%CMD%"=="panel" goto :panel
if /I "%CMD%"=="reload" goto :reload
if /I "%CMD%"=="logs" goto :logs
if /I "%CMD%"=="chats" goto :chats
if /I "%CMD%"=="messages" goto :messages
if /I "%CMD%"=="send" goto :send
if /I "%CMD%"=="plugin-enable" goto :plugin_enable
if /I "%CMD%"=="plugin-disable" goto :plugin_disable
if /I "%CMD%"=="plugin-remove" goto :plugin_remove
if /I "%CMD%"=="plugin-actions" goto :plugin_actions
if /I "%CMD%"=="plugin-panels" goto :plugin_panels
if /I "%CMD%"=="plugin-settings" goto :plugin_settings
if /I "%CMD%"=="setting-bool" goto :setting_bool
if /I "%CMD%"=="setting-int" goto :setting_int
if /I "%CMD%"=="setting-text" goto :setting_text
if /I "%CMD%"=="safe-mode" goto :safe_mode
if /I "%CMD%"=="runtime" goto :runtime
if /I "%CMD%"=="raw" goto :raw

echo Unknown command: %CMD%
echo Run: astro help
exit /b 2

:help
echo Astrogram Runtime API CLI
echo.
echo Usage:
echo   astro status
echo   astro enable
echo   astro disable
echo   astro health
echo   astro host
echo   astro system
echo   astro plugins
echo   astro actions
echo   astro panels
echo   astro settings-pages
echo   astro action ^<id^>
echo   astro panel ^<id^>
echo   astro reload
echo   astro logs [plugins^|client^|trace] [lines]
echo   astro chats [limit]
echo   astro messages ^<peerId^> [limit]
echo   astro send ^<peerId^> ^<text...^>
echo   astro plugin-enable ^<pluginId^>
echo   astro plugin-disable ^<pluginId^>
echo   astro plugin-remove ^<pluginId^>
echo   astro plugin-actions ^<pluginId^>
echo   astro plugin-panels ^<pluginId^>
echo   astro plugin-settings ^<pluginId^>
echo   astro setting-bool ^<pageId^> ^<settingId^> on^|off
echo   astro setting-int ^<pageId^> ^<settingId^> ^<value^>
echo   astro setting-text ^<pageId^> ^<settingId^> ^<text...^>
echo   astro safe-mode on^|off
echo   astro runtime on^|off [port]
echo   astro raw ^<GET^|POST^|DELETE^> ^</v1/...^> [jsonBody]
exit /b 0

:status
call :detect_config || exit /b 1
echo config=%CFG%
echo runtimeApi.enabled=%ENABLED%
echo runtimeApi.port=%PORT%
echo runtimeApi.baseUrl=%BASE%
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$uri = $env:BASE + '/v1/health';" ^
  "try { $result = Invoke-RestMethod -Method Get -Uri $uri -TimeoutSec 3; Write-Host 'runtimeApi.live=true'; $result | ConvertTo-Json -Depth 20 } catch { Write-Host 'runtimeApi.live=false' }"
exit /b 0

:enable
call :set_config true || exit /b 1
call :detect_config >nul 2>nul
call :invoke POST "/v1/runtime" "{\"enabled\":true,\"port\":%PORT%}" >nul 2>nul
echo Runtime API enabled in plugins.json.
echo If Astrogram is already running with Runtime API disabled, reopen Settings ^> Runtime API or restart the client once.
exit /b 0

:disable
call :detect_config >nul 2>nul
call :invoke POST "/v1/runtime" "{\"enabled\":false}" >nul 2>nul
call :set_config false || exit /b 1
echo Runtime API disabled.
exit /b 0

:health
call :ensure_online || exit /b 1
call :invoke GET "/v1/health" "" || exit /b 1
exit /b 0

:host
call :ensure_online || exit /b 1
call :invoke GET "/v1/host" "" || exit /b 1
exit /b 0

:system
call :ensure_online || exit /b 1
call :invoke GET "/v1/system" "" || exit /b 1
exit /b 0

:plugins
call :ensure_online || exit /b 1
call :invoke GET "/v1/plugins" "" || exit /b 1
exit /b 0

:actions
call :ensure_online || exit /b 1
call :invoke GET "/v1/actions" "" || exit /b 1
exit /b 0

:panels
call :ensure_online || exit /b 1
call :invoke GET "/v1/panels" "" || exit /b 1
exit /b 0

:settings_pages
call :ensure_online || exit /b 1
call :invoke GET "/v1/settings-pages" "" || exit /b 1
exit /b 0

:action
if "%~1"=="" (
  echo Usage: astro action ^<id^>
  exit /b 2
)
call :ensure_online || exit /b 1
call :invoke POST "/v1/actions/%~1/trigger" "{}" || exit /b 1
exit /b 0

:panel
if "%~1"=="" (
  echo Usage: astro panel ^<id^>
  exit /b 2
)
call :ensure_online || exit /b 1
call :invoke POST "/v1/panels/%~1/open" "{}" || exit /b 1
exit /b 0

:reload
call :ensure_online || exit /b 1
call :invoke POST "/v1/plugins/reload" "{}" || exit /b 1
exit /b 0

:logs
set "SOURCE=%~1"
set "LINES=%~2"
if "%SOURCE%"=="" set "SOURCE=plugins"
if /I "%SOURCE%"=="plugins" goto :logs_ready
if /I "%SOURCE%"=="client" goto :logs_ready
if /I "%SOURCE%"=="trace" goto :logs_ready
if "%LINES%"=="" (
  set "LINES=%SOURCE%"
  set "SOURCE=plugins"
)
:logs_ready
if "%LINES%"=="" set "LINES=200"
call :ensure_online || exit /b 1
call :invoke GET "/v1/logs?source=%SOURCE%&lines=%LINES%" "" || exit /b 1
exit /b 0

:chats
set "LIMIT=%~1"
if "%LIMIT%"=="" set "LIMIT=100"
call :ensure_online || exit /b 1
call :invoke GET "/v1/chats?limit=%LIMIT%" "" || exit /b 1
exit /b 0

:messages
if "%~1"=="" (
  echo Usage: astro messages ^<peerId^> [limit]
  exit /b 2
)
set "PEER=%~1"
set "LIMIT=%~2"
if "%LIMIT%"=="" set "LIMIT=50"
call :ensure_online || exit /b 1
call :invoke GET "/v1/chats/%PEER%/messages?limit=%LIMIT%" "" || exit /b 1
exit /b 0

:send
if "%~1"=="" (
  echo Usage: astro send ^<peerId^> ^<text...^>
  exit /b 2
)
set "PEER=%~1"
shift
if "%~1"=="" (
  echo Usage: astro send ^<peerId^> ^<text...^>
  exit /b 2
)
set "ASTRO_TEXT=%*"
call :ensure_online || exit /b 1
set "ASTRO_PEER=%PEER%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$payload = @{ peerId = $env:ASTRO_PEER; text = $env:ASTRO_TEXT } | ConvertTo-Json -Compress;" ^
  "$uri = $env:BASE + '/v1/messages/send';" ^
  "try { $result = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 20 -ContentType 'application/json' -Body $payload; $result | ConvertTo-Json -Depth 50 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:plugin_enable
if "%~1"=="" (
  echo Usage: astro plugin-enable ^<pluginId^>
  exit /b 2
)
call :ensure_online || exit /b 1
set "ASTRO_PLUGIN_ID=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$id = [uri]::EscapeDataString($env:ASTRO_PLUGIN_ID);" ^
  "$uri = $env:BASE + '/v1/plugins/' + $id + '/enable';" ^
  "try { $result = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 20; $result | ConvertTo-Json -Depth 50 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:plugin_disable
if "%~1"=="" (
  echo Usage: astro plugin-disable ^<pluginId^>
  exit /b 2
)
call :ensure_online || exit /b 1
set "ASTRO_PLUGIN_ID=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$id = [uri]::EscapeDataString($env:ASTRO_PLUGIN_ID);" ^
  "$uri = $env:BASE + '/v1/plugins/' + $id + '/disable';" ^
  "try { $result = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 20; $result | ConvertTo-Json -Depth 50 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:plugin_remove
if "%~1"=="" (
  echo Usage: astro plugin-remove ^<pluginId^>
  exit /b 2
)
call :ensure_online || exit /b 1
set "ASTRO_PLUGIN_ID=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$id = [uri]::EscapeDataString($env:ASTRO_PLUGIN_ID);" ^
  "$uri = $env:BASE + '/v1/plugins/' + $id;" ^
  "try { $result = Invoke-RestMethod -Method Delete -Uri $uri -TimeoutSec 20; $result | ConvertTo-Json -Depth 50 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:plugin_actions
if "%~1"=="" (
  echo Usage: astro plugin-actions ^<pluginId^>
  exit /b 2
)
call :ensure_online || exit /b 1
set "ASTRO_PLUGIN_ID=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$id = [uri]::EscapeDataString($env:ASTRO_PLUGIN_ID);" ^
  "$uri = $env:BASE + '/v1/plugins/' + $id + '/actions';" ^
  "try { $result = Invoke-RestMethod -Method Get -Uri $uri -TimeoutSec 20; $result | ConvertTo-Json -Depth 50 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:plugin_panels
if "%~1"=="" (
  echo Usage: astro plugin-panels ^<pluginId^>
  exit /b 2
)
call :ensure_online || exit /b 1
set "ASTRO_PLUGIN_ID=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$id = [uri]::EscapeDataString($env:ASTRO_PLUGIN_ID);" ^
  "$uri = $env:BASE + '/v1/plugins/' + $id + '/panels';" ^
  "try { $result = Invoke-RestMethod -Method Get -Uri $uri -TimeoutSec 20; $result | ConvertTo-Json -Depth 50 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:plugin_settings
if "%~1"=="" (
  echo Usage: astro plugin-settings ^<pluginId^>
  exit /b 2
)
call :ensure_online || exit /b 1
set "ASTRO_PLUGIN_ID=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$id = [uri]::EscapeDataString($env:ASTRO_PLUGIN_ID);" ^
  "$uri = $env:BASE + '/v1/plugins/' + $id + '/settings-pages';" ^
  "try { $result = Invoke-RestMethod -Method Get -Uri $uri -TimeoutSec 20; $result | ConvertTo-Json -Depth 80 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:setting_bool
if "%~3"=="" (
  echo Usage: astro setting-bool ^<pageId^> ^<settingId^> on^|off
  exit /b 2
)
set "ASTRO_PAGE_ID=%~1"
set "ASTRO_SETTING_ID=%~2"
set "ASTRO_SETTING_BOOL=%~3"
if /I "%ASTRO_SETTING_BOOL%"=="on" set "ASTRO_SETTING_BOOL_JSON=true"
if /I "%ASTRO_SETTING_BOOL%"=="off" set "ASTRO_SETTING_BOOL_JSON=false"
if not defined ASTRO_SETTING_BOOL_JSON (
  echo Usage: astro setting-bool ^<pageId^> ^<settingId^> on^|off
  exit /b 2
)
call :ensure_online || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$page = $env:ASTRO_PAGE_ID;" ^
  "$setting = [uri]::EscapeDataString($env:ASTRO_SETTING_ID);" ^
  "$uri = $env:BASE + '/v1/settings-pages/' + $page + '/settings/' + $setting;" ^
  "$payload = '{\"value\":' + $env:ASTRO_SETTING_BOOL_JSON + '}';" ^
  "try { $result = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 20 -ContentType 'application/json' -Body $payload; $result | ConvertTo-Json -Depth 80 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:setting_int
if "%~3"=="" (
  echo Usage: astro setting-int ^<pageId^> ^<settingId^> ^<value^>
  exit /b 2
)
set "ASTRO_PAGE_ID=%~1"
set "ASTRO_SETTING_ID=%~2"
set "ASTRO_SETTING_INT=%~3"
call :ensure_online || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$page = $env:ASTRO_PAGE_ID;" ^
  "$setting = [uri]::EscapeDataString($env:ASTRO_SETTING_ID);" ^
  "$uri = $env:BASE + '/v1/settings-pages/' + $page + '/settings/' + $setting;" ^
  "$payload = @{ value = [int]$env:ASTRO_SETTING_INT } | ConvertTo-Json -Compress;" ^
  "try { $result = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 20 -ContentType 'application/json' -Body $payload; $result | ConvertTo-Json -Depth 80 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:setting_text
if "%~2"=="" (
  echo Usage: astro setting-text ^<pageId^> ^<settingId^> ^<text...^>
  exit /b 2
)
set "ASTRO_PAGE_ID=%~1"
set "ASTRO_SETTING_ID=%~2"
shift
shift
if "%~1"=="" (
  echo Usage: astro setting-text ^<pageId^> ^<settingId^> ^<text...^>
  exit /b 2
)
set "ASTRO_SETTING_TEXT=%*"
call :ensure_online || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$page = $env:ASTRO_PAGE_ID;" ^
  "$setting = [uri]::EscapeDataString($env:ASTRO_SETTING_ID);" ^
  "$uri = $env:BASE + '/v1/settings-pages/' + $page + '/settings/' + $setting;" ^
  "$payload = @{ value = $env:ASTRO_SETTING_TEXT } | ConvertTo-Json -Compress;" ^
  "try { $result = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 20 -ContentType 'application/json' -Body $payload; $result | ConvertTo-Json -Depth 80 } catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%

:safe_mode
if "%~1"=="" (
  echo Usage: astro safe-mode on^|off
  exit /b 2
)
if /I "%~1"=="on" set "ASTRO_SAFE=true"
if /I "%~1"=="off" set "ASTRO_SAFE=false"
if not defined ASTRO_SAFE (
  echo Usage: astro safe-mode on^|off
  exit /b 2
)
call :ensure_online || exit /b 1
call :invoke POST "/v1/safe-mode" "{\"enabled\":%ASTRO_SAFE%}" || exit /b 1
exit /b 0

:runtime
if "%~1"=="" (
  echo Usage: astro runtime on^|off [port]
  exit /b 2
)
if /I "%~1"=="on" set "ASTRO_RUNTIME=true"
if /I "%~1"=="off" set "ASTRO_RUNTIME=false"
if not defined ASTRO_RUNTIME (
  echo Usage: astro runtime on^|off [port]
  exit /b 2
)
if not "%~2"=="" set "PORT=%~2"
if /I "%ASTRO_RUNTIME%"=="true" (
  call :set_config true || exit /b 1
  call :invoke POST "/v1/runtime" "{\"enabled\":true,\"port\":%PORT%}" >nul 2>nul
  echo Runtime API requested on port %PORT%.
  exit /b 0
)
call :invoke POST "/v1/runtime" "{\"enabled\":false}" >nul 2>nul
call :set_config false || exit /b 1
echo Runtime API disabled.
exit /b 0

:raw
if "%~2"=="" (
  echo Usage: astro raw ^<GET^|POST^|DELETE^> ^</v1/...^> [jsonBody]
  exit /b 2
)
set "ASTRO_RAW_METHOD=%~1"
set "ASTRO_RAW_ENDPOINT=%~2"
shift
shift
set "ASTRO_RAW_BODY=%*"
call :ensure_online || exit /b 1
call :invoke %ASTRO_RAW_METHOD% "%ASTRO_RAW_ENDPOINT%" "%ASTRO_RAW_BODY%" || exit /b 1
exit /b 0

:detect_config
for /f "usebackq tokens=1,* delims==" %%A in (`powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$cfgPath = $env:CFG;" ^
  "$enabled = $false; $port = [int]$env:DEFAULT_PORT;" ^
  "if (Test-Path $cfgPath) { try { $cfg = Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json; if ($cfg.runtimeApi) { $enabled = [bool]$cfg.runtimeApi.enabled; if ($cfg.runtimeApi.port) { $port = [int]$cfg.runtimeApi.port } } } catch {} }" ^
  "Write-Output ('ENABLED=' + $enabled);" ^
  "Write-Output ('PORT=' + $port);" ^
  "Write-Output ('BASE=http://127.0.0.1:' + $port);"`) do set "%%A=%%B"
exit /b 0

:set_config
set "ASTRO_ENABLED=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$path = $env:CFG;" ^
  "$enabled = ($env:ASTRO_ENABLED -eq 'true');" ^
  "$defaultPort = [int]$env:DEFAULT_PORT;" ^
  "if (Test-Path $path) { try { $cfg = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } catch { $cfg = [pscustomobject]@{} } } else { $cfg = [pscustomobject]@{} }" ^
  "if (-not $cfg.PSObject.Properties['runtimeApi']) { $cfg | Add-Member -NotePropertyName runtimeApi -NotePropertyValue ([pscustomobject]@{ enabled = $enabled; port = $defaultPort }) }" ^
  "if (-not $cfg.runtimeApi.PSObject.Properties['port']) { $cfg.runtimeApi | Add-Member -NotePropertyName port -NotePropertyValue $defaultPort }" ^
  "$cfg.runtimeApi.enabled = $enabled;" ^
  "if (-not $cfg.runtimeApi.port) { $cfg.runtimeApi.port = $defaultPort }" ^
  "$cfg | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding UTF8"
exit /b %errorlevel%

:ensure_online
call :detect_config >nul 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$uri = $env:BASE + '/v1/health';" ^
  "try { Invoke-RestMethod -Method Get -Uri $uri -TimeoutSec 3 | Out-Null; exit 0 } catch { exit 1 }"
if errorlevel 1 (
  echo Runtime API is not reachable at %BASE%.
  echo Enable it in Astrogram settings or run: astro enable
  exit /b 1
)
exit /b 0

:invoke
set "ASTRO_METHOD=%~1"
set "ASTRO_ENDPOINT=%~2"
set "ASTRO_BODY=%~3"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$uri = $env:BASE + $env:ASTRO_ENDPOINT;" ^
  "$method = $env:ASTRO_METHOD;" ^
  "$body = $env:ASTRO_BODY;" ^
  "try {" ^
  "  if ($method -eq 'GET' -or [string]::IsNullOrWhiteSpace($body)) { $result = Invoke-RestMethod -Method $method -Uri $uri -TimeoutSec 20 }" ^
  "  else { $result = Invoke-RestMethod -Method $method -Uri $uri -TimeoutSec 20 -ContentType 'application/json' -Body $body }" ^
  "  $result | ConvertTo-Json -Depth 100" ^
  "} catch { if ($_.Exception.Response) { $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $payload = $reader.ReadToEnd(); if ($payload) { Write-Output $payload }; exit 1 } else { Write-Error $_; exit 1 } }"
exit /b %errorlevel%
)BATCH");
	script.replace(u"\n"_q, u"\r\n"_q);
	return script;
}

bool EnsureTextFileContent(const QString &path, const QString &text) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	const auto bytes = text.toUtf8();
	auto existing = QFile(path);
	if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes) {
		return true;
	}
	existing.close();
	if (!existing.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto written = existing.write(bytes);
	existing.close();
	return (written == bytes.size());
}
#endif // _WIN32

void FlushPluginUnload();

[[nodiscard]] QString ClientLogPath() {
	return cWorkingDir() + u"client.log"_q;
}

bool RemoveFileWithRetries(const QString &path, QString *error) {
	for (auto attempt = 0; attempt != 16; ++attempt) {
		QFile file(path);
		if (file.remove() || !QFileInfo::exists(path)) {
			if (error) {
				error->clear();
			}
			return true;
		}
		if (error) {
			*error = file.errorString();
		}
		FlushPluginUnload();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
		QThread::msleep(20);
	}
	return !QFileInfo::exists(path);
}

bool RenameFileWithRetries(
		const QString &from,
		const QString &to,
		QString *error) {
	if (from == to) {
		if (error) {
			error->clear();
		}
		return true;
	}
	if (QFileInfo::exists(to)) {
		QFile::remove(to);
	}
	for (auto attempt = 0; attempt != 16; ++attempt) {
		if (QFile::rename(from, to)) {
			if (error) {
				error->clear();
			}
			return true;
		}
		auto file = QFile(from);
		if (!QFileInfo::exists(from)) {
			if (error) {
				error->clear();
			}
			return true;
		}
		if (error) {
			*error = file.errorString();
		}
		FlushPluginUnload();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
		QThread::msleep(20);
	}
	return !QFileInfo::exists(from);
}

[[nodiscard]] QString PluginDeleteStagingPath(const QString &path) {
	const auto stamp = QDateTime::currentDateTimeUtc().toString(
		u"yyyyMMddHHmmsszzz"_q);
	return path + u".delete."_q + stamp;
}

#if defined(_WIN32)
bool ScheduleDeleteOnReboot(const QString &path) {
	const auto native = QDir::toNativeSeparators(path);
	return MoveFileExW(
		reinterpret_cast<LPCWSTR>(native.utf16()),
		nullptr,
		MOVEFILE_DELAY_UNTIL_REBOOT);
}
#endif // _WIN32

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

[[nodiscard]] QString RecoveryOperationText(const QString &kind) {
	if (kind == u"install"_q) {
		return PluginUiText(u"plugin install"_q, u"установки плагина"_q);
	} else if (kind == u"update"_q) {
		return PluginUiText(u"plugin update"_q, u"обновления плагина"_q);
	} else if (kind == u"reload"_q) {
		return PluginUiText(u"plugin reload"_q, u"перезагрузки плагинов"_q);
	} else if (kind == u"enable"_q) {
		return PluginUiText(u"plugin enable"_q, u"включения плагина"_q);
	} else if (kind == u"disable"_q) {
		return PluginUiText(u"plugin disable"_q, u"выключения плагина"_q);
	} else if (kind == u"load"_q) {
		return PluginUiText(u"plugin loading"_q, u"загрузки плагина"_q);
	} else if (kind == u"onload"_q) {
		return PluginUiText(u"plugin startup"_q, u"запуска плагина"_q);
	} else if (kind == u"unload"_q) {
		return PluginUiText(u"plugin unload"_q, u"выгрузки плагина"_q);
	} else if (kind == u"command"_q) {
		return PluginUiText(u"plugin command"_q, u"команды плагина"_q);
	} else if (kind == u"action"_q) {
		return PluginUiText(u"plugin action"_q, u"действия плагина"_q);
	} else if (kind == u"panel"_q) {
		return PluginUiText(u"plugin panel"_q, u"панели плагина"_q);
	} else if (kind == u"settings"_q) {
		return PluginUiText(
			u"plugin settings"_q,
			u"настроек плагина"_q);
	} else if (kind == u"window"_q) {
		return PluginUiText(
			u"plugin window callback"_q,
			u"оконного callback плагина"_q);
	} else if (kind == u"session"_q) {
		return PluginUiText(
			u"plugin session callback"_q,
			u"session callback плагина"_q);
	} else if (kind == u"outgoing"_q) {
		return PluginUiText(
			u"outgoing interceptor"_q,
			u"перехватчика исходящего текста"_q);
	} else if (kind == u"observer"_q) {
		return PluginUiText(
			u"message observer"_q,
			u"наблюдателя сообщений"_q);
	}
	return PluginUiText(u"plugin operation"_q, u"операции плагина"_q);
}

void FlushPluginUnload() {
	for (auto attempt = 0; attempt != 6; ++attempt) {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		QThread::msleep(25);
	}
}

class RecoveryDuckIcon final : public Ui::RpWidget {
public:
	explicit RecoveryDuckIcon(QWidget *parent) : Ui::RpWidget(parent) {
		setMinimumSize(kRecoveryDuckSize, kRecoveryDuckSize);
		setMaximumSize(kRecoveryDuckSize, kRecoveryDuckSize);
		paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(this);
			p.setRenderHint(QPainter::Antialiasing);
			const auto rect = QRect(0, 0, width(), height());
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 214, 10));
			p.drawEllipse(rect.adjusted(4, 4, -4, -4));
			auto font = this->font();
			font.setPointSize(font.pointSize() + 18);
			p.setFont(font);
			p.setPen(Qt::black);
			p.drawText(rect, Qt::AlignCenter, QString::fromUtf8("🦆"));
		}, lifetime());
	}
};

QString ProcessUserName() {
#if defined(Q_OS_WIN)
	return qEnvironmentVariable("USERNAME");
#else
	return qEnvironmentVariable("USER");
#endif
}

int PhysicalCpuCoresFallback() {
	const auto logical = QThread::idealThreadCount();
	return logical > 0 ? logical : 1;
}

QString TrimmedCommand(QString command) {
	command = command.trimmed();
	if (command.startsWith('/')) {
		command = command.mid(1);
	}
	return command.trimmed();
}

QString NormalizeCommand(QString command) {
	return TrimmedCommand(std::move(command)).toLower();
}

bool IsValidCommandKey(const QString &key) {
	if (key.isEmpty()) {
		return false;
	}
	for (const auto ch : key) {
		if (ch.isSpace() || ch == '/' || ch == '\\' || ch == '@') {
			return false;
		}
	}
	return true;
}

QString PluginBaseName(const QString &path) {
	const auto info = QFileInfo(path);
	const auto base = info.completeBaseName();
	return base.isEmpty() ? info.fileName() : base;
}

QString PreviewText(const char *value) {
	return value
		? QString::fromUtf8(value).trimmed()
		: QString();
}

PluginInfo PluginInfoFromPreview(const Plugins::PreviewInfo &preview) {
	return {
		.id = PreviewText(preview.id),
		.name = PreviewText(preview.name),
		.version = PreviewText(preview.version),
		.author = PreviewText(preview.author),
		.description = PreviewText(preview.description),
		.website = PreviewText(preview.website),
	};
}

void MergePluginInfo(PluginInfo &target, const PluginInfo &preview) {
	if (!preview.id.isEmpty()) {
		target.id = preview.id;
	}
	if (!preview.name.isEmpty()) {
		target.name = preview.name;
	}
	if (!preview.version.isEmpty()) {
		target.version = preview.version;
	}
	if (!preview.author.isEmpty()) {
		target.author = preview.author;
	}
	if (!preview.description.isEmpty()) {
		target.description = preview.description;
	}
	if (!preview.website.isEmpty()) {
		target.website = preview.website;
	}
}

QString PreviewIconFromInfo(const Plugins::PreviewInfo &preview) {
	return PreviewText(preview.icon);
}

QString ReadManifestField(const QByteArray &data, int *offset, bool *ok) {
	if (!offset || !ok || *offset < 0 || *offset >= data.size()) {
		if (ok) {
			*ok = false;
		}
		return QString();
	}
	const auto end = data.indexOf('\0', *offset);
	if (end < 0) {
		*ok = false;
		return QString();
	}
	const auto value = QString::fromUtf8(
		data.constData() + *offset,
		end - *offset).trimmed();
	*offset = end + 1;
	*ok = true;
	return value;
}

bool ReadPreviewManifest(
		const QString &path,
		PluginInfo *info,
		QString *icon) {
	if (!info || !icon) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto data = file.readAll();
	const auto marker = QByteArray(TGD_PLUGIN_MANIFEST_MAGIC) + '\0';
	const auto start = data.indexOf(marker);
	if (start < 0) {
		return false;
	}
	auto offset = start + marker.size();
	auto ok = false;
	const auto id = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto name = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto version = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto author = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto description = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto website = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto parsedIcon = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto endMarker = ReadManifestField(data, &offset, &ok);
	if (!ok || endMarker != QString::fromLatin1(TGD_PLUGIN_MANIFEST_END)) {
		return false;
	}
	info->id = id;
	info->name = name;
	info->version = version;
	info->author = author;
	info->description = description;
	info->website = website;
	*icon = parsedIcon;
	return true;
}

QString SanitizedPluginFileStem(QString value) {
	value = value.trimmed();
	for (auto &ch : value) {
		if (ch.isLetterOrNumber() || ch == '.' || ch == '_' || ch == '-') {
			continue;
		}
		ch = '-';
	}
	while (value.contains(u"--"_q)) {
		value.replace(u"--"_q, u"-"_q);
	}
	value = value.trimmed();
	value.remove(QChar('/'));
	value.remove(QChar('\\'));
	value.remove(QChar(':'));
	value.remove(QChar('*'));
	value.remove(QChar('?'));
	value.remove(QChar('"'));
	value.remove(QChar('<'));
	value.remove(QChar('>'));
	value.remove(QChar('|'));
	return value.isEmpty() ? u"plugin"_q : value;
}

bool CommandLineSafeModeRequested() {
	const auto &arguments = Core::Launcher::Instance().arguments();
	return arguments.contains(QString::fromLatin1(kNoPluginsArgument));
}

QString CurrentExceptionText() {
	try {
		throw;
	} catch (const std::exception &e) {
		const auto message = QString::fromUtf8(e.what()).trimmed();
		return message.isEmpty()
			? u"std::exception"_q
			: message;
	} catch (...) {
		return u"Unknown exception."_q;
	}
}

QString SehExceptionText(unsigned int code) {
	return QString::fromLatin1("Structured exception (0x%1).")
		.arg(QString::number(quint64(code), 16)
			.rightJustified(8, QLatin1Char('0'))
			.toUpper());
}

#if defined(_WIN32) && defined(_MSC_VER)

constexpr auto kMsvcCppExceptionCode = 0xE06D7363u;

int PluginSehFilter(unsigned int code) noexcept {
	return (code == kMsvcCppExceptionCode)
		? EXCEPTION_CONTINUE_SEARCH
		: EXCEPTION_EXECUTE_HANDLER;
}

bool InvokeWithSehGuard(
		void (*callback)(void*) noexcept,
		void *opaque,
		unsigned int *exceptionCode) noexcept {
	__try {
		callback(opaque);
		if (exceptionCode) {
			*exceptionCode = 0;
		}
		return true;
	} __except (PluginSehFilter(GetExceptionCode())) {
		if (exceptionCode) {
			*exceptionCode = GetExceptionCode();
		}
		return false;
	}
}

#endif // _WIN32 && _MSC_VER

template <typename Callback>
void InvokePluginCallbackOrThrow(Callback &&callback) {
#if defined(_WIN32) && defined(_MSC_VER)
	unsigned int code = 0;
	auto *opaque = static_cast<void*>(&callback);
	const auto ok = InvokeWithSehGuard(
		+[](void *context) noexcept {
			auto *fn = static_cast<std::remove_reference_t<Callback>*>(context);
			(*fn)();
		},
		opaque,
		&code);
	if (!ok) {
		throw std::runtime_error(SehExceptionText(code).toStdString());
	}
#else // _WIN32 && _MSC_VER
	callback();
#endif // _WIN32 && _MSC_VER
}

QString DescribeBinaryInfoMismatch(const Plugins::BinaryInfo &info) {
	const auto pluginCompiler = QString::fromLatin1(info.compiler ? info.compiler : "unknown");
	const auto hostCompiler = QString::fromLatin1(Plugins::kCompilerId);
	const auto pluginPlatform = QString::fromLatin1(info.platform ? info.platform : "unknown");
	const auto hostPlatform = QString::fromLatin1(Plugins::kPlatformId);

	if (info.structVersion != Plugins::kBinaryInfoVersion) {
		return u"Incompatible plugin metadata version."_q;
	}
	if (info.apiVersion != Plugins::kApiVersion) {
		return u"Incompatible plugin API version."_q;
	}
	if (info.pointerSize != int(sizeof(void*))) {
		return u"Incompatible plugin architecture."_q;
	}
	if (pluginPlatform != hostPlatform) {
		return u"Incompatible plugin platform."_q;
	}
	if (pluginCompiler != hostCompiler
		|| info.compilerVersion != Plugins::kCompilerVersion) {
		return u"Incompatible plugin compiler ABI."_q;
	}
	if (info.qtMajor != QT_VERSION_MAJOR || info.qtMinor != QT_VERSION_MINOR) {
		return u"Incompatible Qt major/minor version."_q;
	}
	return QString();
}

QString DescribeBinaryInfo(const Plugins::BinaryInfo &info) {
	return QString::fromLatin1(
		"api=%1 ptr=%2 qt=%3.%4 compiler=%5/%6 platform=%7")
		.arg(info.apiVersion)
		.arg(info.pointerSize)
		.arg(info.qtMajor)
		.arg(info.qtMinor)
		.arg(QString::fromLatin1(info.compiler ? info.compiler : "unknown"))
		.arg(info.compilerVersion)
			.arg(QString::fromLatin1(info.platform ? info.platform : "unknown"));
}

QString DescribeBinaryInfoMismatchField(const Plugins::BinaryInfo &info) {
	const auto pluginCompiler = QString::fromLatin1(
		info.compiler ? info.compiler : "unknown");
	const auto hostCompiler = QString::fromLatin1(Plugins::kCompilerId);
	const auto pluginPlatform = QString::fromLatin1(
		info.platform ? info.platform : "unknown");
	const auto hostPlatform = QString::fromLatin1(Plugins::kPlatformId);

	if (info.structVersion != Plugins::kBinaryInfoVersion) {
		return u"structVersion"_q;
	}
	if (info.apiVersion != Plugins::kApiVersion) {
		return u"apiVersion"_q;
	}
	if (info.pointerSize != int(sizeof(void*))) {
		return u"pointerSize"_q;
	}
	if (pluginPlatform != hostPlatform) {
		return u"platform"_q;
	}
	if (pluginCompiler != hostCompiler) {
		return u"compiler"_q;
	}
	if (info.compilerVersion != Plugins::kCompilerVersion) {
		return u"compilerVersion"_q;
	}
	if (info.qtMajor != QT_VERSION_MAJOR) {
		return u"qtMajor"_q;
	}
	if (info.qtMinor != QT_VERSION_MINOR) {
		return u"qtMinor"_q;
	}
	return QString();
}

struct PackageLoadabilityCheckResult {
	bool compatible = false;
	bool libraryLoaded = false;
	bool previewExportFound = false;
	bool previewExportValid = false;
	bool binaryInfoFound = false;
	bool entryFound = false;
	PluginInfo previewInfo;
	QString previewIcon;
	QString error;
	QString binarySummary;
	QString mismatchField;
};

PackageLoadabilityCheckResult CheckPackageLoadability(const QString &path) {
	auto result = PackageLoadabilityCheckResult();
	auto library = QLibrary(path);
	if (!library.load()) {
		const auto message = library.errorString().trimmed();
		result.error = message.isEmpty()
			? u"Could not load the plugin package as a library."_q
			: message;
		return result;
	}
	result.libraryLoaded = true;

	const auto finish = [&](QString error = QString()) {
		result.compatible = error.isEmpty();
		if (!error.isEmpty()) {
			result.error = std::move(error);
		}
		library.unload();
		return result;
	};

	if (const auto previewInfo = reinterpret_cast<PreviewInfoFn>(
			library.resolve(kPreviewInfoName))) {
		result.previewExportFound = true;
		auto preview = static_cast<const Plugins::PreviewInfo*>(nullptr);
		try {
			InvokePluginCallbackOrThrow([&] {
				preview = previewInfo();
			});
		} catch (...) {
			return finish(u"Plugin preview export failed: "_q + CurrentExceptionText());
		}
		if (preview
			&& preview->structVersion == kPreviewInfoVersion
			&& preview->apiVersion == kApiVersion) {
			result.previewExportValid = true;
			result.previewInfo = PluginInfoFromPreview(*preview);
			result.previewIcon = PreviewIconFromInfo(*preview);
		}
	}

	const auto binaryInfo = reinterpret_cast<BinaryInfoFn>(
		library.resolve(kBinaryInfoName));
	if (!binaryInfo) {
		return finish(u"Missing TgdPluginBinaryInfo export."_q);
	}
	result.binaryInfoFound = true;

	auto info = static_cast<const Plugins::BinaryInfo*>(nullptr);
	try {
		InvokePluginCallbackOrThrow([&] {
			info = binaryInfo();
		});
	} catch (...) {
		return finish(u"TgdPluginBinaryInfo failed: "_q + CurrentExceptionText());
	}
	if (!info) {
		return finish(u"Plugin binary metadata is null."_q);
	}
	result.binarySummary = DescribeBinaryInfo(*info);
	if (const auto mismatch = DescribeBinaryInfoMismatch(*info);
		!mismatch.isEmpty()) {
		result.mismatchField = DescribeBinaryInfoMismatchField(*info);
		return finish(mismatch);
	}

	result.entryFound = (library.resolve(kEntryName) != nullptr);
	if (!result.entryFound) {
		return finish(u"Missing TgdPluginEntry export."_q);
	}

	return finish();
}

QString JsonValueToText(const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) {
		return u"null"_q;
	}
	if (value.isBool()) {
		return value.toBool() ? u"true"_q : u"false"_q;
	}
	if (value.isDouble()) {
		return QString::number(value.toDouble(), 'g', 16);
	}
	if (value.isString()) {
		auto text = value.toString();
		text.replace(u'\\', u"\\\\"_q);
		text.replace(u'\r', u"\\r"_q);
		text.replace(u'\n', u"\\n"_q);
		text.replace(u'\t', u"\\t"_q);
		text.replace(u'"', u"\\\""_q);
		return u"\""_q + text + u"\""_q;
	}
	return QString::fromUtf8(
		QJsonDocument(value.isObject()
			? QJsonDocument(value.toObject())
			: QJsonDocument(value.toArray()))
			.toJson(QJsonDocument::Compact));
}

void MergeJsonObject(QJsonObject &target, const QJsonObject &extra) {
	for (auto i = extra.begin(); i != extra.end(); ++i) {
		target.insert(i.key(), i.value());
	}
}

QJsonArray JsonArrayFromStrings(const QStringList &list) {
	auto result = QJsonArray();
	for (const auto &value : list) {
		result.push_back(value);
	}
	return result;
}

QStringList SortedJsonKeys(const QJsonObject &object) {
	auto keys = object.keys();
	keys.sort(Qt::CaseInsensitive);
	return keys;
}

QString CommandResultActionName(const Plugins::CommandResult::Action action) {
	using Action = Plugins::CommandResult::Action;
	switch (action) {
	case Action::Continue:
		return u"continue"_q;
	case Action::Cancel:
		return u"cancel"_q;
	case Action::Handled:
		return u"handled"_q;
	case Action::ReplaceText:
		return u"replace_text"_q;
	}
	return u"unknown"_q;
}

QString MessageEventName(const Plugins::MessageEvent event) {
	switch (event) {
	case Plugins::MessageEvent::New:
		return u"new"_q;
	case Plugins::MessageEvent::Edited:
		return u"edited"_q;
	case Plugins::MessageEvent::Deleted:
		return u"deleted"_q;
	}
	return u"unknown"_q;
}

QString SettingControlName(const Plugins::SettingControl control) {
	using Control = Plugins::SettingControl;
	switch (control) {
	case Control::Toggle:
		return u"toggle"_q;
	case Control::IntSlider:
		return u"int_slider"_q;
	case Control::TextInput:
		return u"text_input"_q;
	case Control::ActionButton:
		return u"action_button"_q;
	case Control::InfoText:
		return u"info_text"_q;
	}
	return u"unknown"_q;
}

bool HasAnySettings(const Plugins::SettingsPageDescriptor &descriptor) {
	for (const auto &section : descriptor.sections) {
		if (!section.settings.isEmpty()) {
			return true;
		}
	}
	return false;
}

Plugins::SettingDescriptor NormalizeSettingDescriptor(
		Plugins::SettingDescriptor descriptor) {
	descriptor.id = descriptor.id.trimmed();
	descriptor.title = descriptor.title.trimmed();
	descriptor.description = descriptor.description.trimmed();
	descriptor.textValue = descriptor.textValue.trimmed();
	descriptor.placeholderText = descriptor.placeholderText.trimmed();
	descriptor.valueSuffix = descriptor.valueSuffix.trimmed();
	descriptor.buttonText = descriptor.buttonText.trimmed();
	switch (descriptor.type) {
	case Plugins::SettingControl::Toggle:
	case Plugins::SettingControl::TextInput:
	case Plugins::SettingControl::ActionButton:
	case Plugins::SettingControl::InfoText:
		break;
	case Plugins::SettingControl::IntSlider: {
		if (descriptor.intMinimum > descriptor.intMaximum) {
			std::swap(descriptor.intMinimum, descriptor.intMaximum);
		}
		if (descriptor.intStep <= 0) {
			descriptor.intStep = 1;
		}
		descriptor.intValue = std::clamp(
			descriptor.intValue,
			descriptor.intMinimum,
			descriptor.intMaximum);
	} break;
	}
	return descriptor;
}

QString DateTimeToIsoUtc(const QDateTime &value) {
	return value.isValid()
		? value.toUTC().toString(Qt::ISODateWithMs)
		: QString();
}

QJsonObject RecoveryOperationToJson(
		const RecoveryOperationState &state) {
	auto object = QJsonObject{
		{ u"kind"_q, state.kind },
		{ u"details"_q, state.details },
		{ u"startedAt"_q, state.startedAt },
	};
	auto pluginIds = QJsonArray();
	for (const auto &id : state.pluginIds) {
		pluginIds.push_back(id);
	}
	object.insert(u"pluginIds"_q, pluginIds);
	return object;
}

RecoveryOperationState RecoveryOperationFromJson(
		const QJsonObject &object) {
	auto state = RecoveryOperationState();
	state.kind = object.value(u"kind"_q).toString();
	state.details = object.value(u"details"_q).toString();
	state.startedAt = object.value(u"startedAt"_q).toString();
	const auto pluginIds = object.value(u"pluginIds"_q).toArray();
	for (const auto &value : pluginIds) {
		if (value.isString()) {
			state.pluginIds.push_back(value.toString());
		}
	}
	state.active = !state.kind.isEmpty();
	return state;
}

QByteArray RuntimeStatusText(int code) {
	switch (code) {
	case 200: return "OK";
	case 201: return "Created";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	default: return "OK";
	}
}

QByteArray RuntimeJsonResponse(
		int code,
		const QJsonDocument &document) {
	const auto payload = document.toJson(QJsonDocument::Compact);
	auto response = QByteArray();
	response.reserve(payload.size() + 256);
	response += "HTTP/1.1 ";
	response += QByteArray::number(code);
	response += ' ';
	response += RuntimeStatusText(code);
	response += "\r\nContent-Type: application/json; charset=utf-8";
	response += "\r\nConnection: close";
	response += "\r\nContent-Length: ";
	response += QByteArray::number(payload.size());
	response += "\r\n\r\n";
	response += payload;
	return response;
}

QByteArray RuntimeErrorResponse(
		int code,
		const QString &message) {
	return RuntimeJsonResponse(
		code,
		QJsonDocument(QJsonObject{
			{ u"ok"_q, false },
			{ u"error"_q, message },
		}));
}

QByteArray RuntimeOkResponse(const QJsonObject &payload) {
	auto result = payload;
	if (!result.contains(u"ok"_q)) {
		result.insert(u"ok"_q, true);
	}
	return RuntimeJsonResponse(200, QJsonDocument(result));
}

bool ParseRuntimeHttpRequest(
		const QByteArray &buffer,
		QString &method,
		QString &target,
		QHash<QByteArray, QByteArray> &headers,
		QByteArray &body,
		qsizetype &consumed,
		QString &error) {
	const auto headerEnd = buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		error = u"incomplete"_q;
		return false;
	}
	const auto headersPart = buffer.left(headerEnd);
	const auto lines = headersPart.split('\n');
	if (lines.isEmpty()) {
		error = u"empty request"_q;
		return false;
	}
	const auto requestLine = QByteArray(lines.front()).trimmed();
	const auto requestParts = requestLine.split(' ');
	if (requestParts.size() < 2) {
		error = u"invalid request line"_q;
		return false;
	}
	method = QString::fromLatin1(requestParts[0]).trimmed().toUpper();
	target = QString::fromLatin1(requestParts[1]).trimmed();
	headers.clear();
	for (auto i = 1; i < lines.size(); ++i) {
		const auto line = QByteArray(lines[i]).trimmed();
		if (line.isEmpty()) {
			continue;
		}
		const auto colon = line.indexOf(':');
		if (colon <= 0) {
			continue;
		}
		const auto key = line.left(colon).trimmed().toLower();
		const auto value = line.mid(colon + 1).trimmed();
		headers.insert(key, value);
	}
	auto contentLength = 0;
	if (const auto value = headers.value("content-length"); !value.isEmpty()) {
		contentLength = value.toInt();
		if (contentLength < 0) {
			error = u"invalid content-length"_q;
			return false;
		}
	}
	const auto bodyOffset = headerEnd + 4;
	if (buffer.size() < (bodyOffset + contentLength)) {
		error = u"incomplete"_q;
		return false;
	}
	body = buffer.mid(bodyOffset, contentLength);
	consumed = bodyOffset + contentLength;
	return true;
}

QJsonObject RuntimePluginJson(const PluginState &state) {
	return QJsonObject{
		{ u"id"_q, state.info.id },
		{ u"name"_q, state.info.name },
		{ u"version"_q, state.info.version },
		{ u"author"_q, state.info.author },
		{ u"description"_q, state.info.description },
		{ u"path"_q, state.path },
		{ u"enabled"_q, state.enabled },
		{ u"loaded"_q, state.loaded },
		{ u"error"_q, state.error },
		{ u"disabledByRecovery"_q, state.disabledByRecovery },
		{ u"recoverySuspected"_q, state.recoverySuspected },
		{ u"recoveryReason"_q, state.recoveryReason },
	};
}

QStringList RuntimeTailLines(const QString &path, int wantedLines) {
	if (wantedLines <= 0) {
		return {};
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return {};
	}
	constexpr auto kBlock = qint64(4096);
	auto buffer = QByteArray();
	auto left = file.size();
	auto lines = 0;
	while (left > 0 && lines <= wantedLines + 16) {
		const auto take = std::min(kBlock, left);
		left -= take;
		file.seek(left);
		buffer.prepend(file.read(take));
		lines = buffer.count('\n');
	}
	auto text = QString::fromUtf8(buffer);
	auto split = text.split(QChar::fromLatin1('\n'));
	for (auto &line : split) {
		line = line.trimmed();
	}
	split.erase(
		std::remove_if(split.begin(), split.end(), [](const QString &line) {
			return line.isEmpty();
		}),
		split.end());
	if (split.size() > wantedLines) {
		split = split.mid(split.size() - wantedLines);
	}
	return split;
}

QString RuntimeApiAdvertisedHost() {
	const auto interfaces = QNetworkInterface::allInterfaces();
	for (const auto &iface : interfaces) {
		if (!(iface.flags() & QNetworkInterface::IsUp)
			|| !(iface.flags() & QNetworkInterface::IsRunning)
			|| (iface.flags() & QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const auto &entry : iface.addressEntries()) {
			const auto ip = entry.ip();
			if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
				continue;
			}
			if (ip.isLoopback()) {
				continue;
			}
			const auto value = ip.toString();
			if (value.startsWith(u"169.254."_q)) {
				continue;
			}
			return value;
		}
	}
	return u"127.0.0.1"_q;
}

QHostAddress RuntimeApiListenAddress() {
	return QHostAddress::AnyIPv4;
}

std::optional<PeerId> RuntimePeerIdFromString(const QString &text) {
	bool ok = false;
	const auto value = text.trimmed().toULongLong(&ok);
	if (!ok || value == 0) {
		return std::nullopt;
	}
	return PeerId(PeerIdHelper{ value });
}

QJsonObject RuntimeMessageJson(
		not_null<HistoryItem*> item,
		PeerId peerId) {
	const auto fullId = item->fullId();
	auto result = QJsonObject();
	result.insert(u"peerId"_q, QString::number(peerId.value));
	result.insert(u"id"_q, item->id.bare);
	result.insert(u"fullMsgPeerId"_q, QString::number(fullId.peer.value));
	result.insert(u"date"_q, item->date());
	result.insert(u"out"_q, item->out());
	result.insert(u"text"_q, item->notificationText().text);
	if (const auto from = item->from()) {
		result.insert(u"fromPeerId"_q, QString::number(from->id.value));
	}
	return result;
}

} // namespace

Manager::Manager(QObject *parent) : QObject(parent) {
}

Manager::~Manager() {
	stopRuntimeApiServer();
	unloadAll();
}

void Manager::loadRecoveryState() {
	_disabledByRecovery.clear();
	_recoveryPending = RecoveryOperationState();
	_recoveryNotice = RecoveryOperationState();
	_recoveryNoticeShown = false;

	QFile file(_recoveryPath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return;
	}
	const auto object = document.object();
	const auto disabledArray = object.value(u"disabledByRecovery"_q).toArray();
	for (const auto &value : disabledArray) {
		if (value.isString()) {
			_disabledByRecovery.insert(value.toString());
		}
	}
	if (const auto pending = object.value(u"pending"_q); pending.isObject()) {
		_recoveryPending = RecoveryOperationFromJson(pending.toObject());
	}
	if (const auto notice = object.value(u"notice"_q); notice.isObject()) {
		_recoveryNotice = RecoveryOperationFromJson(notice.toObject());
	}
	logEvent(
		u"recovery"_q,
		u"state-loaded"_q,
		QJsonObject{
			{ u"path"_q, _recoveryPath },
			{ u"disabledByRecovery"_q, JsonArrayFromStrings(_disabledByRecovery.values()) },
			{ u"pendingActive"_q, _recoveryPending.active },
			{ u"pendingKind"_q, _recoveryPending.kind },
			{ u"noticeActive"_q, _recoveryNotice.active },
			{ u"noticeKind"_q, _recoveryNotice.kind },
		});
}

void Manager::saveRecoveryState() const {
	if (_recoveryPath.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(_recoveryPath).absolutePath());

	auto disabledByRecovery = _disabledByRecovery.values();
	disabledByRecovery.sort(Qt::CaseInsensitive);

	auto object = QJsonObject();
	auto disabledArray = QJsonArray();
	for (const auto &id : disabledByRecovery) {
		disabledArray.push_back(id);
	}
	object.insert(u"disabledByRecovery"_q, disabledArray);
	if (_recoveryPending.active) {
		object.insert(u"pending"_q, RecoveryOperationToJson(_recoveryPending));
	}
	if (_recoveryNotice.active) {
		object.insert(u"notice"_q, RecoveryOperationToJson(_recoveryNotice));
	}

	QSaveFile file(_recoveryPath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
		file.commit();
	}
	logEvent(
		u"recovery"_q,
		u"state-saved"_q,
		QJsonObject{
			{ u"path"_q, _recoveryPath },
			{ u"disabledByRecovery"_q, JsonArrayFromStrings(disabledByRecovery) },
			{ u"pendingActive"_q, _recoveryPending.active },
			{ u"noticeActive"_q, _recoveryNotice.active },
		});
}

void Manager::recoverFromPendingState() {
	if (!_recoveryPending.active) {
		return;
	}
	const auto pluginIds = _recoveryPending.pluginIds;
	const auto details = _recoveryPending.details;
	const auto kind = _recoveryPending.kind;

	for (const auto &pluginId : pluginIds) {
		if (pluginId.isEmpty()) {
			continue;
		}
		_disabled.insert(pluginId);
		_disabledByRecovery.insert(pluginId);
	}
	saveConfig();
	QDir().mkpath(QFileInfo(_safeModePath).absolutePath());
	auto file = QFile(_safeModePath);
	if (!file.exists() && file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.close();
	}
	_recoveryPending = RecoveryOperationState();
	queueRecoveryNotice(kind, pluginIds, details);
	logEvent(
		u"recovery"_q,
		u"safe-mode-enabled-after-crash"_q,
		QJsonObject{
			{ u"kind"_q, kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(pluginIds) },
			{ u"details"_q, details },
			{ u"safeModePath"_q, _safeModePath },
		});
	saveRecoveryState();
}

void Manager::startRecoveryOperation(
		QString kind,
		QStringList pluginIds,
		QString details) {
	_recoveryPending.active = true;
	_recoveryPending.kind = std::move(kind);
	_recoveryPending.pluginIds = std::move(pluginIds);
	_recoveryPending.details = std::move(details);
	_recoveryPending.startedAt = QDateTime::currentDateTimeUtc().toString(
		Qt::ISODateWithMs);
	logOperationStart(
		_recoveryPending.kind,
		_recoveryPending.pluginIds,
		_recoveryPending.details);
	saveRecoveryState();
}

void Manager::finishRecoveryOperation() {
	if (!_recoveryPending.active) {
		return;
	}
	_recoveryPending = RecoveryOperationState();
	saveRecoveryState();
	logOperationFinish();
}

void Manager::syncRecoveryFlags(PluginState &state) const {
	state.disabledByRecovery = _disabledByRecovery.contains(state.info.id);
	state.recoverySuspected = state.disabledByRecovery;
	state.recoveryReason = state.disabledByRecovery
		? PluginUiText(
			u"Disabled automatically after a suspected plugin crash."_q,
			u"Автоматически выключен после подозрения на крэш плагина."_q)
		: QString();
}

void Manager::clearRecoveryDisabled(const QString &pluginId) {
	if (pluginId.isEmpty()) {
		return;
	}
	_disabledByRecovery.remove(pluginId);
	if (_recoveryNotice.active) {
		_recoveryNotice.pluginIds.removeAll(pluginId);
		if (_recoveryNotice.pluginIds.isEmpty()) {
			_recoveryNotice = RecoveryOperationState();
		}
	}
	saveRecoveryState();
}

void Manager::queueRecoveryNotice(
		QString kind,
		QStringList pluginIds,
		QString details) {
	_recoveryNotice.active = true;
	_recoveryNotice.kind = std::move(kind);
	_recoveryNotice.pluginIds = std::move(pluginIds);
	_recoveryNotice.details = std::move(details);
	_recoveryNotice.startedAt = QDateTime::currentDateTimeUtc().toString(
		Qt::ISODateWithMs);
	_recoveryNoticeShown = false;
	logEvent(
		u"recovery"_q,
		u"notice-queued"_q,
		QJsonObject{
			{ u"kind"_q, _recoveryNotice.kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(_recoveryNotice.pluginIds) },
			{ u"details"_q, _recoveryNotice.details },
			{ u"startedAt"_q, _recoveryNotice.startedAt },
		});
}

QStringList Manager::describeRecoveryPlugins(
		const QStringList &pluginIds) const {
	auto result = QStringList();
	for (const auto &pluginId : pluginIds) {
		if (pluginId.isEmpty()) {
			continue;
		}
		if (const auto record = findRecord(pluginId)) {
			const auto name = record->state.info.name.trimmed();
			result.push_back(name.isEmpty() ? pluginId : name);
		} else {
			result.push_back(pluginId);
		}
	}
	result.removeDuplicates();
	return result;
}

QString Manager::composeRecoveryClipboardText() const {
	auto lines = QStringList();
	lines.push_back(PluginUiText(
		u"Astrogram Desktop plugin recovery log"_q,
		u"Лог восстановления плагинов Astrogram Desktop"_q));
	if (_recoveryNotice.active) {
		const auto plugins = describeRecoveryPlugins(_recoveryNotice.pluginIds);
		lines.push_back(
			PluginUiText(u"Operation: "_q, u"Операция: "_q)
			+ RecoveryOperationText(_recoveryNotice.kind));
		if (!plugins.isEmpty()) {
			lines.push_back(
				PluginUiText(u"Plugins: "_q, u"Плагины: "_q)
				+ plugins.join(u", "_q));
		}
		if (!_recoveryNotice.details.trimmed().isEmpty()) {
			lines.push_back(
				PluginUiText(u"Details: "_q, u"Детали: "_q)
				+ _recoveryNotice.details.trimmed());
		}
	}
	if (!_logPath.isEmpty()) {
		auto file = QFile(_logPath);
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const auto content = QString::fromUtf8(file.readAll()).trimmed();
			if (!content.isEmpty()) {
				lines.push_back(QString());
				lines.push_back(u"plugins.log"_q);
				lines.push_back(content);
			}
		}
	}
	if (!_tracePath.isEmpty()) {
		auto file = QFile(_tracePath);
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const auto content = QString::fromUtf8(file.readAll()).trimmed();
			if (!content.isEmpty()) {
				lines.push_back(QString());
				lines.push_back(u"plugins.trace.jsonl"_q);
				lines.push_back(content);
			}
		}
	}
	return lines.join(u"\n"_q);
}

void Manager::showRecoveryNotice(Window::Controller *window) {
	if (_recoveryNoticeShown || !_recoveryNotice.active || !window) {
		return;
	}
	_recoveryNoticeShown = true;
	const auto clipboardText = composeRecoveryClipboardText();
	if (const auto clipboard = QGuiApplication::clipboard()) {
		clipboard->setText(clipboardText);
	}

	const auto plugins = describeRecoveryPlugins(_recoveryNotice.pluginIds);
	const auto pluginText = plugins.isEmpty()
		? PluginUiText(u"unknown plugin"_q, u"неизвестный плагин"_q)
		: plugins.join(u", "_q);
	const auto title = PluginUiText(u"Safe Mode"_q, u"Безопасный режим"_q);
	const auto body = plugins.size() == 1
		? PluginUiText(
			u"Astrogram noticed a crash during "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nSuspected plugin: "_q
				+ pluginText
				+ u"\n\nSafe mode was enabled automatically, the plugin was turned off, and the recovery log was copied to the clipboard."_q,
			u"Astrogram заметил сбой во время "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nПодозреваемый плагин: "_q
				+ pluginText
				+ u"\n\nБезопасный режим включён автоматически, плагин выключен, лог восстановления уже скопирован в буфер обмена."_q)
		: PluginUiText(
			u"Astrogram noticed a crash during "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nSuspected plugins: "_q
				+ pluginText
				+ u"\n\nSafe mode was enabled automatically, the listed plugins were turned off, and the recovery log was copied to the clipboard."_q,
			u"Astrogram заметил сбой во время "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nПодозреваемые плагины: "_q
				+ pluginText
				+ u"\n\nБезопасный режим включён автоматически, указанные плагины выключены, лог восстановления уже скопирован в буфер обмена."_q);

	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(title));
		box->addLeftButton(rpl::single(
			PluginUiText(u"Copy"_q, u"Копировать"_q)), [=] {
			if (const auto clipboard = QGuiApplication::clipboard()) {
				clipboard->setText(clipboardText);
			}
			window->showToast(PluginUiText(
				u"Recovery log copied."_q,
				u"Лог восстановления скопирован."_q));
		});
		box->addRow(
			object_ptr<RecoveryDuckIcon>(box),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				st::boxPadding.bottom() / 2),
			style::al_top);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(body),
			st::boxLabel),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				0),
			style::al_top);
		box->addButton(rpl::single(
			PluginUiText(u"Continue"_q, u"Продолжить"_q)), [=] {
			box->closeBox();
		});
	}));

	_recoveryNotice = RecoveryOperationState();
	saveRecoveryState();
}

void Manager::start() {
	_pluginsPath = cWorkingDir() + QString::fromLatin1(kPluginsFolder);
	_configPath = cWorkingDir() + QString::fromLatin1(kConfigFile);
	_logPath = cWorkingDir() + QString::fromLatin1(kLogFile);
	_tracePath = cWorkingDir() + QString::fromLatin1(kTraceFile);
	_safeModePath = cWorkingDir() + QString::fromLatin1(kSafeModeFile);
	_recoveryPath = cWorkingDir() + QString::fromLatin1(kRecoveryFile);
	ensureRuntimeApiCliHelper();
	logEvent(
		u"manager"_q,
		u"start"_q,
		QJsonObject{
			{ u"pluginsPath"_q, _pluginsPath },
			{ u"configPath"_q, _configPath },
			{ u"logPath"_q, _logPath },
			{ u"tracePath"_q, _tracePath },
			{ u"safeModePath"_q, _safeModePath },
			{ u"recoveryPath"_q, _recoveryPath },
			{ u"apiVersion"_q, kApiVersion },
		});
	loadConfig();
	ensureRuntimeApiServerState();
	loadRecoveryState();
	recoverFromPendingState();
	if (safeModeEnabled()) {
		logEvent(u"safe-mode"_q, u"startup-scan-metadata-only"_q);
		scanPlugins(true);
	} else {
		scanPlugins();
	}
	_sessionLifetime.destroy();
	Core::App().domain().activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		handleActiveSessionChanged(session);
	}, _sessionLifetime);
}

void Manager::ensureRuntimeApiCliHelper() {
#if defined(_WIN32)
	const auto helperDir = RuntimeCliDirectoryPath();
	const auto helperPath = RuntimeCliHelperPath();
	const auto helperWritten = EnsureTextFileContent(
		helperPath,
		RuntimeApiCliScript());
	const auto pathRegistered = helperWritten
		&& EnsureUserPathContainsDirectory(helperDir);
	Logs::writeClient(QString::fromLatin1(
		"[runtime-api] cli helper: path=%1 written=%2 pathRegistered=%3")
		.arg(QDir::toNativeSeparators(helperPath))
		.arg(helperWritten ? u"true"_q : u"false"_q)
		.arg(pathRegistered ? u"true"_q : u"false"_q));
	logEvent(
		u"runtime-api"_q,
		u"cli-helper"_q,
		QJsonObject{
			{ u"path"_q, helperPath },
			{ u"directory"_q, helperDir },
			{ u"written"_q, helperWritten },
			{ u"pathRegistered"_q, pathRegistered },
		});
#endif // _WIN32
}

void Manager::reload() {
	if (_reloadInProgress) {
		_reloadQueued = true;
		Logs::writeClient(u"[plugins] reload queued while another reload is running"_q);
		logEvent(
			u"manager"_q,
			u"reload-queued"_q,
			QJsonObject{
				{ u"safeMode"_q, safeModeEnabled() },
				{ u"knownPlugins"_q, int(_plugins.size()) },
			});
		return;
	}
	_reloadInProgress = true;
	do {
		_reloadQueued = false;
		Logs::writeClient(QString::fromLatin1(
			"[plugins] reload requested: safeMode=%1 knownPlugins=%2")
			.arg(safeModeEnabled() ? u"true"_q : u"false"_q)
			.arg(_plugins.size()));
		logEvent(
			u"manager"_q,
			u"reload-requested"_q,
			QJsonObject{
				{ u"safeMode"_q, safeModeEnabled() },
				{ u"knownPlugins"_q, int(_plugins.size()) },
			});
		unloadAll();
		loadConfig();
		ensureRuntimeApiServerState();
		loadRecoveryState();
		FlushPluginUnload();
		if (safeModeEnabled()) {
			logEvent(u"safe-mode"_q, u"reload-scan-metadata-only"_q);
			scanPlugins(true);
			continue;
		}
		const auto ownsRecovery = !_recoveryPending.active;
		if (ownsRecovery) {
			startRecoveryOperation(u"reload"_q);
		}
		scanPlugins();
		if (ownsRecovery) {
			finishRecoveryOperation();
		}
	} while (_reloadQueued);
	_reloadInProgress = false;
}

std::vector<PluginState> Manager::plugins() const {
	auto result = std::vector<PluginState>();
	result.reserve(_plugins.size());
	for (const auto &plugin : _plugins) {
		auto state = plugin.state;
		syncRecoveryFlags(state);
		syncSourceTrustState(state);
		result.push_back(std::move(state));
	}
	return result;
}

bool Manager::safeModeEnabled() const {
	if (CommandLineSafeModeRequested()) {
		return true;
	}
	return !_safeModePath.isEmpty() && QFileInfo::exists(_safeModePath);
}

bool Manager::setSafeModeEnabled(bool enabled) {
	if (_safeModePath.isEmpty()) {
		return false;
	}
	QDir().mkpath(QFileInfo(_safeModePath).absolutePath());
	if (enabled) {
		QFile file(_safeModePath);
		if (!file.exists()) {
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				return false;
			}
			file.close();
		}
	} else if (QFileInfo::exists(_safeModePath) && !QFile::remove(_safeModePath)) {
		return false;
	}
	if (!enabled) {
		const auto recovered = _disabledByRecovery.values();
		for (const auto &pluginId : recovered) {
			_disabled.remove(pluginId);
			clearRecoveryDisabled(pluginId);
		}
		saveConfig();
	}
	logEvent(
		u"safe-mode"_q,
		u"set"_q,
		QJsonObject{
			{ u"enabled"_q, enabled },
			{ u"path"_q, _safeModePath },
		});
	reload();
	return true;
}

bool Manager::runtimeApiEnabled() const {
	return _runtimeApiEnabled;
}

int Manager::runtimeApiPort() const {
	return _runtimeApiPort;
}

bool Manager::setRuntimeApiEnabled(bool enabled) {
	if (_runtimeApiEnabled == enabled) {
		return true;
	}
	_runtimeApiEnabled = enabled;
	ensureRuntimeApiServerState();
	saveConfig();
	logEvent(
		u"runtime-api"_q,
		u"set-enabled"_q,
		QJsonObject{
			{ u"enabled"_q, enabled },
			{ u"port"_q, _runtimeApiPort },
		});
	return true;
}

void Manager::ensureRuntimeApiServerState() {
	if (_runtimeApiEnabled) {
		startRuntimeApiServer();
	} else {
		stopRuntimeApiServer();
	}
}

bool Manager::startRuntimeApiServer() {
	if (_runtimeApiServer && _runtimeApiServer->isListening()) {
		return true;
	}
	if (!_runtimeApiServer) {
		_runtimeApiServer = std::make_unique<QTcpServer>();
		_runtimeApiServer->setParent(this);
		QObject::connect(
			_runtimeApiServer.get(),
			&QTcpServer::newConnection,
			this,
			&Manager::onRuntimeApiNewConnection);
	}
	if (_runtimeApiPort <= 0 || _runtimeApiPort > 65535) {
		_runtimeApiPort = 37080;
	}
	const auto listenAddress = RuntimeApiListenAddress();
	if (!_runtimeApiServer->listen(listenAddress, quint16(_runtimeApiPort))) {
		logEvent(
			u"runtime-api"_q,
			u"listen-failed"_q,
			QJsonObject{
				{ u"address"_q, listenAddress.toString() },
				{ u"port"_q, _runtimeApiPort },
				{ u"error"_q, _runtimeApiServer->errorString() },
			});
		return false;
	}
	logEvent(
		u"runtime-api"_q,
		u"listening"_q,
		QJsonObject{
			{ u"port"_q, _runtimeApiPort },
			{ u"address"_q, listenAddress.toString() },
			{ u"advertisedAddress"_q, RuntimeApiAdvertisedHost() },
		});
	return true;
}

void Manager::stopRuntimeApiServer() {
	const auto sockets = _runtimeApiBuffers.keys();
	for (const auto socket : sockets) {
		if (socket) {
			socket->disconnectFromHost();
			socket->deleteLater();
		}
	}
	_runtimeApiBuffers.clear();
	if (_runtimeApiServer && _runtimeApiServer->isListening()) {
		_runtimeApiServer->close();
		logEvent(
			u"runtime-api"_q,
			u"stopped"_q,
			QJsonObject{
				{ u"port"_q, _runtimeApiPort },
			});
	}
}

void Manager::onRuntimeApiNewConnection() {
	if (!_runtimeApiServer) {
		return;
	}
	while (_runtimeApiServer->hasPendingConnections()) {
		const auto socket = _runtimeApiServer->nextPendingConnection();
		if (!socket) {
			continue;
		}
		_runtimeApiBuffers.insert(socket, QByteArray());
		QObject::connect(
			socket,
			&QTcpSocket::readyRead,
			this,
			[this, socket] { onRuntimeApiSocketReadyRead(socket); });
		QObject::connect(
			socket,
			&QTcpSocket::disconnected,
			this,
			[this, socket] {
				_runtimeApiBuffers.remove(socket);
				socket->deleteLater();
			});
	}
}

void Manager::onRuntimeApiSocketReadyRead(QTcpSocket *socket) {
	if (!socket) {
		return;
	}
	const auto it = _runtimeApiBuffers.find(socket);
	if (it == _runtimeApiBuffers.end()) {
		return;
	}
	auto &buffer = it.value();
	buffer += socket->readAll();
	QString method;
	QString target;
	QHash<QByteArray, QByteArray> headers;
	QByteArray body;
	qsizetype consumed = 0;
	QString error;
	if (!ParseRuntimeHttpRequest(
			buffer,
			method,
			target,
			headers,
			body,
			consumed,
			error)) {
		if (error == u"incomplete"_q) {
			return;
		}
		socket->write(RuntimeErrorResponse(400, error));
		socket->disconnectFromHost();
		_runtimeApiBuffers.remove(socket);
		return;
	}
	buffer.remove(0, consumed);
	auto disableAfterResponse = false;
	Logs::writeClient(QString::fromLatin1(
		"[runtime-api] request %1 %2 bodyBytes=%3")
		.arg(method, target)
		.arg(body.size()));
	logEvent(
		u"runtime-api"_q,
		u"request"_q,
		QJsonObject{
			{ u"method"_q, method },
			{ u"target"_q, target },
			{ u"bodyBytes"_q, body.size() },
			{ u"peerAddress"_q, socket->peerAddress().toString() },
			{ u"peerPort"_q, socket->peerPort() },
		});
	const auto response = processRuntimeApiRequest(
		method,
		target,
		body,
		disableAfterResponse);
	Logs::writeClient(QString::fromLatin1(
		"[runtime-api] response %1 %2 bytes=%3 disableAfterResponse=%4")
		.arg(method, target)
		.arg(response.size())
		.arg(disableAfterResponse ? u"true"_q : u"false"_q));
	logEvent(
		u"runtime-api"_q,
		u"response"_q,
		QJsonObject{
			{ u"method"_q, method },
			{ u"target"_q, target },
			{ u"responseBytes"_q, response.size() },
			{ u"disableAfterResponse"_q, disableAfterResponse },
		});
	socket->write(response);
	socket->disconnectFromHost();
	_runtimeApiBuffers.remove(socket);
	if (disableAfterResponse) {
		QTimer::singleShot(0, this, [=] {
			setRuntimeApiEnabled(false);
		});
	}
}

QByteArray Manager::processRuntimeApiRequest(
		const QString &method,
		const QString &target,
		const QByteArray &body,
		bool &disableRuntimeApiAfterResponse) {
	disableRuntimeApiAfterResponse = false;
	if (method.isEmpty() || target.isEmpty()) {
		return RuntimeErrorResponse(400, u"invalid request"_q);
	}
	const auto url = QUrl(u"http://runtime.local"_q + target);
	const auto path = url.path();
	const auto query = QUrlQuery(url);
	const auto resolvedMethod = method.trimmed().toUpper();

	const auto session = activeSession();
	if (!session
		&& path != u"/"_q
		&& path != u"/v1"_q
		&& path != u"/v1/health"_q
		&& path != u"/v1/system"_q
		&& path != u"/v1/runtime"_q
		&& path != u"/v1/host"_q) {
		return RuntimeErrorResponse(503, u"no active telegram session"_q);
	}

	auto parseBodyObject = [&]() -> QJsonObject {
		if (body.trimmed().isEmpty()) {
			return QJsonObject();
		}
		const auto doc = QJsonDocument::fromJson(body);
		return doc.isObject() ? doc.object() : QJsonObject();
	};
	auto runtimeSettingsPageJson = [&](const SettingsPageEntry &entry) {
		auto object = settingsPageDescriptorToJson(entry.descriptor);
		auto settingsCount = 0;
		for (const auto &section : entry.descriptor.sections) {
			settingsCount += section.settings.size();
		}
		object.insert(u"pageId"_q, QString::number(entry.id));
		object.insert(u"pluginId"_q, entry.pluginId);
		object.insert(u"settingsCount"_q, settingsCount);
		return object;
	};
	auto runtimeSettingUpdate = [&](SettingsPageId pageId,
			const QString &settingId,
			const QJsonObject &object,
			SettingDescriptor &updated,
			QString &failure) {
		const auto page = _settingsPages.find(pageId);
		if (page == _settingsPages.end()) {
			failure = u"settings page not found"_q;
			return false;
		}
		auto found = false;
		for (const auto &section : page->descriptor.sections) {
			for (const auto &candidate : section.settings) {
				if (candidate.id == settingId) {
					updated = candidate;
					found = true;
					break;
				}
			}
			if (found) {
				break;
			}
		}
		if (!found) {
			failure = u"setting not found"_q;
			return false;
		}
		switch (updated.type) {
		case SettingControl::Toggle:
			if (object.value(u"value"_q).isBool()) {
				updated.boolValue = object.value(u"value"_q).toBool();
			} else if (object.value(u"boolValue"_q).isBool()) {
				updated.boolValue = object.value(u"boolValue"_q).toBool();
			} else {
				failure = u"body.value bool is required"_q;
				return false;
			}
			break;
		case SettingControl::IntSlider:
			if (object.value(u"value"_q).isDouble()) {
				updated.intValue = object.value(u"value"_q).toInt();
			} else if (object.value(u"intValue"_q).isDouble()) {
				updated.intValue = object.value(u"intValue"_q).toInt();
			} else {
				failure = u"body.value int is required"_q;
				return false;
			}
			break;
		case SettingControl::TextInput:
			if (object.value(u"value"_q).isString()) {
				updated.textValue = object.value(u"value"_q).toString();
			} else if (object.value(u"textValue"_q).isString()) {
				updated.textValue = object.value(u"textValue"_q).toString();
			} else {
				failure = u"body.value string is required"_q;
				return false;
			}
			break;
		case SettingControl::ActionButton:
		case SettingControl::InfoText:
			failure = u"setting type is read-only"_q;
			return false;
		}
		return true;
	};
	auto runtimeLogPath = [&](QString source) {
		source = source.trimmed().toLower();
		if (source.isEmpty() || source == u"plugins"_q || source == u"plugin"_q) {
			return std::pair<QString, QString>(_logPath, u"plugins"_q);
		} else if (source == u"trace"_q) {
			return std::pair<QString, QString>(_tracePath, u"trace"_q);
		} else if (source == u"client"_q) {
			return std::pair<QString, QString>(ClientLogPath(), u"client"_q);
		}
		return std::pair<QString, QString>(QString(), QString());
	};

	if (resolvedMethod == u"GET"_q && (path == u"/"_q || path == u"/v1"_q)) {
		return RuntimeOkResponse(QJsonObject{
			{ u"name"_q, u"Astrogram Runtime API"_q },
			{ u"version"_q, 1 },
			{ u"baseUrl"_q, hostInfo().runtimeApiBaseUrl },
			{ u"endpoints"_q, QJsonArray{
				u"GET /v1/health"_q,
				u"GET /v1/host"_q,
				u"GET /v1/system"_q,
				u"GET /v1/plugins"_q,
				u"GET /v1/actions"_q,
				u"GET /v1/panels"_q,
				u"GET /v1/settings-pages"_q,
				u"POST /v1/plugins/reload"_q,
				u"POST /v1/actions/<id>/trigger"_q,
				u"POST /v1/panels/<id>/open"_q,
				u"POST /v1/plugins/<id>/enable"_q,
				u"POST /v1/plugins/<id>/disable"_q,
				u"GET /v1/plugins/<id>/actions"_q,
				u"GET /v1/plugins/<id>/panels"_q,
				u"GET /v1/plugins/<id>/settings-pages"_q,
				u"DELETE /v1/plugins/<id>"_q,
				u"GET /v1/logs?source=plugins&lines=200"_q,
				u"POST /v1/runtime"_q,
				u"POST /v1/safe-mode"_q,
				u"POST /v1/settings-pages/<pageId>/settings/<settingId>"_q,
				u"GET /v1/chats?limit=100"_q,
				u"GET /v1/chats/<peerId>/messages?limit=50"_q,
				u"POST /v1/messages/send"_q,
			} },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/health"_q) {
		return RuntimeOkResponse(QJsonObject{
			{ u"status"_q, u"ok"_q },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
			{ u"runtimeApiHost"_q, RuntimeApiAdvertisedHost() },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/host"_q) {
		const auto host = hostInfo();
		return RuntimeOkResponse(QJsonObject{
			{ u"compiler"_q, host.compiler },
			{ u"platform"_q, host.platform },
			{ u"workingPath"_q, host.workingPath },
			{ u"pluginsPath"_q, host.pluginsPath },
			{ u"appUiLanguage"_q, host.appUiLanguage },
			{ u"safeModeEnabled"_q, host.safeModeEnabled },
			{ u"runtimeApiEnabled"_q, host.runtimeApiEnabled },
			{ u"runtimeApiPort"_q, host.runtimeApiPort },
			{ u"runtimeApiBaseUrl"_q, host.runtimeApiBaseUrl },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/system"_q) {
		const auto system = systemInfo();
		return RuntimeOkResponse(QJsonObject{
			{ u"processId"_q, QString::number(system.processId) },
			{ u"logicalCpuCores"_q, system.logicalCpuCores },
			{ u"physicalCpuCores"_q, system.physicalCpuCores },
			{ u"productType"_q, system.productType },
			{ u"productVersion"_q, system.productVersion },
			{ u"prettyProductName"_q, system.prettyProductName },
			{ u"kernelType"_q, system.kernelType },
			{ u"kernelVersion"_q, system.kernelVersion },
			{ u"architecture"_q, system.architecture },
			{ u"buildAbi"_q, system.buildAbi },
			{ u"hostName"_q, system.hostName },
			{ u"userName"_q, system.userName },
			{ u"locale"_q, system.locale },
			{ u"uiLanguage"_q, system.uiLanguage },
			{ u"timeZone"_q, system.timeZone },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/plugins"_q) {
		auto list = QJsonArray();
		for (const auto &state : plugins()) {
			auto plugin = RuntimePluginJson(state);
			plugin.insert(
				u"actionsCount"_q,
				int(actionsFor(state.info.id).size()));
			plugin.insert(
				u"settingsPagesCount"_q,
				int(settingsPagesFor(state.info.id).size()));
			list.push_back(plugin);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"plugins"_q, list },
			{ u"count"_q, list.size() },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/actions"_q) {
		auto ids = _actions.keys();
		std::sort(ids.begin(), ids.end());
		auto list = QJsonArray();
		for (const auto id : ids) {
			const auto &entry = _actions[id];
			list.push_back(QJsonObject{
				{ u"id"_q, int(id) },
				{ u"pluginId"_q, entry.pluginId },
				{ u"title"_q, entry.title },
				{ u"description"_q, entry.description },
			});
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"actions"_q, list },
			{ u"count"_q, list.size() },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/panels"_q) {
		auto ids = _panels.keys();
		std::sort(ids.begin(), ids.end());
		auto list = QJsonArray();
		for (const auto id : ids) {
			const auto &entry = _panels[id];
			list.push_back(QJsonObject{
				{ u"id"_q, int(id) },
				{ u"pluginId"_q, entry.pluginId },
				{ u"title"_q, entry.descriptor.title },
				{ u"description"_q, entry.descriptor.description },
			});
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"panels"_q, list },
			{ u"count"_q, list.size() },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/settings-pages"_q) {
		auto ids = _settingsPages.keys();
		std::sort(ids.begin(), ids.end());
		auto list = QJsonArray();
		for (const auto id : ids) {
			list.push_back(runtimeSettingsPageJson(_settingsPages[id]));
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"settingsPages"_q, list },
			{ u"count"_q, list.size() },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/plugins/reload"_q) {
		reload();
		return RuntimeOkResponse(QJsonObject{
			{ u"message"_q, u"plugins reloaded"_q },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/logs"_q) {
		auto lines = std::max(1, query.queryItemValue(u"lines"_q).toInt());
		lines = std::min(lines, 5000);
		const auto [pathForLogs, sourceForLogs] = runtimeLogPath(
			query.queryItemValue(u"source"_q));
		if (pathForLogs.isEmpty()) {
			return RuntimeErrorResponse(400, u"unsupported log source"_q);
		}
		auto array = QJsonArray();
		for (const auto &line : RuntimeTailLines(pathForLogs, lines)) {
			array.push_back(line);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"lines"_q, array },
			{ u"count"_q, array.size() },
			{ u"source"_q, sourceForLogs },
			{ u"path"_q, pathForLogs },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/safe-mode"_q) {
		const auto object = parseBodyObject();
		if (!object.contains(u"enabled"_q) || !object.value(u"enabled"_q).isBool()) {
			return RuntimeErrorResponse(400, u"body.enabled bool is required"_q);
		}
		const auto enabled = object.value(u"enabled"_q).toBool();
		if (!setSafeModeEnabled(enabled)) {
			return RuntimeErrorResponse(500, u"failed to change safe mode"_q);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"safeModeEnabled"_q, enabled },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/runtime"_q) {
		const auto object = parseBodyObject();
		if (!object.contains(u"enabled"_q) || !object.value(u"enabled"_q).isBool()) {
			return RuntimeErrorResponse(400, u"body.enabled bool is required"_q);
		}
		const auto enabled = object.value(u"enabled"_q).toBool();
		if (object.value(u"port"_q).isDouble()) {
			const auto port = object.value(u"port"_q).toInt();
			if (port > 0 && port <= 65535) {
				_runtimeApiPort = port;
			}
		}
		if (enabled) {
			if (!setRuntimeApiEnabled(true)) {
				return RuntimeErrorResponse(500, u"failed to enable runtime api"_q);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"runtimeApiEnabled"_q, true },
				{ u"runtimeApiPort"_q, _runtimeApiPort },
				{ u"runtimeApiBaseUrl"_q, hostInfo().runtimeApiBaseUrl },
			});
		}
		disableRuntimeApiAfterResponse = true;
		return RuntimeOkResponse(QJsonObject{
			{ u"runtimeApiEnabled"_q, false },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
			{ u"message"_q, u"runtime api will stop after this response"_q },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/chats"_q) {
		auto limit = query.queryItemValue(u"limit"_q).toInt();
		if (limit <= 0) {
			limit = 100;
		}
		limit = std::min(limit, 1000);
		auto result = QJsonArray();
		auto count = 0;
		const auto &list = session->data().chatsList()->indexed()->all();
		for (const auto row : list) {
			if (!row || !row->history()) {
				continue;
			}
			const auto history = row->history();
			const auto peer = history->peer.get();
			auto chat = QJsonObject{
				{ u"peerId"_q, QString::number(peer->id.value) },
				{ u"type"_q, peerIsUser(peer->id)
					? u"user"_q
					: (peerIsChannel(peer->id) ? u"channel"_q : u"chat"_q) },
				{ u"name"_q, peer->name() },
				{ u"username"_q, peer->username() },
				{ u"muted"_q, history->muted() },
				{ u"unreadCount"_q, history->unreadCountKnown()
					? history->unreadCount()
					: -1 },
			};
			const auto last = history->lastMessage();
			if (last) {
				chat.insert(u"lastMessage"_q, RuntimeMessageJson(last, peer->id));
			}
			result.push_back(chat);
			++count;
			if (count >= limit) {
				break;
			}
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"count"_q, result.size() },
			{ u"chats"_q, result },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/messages/send"_q) {
		const auto object = parseBodyObject();
		const auto peerText = object.value(u"peerId"_q).toString().trimmed();
		const auto text = object.value(u"text"_q).toString();
		if (peerText.isEmpty()) {
			return RuntimeErrorResponse(400, u"body.peerId string is required"_q);
		}
		if (text.trimmed().isEmpty()) {
			return RuntimeErrorResponse(400, u"body.text string is required"_q);
		}
		const auto peerId = RuntimePeerIdFromString(peerText);
		if (!peerId) {
			return RuntimeErrorResponse(400, u"invalid peerId"_q);
		}
		auto history = session->data().history(*peerId);
		auto message = Api::MessageToSend(Api::SendAction(history));
		message.textWithTags.text = text;
		session->api().sendMessage(std::move(message));
		return RuntimeOkResponse(QJsonObject{
			{ u"peerId"_q, QString::number(peerId->value) },
			{ u"sent"_q, true },
		});
	}

	const auto prefix = u"/v1/plugins/"_q;
	if (path.startsWith(prefix)) {
		auto rest = path.mid(prefix.size());
		if (resolvedMethod == u"GET"_q
			&& (rest.endsWith(u"/actions"_q)
				|| rest.endsWith(u"/panels"_q)
				|| rest.endsWith(u"/settings-pages"_q))) {
			auto suffix = QString();
			if (rest.endsWith(u"/actions"_q)) {
				suffix = u"/actions"_q;
			} else if (rest.endsWith(u"/panels"_q)) {
				suffix = u"/panels"_q;
			} else {
				suffix = u"/settings-pages"_q;
			}
			rest.chop(suffix.size());
			const auto id = QUrl::fromPercentEncoding(rest.toUtf8());
			if (id.isEmpty()) {
				return RuntimeErrorResponse(400, u"plugin id is required"_q);
			}
			if (suffix == u"/actions"_q) {
				auto list = QJsonArray();
				for (const auto &action : actionsFor(id)) {
					list.push_back(QJsonObject{
						{ u"id"_q, QString::number(action.id) },
						{ u"title"_q, action.title },
						{ u"description"_q, action.description },
					});
				}
				return RuntimeOkResponse(QJsonObject{
					{ u"pluginId"_q, id },
					{ u"actions"_q, list },
					{ u"count"_q, list.size() },
				});
			} else if (suffix == u"/panels"_q) {
				auto list = QJsonArray();
				for (const auto &panel : panelsFor(id)) {
					list.push_back(QJsonObject{
						{ u"id"_q, QString::number(panel.id) },
						{ u"title"_q, panel.title },
						{ u"description"_q, panel.description },
					});
				}
				return RuntimeOkResponse(QJsonObject{
					{ u"pluginId"_q, id },
					{ u"panels"_q, list },
					{ u"count"_q, list.size() },
				});
			}
			auto list = QJsonArray();
			for (const auto &page : settingsPagesFor(id)) {
				auto descriptor = SettingsPageDescriptor();
				descriptor.id = QString::number(page.id);
				descriptor.title = page.title;
				descriptor.description = page.description;
				descriptor.sections = page.sections;
				auto object = settingsPageDescriptorToJson(descriptor);
				auto settingsCount = 0;
				for (const auto &section : page.sections) {
					settingsCount += section.settings.size();
				}
				object.insert(u"pageId"_q, QString::number(page.id));
				object.insert(u"pluginId"_q, id);
				object.insert(u"settingsCount"_q, settingsCount);
				list.push_back(object);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"pluginId"_q, id },
				{ u"settingsPages"_q, list },
				{ u"count"_q, list.size() },
			});
		}
		if (rest.endsWith(u"/enable"_q) || rest.endsWith(u"/disable"_q)) {
			if (resolvedMethod != u"POST"_q) {
				return RuntimeErrorResponse(405, u"method not allowed"_q);
			}
			const auto enable = rest.endsWith(u"/enable"_q);
			rest.chop(enable ? 7 : 8);
			const auto id = QUrl::fromPercentEncoding(rest.toUtf8());
			if (id.isEmpty()) {
				return RuntimeErrorResponse(400, u"plugin id is required"_q);
			}
			if (!setEnabled(id, enable)) {
				return RuntimeErrorResponse(404, u"plugin not found or state unchanged"_q);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"id"_q, id },
				{ u"enabled"_q, enable },
			});
		}
		if (resolvedMethod == u"DELETE"_q) {
			const auto id = QUrl::fromPercentEncoding(rest.toUtf8());
			if (id.isEmpty()) {
				return RuntimeErrorResponse(400, u"plugin id is required"_q);
			}
			QString error;
			if (!removePlugin(id, &error)) {
				return RuntimeErrorResponse(
					409,
					error.isEmpty() ? u"failed to remove plugin"_q : error);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"id"_q, id },
				{ u"removed"_q, true },
			});
		}
	}
	const auto settingPrefix = u"/v1/settings-pages/"_q;
	if (resolvedMethod == u"POST"_q
		&& path.startsWith(settingPrefix)
		&& path.contains(u"/settings/"_q)) {
		auto rest = path.mid(settingPrefix.size());
		const auto split = rest.indexOf(u"/settings/"_q);
		if (split <= 0) {
			return RuntimeErrorResponse(400, u"invalid settings endpoint"_q);
		}
		const auto pageText = rest.left(split);
		const auto settingText = rest.mid(split + QString(u"/settings/"_q).size());
		auto ok = false;
		const auto pageId = pageText.toULongLong(&ok);
		if (!ok || !pageId) {
			return RuntimeErrorResponse(400, u"settings page id is required"_q);
		}
		const auto settingId = QUrl::fromPercentEncoding(settingText.toUtf8());
		if (settingId.isEmpty()) {
			return RuntimeErrorResponse(400, u"setting id is required"_q);
		}
		auto updated = SettingDescriptor();
		auto failure = QString();
		if (!runtimeSettingUpdate(
				SettingsPageId(pageId),
				settingId,
				parseBodyObject(),
				updated,
				failure)) {
			return RuntimeErrorResponse(
				400,
				failure.isEmpty() ? u"invalid setting update"_q : failure);
		}
		if (!updateSetting(SettingsPageId(pageId), updated)) {
			return RuntimeErrorResponse(409, u"failed to update setting"_q);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"pageId"_q, QString::number(pageId) },
			{ u"setting"_q, settingDescriptorToJson(updated) },
		});
	}
	const auto actionPrefix = u"/v1/actions/"_q;
	if (resolvedMethod == u"POST"_q
		&& path.startsWith(actionPrefix)
		&& path.endsWith(u"/trigger"_q)) {
		auto rest = path.mid(actionPrefix.size());
		rest.chop(QString(u"/trigger"_q).size());
		auto ok = false;
		const auto id = rest.toULongLong(&ok);
		if (!ok || !id) {
			return RuntimeErrorResponse(400, u"action id is required"_q);
		}
		if (!triggerAction(ActionId(id))) {
			return RuntimeErrorResponse(404, u"action not found or trigger failed"_q);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"triggered"_q, true },
		});
	}
	const auto panelPrefix = u"/v1/panels/"_q;
	if (resolvedMethod == u"POST"_q
		&& path.startsWith(panelPrefix)
		&& path.endsWith(u"/open"_q)) {
		auto rest = path.mid(panelPrefix.size());
		rest.chop(QString(u"/open"_q).size());
		auto ok = false;
		const auto id = rest.toULongLong(&ok);
		if (!ok || !id) {
			return RuntimeErrorResponse(400, u"panel id is required"_q);
		}
		if (!openPanel(PanelId(id))) {
			return RuntimeErrorResponse(404, u"panel not found or open failed"_q);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"opened"_q, true },
		});
	}
	const auto chatPrefix = u"/v1/chats/"_q;
	if (resolvedMethod == u"GET"_q
		&& path.startsWith(chatPrefix)
		&& path.endsWith(u"/messages"_q)) {
		auto rest = path.mid(chatPrefix.size());
		rest.chop(QString(u"/messages"_q).size());
		const auto peerId = RuntimePeerIdFromString(rest);
		if (!peerId) {
			return RuntimeErrorResponse(400, u"invalid chat peer id"_q);
		}
		auto limit = query.queryItemValue(u"limit"_q).toInt();
		if (limit <= 0) {
			limit = 50;
		}
		limit = std::min(limit, 200);
		const auto history = session->data().historyLoaded(*peerId);
		if (!history) {
			return RuntimeOkResponse(QJsonObject{
				{ u"peerId"_q, QString::number(peerId->value) },
				{ u"loaded"_q, false },
				{ u"messages"_q, QJsonArray() },
				{ u"count"_q, 0 },
			});
		}
		const auto lastMessage = history->lastMessage();
		if (!lastMessage) {
			return RuntimeOkResponse(QJsonObject{
				{ u"peerId"_q, QString::number(peerId->value) },
				{ u"loaded"_q, true },
				{ u"messages"_q, QJsonArray() },
				{ u"count"_q, 0 },
			});
		}
		const auto snapshot = history->messages().snapshot(
			Storage::SparseIdsListQuery(lastMessage->id, limit, 0));
		auto ids = std::vector<MsgId>();
		ids.reserve(snapshot.messageIds.size());
		for (const auto id : snapshot.messageIds) {
			ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end(), std::greater<MsgId>());
		if (int(ids.size()) > limit) {
			ids.resize(limit);
		}
		auto array = QJsonArray();
		for (const auto msgId : ids) {
			const auto item = session->data().message(*peerId, msgId);
			if (!item) {
				continue;
			}
			array.push_back(RuntimeMessageJson(item, *peerId));
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"peerId"_q, QString::number(peerId->value) },
			{ u"loaded"_q, true },
			{ u"messages"_q, array },
			{ u"count"_q, array.size() },
		});
	}

	return RuntimeErrorResponse(404, u"endpoint not found"_q);
}

PackagePreviewState Manager::inspectPackage(const QString &path) const {
	auto result = PackagePreviewState();
	result.sourcePath = path;
	result.info.id = PluginBaseName(path);
	result.info.name = result.info.id;
	result.compatible = false;

	const auto fileInfo = QFileInfo(path);
	if (!fileInfo.exists() || !fileInfo.isFile()) {
		result.error = u"Plugin package file was not found."_q;
		logEvent(
			u"package"_q,
			u"inspect-failed"_q,
			QJsonObject{
				{ u"reason"_q, result.error },
				{ u"file"_q, fileInfoToJson(fileInfo) },
			});
		return result;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		result.error = u"Could not read the plugin package."_q;
		logEvent(
			u"package"_q,
			u"inspect-failed"_q,
			QJsonObject{
				{ u"reason"_q, result.error },
				{ u"file"_q, fileInfoToJson(fileInfo) },
			});
		return result;
	}
	if (file.size() <= 0) {
		result.error = u"Plugin package is empty."_q;
		logEvent(
			u"package"_q,
			u"inspect-failed"_q,
			QJsonObject{
				{ u"reason"_q, result.error },
				{ u"file"_q, fileInfoToJson(fileInfo) },
			});
		return result;
	}
	file.close();
	auto previewInfo = PluginInfo();
	auto previewIcon = QString();
	if (ReadPreviewManifest(path, &previewInfo, &previewIcon)) {
		result.previewAvailable = true;
		MergePluginInfo(result.info, previewInfo);
		result.icon = previewIcon;
	}
	const auto loadability = CheckPackageLoadability(path);
	if (loadability.previewExportValid) {
		result.previewAvailable = true;
		MergePluginInfo(result.info, loadability.previewInfo);
		if (result.icon.isEmpty()) {
			result.icon = loadability.previewIcon;
		}
	}
	result.compatible = loadability.compatible;
	result.error = loadability.error;

	if (result.info.id.isEmpty()) {
		result.info.id = PluginBaseName(path);
	}
	if (result.info.name.isEmpty()) {
		result.info.name = result.info.id;
	}
	if (!result.info.id.isEmpty()) {
		if (const auto installed = findRecord(result.info.id)) {
			result.installed = true;
			result.update = true;
			result.installedVersion = installed->state.info.version;
			result.installedPath = installed->state.path;
		}
	}
	logEvent(
		u"package"_q,
		u"inspect"_q,
		QJsonObject{
			{ u"file"_q, fileInfoToJson(fileInfo) },
			{ u"sha256"_q, fileSha256(path) },
			{ u"previewAvailable"_q, result.previewAvailable },
			{ u"compatible"_q, result.compatible },
			{ u"installed"_q, result.installed },
			{ u"update"_q, result.update },
			{ u"installedVersion"_q, result.installedVersion },
			{ u"installedPath"_q, result.installedPath },
			{ u"icon"_q, result.icon },
			{ u"plugin"_q, pluginInfoToJson(result.info) },
			{ u"error"_q, result.error },
			{ u"loadability"_q, QJsonObject{
				{ u"libraryLoaded"_q, loadability.libraryLoaded },
				{ u"previewExportFound"_q, loadability.previewExportFound },
				{ u"previewExportValid"_q, loadability.previewExportValid },
				{ u"binaryInfoFound"_q, loadability.binaryInfoFound },
				{ u"entryFound"_q, loadability.entryFound },
				{ u"binarySummary"_q, loadability.binarySummary },
				{ u"mismatchField"_q, loadability.mismatchField },
				{ u"error"_q, loadability.error },
			}},
		});
	return result;
}

bool Manager::installPackage(const QString &sourcePath, QString *error) {
	const auto preview = inspectPackage(sourcePath);
	if (!preview.compatible) {
		if (error) {
			*error = preview.error.isEmpty()
				? u"Plugin package is incompatible."_q
				: preview.error;
		}
		logEvent(
			u"package"_q,
			u"install-rejected"_q,
			QJsonObject{
				{ u"sourcePath"_q, sourcePath },
				{ u"reason"_q, error ? *error : preview.error },
				{ u"plugin"_q, pluginInfoToJson(preview.info) },
			});
		return false;
	}

	QDir().mkpath(_pluginsPath);
	const auto sourceInfo = QFileInfo(sourcePath);
	const auto fileStem = !preview.info.id.isEmpty()
		? SanitizedPluginFileStem(preview.info.id)
		: SanitizedPluginFileStem(sourceInfo.completeBaseName());
	const auto targetPath = QDir(_pluginsPath).absoluteFilePath(
		fileStem + u".tgd"_q);
	const auto tempPath = targetPath + u".tmp"_q;
	const auto rollbackBackupPath = targetPath + u".rollback"_q;
	const auto rescanNow = [this] {
		if (safeModeEnabled()) {
			logEvent(u"safe-mode"_q, u"install-rescan-metadata-only"_q);
			scanPlugins(true);
		} else {
			scanPlugins();
		}
	};
	startRecoveryOperation(
			preview.update ? u"update"_q : u"install"_q,
			preview.info.id.isEmpty() ? QStringList() : QStringList{ preview.info.id },
			preview.info.name.isEmpty() ? targetPath : preview.info.name);
	logEvent(
		u"package"_q,
		u"install-start"_q,
		QJsonObject{
			{ u"sourcePath"_q, sourcePath },
			{ u"targetPath"_q, targetPath },
			{ u"tempPath"_q, tempPath },
			{ u"sourceSha256"_q, fileSha256(sourcePath) },
			{ u"source"_q, fileInfoToJson(sourceInfo) },
			{ u"plugin"_q, pluginInfoToJson(preview.info) },
			{ u"update"_q, preview.update },
			{ u"installedPath"_q, preview.installedPath },
		});

	unloadAll();
	loadConfig();
	loadRecoveryState();

	QFile::remove(tempPath);
	QFile::remove(rollbackBackupPath);

	auto rollbackRestorePath = QString();
	auto rollbackBackupCreated = false;
	const auto restoreRollbackBackup = [&](const QString &reason) {
		QFile::remove(tempPath);
		if (QFileInfo::exists(targetPath)
			&& QFileInfo(targetPath).absoluteFilePath() != sourceInfo.absoluteFilePath()) {
			QFile::remove(targetPath);
		}
		if (!rollbackBackupCreated) {
			logEvent(
				u"package"_q,
				u"rollback-skipped"_q,
				QJsonObject{
					{ u"sourcePath"_q, sourcePath },
					{ u"targetPath"_q, targetPath },
					{ u"reason"_q, reason },
				});
			return;
		}
		if (QFileInfo::exists(rollbackRestorePath)) {
			QFile::remove(rollbackRestorePath);
		}
		if (!QFile::rename(rollbackBackupPath, rollbackRestorePath)) {
			logEvent(
				u"package"_q,
				u"rollback-restore-failed"_q,
				QJsonObject{
					{ u"backupPath"_q, rollbackBackupPath },
					{ u"restorePath"_q, rollbackRestorePath },
					{ u"reason"_q, reason },
				});
			return;
		}
		logEvent(
			u"package"_q,
			u"rollback-restored"_q,
			QJsonObject{
				{ u"backupPath"_q, rollbackBackupPath },
				{ u"restorePath"_q, rollbackRestorePath },
				{ u"reason"_q, reason },
			});
	};
	const auto rollbackInstalledPackage = [&](const QString &reason) {
		logEvent(
			u"package"_q,
			u"rollback-start"_q,
			QJsonObject{
				{ u"sourcePath"_q, sourcePath },
				{ u"targetPath"_q, targetPath },
				{ u"backupPath"_q, rollbackBackupPath },
				{ u"restorePath"_q, rollbackRestorePath },
				{ u"reason"_q, reason },
			});
		unloadAll();
		restoreRollbackBackup(reason);
		loadConfig();
		loadRecoveryState();
		rescanNow();
	};

	if (sourceInfo.absoluteFilePath() != targetPath) {
		if (!QFile::copy(sourcePath, tempPath)) {
			logEvent(
				u"package"_q,
				u"copy-failed"_q,
				QJsonObject{
					{ u"sourcePath"_q, sourcePath },
					{ u"tempPath"_q, tempPath },
				});
			rescanNow();
			if (error) {
				*error = u"Could not copy the plugin package into tdata/plugins."_q;
			}
			finishRecoveryOperation();
			return false;
		}
		const auto existingPathToProtect = (!preview.installedPath.isEmpty()
			&& QFileInfo::exists(preview.installedPath))
			? QFileInfo(preview.installedPath).absoluteFilePath()
			: (QFileInfo::exists(targetPath)
				? QFileInfo(targetPath).absoluteFilePath()
				: QString());
		if (!existingPathToProtect.isEmpty()) {
			rollbackRestorePath = existingPathToProtect;
			if (!QFile::rename(existingPathToProtect, rollbackBackupPath)) {
				logEvent(
					u"package"_q,
					u"backup-existing-failed"_q,
					QJsonObject{
						{ u"existingPath"_q, existingPathToProtect },
						{ u"backupPath"_q, rollbackBackupPath },
					});
				QFile::remove(tempPath);
				rescanNow();
				if (error) {
					*error = u"Could not prepare rollback for the previous plugin file."_q;
				}
				finishRecoveryOperation();
				return false;
			}
			rollbackBackupCreated = true;
		}
		if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath)) {
			logEvent(
				u"package"_q,
				u"overwrite-remove-failed"_q,
				QJsonObject{
					{ u"targetPath"_q, targetPath },
				});
			restoreRollbackBackup(u"overwrite-remove-failed"_q);
			rescanNow();
			if (error) {
				*error = u"Could not overwrite the target plugin file."_q;
			}
			finishRecoveryOperation();
			return false;
		}
		if (!QFile::rename(tempPath, targetPath)) {
			logEvent(
				u"package"_q,
				u"rename-failed"_q,
				QJsonObject{
					{ u"tempPath"_q, tempPath },
					{ u"targetPath"_q, targetPath },
				});
			restoreRollbackBackup(u"rename-failed"_q);
			rescanNow();
			if (error) {
				*error = u"Could not finalize the installed plugin file."_q;
			}
			finishRecoveryOperation();
			return false;
		}
	}

	if (!preview.info.id.isEmpty()) {
		_disabled.remove(preview.info.id);
		clearRecoveryDisabled(preview.info.id);
		saveConfig();
	}

	if (safeModeEnabled()) {
		logEvent(
			u"package"_q,
			u"installed-while-safe-mode"_q,
			QJsonObject{
				{ u"targetPath"_q, targetPath },
				{ u"plugin"_q, pluginInfoToJson(preview.info) },
			});
	} else {
		scanPlugins();
		if (!preview.info.id.isEmpty()) {
			if (const auto record = findRecord(preview.info.id);
				record && !record->state.error.trimmed().isEmpty()) {
				logEvent(
					u"package"_q,
					u"install-load-failed"_q,
					QJsonObject{
						{ u"targetPath"_q, targetPath },
						{ u"plugin"_q, pluginInfoToJson(preview.info) },
						{ u"loadError"_q, record->state.error.trimmed() },
					});
				rollbackInstalledPackage(record->state.error.trimmed());
				if (error) {
					*error = record->state.error.trimmed();
				}
				finishRecoveryOperation();
				return false;
			}
		}
	}
	finishRecoveryOperation();
	QFile::remove(rollbackBackupPath);
	if (error) {
		error->clear();
	}
	logEvent(
		u"package"_q,
		u"install-finished"_q,
		QJsonObject{
			{ u"targetPath"_q, targetPath },
			{ u"targetSha256"_q, fileSha256(targetPath) },
			{ u"plugin"_q, pluginInfoToJson(preview.info) },
			{ u"safeMode"_q, safeModeEnabled() },
		});
	return true;
}

bool Manager::removePlugin(const QString &pluginId, QString *error) {
	Logs::writeClient(QString::fromLatin1("[plugins] remove requested: %1").arg(pluginId));
	const auto index = findRecordIndex(pluginId);
	if (index < 0) {
		if (error) {
			*error = u"Plugin was not found or plugin id is ambiguous."_q;
		}
		logEvent(
			u"package"_q,
			u"remove-missing"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
			});
		return false;
	}

	const auto &record = _plugins[index];
	const auto pluginInfo = record.state.info;
	const auto pluginPath = record.state.path;
	const auto restoreIndex = index;
	const auto wasEnabled = record.state.enabled;

	startRecoveryOperation(
		u"remove"_q,
		{ pluginId },
		pluginInfo.name.isEmpty() ? pluginId : pluginInfo.name);
	logEvent(
		u"package"_q,
		u"remove-start"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"path"_q, pluginPath },
			{ u"plugin"_q, pluginInfoToJson(pluginInfo) },
		});

	unloadPluginRecord(_plugins[restoreIndex], true);
	auto removedPath = pluginPath;
	auto scheduledOnReboot = false;
	auto removeError = QString();
	const auto removed = removePluginFileReliable(
		pluginPath,
		&removedPath,
		&removeError,
		&scheduledOnReboot);
	if (!removed) {
		Logs::writeClient(QString::fromLatin1(
			"[plugins] remove failed: %1 reason=%2")
			.arg(pluginId, removeError));
		logEvent(
			u"package"_q,
			u"remove-file-failed"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"path"_q, pluginPath },
				{ u"failedPath"_q, removedPath },
				{ u"reason"_q, removeError },
			});
		if (wasEnabled && QFileInfo::exists(pluginPath)) {
			_plugins.erase(_plugins.begin() + restoreIndex);
			rebuildPluginIndex();
			loadPlugin(pluginPath);
			moveLastPluginRecordToIndex(restoreIndex);
			Logs::writeClient(QString::fromLatin1(
				"[plugins] remove rollback reload: %1")
				.arg(pluginId));
		} else if (restoreIndex >= 0 && restoreIndex < int(_plugins.size())) {
			auto &failedRecord = _plugins[restoreIndex];
			failedRecord.state.enabled = wasEnabled;
			failedRecord.state.loaded = false;
			if (!removeError.trimmed().isEmpty()) {
				failedRecord.state.error = removeError.trimmed();
			}
		}
		if (error) {
			*error = removeError.isEmpty()
				? u"Could not delete the plugin package file."_q
				: removeError;
		}
		finishRecoveryOperation();
		return false;
	}

	_disabled.remove(pluginId);
	_disabledByRecovery.remove(pluginId);
	_storedSettings.remove(pluginId);
	if (_recoveryNotice.active) {
		_recoveryNotice.pluginIds.removeAll(pluginId);
		if (_recoveryNotice.pluginIds.isEmpty()) {
			_recoveryNotice = RecoveryOperationState();
		}
	}
	saveConfig();
	saveRecoveryState();
	_plugins.erase(_plugins.begin() + restoreIndex);
	rebuildPluginIndex();
	Logs::writeClient(QString::fromLatin1("[plugins] remove finished: %1").arg(pluginId));
	finishRecoveryOperation();

	if (error) {
		error->clear();
	}
	logEvent(
		u"package"_q,
		u"remove-finished"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"path"_q, pluginPath },
			{ u"removedPath"_q, removedPath },
			{ u"scheduledOnReboot"_q, scheduledOnReboot },
			{ u"plugin"_q, pluginInfoToJson(pluginInfo) },
		});
	return true;
}

std::vector<CommandDescriptor> Manager::commandsFor(
		const QString &pluginId) const {
	auto result = std::vector<CommandDescriptor>();
	const auto it = _commandsByPlugin.find(pluginId);
	if (it == _commandsByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _commands.find(id);
		if (entry != _commands.end()) {
			result.push_back(entry->descriptor);
		}
	}
	return result;
}

std::vector<ActionState> Manager::actionsFor(const QString &pluginId) const {
	auto result = std::vector<ActionState>();
	const auto it = _actionsByPlugin.find(pluginId);
	if (it == _actionsByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _actions.find(id);
		if (entry != _actions.end()) {
			result.push_back({
				.id = entry->id,
				.title = entry->title,
				.description = entry->description,
			});
		}
	}
	return result;
}

std::vector<PanelState> Manager::panelsFor(const QString &pluginId) const {
	auto result = std::vector<PanelState>();
	const auto it = _panelsByPlugin.find(pluginId);
	if (it == _panelsByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _panels.find(id);
		if (entry != _panels.end()) {
			result.push_back({
				.id = entry->id,
				.title = entry->descriptor.title,
				.description = entry->descriptor.description,
			});
		}
	}
	return result;
}

std::vector<SettingsPageState> Manager::settingsPagesFor(
		const QString &pluginId) const {
	auto result = std::vector<SettingsPageState>();
	const auto it = _settingsPagesByPlugin.find(pluginId);
	if (it == _settingsPagesByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _settingsPages.find(id);
		if (entry != _settingsPages.end()) {
			result.push_back({
				.id = entry->id,
				.title = entry->descriptor.title,
				.description = entry->descriptor.description,
				.sections = entry->descriptor.sections,
			});
		}
	}
	return result;
}

bool Manager::triggerAction(ActionId id) {
	const auto it = _actions.find(id);
	if (it == _actions.end()) {
		logEvent(
			u"action"_q,
			u"missing"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
			});
		return false;
	}
	try {
		if (it->handlerWithContext) {
			startRecoveryOperation(u"action"_q, { it->pluginId }, it->title);
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = it->pluginId;
			auto context = ActionContext();
			context.window = activeWindow();
			if (!context.window) {
				context.window = Core::App().activePrimaryWindow();
			}
			if (context.window) {
				context.session = context.window->maybeSession();
			}
			if (!context.session) {
				context.session = activeSession();
			}
			logEvent(
				u"action"_q,
				u"invoke-context"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
					{ u"hasWindow"_q, context.window != nullptr },
					{ u"hasSession"_q, context.session != nullptr },
				});
			InvokePluginCallbackOrThrow([&] {
				it->handlerWithContext(context);
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"action"_q,
				u"success"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			finishRecoveryOperation();
			return true;
		}
		if (it->handler) {
			startRecoveryOperation(u"action"_q, { it->pluginId }, it->title);
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = it->pluginId;
			logEvent(
				u"action"_q,
				u"invoke"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			InvokePluginCallbackOrThrow([&] {
				it->handler();
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"action"_q,
				u"success"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			finishRecoveryOperation();
			return true;
		}
	} catch (...) {
		_registeringPluginId.clear();
		logEvent(
			u"action"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->title },
				{ u"reason"_q, CurrentExceptionText() },
			});
		disablePlugin(it->pluginId, u"Action failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin action failed and was disabled."_q,
			u"Действие плагина завершилось с ошибкой и было выключено."_q));
		finishRecoveryOperation();
	}
	return false;
}

bool Manager::openPanel(PanelId id) {
	const auto it = _panels.find(id);
	if (it == _panels.end()) {
		logEvent(
			u"panel"_q,
			u"missing"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
			});
		return false;
	}
	const auto window = activeWindow()
		? activeWindow()
		: Core::App().activePrimaryWindow();
	if (!window) {
		logEvent(
			u"panel"_q,
			u"no-active-window"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->descriptor.title },
			});
		showToast(u"No active window to show panel."_q);
		return false;
	}
	if (!it->handler) {
		logEvent(
			u"panel"_q,
			u"missing-handler"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
			});
		return false;
	}
	try {
		startRecoveryOperation(u"panel"_q, { it->pluginId }, it->descriptor.title);
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = it->pluginId;
		logEvent(
			u"panel"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"descriptor"_q, panelDescriptorToJson(it->descriptor) },
			});
		InvokePluginCallbackOrThrow([&] {
			it->handler(window);
		});
		_registeringPluginId = previousPluginId;
		logEvent(
			u"panel"_q,
			u"success"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->descriptor.title },
			});
		finishRecoveryOperation();
		return true;
	} catch (...) {
		_registeringPluginId.clear();
		logEvent(
			u"panel"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->descriptor.title },
				{ u"reason"_q, CurrentExceptionText() },
			});
		disablePlugin(it->pluginId, u"Panel failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin panel failed and was disabled."_q,
			u"Панель плагина завершилась с ошибкой и была выключена."_q));
		finishRecoveryOperation();
		return false;
	}
}

bool Manager::updateSetting(SettingsPageId id, SettingDescriptor setting) {
	const auto it = _settingsPages.find(id);
	if (it == _settingsPages.end()) {
		logEvent(
			u"settings"_q,
			u"missing-page"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
			});
		return false;
	}
	if (!it->handler) {
		logEvent(
			u"settings"_q,
			u"missing-handler"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
			});
		return false;
	}
	setting = NormalizeSettingDescriptor(std::move(setting));
	auto *target = (SettingDescriptor*)nullptr;
	for (auto &section : it->descriptor.sections) {
		for (auto &candidate : section.settings) {
			if (candidate.id == setting.id) {
				target = &candidate;
				break;
			}
		}
		if (target) {
			break;
		}
	}
	if (!target) {
		logEvent(
			u"settings"_q,
			u"missing-setting"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"settingId"_q, setting.id },
			});
		return false;
	}
	const auto previous = *target;
	switch (target->type) {
	case SettingControl::Toggle:
		target->boolValue = setting.boolValue;
		break;
	case SettingControl::IntSlider:
		target->intValue = std::clamp(
			setting.intValue,
			target->intMinimum,
			target->intMaximum);
		break;
	case SettingControl::TextInput:
		target->textValue = setting.textValue.trimmed();
		break;
	case SettingControl::ActionButton:
	case SettingControl::InfoText:
		break;
	}
	const auto snapshot = *target;
	rememberSettingValue(it->pluginId, snapshot);
	saveConfig();
	try {
		startRecoveryOperation(
			u"settings"_q,
			{ it->pluginId },
			snapshot.title.isEmpty() ? snapshot.id : snapshot.title);
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = it->pluginId;
		logEvent(
			u"settings"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"setting"_q, settingDescriptorToJson(snapshot) },
			});
		InvokePluginCallbackOrThrow([&] {
			it->handler(snapshot);
		});
		_registeringPluginId = previousPluginId;
		logEvent(
			u"settings"_q,
			u"success"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"settingId"_q, snapshot.id },
			});
		finishRecoveryOperation();
		return true;
	} catch (...) {
		*target = previous;
		rememberSettingValue(it->pluginId, previous);
		saveConfig();
		_registeringPluginId.clear();
		logEvent(
			u"settings"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"setting"_q, settingDescriptorToJson(snapshot) },
				{ u"reason"_q, CurrentExceptionText() },
			});
		disablePlugin(
			it->pluginId,
			u"Settings update failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin settings failed and plugin was disabled."_q,
			u"Настройки плагина завершились с ошибкой, плагин был выключен."_q));
		finishRecoveryOperation();
		return false;
	}
}

bool Manager::setEnabled(const QString &pluginId, bool enabled) {
	Logs::writeClient(QString::fromLatin1("[plugins] toggle requested: %1 -> %2")
		.arg(pluginId)
		.arg(enabled ? u"enabled"_q : u"disabled"_q));
	const auto index = findRecordIndex(pluginId);
	if (index < 0) {
		logEvent(
			u"plugin"_q,
			u"toggle-missing"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"enabled"_q, enabled },
			});
		return false;
	}
	const auto current = _plugins[index].state;
	const auto alreadyEnabled = current.enabled && current.loaded;
	const auto alreadyDisabled = !current.enabled && !current.loaded;
	if (enabled && alreadyEnabled && current.error.trimmed().isEmpty()) {
		logEvent(
			u"plugin"_q,
			u"toggle-noop"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"enabled"_q, enabled },
				{ u"reason"_q, u"already-enabled"_q },
			});
		return true;
	}
	if (!enabled && alreadyDisabled) {
		logEvent(
			u"plugin"_q,
			u"toggle-noop"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"enabled"_q, enabled },
				{ u"reason"_q, u"already-disabled"_q },
			});
		return true;
	}

	logEvent(
		u"plugin"_q,
		u"toggle"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"enabled"_q, enabled },
		});

	if (!enabled) {
		startRecoveryOperation(u"disable"_q, { pluginId });
		auto &record = _plugins[index];
		unloadPluginRecord(record, true);
		record.state.enabled = false;
		record.state.loaded = false;
		record.state.disabledByRecovery = false;
		record.state.recoverySuspected = false;
		record.state.recoveryReason.clear();
		_disabled.insert(pluginId);
		_disabledByRecovery.remove(pluginId);
		if (_recoveryNotice.active) {
			_recoveryNotice.pluginIds.removeAll(pluginId);
			if (_recoveryNotice.pluginIds.isEmpty()) {
				_recoveryNotice = RecoveryOperationState();
			}
		}
		saveConfig();
		saveRecoveryState();
		finishRecoveryOperation();
		Logs::writeClient(QString::fromLatin1(
			"[plugins] toggle applied: %1 -> disabled")
			.arg(pluginId));
		return true;
	}

	const auto restoreIndex = index;
	const auto pluginPath = current.path;
	if (pluginPath.trimmed().isEmpty() || !QFileInfo::exists(pluginPath)) {
		auto &record = _plugins[index];
		record.state.enabled = false;
		record.state.loaded = false;
		record.state.error = u"Plugin package file was not found."_q;
		_disabled.insert(pluginId);
		saveConfig();
		logEvent(
			u"plugin"_q,
			u"toggle-enable-missing-file"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"path"_q, pluginPath },
			});
		return false;
	}

	logEvent(
		u"plugin"_q,
		u"toggle-enable-begin"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"path"_q, pluginPath },
		});
	_disabled.remove(pluginId);
	clearRecoveryDisabled(pluginId);
	saveConfig();
	unloadPluginRecord(_plugins[restoreIndex], true);
	_plugins.erase(_plugins.begin() + restoreIndex);
	rebuildPluginIndex();
	loadPlugin(pluginPath);
	moveLastPluginRecordToIndex(restoreIndex);
	auto reloadedIndex = findRecordIndex(pluginId);
	if (reloadedIndex < 0) {
		for (auto i = 0, count = int(_plugins.size()); i != count; ++i) {
			if (_plugins[i].state.path == pluginPath) {
				reloadedIndex = i;
				break;
			}
		}
	}
	const auto success = (reloadedIndex >= 0)
		&& _plugins[reloadedIndex].state.enabled
		&& _plugins[reloadedIndex].state.loaded
		&& _plugins[reloadedIndex].state.error.trimmed().isEmpty();
	if (!success) {
		logEvent(
			u"plugin"_q,
			u"toggle-enable-failed"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"path"_q, pluginPath },
			});
	}
	Logs::writeClient(QString::fromLatin1("[plugins] toggle applied: %1 -> %2")
		.arg(pluginId)
		.arg(success ? u"enabled"_q : u"enable-failed"_q));
	return success;
}

CommandResult Manager::interceptOutgoingText(
		Main::Session *session,
		History *history,
		const TextWithTags &text,
		const Api::SendOptions &options) {
	auto result = CommandResult();
	if (!_outgoingInterceptors.empty()) {
		auto ordered = std::vector<OutgoingInterceptorEntry>();
		ordered.reserve(_outgoingInterceptors.size());
		for (auto it = _outgoingInterceptors.cbegin();
			it != _outgoingInterceptors.cend();
			++it) {
			ordered.push_back(it.value());
		}
		std::sort(ordered.begin(), ordered.end(), [](
				const OutgoingInterceptorEntry &a,
				const OutgoingInterceptorEntry &b) {
			if (a.priority != b.priority) {
				return a.priority < b.priority;
			}
			return a.id < b.id;
		});
		const auto context = OutgoingTextContext{
			.session = session,
			.history = history,
			.text = text.text,
			.options = &options,
		};
		for (const auto &entry : ordered) {
			if (!entry.handler) {
				continue;
			}
			try {
				startRecoveryOperation(
					u"outgoing"_q,
					{ entry.pluginId });
				logEvent(
					u"outgoing"_q,
					u"invoke"_q,
					QJsonObject{
						{ u"id"_q, QString::number(entry.id) },
						{ u"pluginId"_q, entry.pluginId },
						{ u"priority"_q, entry.priority },
						{ u"text"_q, context.text },
						{ u"sendOptions"_q, sendOptionsToJson(context.options) },
					});
				const auto previousPluginId = _registeringPluginId;
				_registeringPluginId = entry.pluginId;
				auto handled = CommandResult();
				InvokePluginCallbackOrThrow([&] {
					handled = entry.handler(context);
				});
				_registeringPluginId = previousPluginId;
				logEvent(
					u"outgoing"_q,
					u"result"_q,
					QJsonObject{
						{ u"id"_q, QString::number(entry.id) },
						{ u"pluginId"_q, entry.pluginId },
						{ u"result"_q, commandResultToJson(handled) },
					});
				finishRecoveryOperation();
				if (handled.action != CommandResult::Action::Continue) {
					return handled;
				}
			} catch (...) {
				_registeringPluginId.clear();
				logEvent(
					u"outgoing"_q,
					u"failed"_q,
					QJsonObject{
						{ u"id"_q, QString::number(entry.id) },
						{ u"pluginId"_q, entry.pluginId },
						{ u"reason"_q, CurrentExceptionText() },
						{ u"text"_q, context.text },
					});
				disablePlugin(
					entry.pluginId,
					u"Outgoing interceptor failed: "_q + CurrentExceptionText());
				showToast(PluginUiText(
					u"Plugin interceptor failed and was disabled."_q,
					u"Перехватчик плагина завершился с ошибкой и был выключен."_q));
				finishRecoveryOperation();
			}
		}
	}
	const auto full = text.text.trimmed();
	if (full.isEmpty() || !full.startsWith('/')) {
		return result;
	}
	auto end = full.size();
	for (auto i = 1; i < full.size(); ++i) {
		if (full.at(i).isSpace()) {
			end = i;
			break;
		}
	}
	const auto token = full.mid(1, end - 1);
	if (token.isEmpty() || token.contains('@')) {
		return result;
	}
	const auto key = commandKey(token);
	if (key.isEmpty() || !IsValidCommandKey(key)) {
		return result;
	}
	const auto idIt = _commandIdByName.find(key);
	if (idIt == _commandIdByName.end()) {
		return result;
	}
	const auto entryIt = _commands.find(idIt.value());
	if (entryIt == _commands.end()) {
		return result;
	}
	const auto args = full.mid(end).trimmed();
	const auto command = '/' + key;
	const auto context = CommandContext{
		.session = session,
		.history = history,
		.text = text.text,
		.command = command,
		.args = args,
		.options = &options,
	};
	if (entryIt->handler) {
		try {
			startRecoveryOperation(
				u"command"_q,
				{ entryIt->pluginId },
				entryIt->descriptor.command);
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entryIt->pluginId;
			logEvent(
				u"command"_q,
				u"invoke"_q,
				QJsonObject{
					{ u"id"_q, QString::number(entryIt->id) },
					{ u"pluginId"_q, entryIt->pluginId },
					{ u"descriptor"_q, commandDescriptorToJson(entryIt->descriptor) },
					{ u"text"_q, context.text },
					{ u"commandText"_q, context.command },
					{ u"args"_q, context.args },
					{ u"sendOptions"_q, sendOptionsToJson(context.options) },
				});
			auto handled = CommandResult();
			InvokePluginCallbackOrThrow([&] {
				handled = entryIt->handler(context);
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"command"_q,
				u"result"_q,
				QJsonObject{
					{ u"id"_q, QString::number(entryIt->id) },
					{ u"pluginId"_q, entryIt->pluginId },
					{ u"result"_q, commandResultToJson(handled) },
				});
			finishRecoveryOperation();
			return handled;
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"command"_q,
				u"failed"_q,
				QJsonObject{
					{ u"id"_q, QString::number(entryIt->id) },
					{ u"pluginId"_q, entryIt->pluginId },
					{ u"reason"_q, CurrentExceptionText() },
					{ u"text"_q, context.text },
					{ u"commandText"_q, context.command },
					{ u"args"_q, context.args },
				});
			disablePlugin(
				entryIt->pluginId,
				u"Command handler failed: "_q + CurrentExceptionText());
			showToast(PluginUiText(
				u"Plugin command failed and was disabled."_q,
				u"Команда плагина завершилась с ошибкой и была выключена."_q));
			finishRecoveryOperation();
			return {
				.action = CommandResult::Action::Cancel,
			};
		}
	}
	return result;
}

void Manager::notifyWindowCreated(Window::Controller *window) {
	logEvent(
		u"window"_q,
		u"created"_q,
		QJsonObject{
			{ u"hasWindow"_q, window != nullptr },
			{ u"hasWidget"_q, (window && window->widget()) },
		});
	if (_recoveryNotice.active && !_recoveryNoticeShown) {
		QTimer::singleShot(0, this, [=] {
			showRecoveryNotice(window);
		});
	}
	const auto handlers = _windowHandlers;
	for (const auto &entry : handlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			startRecoveryOperation(u"window"_q, { entry.pluginId });
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			logEvent(
				u"window"_q,
				u"invoke-callback"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"hasWindow"_q, window != nullptr },
				});
			InvokePluginCallbackOrThrow([&] {
				entry.handler(window);
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"window"_q,
				u"callback-success"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
				});
			finishRecoveryOperation();
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"window"_q,
				u"callback-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"reason"_q, CurrentExceptionText() },
				});
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Window callback failed: "_q + CurrentExceptionText());
			}
			showToast(PluginUiText(
				u"Plugin window callback failed."_q,
				u"Оконный callback плагина завершился с ошибкой."_q));
			finishRecoveryOperation();
		}
	}
	if (!window) {
		return;
	}
	const auto widget = window->widget();
	const auto widgetHandlers = _windowWidgetHandlers;
	for (const auto &entry : widgetHandlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			startRecoveryOperation(u"window"_q, { entry.pluginId });
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			logEvent(
				u"window"_q,
				u"invoke-widget-callback"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"hasWidget"_q, true },
				});
			InvokePluginCallbackOrThrow([&] {
				entry.handler(widget);
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"window"_q,
				u"widget-callback-success"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
				});
			finishRecoveryOperation();
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"window"_q,
				u"widget-callback-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"reason"_q, CurrentExceptionText() },
				});
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Window widget callback failed: "_q + CurrentExceptionText());
			}
			showToast(PluginUiText(
				u"Plugin window widget callback failed."_q,
				u"Window widget callback плагина завершился с ошибкой."_q));
			finishRecoveryOperation();
		}
	}
}

int Manager::apiVersion() const {
	return kApiVersion;
}

QString Manager::pluginsPath() const {
	return _pluginsPath;
}

CommandId Manager::registerCommand(
		const QString &pluginId,
	CommandDescriptor descriptor,
	CommandHandler handler) {
	if (!hasPlugin(pluginId) || !handler) {
		logEvent(
			u"registry"_q,
			u"register-command-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto key = commandKey(descriptor.command);
	if (!IsValidCommandKey(key) || _commandIdByName.contains(key)) {
		logEvent(
			u"registry"_q,
			u"register-command-invalid"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"descriptor"_q, commandDescriptorToJson(descriptor) },
				{ u"normalizedKey"_q, key },
				{ u"duplicate"_q, _commandIdByName.contains(key) },
			});
		return 0;
	}
	descriptor.command = '/' + key;
	const auto id = _nextCommandId++;
	_commands.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.descriptor = std::move(descriptor),
		.handler = std::move(handler),
	});
	_commandIdByName.insert(key, id);
	_commandsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->commandIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-command"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"descriptor"_q, commandDescriptorToJson(_commands.value(id).descriptor) },
		});
	return id;
}

void Manager::unregisterCommand(CommandId id) {
	const auto it = _commands.find(id);
	if (it == _commands.end()) {
		return;
	}
	const auto key = commandKey(it->descriptor.command);
	_commandIdByName.remove(key);
	const auto pluginId = it->pluginId;
	_commands.remove(id);
	const auto listIt = _commandsByPlugin.find(pluginId);
	if (listIt != _commandsByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_commandsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->commandIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-command"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

ActionId Manager::registerAction(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionHandler handler) {
	if (!hasPlugin(pluginId) || !handler || title.trimmed().isEmpty()) {
		logEvent(
			u"registry"_q,
			u"register-action-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"title"_q, title },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto id = _nextActionId++;
	_actions.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.title = title,
		.description = description,
		.handler = std::move(handler),
	});
	_actionsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->actionIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-action"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"title"_q, title },
			{ u"description"_q, description },
			{ u"contextAware"_q, false },
		});
	return id;
}

void Manager::unregisterAction(ActionId id) {
	const auto it = _actions.find(id);
	if (it == _actions.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_actions.remove(id);
	const auto listIt = _actionsByPlugin.find(pluginId);
	if (listIt != _actionsByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_actionsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->actionIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-action"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

ActionId Manager::registerActionWithContext(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionWithContextHandler handler) {
	if (!hasPlugin(pluginId) || !handler || title.trimmed().isEmpty()) {
		logEvent(
			u"registry"_q,
			u"register-action-context-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"title"_q, title },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto id = _nextActionId++;
	_actions.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.title = title,
		.description = description,
		.handler = nullptr,
		.handlerWithContext = std::move(handler),
	});
	_actionsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->actionIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-action"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"title"_q, title },
			{ u"description"_q, description },
			{ u"contextAware"_q, true },
		});
	return id;
}

OutgoingInterceptorId Manager::registerOutgoingTextInterceptor(
		const QString &pluginId,
		OutgoingTextHandler handler,
		int priority) {
	if (!hasPlugin(pluginId) || !handler) {
		logEvent(
			u"registry"_q,
			u"register-outgoing-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"priority"_q, priority },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto id = _nextOutgoingInterceptorId++;
	_outgoingInterceptors.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.priority = priority,
		.handler = std::move(handler),
	});
	_outgoingInterceptorsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->outgoingInterceptorIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-outgoing"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"priority"_q, priority },
		});
	return id;
}

void Manager::unregisterOutgoingTextInterceptor(OutgoingInterceptorId id) {
	const auto it = _outgoingInterceptors.find(id);
	if (it == _outgoingInterceptors.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_outgoingInterceptors.remove(id);
	const auto listIt = _outgoingInterceptorsByPlugin.find(pluginId);
	if (listIt != _outgoingInterceptorsByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_outgoingInterceptorsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->outgoingInterceptorIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-outgoing"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

MessageObserverId Manager::registerMessageObserver(
		const QString &pluginId,
		MessageObserverOptions options,
		MessageEventHandler handler) {
	if (!hasPlugin(pluginId) || !handler) {
		logEvent(
			u"registry"_q,
			u"register-observer-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto id = _nextMessageObserverId++;
	_messageObservers.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.options = options,
		.handler = std::move(handler),
	});
	_messageObserversByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->messageObserverIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-observer"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"newMessages"_q, options.newMessages },
			{ u"editedMessages"_q, options.editedMessages },
			{ u"deletedMessages"_q, options.deletedMessages },
			{ u"incoming"_q, options.incoming },
			{ u"outgoing"_q, options.outgoing },
		});
	updateMessageObserverSubscriptions();
	return id;
}

void Manager::unregisterMessageObserver(MessageObserverId id) {
	const auto it = _messageObservers.find(id);
	if (it == _messageObservers.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_messageObservers.remove(id);
	const auto listIt = _messageObserversByPlugin.find(pluginId);
	if (listIt != _messageObserversByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_messageObserversByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->messageObserverIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-observer"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
	updateMessageObserverSubscriptions();
}

PanelId Manager::registerPanel(
		const QString &pluginId,
		PanelDescriptor descriptor,
		PanelHandler handler) {
	if (!hasPlugin(pluginId) || !handler || descriptor.title.trimmed().isEmpty()) {
		logEvent(
			u"registry"_q,
			u"register-panel-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"descriptor"_q, panelDescriptorToJson(descriptor) },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto id = _nextPanelId++;
	_panels.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.descriptor = std::move(descriptor),
		.handler = std::move(handler),
	});
	_panelsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->panelIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-panel"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"descriptor"_q, panelDescriptorToJson(_panels.value(id).descriptor) },
		});
	return id;
}

void Manager::unregisterPanel(PanelId id) {
	const auto it = _panels.find(id);
	if (it == _panels.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_panels.remove(id);
	const auto listIt = _panelsByPlugin.find(pluginId);
	if (listIt != _panelsByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_panelsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->panelIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-panel"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

SettingsPageId Manager::registerSettingsPage(
		const QString &pluginId,
		SettingsPageDescriptor descriptor,
		SettingsChangedHandler handler) {
	if (!hasPlugin(pluginId) || !handler || !HasAnySettings(descriptor)) {
		logEvent(
			u"registry"_q,
			u"register-settings-page-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"descriptor"_q, settingsPageDescriptorToJson(descriptor) },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	for (auto &section : descriptor.sections) {
		section.id = section.id.trimmed();
		section.title = section.title.trimmed();
		section.description = section.description.trimmed();
		for (auto &setting : section.settings) {
			setting = NormalizeSettingDescriptor(std::move(setting));
		}
	}
	applyStoredSettings(pluginId, descriptor);
	const auto id = _nextSettingsPageId++;
	_settingsPages.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.descriptor = std::move(descriptor),
		.handler = std::move(handler),
	});
	_settingsPagesByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->settingsPageIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-settings-page"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"descriptor"_q, settingsPageDescriptorToJson(_settingsPages.value(id).descriptor) },
		});
	return id;
}

void Manager::unregisterSettingsPage(SettingsPageId id) {
	const auto it = _settingsPages.find(id);
	if (it == _settingsPages.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_settingsPages.remove(id);
	const auto listIt = _settingsPagesByPlugin.find(pluginId);
	if (listIt != _settingsPagesByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_settingsPagesByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->settingsPageIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-settings-page"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

void Manager::showToast(const QString &text) {
	logEvent(
		u"ui"_q,
		u"toast"_q,
		QJsonObject{
			{ u"text"_q, text },
		});
	if (const auto window = Core::App().activeWindow()) {
		window->showToast(text);
		return;
	}
	if (const auto primary = Core::App().activePrimaryWindow()) {
		primary->showToast(text);
		return;
	}
	Ui::Toast::Show(text);
}

void Manager::forEachWindow(
		std::function<void(Window::Controller*)> visitor) {
	if (!visitor) {
		return;
	}
	Core::App().forEachWindow([&](not_null<Window::Controller*> window) {
		visitor(window.get());
	});
}

void Manager::onWindowCreated(
		std::function<void(Window::Controller*)> handler) {
	if (handler) {
		_windowHandlers.push_back({
			.pluginId = _registeringPluginId,
			.handler = std::move(handler),
		});
		logEvent(
			u"registry"_q,
			u"register-window-handler"_q,
			QJsonObject{
				{ u"pluginId"_q, _registeringPluginId },
			});
	}
}

void Manager::forEachWindowWidget(
		std::function<void(QWidget*)> visitor) {
	if (!visitor) {
		return;
	}
	Core::App().forEachWindow([&](not_null<Window::Controller*> window) {
		if (const auto widget = window->widget()) {
			visitor(widget);
		}
	});
}

void Manager::onWindowWidgetCreated(
		std::function<void(QWidget*)> handler) {
	if (handler) {
		_windowWidgetHandlers.push_back({
			.pluginId = _registeringPluginId,
			.handler = std::move(handler),
		});
		logEvent(
			u"registry"_q,
			u"register-window-widget-handler"_q,
			QJsonObject{
				{ u"pluginId"_q, _registeringPluginId },
			});
	}
}

Window::Controller *Manager::activeWindow() const {
	if (const auto window = Core::App().activeWindow()) {
		return window;
	}
	return Core::App().activePrimaryWindow();
}

QWidget *Manager::activeWindowWidget() const {
	if (const auto window = activeWindow()) {
		return window->widget();
	}
	return nullptr;
}

bool Manager::settingBoolValue(
		const QString &pluginId,
		const QString &settingId,
		bool fallback) const {
	const auto value = storedSettingValue(pluginId, settingId);
	return value.isBool() ? value.toBool(fallback) : fallback;
}

int Manager::settingIntValue(
		const QString &pluginId,
		const QString &settingId,
		int fallback) const {
	const auto value = storedSettingValue(pluginId, settingId);
	return value.isDouble() ? value.toInt(fallback) : fallback;
}

QString Manager::settingStringValue(
		const QString &pluginId,
		const QString &settingId,
		const QString &fallback) const {
	const auto value = storedSettingValue(pluginId, settingId);
	return value.isString() ? value.toString(fallback) : fallback;
}

Main::Session *Manager::activeSession() const {
	if (const auto window = activeWindow()) {
		if (const auto session = window->maybeSession()) {
			return session;
		}
	}
	if (_activeSession) {
		return _activeSession;
	}
	if (Core::App().domain().started()) {
		const auto &account = Core::App().domain().active();
		if (account.sessionExists()) {
			return &account.session();
		}
	}
	return nullptr;
}

void Manager::forEachSession(
		std::function<void(Main::Session*)> visitor) {
	if (!visitor || !Core::App().domain().started()) {
		return;
	}
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (const auto session = account->maybeSession()) {
			visitor(session);
		}
	}
}

void Manager::onSessionActivated(
		std::function<void(Main::Session*)> handler) {
	if (handler) {
		_sessionHandlers.push_back({
			.pluginId = _registeringPluginId,
			.handler = std::move(handler),
		});
		logEvent(
			u"registry"_q,
			u"register-session-handler"_q,
			QJsonObject{
				{ u"pluginId"_q, _registeringPluginId },
			});
	}
}

HostInfo Manager::hostInfo() const {
	auto info = HostInfo();
	info.compiler = QString::fromLatin1(kCompilerId);
	info.platform = QString::fromLatin1(kPlatformId);
	info.workingPath = cWorkingDir();
	info.pluginsPath = _pluginsPath;
	info.appUiLanguage = Lang::LanguageIdOrDefault(Lang::Id());
	info.safeModeEnabled = safeModeEnabled();
	info.runtimeApiEnabled = _runtimeApiEnabled;
	info.runtimeApiPort = _runtimeApiEnabled ? _runtimeApiPort : 0;
	info.runtimeApiBaseUrl = _runtimeApiEnabled
		? (u"http://%1:%2"_q.arg(RuntimeApiAdvertisedHost()).arg(_runtimeApiPort))
		: QString();
	return info;
}

SystemInfo Manager::systemInfo() const {
	auto info = SystemInfo();
	const auto locale = QLocale::system();
	info.processId = quint64(QCoreApplication::applicationPid());
	info.logicalCpuCores = std::max(QThread::idealThreadCount(), 1);
	info.physicalCpuCores = PhysicalCpuCoresFallback();
	info.productType = QSysInfo::productType();
	info.productVersion = QSysInfo::productVersion();
	info.prettyProductName = QSysInfo::prettyProductName();
	info.kernelType = QSysInfo::kernelType();
	info.kernelVersion = QSysInfo::kernelVersion();
	info.architecture = QSysInfo::currentCpuArchitecture();
	info.buildAbi = QSysInfo::buildAbi();
	info.hostName = QSysInfo::machineHostName();
	info.userName = ProcessUserName();
	info.locale = locale.name();
	const auto uiLanguages = locale.uiLanguages();
	info.uiLanguage = uiLanguages.isEmpty() ? QString() : uiLanguages.value(0);
	info.timeZone = QString::fromLatin1(QTimeZone::systemTimeZoneId());
	return info;
}

void Manager::loadConfig() {
	_disabled.clear();
	_storedSettings.clear();
	_runtimeApiEnabled = false;
	_runtimeApiPort = 37080;
	QFile file(_configPath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		logEvent(
			u"config"_q,
			u"load-empty"_q,
			QJsonObject{
				{ u"path"_q, _configPath },
			});
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		logEvent(
			u"config"_q,
			u"load-invalid-json"_q,
			QJsonObject{
				{ u"path"_q, _configPath },
			});
		return;
	}
	const auto object = document.object();
	const auto array = object.value(u"disabled"_q).toArray();
	for (const auto &value : array) {
		if (value.isString()) {
			_disabled.insert(value.toString());
		}
	}
	const auto settings = object.value(u"settings"_q).toObject();
	for (const auto &pluginId : SortedJsonKeys(settings)) {
		const auto value = settings.value(pluginId);
		if (value.isObject()) {
			_storedSettings.insert(pluginId, value.toObject());
		}
	}
	if (const auto runtimeValue = object.value(u"runtimeApi"_q); runtimeValue.isObject()) {
		const auto runtimeObject = runtimeValue.toObject();
		_runtimeApiEnabled = runtimeObject.value(u"enabled"_q).toBool(false);
		const auto loadedPort = runtimeObject.value(u"port"_q).toInt(_runtimeApiPort);
		_runtimeApiPort = std::clamp(loadedPort, 1, 65535);
	}
	logEvent(
		u"config"_q,
		u"loaded"_q,
		QJsonObject{
			{ u"path"_q, _configPath },
			{ u"disabled"_q, JsonArrayFromStrings(_disabled.values()) },
			{ u"settingsPlugins"_q, _storedSettings.size() },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
		});
}

void Manager::saveConfig() const {
	QDir().mkpath(QFileInfo(_configPath).absolutePath());
	auto list = _disabled.values();
	list.sort(Qt::CaseInsensitive);
	auto array = QJsonArray();
	for (const auto &id : list) {
		array.push_back(id);
	}
	auto settings = QJsonObject();
	auto settingPluginIds = _storedSettings.keys();
	settingPluginIds.sort(Qt::CaseInsensitive);
	for (const auto &pluginId : settingPluginIds) {
		const auto values = _storedSettings.value(pluginId);
		if (!values.isEmpty()) {
			settings.insert(pluginId, values);
		}
	}
	const auto object = QJsonObject{
		{ u"disabled"_q, array },
		{ u"settings"_q, settings },
		{ u"runtimeApi"_q, QJsonObject{
			{ u"enabled"_q, _runtimeApiEnabled },
			{ u"port"_q, _runtimeApiPort },
		} },
	};
	const auto document = QJsonDocument(object);
	QSaveFile file(_configPath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(document.toJson(QJsonDocument::Indented));
		file.commit();
	}
	logEvent(
		u"config"_q,
		u"saved"_q,
		QJsonObject{
			{ u"path"_q, _configPath },
			{ u"disabled"_q, JsonArrayFromStrings(list) },
			{ u"settingsPlugins"_q, settings.size() },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
		});
}

void Manager::appendTraceLine(const QByteArray &line) const {
	writeLogRecord(_tracePath, line, true);
}

void Manager::writeLogRecord(
		const QString &path,
		const QByteArray &record,
		bool jsonLog) const {
	if (path.isEmpty() || record.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	const auto rotated = rotateLogFileIfNeeded(path, record.size(), jsonLog);
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		return;
	}
	if (rotated) {
		const auto rotationEvent = makeLogEvent(
			u"log"_q,
			u"rotated"_q,
			QJsonObject{
				{ u"target"_q, QFileInfo(path).fileName() },
				{ u"json"_q, jsonLog },
				{ u"reason"_q, u"max-size"_q },
				{ u"maxBytes"_q, QString::number(kMaxPluginLogBytes) },
			});
		const auto rotationLine = jsonLog
			? (QJsonDocument(rotationEvent).toJson(QJsonDocument::Compact) + '\n')
			: ((rotationEvent.value(u"timestampUtc"_q).toString()
				+ u" "_q
				+ formatLogEventText(rotationEvent)
				+ u"\n"_q).toUtf8());
		file.write(rotationLine);
	}
	file.write(record);
}

bool Manager::rotateLogFileIfNeeded(
		const QString &path,
		qsizetype recordSize,
		bool jsonLog) const {
	Q_UNUSED(jsonLog);
	if (path.isEmpty()) {
		return false;
	}
	const auto info = QFileInfo(path);
	if (!info.exists()) {
		return false;
	}
	if ((info.size() + recordSize) <= kMaxPluginLogBytes) {
		return false;
	}
	const auto rotatedName = [&](int index) {
		return path + u'.' + QString::number(index);
	};
	for (auto index = kMaxPluginLogBackups; index >= 1; --index) {
		const auto source = (index == 1) ? path : rotatedName(index - 1);
		const auto target = rotatedName(index);
		if (!QFileInfo::exists(source)) {
			continue;
		}
		if (QFileInfo::exists(target)) {
			QFile::remove(target);
		}
		QFile::rename(source, target);
	}
	return true;
}

void Manager::appendLogLine(const QString &line) const {
	if (_logPath.isEmpty()) {
		return;
	}
	const auto stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
	writeLogRecord(
		_logPath,
		(stamp + u" "_q + line.trimmed() + u"\n"_q).toUtf8(),
		false);
}

QJsonObject Manager::makeLogEvent(
		QString phase,
		QString event,
		QJsonObject details) const {
	auto result = QJsonObject{
		{ u"timestampUtc"_q, DateTimeToIsoUtc(QDateTime::currentDateTimeUtc()) },
		{ u"pid"_q, int(QCoreApplication::applicationPid()) },
		{ u"phase"_q, std::move(phase) },
		{ u"event"_q, std::move(event) },
		{ u"language"_q, UseRussianPluginUi() ? u"ru"_q : u"en"_q },
	};
	if (_activeSession) {
		result.insert(
			u"sessionUniqueId"_q,
			QString::number(_activeSession->uniqueId()));
	}
	if (!_traceOperations.isEmpty()) {
		const auto &operation = _traceOperations.back();
		result.insert(u"operationId"_q, QString::number(operation.id));
		result.insert(u"operationKind"_q, operation.kind);
		result.insert(u"operationDepth"_q, _traceOperations.size());
		if (!operation.pluginIds.isEmpty()) {
			result.insert(
				u"operationPluginIds"_q,
				JsonArrayFromStrings(operation.pluginIds));
		}
		if (!operation.details.isEmpty()) {
			result.insert(u"operationDetails"_q, operation.details);
		}
	}
	MergeJsonObject(result, details);
	return result;
}

QString Manager::formatLogEventText(const QJsonObject &event) const {
	auto parts = QStringList();
	if (const auto phase = event.value(u"phase"_q).toString(); !phase.isEmpty()) {
		parts.push_back(u"phase="_q + JsonValueToText(phase));
	}
	if (const auto name = event.value(u"event"_q).toString(); !name.isEmpty()) {
		parts.push_back(u"event="_q + JsonValueToText(name));
	}
	for (const auto &key : SortedJsonKeys(event)) {
		if (key == u"timestampUtc"_q || key == u"phase"_q || key == u"event"_q) {
			continue;
		}
		parts.push_back(key + u"="_q + JsonValueToText(event.value(key)));
	}
	return parts.join(u" "_q);
}

void Manager::logEvent(
		QString phase,
		QString event,
		QJsonObject details) const {
	const auto payload = makeLogEvent(
		std::move(phase),
		std::move(event),
		std::move(details));
	appendLogLine(formatLogEventText(payload));
	appendTraceLine(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
}

void Manager::logOperationStart(
		const QString &kind,
		const QStringList &pluginIds,
		const QString &details) {
	auto operation = TraceOperationState();
	operation.id = _nextTraceOperationId++;
	operation.kind = kind;
	operation.pluginIds = pluginIds;
	operation.details = details;
	operation.startedAt = QDateTime::currentDateTimeUtc();
	_traceOperations.push_back(operation);
	logEvent(
		u"operation"_q,
		u"start"_q,
		QJsonObject{
			{ u"kind"_q, kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(pluginIds) },
			{ u"details"_q, details },
			{ u"startedAt"_q, DateTimeToIsoUtc(operation.startedAt) },
		});
}

void Manager::logOperationFinish(
		const QString &result,
		const QString &reason) {
	if (_traceOperations.isEmpty()) {
		return;
	}
	const auto operation = _traceOperations.back();
	_traceOperations.pop_back();
	const auto finishedAt = QDateTime::currentDateTimeUtc();
	logEvent(
		u"operation"_q,
		u"finish"_q,
		QJsonObject{
			{ u"operationId"_q, QString::number(operation.id) },
			{ u"kind"_q, operation.kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(operation.pluginIds) },
			{ u"details"_q, operation.details },
			{ u"startedAt"_q, DateTimeToIsoUtc(operation.startedAt) },
			{ u"finishedAt"_q, DateTimeToIsoUtc(finishedAt) },
			{ u"durationMs"_q, QString::number(operation.startedAt.msecsTo(finishedAt)) },
			{ u"result"_q, result.isEmpty() ? u"ok"_q : result },
			{ u"reason"_q, reason },
		});
}

quint64 Manager::currentOperationId() const {
	return _traceOperations.isEmpty() ? 0 : _traceOperations.back().id;
}

QJsonObject Manager::pluginInfoToJson(const PluginInfo &info) const {
	return QJsonObject{
		{ u"id"_q, info.id },
		{ u"name"_q, info.name },
		{ u"version"_q, info.version },
		{ u"author"_q, info.author },
		{ u"description"_q, info.description },
		{ u"website"_q, info.website },
	};
}

QJsonObject Manager::pluginStateToJson(const PluginState &state) const {
	auto result = pluginInfoToJson(state.info);
	result.insert(u"path"_q, state.path);
	result.insert(u"sha256"_q, state.sha256);
	result.insert(u"enabled"_q, state.enabled);
	result.insert(u"loaded"_q, state.loaded);
	result.insert(u"error"_q, state.error);
	result.insert(u"disabledByRecovery"_q, state.disabledByRecovery);
	result.insert(u"recoverySuspected"_q, state.recoverySuspected);
	result.insert(u"recoveryReason"_q, state.recoveryReason);
	result.insert(u"sourceVerified"_q, state.sourceVerified);
	result.insert(u"sourceTrustText"_q, state.sourceTrustText);
	result.insert(u"sourceTrustDetails"_q, state.sourceTrustDetails);
	result.insert(u"sourceTrustReason"_q, state.sourceTrustReason);
	result.insert(u"sourceChannelId"_q, QString::number(state.sourceChannelId));
	result.insert(u"sourceMessageId"_q, QString::number(state.sourceMessageId));
	return result;
}

QJsonObject Manager::commandDescriptorToJson(
		const CommandDescriptor &descriptor) const {
	return QJsonObject{
		{ u"command"_q, descriptor.command },
		{ u"description"_q, descriptor.description },
		{ u"usage"_q, descriptor.usage },
	};
}

QJsonObject Manager::panelDescriptorToJson(
		const PanelDescriptor &descriptor) const {
	return QJsonObject{
		{ u"title"_q, descriptor.title },
		{ u"description"_q, descriptor.description },
	};
}

QJsonObject Manager::settingDescriptorToJson(
		const SettingDescriptor &descriptor) const {
	return QJsonObject{
		{ u"id"_q, descriptor.id },
		{ u"title"_q, descriptor.title },
		{ u"description"_q, descriptor.description },
		{ u"type"_q, SettingControlName(descriptor.type) },
		{ u"boolValue"_q, descriptor.boolValue },
		{ u"intValue"_q, descriptor.intValue },
		{ u"intMinimum"_q, descriptor.intMinimum },
		{ u"intMaximum"_q, descriptor.intMaximum },
		{ u"intStep"_q, descriptor.intStep },
		{ u"valueSuffix"_q, descriptor.valueSuffix },
		{ u"textLength"_q, descriptor.textValue.size() },
		{ u"hasTextValue"_q, !descriptor.textValue.isEmpty() },
		{ u"placeholderText"_q, descriptor.placeholderText },
		{ u"secret"_q, descriptor.secret },
		{ u"buttonText"_q, descriptor.buttonText },
	};
}

QJsonObject Manager::settingsPageDescriptorToJson(
		const SettingsPageDescriptor &descriptor) const {
	auto sections = QJsonArray();
	for (const auto &section : descriptor.sections) {
		auto settings = QJsonArray();
		for (const auto &setting : section.settings) {
			settings.push_back(settingDescriptorToJson(setting));
		}
		sections.push_back(QJsonObject{
			{ u"id"_q, section.id },
			{ u"title"_q, section.title },
			{ u"description"_q, section.description },
			{ u"settings"_q, settings },
		});
	}
	return QJsonObject{
		{ u"id"_q, descriptor.id },
		{ u"title"_q, descriptor.title },
		{ u"description"_q, descriptor.description },
		{ u"sections"_q, sections },
	};
}

QJsonObject Manager::sendOptionsToJson(
		const Api::SendOptions *options) const {
	if (!options) {
		return {};
	}
	return QJsonObject{
		{ u"price"_q, QString::number(options->price) },
		{ u"scheduled"_q, QString::number(options->scheduled) },
		{ u"scheduleRepeatPeriod"_q, QString::number(options->scheduleRepeatPeriod) },
		{ u"shortcutId"_q, QString::number(options->shortcutId) },
		{ u"effectId"_q, QString::number(options->effectId) },
		{ u"stakeNanoTon"_q, QString::number(options->stakeNanoTon) },
		{ u"starsApproved"_q, options->starsApproved },
		{ u"silent"_q, options->silent },
		{ u"handleSupportSwitch"_q, options->handleSupportSwitch },
		{ u"invertCaption"_q, options->invertCaption },
		{ u"hideViaBot"_q, options->hideViaBot },
		{ u"mediaSpoiler"_q, options->mediaSpoiler },
		{ u"ttlSeconds"_q, QString::number(options->ttlSeconds) },
		{ u"hasSendAs"_q, options->sendAs != nullptr },
		{ u"stakeSeedHashHex"_q, QString::fromLatin1(options->stakeSeedHash.toHex()) },
	};
}

QJsonObject Manager::binaryInfoToJson(const BinaryInfo &info) const {
	return QJsonObject{
		{ u"structVersion"_q, info.structVersion },
		{ u"apiVersion"_q, info.apiVersion },
		{ u"pointerSize"_q, info.pointerSize },
		{ u"qtMajor"_q, info.qtMajor },
		{ u"qtMinor"_q, info.qtMinor },
		{ u"compiler"_q, QString::fromLatin1(info.compiler ? info.compiler : "unknown") },
		{ u"compilerVersion"_q, info.compilerVersion },
		{ u"platform"_q, QString::fromLatin1(info.platform ? info.platform : "unknown") },
	};
}

QJsonObject Manager::fileInfoToJson(const QFileInfo &info) const {
	return QJsonObject{
		{ u"fileName"_q, info.fileName() },
		{ u"path"_q, info.absoluteFilePath() },
		{ u"canonicalPath"_q, info.canonicalFilePath() },
		{ u"exists"_q, info.exists() },
		{ u"size"_q, QString::number(info.size()) },
		{ u"lastModifiedUtc"_q, DateTimeToIsoUtc(info.lastModified()) },
	};
}

QJsonObject Manager::messageContextToJson(
		const MessageEventContext &context) const {
	auto result = QJsonObject{
		{ u"event"_q, MessageEventName(context.event) },
		{ u"hasSession"_q, context.session != nullptr },
		{ u"hasHistory"_q, context.history != nullptr },
		{ u"hasItem"_q, context.item != nullptr },
	};
	if (context.item) {
		const auto fullId = context.item->fullId();
		result.insert(u"peerId"_q, QString::number(fullId.peer.value));
		result.insert(u"msgId"_q, QString::number(fullId.msg.bare));
		result.insert(u"outgoing"_q, context.item->out());
		result.insert(u"text"_q, context.item->originalText().text);
	}
	return result;
}

QJsonObject Manager::commandResultToJson(const CommandResult &result) const {
	return QJsonObject{
		{ u"action"_q, CommandResultActionName(result.action) },
		{ u"replacementText"_q, result.replacementText },
	};
}

QJsonObject Manager::registrationSummaryToJson(
		const PluginRecord &record) const {
	auto windowHandlers = 0;
	for (const auto &entry : _windowHandlers) {
		if (entry.pluginId == record.state.info.id) {
			++windowHandlers;
		}
	}
	auto windowWidgetHandlers = 0;
	for (const auto &entry : _windowWidgetHandlers) {
		if (entry.pluginId == record.state.info.id) {
			++windowWidgetHandlers;
		}
	}
	auto sessionHandlers = 0;
	for (const auto &entry : _sessionHandlers) {
		if (entry.pluginId == record.state.info.id) {
			++sessionHandlers;
		}
	}
	return QJsonObject{
		{ u"commands"_q, record.commandIds.size() },
		{ u"actions"_q, record.actionIds.size() },
		{ u"panels"_q, record.panelIds.size() },
		{ u"settingsPages"_q, record.settingsPageIds.size() },
		{ u"outgoingInterceptors"_q, record.outgoingInterceptorIds.size() },
		{ u"messageObservers"_q, record.messageObserverIds.size() },
		{ u"windowHandlers"_q, windowHandlers },
		{ u"windowWidgetHandlers"_q, windowWidgetHandlers },
		{ u"sessionHandlers"_q, sessionHandlers },
	};
}

QString Manager::fileSha256(const QString &path) const {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		hash.addData(file.read(256 * 1024));
	}
	return QString::fromLatin1(hash.result().toHex());
}

void Manager::syncSourceTrustState(PluginState &state) const {
	const auto normalizeHash = [](QString value) {
		value = value.trimmed().toLower();
		if (value.startsWith(u"sha256:"_q)) {
			value = value.mid(7);
		}
		if (value.size() != 64) {
			return QString();
		}
		for (const auto ch : value) {
			const auto hex = ch.unicode();
			const auto digit = (hex >= '0' && hex <= '9');
			const auto lower = (hex >= 'a' && hex <= 'f');
			if (!digit && !lower) {
				return QString();
			}
		}
		return value;
	};
	struct ParsedRecord {
		QString sha256;
		int64 channelId = 0;
		int64 messageId = 0;
		QString label;
	};
	const auto parseRecord = [&](QString raw) -> ParsedRecord {
		raw = raw.trimmed();
		if (raw.isEmpty()) {
			return {};
		}
		auto delimiter = u'|';
		auto parts = raw.split(delimiter, Qt::KeepEmptyParts);
		if (parts.size() < 3) {
			delimiter = u';';
			parts = raw.split(delimiter, Qt::KeepEmptyParts);
		}
		if (parts.size() < 3) {
			return {};
		}
		auto result = ParsedRecord();
		result.sha256 = normalizeHash(parts[0]);
		result.channelId = parts[1].trimmed().toLongLong();
		result.messageId = parts[2].trimmed().toLongLong();
		if (parts.size() > 3) {
			result.label = parts.mid(3).join(delimiter).trimmed();
		}
		return result;
	};

	state.sha256 = normalizeHash(fileSha256(state.path));
	state.sourceVerified = false;
	state.sourceTrustText = u"unverified"_q;
	state.sourceTrustDetails.clear();
	state.sourceTrustReason.clear();
	state.sourceChannelId = 0;
	state.sourceMessageId = 0;

	if (state.sha256.isEmpty()) {
		state.sourceTrustReason = u"sha256-unavailable"_q;
		state.sourceTrustDetails = u"sha256-unavailable"_q;
		return;
	}
	const auto session = activeSession();
	if (!session) {
		state.sourceTrustReason = u"no-active-session"_q;
		state.sourceTrustDetails = u"no-active-session"_q;
		return;
	}
	const auto trustedChannels = session->appConfig().astrogramTrustedPluginChannelIds();
	const auto trustedRecords = session->appConfig().astrogramTrustedPluginRecords();
	auto matchedHashInUntrustedChannel = false;
	auto matchedHashWithoutOrigin = false;
	auto hasValidTrustedRecord = false;
	auto matchedChannelId = int64(0);
	auto matchedMessageId = int64(0);
	for (const auto &rawRecord : trustedRecords) {
		const auto record = parseRecord(rawRecord);
		if (!record.sha256.isEmpty()) {
			hasValidTrustedRecord = true;
		}
		if (record.sha256.isEmpty() || record.sha256 != state.sha256) {
			continue;
		}
		if (!record.channelId || (record.messageId <= 0)) {
			matchedHashWithoutOrigin = true;
			continue;
		}
		if (record.channelId
			&& !trustedChannels.empty()
			&& (std::find(
				trustedChannels.begin(),
				trustedChannels.end(),
				record.channelId) == trustedChannels.end())) {
			matchedHashInUntrustedChannel = true;
			matchedChannelId = record.channelId;
			matchedMessageId = record.messageId;
			continue;
		}
		state.sourceVerified = true;
		state.sourceTrustText = u"verified"_q;
		state.sourceTrustReason = u"exact-sha256-trusted-record"_q;
		state.sourceChannelId = record.channelId;
		state.sourceMessageId = record.messageId;
		state.sourceTrustDetails = !record.label.isEmpty()
			? record.label
			: (record.channelId
				? (QString::number(record.channelId)
					+ u":"_q
					+ QString::number(record.messageId))
				: QString());
		return;
	}

	state.sourceChannelId = matchedChannelId;
	state.sourceMessageId = matchedMessageId;
	state.sourceTrustReason = trustedRecords.empty()
		? u"no-trusted-records"_q
		: !hasValidTrustedRecord
		? u"no-valid-trusted-records"_q
		: matchedHashInUntrustedChannel
		? u"hash-found-in-untrusted-channel"_q
		: matchedHashWithoutOrigin
		? u"matching-record-missing-origin"_q
		: u"hash-not-in-trusted-records"_q;
	state.sourceTrustDetails = state.sourceTrustReason;
}

QJsonValue Manager::storedSettingValue(
		const QString &pluginId,
		const QString &settingId) const {
	const auto it = _storedSettings.constFind(pluginId);
	if (it == _storedSettings.cend()) {
		return QJsonValue();
	}
	return it.value().value(settingId);
}

void Manager::applyStoredSettings(
		const QString &pluginId,
		SettingsPageDescriptor &descriptor) const {
	for (auto &section : descriptor.sections) {
		for (auto &setting : section.settings) {
			const auto value = storedSettingValue(pluginId, setting.id);
			if (value.isUndefined() || value.isNull()) {
				continue;
			}
			switch (setting.type) {
			case SettingControl::Toggle:
				if (value.isBool()) {
					setting.boolValue = value.toBool(setting.boolValue);
				}
				break;
			case SettingControl::IntSlider:
				if (value.isDouble()) {
					setting.intValue = std::clamp(
						value.toInt(setting.intValue),
						setting.intMinimum,
						setting.intMaximum);
				}
				break;
			case SettingControl::TextInput:
				if (value.isString()) {
					setting.textValue = value.toString(setting.textValue).trimmed();
				}
				break;
			case SettingControl::ActionButton:
			case SettingControl::InfoText:
				break;
			}
		}
	}
}

void Manager::rememberSettingValue(
		const QString &pluginId,
		const SettingDescriptor &descriptor) {
	if (pluginId.trimmed().isEmpty() || descriptor.id.trimmed().isEmpty()) {
		return;
	}
	auto values = _storedSettings.value(pluginId);
	switch (descriptor.type) {
	case SettingControl::Toggle:
		values.insert(descriptor.id, descriptor.boolValue);
		break;
	case SettingControl::IntSlider:
		values.insert(descriptor.id, descriptor.intValue);
		break;
	case SettingControl::TextInput:
		values.insert(descriptor.id, descriptor.textValue);
		break;
	case SettingControl::ActionButton:
	case SettingControl::InfoText:
		values.remove(descriptor.id);
		break;
	}
	if (values.isEmpty()) {
		_storedSettings.remove(pluginId);
	} else {
		_storedSettings.insert(pluginId, values);
	}
}

int Manager::findRecordIndex(const QString &pluginId) const {
	const auto key = pluginId.trimmed();
	if (key.isEmpty()) {
		return -1;
	}
	const auto indexed = _pluginIndexById.constFind(key);
	if (indexed != _pluginIndexById.cend()) {
		const auto index = indexed.value();
		if (index >= 0
			&& index < int(_plugins.size())
			&& _plugins[index].state.info.id == key) {
			return index;
		}
	}
	auto fallback = -1;
	for (auto i = 0, count = int(_plugins.size()); i != count; ++i) {
		if (_plugins[i].state.info.id != key) {
			continue;
		}
		if (fallback >= 0) {
			return -1;
		}
		fallback = i;
	}
	return fallback;
}

void Manager::rebuildPluginIndex() {
	_pluginIndexById.clear();
	for (auto i = 0, count = int(_plugins.size()); i != count; ++i) {
		const auto id = _plugins[i].state.info.id.trimmed();
		if (!id.isEmpty() && !_pluginIndexById.contains(id)) {
			_pluginIndexById.insert(id, i);
		}
	}
}

void Manager::moveLastPluginRecordToIndex(int index) {
	if (_plugins.empty()) {
		return;
	}
	const auto lastIndex = int(_plugins.size()) - 1;
	if (index < 0 || index >= lastIndex) {
		rebuildPluginIndex();
		return;
	}
	auto moved = std::move(_plugins.back());
	_plugins.pop_back();
	_plugins.insert(_plugins.begin() + index, std::move(moved));
	rebuildPluginIndex();
}

void Manager::unloadPluginRecord(
		PluginRecord &record,
		bool preserveLoadError) {
	const auto pluginId = record.state.info.id.trimmed();
	const auto hadInstance = (record.state.loaded && record.instance != nullptr);
	const auto hadLibrary = (record.library != nullptr);
	const auto hadRegistrations = !record.commandIds.isEmpty()
		|| !record.actionIds.isEmpty()
		|| !record.panelIds.isEmpty()
		|| !record.settingsPageIds.isEmpty()
		|| !record.outgoingInterceptorIds.isEmpty()
		|| !record.messageObserverIds.isEmpty();
	if (!pluginId.isEmpty()) {
		logEvent(
			u"unload"_q,
			u"single-begin"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"path"_q, record.state.path },
				{ u"hadInstance"_q, hadInstance },
				{ u"hadLibrary"_q, hadLibrary },
				{ u"hadRegistrations"_q, hadRegistrations },
				{ u"registrations"_q, registrationSummaryToJson(record) },
			});
	}
	unregisterPluginCommands(pluginId);
	unregisterPluginActions(pluginId);
	unregisterPluginPanels(pluginId);
	unregisterPluginSettingsPages(pluginId);
	unregisterPluginOutgoingInterceptors(pluginId);
	unregisterPluginMessageObservers(pluginId);
	unregisterPluginWindowHandlers(pluginId);
	unregisterPluginWindowWidgetHandlers(pluginId);
	unregisterPluginSessionHandlers(pluginId);
	if (record.instance) {
		_registeringPluginId = pluginId;
		try {
			InvokePluginCallbackOrThrow([&] {
				record.instance->onUnload();
			});
		} catch (...) {
			if (!preserveLoadError || record.state.error.trimmed().isEmpty()) {
				record.state.error = u"onUnload failed: "_q + CurrentExceptionText();
			}
			logEvent(
				u"unload"_q,
				u"single-onunload-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, pluginId },
					{ u"path"_q, record.state.path },
					{ u"reason"_q, CurrentExceptionText() },
				});
		}
		_registeringPluginId.clear();
		record.instance.reset();
	}
	record.commandIds.clear();
	record.actionIds.clear();
	record.panelIds.clear();
	record.settingsPageIds.clear();
	record.outgoingInterceptorIds.clear();
	record.messageObserverIds.clear();
	record.state.loaded = false;
	if (record.library) {
		record.library->unload();
		record.library.reset();
	}
	FlushPluginUnload();
	if (!pluginId.isEmpty()) {
		logEvent(
			u"unload"_q,
			u"single-finish"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"path"_q, record.state.path },
				{ u"loaded"_q, record.state.loaded },
				{ u"error"_q, record.state.error },
			});
	}
}

bool Manager::removePluginFileReliable(
		const QString &path,
		QString *finalPath,
		QString *error,
		bool *scheduledOnReboot) {
	if (finalPath) {
		*finalPath = path;
	}
	if (scheduledOnReboot) {
		*scheduledOnReboot = false;
	}
	if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
		if (error) {
			error->clear();
		}
		return true;
	}
	auto effectivePath = path;
	auto removeError = QString();
	if (RemoveFileWithRetries(effectivePath, &removeError)) {
		if (finalPath) {
			*finalPath = effectivePath;
		}
		if (error) {
			error->clear();
		}
		return true;
	}

	const auto stagedPath = PluginDeleteStagingPath(path);
	auto renameError = QString();
	if (RenameFileWithRetries(path, stagedPath, &renameError)) {
		effectivePath = stagedPath;
		removeError.clear();
		if (RemoveFileWithRetries(effectivePath, &removeError)) {
			if (finalPath) {
				*finalPath = effectivePath;
			}
			if (error) {
				error->clear();
			}
			return true;
		}
#if defined(_WIN32)
		if (ScheduleDeleteOnReboot(effectivePath)) {
			if (finalPath) {
				*finalPath = effectivePath;
			}
			if (scheduledOnReboot) {
				*scheduledOnReboot = true;
			}
			if (error) {
				error->clear();
			}
			return true;
		}
#endif // _WIN32
	} else if (!renameError.trimmed().isEmpty()) {
		removeError = renameError;
	}

	if (finalPath) {
		*finalPath = effectivePath;
	}
	if (error) {
		*error = removeError.trimmed().isEmpty()
			? u"Could not delete the plugin package file."_q
			: removeError.trimmed();
	}
	return false;
}

void Manager::logLoadFailure(const QString &path, const QString &reason) const {
	logEvent(
		u"load"_q,
		u"failed"_q,
		QJsonObject{
			{ u"reason"_q, reason },
			{ u"sha256"_q, fileSha256(path) },
			{ u"file"_q, fileInfoToJson(QFileInfo(path)) },
		});
}

void Manager::scanPlugins(bool metadataOnly) {
	QDir().mkpath(_pluginsPath);
	auto dir = QDir(_pluginsPath);
	const auto files = dir.entryInfoList(
		{ u"*.tgd"_q },
		QDir::Files,
		QDir::Name | QDir::IgnoreCase);
	logEvent(
		u"scan"_q,
		u"start"_q,
		QJsonObject{
			{ u"metadataOnly"_q, metadataOnly },
			{ u"pluginsPath"_q, _pluginsPath },
			{ u"count"_q, files.size() },
		});
	for (const auto &info : files) {
		logEvent(
			u"scan"_q,
			u"discovered-file"_q,
			QJsonObject{
				{ u"metadataOnly"_q, metadataOnly },
				{ u"file"_q, fileInfoToJson(info) },
				{ u"sha256"_q, fileSha256(info.absoluteFilePath()) },
			});
		if (metadataOnly) {
			loadPluginMetadataOnly(info.absoluteFilePath());
		} else {
			loadPlugin(info.absoluteFilePath());
		}
	}
	logEvent(
		u"scan"_q,
		u"finish"_q,
		QJsonObject{
			{ u"metadataOnly"_q, metadataOnly },
			{ u"loadedPlugins"_q, int(_plugins.size()) },
		});
}

void Manager::loadPluginMetadataOnly(const QString &path) {
	auto record = PluginRecord();
	record.state.path = path;
	record.state.enabled = true;
	record.state.loaded = false;
	record.state.info.id = PluginBaseName(path);
	record.state.info.name = record.state.info.id;

	auto previewInfo = PluginInfo();
	auto previewIcon = QString();
	if (ReadPreviewManifest(path, &previewInfo, &previewIcon)) {
		MergePluginInfo(record.state.info, previewInfo);
		logEvent(
			u"plugin"_q,
			u"metadata-preview"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"icon"_q, previewIcon },
				{ u"plugin"_q, pluginInfoToJson(previewInfo) },
			});
	} else {
		logEvent(
			u"plugin"_q,
			u"metadata-preview-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
	}
	record.state.info.id = record.state.info.id.trimmed();
	if (record.state.info.name.isEmpty()) {
		record.state.info.name = record.state.info.id;
	}
	record.state.enabled = !_disabled.contains(record.state.info.id);
	syncRecoveryFlags(record.state);
	const auto index = int(_plugins.size());
	_plugins.push_back(std::move(record));
	_pluginIndexById.insert(_plugins.back().state.info.id, index);
	logEvent(
		u"plugin"_q,
		u"metadata-loaded"_q,
		QJsonObject{
			{ u"state"_q, pluginStateToJson(_plugins.back().state) },
		});
}

void Manager::loadPlugin(const QString &path) {
	auto record = PluginRecord();
	record.state.path = path;
	record.state.enabled = false;
	record.state.loaded = false;
	record.state.info.id = PluginBaseName(path);
	record.state.info.name = record.state.info.id;
	logEvent(
		u"load"_q,
		u"begin"_q,
		QJsonObject{
			{ u"path"_q, path },
			{ u"file"_q, fileInfoToJson(QFileInfo(path)) },
			{ u"sha256"_q, fileSha256(path) },
			{ u"baseName"_q, record.state.info.id },
		});

	auto previewManifestInfo = PluginInfo();
	auto previewManifestIcon = QString();
	const auto hasPreviewManifest = ReadPreviewManifest(
		path,
		&previewManifestInfo,
		&previewManifestIcon);
	if (hasPreviewManifest) {
		MergePluginInfo(record.state.info, previewManifestInfo);
		logEvent(
			u"load"_q,
			u"preview-manifest-read"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"icon"_q, previewManifestIcon },
				{ u"plugin"_q, pluginInfoToJson(previewManifestInfo) },
			});
	} else {
		logEvent(
			u"load"_q,
			u"preview-manifest-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
	}
	record.state.info.id = record.state.info.id.trimmed();
	if (record.state.info.name.isEmpty()) {
		record.state.info.name = record.state.info.id;
	}
	if (record.state.info.id.isEmpty()) {
		record.state.error = u"Plugin id is empty."_q;
	}
	if (_pluginIndexById.contains(record.state.info.id)) {
		record.state.error = u"Duplicate plugin id."_q;
	}
	record.state.enabled = !_disabled.contains(record.state.info.id);
	syncRecoveryFlags(record.state);
	if (!record.state.error.isEmpty()) {
		logLoadFailure(path, record.state.error);
		_plugins.push_back(std::move(record));
		return;
	}
	if (!record.state.enabled) {
		logEvent(
			u"load"_q,
			u"skip-disabled-metadata-only"_q,
			QJsonObject{
				{ u"state"_q, pluginStateToJson(record.state) },
			});
		const auto index = int(_plugins.size());
		_plugins.push_back(std::move(record));
		_pluginIndexById.insert(_plugins.back().state.info.id, index);
		return;
	}

	startRecoveryOperation(u"load"_q, { record.state.info.id }, path);

	auto library = std::make_unique<QLibrary>(path);
	if (!library->load()) {
		record.state.error = library->errorString();
		logLoadFailure(path, record.state.error);
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	const auto previewInfo = reinterpret_cast<PreviewInfoFn>(
		library->resolve(kPreviewInfoName));
	if (previewInfo) {
		logEvent(
			u"load"_q,
			u"preview-export-found"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
		auto preview = static_cast<const Plugins::PreviewInfo*>(nullptr);
		try {
			InvokePluginCallbackOrThrow([&] {
				preview = previewInfo();
			});
		} catch (...) {
			record.state.error = u"Plugin preview export failed: "_q + CurrentExceptionText();
			logLoadFailure(path, record.state.error);
			library->unload();
			_plugins.push_back(std::move(record));
			finishRecoveryOperation();
			return;
		}
		if (preview
			&& preview->structVersion == kPreviewInfoVersion
			&& preview->apiVersion == kApiVersion) {
			MergePluginInfo(record.state.info, PluginInfoFromPreview(*preview));
			logEvent(
				u"load"_q,
				u"preview-export-read"_q,
				QJsonObject{
					{ u"path"_q, path },
					{ u"plugin"_q, pluginInfoToJson(record.state.info) },
					{ u"icon"_q, PreviewIconFromInfo(*preview) },
				});
			if (record.state.info.name.isEmpty()) {
				record.state.info.name = record.state.info.id;
			}
		} else {
			logEvent(
				u"load"_q,
				u"preview-export-invalid"_q,
				QJsonObject{
					{ u"path"_q, path },
				});
		}
	} else {
		logEvent(
			u"load"_q,
			u"preview-export-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
	}
	const auto entry = reinterpret_cast<EntryFn>(
		library->resolve(kEntryName));
	const auto binaryInfo = reinterpret_cast<BinaryInfoFn>(
		library->resolve(kBinaryInfoName));
	if (!binaryInfo) {
		record.state.error = u"Missing TgdPluginBinaryInfo export."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	auto info = static_cast<const Plugins::BinaryInfo*>(nullptr);
	try {
		InvokePluginCallbackOrThrow([&] {
			info = binaryInfo();
		});
	} catch (...) {
		record.state.error = u"TgdPluginBinaryInfo failed: "_q + CurrentExceptionText();
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	if (!info) {
		record.state.error = u"Plugin binary metadata is null."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	logEvent(
		u"load"_q,
		u"binary-info"_q,
		QJsonObject{
			{ u"path"_q, path },
			{ u"binary"_q, binaryInfoToJson(*info) },
			{ u"hostBinary"_q, binaryInfoToJson(kBinaryInfo) },
		});
	if (const auto mismatch = DescribeBinaryInfoMismatch(*info);
		!mismatch.isEmpty()) {
		record.state.error = mismatch;
		logEvent(
			u"load"_q,
			u"abi-mismatch"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"reason"_q, mismatch },
				{ u"field"_q, DescribeBinaryInfoMismatchField(*info) },
				{ u"pluginBinary"_q, binaryInfoToJson(*info) },
				{ u"hostBinary"_q, binaryInfoToJson(kBinaryInfo) },
			});
		logLoadFailure(path, mismatch + u" ["_q + DescribeBinaryInfo(*info) + u"]"_q);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	if (!entry) {
		record.state.error = u"Missing TgdPluginEntry export."_q;
		logEvent(
			u"load"_q,
			u"entry-export-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}

	auto instance = std::unique_ptr<Plugin>();
	try {
		logEvent(
			u"load"_q,
			u"entry-call"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"apiVersion"_q, kApiVersion },
			});
		InvokePluginCallbackOrThrow([&] {
			instance.reset(entry(this, kApiVersion));
		});
	} catch (...) {
		record.state.error = u"Plugin entry failed: "_q + CurrentExceptionText();
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	if (!instance) {
		record.state.error = u"Plugin entry returned null."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}

	if (hasPreviewManifest) {
		logEvent(
			u"load"_q,
			u"info-call-skipped-preview-manifest"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"pluginId"_q, record.state.info.id },
			});
	} else {
		try {
			logEvent(
				u"load"_q,
				u"info-call"_q,
				QJsonObject{
					{ u"path"_q, path },
				});
			InvokePluginCallbackOrThrow([&] {
				record.state.info = instance->info();
			});
		} catch (...) {
			record.state.error = u"Plugin info() failed: "_q + CurrentExceptionText();
			logLoadFailure(path, record.state.error);
			instance.reset();
			library->unload();
			_plugins.push_back(std::move(record));
			finishRecoveryOperation();
			return;
		}
	}
	record.state.info.id = record.state.info.id.trimmed();
	if (record.state.info.name.isEmpty()) {
		record.state.info.name = record.state.info.id;
	}
	if (record.state.info.id.isEmpty()) {
		record.state.error = u"Plugin id is empty."_q;
	}
	if (_pluginIndexById.contains(record.state.info.id)) {
		record.state.error = u"Duplicate plugin id."_q;
	}

	record.state.enabled = !_disabled.contains(record.state.info.id);
	syncRecoveryFlags(record.state);

	if (!record.state.error.isEmpty()) {
		logLoadFailure(path, record.state.error);
		record.state.enabled = false;
		instance.reset();
		library->unload();
		record.state.loaded = false;
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}

	if (record.state.enabled) {
		record.library = std::move(library);
		record.instance = std::move(instance);
		record.state.loaded = true;
		logEvent(
			u"load"_q,
			u"library-loaded"_q,
			QJsonObject{
				{ u"state"_q, pluginStateToJson(record.state) },
			});
	} else {
		instance.reset();
		library->unload();
		logEvent(
			u"load"_q,
			u"library-not-enabled"_q,
			QJsonObject{
				{ u"state"_q, pluginStateToJson(record.state) },
			});
	}

	const auto index = int(_plugins.size());
	_plugins.push_back(std::move(record));
	_pluginIndexById.insert(_plugins.back().state.info.id, index);

	if (_plugins.back().state.enabled) {
		startRecoveryOperation(
			u"onload"_q,
			{ _plugins.back().state.info.id },
			_plugins.back().state.path);
		_registeringPluginId = _plugins.back().state.info.id;
			try {
				logEvent(
					u"load"_q,
					u"onload-call"_q,
					QJsonObject{
						{ u"pluginId"_q, _plugins.back().state.info.id },
						{ u"path"_q, _plugins.back().state.path },
					});
				InvokePluginCallbackOrThrow([&] {
					_plugins.back().instance->onLoad();
				});
		} catch (...) {
			const auto pluginId = _plugins.back().state.info.id;
			_registeringPluginId.clear();
			disablePlugin(
				pluginId,
				u"onLoad failed: "_q + CurrentExceptionText());
			showToast(PluginUiText(
				u"Plugin failed to load and was disabled."_q,
				u"Плагин упал при загрузке и был выключен."_q));
			finishRecoveryOperation();
			return;
			}
			_registeringPluginId.clear();
			logEvent(
				u"load"_q,
				u"success"_q,
				QJsonObject{
					{ u"state"_q, pluginStateToJson(_plugins.back().state) },
					{ u"registrations"_q, registrationSummaryToJson(_plugins.back()) },
				});
			finishRecoveryOperation();
		} else {
			_plugins.back().instance.reset();
			_plugins.back().library.reset();
	}
	finishRecoveryOperation();
}

void Manager::unloadAll() {
	logEvent(
		u"unload"_q,
		u"begin"_q,
		QJsonObject{
			{ u"count"_q, int(_plugins.size()) },
		});
	for (auto &plugin : _plugins) {
		if (plugin.state.loaded || plugin.instance || plugin.library) {
			startRecoveryOperation(
				u"unload"_q,
				{ plugin.state.info.id },
				plugin.state.path);
			unloadPluginRecord(plugin, false);
			finishRecoveryOperation();
		}
	}
	_commands.clear();
	_commandIdByName.clear();
	_commandsByPlugin.clear();
	_nextCommandId = 1;
	_actions.clear();
	_actionsByPlugin.clear();
	_nextActionId = 1;
	_panels.clear();
	_panelsByPlugin.clear();
	_nextPanelId = 1;
	_settingsPages.clear();
	_settingsPagesByPlugin.clear();
	_nextSettingsPageId = 1;
	_outgoingInterceptors.clear();
	_outgoingInterceptorsByPlugin.clear();
	_nextOutgoingInterceptorId = 1;
	_messageObservers.clear();
	_messageObserversByPlugin.clear();
	_nextMessageObserverId = 1;
	_messageObserverLifetime.destroy();
	_windowHandlers.clear();
	_windowWidgetHandlers.clear();
	_sessionHandlers.clear();

	for (auto &plugin : _plugins) {
		plugin.commandIds.clear();
		plugin.actionIds.clear();
		plugin.panelIds.clear();
		plugin.settingsPageIds.clear();
		plugin.outgoingInterceptorIds.clear();
		plugin.messageObserverIds.clear();
		plugin.state.loaded = false;
		plugin.instance.reset();
		plugin.library.reset();
	}
	_plugins.clear();
	_pluginIndexById.clear();
	logEvent(u"unload"_q, u"finish"_q);
}

Manager::PluginRecord *Manager::findRecord(const QString &pluginId) {
	const auto index = findRecordIndex(pluginId);
	if (index < 0 || index >= int(_plugins.size())) {
		return nullptr;
	}
	return &_plugins[index];
}

const Manager::PluginRecord *Manager::findRecord(
		const QString &pluginId) const {
	const auto index = findRecordIndex(pluginId);
	if (index < 0 || index >= int(_plugins.size())) {
		return nullptr;
	}
	return &_plugins[index];
}

void Manager::unregisterPluginCommands(const QString &pluginId) {
	const auto ids = _commandsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterCommand(id);
	}
}

void Manager::unregisterPluginActions(const QString &pluginId) {
	const auto ids = _actionsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterAction(id);
	}
}

void Manager::unregisterPluginPanels(const QString &pluginId) {
	const auto ids = _panelsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterPanel(id);
	}
}

void Manager::unregisterPluginSettingsPages(const QString &pluginId) {
	const auto ids = _settingsPagesByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterSettingsPage(id);
	}
}

void Manager::unregisterPluginOutgoingInterceptors(const QString &pluginId) {
	const auto ids = _outgoingInterceptorsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterOutgoingTextInterceptor(id);
	}
}

void Manager::unregisterPluginMessageObservers(const QString &pluginId) {
	const auto ids = _messageObserversByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterMessageObserver(id);
	}
}

void Manager::unregisterPluginWindowHandlers(const QString &pluginId) {
	_windowHandlers.erase(
		std::remove_if(
			_windowHandlers.begin(),
			_windowHandlers.end(),
			[&](const WindowHandlerEntry &entry) {
				return entry.pluginId == pluginId;
			}),
		_windowHandlers.end());
}

void Manager::unregisterPluginWindowWidgetHandlers(const QString &pluginId) {
	_windowWidgetHandlers.erase(
		std::remove_if(
			_windowWidgetHandlers.begin(),
			_windowWidgetHandlers.end(),
			[&](const WindowWidgetHandlerEntry &entry) {
				return entry.pluginId == pluginId;
			}),
		_windowWidgetHandlers.end());
}

void Manager::unregisterPluginSessionHandlers(const QString &pluginId) {
	_sessionHandlers.erase(
		std::remove_if(
			_sessionHandlers.begin(),
			_sessionHandlers.end(),
			[&](const SessionHandlerEntry &entry) {
				return entry.pluginId == pluginId;
			}),
		_sessionHandlers.end());
}

QString Manager::commandKey(const QString &command) const {
	return NormalizeCommand(command);
}

bool Manager::hasPlugin(const QString &pluginId) const {
	if (pluginId.trimmed().isEmpty()) {
		return false;
	}
	return (findRecordIndex(pluginId) >= 0);
}

void Manager::disablePlugin(const QString &pluginId, const QString &reason) {
	disablePlugin(pluginId, reason, false, QString());
}

void Manager::disablePlugin(
		const QString &pluginId,
		const QString &reason,
		bool disabledByRecovery,
		const QString &recoveryReason) {
	auto *record = findRecord(pluginId);
	if (!record) {
		return;
	}
	appendLogLine(
		u"[disabled] "_q
		+ pluginId
		+ (reason.trimmed().isEmpty()
			? QString()
			: (u": "_q + reason.trimmed())));
	logEvent(
		u"plugin"_q,
		u"disabled"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"reason"_q, reason.trimmed() },
			{ u"disabledByRecovery"_q, disabledByRecovery },
			{ u"recoveryReason"_q, recoveryReason },
			{ u"stateBefore"_q, pluginStateToJson(record->state) },
			{ u"registrationsBefore"_q, registrationSummaryToJson(*record) },
		});
	record->state.enabled = false;
	record->state.loaded = false;
	if (!reason.trimmed().isEmpty()) {
		record->state.error = reason.trimmed();
	}
	record->state.disabledByRecovery = disabledByRecovery;
	record->state.recoverySuspected = disabledByRecovery;
	record->state.recoveryReason = recoveryReason;
	unloadPluginRecord(*record, true);
	_disabled.insert(pluginId);
	if (disabledByRecovery) {
		_disabledByRecovery.insert(pluginId);
	} else {
		_disabledByRecovery.remove(pluginId);
	}
	saveConfig();
	saveRecoveryState();
	logEvent(
		u"plugin"_q,
		u"disabled-finished"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"stateAfter"_q, pluginStateToJson(record->state) },
		});
}

void Manager::updateMessageObserverSubscriptions() {
	_messageObserverLifetime.destroy();
	if (!_activeSession || _messageObservers.isEmpty()) {
		logEvent(
			u"observer"_q,
			u"subscriptions-cleared"_q,
			QJsonObject{
				{ u"hasActiveSession"_q, _activeSession != nullptr },
				{ u"observerCount"_q, _messageObservers.size() },
			});
		return;
	}
	const auto observerSnapshot = [=] {
		auto result = std::vector<MessageObserverEntry>();
		result.reserve(_messageObservers.size());
		for (auto it = _messageObservers.cbegin();
			it != _messageObservers.cend();
			++it) {
			result.push_back(it.value());
		}
		return result;
	};
	auto wantsNew = false;
	auto wantsEdited = false;
	auto wantsDeleted = false;
	for (auto it = _messageObservers.cbegin();
		it != _messageObservers.cend();
		++it) {
		const auto &options = it.value().options;
		wantsNew |= options.newMessages;
		wantsEdited |= options.editedMessages;
		wantsDeleted |= options.deletedMessages;
		if (wantsNew && wantsEdited && wantsDeleted) {
			break;
		}
	}
	logEvent(
		u"observer"_q,
		u"subscriptions-updated"_q,
		QJsonObject{
			{ u"observerCount"_q, _messageObservers.size() },
			{ u"wantsNew"_q, wantsNew },
			{ u"wantsEdited"_q, wantsEdited },
			{ u"wantsDeleted"_q, wantsDeleted },
			{ u"sessionUniqueId"_q, _activeSession ? QString::number(_activeSession->uniqueId()) : QString() },
		});
	if (wantsNew) {
		_activeSession->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			auto context = MessageEventContext{
				.session = _activeSession,
				.history = update.item->history().get(),
				.item = update.item.get(),
				.event = MessageEvent::New,
			};
			const auto snapshot = observerSnapshot();
			for (const auto &entry : snapshot) {
				dispatchMessageEvent(
					_activeSession,
					context,
					entry.options,
					entry);
			}
		}, _messageObserverLifetime);
	}
	if (wantsEdited) {
		_activeSession->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::Edited
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			auto context = MessageEventContext{
				.session = _activeSession,
				.history = update.item->history().get(),
				.item = update.item.get(),
				.event = MessageEvent::Edited,
			};
			const auto snapshot = observerSnapshot();
			for (const auto &entry : snapshot) {
				dispatchMessageEvent(
					_activeSession,
					context,
					entry.options,
					entry);
			}
		}, _messageObserverLifetime);
	}
	if (wantsDeleted) {
		_activeSession->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::Destroyed
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			auto context = MessageEventContext{
				.session = _activeSession,
				.history = update.item->history().get(),
				.item = update.item.get(),
				.event = MessageEvent::Deleted,
			};
			const auto snapshot = observerSnapshot();
			for (const auto &entry : snapshot) {
				dispatchMessageEvent(
					_activeSession,
					context,
					entry.options,
					entry);
			}
		}, _messageObserverLifetime);
	}
}

void Manager::handleActiveSessionChanged(Main::Session *session) {
	_activeSession = session;
	logEvent(
		u"session"_q,
		u"active-changed"_q,
		QJsonObject{
			{ u"hasSession"_q, session != nullptr },
			{ u"sessionUniqueId"_q, session ? QString::number(session->uniqueId()) : QString() },
			{ u"handlerCount"_q, int(_sessionHandlers.size()) },
		});
	const auto handlers = _sessionHandlers;
	for (const auto &entry : handlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			startRecoveryOperation(u"session"_q, { entry.pluginId });
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			logEvent(
				u"session"_q,
				u"invoke-callback"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"hasSession"_q, session != nullptr },
					{ u"sessionUniqueId"_q, session ? QString::number(session->uniqueId()) : QString() },
				});
			InvokePluginCallbackOrThrow([&] {
				entry.handler(session);
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"session"_q,
				u"callback-success"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
				});
			finishRecoveryOperation();
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"session"_q,
				u"callback-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"reason"_q, CurrentExceptionText() },
				});
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Session callback failed: "_q + CurrentExceptionText());
			}
			showToast(PluginUiText(
				u"Plugin session callback failed."_q,
				u"Session callback плагина завершился с ошибкой."_q));
			finishRecoveryOperation();
		}
	}
	updateMessageObserverSubscriptions();
}

void Manager::dispatchMessageEvent(
		Main::Session *session,
		const MessageEventContext &context,
		const MessageObserverOptions &options,
		const MessageObserverEntry &entry) {
	if (!entry.handler) {
		return;
	}
	switch (context.event) {
	case MessageEvent::New:
		if (!options.newMessages) {
			return;
		}
		break;
	case MessageEvent::Edited:
		if (!options.editedMessages) {
			return;
		}
		break;
	case MessageEvent::Deleted:
		if (!options.deletedMessages) {
			return;
		}
		break;
	}
	const auto item = context.item;
	if (!item) {
		return;
	}
	const auto outgoing = item->out();
	if (outgoing && !options.outgoing) {
		return;
	}
	if (!outgoing && !options.incoming) {
		return;
	}
	auto callContext = context;
	callContext.session = session;
	try {
		startRecoveryOperation(u"observer"_q, { entry.pluginId });
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = entry.pluginId;
		logEvent(
			u"observer"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(entry.id) },
				{ u"pluginId"_q, entry.pluginId },
				{ u"context"_q, messageContextToJson(callContext) },
			});
		InvokePluginCallbackOrThrow([&] {
			entry.handler(callContext);
		});
		_registeringPluginId = previousPluginId;
		logEvent(
			u"observer"_q,
			u"success"_q,
			QJsonObject{
				{ u"id"_q, QString::number(entry.id) },
				{ u"pluginId"_q, entry.pluginId },
			});
		finishRecoveryOperation();
	} catch (...) {
		_registeringPluginId.clear();
		logEvent(
			u"observer"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(entry.id) },
				{ u"pluginId"_q, entry.pluginId },
				{ u"reason"_q, CurrentExceptionText() },
				{ u"context"_q, messageContextToJson(callContext) },
			});
		disablePlugin(
			entry.pluginId,
			u"Message observer failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin observer failed and was disabled."_q,
			u"Наблюдатель плагина завершился с ошибкой и был выключен."_q));
		finishRecoveryOperation();
	}
}

} // namespace Plugins
