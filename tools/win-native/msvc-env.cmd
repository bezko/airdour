@echo off
rem Shared MSVC + vcpkg environment for Airdour waf builds.
rem Usage: call msvc-env.cmd [waf-out-dir] [--waf-probe]
rem
rem --waf-probe  Short PATH + no VsDevCmd. Waf re-invokes vcvarsall itself;
rem              stacking that on the already-huge GHA/ilammy PATH makes
rem              waf-print-msvc-*.bat exit 255 ("input line is too long").
rem
rem Exports ROOT, VCPKG, WAF_OUT into the caller (setlocal must be in the caller).

set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not defined VCPKG_INSTALLED_DIR set "VCPKG_INSTALLED_DIR=%ROOT%\tools\win-native\vcpkg_installed"
set "VCPKG=%VCPKG_INSTALLED_DIR%\x64-windows"
if "%~1"=="" (set "WAF_OUT=%ROOT%\build-rel") else (set "WAF_OUT=%~1")
if /I "%~1"=="--waf-probe" (
  set "WAF_OUT=%ROOT%\build-rel"
  set "AIRDOUR_WAF_PROBE=1"
)
if /I "%~2"=="--waf-probe" set "AIRDOUR_WAF_PROBE=1"

set "PKG_CONFIG_PATH=%VCPKG%\lib\pkgconfig"
set "INCLUDE=%VCPKG%\include;%INCLUDE%"
set "LIB=%VCPKG%\lib;%LIB%"

if defined AIRDOUR_WAF_PROBE goto :waf_probe

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
if not defined DevEnvDir (
  call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
  if errorlevel 1 exit /b %ERRORLEVEL%
)
set "PATH=%VCPKG%\bin;%VCPKG%\tools\pkgconf;%PATH%"
exit /b 0

:waf_probe
rem Keep only what waf + pkg-config need. Waf discovers cl.exe via vcvarsall.
set "PY_DIR="
for /f "delims=" %%P in ('where python 2^>nul') do (
  set "PY_DIR=%%~dpP"
  goto :got_py
)
:got_py
if not defined PY_DIR (
  echo ERROR: python not found on PATH before waf probe prune.
  exit /b 1
)
rem Drop trailing backslash from for /f dp for cleaner PATH joins
if "%PY_DIR:~-1%"=="\" set "PY_DIR=%PY_DIR:~0,-1%"
set "PATH=%VCPKG%\bin;%VCPKG%\tools\pkgconf;%PY_DIR%;%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%ProgramFiles%\Git\cmd;%ProgramFiles%\Git\bin"
rem Stop waf's msvc options() from preferring a non-registry VSCMD_VER like 17.14
set "VSCMD_VER="
set "VSCMD_ARG_TGT_ARCH="
set "VSCMD_ARG_HOST_ARCH="
set "VSCMD_ARG_app_plat="
set "DevEnvDir="
set "VSINSTALLDIR="
set "VCINSTALLDIR="
set "VCToolsInstallDir="
set "VCToolsVersion="
set "VSCMD_VER="
echo waf-probe PATH length: 
powershell -NoProfile -Command "Write-Host $env:PATH.Length"
exit /b 0
