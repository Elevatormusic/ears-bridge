@echo off
REM Build the Release app (self-contained exe) and package it into a Windows installer.
REM
REM Usage:  tools\build-installer.cmd
REM Output: dist\EARS-Bridge-<version>-Setup.exe
REM
REM Requires Inno Setup (one-time):  winget install JRSoftware.InnoSetup
setlocal

REM 1. Configure + build the app in Release (static MSVC runtime -> no VC++ redist needed).
call "%~dp0dev.cmd" cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release || exit /b 1
call "%~dp0dev.cmd" cmake --build build --target EarsBridge || exit /b 1

REM 2. Locate the Inno Setup compiler (ISCC.exe) across its common install locations.
set "ISCC="
for %%P in (
  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
  "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
  "%ProgramFiles%\Inno Setup 6\ISCC.exe"
) do if exist "%%~P" set "ISCC=%%~P"

if not defined ISCC (
  echo.
  echo ERROR: Inno Setup ^(ISCC.exe^) not found.
  echo Install it once with:  winget install JRSoftware.InnoSetup
  exit /b 1
)

REM 3. Compile the installer into dist\.
"%ISCC%" "%~dp0..\installer\ears-bridge.iss" || exit /b 1

echo.
echo ============================================================
echo  Installer written to:  %~dp0..\dist\
echo ============================================================
exit /b 0
