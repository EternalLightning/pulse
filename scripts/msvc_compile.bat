@rem SPDX-License-Identifier: Apache-2.0
@rem Pulse project; see LICENSE and NOTICE for licensing and attribution.
@rem Modified by EternalLightning for this independent distribution, including licensing notices.
@rem Existing third-party notices and license terms remain applicable.

@echo off
rem Ninja's dependency prefix and compiler diagnostics must share UTF-8.
rem VSLANG can fall back when only localized compiler resources are installed.
chcp 65001 >nul
set "VSLANG=1033"
%*
exit /b %errorlevel%
