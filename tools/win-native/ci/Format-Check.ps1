# Check clang-format formatting for lines changed in win_ardour sources.
#
# Usage:
#   pwsh tools/win-native/ci/Format-Check.ps1 [-BaseRef origin/master]
#   pwsh tools/win-native/ci/Format-Check.ps1 -Fix [-BaseRef origin/master]
#
# Only changed lines are checked, so legacy formatting in untouched code never
# blocks a PR. A bare clang-format run over a whole legacy file would flag
# everything. Exits 0 when clang-format is unavailable (local machines without
# LLVM); CI installs it.

param(
  [string]$BaseRef = "",
  [string]$ClangFormat = "",
  [switch]$Fix
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
Push-Location $repoRoot
try {
  if (-not $ClangFormat) {
    $cmd = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($cmd) { $ClangFormat = $cmd.Source }
  }
  if (-not $ClangFormat) {
    $llvm = Join-Path $env:ProgramFiles "LLVM\bin\clang-format.exe"
    if (Test-Path $llvm) { $ClangFormat = $llvm }
  }
  if (-not $ClangFormat) {
    $vs = Get-ChildItem (Join-Path $env:ProgramFiles "Microsoft Visual Studio") -Filter clang-format.exe -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match '\\Llvm\\x64\\bin\\clang-format\.exe$' } |
      Select-Object -First 1
    if ($vs) { $ClangFormat = $vs.FullName }
  }
  if (-not $ClangFormat) {
    Write-Warning "clang-format not found; skipping format check."
    exit 0
  }

  $base = if ($BaseRef) { $BaseRef } else { "HEAD~1" }
  & git rev-parse --verify --quiet "$base^{commit}" > $null 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "Base ref '$base' not found; skipping format check."
    exit 0
  }

  $files = @(& git diff --name-only --diff-filter=ACMR "$base" -- win_ardour 2>$null) |
    Where-Object { $_ -match '\.(cc|h)$' }
  if (-not $files -or $files.Count -eq 0) {
    Write-Host "No changed win_ardour C++ files."
    exit 0
  }

  Write-Host "Checking changed lines in $($files.Count) file(s) with $ClangFormat"
  $failed = @()
  foreach ($file in $files) {
    $diff = @(& git diff -U0 --no-color "$base" -- $file 2>$null)
    $lines = @()
    foreach ($line in $diff) {
      if ($line -match '^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@') {
        $start = [int]$Matches[1]
        $count = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
        if ($count -gt 0) { $lines += "$start`:$(($start + $count - 1))" }
      }
    }
    if ($lines.Count -eq 0) { continue }

    $fmtArgs = @()
    if ($Fix) { $fmtArgs += "-i" } else { $fmtArgs += @("--dry-run", "--Werror") }
    $fmtArgs += ($lines | ForEach-Object { "--lines=$_" }) + @($file)
    & $ClangFormat @fmtArgs
    if (-not $Fix -and $LASTEXITCODE -ne 0) { $failed += $file }
  }

  if ($failed.Count -gt 0) {
    throw "clang-format check failed for: $($failed -join ', '). Run: clang-format -i <files>"
  }
  Write-Host "Format check passed."
}
finally {
  Pop-Location
}
