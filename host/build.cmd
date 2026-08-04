@echo off
REM ---------------------------------------------------------------------------
REM Builds the farever-modkit dxgi.dll proxy (x64).
REM
REM Output: host\build\dxgi.dll
REM
REM The build does NOT install into the game. Copying a dxgi.dll next to
REM Farever.exe changes what loads at the next launch, so that stays a
REM deliberate step - install.cmd in the repo root does it.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

REM --- find the compiler -----------------------------------------------------
REM
REM vswhere.exe is installed by every Visual Studio 2017+ installer, always at
REM this one path, and it knows about every edition, channel and install drive.
REM Probing for "2022\Community" finds one machine's layout; asking the
REM installer finds everybody's.

if defined VCINSTALLDIR goto :have_env

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VCVARS="
if exist "%VSWHERE%" (
  REM -latest across all products, including Build Tools and Preview, that
  REM actually carry the x64 C++ toolset rather than merely being installed.
  for /f "usebackq tokens=*" %%i in (`
    "%VSWHERE%" -latest -prerelease -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath 2^>nul
  `) do set "VS=%%i"
  if defined VS if exist "!VS!\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=!VS!\VC\Auxiliary\Build\vcvars64.bat"
  )
)

REM Fallback for a machine where vswhere is missing or reports nothing: the
REM well-known layouts, across both Program Files roots and both recent
REM versions. Ordered newest-first, editions in the order a developer is
REM likely to have them.
if not defined VCVARS (
  for %%r in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for %%v in (2026 2022 2019) do (
      for %%e in (Community Professional Enterprise BuildTools Preview) do (
        if not defined VCVARS (
          if exist "%%~r\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS=%%~r\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat"
          )
        )
      )
    )
  )
)

if not defined VCVARS (
  echo ERROR: no MSVC x64 toolset found.
  echo.
  echo Install "Desktop development with C++" from either:
  echo   Visual Studio 2022 Community  https://visualstudio.microsoft.com/vs/community/
  echo   Build Tools for Visual Studio https://visualstudio.microsoft.com/downloads/
  echo.
  echo The workload must include the MSVC v143 x64 build tools and a Windows
  echo 10/11 SDK. If you already have them, run this from a
  echo "x64 Native Tools Command Prompt" instead.
  exit /b 1
)

echo using !VCVARS!
call "!VCVARS!" >nul
if errorlevel 1 (
  echo ERROR: vcvars64.bat failed.
  exit /b 1
)

:have_env
where cl.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: cl.exe is not on PATH even after initialising the toolset.
  exit /b 1
)

cd /d "%~dp0"
if not exist build mkdir build

REM /EHa: the reader guards game-memory dereferences with SEH __try/__except,
REM which needs asynchronous exception handling.
REM /MT: static CRT, so the DLL does not need the VC++ redistributable to be
REM installed on the machine that runs it. This is a mod other people
REM download; a missing runtime would stop the game from starting at all.
cl /nologo /LD /O2 /MT /W3 /EHa /std:c++17 ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:build\ /Fe:build\dxgi.dll ^
   src\dllmain.cpp src\hl_runtime.cpp src\hl_scan.cpp src\hl_reader.cpp ^
   src\dxgi_wrap.cpp src\overlay_d3d12.cpp src\input.cpp src\atlas_ui.cpp ^
   src\navigator.cpp src\routes.cpp src\loot.cpp src\mapwatch.cpp ^
   src\chat.cpp src\players.cpp src\report.cpp src\paths.cpp src\dashboard.cpp ^
   /link /DEF:dxgi.def /OUT:build\dxgi.dll kernel32.lib advapi32.lib ^
   user32.lib gdi32.lib dxgi.lib d3d12.lib d3dcompiler.lib

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo.
echo built: %~dp0build\dxgi.dll
endlocal
