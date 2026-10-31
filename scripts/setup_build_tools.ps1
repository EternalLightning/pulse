# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

#requires -Version 5.1
[CmdletBinding()]
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
# cmd.exe can inherit PowerShell 7's module search path before starting Windows PowerShell.
$env:PSModulePath = (Join-Path $PSHOME 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
$repo = Split-Path -Parent $PSScriptRoot
$root = [IO.Path]::GetFullPath((Join-Path $repo '.build-tools'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Build tools root must not be a reparse point' }
function Remove-ManagedDirectory([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if (!$absolute.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe tool cleanup: $absolute" }
    if (Test-Path -LiteralPath $absolute) {
        if ((Get-Item -LiteralPath $absolute -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing reparse tool cleanup: $absolute" }
        Remove-Item -LiteralPath $absolute -Recurse -Force
    }
}
$gate = $null
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (!$gate) {
        try { $gate = [IO.File]::Open((Join-Path $root 'setup.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
        catch {
            $code = $_.Exception.GetBaseException().HResult -band 65535
            if ($code -notin @(32,33) -or [DateTime]::UtcNow -ge $deadline) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
    $pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build-tools.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $paths = @{}
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($name in @('cmake', 'ninja')) {
        $pin = $pins.$name
        $destination = Join-Path $root "$name-$($pin.version)"
        $relativeExe = if ($name -eq 'cmake') { 'bin\cmake.exe' } else { 'ninja.exe' }
        $exe = Join-Path $destination $relativeExe
        $marker = Join-Path $destination 'package.json'
        $valid = $false
        if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $marker)) {
            try {
                $saved = Get-Content -LiteralPath $marker -Raw -Encoding UTF8 | ConvertFrom-Json
                $valid = $saved.package_sha256 -eq $pin.sha256 -and $saved.exe_sha256 -eq (Get-FileHash -LiteralPath $exe).Hash
                if ($name -eq 'cmake') {
                    $series = ($pin.version.Split('.')[0..1] -join '.')
                    $valid = $valid -and (Test-Path -LiteralPath (Join-Path $destination "share\cmake-$series\Modules\CMakeDetermineSystem.cmake"))
                }
            } catch { $valid = $false }
        }
        if (!$valid) {
            $archive = Join-Path $root "$name-$($pin.version).zip"
            if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $pin.sha256) {
                if (!$Quiet) { Write-Host "Downloading pinned $name $($pin.version)" }
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -UseBasicParsing -Uri $pin.url -OutFile $archive
            }
            if ((Get-FileHash -LiteralPath $archive).Hash -ne $pin.sha256) { throw "$name package checksum mismatch" }
            $stage = Join-Path $root ('.extract-' + [guid]::NewGuid().ToString('N'))
            try {
                [IO.Compression.ZipFile]::ExtractToDirectory($archive, $stage)
                $found = @(Get-ChildItem -LiteralPath $stage -Filter "$name.exe" -File -Recurse)
                if ($found.Count -ne 1) { throw "Unexpected $name package layout" }
                Remove-ManagedDirectory $destination
                if ($name -eq 'cmake') {
                    Move-Item -LiteralPath $found[0].Directory.Parent.FullName -Destination $destination
                } else {
                    New-Item -ItemType Directory -Path $destination | Out-Null
                    Copy-Item -LiteralPath $found[0].FullName -Destination $exe
                }
                @{package_sha256=$pin.sha256; exe_sha256=(Get-FileHash -LiteralPath $exe).Hash} |
                    ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding ascii
            } finally { Remove-ManagedDirectory $stage }
        }
        $version = & $exe --version
        if ($LASTEXITCODE -ne 0 -or ($version -join ' ') -notmatch [regex]::Escape($pin.version)) { throw "Cannot run pinned $name" }
        $paths[$name] = $exe
    }
    $temporary = Join-Path $root 'tmp'
    New-Item -ItemType Directory -Path $temporary -Force | Out-Null
    $batch = @('@echo off', ('set "PATH=' + (Split-Path $paths.cmake) + ';' + (Split-Path $paths.ninja) + ';%PATH%"'),
        ('set "TEMP=' + $temporary + '"'), ('set "TMP=' + $temporary + '"'))
    [IO.File]::WriteAllLines((Join-Path $root 'env.bat'), $batch, (New-Object Text.UTF8Encoding($false)))
    $paths | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'paths.json') -Encoding UTF8
    if (!$Quiet) { Write-Host "Project build tools ready: CMake $($pins.cmake.version), Ninja $($pins.ninja.version)" }
} finally { if ($gate) { $gate.Dispose() } }
