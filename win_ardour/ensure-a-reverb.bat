@echo off
REM Build ACE Reverb LV2 and install under ARDOUR_DLL_PATH\LV2.
setlocal
cd /d "%~dp0\.."
set "WAF=%CD%\build\libs"
set "SRC=%WAF%\LV2\a-reverb.lv2"
set "DST=%WAF%\ardour\LV2\a-reverb.lv2"
set "IN=%CD%\libs\plugins\a-reverb.lv2"

echo Building a-reverb…
python waf --targets=a-reverb
if errorlevel 1 exit /b 1

if not exist "%SRC%\a-reverb.dll" (
  echo Missing %SRC%\a-reverb.dll
  exit /b 1
)

mkdir "%DST%" 2>nul
copy /Y "%SRC%\a-reverb.dll" "%DST%\" >nul

REM Write TTL with real newlines (avoid PowerShell `n escaping traps).
powershell -NoProfile -Command ^
  "$utf8 = New-Object System.Text.UTF8Encoding $false; " ^
  "$nl = [char]10; " ^
  "$m = '@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .' + $nl + " ^
  "  '@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .' + $nl + $nl + " ^
  "  '<urn:ardour:a-reverb>' + $nl + " ^
  "  [char]9 + 'a lv2:Plugin ;' + $nl + " ^
  "  [char]9 + 'lv2:binary <a-reverb.dll>  ;' + $nl + " ^
  "  [char]9 + 'rdfs:seeAlso <a-reverb.ttl> .' + $nl; " ^
  "[IO.File]::WriteAllText((Join-Path '%DST%' 'manifest.ttl'), $m, $utf8); " ^
  "$ttl = (Get-Content -Raw '%IN%\a-reverb.ttl.in') -replace '@LIB_EXT@','.dll'; " ^
  "if (-not $ttl) { $ttl = Get-Content -Raw '%IN%\a-reverb.ttl.in' }; " ^
  "[IO.File]::WriteAllText((Join-Path '%DST%' 'a-reverb.ttl'), $ttl, $utf8)"

echo Installed:
dir /b "%DST%"
exit /b 0
