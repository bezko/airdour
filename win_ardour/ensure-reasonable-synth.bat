@echo off
REM Build ACE Reasonable Synth LV2 and install under ARDOUR_DLL_PATH\LV2.
setlocal
cd /d "%~dp0\.."
set "WAF=%CD%\build\libs"
set "SRC=%WAF%\LV2\reasonablesynth.lv2"
set "DST=%WAF%\ardour\LV2\reasonablesynth.lv2"
set "IN=%CD%\libs\plugins\reasonablesynth.lv2"

echo Building reasonablesynth…
python waf --targets=reasonablesynth
if errorlevel 1 exit /b 1

if not exist "%SRC%\reasonablesynth.dll" (
  echo Missing %SRC%\reasonablesynth.dll
  exit /b 1
)

mkdir "%DST%" 2>nul
copy /Y "%SRC%\reasonablesynth.dll" "%DST%\" >nul

REM waf subst for TTL is flaky on this tree — write from .in
powershell -NoProfile -Command ^
  "$utf8 = New-Object System.Text.UTF8Encoding $false; " ^
  "$m = '@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .`n@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .`n`n<https://community.ardour.org/node/7596>`n`ta lv2:Plugin ;`n`tlv2:binary <reasonablesynth.dll>  ;`n`trdfs:seeAlso <reasonablesynth.ttl> .`n'; " ^
  "[IO.File]::WriteAllText('%DST%\manifest.ttl', $m, $utf8); " ^
  "[IO.File]::WriteAllText('%DST%\reasonablesynth.ttl', (Get-Content -Raw '%IN%\reasonablesynth.ttl.in'), $utf8)"

echo Installed:
dir /b "%DST%"
exit /b 0
