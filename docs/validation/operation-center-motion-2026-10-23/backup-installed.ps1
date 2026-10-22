$ErrorActionPreference = 'Stop'
$root = 'D:\Applications\Pulse'
$processes = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Pulse*' } | Select-Object ProcessId,Name,ExecutablePath,CommandLine)
if (@($processes | Where-Object { $_.Name -ne 'Pulse.Index.exe' }).Count) { throw 'Pulse UI or helper is still running' }
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" | Select-Object Name,State,StartMode,PathName,ProcessId
[pscustomobject]@{Service=$service;Processes=$processes;Time=(Get-Date).ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$PSScriptRoot\before-replacement.json" -Encoding utf8
$backup = Join-Path 'E:\Projects\pulse\dist' ('rollback-before-1.2.0.0-motion-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path $backup | Out-Null
$files = @(Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($root, $_.FullName)
    $destination = [IO.Path]::GetFullPath((Join-Path $backup $relative))
    if (-not $destination.StartsWith($backup + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe backup path' }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) { throw "Backup mismatch: $relative" }
    [pscustomobject]@{Path=$relative;SHA256=$hash;Length=$_.Length}
})
[pscustomobject]@{Root=$root;Backup=$backup;Files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$PSScriptRoot\rollback.json" -Encoding utf8
$preferences = @(Get-Content -LiteralPath "$PSScriptRoot\..\context-menu-refinements-2026-10-23\preferences-before.json" -Raw | ConvertFrom-Json | ForEach-Object {
    [pscustomobject]@{Path=$_.Path;SHA256=(Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash}
})
ConvertTo-Json -InputObject $preferences -Depth 4 | Set-Content -LiteralPath "$PSScriptRoot\preferences-before.json" -Encoding utf8
[pscustomobject]@{Backup=$backup;VerifiedFiles=$files.Count;Preferences=$preferences.Count;Service=$service;Processes=$processes} | ConvertTo-Json -Depth 5
