# Packages the staged Windows release zip for GitHub Releases.
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = "",
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
Write-Output $zipPath
