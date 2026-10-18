# Writes BUILD_STATUS.txt describing what this CI/release artifact contains.
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$GitSha,
    [Parameter(Mandatory = $true)][string]$GitRef,
    [string]$WasapiSmokePath = ""
)

$smokeLine = if ($WasapiSmokePath -and (Test-Path $WasapiSmokePath)) {
    "GREEN  wasapi_io_smoke static lib: $WasapiSmokePath"
} else {
    "MISSING wasapi_io_smoke (compile may have been skipped)"
}

$status = @"
Airdour Windows CI / release artifact status
============================================
git_ref:  $GitRef
git_sha:  $GitSha
host:     windows-latest (MSVC / Win64)
audio:    WASAPI shared only (no exclusive, DirectSound, or ASIO)
product:  AIRDOUR_WINDOWS_ONLY / --windows-only

What is in this artifact
------------------------
$smokeLine
GREEN  cmake/win64 spine configured
GREEN  libs/backends/wasapi sources (shared-mode implementation)
GREEN  tools/win-native docs and packaging scripts

What is NOT in this artifact yet
--------------------------------
BLOCKED  libpbd / libardour / hardour (headless) — needs MSVC dep prefix
BLOCKED  gtk2_ardour GUI — needs GTK2/Cairo/Pango stack on MSVC (hard)
BLOCKED  WinMME MIDI inside wasapi backend
BLOCKED  NSIS / MSIX installer (zip only for now)

Dependency expectations (next stage)
------------------------------------
Prefer vcpkg (x64-windows) with binary caching:
  glib libxml2 libsndfile curl libarchive liblo taglib
  libsigcpp glibmm (versions matching Ardour's waf checks)
  rubberband vamp-sdk
  pthreads4w (until Win32 thread migration completes)

GTK2 on MSVC is the main GUI blocker; upstream Windows builds historically
used MinGW. Airdour either ports bundled libs/tk to MSVC or replaces UI
surfaces with Direct2D before a full GUI release zip is realistic.

Secrets
-------
No required GitHub secrets for the smoke/release zip path.
Optional later:
  VCPKG_BINARY_SOURCES  — for shared binary cache
  CODE_SIGNING_*        — Authenticode (not wired yet)
"@

Set-Content -Path $OutFile -Value $status -Encoding utf8
Write-Host "Wrote $OutFile"
