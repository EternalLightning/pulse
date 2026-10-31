@rem SPDX-License-Identifier: Apache-2.0
@rem Pulse project; see LICENSE and NOTICE for licensing and attribution.
@rem Modified by EternalLightning for this independent distribution, including licensing notices.
@rem Existing third-party notices and license terms remain applicable.

@echo off
rem Keep MSVC include diagnostics consistent with Ninja dependency parsing.
chcp 65001 >nul
set "VSLANG=1033"
rem Select the same supported compiler even from an inherited developer prompt.
set "PULSE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%PULSE_VSWHERE%" (
    echo Visual Studio C++ build tools are required.
    exit /b 1
)
set "PULSE_VS_PATH="
for /f "usebackq delims=" %%i in (`call "%PULSE_VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PULSE_VS_PATH=%%i"
if not defined PULSE_VS_PATH (
    echo No Visual Studio C++ toolchain found.
    exit /b 1
)
call "%PULSE_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
if not exist "%~dp0..\.build-tools\env.bat" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_build_tools.ps1" -Quiet
    if errorlevel 1 exit /b 1
)
call "%~dp0..\.build-tools\env.bat"
where cl >nul 2>&1
exit /b %errorlevel%
