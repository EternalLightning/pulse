@rem SPDX-License-Identifier: Apache-2.0
@rem Pulse project; see LICENSE and NOTICE for licensing and attribution.
@rem Modified by EternalLightning for this independent distribution, including licensing notices.
@rem Existing third-party notices and license terms remain applicable.

@echo off
setlocal
cd /d "%~dp0"
set "PULSE_RELEASE_BUILD=%~2"
if not defined PULSE_RELEASE_BUILD set "PULSE_RELEASE_BUILD=build-release"
set "PULSE_CLEAN_ARG="
if /i "%~1"=="/clean" set "PULSE_CLEAN_ARG=-Clean"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -BuildDir "%PULSE_RELEASE_BUILD%" -Configure %PULSE_CLEAN_ARG%
exit /b %errorlevel%
