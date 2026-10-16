$ErrorActionPreference = 'Stop'
$target = 'D:\Applications\Pulse'
$package = Get-Content (Join-Path $PSScriptRoot 'package-verification.json') -Raw | ConvertFrom-Json
$plan = @(Get-Content (Join-Path $PSScriptRoot 'deployment-plan.json') -Raw | ConvertFrom-Json)
$backup = Join-Path $target ('backups\before-audit-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
if (!$service -or $service.PathName -ne '"D:\Applications\Pulse\Pulse.Index.exe" --service') {
    throw 'Service does not match the authorized installation'
}
$wasRunning = $service.State -eq 'Running'
$changed = @($plan | Where-Object changed)
$attempted = [Collections.Generic.List[object]]::new()
$failure = $null
$restorationFailure = $null
$rollbackErrors = @()

function AssertPlainPath([string]$path, [bool]$directory) {
    $cursor = [IO.Path]::GetFullPath($path)
    if (Test-Path -LiteralPath $cursor) {
        $item = Get-Item -LiteralPath $cursor -Force
        if ($item.PSIsContainer -ne $directory) { throw "Wrong object type: $cursor" }
    }
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Reparse point in deployment path: $cursor"
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($parent) { $parent.FullName } else { $null }
    }
}

try {
    AssertPlainPath $target $true
    AssertPlainPath $backup $true
    if ((Get-FileHash -LiteralPath $package.archive).Hash -ne $package.sha256) { throw 'Package changed after verification' }
    foreach ($entry in $plan) {
        if ([IO.Path]::IsPathRooted($entry.path) -or $entry.path -match '(^|[\\/])\.\.([\\/]|$)') { throw 'Invalid relative payload path' }
        $source = Join-Path $package.payload_path $entry.path
        $destination = Join-Path $target $entry.path
        AssertPlainPath $source $false
        AssertPlainPath $destination $false
        if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256) { throw "Source changed: $($entry.path)" }
        $current = if (Test-Path -LiteralPath $destination) { (Get-FileHash -LiteralPath $destination).Hash } else { $null }
        if ($current -ne $entry.previous_sha256) { throw "Installed file changed since planning: $($entry.path)" }
    }
    if ($wasRunning) {
        Stop-Service PulseIndex
        (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
    }
    $processes = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)
    })
    foreach ($running in $processes) {
        if ($running.ExecutablePath -eq (Join-Path $target 'Pulse.exe')) {
            $main = Get-Process -Id $running.ProcessId -ErrorAction Stop
            [void]$main.CloseMainWindow()
            if (!$main.WaitForExit(10000)) { throw 'Pulse did not close gracefully; no files replaced' }
        } elseif ($running.ExecutablePath -eq (Join-Path $target 'Pulse.Index.exe') -and
            $running.CommandLine -eq '"D:\Applications\Pulse\Pulse.Index.exe" --network-agent') {
            Stop-Process -Id $running.ProcessId -ErrorAction Stop
            $agent = Get-Process -Id $running.ProcessId -ErrorAction SilentlyContinue
            if ($agent -and !$agent.WaitForExit(10000)) { throw 'Network agent did not stop' }
        } else { throw "Unexpected installed process: $($running.Name):$($running.ProcessId)" }
    }
    New-Item -ItemType Directory -Path $backup -ErrorAction Stop | Out-Null
    foreach ($entry in $changed) {
        $destination = Join-Path $target $entry.path
        if ($entry.previous_sha256) {
            $lock = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $lock.Dispose()
            $backupFile = Join-Path $backup $entry.path
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backupFile)) -Force | Out-Null
            Copy-Item -LiteralPath $destination -Destination $backupFile
            if ((Get-FileHash -LiteralPath $backupFile).Hash -ne $entry.previous_sha256) { throw 'Backup hash mismatch' }
        }
    }
    $changed | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $backup 'backup-manifest.json') -Encoding utf8
    foreach ($entry in $changed) {
        $destination = Join-Path $target $entry.path
        $current = if (Test-Path -LiteralPath $destination) { (Get-FileHash -LiteralPath $destination).Hash } else { $null }
        if ($current -ne $entry.previous_sha256) { throw "Concurrent installation change: $($entry.path)" }
        $source = Join-Path $package.payload_path $entry.path
        if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256) { throw 'Concurrent source change' }
        $attempted.Add($entry)
        Copy-Item -LiteralPath $source -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $destination).Hash -ne $entry.sha256) { throw 'Installed hash mismatch' }
    }
    foreach ($entry in $plan) {
        if ((Get-FileHash -LiteralPath (Join-Path $target $entry.path)).Hash -ne $entry.sha256) { throw 'Final payload mismatch' }
    }
    [pscustomobject]@{
        timestamp = (Get-Date).ToString('o'); target = $target; backup = $backup
        version = $package.version; build_id = $package.build_id; package_sha256 = $package.sha256
        payload_files = $plan.Count; replaced_files = $changed.Count; unchanged_files = $plan.Count - $changed.Count
        all_payload_hashes_match = $true
    } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PSScriptRoot 'deployment.json') -Encoding utf8
} catch {
    $failure = $_.ToString()
    for ($i = $attempted.Count - 1; $i -ge 0; --$i) {
        $entry = $attempted[$i]
        try {
            $destination = Join-Path $target $entry.path
            if ($entry.previous_sha256) {
                Copy-Item -LiteralPath (Join-Path $backup $entry.path) -Destination $destination -Force
                if ((Get-FileHash -LiteralPath $destination).Hash -ne $entry.previous_sha256) { throw 'Rollback hash mismatch' }
            } else { Remove-Item -LiteralPath $destination -Force }
        } catch { $rollbackErrors += $_.ToString() }
    }
} finally {
    if ($wasRunning) {
        try {
            if ((Get-Service PulseIndex).Status -ne 'Running') { Start-Service PulseIndex }
            (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(30))
        } catch { $restorationFailure = $_.ToString() }
    }
    $after = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
    [pscustomobject]@{
        timestamp = (Get-Date).ToString('o'); was_running = $wasRunning; state_after = $after.State
        start_mode_before = $service.StartMode; start_mode_after = $after.StartMode
        path_before = $service.PathName; path_after = $after.PathName; process_id_after = $after.ProcessId
        deployment_error = $failure; restoration_error = $restorationFailure; rollback_errors = $rollbackErrors
    } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PSScriptRoot 'deployment-service.json') -Encoding utf8
}
if ($failure -or $restorationFailure) { Write-Error "Deployment: $failure; Restoration: $restorationFailure"; exit 1 }
exit 0
