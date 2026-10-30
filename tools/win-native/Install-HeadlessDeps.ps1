#Requires -Version 5.1
<#
.SYNOPSIS
  Bootstrap vcpkg (if needed) and install tools/win-native/vcpkg.json for x64-windows.

.NOTES
  Does NOT enable cxx-glib-modern-incompatible (wrong glibmm/sigc++ generation).
  Prefer a short vcpkg root (default C:\vcpkg) to avoid Meson path-length issues.
#>
[CmdletBinding()]
param(
  [string]$VcpkgRoot = "C:\vcpkg",
  [string]$Triplet = "x64-windows",
  [string]$ManifestDir = "",
  [switch]$WithGtkLibs
)

$ErrorActionPreference = "Stop"

if (-not $ManifestDir) {
  $ManifestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}

# VsDevCmd may set VCPKG_ROOT to the VS-bundled copy; force the short root we bootstrap.
$env:VCPKG_ROOT = $VcpkgRoot
Remove-Item Env:VCPKG_DEFAULT_BINARY_CACHE -ErrorAction SilentlyContinue

# Overlay ports (glibmm-2.4 / sigc++-2.0) — also declared in vcpkg-configuration.json.
$overlay = Join-Path $ManifestDir "overlays"
if (Test-Path $overlay) {
  $env:VCPKG_OVERLAY_PORTS = $overlay
  Write-Host "VCPKG_OVERLAY_PORTS=$overlay"
}

$git = @(
  "C:\Program Files\Git\cmd\git.exe",
  "git"
) | Where-Object { $_ -eq "git" -or (Test-Path $_) } | Select-Object -First 1

if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
  Write-Host "Cloning vcpkg -> $VcpkgRoot"
  New-Item -ItemType Directory -Force -Path (Split-Path $VcpkgRoot) | Out-Null
  & $git clone --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
  Write-Host "Bootstrapping vcpkg..."
  & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
  if (-not (Test-Path $vcpkgExe)) { throw "bootstrap failed: $vcpkgExe missing" }
}

$featureArgs = @()
if ($WithGtkLibs) {
  # Feature gtk-libs → gtk3 + cairo + pango (MSVC libs; not product chrome).
  $featureArgs += "--x-feature=gtk-libs"
  Write-Host "WithGtkLibs: enabling manifest feature gtk-libs (gtk3/cairo/pango)"
}

Write-Host "Installing manifest from $ManifestDir (triplet=$Triplet)"
Push-Location $ManifestDir
try {
  & $vcpkgExe install --triplet $Triplet @featureArgs
  if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}

$installed = Join-Path $ManifestDir "vcpkg_installed\$Triplet"
# waf looks for pkg-config.exe; vcpkg ships pkgconf.exe
$pkgconfDir = Join-Path $installed "tools\pkgconf"
$pkgconf = Join-Path $pkgconfDir "pkgconf.exe"
$pkgconfig = Join-Path $pkgconfDir "pkg-config.exe"
if ((Test-Path $pkgconf) -and -not (Test-Path $pkgconfig)) {
  Copy-Item $pkgconf $pkgconfig -Force
  Write-Host "Created pkg-config.exe alias -> pkgconf.exe"
}

$pcDir = Join-Path $installed "lib\pkgconfig"
New-Item -ItemType Directory -Force -Path $pcDir | Out-Null

# Stock aubio vcpkg port installs CMake config only (no aubio.pc); waf needs pkg-config.
$aubioLib = Join-Path $installed "lib\aubio.lib"
$aubioPc = Join-Path $pcDir "aubio.pc"
if ((Test-Path $aubioLib) -and -not (Test-Path $aubioPc)) {
  @"
prefix=`${pcfiledir}/../..
exec_prefix=`${prefix}
libdir=`${exec_prefix}/lib
includedir=`${prefix}/include

Name: aubio
Description: a library for audio labelling
Version: 0.5.0
Libs: "-L`${libdir}" -laubio
Cflags: "-I`${includedir}"
"@ | Set-Content -Path $aubioPc -Encoding ASCII
  Write-Host "Wrote $aubioPc (vcpkg aubio has no pkg-config module)"
}

# vamp-sdk .pc says -lvamp-sdk but MSVC artifacts are VampPluginSDK.lib / VampHostSDK.lib.
$vampSdkPc = Join-Path $pcDir "vamp-sdk.pc"
$vampHostPc = Join-Path $pcDir "vamp-hostsdk.pc"
$vampPluginLib = Join-Path $installed "lib\VampPluginSDK.lib"
$vampHostLib = Join-Path $installed "lib\VampHostSDK.lib"
if (Test-Path $vampPluginLib) {
  @"
prefix=`${pcfiledir}/../..
exec_prefix=`${prefix}
libdir=`${exec_prefix}/lib
includedir=`${prefix}/include

Name: vamp-sdk
Version: 2.10
Description: Development library for Vamp audio analysis plugins
Libs: "-L`${libdir}" -lVampPluginSDK
Cflags: "-I`${includedir}"
"@ | Set-Content -Path $vampSdkPc -Encoding ASCII
  Write-Host "Patched $vampSdkPc -> -lVampPluginSDK"
}
if (Test-Path $vampHostLib) {
  @"
prefix=`${pcfiledir}/../..
exec_prefix=`${prefix}
libdir=`${exec_prefix}/lib
includedir=`${prefix}/include

Name: vamp-hostsdk
Version: 2.10
Description: Development library for Vamp audio analysis plugin hosts
Libs: "-L`${libdir}" -lVampHostSDK
Cflags: "-I`${includedir}"
"@ | Set-Content -Path $vampHostPc -Encoding ASCII
  Write-Host "Patched $vampHostPc -> -lVampHostSDK (no -ldl on Windows)"
}

# liblo.pc says -llo but the MSVC import lib is liblo.lib (would look for lo.lib).
$libloLib = Join-Path $installed "lib\liblo.lib"
$libloPc = Join-Path $pcDir "liblo.pc"
if (Test-Path $libloLib) {
  @"
prefix=`${pcfiledir}/../..
exec_prefix=`${prefix}
libdir=`${prefix}/lib
includedir=`${prefix}/include

Name: liblo
Version: 0.36
Description: A lightweight OSC server/client library
Libs: "-L`${libdir}" -lliblo
Cflags: "-I`${includedir}"
"@ | Set-Content -Path $libloPc -Encoding ASCII
  Write-Host "Patched $libloPc -> -lliblo"
}

Write-Host "Installed prefix: $installed"
Write-Host "CMake toolchain: $(Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake')"
Write-Host "Pass -DVCPKG_MANIFEST_DIR=$ManifestDir when configuring cmake/win64."
Write-Host "waf tip: --boost-include=$installed\include and PKG_CONFIG_PATH=$installed\lib\pkgconfig"
Write-Host "Overlays: libsigcpp2 + glibmm24 provide sigc++-2.0 / glibmm-2.4 (see overlays/README.md)."
if ($WithGtkLibs) {
  $gtkH = Join-Path $installed "include\gtk-3.0\gtk\gtk.h"
  if (Test-Path $gtkH) {
    Write-Host "GTK3 libs ready: $gtkH"
  } else {
    Write-Warning "WithGtkLibs set but gtk/gtk.h missing under $installed"
  }
}
