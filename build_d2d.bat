@echo off
setlocal
cd /d "%~dp0"
call "%~dp0scripts\vcvars.bat"
if errorlevel 1 exit /b 1
call "%~dp0scripts\prepare_nmake.bat" build-d2d
if errorlevel 1 exit /b 1
cmake %PULSE_CMAKE_FRESH% -S . -B build-d2d -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build-d2d --target pulse_d2d_list
exit /b %errorlevel%
