$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath('E:\Projects\pulse')
$prefix = $repo.TrimEnd('\') + '\'
$deployment = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Raw | ConvertFrom-Json
$service = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment-service.json') -Raw | ConvertFrom-Json
if (!$deployment.hashes_match -or $service.deployment_error -or $service.restoration_error -or $service.state_after -ne 'Running') { throw 'Deployment/service gate failed' }
if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'deployed-smoke.json'))) { throw 'Deployment startup gate missing' }
$paths = @(
    'dist\portable-stage-0aee892703a446d79fac1511a761c370',
    'dist\portable-stage-7fa4d4e672404d998f1b0d1e0a4ef151',
    'dist\Pulse-1.1.1.0-portable-win-x64.zip',
    'dist\Pulse-1.1.1.0-antivirus-update-20261012.zip',
    'build\deployment-backups\Pulse-archive-focus-20261012-223528-61e7d9bd32e7469d888e4269354ec572',
    'build\deployment-backups\Pulse-before-1.1.2.0-20261012-200643-c625ac22865149d4877a198d6873510f',
    'build\deployment-backups\Pulse-copy-callback-20261012-212810-83510acd05504367b4f8d228b409d5e8',
    'build\deployment-backups\Pulse-feedback-20261012-210035-4a83cb6aee8a4f97bd21e1498e3a4206'
)
$targets = @()
$files = @()
foreach ($relative in $paths) {
    $absolute = [IO.Path]::GetFullPath((Join-Path $repo $relative))
    if (!$absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or $absolute -eq [IO.Path]::GetFullPath($deployment.backup)) { throw "Unsafe cleanup target: $absolute" }
    if (!(Test-Path -LiteralPath $absolute)) { continue }
    $resolved = (Resolve-Path -LiteralPath $absolute).Path
    if (![String]::Equals($absolute,$resolved,[StringComparison]::OrdinalIgnoreCase)) { throw 'Resolved cleanup path mismatch' }
    $cursor = Get-Item -LiteralPath $absolute -Force
    while ($cursor -and $cursor.FullName -ne $repo) {
        if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path: $($cursor.FullName)" }
        $cursor = Get-Item -LiteralPath ([IO.Path]::GetDirectoryName($cursor.FullName)) -Force
    }
    $tracked = @(& git -C $repo ls-files -- $relative.Replace('\','/'))
    if ($LASTEXITCODE -or $tracked.Count) { throw "Tracked or unverified cleanup target: $relative" }
    $item = Get-Item -LiteralPath $absolute -Force
    $children = if ($item.PSIsContainer) { @(Get-ChildItem -LiteralPath $absolute -Force -Recurse) } else { @($item) }
    if (@($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw "Reparse child: $absolute" }
    foreach ($file in $children | Where-Object { !$_.PSIsContainer }) {
        $files += [pscustomobject]@{path=$file.FullName;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
    }
    $targets += [pscustomobject]@{path=$absolute;directory=$item.PSIsContainer}
}
$manifest = [pscustomobject]@{timestamp=(Get-Date).ToString('o');targets=$targets;file_count=$files.Count;removed_bytes=($files|Measure-Object bytes -Sum).Sum;files=$files;retained_rollback=$deployment.backup;reason='Superseded portable archives/stages and obsolete deployment backups; retained current package, current staging payload, latest full rollback, verification evidence and active build cache'}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cleanup-inventory.json')
foreach ($target in $targets) {
    if ($target.directory) { Remove-Item -LiteralPath $target.path -Recurse -Force } else { Remove-Item -LiteralPath $target.path -Force }
    if (Test-Path -LiteralPath $target.path) { throw "Cleanup target remains: $($target.path)" }
}
$manifest | Add-Member -NotePropertyName completed -NotePropertyValue $true
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cleanup.json')
"CLEANUP_OK: $($targets.Count) paths; $($files.Count) files; $([math]::Round($manifest.removed_bytes/1MB,2)) MiB"
