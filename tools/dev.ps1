# Enters the Visual Studio MSVC x64 developer environment, puts the VS-bundled
# Ninja on PATH, then runs the passed command (e.g. cmake / ctest).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
#   powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake --build build --target eb_tests
#   powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ctest --test-dir build --output-on-failure
#
# Why: CMake 3.30 has no generator for VS 2026 (v18), so we build with the Ninja
# generator + MSVC toolchain. vcvars is not persisted across separate tool
# invocations, so every build command is wrapped through this script: it captures
# the environment vcvars64.bat produces and imports it before running the command.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)] [string[]] $Command)

$ErrorActionPreference = 'Stop'

$vsw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsw)) { throw "vswhere not found at $vsw" }
$vsRoot = & $vsw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw "No Visual Studio install with VC.Tools.x86.x64 found" }

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Run vcvars in a child cmd and capture the resulting environment.
$tmp = [System.IO.Path]::GetTempFileName()
try {
    & cmd.exe /c "call `"$vcvars`" >nul 2>&1 && set > `"$tmp`""
    foreach ($line in Get-Content $tmp) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

$ninjaDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }

$exe = $Command[0]
$rest = @()
if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }
& $exe @rest
exit $LASTEXITCODE
