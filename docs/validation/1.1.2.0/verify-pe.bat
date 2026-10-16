@echo off
call scripts\vcvars.bat >nul
if errorlevel 1 exit /b %errorlevel%
for %%N in (Pulse Pulse.Index Pulse.Document Pulse.Preview Pulse.Shell) do (
    dumpbin /dependents "build\%%N.exe" > "docs\validation\1.1.2.0\%%N-dependents.txt"
    if errorlevel 1 exit /b 1
)
mt -nologo "-inputresource:build\Pulse.exe;#1" "-out:docs\validation\1.1.2.0\main-embedded-manifest.xml"
if errorlevel 1 exit /b %errorlevel%
mt -nologo "-inputresource:build\Pulse.Shell.exe;#1" "-out:docs\validation\1.1.2.0\shell-embedded-manifest.xml"
exit /b %errorlevel%
