@echo off
rem Shared MSVC + vcpkg environment for Airdour waf builds.
rem Usage: call msvc-env.cmd [waf-out-dir]
rem Exports ROOT, VCPKG, WAF_OUT into the caller (setlocal must be in the caller).

set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not defined VCPKG_INSTALLED_DIR set "VCPKG_INSTALLED_DIR=%ROOT%\tools\win-native\vcpkg_installed"
set "VCPKG=%VCPKG_INSTALLED_DIR%\x64-windows"
if "%~1"=="" (set "WAF_OUT=%ROOT%\build-rel") else (set "WAF_OUT=%~1")

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found - install Visual Studio with the C++ workload.
  exit /b 1
)
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo ERROR: no Visual Studio installation with the MSVC x64 toolset.
  exit /b 1
)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b %ERRORLEVEL%

set "PKG_CONFIG_PATH=%VCPKG%\lib\pkgconfig"
set "PATH=%VCPKG%\bin;%VCPKG%\tools\pkgconf;%PATH%"
set "INCLUDE=%VCPKG%\include;%INCLUDE%"
set "LIB=%VCPKG%\lib;%LIB%"
exit /b 0
