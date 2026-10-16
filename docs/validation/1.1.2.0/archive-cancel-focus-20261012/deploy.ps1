$ErrorActionPreference = 'Stop'
$evidenceDirectory = $PSScriptRoot
$targetDirectory = 'D:\Applications\Pulse'
$source = 'E:\Projects\pulse\build\Pulse.exe'
$target = Join-Path $targetDirectory 'Pulse.exe'
$previous = Get-Content -LiteralPath 'E:\Projects\pulse\docs\validation\1.1.2.0\copy-callback-20261012\deployment.json' -Raw | ConvertFrom-Json
$manifest = @(Get-Content -LiteralPath 'E:\Projects\pulse\docs\validation\1.1.2.0\payload-manifest.json' -Raw | ConvertFrom-Json)
if (@(Get-Process -Name 'Pulse','Pulse.Shell' -ErrorAction SilentlyContinue).Count) { throw 'Pulse is still running' }
$cursor = $targetDirectory
while ($cursor) {
    $directory = Get-Item -LiteralPath $cursor -Force
    if (-not $directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Invalid directory: $cursor" }
    $parent = [IO.Directory]::GetParent($cursor)
    $cursor = if ($parent) { $parent.FullName } else { $null }
}
$targetPrefix = [IO.Path]::GetFullPath($targetDirectory).TrimEnd('\') + '\'
foreach ($entry in $manifest) {
    $file = [IO.Path]::GetFullPath((Join-Path $targetDirectory $entry.path))
    if ([IO.Path]::IsPathRooted($entry.path) -or -not $file.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path' }
    $expected = $entry.sha256
    $update = @($previous.updates | Where-Object name -eq $entry.path)
    if ($update.Count) { $expected = $update[0].sha256 }
    if ((Get-FileHash -LiteralPath $file).Hash -ne $expected) { throw "Installed file changed: $file" }
}
foreach ($file in @($source, $target)) {
    $item = Get-Item -LiteralPath $file -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Invalid executable: $file" }
    if ($item.VersionInfo.FileVersion -ne '1.1.2.0') { throw "Invalid version: $file" }
}
$serviceBefore = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
$sourceHash = (Get-FileHash -LiteralPath $source).Hash
$oldHash = (Get-FileHash -LiteralPath $target).Hash
$handle = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$handle.Dispose()
$backupDirectory = 'E:\Projects\pulse\build\deployment-backups\Pulse-archive-focus-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $backupDirectory | Out-Null
$backup = Join-Path $backupDirectory 'Pulse.exe'
Copy-Item -LiteralPath $target -Destination $backup
if ((Get-FileHash -LiteralPath $backup).Hash -ne $oldHash) { throw 'Backup hash mismatch' }
if ((Get-FileHash -LiteralPath $target).Hash -ne $oldHash) { throw 'Target changed before copy' }
try {
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $sourceHash) { throw 'Installed hash mismatch' }
} catch {
    Copy-Item -LiteralPath $backup -Destination $target -Force
    throw
}
foreach ($entry in $manifest) {
    $expected = $entry.sha256
    $update = @($previous.updates | Where-Object name -eq $entry.path)
    if ($update.Count) { $expected = $update[0].sha256 }
    if ($entry.path -eq 'Pulse.exe') { $expected = $sourceHash }
    if ((Get-FileHash -LiteralPath (Join-Path $targetDirectory $entry.path)).Hash -ne $expected) { throw "Final hash mismatch: $($entry.path)" }
}
$serviceAfter = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
if ($serviceBefore.State -ne $serviceAfter.State -or $serviceBefore.StartMode -ne $serviceAfter.StartMode) { throw 'Service state changed during deployment' }
[pscustomobject]@{
    timestamp = (Get-Date).ToString('o'); target = $target; version = '1.1.2.0'
    sha256 = $sourceHash; old_sha256 = $oldHash; backup = $backup
    payload_files_verified = $manifest.Count; service_state = $serviceAfter.State
    service_modified = $false; main_started = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceDirectory 'deployment.json') -Encoding utf8
Write-Output "DEPLOYED_MAIN; PAYLOAD_HASHES_OK=$($manifest.Count); INDEX_SERVICE=$($serviceAfter.State)"
