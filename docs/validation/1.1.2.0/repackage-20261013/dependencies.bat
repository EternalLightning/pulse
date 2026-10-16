@echo off
call scripts\vcvars.bat >nul
if errorlevel 1 exit /b 1
for %%N in (Pulse.exe Pulse.Index.exe Pulse.Document.exe Pulse.Preview.exe Pulse.Shell.exe lumatext.dll pdfium.dll 7zip\7z.exe 7zip\7z.dll) do (
    echo ==== %%N ====
    dumpbin /dependents "build\%%N"
    if errorlevel 1 exit /b 1
)
