@echo off
REM Runs a command inside the Visual Studio MSVC x64 developer environment with the
REM VS-bundled Ninja on PATH. Batch wrapper (no PowerShell execution-policy concerns).
REM
REM Usage:
REM   tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
REM   tools\dev.cmd cmake --build build --target eb_tests
REM   tools\dev.cmd ctest --test-dir build --output-on-failure
REM
REM Why: CMake 3.30 has no generator for VS 2026 (v18); we build with Ninja + MSVC.
REM vcvars is not persisted across separate tool invocations, so every build command
REM is wrapped through this script.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found at "%VSWHERE%"
    exit /b 1
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo ERROR: no Visual Studio install with VC.Tools.x86.x64 found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
%*
exit /b %ERRORLEVEL%
