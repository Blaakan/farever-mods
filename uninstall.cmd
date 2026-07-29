@echo off
REM ---------------------------------------------------------------------------
REM Removes the Collection Atlas from your copy of Farever.
REM
REM   uninstall.cmd            remove the DLL, keep the generated data
REM   uninstall.cmd --purge    remove the generated data as well
REM
REM Routes you recorded yourself (farever-routes-custom.txt) are never
REM removed without --purge --force. Deleting the one dxgi.dll by hand does
REM the same job as this script.
REM ---------------------------------------------------------------------------
setlocal

cd /d "%~dp0"

where node >nul 2>&1
if errorlevel 1 (
  echo.
  echo   Node.js is not installed, so this script cannot run.
  echo.
  echo   You do not need it to uninstall: delete dxgi.dll from the folder
  echo   containing Farever.exe and the game is back to stock.
  echo.
  pause
  exit /b 1
)

node tools\install.mjs --uninstall %*
set "RC=%ERRORLEVEL%"

echo.
pause
exit /b %RC%
