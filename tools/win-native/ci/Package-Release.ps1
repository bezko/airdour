# Packages the staged Windows release zip for GitHub Releases.
# Always builds the CI spine zip (wasapi_io smoke + sources).
# Also builds a portable win_shell zip when airdour_win_shell.exe is available
# (UiOnly on GHA; Engine when waf libs exist on a laptop agent).
# When a shell stage exists, best-effort Inno installer (+ always installer-src zip).
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = "",
    [string]$ShellExe = "",
    [string]$WafLibsDir = "",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown",
    [switch]$BootstrapInno
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stage = Join-Path $OutDir "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Docs + product path notes
Copy-Item (Join-Path $RepoRoot "tools\win-native\README.md") (Join-Path $stage "WIN-NATIVE-README.md")
Copy-Item (Join-Path $RepoRoot "libs\backends\wasapi\README.md") (Join-Path $stage "WASAPI-README.md")
Copy-Item (Join-Path $RepoRoot "COPYING") (Join-Path $stage "COPYING") -ErrorAction SilentlyContinue

# WASAPI sources (auditable snapshot in the release)
$wasapiSrc = Join-Path $stage "src\backends\wasapi"
New-Item -ItemType Directory -Force -Path $wasapiSrc | Out-Null
Copy-Item (Join-Path $RepoRoot "libs\backends\wasapi\*") $wasapiSrc -Recurse -Force

# CMake spine
$cmakeOut = Join-Path $stage "cmake\win64"
New-Item -ItemType Directory -Force -Path $cmakeOut | Out-Null
Copy-Item (Join-Path $RepoRoot "cmake\win64\*") $cmakeOut -Recurse -Force

# Smoke build product if present
$binDir = Join-Path $stage "bin"
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
$smokeLib = $null
if ($BuildDir) {
    $candidates = @(
        (Join-Path $BuildDir "artifacts\airdour_wasapi_io.lib"),
        (Join-Path $BuildDir "artifacts\Release\airdour_wasapi_io.lib"),
        (Join-Path $BuildDir "artifacts\Debug\airdour_wasapi_io.lib"),
        (Join-Path $BuildDir "Release\airdour_wasapi_io.lib"),
        (Join-Path $BuildDir "Debug\airdour_wasapi_io.lib")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            Copy-Item $c $binDir -Force
            $smokeLib = Join-Path $binDir (Split-Path $c -Leaf)
            break
        }
    }
}

& (Join-Path $RepoRoot "tools\win-native\ci\Write-BuildStatus.ps1") `
    -OutFile (Join-Path $stage "BUILD_STATUS.txt") `
    -GitSha $GitSha `
    -GitRef $GitRef `
    -WasapiSmokePath $(if ($smokeLib) { $smokeLib } else { "" })

$zipName = "airdour-$Version-win64-ci.zip"
$zipPath = Join-Path $OutDir $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

# Compress contents of stage/ (not the stage folder itself)
Push-Location $stage
try {
    Compress-Archive -Path * -DestinationPath $zipPath -Force
} finally {
    Pop-Location
}
Write-Host "Created $zipPath"

# Always publish installer source scripts (GHA + laptop) so Engine setup can be
# rebuilt offline when full Engine deps are not in CI.
$instSrc = Join-Path $OutDir "installer-src-stage"
if (Test-Path $instSrc) { Remove-Item -Recurse -Force $instSrc }
New-Item -ItemType Directory -Force -Path $instSrc | Out-Null
Copy-Item (Join-Path $RepoRoot "tools\win-native\installer\airdour.iss") $instSrc -Force
Copy-Item (Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Installer.ps1") $instSrc -Force
Copy-Item (Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Portable.ps1") $instSrc -Force
$instReadme = @"
Airdour Windows installer sources
=================================
Brand: Airdour
Version: $Version
Git: $GitRef @ $GitSha

These scripts build the Inno Setup installer from a portable shell stage.

On GitHub Actions (windows-latest)
----------------------------------
CI builds a UiOnly shell and may publish airdour-*-win64-setup-ui.exe
(bootstrap Inno Setup via Package-WinShell-Installer.ps1 -BootstrapInno).
Full Engine deps (waf libardour + vcpkg) do NOT fit the default GHA job yet.

On a laptop with Engine Release trees
-------------------------------------
1. Package portable Engine stage/zip:
   powershell -File Package-WinShell-Portable.ps1 -RepoRoot <repo> -OutDir dist -Version $Version -Mode Engine -Config Release
2. Build installer (Program Files\Airdour):
   powershell -File Package-WinShell-Installer.ps1 -RepoRoot <repo> -OutDir dist -Version $Version -RefreshPortable -BootstrapInno

Attach airdour-*-win64-setup.exe (Engine) to the GitHub Release alongside CI UiOnly artifacts.
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

# Portable win_shell zip (best available: Engine on laptop, UiOnly on GHA)
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
            (Join-Path $BuildDir "artifacts\Release\airdour_win_shell.exe"),
            (Join-Path $BuildDir "artifacts\Debug\airdour_win_shell.exe")
        )) {
            if (Test-Path $c) { $shellArgs.ShellExe = $c; break }
        }
    }
    if ($WafLibsDir) { $shellArgs.WafLibsDir = $WafLibsDir }
    & $packShell @shellArgs | Out-Null
    $shellZip = Get-ChildItem -Path $OutDir -Filter "airdour-$Version-win64-shell*.zip" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
} catch {
    Write-Warning "Portable win_shell zip skipped: $($_.Exception.Message)"
}

# Best-effort Inno installer from shell-stage (UiOnly on GHA; Engine on laptop).
$setupExe = $null
$shellStage = Join-Path $OutDir "shell-stage"
if ((Test-Path (Join-Path $shellStage "airdour_win_shell.exe"))) {
    try {
        $packInst = Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Installer.ps1"
        $instArgs = @{
            RepoRoot  = $RepoRoot
            OutDir    = $OutDir
            Version   = $Version
            StageDir  = $shellStage
            GitSha    = $GitSha
            GitRef    = $GitRef
        }
        if ($BootstrapInno) { $instArgs.BootstrapInno = $true }
        & $packInst @instArgs | Out-Null
        $setupExe = Get-ChildItem -Path $OutDir -Filter "airdour-$Version-win64-setup*.exe" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    } catch {
        Write-Warning "Inno installer skipped: $($_.Exception.Message)"
    }
} else {
    Write-Warning "Inno installer skipped: no shell-stage exe"
}

Write-Output $zipPath
Write-Output $instSrcZip
if ($shellZip) { Write-Output $shellZip }
if ($setupExe) { Write-Output $setupExe }
