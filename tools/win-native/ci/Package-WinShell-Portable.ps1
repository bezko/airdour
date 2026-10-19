# Package a portable Airdour win_shell zip that runs after unzip
# without a full source tree.
#
# Modes:
#   Engine  — exe linked with libardour + Dummy/WASAPI + ACE LV2 (laptop Debug path)
#   UiOnly  — D2D shell without engine (what GHA can build today)
#
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ShellExe = "",
    [string]$WafLibsDir = "",
    [string]$VcpkgInstalled = "",
    [ValidateSet("Auto", "Engine", "UiOnly")][string]$Mode = "Auto",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path

function Resolve-ShellExe([string]$hint) {
    if ($hint -and (Test-Path $hint)) { return (Resolve-Path $hint).Path }
    $candidates = @(
        (Join-Path $RepoRoot "build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe"),
        (Join-Path $RepoRoot "build-win64-gui-engine\artifacts\Release\airdour_win_shell.exe"),
        (Join-Path $RepoRoot "build-win64-gui\artifacts\Release\airdour_win_shell.exe"),
        (Join-Path $RepoRoot "build-win64-gui\artifacts\Debug\airdour_win_shell.exe"),
        (Join-Path $RepoRoot "build-win64\artifacts\Release\airdour_win_shell.exe"),
        (Join-Path $RepoRoot "build-win64\artifacts\Debug\airdour_win_shell.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "airdour_win_shell.exe not found (pass -ShellExe)"
}

function Copy-FileSafe([string]$src, [string]$dstDir) {
    if (-not (Test-Path $src)) { return $false }
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    Copy-Item -Force $src (Join-Path $dstDir (Split-Path $src -Leaf))
    return $true
}

$ShellExe = Resolve-ShellExe $ShellExe
if (-not $WafLibsDir) { $WafLibsDir = Join-Path $RepoRoot "build\libs" }
if (-not $VcpkgInstalled) {
    $VcpkgInstalled = Join-Path $RepoRoot "tools\win-native\vcpkg_installed\x64-windows"
}

$hasArdourDll = Test-Path (Join-Path $WafLibsDir "ardour\ardour.dll")
$hasWasapi = Test-Path (Join-Path $WafLibsDir "backends\wasapi\wasapi_audiobackend.dll")
if ($Mode -eq "Auto") {
    $Mode = if ($hasArdourDll -and $hasWasapi) { "Engine" } else { "UiOnly" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path $OutDir "shell-stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Write-Host "Packaging Airdour win_shell ($Mode) from $ShellExe"

# --- Core binary ---
Copy-Item -Force $ShellExe (Join-Path $stage "airdour_win_shell.exe")

$copied = New-Object System.Collections.Generic.List[string]
$copied.Add("airdour_win_shell.exe")

if ($Mode -eq "Engine") {
    # Waf product DLLs next to exe (load-time + POST_BUILD set)
    $wafDlls = @(
        "ardour\ardour.dll",
        "pbd\pbd.dll",
        "temporal\temporal.dll",
        "midi++2\midipp.dll",
        "evoral\evoral.dll",
        "audiographer\audiographer.dll",
        "ptformat\ptformat.dll",
        "ctrl-interface\control_protocol\ardourcp.dll"
    )
    foreach ($rel in $wafDlls) {
        $src = Join-Path $WafLibsDir $rel
        if (Copy-FileSafe $src $stage) { $copied.Add((Split-Path $rel -Leaf)) }
    }

    # Backends under backends/<name>/ so ARDOUR_BACKEND_PATH works
    foreach ($be in @("dummy", "wasapi")) {
        $srcDir = Join-Path $WafLibsDir ("backends\" + $be)
        $dstDir = Join-Path $stage ("backends\" + $be)
        if (Test-Path $srcDir) {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
            Get-ChildItem $srcDir -Filter "*.dll" | ForEach-Object {
                Copy-Item -Force $_.FullName $dstDir
                $copied.Add(("backends\{0}\{1}" -f $be, $_.Name))
            }
        }
    }

    # Panners under ARDOUR_DLL_PATH\panners
    $panSrc = Join-Path $WafLibsDir "ardour\panners"
    $panDst = Join-Path $stage "panners"
    if (Test-Path $panSrc) {
        New-Item -ItemType Directory -Force -Path $panDst | Out-Null
        Get-ChildItem $panSrc -Filter "*.dll" | ForEach-Object {
            Copy-Item -Force $_.FullName $panDst
            $copied.Add(("panners\{0}" -f $_.Name))
        }
    }

    # ACE LV2 bundles
    $lv2Root = Join-Path $stage "LV2"
    New-Item -ItemType Directory -Force -Path $lv2Root | Out-Null
    foreach ($bundle in @("reasonablesynth.lv2", "a-reverb.lv2")) {
        $candidates = @(
            (Join-Path $WafLibsDir ("ardour\LV2\" + $bundle)),
            (Join-Path $WafLibsDir ("LV2\" + $bundle))
        )
        $src = $null
        foreach ($c in $candidates) { if (Test-Path $c) { $src = $c; break } }
        if (-not $src) { Write-Warning "Missing LV2 bundle $bundle"; continue }
        $dst = Join-Path $lv2Root $bundle
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Get-ChildItem $src -File | Where-Object {
            $_.Extension -in @(".dll", ".ttl") -or $_.Name -eq "manifest.ttl"
        } | ForEach-Object {
            Copy-Item -Force $_.FullName $dst
            $copied.Add(("LV2\{0}\{1}" -f $bundle, $_.Name))
        }
    }

    # vcpkg runtime DLLs (debug + a few release names ardour imports)
    $vcpkgDebug = Join-Path $VcpkgInstalled "debug\bin"
    $vcpkgRel = Join-Path $VcpkgInstalled "bin"
    if (Test-Path $vcpkgDebug) {
        Get-ChildItem $vcpkgDebug -Filter "*.dll" | ForEach-Object {
            Copy-Item -Force $_.FullName $stage
            $copied.Add($_.Name)
        }
    }
    foreach ($extra in @("libcurl.dll")) {
        $p = Join-Path $vcpkgRel $extra
        if (Test-Path $p) {
            Copy-Item -Force $p $stage
            $copied.Add($extra)
        }
    }

    # Debug CRT (this engine shell is /MDd today)
    foreach ($crt in @("ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll", "msvcp140_1d.dll", "msvcp140_2d.dll")) {
        $sys = Join-Path $env:SystemRoot ("System32\" + $crt)
        if (Test-Path $sys) {
            Copy-Item -Force $sys $stage
            $copied.Add($crt)
        }
    }
}

# Launcher — self-contained PATH / ARDOUR_* (no source tree)
$launcher = @"
@echo off
REM Portable Airdour (win_shell) launcher — unzip and run. Brand=Airdour.
setlocal
cd /d "%~dp0"
set "PATH=%CD%;%PATH%"
set "ARDOUR_DLL_PATH=%CD%"
set "ARDOUR_BACKEND_PATH=%CD%\backends\dummy;%CD%\backends\wasapi"
if "%~1"=="" (
  if exist "%CD%\backends\wasapi\wasapi_audiobackend.dll" (
    airdour_win_shell.exe --backend WASAPI
  ) else (
    airdour_win_shell.exe --no-engine
  )
) else (
  airdour_win_shell.exe %*
)
echo EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
"@
Set-Content -Path (Join-Path $stage "run-airdour.bat") -Value $launcher -Encoding ASCII

$readme = @"
Airdour portable Windows shell
==============================
Brand: Airdour
Version: $Version
Git: $GitRef @ $GitSha
Mode: $Mode
Audio: WASAPI shared (engine) or UI-only (--no-engine)

How to run
----------
1. Unzip this folder anywhere.
2. Double-click run-airdour.bat  OR
   run-airdour.bat --timeout 5 --backend Dummy --smoke-plugin
3. No full source tree required.

Layout
------
airdour_win_shell.exe   Win32 + Direct2D/DirectWrite shell
run-airdour.bat         Sets PATH / ARDOUR_DLL_PATH / ARDOUR_BACKEND_PATH
backends\               Dummy + WASAPI backend DLLs (Engine mode)
panners\                libardour panner plugins (Engine mode)
LV2\                    ACE Reasonable Synth + ACE Reverb (Engine mode)
*.dll                   waf + vcpkg (+ Debug CRT for Engine Debug builds)

Honest limits
-------------
- No GTK plugin UIs. Mixer shows FX name / BYP / Blend.
- Engine mode today is an MSVC Debug (/MDd) laptop slice — Debug CRT DLLs are bundled.
- GitHub Actions publishes UiOnly when waf+vcpkg engine libs are not in CI yet.
- Not an NSIS/MSIX installer.

Smoke examples (Engine)
-----------------------
run-airdour.bat --timeout 3 --backend Dummy
run-airdour.bat --timeout 3 --backend WASAPI
run-airdour.bat --timeout 10 --backend Dummy --smoke-plugin
run-airdour.bat --timeout 2 --no-engine
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding utf8

$status = @"
Airdour portable win_shell package
==================================
git_ref: $GitRef
git_sha: $GitSha
version: $Version
mode:    $Mode
exe:     airdour_win_shell.exe
gui:     Win32 + Direct2D/DirectWrite (no GTK2)
audio:   $(if ($Mode -eq 'Engine') { 'WASAPI shared + Dummy backends bundled' } else { 'UI-only (--no-engine); backends not bundled' })

Files copied: $($copied.Count)
"@
Set-Content -Path (Join-Path $stage "BUILD_STATUS.txt") -Value $status -Encoding utf8

$zipName = if ($Mode -eq "Engine") {
    "airdour-$Version-win64-shell.zip"
} else {
    "airdour-$Version-win64-shell-ui.zip"
}
$zipPath = Join-Path $OutDir $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

Push-Location $stage
try {
    Compress-Archive -Path * -DestinationPath $zipPath -Force
} finally {
    Pop-Location
}

Write-Host "Created $zipPath ($Mode, $($copied.Count) payloads)"
Write-Output $zipPath
