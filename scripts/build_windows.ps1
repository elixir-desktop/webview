# Build DesktopWebView.exe + aumid_nif.dll (x64) into priv/native/windows/
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

function Get-ErtsInclude {
  $elixir = Get-Command elixir -ErrorAction SilentlyContinue
  if (-not $elixir) { return $null }
  # elixir.bat + powershell -e "(...)" breaks on parentheses; use a temp script.
  $exs = Join-Path $env:TEMP ("edw-erts-" + [guid]::NewGuid().ToString("n") + ".exs")
  try {
    Set-Content -Path $exs -Encoding ASCII -Value @(
      'IO.write(Path.join(['
      '  List.to_string(:code.root_dir()),'
      '  "erts-#{List.to_string(:erlang.system_info(:version))}",'
      '  "include"'
      ']))'
    )
    $path = & elixir $exs
    if ($LASTEXITCODE -ne 0 -or -not $path -or -not (Test-Path (Join-Path $path "erl_nif.h"))) {
      return $null
    }
    return $path
  } finally {
    Remove-Item $exs -ErrorAction SilentlyContinue
  }
}

$ertsInclude = Get-ErtsInclude

$cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
$bat = Join-Path $env:TEMP ("edw-build-" + [guid]::NewGuid().ToString("n") + ".cmd")

# enif_* resolve via WinDynNifCallbacks (no erl_dll.lib). shell32/ole32 for AUMID.
$nifBlock = "echo aumid_nif: skipped (elixir/erl_nif.h not found)"
if ($ertsInclude) {
  $nifSrc = Join-Path $Root "c_src\aumid_nif.c"
  $nifDll = Join-Path $OutDir "aumid_nif.dll"
  $nifObj = Join-Path $BuildDir "aumid_nif.obj"
  $nifExp = Join-Path $OutDir "aumid_nif.exp"
  $nifLib = Join-Path $OutDir "aumid_nif.lib"
  $nifBlock = @"
cl /nologo /LD /O2 /MD /I"$ertsInclude" "$nifSrc" /Fo"$nifObj" /Fe"$nifDll" /link shell32.lib ole32.lib || exit /b 1
del /q "$nifExp" "$nifLib" 2>nul
echo Built priv\native\windows\aumid_nif.dll
"@
}

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
$nifBlock
"@ | Set-Content -Path $bat -Encoding ASCII

try {
  & $cmdExe /c $bat
  if ($LASTEXITCODE -ne 0) { throw "Windows host build failed with exit $LASTEXITCODE" }
} finally {
  Remove-Item $bat -ErrorAction SilentlyContinue
}

if (-not $ertsInclude) {
  Write-Warning "aumid_nif.dll not built: need elixir on PATH with erl_nif.h (CI/setup-beam provides this)."
}
