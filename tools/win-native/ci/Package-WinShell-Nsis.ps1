# Build an NSIS installer for a staged Airdour portable shell.
#
# NSIS is open source (https://nsis.sourceforge.io/). This script does not
# vendor the compiler. It uses makensis already on PATH, or Chocolatey when
# -BootstrapNsis is set.
#
# Prerequisites:
#   - dist\shell-stage from Package-WinShell-Portable.ps1, or -StageDir, or
#   - -RefreshPortable to stage the Engine Release tree first
#
# Output:
#   dist\airdour-<Version>-win64-setup.exe        (engine stage)
#   dist\airdour-<Version>-win64-setup-ui.exe     (UI-only stage)
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$StageDir = "",
    [string]$MakensisPath = "",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown",
    [switch]$RefreshPortable,
    [switch]$BootstrapNsis
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Convert-ToNsisPath([string]$Path, [string]$FromDir) {
    # makensis uses the .nsi directory as cwd. Relative forward-slash paths
    # avoid the Windows compile-time /FileExists miss on absolute "D:/..." paths.
    $fromFull = [System.IO.Path]::GetFullPath($FromDir)
    if (-not $fromFull.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $fromFull += [IO.Path]::DirectorySeparatorChar
    }
    $toFull = [System.IO.Path]::GetFullPath($Path)
    $rel = [System.Uri]::UnescapeDataString(
        ([System.Uri]$fromFull).MakeRelativeUri([System.Uri]$toFull).ToString()
    )
    $rel = ($rel -replace '\\', '/').TrimEnd('/')
    if (-not $rel) { return "." }
    return $rel
}

function Convert-ToViVersion([string]$DisplayVersion) {
    $nums = [regex]::Matches($DisplayVersion, '\d+') | ForEach-Object { [int]$_.Value }
    $parts = @()
    foreach ($n in $nums) { $parts += [Math]::Min($n, 65535) }
    while ($parts.Count -lt 4) { $parts += 0 }
    return ($parts[0..3] -join '.')
}

if ($RefreshPortable -or -not $StageDir) {
    $defaultStage = Join-Path $OutDir "shell-stage"
    if ($RefreshPortable -or -not (Test-Path (Join-Path $defaultStage "airdour_win_shell.exe"))) {
        Write-Host "Refreshing portable Engine Release stage..."
        & powershell -NoProfile -File (Join-Path $RepoRoot "tools\win-native\ci\Package-WinShell-Portable.ps1") `
            -RepoRoot $RepoRoot -OutDir $OutDir -Version $Version `
            -Mode Engine -Config Release -GitSha $GitSha -GitRef $GitRef | Out-Host
    }
    $StageDir = $defaultStage
}

$StageDir = (Resolve-Path $StageDir).Path
$exe = Join-Path $StageDir "airdour_win_shell.exe"
if (-not (Test-Path $exe)) {
    throw "Missing $exe - run Package-WinShell-Portable.ps1 first or pass -RefreshPortable"
}
$launcher = Join-Path $StageDir "run-airdour.bat"
if (-not (Test-Path $launcher)) {
    throw "Missing $launcher - the portable stage must include run-airdour.bat"
}

$copying = Join-Path $RepoRoot "COPYING"
if (Test-Path $copying) {
    Copy-Item -Force $copying (Join-Path $StageDir "COPYING")
}

function Find-Makensis([string]$Hint) {
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $cmd = Get-Command makensis -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $pf86 = ${env:ProgramFiles(x86)}
    foreach ($c in @(
        (Join-Path $pf86 "NSIS\makensis.exe"),
        (Join-Path $env:ProgramFiles "NSIS\makensis.exe"),
        (Join-Path $env:LOCALAPPDATA "NSIS\makensis.exe")
    )) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

function Install-NsisWithChoco {
    $choco = Get-Command choco -ErrorAction SilentlyContinue
    if (-not $choco) {
        throw "makensis.exe not found and Chocolatey is not available. Install NSIS 3, or pass -MakensisPath."
    }
    Write-Host "Installing NSIS via Chocolatey..."
    & choco install nsis --yes --no-progress
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) {
        throw "choco install nsis failed (exit $LASTEXITCODE)"
    }
}

$makensis = Find-Makensis $MakensisPath
if (-not $makensis -and $BootstrapNsis) {
    Install-NsisWithChoco
    $makensis = Find-Makensis ""
}
if (-not $makensis) {
    throw "makensis.exe not found. Install NSIS 3 or pass -BootstrapNsis / -MakensisPath."
}
Write-Host "makensis: $makensis"

$appVer = $Version
if ($appVer -match '^v(.+)$') { $appVer = $Matches[1] }
if ($appVer -notmatch '^[A-Za-z0-9._+-]+$') {
    throw "Version '$Version' has characters makensis /D cannot safely carry."
}
$viVer = Convert-ToViVersion $appVer

$isEngine = Test-Path (Join-Path $StageDir "backends\wasapi\wasapi_audiobackend.dll")
$outBase = if ($isEngine) {
    "airdour-$Version-win64-setup"
} else {
    "airdour-$Version-win64-setup-ui"
}
Write-Host ("Installer mode: " + $(if ($isEngine) { "Engine" } else { "UiOnly (-setup-ui.exe)" }))

$nsi = Join-Path $RepoRoot "tools\win-native\installer\airdour.nsi"
if (-not (Test-Path $nsi)) { throw "Missing $nsi" }
if (-not (Test-Path $copying)) { throw "Missing $copying" }

$outExe = Join-Path $OutDir ($outBase + ".exe")
if (Test-Path $outExe) { Remove-Item -Force $outExe }

$nsiDir = Split-Path $nsi -Parent
$sourceRel = Convert-ToNsisPath $StageDir $nsiDir
$outRel = Convert-ToNsisPath $outExe $nsiDir
$licenseRel = Convert-ToNsisPath $copying $nsiDir
Write-Host "Stage files:"
Get-ChildItem -File -Recurse $StageDir | ForEach-Object { Write-Host ("  " + $_.FullName) }
Write-Host "NSIS SOURCE_DIR=$sourceRel"
Write-Host "NSIS OUT_FILE=$outRel"
Write-Host "NSIS LICENSE_FILE=$licenseRel"

$nsisArgs = @(
    "/V3",
    "/DVERSION=$appVer",
    "/DPRODUCT_VERSION=$viVer",
    "/DSOURCE_DIR=$sourceRel",
    "/DOUT_FILE=$outRel",
    "/DLICENSE_FILE=$licenseRel",
    $nsi
)

Write-Host "Compiling installer from stage: $StageDir"
Write-Host "  Output: $outExe"
& $makensis @nsisArgs
if ($LASTEXITCODE -ne 0) {
    throw "makensis failed (exit $LASTEXITCODE)"
}
if (-not (Test-Path $outExe)) {
    throw "Installer not produced: $outExe"
}
$mb = [math]::Round(((Get-Item $outExe).Length / 1MB), 1)
Write-Host ("Created $outExe (" + $mb + " MB)")
Write-Output $outExe
