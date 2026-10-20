@echo off
REM Build + run gtk_lib_probe against tools/win-native/vcpkg_installed (Release).
setlocal
cd /d "%~dp0\..\..\.."
set "ROOT=%CD%"
set "PROBE=%ROOT%\tools\win-native\gtk-lib-probe"
set "BUILD=%ROOT%\build-gtk-lib-probe"
set "VCPKG=%ROOT%\tools\win-native\vcpkg_installed\x64-windows"

if not exist "%VCPKG%\include\gtk-3.0\gtk\gtk.h" (
  echo Missing GTK3 under %VCPKG%
  echo Run: powershell -ExecutionPolicy Bypass -File tools\win-native\Install-HeadlessDeps.ps1 -WithGtkLibs
  exit /b 1
)

if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" (
  call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
) else if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
  call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
)

cmake -S "%PROBE%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 ^
  -D_VCPKG_PREFIX="%VCPKG:\=/%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" --config Release --target gtk_lib_probe
if errorlevel 1 exit /b 1

set "EXE=%BUILD%\artifacts\Release\gtk_lib_probe.exe"
if not exist "%EXE%" set "EXE=%BUILD%\Release\gtk_lib_probe.exe"
if not exist "%EXE%" (
  echo Missing %EXE%
  exit /b 1
)

set "PATH=%VCPKG%\bin;%PATH%"
echo Running: %EXE%
"%EXE%"
echo EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
