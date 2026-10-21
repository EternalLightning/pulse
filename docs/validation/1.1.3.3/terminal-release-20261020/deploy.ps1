$ErrorActionPreference = 'Stop'
if (![Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Service-preserving deployment requires an administrator process'
}
$targetRoot = 'D:\Applications\Pulse'
$repo = 'E:\Projects\pulse'
$package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-verification.json') -Raw | ConvertFrom-Json
$manifest = @(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'payload-manifest.json') -Raw | ConvertFrom-Json)
if (!$package.zip_hashes_match -or (Get-FileHash -LiteralPath $package.archive).Hash -ne $package.sha256) { throw 'Package gate failed' }
if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'staged-smoke.json'))) { throw 'Staged startup gate missing' }
function AssertPlainDirectory([string]$directory) {
    $cursor = [IO.Path]::GetFullPath($directory)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe directory: $cursor" }
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($parent) { $parent.FullName } else { $null }
    }
}
AssertPlainDirectory $targetRoot
$targetPrefix = $targetRoot.TrimEnd('\') + '\'
$processes = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'Pulse%'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($targetPrefix,[StringComparison]::OrdinalIgnoreCase) })
if (@($processes | Where-Object Name -eq 'Pulse.exe').Count) { throw 'Installed Pulse UI is still running; normal exit must complete before replacement' }
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
if (!$service -or $service.PathName -ne '"D:\Applications\Pulse\Pulse.Index.exe" --service') { throw 'Index service identity differs from expected target' }
$service | Select-Object Name,State,StartMode,PathName,ProcessId | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $PSScriptRoot 'service-before.json') -Encoding utf8
$wasRunning = $service.State -eq 'Running'
$networkAgent = @($processes | Where-Object { $_.Name -eq 'Pulse.Index.exe' -and $_.CommandLine -match '(?:^|\s)--network-agent(?:\s|$)' }).Count -gt 0
$backup = Join-Path $repo ('dist\rollback-before-1.1.3.3-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Test-Path -LiteralPath $backup) { throw 'Backup directory already exists' }
$files = @(foreach ($entry in $manifest) {
    if ([IO.Path]::IsPathRooted($entry.path)) { throw 'Rooted payload path' }
    $target = [IO.Path]::GetFullPath((Join-Path $targetRoot $entry.path))
    if (!$target.StartsWith($targetPrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Payload path escapes installation' }
    AssertPlainDirectory ([IO.Path]::GetDirectoryName($target))
    $source = Join-Path $package.payload_path $entry.path
    if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256) { throw "Source hash mismatch: $($entry.path)" }
    $existed = Test-Path -LiteralPath $target
    $oldHash = $null
    if ($existed) {
        $item = Get-Item -LiteralPath $target -Force
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe target: $target" }
        $oldHash = (Get-FileHash -LiteralPath $target).Hash
    }
    [pscustomobject]@{path=$entry.path;source=$source;target=$target;sha256=$entry.sha256;previous_sha256=$oldHash;
        existed=$existed;changed=(!$existed -or $oldHash -ne $entry.sha256);backup=(Join-Path $backup $entry.path)}
})
$changed = @($files | Where-Object changed)
$attempted = [Collections.Generic.List[object]]::new()
$failure = $null
$restoreFailure = $null
$rollbackErrors = @()
try {
    if ($wasRunning) {
        Stop-Service -Name PulseIndex
        (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(30))
    }
    foreach ($process in $processes) {
        if ($process.Name -notin @('Pulse.Index.exe','Pulse.Shell.exe','Pulse.Preview.exe','Pulse.Document.exe')) { throw 'Unexpected target process' }
        $live = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
        if ($live) { Stop-Process -Id $process.ProcessId -ErrorAction Stop; $live.WaitForExit(10000) | Out-Null }
    }
    New-Item -ItemType Directory -Path $backup | Out-Null
    foreach ($file in $files) {
        if (!$file.existed) { continue }
        if ((Get-FileHash -LiteralPath $file.target).Hash -ne $file.previous_sha256) { throw 'Installation changed since inventory' }
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.backup)) -Force | Out-Null
        Copy-Item -LiteralPath $file.target -Destination $file.backup
        if ((Get-FileHash -LiteralPath $file.backup).Hash -ne $file.previous_sha256) { throw 'Rollback backup hash mismatch' }
    }
    $files | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'backup-manifest.json') -Encoding utf8
    foreach ($file in $changed) {
        $attempted.Add($file)
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.target)) -Force | Out-Null
        Copy-Item -LiteralPath $file.source -Destination $file.target -Force
        if ((Get-FileHash -LiteralPath $file.target).Hash -ne $file.sha256) { throw "Replacement hash mismatch: $($file.path)" }
    }
    foreach ($file in $files) {
        if ((Get-FileHash -LiteralPath $file.target).Hash -ne $file.sha256) { throw "Installed hash mismatch: $($file.path)" }
    }
    & (Join-Path $repo 'scripts\check_release_payload.ps1') -BuildDir $targetRoot
} catch {
    $failure = $_.ToString()
    for ($i=$attempted.Count-1;$i -ge 0;--$i) {
        $file = $attempted[$i]
        try {
            if ($file.existed) {
                Copy-Item -LiteralPath $file.backup -Destination $file.target -Force
                if ((Get-FileHash -LiteralPath $file.target).Hash -ne $file.previous_sha256) { throw 'Rollback hash mismatch' }
            } elseif (Test-Path -LiteralPath $file.target) { Remove-Item -LiteralPath $file.target -Force }
        } catch { $rollbackErrors += $_.ToString() }
    }
} finally {
    if ($wasRunning) {
        try {
            if ((Get-Service PulseIndex).Status -ne 'Running') { Start-Service PulseIndex }
            (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,[TimeSpan]::FromSeconds(30))
        } catch { $restoreFailure = $_.ToString() }
    }
    $after = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
    $record = [pscustomobject]@{version=$package.version;target=$targetRoot;backup=$backup;files=$files.Count;
        changed_files=@($changed.path);hashes_match=(!$failure);network_agent_before=$networkAgent;service_running_before=$wasRunning;
        service_state_after=$after.State;service_start_mode_before=$service.StartMode;service_start_mode_after=$after.StartMode;
        service_path_before=$service.PathName;service_path_after=$after.PathName;failure=$failure;restoration_error=$restoreFailure;rollback_errors=$rollbackErrors}
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding utf8
}
if ($failure -or $restoreFailure) { throw "Deployment failed: $failure; service restoration: $restoreFailure" }
"DEPLOY_OK: $($changed.Count) changed files; $($files.Count) payload hashes verified; $backup"
