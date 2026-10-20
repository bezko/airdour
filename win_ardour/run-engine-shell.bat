@echo off
REM Launch airdour_win_shell Debug with waf + vcpkg DLLs on PATH.
REM Run from repo root, or this script cd's there via %~dp0.
setlocal
cd /d "%~dp0\.."

set "ROOT=%CD%"
set "VCPKG=%ROOT%\tools\win-native\vcpkg_installed\x64-windows"
set "WAF=%ROOT%\build\libs"
set "EXE=%ROOT%\build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe"

if not exist "%EXE%" (
  echo Missing %EXE%
  echo Build: cmake --build build-win64-gui-engine --config Debug --target airdour_win_shell
  exit /b 1
)

REM VsDevCmd puts Debug CRT (ucrtbased / VCRUNTIME140D) on PATH.
if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" (
  call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
) else if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
  call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
)

set "PATH=%VCPKG%\debug\bin;%VCPKG%\bin;%WAF%\pbd;%WAF%\temporal;%WAF%\midi++2;%WAF%\evoral;%WAF%\ardour;%WAF%\audiographer;%WAF%\ptformat;%WAF%\lua;%WAF%\libltc;%WAF%\ctrl-interface\control_protocol;%WAF%\backends\dummy;%WAF%\backends\wasapi;%WAF%\ardour\panners;%PATH%"
set "ARDOUR_DLL_PATH=%WAF%\ardour"
set "ARDOUR_BACKEND_PATH=%WAF%\backends\dummy;%WAF%\backends\wasapi"

REM Bundled ACE LV2s: Reasonable Synth + Reverb + Compressor (audio FX).
if not exist "%WAF%\ardour\LV2\reasonablesynth.lv2\reasonablesynth.dll" (
  if exist "%WAF%\LV2\reasonablesynth.lv2\reasonablesynth.dll" (
    mkdir "%WAF%\ardour\LV2\reasonablesynth.lv2" 2>nul
    copy /Y "%WAF%\LV2\reasonablesynth.lv2\reasonablesynth.dll" "%WAF%\ardour\LV2\reasonablesynth.lv2\" >nul
    if exist "%WAF%\LV2\reasonablesynth.lv2\manifest.ttl" copy /Y "%WAF%\LV2\reasonablesynth.lv2\manifest.ttl" "%WAF%\ardour\LV2\reasonablesynth.lv2\" >nul
    if exist "%WAF%\LV2\reasonablesynth.lv2\reasonablesynth.ttl" copy /Y "%WAF%\LV2\reasonablesynth.lv2\reasonablesynth.ttl" "%WAF%\ardour\LV2\reasonablesynth.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-reverb.lv2\a-reverb.dll" (
  if exist "%WAF%\LV2\a-reverb.lv2\a-reverb.dll" (
    mkdir "%WAF%\ardour\LV2\a-reverb.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-reverb.lv2\a-reverb.dll" "%WAF%\ardour\LV2\a-reverb.lv2\" >nul
    if exist "%WAF%\LV2\a-reverb.lv2\manifest.ttl" copy /Y "%WAF%\LV2\a-reverb.lv2\manifest.ttl" "%WAF%\ardour\LV2\a-reverb.lv2\" >nul
    if exist "%WAF%\LV2\a-reverb.lv2\a-reverb.ttl" copy /Y "%WAF%\LV2\a-reverb.lv2\a-reverb.ttl" "%WAF%\ardour\LV2\a-reverb.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-comp.lv2\a-comp.dll" (
  if exist "%WAF%\LV2\a-comp.lv2\a-comp.dll" (
    mkdir "%WAF%\ardour\LV2\a-comp.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-comp.lv2\*" "%WAF%\ardour\LV2\a-comp.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-eq.lv2\a-eq.dll" (
  if exist "%WAF%\LV2\a-eq.lv2\a-eq.dll" (
    mkdir "%WAF%\ardour\LV2\a-eq.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-eq.lv2\*" "%WAF%\ardour\LV2\a-eq.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-delay.lv2\a-delay.dll" (
  if exist "%WAF%\LV2\a-delay.lv2\a-delay.dll" (
    mkdir "%WAF%\ardour\LV2\a-delay.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-delay.lv2\*" "%WAF%\ardour\LV2\a-delay.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-limiter.lv2\a-limiter.dll" (
  if exist "%WAF%\LV2\a-limiter.lv2\a-limiter.dll" (
    mkdir "%WAF%\ardour\LV2\a-limiter.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-limiter.lv2\*" "%WAF%\ardour\LV2\a-limiter.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-gate.lv2\a-gate.dll" (
  if exist "%WAF%\LV2\a-gate.lv2\a-gate.dll" (
    mkdir "%WAF%\ardour\LV2\a-gate.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-gate.lv2\*" "%WAF%\ardour\LV2\a-gate.lv2\" >nul
  )
)

echo PATH includes ptformat + waf libs. Launching:
echo   %EXE% %*
"%EXE%" %*
echo EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
