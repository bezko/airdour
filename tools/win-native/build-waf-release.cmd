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
python waf build -o "%WAF_OUT%" -j %NUMBER_OF_PROCESSORS% --targets=libpbd,libtemporal,libmidipp,libevoral,libardour_cp,libardour,dummy_audiobackend,wasapi_audiobackend,a-reverb,reasonablesynth,a-comp,a-eq,a-delay,a-limiter,a-gate,libardour_pan1in2out,libardour_pan2in2out,libardour_panvbap,libardour_panbalance,libaudiographer,libptformat,liblua,libltc,ardour-vst3-scanner
set "WAF_RC=%ERRORLEVEL%"
if not "%WAF_RC%"=="0" exit /b %WAF_RC%

rem Panner modules are dlopen'd from <dll dir>\panners (panner_search_path) and
rem the bundled LV2s are scanned in <dll dir>\LV2, but waf writes them under
rem libs\panners\<variant> and libs\LV2. Without this stage, Session startup
rem finds no panner for 2/2 and PannerShell aborts with "No panner found".
if not exist "%WAF_OUT%\libs\ardour\panners" mkdir "%WAF_OUT%\libs\ardour\panners"
for /r "%WAF_OUT%\libs\panners" %%F in (*.dll) do copy /Y "%%F" "%WAF_OUT%\libs\ardour\panners\" >nul
if not exist "%WAF_OUT%\libs\ardour\LV2" mkdir "%WAF_OUT%\libs\ardour\LV2"
for /d %%D in ("%WAF_OUT%\libs\LV2\*.lv2") do xcopy /E /I /Y "%%~fD" "%WAF_OUT%\libs\ardour\LV2\%%~nxD\" >nul
exit /b 0
