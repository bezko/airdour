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

echo PATH includes ptformat + waf libs. Launching:
echo   %EXE% %*
"%EXE%" %*
echo EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
