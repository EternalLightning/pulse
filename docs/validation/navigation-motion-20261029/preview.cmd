@echo off
setlocal
cd /d "%~dp0..\..\.."
set "PULSE_TEST_DATA_DIR=%CD%\bench_data\navigation-motion-preview"
if not exist "%PULSE_TEST_DATA_DIR%" mkdir "%PULSE_TEST_DATA_DIR%"
start "" "%CD%\build-release\Pulse.exe" --test-instance "%CD%"
