# Packages the Windows release assets for GitHub Releases.
# Builds a portable win_shell zip when airdour_win_shell.exe is available.
# Pass -Mode Engine -WafLibsDir build-rel\libs for the functional DAW package
# (libardour + Dummy/WASAPI + bundled LV2). UiOnly is the shell without that tree.
# When a shell stage exists, build the NSIS installer (+ always installer-src zip).
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = "",
    [string]$ShellExe = "",
    [string]$WafLibsDir = "",
    [string]$VcpkgInstalled = "",
    [ValidateSet("Auto", "Engine", "UiOnly")][string]$Mode = "Auto",
    [ValidateSet("Auto", "Release", "Debug")][string]$Config = "Auto",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown",
    [switch]$BootstrapNsis
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Always publish installer source scripts so the Engine setup can be rebuilt
# offline.
$instSrc = Join-Path $OutDir "installer-src-stage"
if (Test-Path $instSrc) { Remove-Item -Recurse -Force $instSrc }
New-Item -ItemType Directory -Force -Path $instSrc | Out-Null
Copy-Item (Join-Path $RepoRoot "tools\win-native\installer\airdour.nsi") $instSrc -Force
Copy-Item (Join-Path $RepoRoot "tools\win-native\installer\airdour.iss") $instSrc -Force
Copy-Item (Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Nsis.ps1") $instSrc -Force
Copy-Item (Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Installer.ps1") $instSrc -Force
Copy-Item (Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Portable.ps1") $instSrc -Force
$instReadme = @"
Airdour Windows installer sources
=================================
Brand: Airdour
Version: $Version
Git: $GitRef @ $GitSha

The release installer is NSIS (open source). airdour.nsi + Package-WinShell-Nsis.ps1
compile a per-user setup.exe from a portable shell stage. airdour.iss is the
optional Inno Setup script.

GitHub Actions release (windows-2022)
------------------------------------
The tag workflow builds the engine-linked shell and publishes:
  airdour-*-win64-shell.zip
  airdour-*-win64-setup.exe
Install for the current user (the /D= path is last and is not quoted):

  airdour-*-win64-setup.exe /S /D=C:\Airdour

A UI-only package, when one is produced, is named *-setup-ui.exe.

Rebuild on a laptop
-------------------
1. Package portable Engine stage/zip:
   powershell -File Package-WinShell-Portable.ps1 -RepoRoot <repo> -OutDir dist -Version $Version -Mode Engine -Config Release
2. Build the NSIS installer (per-user, %LOCALAPPDATA%\Airdour):
   powershell -File Package-WinShell-Nsis.ps1 -RepoRoot <repo> -OutDir dist -Version $Version -RefreshPortable -BootstrapNsis
"@
Set-Content -Path (Join-Path $instSrc "README.txt") -Value $instReadme -Encoding utf8
$instSrcZip = Join-Path $OutDir "airdour-$Version-win64-installer-src.zip"
if (Test-Path $instSrcZip) { Remove-Item -Force $instSrcZip }
Push-Location $instSrc
try {
    Compress-Archive -Path * -DestinationPath $instSrcZip -Force
} finally {
    Pop-Location
}
Write-Host "Created $instSrcZip"

# Portable win_shell zip. -Mode Engine requires the waf libs and vcpkg bin.
$shellZip = $null
try {
    $packShell = Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Portable.ps1"
    $shellArgs = @{
        RepoRoot = $RepoRoot
        OutDir   = $OutDir
        Version  = $Version
        GitSha   = $GitSha
        GitRef   = $GitRef
    }
    if ($ShellExe) { $shellArgs.ShellExe = $ShellExe }
    elseif ($BuildDir) {
        foreach ($c in @(
            (Join-Path $BuildDir "artifacts\airdour_win_shell.exe"),
            (Join-Path $BuildDir "artifacts\Release\airdour_win_shell.exe"),
            (Join-Path $BuildDir "artifacts\Debug\airdour_win_shell.exe")
        )) {
            if (Test-Path $c) { $shellArgs.ShellExe = $c; break }
        }
    }
    if ($WafLibsDir) { $shellArgs.WafLibsDir = $WafLibsDir }
    if ($VcpkgInstalled) { $shellArgs.VcpkgInstalled = $VcpkgInstalled }
    if ($Mode -ne "Auto") { $shellArgs.Mode = $Mode }
    if ($Config -ne "Auto") { $shellArgs.Config = $Config }
    & $packShell @shellArgs | Out-Null
    $shellZip = Get-ChildItem -Path $OutDir -Filter "airdour-$Version-win64-shell*.zip" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
} catch {
    if ($Mode -eq "Engine") { throw }
    Write-Warning "Portable win_shell zip skipped: $($_.Exception.Message)"
}

if ($Mode -eq "Engine") {
    $engineStage = Join-Path $OutDir "shell-stage"
    foreach ($rel in @(
        "airdour_win_shell.exe",
        "ardour.dll",
        "backends\wasapi\wasapi_audiobackend.dll",
        "backends\dummy\dummy_audiobackend.dll",
        "run-airdour.bat"
    )) {
        $p = Join-Path $engineStage $rel
        if (-not (Test-Path $p)) { throw "Engine package is missing $rel" }
    }
    if (-not $shellZip -or $shellZip -notmatch 'win64-shell\.zip$') {
        throw "Engine package did not produce airdour-$Version-win64-shell.zip"
    }
}

# NSIS installer from shell-stage (Engine when backends were staged, else UiOnly).
$setupExe = $null
$shellStage = Join-Path $OutDir "shell-stage"
if (-not (Test-Path (Join-Path $shellStage "airdour_win_shell.exe"))) {
    throw "NSIS installer requires shell-stage\airdour_win_shell.exe"
}
$packNsis = Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Nsis.ps1"
$nsisArgs = @{
    RepoRoot = $RepoRoot
    OutDir   = $OutDir
    Version  = $Version
    StageDir = $shellStage
    GitSha   = $GitSha
    GitRef   = $GitRef
}
if ($BootstrapNsis) { $nsisArgs.BootstrapNsis = $true }
& $packNsis @nsisArgs | Out-Null
$setupExe = Get-ChildItem -Path $OutDir -Filter "airdour-$Version-win64-setup*.exe" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $setupExe) { throw "NSIS installer was not produced" }

Write-Output $instSrcZip
if ($shellZip) { Write-Output $shellZip }
if ($setupExe) { Write-Output $setupExe }
