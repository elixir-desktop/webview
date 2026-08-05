# Build DesktopWebView.exe (x64) and copy to priv/native/windows/
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path "$PSScriptRoot\..").Path }

$OutDir = Join-Path $Root "priv\native\windows"
$BuildDir = Join-Path $Root "native\windows\build"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

function Enter-VsDevShell {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
  $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vsPath) { throw "Visual Studio with MSVC not found" }
  $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

  $temp = [System.IO.Path]::GetTempFileName() + ".cmd"
  @"
@echo off
call "$vcvars" >nul
set > "$temp.env"
"@ | Set-Content -Path $temp -Encoding ASCII
  cmd /c $temp
  Get-Content "$temp.env" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
      Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
  }
  Remove-Item $temp -ErrorAction SilentlyContinue
  Remove-Item "$temp.env" -ErrorAction SilentlyContinue
}

# Ensure x64 cl is available
$cl = Get-Command cl -ErrorAction SilentlyContinue
if (-not $cl -or ($cl.Source -notmatch 'Hostx64\\x64')) {
  Write-Host "Entering VS x64 environment..."
  Enter-VsDevShell
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { throw "cmake not found on PATH" }

Push-Location $Root
try {
  & cmake -S (Join-Path $Root "native\windows") -B $BuildDir -G "Ninja" -DCMAKE_BUILD_TYPE=Release
  if ($LASTEXITCODE -ne 0) {
    # Fallback to default generator (VS)
    & cmake -S (Join-Path $Root "native\windows") -B $BuildDir -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
  }
  & cmake --build $BuildDir --config Release
  if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

  $exeCandidates = @(
    (Join-Path $BuildDir "DesktopWebView.exe"),
    (Join-Path $BuildDir "Release\DesktopWebView.exe"),
    (Join-Path $BuildDir "RelWithDebInfo\DesktopWebView.exe")
  )
  $built = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $built) { throw "DesktopWebView.exe not found under $BuildDir" }

  Copy-Item -Force $built (Join-Path $OutDir "DesktopWebView.exe")
  Write-Host "Built $(Join-Path $OutDir 'DesktopWebView.exe')"
} finally {
  Pop-Location
}
