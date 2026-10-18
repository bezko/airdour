# Writes BUILD_STATUS.txt describing what this CI/release artifact contains.
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$GitSha,
    [Parameter(Mandatory = $true)][string]$GitRef,
    [string]$WasapiSmokePath = "",
    [string]$WinShellPath = ""
)

$smokeLine = if ($WasapiSmokePath -and (Test-Path $WasapiSmokePath)) {
    "GREEN  wasapi_io_smoke static lib: $WasapiSmokePath"
} else {
    "MISSING wasapi_io_smoke (compile may have been skipped)"
}

$guiLine = if ($WinShellPath -and (Test-Path $WinShellPath)) {
    "GREEN  airdour_win_shell (Win32+D2D+DirectWrite): $WinShellPath"
} else {
    "MISSING airdour_win_shell (GUI shell may have been skipped)"
}

$status = @"
Airdour Windows CI / release artifact status
============================================
git_ref:  $GitRef
git_sha:  $GitSha
host:     windows-latest (MSVC / Win64)
audio:    WASAPI shared only (no exclusive, DirectSound, or ASIO)
gui:      Win32 + Direct2D/DirectWrite (no GTK2)
product:  AIRDOUR_WINDOWS_ONLY / --windows-only

What is in this artifact
------------------------
$smokeLine
$guiLine
GREEN  cmake/win64 spine configured
GREEN  libs/backends/wasapi sources (shared-mode implementation)
GREEN  win_ardour minimal shell sources
GREEN  tools/win-native docs and packaging scripts

What is NOT in this artifact yet
--------------------------------
STAGED   libpbd / libardour / hardour — laptop-proven; wire into CI next
STAGED   WinMME MIDI inside wasapi backend
OUT      gtk2_ardour / libs/tk on MSVC (dead end; use win_ardour)
BLOCKED  NSIS / MSIX installer (zip only for now)

Dependency expectations (headless product)
------------------------------------------
Prefer vcpkg (x64-windows) with binary caching:
  glib libxml2 libsndfile curl libarchive liblo taglib
  overlay glibmm-2.4 + sigc++-2.0 (not stock glibmm 2.68+)
  rubberband vamp-sdk
  pthreads4w (until Win32 thread migration completes)

Secrets
-------
No required GitHub secrets for the smoke/release zip path.
Optional later:
  VCPKG_BINARY_SOURCES  — for shared binary cache
  CODE_SIGNING_*        — Authenticode (not wired yet)
"@

Set-Content -Path $OutFile -Value $status -Encoding utf8
Write-Host "Wrote $OutFile"
