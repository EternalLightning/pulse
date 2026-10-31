# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$root = Join-Path $repo 'bench_data\environment-repair-20261030'
$source = Join-Path $root 'source'
New-Item -ItemType Directory -Path $source -Force | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $source 'CMakeLists.txt'), @'
cmake_minimum_required(VERSION 3.25)
project(PulseEnvironmentProbe LANGUAGES CXX)
add_executable(env_probe main.cpp)
target_compile_options(env_probe PRIVATE /utf-8 /showIncludes)
'@, $utf8)
# A non-ASCII filename exercises MSVC include decoding as well as header dependencies.
$header = ([string][char]0x5305) + ([string][char]0x542b) + '.h'
[IO.File]::WriteAllText((Join-Path $source 'main.cpp'), "#include <cstdio>`n#include `"$header`"`nint main() { std::printf(`"%d\n`", PROBE_VALUE); }`n", $utf8)
[IO.File]::WriteAllText((Join-Path $source $header), "#define PROBE_VALUE 1`n", $utf8)
$build = Join-Path $repo 'build-env-probe'
$checks = 0
function Check([bool]$Ok, [string]$Label) {
    if (!$Ok) { throw "[FAIL] $Label" }
    $script:checks++; Write-Host "[PASS] $Label"
}
function Build-Probe([string]$Label, [string[]]$Extra = @(), [int]$Expected = 0, [string]$Directory = 'build-env-probe') {
    $log = Join-Path $root "$Label.log"
    $argsForBuild = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'build.ps1'),
        '-BuildDir',$Directory,'-SourceDir','bench_data/environment-repair-20261030/source','-Targets','env_probe') + $Extra
    # Capture expected failures as output instead of allowing native stderr to bypass the assertions.
    $saved = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & powershell.exe @argsForBuild *> $log
    $exit = $LASTEXITCODE; $ErrorActionPreference = $saved
    Check ($exit -eq $Expected) "$Label returns expected exit code $Expected"
    return Get-Content -LiteralPath $log -Raw
}
$null = Build-Probe 'probe-first' @('-Configure','-Clean')
$output = & (Join-Path $build 'env_probe.exe')
Check ($output -eq '1') 'first executable contains the initial header value'
$log = Build-Probe 'probe-no-change'
Check ($log -match 'no work to do' -and $log -notmatch 'recovering|premature end') 'unchanged project needs no compilation and has no damaged-cache warning'
[IO.File]::WriteAllText((Join-Path $source $header), "#define PROBE_VALUE 2`n", $utf8)
$log = Build-Probe 'probe-header-change'
$output = & (Join-Path $build 'env_probe.exe')
Check ($output -eq '2') 'changing a non-ASCII header recompiles the actual executable'
Check ($log -match 'Building CXX object') 'header change schedules a real object compilation'
$log = Build-Probe 'probe-no-change-again'
Check ($log -match 'no work to do' -and $log -notmatch 'recovering|premature end') 'dependency cache survives another separate build process'
$hash = [Security.Cryptography.SHA256]::Create()
try { $key = ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($build.ToUpperInvariant())))).Replace('-', '') }
finally { $hash.Dispose() }
$gate = [IO.File]::Open((Join-Path $repo ".build-tools\locks\$key.lock"), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $log = Build-Probe 'probe-lock-contention' @() 1
    Check ($log -match 'Another build owns') 'a second owner cannot write the same build cache'
} finally { $gate.Dispose() }
$log = Build-Probe 'probe-lock-released'
Check ($log -match 'no work to do') 'disposing the lock permits the next build normally'
$log = Build-Probe 'probe-unsafe-clean' @('-Clean') 1 'src'
Check ((Test-Path -LiteralPath (Join-Path $repo 'src\app\app_main.cpp')) -and $log -match 'Use a build') 'unsafe cleanup target is rejected before deleting source'
Write-Host "RESULT checks=$checks failures=0"
