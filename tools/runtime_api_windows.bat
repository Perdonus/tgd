@echo off
setlocal EnableExtensions

set "ASTRO_CMD="
where astro >nul 2>nul
if not errorlevel 1 set "ASTRO_CMD=astro"

if not defined ASTRO_CMD (
  for %%I in (
    "%~dp0..\tdata\bin\astro.bat"
    "%~dp0..\Telegram\tdata\bin\astro.bat"
    "%~dp0..\out\Release\tdata\bin\astro.bat"
    "%~dp0..\Telegram\out\Release\tdata\bin\astro.bat"
  ) do (
    if not defined ASTRO_CMD if exist "%%~I" set "ASTRO_CMD=%%~fI"
  )
)

if not defined ASTRO_CMD (
  echo Astrogram installs astro.bat on startup and adds it to PATH automatically.
  echo Start Astrogram once, then run commands like:
  echo   astro status
  echo   astro plugins
  echo   astro logs client
  echo   astro send ^<peerId^> ^<text...^>
  exit /b 1
)

if /I "%ASTRO_CMD%"=="astro" (
  call astro %*
) else (
  call "%ASTRO_CMD%" %*
)
exit /b %errorlevel%
