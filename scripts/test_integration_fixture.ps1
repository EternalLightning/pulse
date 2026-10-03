param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-z0-9_]+$')][string]$Fixture,
    [string]$BuildDirectory = 'build',
    [string[]]$FixtureArguments = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path (Join-Path $root $BuildDirectory)).Path
$source = Join-Path $root "src\bench\$Fixture.cpp"
if (!(Test-Path -LiteralPath $source)) { throw "Missing fixture: $source" }
$ninja = Get-Content -LiteralPath (Join-Path $build 'build.ninja')
$edge = $ninja | Where-Object { $_.StartsWith('build Pulse.exe:') } | Select-Object -First 1
if (!$edge) { throw 'Build pulse with Ninja before running this fixture.' }
$objects = (($edge -split ' \|')[0] -replace '^build Pulse.exe: \S+ ', '')
$index = [array]::IndexOf($ninja, $edge)
$libraries = ($ninja[($index + 3)] -replace '^  LINK_LIBRARIES = ', '')
Push-Location $build
try {
    & cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /W4 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /c $source "/Fo$Fixture.obj"
    if ($LASTEXITCODE) { throw "Fixture compilation failed: $LASTEXITCODE" }
    $response = "$objects $Fixture.obj $libraries /OUT:$Fixture.exe /SUBSYSTEM:CONSOLE /MACHINE:X64 /DELAYLOAD:lumatext.dll /MANIFEST:NO"
    Set-Content -LiteralPath "$Fixture.rsp" -Encoding ascii -Value $response
    & link /nologo "@$Fixture.rsp"
    if ($LASTEXITCODE) { throw "Fixture link failed: $LASTEXITCODE" }
    & (Join-Path $build "$Fixture.exe") @FixtureArguments
    if ($LASTEXITCODE) { throw "Fixture failed: $LASTEXITCODE" }
} finally { Pop-Location }
