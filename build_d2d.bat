@rem SPDX-License-Identifier: Apache-2.0
@rem Pulse project; see LICENSE and NOTICE for licensing and attribution.
@rem Modified by EternalLightning for this independent distribution, including licensing notices.
@rem Existing third-party notices and license terms remain applicable.

@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -BuildDir build-d2d -Targets pulse_d2d_list -Configure
exit /b %errorlevel%
