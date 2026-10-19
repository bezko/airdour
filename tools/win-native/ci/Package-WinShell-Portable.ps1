# Package a portable Airdour win_shell zip that runs after unzip
# without a full source tree.
#
# Modes:
#   Engine  — exe linked with libardour + Dummy/WASAPI + ACE LV2
#   UiOnly  — D2D shell without engine (what GHA can build today)
#
# Config:
#   Release — default when build-rel + Release shell exist (/MD, ~116 MB WS)
#   Debug   — build/ + Debug CRT (/MDd)
#   Auto    — prefer Release Engine paths, else Debug, else UiOnly
#
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ShellExe = "",
    [string]$WafLibsDir = "",
    [string]$VcpkgInstalled = "",
    [ValidateSet("Auto", "Engine", "UiOnly")][string]$Mode = "Auto",
    [ValidateSet("Auto", "Release", "Debug")][string]$Config = "Auto",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path

function Test-EngineTree([string]$libsDir) {
    if (-not $libsDir -or -not (Test-Path $libsDir)) { return $false }
    $ardour = Test-Path (Join-Path $libsDir "ardour\ardour.dll")
    $wasapi = Test-Path (Join-Path $libsDir "backends\wasapi\wasapi_audiobackend.dll")
    return ($ardour -and $wasapi)
}

function Resolve-ConfigAndPaths {
    param(
        [string]$HintExe,
        [string]$HintWaf,
        [string]$WantConfig
    )

    $relShell = Join-Path $RepoRoot "build-win64-gui-engine-rel\artifacts\Release\airdour_win_shell.exe"
    $dbgShell = Join-Path $RepoRoot "build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe"
    $relWaf = Join-Path $RepoRoot "build-rel\libs"
    $dbgWaf = Join-Path $RepoRoot "build\libs"

    $cfg = $WantConfig
    if ($cfg -eq "Auto") {
        if ((Test-Path $relShell) -and (Test-EngineTree $relWaf)) {
            $cfg = "Release"
        } elseif ((Test-Path $dbgShell) -and (Test-EngineTree $dbgWaf)) {
            $cfg = "Debug"
        } elseif (Test-Path $relShell) {
            $cfg = "Release"
        } else {
            $cfg = "Debug"
        }
    }

    $exe = $null
    if ($HintExe -and (Test-Path $HintExe)) {
        $exe = (Resolve-Path $HintExe).Path
    } else {
        $ordered = if ($cfg -eq "Release") {
            @(
                $relShell,
                (Join-Path $RepoRoot "build-win64-gui\artifacts\Release\airdour_win_shell.exe"),
                (Join-Path $RepoRoot "build-win64\artifacts\Release\airdour_win_shell.exe"),
                $dbgShell
            )
        } else {
            @(
                $dbgShell,
                (Join-Path $RepoRoot "build-win64-gui-engine\artifacts\Release\airdour_win_shell.exe"),
                (Join-Path $RepoRoot "build-win64-gui\artifacts\Debug\airdour_win_shell.exe"),
                $relShell
            )
        }
        foreach ($c in $ordered) {
            if (Test-Path $c) { $exe = (Resolve-Path $c).Path; break }
        }
    }
    if (-not $exe) { throw "airdour_win_shell.exe not found (pass -ShellExe)" }

    $waf = $null
    if ($HintWaf -and (Test-Path $HintWaf)) {
        $waf = (Resolve-Path $HintWaf).Path
    } else {
        $waf = if ($cfg -eq "Release") { $relWaf } else { $dbgWaf }
        if (-not (Test-EngineTree $waf)) {
            # Fall back to the other tree if the preferred one is incomplete.
            $alt = if ($cfg -eq "Release") { $dbgWaf } else { $relWaf }
            if (Test-EngineTree $alt) {
                Write-Warning "Preferred waf tree incomplete; using $alt"
                $waf = $alt
                if ($alt -eq $relWaf) { $cfg = "Release" } else { $cfg = "Debug" }
            }
        }
    }

    return @{
        Config = $cfg
        ShellExe = $exe
        WafLibsDir = $waf
    }
}

function Copy-FileSafe([string]$src, [string]$dstDir) {
    if (-not (Test-Path $src)) { return $false }
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    Copy-Item -Force $src (Join-Path $dstDir (Split-Path $src -Leaf))
    return $true
}

$resolved = Resolve-ConfigAndPaths -HintExe $ShellExe -HintWaf $WafLibsDir -WantConfig $Config
$ShellExe = $resolved.ShellExe
$WafLibsDir = $resolved.WafLibsDir
$Config = $resolved.Config

if (-not $VcpkgInstalled) {
    $VcpkgInstalled = Join-Path $RepoRoot "tools\win-native\vcpkg_installed\x64-windows"
}

$hasEngine = Test-EngineTree $WafLibsDir
if ($Mode -eq "Auto") {
    $Mode = if ($hasEngine) { "Engine" } else { "UiOnly" }
}
if ($Mode -eq "Engine" -and -not $hasEngine) {
    throw "Mode=Engine but waf libs incomplete under $WafLibsDir"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path $OutDir "shell-stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Write-Host "Packaging Airdour win_shell ($Mode / $Config) from $ShellExe"
Write-Host "  waf libs: $WafLibsDir"

# --- Core binary ---
Copy-Item -Force $ShellExe (Join-Path $stage "airdour_win_shell.exe")

$copied = New-Object System.Collections.Generic.List[string]
$copied.Add("airdour_win_shell.exe")

if ($Mode -eq "Engine") {
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

    $panSrc = Join-Path $WafLibsDir "ardour\panners"
    if (-not (Test-Path $panSrc)) {
        $panSrc = Join-Path $WafLibsDir "panners"
    }
    $panDst = Join-Path $stage "panners"
    if (Test-Path $panSrc) {
        New-Item -ItemType Directory -Force -Path $panDst | Out-Null
        Get-ChildItem $panSrc -Recurse -Filter "*.dll" | ForEach-Object {
            Copy-Item -Force $_.FullName $panDst
            $copied.Add(("panners\{0}" -f $_.Name))
        }
    }

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

    $vcpkgDebug = Join-Path $VcpkgInstalled "debug\bin"
    $vcpkgRel = Join-Path $VcpkgInstalled "bin"
    if ($Config -eq "Release") {
        if (-not (Test-Path $vcpkgRel)) { throw "Missing Release vcpkg bin: $vcpkgRel" }
        Get-ChildItem $vcpkgRel -Filter "*.dll" | ForEach-Object {
            Copy-Item -Force $_.FullName $stage
            $copied.Add($_.Name)
        }
        # Redistributable Release CRT (safe on clean machines without VC installed)
        foreach ($crt in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "concrt140.dll")) {
            $sys = Join-Path $env:SystemRoot ("System32\" + $crt)
            if (Test-Path $sys) {
                Copy-Item -Force $sys $stage
                $copied.Add($crt)
            }
        }
    } else {
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
        foreach ($crt in @("ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll", "msvcp140_1d.dll", "msvcp140_2d.dll")) {
            $sys = Join-Path $env:SystemRoot ("System32\" + $crt)
            if (Test-Path $sys) {
                Copy-Item -Force $sys $stage
                $copied.Add($crt)
            }
        }
    }
}

$launcher = @"
@echo off
REM Portable Airdour (win_shell) launcher - unzip and run. Brand=Airdour.
REM Config=$Config Mode=$Mode
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

$crtNote = if ($Config -eq "Release") {
    "MSVC Release (/MD) - Release CRT + vcpkg Release DLLs bundled"
} else {
    "MSVC Debug (/MDd) - Debug CRT + vcpkg debug DLLs bundled"
}

$readme = @"
Airdour portable Windows shell
==============================
Brand: Airdour
Version: $Version
Git: $GitRef @ $GitSha
Mode: $Mode
Config: $Config
Audio: WASAPI shared (engine) or UI-only (--no-engine)
CRT: $crtNote

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
*.dll                   waf + vcpkg (+ CRT redistributables)

Honest limits
-------------
- No GTK plugin UIs. Mixer shows FX name / BYP / Blend.
- Default Engine package is MSVC Release (/MD). Pass -Config Debug for the Debug slice.
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
config:  $Config
exe:     airdour_win_shell.exe
gui:     Win32 + Direct2D/DirectWrite (no GTK2)
audio:   $(if ($Mode -eq 'Engine') { 'WASAPI shared + Dummy backends bundled' } else { 'UI-only (--no-engine); backends not bundled' })
crt:     $crtNote

Files copied: $($copied.Count)
"@
Set-Content -Path (Join-Path $stage "BUILD_STATUS.txt") -Value $status -Encoding utf8

$zipName = if ($Mode -eq "Engine") {
    if ($Config -eq "Debug") {
        "airdour-$Version-win64-shell-debug.zip"
    } else {
        "airdour-$Version-win64-shell.zip"
    }
} else {
    "airdour-$Version-win64-shell-ui.zip"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipPath = [System.IO.Path]::GetFullPath((Join-Path $OutDir $zipName))
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

Push-Location $stage
try {
    Compress-Archive -Path * -DestinationPath $zipPath -Force
} finally {
    Pop-Location
}

Write-Host "Created $zipPath ($Mode/$Config, $($copied.Count) payloads)"
Write-Output $zipPath
