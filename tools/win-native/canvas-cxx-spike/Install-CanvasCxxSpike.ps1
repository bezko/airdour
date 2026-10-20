# Install cairomm (+ pangomm) into an isolated vcpkg_installed tree.
# Does not modify the main tools/win-native/vcpkg_installed (sigc++/glibmm ABI).
param(
  [string]$VcpkgRoot = "C:\vcpkg",
  [string]$Triplet = "x64-windows",
  [switch]$WithGtkmm4
)

$ErrorActionPreference = "Stop"
$ManifestDir = $PSScriptRoot
$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
  throw "vcpkg.exe not found at $vcpkgExe"
}

$featureArgs = @()
if ($WithGtkmm4) {
  $featureArgs += "--x-feature=gtkmm4"
  Write-Host "WithGtkmm4: enabling stock gtkmm 4.x (will NOT unlock libs/canvas)"
}

Write-Host "Installing canvas-cxx-spike deps into $ManifestDir (triplet=$Triplet)"
Push-Location $ManifestDir
try {
  & $vcpkgExe install --triplet $Triplet @featureArgs
  if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}

$installed = Join-Path $ManifestDir "vcpkg_installed\$Triplet"
$cairommH = Join-Path $installed "include\cairomm-1.0\cairomm\cairomm.h"
if (-not (Test-Path $cairommH)) {
  # Some layouts use include/cairomm/cairomm.h
  $alt = Join-Path $installed "include\cairomm\cairomm.h"
  if (Test-Path $alt) {
    Write-Host "OK: cairomm at $alt"
  } else {
    Write-Warning "cairomm header not found under $installed\include"
  }
} else {
  Write-Host "OK: cairomm at $cairommH"
}

Write-Host "Done. Build probes with build-and-run.cmd"
