# Build DesktopWebView.exe (x64) and copy to priv/native/windows/
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path "$PSScriptRoot\..").Path }

$OutDir = Join-Path $Root "priv\native\windows"
$BuildDir = Join-Path $Root "native\windows\build"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio with MSVC not found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
$bat = Join-Path $env:TEMP ("edw-build-" + [guid]::NewGuid().ToString("n") + ".cmd")
@"
@echo off
call "$vcvars" || exit /b 1
cd /d "$Root"
cmake -S native\windows -B native\windows\build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
  rmdir /s /q native\windows\build 2>nul
  mkdir native\windows\build
  cmake -S native\windows -B native\windows\build -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build native\windows\build --config Release --parallel || exit /b 1
if exist native\windows\build\DesktopWebView.exe (
  copy /y native\windows\build\DesktopWebView.exe priv\native\windows\DesktopWebView.exe >nul
) else if exist native\windows\build\Release\DesktopWebView.exe (
  copy /y native\windows\build\Release\DesktopWebView.exe priv\native\windows\DesktopWebView.exe >nul
) else (
  echo DesktopWebView.exe not found
  exit /b 1
)
echo Built priv\native\windows\DesktopWebView.exe
"@ | Set-Content -Path $bat -Encoding ASCII

try {
  & $cmdExe /c $bat
  if ($LASTEXITCODE -ne 0) { throw "Windows host build failed with exit $LASTEXITCODE" }
} finally {
  Remove-Item $bat -ErrorAction SilentlyContinue
}
