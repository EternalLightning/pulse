@echo off
call scripts\vcvars.bat >nul
if errorlevel 1 exit /b %errorlevel%
set "PULSE_AV_EVIDENCE=docs\validation\1.1.1.0\antivirus-main-2026-10-12\worker-isolation"
dumpbin /headers build\Pulse.exe > "%PULSE_AV_EVIDENCE%\main-pe-headers.txt"
if errorlevel 1 exit /b %errorlevel%
dumpbin /dependents build\Pulse.exe > "%PULSE_AV_EVIDENCE%\main-dependents.txt"
if errorlevel 1 exit /b %errorlevel%
dumpbin /headers build\Pulse.Shell.exe > "%PULSE_AV_EVIDENCE%\shell-pe-headers.txt"
if errorlevel 1 exit /b %errorlevel%
dumpbin /dependents build\Pulse.Shell.exe > "%PULSE_AV_EVIDENCE%\shell-dependents.txt"
if errorlevel 1 exit /b %errorlevel%
mt -nologo "-inputresource:build\Pulse.exe;#1" "-out:%PULSE_AV_EVIDENCE%\main-embedded-manifest.xml"
if errorlevel 1 exit /b %errorlevel%
mt -nologo "-inputresource:build\Pulse.Shell.exe;#1" "-out:%PULSE_AV_EVIDENCE%\shell-embedded-manifest.xml"
exit /b %errorlevel%
