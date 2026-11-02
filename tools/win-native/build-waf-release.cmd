@echo off
rem Build the Airdour waf engine spine used by airdour_win_shell.
rem Usage: build-waf-release.cmd [waf-out-dir]   (default: <repo>\build-rel)
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

call "%~dp0msvc-env.cmd" %1
if errorlevel 1 exit /b %ERRORLEVEL%

cd /d "%ROOT%"
set "PKG_CONFIG_PATH=%VCPKG%\lib\pkgconfig"
REM Build product spine needed by airdour_win_shell + WASAPI + ACE LV2s + panners
python waf build -o "%WAF_OUT%" -j %NUMBER_OF_PROCESSORS% --targets=libpbd,libtemporal,libmidipp,libevoral,libardour_cp,libardour,dummy_audiobackend,wasapi_audiobackend,a-reverb,reasonablesynth,libardour_pan1in2out,libardour_pan2in2out,libardour_panvbap,libardour_panbalance,libaudiographer,libptformat,liblua,libltc,ardour-vst3-scanner
exit /b %ERRORLEVEL%
