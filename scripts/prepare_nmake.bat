@echo off
where nmake >nul 2>&1
if errorlevel 1 (
    echo NMake is required. Run from an x64 Visual Studio developer prompt.
    exit /b 1
)
rem NMake does not support CMake's parallel build option.
set "CMAKE_BUILD_PARALLEL_LEVEL="
rem Keep an NMake cache incremental; a different generator requires reconfiguration.
set "PULSE_CMAKE_FRESH="
if /i "%~2"=="/clean" set "PULSE_CMAKE_FRESH=--fresh"
if exist "%~1\CMakeCache.txt" (
    findstr /x /c:"CMAKE_GENERATOR:INTERNAL=NMake Makefiles" "%~1\CMakeCache.txt" >nul
    if errorlevel 1 set "PULSE_CMAKE_FRESH=--fresh"
)
exit /b 0
