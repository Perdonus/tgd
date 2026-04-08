@echo off
setlocal EnableExtensions

where astro >nul 2>nul
if errorlevel 1 (
  echo Astrogram installs astro.bat on startup and adds it to PATH automatically.
  echo Start Astrogram once, then run commands like:
  echo   astro status
  echo   astro plugins
  echo   astro send ^<peerId^> ^<text...^>
  exit /b 1
)

astro %*
