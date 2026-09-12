# Compiles installer\Synthesia.strobe.iss into installer\Output\Synthesia.strobe-Setup.exe.
# Run after building the DLL (build\Release must hold version.dll).
#   powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
$ErrorActionPreference = 'Stop'

$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build\Release'
$iss   = Join-Path $PSScriptRoot 'Synthesia.strobe.iss'

if (-not (Test-Path (Join-Path $build 'version.dll'))) {
  throw "Missing version.dll in $build - build the Release config first (cmake --build build --config Release)."
}

# ISCC lands in Program Files or, via winget, under LocalAppData.
$iscc = @(
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe not found. Install Inno Setup 6 (winget install -e --id JRSoftware.InnoSetup)." }

& $iscc "/DBuildDir=$build" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE." }

Write-Host "Built: $(Join-Path $PSScriptRoot 'Output\Synthesia.strobe-Setup.exe')"
