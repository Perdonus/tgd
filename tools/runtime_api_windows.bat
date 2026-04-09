@echo off
setlocal EnableExtensions

if exist "%~dp0astro.env" call "%~dp0astro.env"

if not defined ASTRO_RUNTIME_HOST set "ASTRO_RUNTIME_HOST=127.0.0.1"
if not defined ASTRO_RUNTIME_PORT set "ASTRO_RUNTIME_PORT=37080"
if not defined ASTRO_BASE_URL set "ASTRO_BASE_URL=http://%ASTRO_RUNTIME_HOST%:%ASTRO_RUNTIME_PORT%"
if not defined ASTRO_CLIENT_LOG if defined ASTRO_WORKING_DIR set "ASTRO_CLIENT_LOG=%ASTRO_WORKING_DIR%client.log"
if not defined ASTRO_PLUGINS_LOG if defined ASTRO_WORKING_DIR set "ASTRO_PLUGINS_LOG=%ASTRO_WORKING_DIR%tdata\plugins.log"
if not defined ASTRO_PLUGINS_CONFIG if defined ASTRO_WORKING_DIR set "ASTRO_PLUGINS_CONFIG=%ASTRO_WORKING_DIR%tdata\plugins.json"

if "%~1"=="" goto help
if /I "%~1"=="help" goto help
if /I "%~1"=="status" goto status
if /I "%~1"=="health" goto health
if /I "%~1"=="host" goto host
if /I "%~1"=="system" goto system
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
  "[pscustomobject]@{ runtimeReachable = $reachable; baseUrl = $base; configuredEnabled = $configuredEnabled; configuredPort = $configuredPort; workingDir = $env:ASTRO_WORKING_DIR; clientLog = $env:ASTRO_CLIENT_LOG; pluginsLog = $env:ASTRO_PLUGINS_LOG; health = $health } | ConvertTo-Json -Depth 16"
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
shift
if "%~1"=="" goto help_send
set "ASTRO_ARG2=%*"
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

:help_runtime
echo Usage: astro runtime on [port]
echo        astro runtime off
exit /b 1

:help_plugins
echo Usage: astro plugins
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
echo   astro status
echo   astro health
echo   astro host
echo   astro system
echo   astro runtime on [port]
echo   astro runtime off
echo   astro plugins
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
echo.
echo Runtime API commands work live when the client runtime server is enabled.
echo If it is currently offline, astro runtime on/off updates the saved config for the next Astrogram start.
exit /b 0
