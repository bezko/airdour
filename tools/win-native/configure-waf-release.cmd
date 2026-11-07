@echo off
rem Configure the Airdour waf engine build (Release, MSVC, Windows-only).
rem Usage: configure-waf-release.cmd [waf-out-dir]   (default: <repo>\build-rel)
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

if "%~1"=="" (
  call "%~dp0msvc-env.cmd" --waf-probe
) else (
  call "%~dp0msvc-env.cmd" %1 --waf-probe
)
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%ROOT%"
set "PKG_CONFIG_PATH=%VCPKG%\lib\pkgconfig"
echo PKG_CONFIG_PATH=%PKG_CONFIG_PATH%
pkg-config --exists glib-2.0 && echo glib OK || echo glib MISSING
rem --msvc_targets=x64: do not let a pre-seeded Developer Prompt pick arm64/x86 first.
python waf configure -o "%WAF_OUT%" --optimize --dist-target=msvc --windows-only --cxx17 --msvc_targets=x64 --prefix="%WAF_OUT%\prefix" --program-name=Airdour
exit /b %ERRORLEVEL%
