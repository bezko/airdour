@echo off
setlocal
set ROOT=C:\Users\mathieu\Documents\GitHub\airdour
set VCPKG=%ROOT%\tools\win-native\vcpkg_installed\x64-windows
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
cd /d "%ROOT%"
set PKG_CONFIG_PATH=%VCPKG%\lib\pkgconfig
set PATH=%VCPKG%\bin;%VCPKG%\tools\pkgconf;%PATH%
set INCLUDE=%VCPKG%\include;%INCLUDE%
set LIB=%VCPKG%\lib;%LIB%
REM Build product spine needed by airdour_win_shell + WASAPI + ACE LV2s + panners
python waf build -o build-rel -j %NUMBER_OF_PROCESSORS% --targets=libpbd,libtemporal,libmidipp,libevoral,libardour_cp,libardour,dummy_audiobackend,wasapi_audiobackend,a-reverb,reasonablesynth,libardour_pan1in2out,libardour_pan2in2out,libardour_panvbap,libardour_panbalance,libaudiographer,libptformat,liblua,libltc,ardour-vst3-scanner
exit /b %ERRORLEVEL%
