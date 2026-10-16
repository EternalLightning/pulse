$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fixture = Join-Path $project 'bench_data/background-window-fit'
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$build = Join-Path $project 'build'
$ninja = Get-Content (Join-Path $build 'build.ninja') -Raw
$compile = [regex]::Match($ninja, '(?m)^build CMakeFiles\\pulse.dir\\src\\app\\settings_ui.cpp.obj:.*\r?\n  DEFINES = ([^\r\n]+)\r?\n  FLAGS = ([^\r\n]+)\r?\n  INCLUDES = ([^\r\n]+)')
if (-not $compile.Success) { throw 'Pulse compile flags not found' }
$link = [regex]::Match($ninja, '(?m)^build Pulse.exe: [^ ]+ ([^\r\n|]+).*\r?\n  FLAGS = [^\r\n]+\r?\n  LINK_FLAGS = ([^\r\n]+)\r?\n  LINK_LIBRARIES = ([^\r\n]+)')
if (-not $link.Success) { throw 'Pulse link inputs not found' }
$object = Join-Path $fixture 'background_window_fit_test.obj'
$exe = Join-Path $fixture 'pulse_background_window_fit_test.exe'
$compileRsp = Join-Path $fixture 'compile.rsp'
$linkRsp = Join-Path $fixture 'link.rsp'
$source = Join-Path $project 'src/bench/background_window_fit_test.cpp'
$compileText = '/nologo /c ' + $compile.Groups[1].Value + ' ' + $compile.Groups[2].Value + ' ' + $compile.Groups[3].Value + ' /Fo"' + $object + '" "' + $source + '"'
Set-Content -LiteralPath $compileRsp -Value $compileText -Encoding ascii
$flags = $link.Groups[2].Value -replace '/subsystem:windows', '/subsystem:console'
$linkText = '/nologo ' + $flags + ' /OUT:"' + $exe + '" "' + $object + '" ' + $link.Groups[1].Value + ' ' + $link.Groups[3].Value
Set-Content -LiteralPath $linkRsp -Value $linkText -Encoding ascii
$batch = Join-Path $fixture 'compile_fixture.cmd'
@"
@echo off
call "$project\scripts\vcvars.bat"
if errorlevel 1 exit /b %errorlevel%
cd /d "$build"
cl.exe @"$compileRsp"
if errorlevel 1 exit /b %errorlevel%
link.exe @"$linkRsp"
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& cmd.exe /d /s /c ('"' + $batch + '"')
if ($LASTEXITCODE -ne 0) { throw "Fixture compile/link failed: $LASTEXITCODE" }
Copy-Item -LiteralPath (Join-Path $build 'lumatext.dll') -Destination $fixture -Force
Write-Output "Built $exe"
