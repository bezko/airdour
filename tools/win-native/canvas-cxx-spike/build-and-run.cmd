@echo off
setlocal
cd /d "%~dp0"
set PREFIX=%CD%\vcpkg_installed\x64-windows
if not exist "%PREFIX%\include\cairomm\cairomm.h" (
  echo Run: powershell -ExecutionPolicy Bypass -File Install-CanvasCxxSpike.ps1
  exit /b 1
)

set CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
if not exist "%CMAKE%" set CMAKE=cmake

"%CMAKE%" -S . -B ..\..\..\build-canvas-cxx-spike -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
"%CMAKE%" --build ..\..\..\build-canvas-cxx-spike --config Release --target cairomm_probe
if errorlevel 1 exit /b 1

set PATH=%PREFIX%\bin;%PATH%
..\..\..\build-canvas-cxx-spike\artifacts\Release\cairomm_probe.exe
set RC=%ERRORLEVEL%

echo.
echo --- canvas_types_probe (expect compile FAIL) ---
"%CMAKE%" --build ..\..\..\build-canvas-cxx-spike --config Release --target canvas_types_probe
echo canvas_types_probe build exit=%ERRORLEVEL% (nonzero expected without gtkmm2ext/ytkmm)

exit /b %RC%
