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
echo PKG_CONFIG_PATH=%PKG_CONFIG_PATH%
pkg-config --exists glib-2.0 && echo glib OK || echo glib MISSING
python waf configure -o build-rel --optimize --dist-target=msvc --windows-only --cxx17 --prefix=C:\airdour-build-rel --program-name=Airdour
exit /b %ERRORLEVEL%
