# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDir = 'build-release',
    [string]$SourceDir = '.',
    [string[]]$Targets = @('pulse'),
    [switch]$Configure,
    [switch]$Clean,
    [switch]$EnableSelfTests,
    [switch]$ArchitectureFixtures,
    [ValidateRange(1,64)][int]$Parallel = 4,
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = 'Stop'
$env:PSModulePath = (Join-Path $PSHOME 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
if (!$build.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'BuildDir must be inside this repository' }
$relative = $build.Substring($repo.Length + 1)
if ($relative -notmatch '^(build[^\\]*|out)(\\|$)') { throw 'Use a build* directory or a directory under out' }
$source = [IO.Path]::GetFullPath((Join-Path $repo $SourceDir))
if ($source -ne $repo -and !$source.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'SourceDir must be inside this repository' }
for ($item = $build; $item.Length -gt $repo.Length; $item = Split-Path -Parent $item) {
    if ((Test-Path -LiteralPath $item) -and ((Get-Item -LiteralPath $item -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'BuildDir must not traverse reparse points'
    }
}
& (Join-Path $PSScriptRoot 'setup_build_tools.ps1') -Quiet
$tools = Get-Content -LiteralPath (Join-Path $repo '.build-tools\paths.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$locks = Join-Path $repo '.build-tools\locks'
New-Item -ItemType Directory -Path $locks -Force | Out-Null
$hasher = [Security.Cryptography.SHA256]::Create()
try { $key = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($build.ToUpperInvariant())))).Replace('-', '') }
finally { $hasher.Dispose() }
$gate = $null
try {
    try { $gate = [IO.File]::Open((Join-Path $locks "$key.lock"), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch {
        if (($_.Exception.GetBaseException().HResult -band 65535) -notin @(32,33)) { throw }
        throw "Another build owns $build. Use a different build directory or wait for it to finish."
    }
    $environment = & cmd.exe /d /c "call `"$PSScriptRoot\vcvars.bat`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the x64 compiler environment' }
    foreach ($line in $environment) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$separator), $line.Substring($separator+1), 'Process') }
    }
    foreach ($name in @('CL', '_CL_', 'LINK', '_LINK_')) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    if ($Clean -and (Test-Path -LiteralPath $build)) {
        # The absolute target, its workspace boundary and reparse parents were checked above.
        Remove-Item -LiteralPath $build -Recurse -Force
    }
    New-Item -ItemType Directory -Path $build -Force | Out-Null
    $cache = Join-Path $build 'CMakeCache.txt'
    if ($Configure -or $Clean -or !(Test-Path -LiteralPath $cache)) {
        $config = @('-S', $source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_MAKE_PROGRAM=$($tools.ninja)", '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
            ('-DPULSE_WITH_SELFTEST=' + $(if ($EnableSelfTests) {'ON'} else {'OFF'})),
            ('-DPULSE_BUILD_ARCHITECTURE_FIXTURES=' + $(if ($ArchitectureFixtures) {'ON'} else {'OFF'})))
        & $tools.cmake @config @CMakeArgs
        if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    } else {
        $text = Get-Content -LiteralPath $cache -Raw
        $cachedCmake = [regex]::Match($text, '(?m)^CMAKE_COMMAND:INTERNAL=(.+)$').Groups[1].Value.Trim().Replace('/','\')
        $cachedNinja = [regex]::Match($text, '(?m)^CMAKE_MAKE_PROGRAM:[^=]+=(.+)$').Groups[1].Value.Trim().Replace('/','\')
        $cachedSource = [regex]::Match($text, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$').Groups[1].Value.Trim().Replace('/','\')
        if ($cachedSource -ne $source) { throw 'This build directory belongs to another source tree. Use its SourceDir or a different BuildDir.' }
        if ($cachedCmake -ne $tools.cmake -or $cachedNinja -ne $tools.ninja) { throw 'This cache uses another toolchain. Reconfigure with -Configure, or use -Clean for a damaged cache.' }
    }
    & $tools.cmake --build $build --parallel $Parallel --target @Targets
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
} finally { if ($gate) { $gate.Dispose() } }
