@echo off
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "VCPKG=%ROOT%\tools\win-native\vcpkg_installed\x64-windows"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDEVCMD="
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat > "%TEMP%\airdour-vsdevcmd.txt"
if errorlevel 1 (
  del "%TEMP%\airdour-vsdevcmd.txt" 2>nul
  echo VsDevCmd.bat not found
  exit /b 1
)
set /p VSDEVCMD=<"%TEMP%\airdour-vsdevcmd.txt"
del "%TEMP%\airdour-vsdevcmd.txt"
if not defined VSDEVCMD (
  echo VsDevCmd.bat not found
  exit /b 1
)
if "%VSDEVCMD%"=="" (
  echo VsDevCmd.bat not found
  exit /b 1
)
call "%VSDEVCMD%" -arch=amd64 -host_arch=amd64
cd /d "%ROOT%"
set PKG_CONFIG_PATH=%VCPKG%\lib\pkgconfig
set PATH=%VCPKG%\bin;%VCPKG%\tools\pkgconf;%PATH%
set INCLUDE=%VCPKG%\include;%INCLUDE%
set LIB=%VCPKG%\lib;%LIB%
echo PKG_CONFIG_PATH=%PKG_CONFIG_PATH%
pkg-config --exists glib-2.0 && echo glib OK || echo glib MISSING
python waf configure -o build-rel --optimize --dist-target=msvc --windows-only --cxx17 --prefix=C:\airdour-build-rel --program-name=Airdour
exit /b %ERRORLEVEL%
