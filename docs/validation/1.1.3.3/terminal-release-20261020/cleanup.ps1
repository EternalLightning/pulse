$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$rootPrefix = $repo.TrimEnd('\') + '\'
$dist = Join-Path $repo 'dist'
$package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-verification.json') -Raw | ConvertFrom-Json
$deployment = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Raw | ConvertFrom-Json
$verification = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installed-and-backup-verification.json') -Raw | ConvertFrom-Json
if (!$deployment.hashes_match -or !$verification.all_hashes_match -or
    (Get-FileHash -LiteralPath $package.archive).Hash -ne $package.sha256 -or
    !(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'installed-smoke.json'))) { throw 'Release/deployment gate failed' }
$candidates = @(Get-ChildItem -LiteralPath $dist -Force | Where-Object {
    ($_.PSIsContainer -and $_.Name -match '^portable-stage-[a-f0-9]{32}$' -and $_.FullName -ne $package.stage) -or
    ($_.PSIsContainer -and $_.Name -match '^rollback-(1\.1\.3\.2-|operation-ui-|taskbar-)') -or
    (!$_.PSIsContainer -and $_.Name -match '^Pulse-(\d+\.\d+\.\d+\.\d+)-portable-win-x64\.zip$' -and [version]$Matches[1] -lt [version]$package.version) -or
    (!$_.PSIsContainer -and $_.Name -match '^PulseSetup-(\d+\.\d+\.\d+\.\d+)\.exe$' -and [version]$Matches[1] -lt [version]$package.version)
})
$previousBuild = Join-Path $repo 'build\release-previous-1.1.1.0-16bc3a0cb3b04ea88c8e409efbbdea31'
if (Test-Path -LiteralPath $previousBuild) { $candidates += Get-Item -LiteralPath $previousBuild }
$targets = @()
$inventory = @()
foreach ($candidate in $candidates) {
    $absolute = [IO.Path]::GetFullPath($candidate.FullName)
    if (!$absolute.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase) -or
        $absolute -eq $package.stage -or $absolute -eq $deployment.backup -or $absolute -eq $package.archive) { throw "Unsafe cleanup target: $absolute" }
    if ((Resolve-Path -LiteralPath $absolute).Path -ne $absolute) { throw 'Cleanup path resolution mismatch' }
    $cursor = $absolute
    while ($cursor -ne $repo) {
        $item = Get-Item -LiteralPath $cursor -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse cleanup path: $cursor" }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    $relative = [IO.Path]::GetRelativePath($repo,$absolute).Replace('\','/')
    $tracked = @(& git -C $repo ls-files -- $relative)
    if ($LASTEXITCODE -ne 0 -or $tracked.Count) { throw "Tracked cleanup target: $relative" }
    $children = if ($candidate.PSIsContainer) { @(Get-ChildItem -LiteralPath $absolute -Recurse -Force) } else { @($candidate) }
    if (@($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Cleanup candidate contains a reparse child' }
    if (@($children | Where-Object { !$_.PSIsContainer -and $_.Extension -in @('.cpp','.h','.ps1','.md') -and
        $_.FullName -notmatch '\\licenses\\' -and $_.FullName -notmatch '\\7zip\\README\.md$' }).Count) {
        throw 'Cleanup candidate contains source or review material'
    }
    foreach ($file in $children | Where-Object { !$_.PSIsContainer }) {
        $inventory += [pscustomobject]@{path=$file.FullName;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
    }
    $targets += [pscustomobject]@{path=$absolute;directory=$candidate.PSIsContainer}
}
$record = [pscustomobject]@{targets=$targets;file_count=$inventory.Count;removed_bytes=($inventory|Measure-Object bytes -Sum).Sum;
    retained_current_stage=$package.stage;retained_rollback=$deployment.backup;files=$inventory;completed=$false}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cleanup-inventory.json') -Encoding utf8
foreach ($target in $targets) {
    if ($target.directory) { Remove-Item -LiteralPath $target.path -Recurse -Force }
    else { Remove-Item -LiteralPath $target.path -Force }
    if (Test-Path -LiteralPath $target.path) { throw "Cleanup target remains: $($target.path)" }
}
$record.completed = $true
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cleanup.json') -Encoding utf8
"CLEANUP_OK: $($targets.Count) paths; $($inventory.Count) files; $([math]::Round($record.removed_bytes/1MB,2)) MiB"
