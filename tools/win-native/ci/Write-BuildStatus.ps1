# Writes BUILD_STATUS.txt describing what this CI/release artifact contains.
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$GitSha,
    [Parameter(Mandatory = $true)][string]$GitRef,
    [string]$WasapiSmokePath = "",
    [string]$WinShellPath = ""
)

$smokeLine = $null
if ($PSBoundParameters.ContainsKey('WasapiSmokePath')) {
    $smokeLine = if ($WasapiSmokePath -and (Test-Path $WasapiSmokePath)) {
        "GREEN  wasapi_io_smoke static lib: $WasapiSmokePath"
    } else {
        "MISSING wasapi_io_smoke (compile may have been skipped)"
    }
}

$guiLine = if ($WinShellPath -and (Test-Path $WinShellPath)) {
    "GREEN  airdour_win_shell (Win32+D2D+DirectWrite): $WinShellPath"
} else {
    "MISSING airdour_win_shell (GUI shell may have been skipped)"
}

$shipped = if ($null -ne $smokeLine) { $smokeLine + "`n" + $guiLine } else { $guiLine }

$status = @"
Airdour Windows CI / release artifact status
============================================
git_ref:  $GitRef
git_sha:  $GitSha
host:     windows-latest (MSVC / Win64)
audio:    WASAPI shared (default) + optional exclusive (prefs/env); no DirectSound/ASIO
gui:      Win32 + Direct2D/DirectWrite (no GTK2)
product:  AIRDOUR_WINDOWS_ONLY / --windows-only

What is in this artifact
------------------------
$shipped
GREEN  cmake/win64 spine configured
GREEN  libs/backends/wasapi sources (shared + exclusive open path)
GREEN  tools/win-native/README.md

Dependency expectations (Windows-only product)
----------------------------------------------
Prefer vcpkg (x64-windows) with binary caching:
  glib libxml2 libsndfile curl libarchive liblo taglib
  overlay glibmm-2.4 + sigc++-2.0 (not stock glibmm 2.68+)
  rubberband vamp-sdk lilv fftw3 aubio
  pthreads4w
Non-UI third-party still linked: fluidsynth, zita-resampler/convolver,
  qm-dsp, vamp-plugins, hidapi, aaf, ptformat, libltc, boost (headers).
Cairo is an optional shell blit (feature gtk-libs / Install-HeadlessDeps -WithGtkLibs).
NOT linked: cairomm, pangocairo, GTK2/ytk, PortAudio (default path).

Secrets
-------
No required GitHub secrets for the smoke/release zip path.
Optional later:
  VCPKG_BINARY_SOURCES  — for shared binary cache
  CODE_SIGNING_*        — Authenticode (not wired yet)
"@

Set-Content -Path $OutFile -Value $status -Encoding utf8
Write-Host "Wrote $OutFile"
