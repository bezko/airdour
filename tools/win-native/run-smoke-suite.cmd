@echo off
rem Run the CTest smoke suite (UI + engine-linked, depending on the build).
rem Usage: run-smoke-suite.cmd [build-dir] [extra ctest args...]
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%ROOT%\build-ninja-ui"
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo ERROR: no CMake build in "%BUILD_DIR%".
  echo Configure first, e.g.: cmake --preset ui
  exit /b 1
)
set "CTEST_CONFIG="
findstr /C:"CMAKE_CONFIGURATION_TYPES" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1 && set "CTEST_CONFIG=-C Release"
ctest --test-dir "%BUILD_DIR%" --output-on-failure %CTEST_CONFIG% %2 %3 %4 %5 %6 %7 %8 %9
exit /b %ERRORLEVEL%
