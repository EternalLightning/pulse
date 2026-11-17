# SPDX-License-Identifier: Apache-2.0
param([string]$BuildDirectory = 'build-release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path (Join-Path $root $BuildDirectory)).Path
$fixture = 'preview_visibility_application_test'
$source = Join-Path $root "src\bench\$fixture.cpp"
$environment = & cmd.exe /d /c "call `"$PSScriptRoot\vcvars.bat`" >nul && set"
if ($LASTEXITCODE) { throw 'Cannot initialize the x64 compiler environment' }
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0,$separator), $line.Substring($separator+1), 'Process')
    }
}
$ninja = Get-Content -LiteralPath (Join-Path $build 'build.ninja')
$edge = $ninja | Where-Object { $_.StartsWith('build Pulse.exe:') } | Select-Object -First 1
if (!$edge) { throw 'Build pulse with Ninja before running this fixture.' }
$objects = (($edge -split ' \|')[0] -replace '^build Pulse.exe: \S+ ', '') -split ' '
# The fixture includes this implementation to seed a fake provider on its STA.
$objects = ($objects | Where-Object { $_ -notlike '*\preview_handler_host.cpp.obj' }) -join ' '
$index = [array]::IndexOf($ninja, $edge)
$libraries = ($ninja[($index + 3)] -replace '^  LINK_LIBRARIES = ', '')
Push-Location $build
try {
    & cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /W4 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /c $source "/Fo$fixture.obj"
    if ($LASTEXITCODE) { throw "Fixture compilation failed: $LASTEXITCODE" }
    Set-Content -LiteralPath "$fixture.rsp" -Encoding ascii -Value "$objects $fixture.obj $libraries /OUT:$fixture.exe /SUBSYSTEM:CONSOLE /MACHINE:X64 /DELAYLOAD:lumatext.dll /MANIFEST:NO"
    & link /nologo "@$fixture.rsp"
    if ($LASTEXITCODE) { throw "Fixture link failed: $LASTEXITCODE" }
    & (Join-Path $build "$fixture.exe")
    if ($LASTEXITCODE) { throw "Fixture failed: $LASTEXITCODE" }
} finally { Pop-Location }
