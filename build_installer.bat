@echo off
rem Build Release binaries, then compile dist\PulseSetup-<version>.exe.
setlocal
cd /d "%~dp0"
set "PULSE_BUILD_DIR=%~2"
if not defined PULSE_BUILD_DIR set "PULSE_BUILD_DIR=build-release"

set /p PULSE_VERSION=<version.txt
if not defined PULSE_VERSION (
    echo Missing version in version.txt
    exit /b 1
)

if /i "%~1"=="/skipbuild" goto after_release_build
call "%~dp0build_release.bat" "" "%PULSE_BUILD_DIR%" || exit /b 1
:after_release_build

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\check_release_payload.ps1" -BuildDir "%PULSE_BUILD_DIR%" || exit /b 1

for %%F in (Pulse.exe Pulse.Index.exe Pulse.Document.exe Pulse.Preview.exe Pulse.Shell.exe lumatext.dll) do (
    if not exist "%PULSE_BUILD_DIR%\%%F" (
        echo Missing build output: %PULSE_BUILD_DIR%\%%F
        exit /b 1
    )
)

set "ISCC="
where ISCC.exe >nul 2>&1 && set "ISCC=ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\stage_installer_runtime.ps1" -BuildDir "%PULSE_BUILD_DIR%" -RequireStaticRuntime || exit /b 1

"%ISCC%" /DAppVersion=%PULSE_VERSION% "/DBuildDir=%PULSE_BUILD_DIR%" installer\PulseSetup.iss || exit /b 1
echo.
echo Installer written to dist\
