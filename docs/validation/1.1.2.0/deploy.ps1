$ErrorActionPreference = 'Stop'
$evidenceDirectory = $PSScriptRoot
$workspaceDirectory = 'E:\Projects\pulse'
$targetDirectory = 'D:\Applications\Pulse'
$package = Get-Content -LiteralPath (Join-Path $evidenceDirectory 'package-verification.json') -Raw | ConvertFrom-Json
$manifest = @(Get-Content -LiteralPath (Join-Path $evidenceDirectory 'payload-manifest.json') -Raw | ConvertFrom-Json)
$sourceDirectory = $package.payload_path
$backupDirectory = Join-Path $workspaceDirectory ('build\deployment-backups\Pulse-before-1.1.2.0-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))

function AssertPlainDirectoryPath([string]$path) {
    $cursor = [IO.Path]::GetFullPath($path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Directory path is a file or reparse point: $cursor"
            }
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($parent) { $parent.FullName } else { $null }
    }
}

AssertPlainDirectoryPath $targetDirectory
$targetPrefix = [IO.Path]::GetFullPath($targetDirectory).TrimEnd('\') + '\'
$running = @(Get-Process -Name 'Pulse','Pulse.Shell','Pulse.Index','Pulse.Document','Pulse.Preview' -ErrorAction SilentlyContinue)
if ($running.Count) { throw ('Pulse processes are still running: ' + (($running | ForEach-Object { $_.ProcessName + ':' + $_.Id }) -join ', ')) }
if ((Get-FileHash -LiteralPath $package.archive -Algorithm SHA256).Hash -ne $package.sha256) { throw 'Package hash mismatch' }

$files = @()
foreach ($entry in $manifest) {
    if ([IO.Path]::IsPathRooted($entry.path)) { throw 'Absolute manifest entry' }
    $target = [IO.Path]::GetFullPath((Join-Path $targetDirectory $entry.path))
    if (-not $target.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest entry outside target' }
    AssertPlainDirectoryPath ([IO.Path]::GetDirectoryName($target))
    $source = Join-Path $sourceDirectory $entry.path
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Source hash mismatch: $($entry.path)" }
    $exists = Test-Path -LiteralPath $target
    $oldHash = $null
    if ($exists) {
        $item = Get-Item -LiteralPath $target -Force
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Invalid target file: $target" }
        $handle = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $handle.Dispose()
        $oldHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    }
    $files += [pscustomobject]@{ path = $entry.path; source = $source; target = $target; sha256 = $entry.sha256; existed = $exists; previous_sha256 = $oldHash; backup = (Join-Path $backupDirectory $entry.path) }
}

New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
foreach ($file in $files) {
    if (-not $file.existed) { continue }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.backup)) -Force | Out-Null
    Copy-Item -LiteralPath $file.target -Destination $file.backup
    if ((Get-FileHash -LiteralPath $file.backup -Algorithm SHA256).Hash -ne $file.previous_sha256) { throw "Backup hash mismatch: $($file.path)" }
}
$files | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backupDirectory 'backup-manifest.json') -Encoding utf8

$attempted = [Collections.Generic.List[object]]::new()
try {
    foreach ($file in $files) {
        if ($file.existed -and (Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne $file.previous_sha256) { throw "Target changed since backup: $($file.path)" }
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.target)) -Force | Out-Null
        $attempted.Add($file)
        Copy-Item -LiteralPath $file.source -Destination $file.target -Force
        if ((Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne $file.sha256) { throw "Installed hash mismatch: $($file.path)" }
    }
    $versions = @()
    foreach ($expected in $package.versions) {
        $info = (Get-Item -LiteralPath (Join-Path $targetDirectory $expected.name)).VersionInfo
        if ($info.FileVersion -ne $package.version -or $info.ProductVersion -ne $package.version -or $info.OriginalFilename -ne $expected.name) { throw "Installed version mismatch: $($expected.name)" }
        $versions += [pscustomobject]@{ name = $expected.name; file_version = $info.FileVersion; product_version = $info.ProductVersion }
    }
    foreach ($file in $files) {
        if ((Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne $file.sha256) { throw "Final hash mismatch: $($file.path)" }
    }
    [pscustomobject]@{
        timestamp = (Get-Date).ToString('o')
        version = $package.version
        build_id = $package.build_id
        target = $targetDirectory
        backup = $backupDirectory
        package_sha256 = $package.sha256
        files = $files.Count
        backed_up_files = @($files | Where-Object existed).Count
        hashes_match = $true
        versions = $versions
        started = $false
        service_changes = $false
        user_settings_changed = $false
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceDirectory 'deployment.json') -Encoding utf8
    Write-Output "DEPLOY_OK: $($files.Count) files; version $($package.version)"
    Write-Output "TARGET: $targetDirectory"
    Write-Output "BACKUP: $backupDirectory"
} catch {
    $failure = $_
    $rollbackErrors = @()
    for ($index = $attempted.Count - 1; $index -ge 0; --$index) {
        $file = $attempted[$index]
        try {
            if ($file.existed) {
                Copy-Item -LiteralPath $file.backup -Destination $file.target -Force
                if ((Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash -ne $file.previous_sha256) { throw "Rollback hash mismatch: $($file.path)" }
            } elseif (Test-Path -LiteralPath $file.target) {
                Remove-Item -LiteralPath $file.target -Force
            }
        } catch { $rollbackErrors += $_.ToString() }
    }
    [pscustomobject]@{ error = $failure.ToString(); backup = $backupDirectory; rollback_errors = $rollbackErrors } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceDirectory 'deployment-failure.json') -Encoding utf8
    throw $failure
}
