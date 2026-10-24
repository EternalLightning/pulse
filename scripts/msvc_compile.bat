@echo off
rem Ninja's dependency prefix and compiler diagnostics must share UTF-8.
rem VSLANG can fall back when only localized compiler resources are installed.
chcp 65001 >nul
set "VSLANG=1033"
%*
exit /b %errorlevel%
