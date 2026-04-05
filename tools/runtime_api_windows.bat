@echo off
setlocal EnableExtensions

REM Astrogram Runtime API helper (Windows)
REM Usage:
REM   runtime_api_windows.bat status
REM   runtime_api_windows.bat enable
REM   runtime_api_windows.bat disable
REM   runtime_api_windows.bat ping
REM
REM The client reads runtime API state from tdata\plugins.json -> runtimeApi.

set "CFG=%~dp0..\tdata\plugins.json"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=status"

if /I "%MODE%"=="status" goto :status
if /I "%MODE%"=="enable" goto :enable
if /I "%MODE%"=="disable" goto :disable
if /I "%MODE%"=="ping" goto :ping

echo Unknown command: %MODE%
exit /b 2

:status
if not exist "%CFG%" (
  echo plugins.json not found: %CFG%
  exit /b 1
)
powershell -NoProfile -Command ^
  "$cfg = Get-Content -LiteralPath '%CFG%' -Raw | ConvertFrom-Json;" ^
  "$api = $cfg.runtimeApi;" ^
  "if (-not $api) { Write-Host 'runtimeApi: missing'; exit 0 }" ^
  "Write-Host ('enabled=' + [bool]$api.enabled);" ^
  "Write-Host ('port=' + [int]$api.port);" ^
  "if ($api.enabled) { Write-Host ('base_url=http://127.0.0.1:' + [int]$api.port) }"
exit /b %errorlevel%

:enable
powershell -NoProfile -Command ^
  "$path='%CFG%';" ^
  "if (Test-Path $path) { $cfg = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } else { $cfg = [pscustomobject]@{} }" ^
  "if (-not $cfg.PSObject.Properties['runtimeApi']) { $cfg | Add-Member -NotePropertyName runtimeApi -NotePropertyValue ([pscustomobject]@{ enabled = $true; port = 37080 }) }" ^
  "$cfg.runtimeApi.enabled = $true;" ^
  "if (-not $cfg.runtimeApi.port) { $cfg.runtimeApi.port = 37080 }" ^
  "$cfg | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding UTF8;" ^
  "Write-Host 'runtimeApi enabled in plugins.json';"
exit /b %errorlevel%

:disable
powershell -NoProfile -Command ^
  "$path='%CFG%';" ^
  "if (-not (Test-Path $path)) { Write-Host 'plugins.json not found'; exit 1 }" ^
  "$cfg = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json;" ^
  "if (-not $cfg.PSObject.Properties['runtimeApi']) { $cfg | Add-Member -NotePropertyName runtimeApi -NotePropertyValue ([pscustomobject]@{ enabled = $false; port = 37080 }) }" ^
  "$cfg.runtimeApi.enabled = $false;" ^
  "$cfg | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding UTF8;" ^
  "Write-Host 'runtimeApi disabled in plugins.json';"
exit /b %errorlevel%

:ping
for /f "tokens=1,2 delims==" %%A in ('%~f0 status ^| findstr /I "enabled= port= base_url="') do (
  if /I "%%A"=="enabled" set "ENABLED=%%B"
  if /I "%%A"=="base_url" set "BASE=%%B"
)
if /I not "%ENABLED%"=="True" (
  echo runtimeApi is disabled.
  exit /b 1
)
if "%BASE%"=="" (
  echo runtimeApi base_url missing.
  exit /b 1
)
where curl >nul 2>nul
if errorlevel 1 (
  echo curl not found. Install curl or use browser: %BASE%
  exit /b 1
)
echo Probing %BASE% ...
curl -fsS "%BASE%" || exit /b 1
echo.
echo runtimeApi probe finished.
exit /b 0
