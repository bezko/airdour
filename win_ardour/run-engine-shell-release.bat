@echo off
REM Launch Release airdour_win_shell with Release waf + vcpkg DLLs on PATH.
setlocal
cd /d "%~dp0\.."

set "ROOT=%CD%"
if not defined VCPKG_INSTALLED_DIR set "VCPKG_INSTALLED_DIR=%ROOT%\tools\win-native\vcpkg_installed"
set "VCPKG=%VCPKG_INSTALLED_DIR%\x64-windows"
if not defined AIRDOUR_WAF_BUILD_DIR set "AIRDOUR_WAF_BUILD_DIR=%ROOT%\build-rel"
set "WAF=%AIRDOUR_WAF_BUILD_DIR%\libs"
set "EXE=%ROOT%\build-win64-gui-engine-rel\artifacts\Release\airdour_win_shell.exe"
if not exist "%EXE%" if exist "%ROOT%\build-ninja-engine\artifacts\airdour_win_shell.exe" (
  set "EXE=%ROOT%\build-ninja-engine\artifacts\airdour_win_shell.exe"
)

if not exist "%EXE%" (
  echo Missing airdour_win_shell.exe
  echo Build: cmake --preset engine ^(in cmake\win64^) or
  echo        cmake --build build-win64-gui-engine-rel --config Release --target airdour_win_shell
  exit /b 1
)

set "PATH=%VCPKG%\bin;%WAF%\pbd;%WAF%\temporal;%WAF%\midi++2;%WAF%\evoral;%WAF%\ardour;%WAF%\audiographer;%WAF%\ptformat;%WAF%\lua;%WAF%\libltc;%WAF%\ctrl-interface\control_protocol;%WAF%\backends\dummy;%WAF%\backends\wasapi;%WAF%\ardour\panners;%WAF%\fst;%PATH%"
set "ARDOUR_DLL_PATH=%WAF%\ardour"
set "ARDOUR_BACKEND_PATH=%WAF%\backends\dummy;%WAF%\backends\wasapi"
REM VST3 scanner next to libardour (PluginManager searches dll_dir and dll_dir\fst)
if exist "%WAF%\fst\ardour-vst3-scanner.exe" if not exist "%WAF%\ardour\fst\ardour-vst3-scanner.exe" (
  mkdir "%WAF%\ardour\fst" 2>nul
  copy /Y "%WAF%\fst\ardour-vst3-scanner.exe" "%WAF%\ardour\fst\" >nul
)

if not exist "%WAF%\ardour\LV2\reasonablesynth.lv2\reasonablesynth.dll" (
  if exist "%WAF%\LV2\reasonablesynth.lv2\reasonablesynth.dll" (
    mkdir "%WAF%\ardour\LV2\reasonablesynth.lv2" 2>nul
    copy /Y "%WAF%\LV2\reasonablesynth.lv2\*" "%WAF%\ardour\LV2\reasonablesynth.lv2\" >nul
  )
)
if not exist "%WAF%\ardour\LV2\a-reverb.lv2\a-reverb.dll" (
  if exist "%WAF%\LV2\a-reverb.lv2\a-reverb.dll" (
    mkdir "%WAF%\ardour\LV2\a-reverb.lv2" 2>nul
    copy /Y "%WAF%\LV2\a-reverb.lv2\*" "%WAF%\ardour\LV2\a-reverb.lv2\" >nul
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

echo PATH includes Release waf libs. Launching:
echo   %EXE% %*
"%EXE%" %*
echo EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
