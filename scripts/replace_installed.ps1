# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PayloadDir,
    [Parameter(Mandatory)][string]$InstallDir,
    [string]$BackupRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist/installed-backups'),
    [switch]$Apply
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Hash([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
    return $null
}
function Get-RuntimeState {
    $service = @(Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" |
        Select-Object State, StartMode, PathName)
    $agents = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.Index.exe'" |
        Where-Object { $_.ExecutablePath -eq (Join-Path $install 'Pulse.Index.exe') } |
        Select-Object ProcessId, CommandLine)
    return (@{ Service = $service; Agents = $agents } | ConvertTo-Json -Depth 5 -Compress)
}
function Assert-NoReparse([string]$Path) {
    $node = Get-Item -LiteralPath $Path
    while ($null -ne $node) {
        if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path refused: $($node.FullName)" }
        $node = $node.Parent
    }
}

$source = (Resolve-Path -LiteralPath $PayloadDir).Path
$install = (Resolve-Path -LiteralPath $InstallDir).Path
Assert-NoReparse $source
Assert-NoReparse $install
$backupBase = [IO.Path]::GetFullPath($BackupRoot)
foreach ($pair in @(@($source, $install), @($source, $backupBase), @($install, $backupBase))) {
    if ($pair[0] -eq $pair[1] -or $pair[0].StartsWith($pair[1].TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $pair[1].StartsWith($pair[0].TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Payload, installation and backup directories must not overlap.'
    }
}
$roots = @('Pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe', 'Pulse.Preview.exe',
    'Pulse.Shell.exe', 'lumatext.dll', 'pdfium.dll', 'LICENSE', 'NOTICE', 'README.txt', 'Pulse.Integration.exe',
    'THIRD_PARTY_NOTICES.md')
foreach ($required in $roots[0..6]) {
    if (!(Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) { throw "Missing payload: $required" }
}
& "$PSScriptRoot/check_release_payload.ps1" -BuildDir $source | Out-Null
$files = @(foreach ($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
    Assert-NoReparse $file.DirectoryName
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked payload file: $($file.FullName)" }
    $relative = [IO.Path]::GetRelativePath($source, $file.FullName)
    if ($relative -notin $roots -and $relative -notmatch '^(7zip|licenses)[\\/]') {
        throw "Unexpected payload file (use a clean portable staging directory): $relative"
    }
    $destination = Join-Path $install $relative
    $ancestor = Split-Path $destination -Parent
    while (!(Test-Path -LiteralPath $ancestor)) { $ancestor = Split-Path $ancestor -Parent }
    Assert-NoReparse $ancestor
    if (Test-Path -LiteralPath $destination) {
        if ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked destination: $relative" }
    }
    $old = Get-Hash $destination
    $new = Get-Hash $file.FullName
    [pscustomobject]@{ Relative = $relative; Before = $old; After = $new; Changed = $old -ne $new }
})
# Retire the exact legacy filename as part of the same backed-up transaction.
$legacyIntegration = Join-Path $install 'pulse_integration.exe'
if (Test-Path -LiteralPath $legacyIntegration) {
    $legacyFile = Get-Item -LiteralPath $legacyIntegration
    if ($legacyFile.PSIsContainer -or ($legacyFile.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Legacy integration helper must be a regular file.'
    }
    $files += [pscustomobject]@{ Relative = 'pulse_integration.exe'; Before = (Get-Hash $legacyIntegration); After = $null; Changed = $true }
}
$changed = @($files | Where-Object Changed)
$state = Get-RuntimeState
$plan = [ordered]@{ InstallDir = $install; PayloadDir = $source; Files = $files; RuntimeBefore = $state }
if (!$Apply) {
    [pscustomobject]@{ Mode = 'Plan'; Files = $files.Count; Changed = $changed.Count; Runtime = ($state | ConvertFrom-Json); Changes = @($changed | ForEach-Object { $_.Relative }) } | ConvertTo-Json -Depth 6
    return
}
if (!$changed.Count) { Write-Output '{"Success":true,"Changed":0}'; return }
$residentUi = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.exe'" |
    Where-Object { $_.ExecutablePath -eq (Join-Path $install 'Pulse.exe') -or !$_.ExecutablePath })
if ($residentUi.Count) { throw 'Pulse UI remains resident (or its path cannot be inspected). Ask the user to exit before retrying.' }
# Check every changed destination before creating backups or writing installed files.
foreach ($file in $changed) {
    $path = Join-Path $install $file.Relative
    if ($null -ne $file.Before) {
        $handle = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
        $handle.Dispose()
    }
}
$backup = Join-Path $backupBase ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
Assert-NoReparse $backup
$plan | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8
foreach ($file in $files | Where-Object { $null -ne $_.Before }) {
    $path = Join-Path $backup ('payload/' + $file.Relative)
    New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $install $file.Relative) -Destination $path
    if ((Get-Hash $path) -ne $file.Before) { throw "Backup mismatch: $($file.Relative)" }
}
$written = [Collections.Generic.List[object]]::new()
try {
    foreach ($file in $changed) {
        $destination = Join-Path $install $file.Relative
        if ((Get-Hash $destination) -ne $file.Before) { throw "Destination changed: $($file.Relative)" }
        if ($null -ne $file.After -and (Get-Hash (Join-Path $source $file.Relative)) -ne $file.After) { throw "Payload changed: $($file.Relative)" }
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        $written.Add($file)
        if ($null -eq $file.After) { Remove-Item -LiteralPath $destination }
        else { Copy-Item -LiteralPath (Join-Path $source $file.Relative) -Destination $destination -Force }
    }
    foreach ($file in $files) {
        if ((Get-Hash (Join-Path $install $file.Relative)) -ne $file.After) { throw "Installed hash mismatch: $($file.Relative)" }
    }
    if ((Get-RuntimeState) -ne $state) { throw 'Service or index-agent state changed during replacement.' }
    @{ Success = $true; Changed = $changed.Count; Verified = $files.Count; Backup = $backup; RuntimePreserved = $true } |
        ConvertTo-Json | Tee-Object -FilePath (Join-Path $backup 'result.json')
} catch {
    $failure = $_
    $errors = @()
    foreach ($file in @($written.ToArray()) | Sort-Object Relative -Descending) {
        try {
            $destination = Join-Path $install $file.Relative
            if ($null -eq $file.Before) { Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue }
            else { Copy-Item -LiteralPath (Join-Path $backup ('payload/' + $file.Relative)) -Destination $destination -Force }
            if ((Get-Hash $destination) -ne $file.Before) { throw 'Rollback hash mismatch' }
        } catch { $errors += "$($file.Relative): $_" }
    }
    @{ Success = $false; Error = "$failure"; RollbackErrors = $errors; Backup = $backup } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'result.json')
    throw "Replacement failed: $failure; rollback errors: $($errors -join '; '); backup: $backup"
}
