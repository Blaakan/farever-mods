@echo off
REM ---------------------------------------------------------------------------
REM Installs the Collection Atlas into your copy of Farever.
REM
REM Double-click this, or run it from a command prompt. It finds the game,
REM builds the item database out of your own game files, and copies one DLL
REM next to Farever.exe.
REM
REM   install.cmd                      find the game and install
REM   install.cmd --game "D:\path"     point at the install yourself
REM ---------------------------------------------------------------------------
setlocal

cd /d "%~dp0"

where node >nul 2>&1
if errorlevel 1 (
  echo.
  echo   Node.js is required, and is not installed.
  echo.
  echo   The mod's item database, icons and routes are built from YOUR copy
  echo   of the game - they are the game's own data, which is not ours to
  echo   redistribute - and the scripts that read it run on Node.
  echo.
  echo   Install it with either:
  echo       winget install OpenJS.NodeJS.LTS
  echo   or from  https://nodejs.org/  ^(the LTS build^)
  echo.
  echo   Then run this again. Nothing has been changed.
  echo.
  pause
  exit /b 1
)

node tools\install.mjs %*
set "RC=%ERRORLEVEL%"

echo.
pause
exit /b %RC%
