# Packages the staged Windows release zip for GitHub Releases.
# Always builds the CI spine zip (wasapi_io smoke + sources).
# Also builds a portable win_shell zip when airdour_win_shell.exe is available
# (UiOnly on GHA; Engine when waf libs exist on a laptop agent).
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = "",
    [string]$ShellExe = "",
    [string]$WafLibsDir = "",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown"
)

$ErrorActionPreference = "Stop"
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

Write-Output $zipPath
if ($shellZip) { Write-Output $shellZip }
