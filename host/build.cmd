@echo off
REM ---------------------------------------------------------------------------
REM Builds the farever-modkit dxgi.dll proxy (x64).
REM
REM Output: host\build\dxgi.dll
REM
REM The build does NOT install into the game. Copying a dxgi.dll next to
REM Farever.exe changes what loads at the next launch, so that stays a
REM deliberate manual step - see host\README.md.
REM ---------------------------------------------------------------------------
setlocal

set "VS=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
  set "VS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
)
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: could not find vcvars64.bat. Install the MSVC C++ x64 toolset.
  exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /LD /O2 /MT /W3 /EHsc /std:c++17 ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:build\ /Fe:build\dxgi.dll ^
   src\dllmain.cpp ^
   /link /DEF:dxgi.def /OUT:build\dxgi.dll kernel32.lib

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo.
echo built: %~dp0build\dxgi.dll
endlocal
