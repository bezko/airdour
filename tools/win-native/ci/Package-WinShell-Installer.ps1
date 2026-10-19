# Build an Inno Setup installer for the Airdour Engine Release portable shell.
#
# Prerequisites:
#   - dist\shell-stage already filled by Package-WinShell-Portable.ps1, OR
#   - -RefreshPortable to run the portable packager first
#   - Inno Setup 6 ISCC.exe (auto-detected; optional -BootstrapInno)
#
# Output:
#   dist\airdour-<Version>-win64-setup.exe
#
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$StageDir = "",
    [string]$IsccPath = "",
    [string]$GitSha = "unknown",
    [string]$GitRef = "unknown",
    [switch]$RefreshPortable,
    [switch]$BootstrapInno
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path $RepoRoot).Path
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

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

function Find-Iscc([string]$Hint) {
    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }
    $pf86 = ${env:ProgramFiles(x86)}
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path $pf86 "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Install-InnoSetupCurrentUser {
    $cache = Join-Path $RepoRoot "tools\win-native\installer\cache"
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $setup = Join-Path $cache "innosetup-6.7.3.exe"
    if (-not (Test-Path $setup)) {
        Write-Host "Downloading Inno Setup 6.7.3..."
        Invoke-WebRequest -Uri "https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe" `
            -OutFile $setup -UseBasicParsing
    }
    Write-Host "Installing Inno Setup (CURRENTUSER /VERYSILENT)..."
    $p = Start-Process -FilePath $setup -ArgumentList @(
        "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/SP-"
    ) -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        throw "Inno Setup bootstrap failed (exit $($p.ExitCode))"
    }
}

$iscc = Find-Iscc $IsccPath
if (-not $iscc) {
    if ($BootstrapInno) {
        Install-InnoSetupCurrentUser
        $iscc = Find-Iscc ""
    }
}
if (-not $iscc) {
    throw "ISCC.exe not found. Install Inno Setup 6 or pass -BootstrapInno / -IsccPath"
}
Write-Host "ISCC: $iscc"

# Strip leading 'v' for AppVersion display if present.
$appVer = $Version
if ($appVer -match '^v(.+)$') { $appVer = $Matches[1] }
# Drop -rel suffix noise for AppVersion (keep in filename).
$appVerClean = ($appVer -replace '-rel$', '')

# Engine stage has backends\wasapi; UiOnly (GHA) does not — suffix the setup name.
$isEngine = Test-Path (Join-Path $StageDir "backends\wasapi\wasapi_audiobackend.dll")
$outBase = if ($isEngine) {
    "airdour-$Version-win64-setup"
} else {
    "airdour-$Version-win64-setup-ui"
}
Write-Host ("Installer mode: " + $(if ($isEngine) { "Engine" } else { "UiOnly (-setup-ui.exe)" }))

$iss = Join-Path $RepoRoot "tools\win-native\installer\airdour.iss"
if (-not (Test-Path $iss)) { throw "Missing $iss" }

# Inno prefers forward slashes in #define paths.
$stageFwd = $StageDir -replace '\\', '/'
$outFwd = $OutDir -replace '\\', '/'
$defines = @(
    "/DMyAppVersion=$appVerClean",
    "/DMyAppSourceDir=$stageFwd",
    "/DMyAppOutputDir=$outFwd",
    "/DMyAppOutputBase=$outBase"
)

Write-Host "Compiling installer from stage: $StageDir"
Write-Host ("  Output: " + (Join-Path $OutDir ($outBase + ".exe")))
$p = Start-Process -FilePath $iscc -ArgumentList ($defines + @($iss)) -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0) {
    throw "ISCC failed (exit $($p.ExitCode))"
}

$outExe = Join-Path $OutDir ($outBase + ".exe")
if (-not (Test-Path $outExe)) {
    throw "Installer not produced: $outExe"
}
$mb = [math]::Round(((Get-Item $outExe).Length / 1MB), 1)
Write-Host ("Created $outExe (" + $mb + " MB)")
Write-Output $outExe
