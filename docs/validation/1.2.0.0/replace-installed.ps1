$ErrorActionPreference = 'Stop'
$audit = $PSScriptRoot
$package = Get-Content -LiteralPath "$audit\package-audit.json" -Raw | ConvertFrom-Json
$baseline = Get-Content -LiteralPath "$audit\before.json" -Raw | ConvertFrom-Json
$rollback = Get-Content -LiteralPath "$audit\rollback.json" -Raw | ConvertFrom-Json
$root = [IO.Path]::GetFullPath('D:\Applications\Pulse')
if ($rollback.Root -ne $root -or $package.Version -ne '1.2.0.0') { throw 'Unexpected deployment manifest' }
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator token required for service replacement' }
function InstalledPath([string]$relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe installed path: $relative" }
    return $path
}
function ServiceState {
    Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
}
function WaitService([string]$state) {
    (Get-Service -Name PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::$state, [TimeSpan]::FromSeconds(30))
}
function RestoreService {
    $current = ServiceState
    if ($current.PathName -ne $baseline.Service.PathName -or $current.StartMode -ne $baseline.Service.StartMode) {
        throw 'Service configuration changed unexpectedly'
    }
    if ($baseline.Service.State -eq 'Running' -and $current.State -ne 'Running') { Start-Service -Name PulseIndex; WaitService 'Running' }
    elseif ($baseline.Service.State -eq 'Stopped' -and $current.State -ne 'Stopped') { Stop-Service -Name PulseIndex; WaitService 'Stopped' }
}
$changed = @()
$written = [Collections.Generic.List[string]]::new()
$pending = [Collections.Generic.List[string]]::new()
$result = [ordered]@{Version=$package.Version;Installed=$root;Backup=$rollback.Backup;Succeeded=$false;Changed=@();VerifiedPayloadFiles=0;Error=$null;RolledBack=$false}
try {
    $service = ServiceState
    if ($service.State -ne $baseline.Service.State -or $service.PathName -ne $baseline.Service.PathName -or $service.StartMode -ne $baseline.Service.StartMode) {
        throw 'Service state changed since preflight'
    }
    $processes = @(Get-CimInstance Win32_Process | Where-Object Name -Like 'Pulse*')
    if (@($processes | Where-Object ProcessId -NE $service.ProcessId).Count) { throw 'Pulse UI or another Pulse host is running; refusing replacement' }
    foreach ($file in $rollback.Files) {
        if ((Get-FileHash -LiteralPath (Join-Path $rollback.Backup $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256 -or
            (Get-FileHash -LiteralPath (InstalledPath $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) {
            throw "Installed files or rollback changed: $($file.Path)"
        }
    }
    foreach ($file in $package.Files) {
        if ((Get-FileHash -LiteralPath (Join-Path $package.Payload $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Staged file changed: $($file.Path)" }
        $destination = InstalledPath $file.Path
        if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $file.SHA256) { $changed += $file }
    }
    if (@($changed | Where-Object Path -EQ 'Pulse.Index.exe').Count -and $service.State -eq 'Running') {
        Stop-Service -Name PulseIndex
        WaitService 'Stopped'
    }
    foreach ($file in $changed) {
        $destination = InstalledPath $file.Path
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        $temporary = $destination + '.update-' + [guid]::NewGuid().ToString('N') + '.tmp'
        $pending.Add($temporary)
        Copy-Item -LiteralPath (Join-Path $package.Payload $file.Path) -Destination $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Copy hash mismatch: $($file.Path)" }
        $written.Add($file.Path)
        Move-Item -LiteralPath $temporary -Destination $destination -Force
    }
    foreach ($file in $package.Files) {
        if ((Get-FileHash -LiteralPath (InstalledPath $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Installed hash mismatch: $($file.Path)" }
    }
    RestoreService
    $result.Succeeded = $true
    $result.Changed = @($changed.Path)
    $result.VerifiedPayloadFiles = $package.Files.Count
} catch {
    $result.Error = $_.Exception.Message
    if ($written.Count) {
        try {
            if ((ServiceState).State -ne 'Stopped') { Stop-Service -Name PulseIndex; WaitService 'Stopped' }
            foreach ($relative in $written) {
                $destination = InstalledPath $relative
                $old = @($rollback.Files | Where-Object Path -EQ $relative)
                if ($old.Count) {
                    Copy-Item -LiteralPath (Join-Path $rollback.Backup $relative) -Destination $destination -Force
                    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $old[0].SHA256) { throw "Rollback mismatch: $relative" }
                } elseif (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
            }
            $result.RolledBack = $true
        } catch { $result.Error += '; rollback: ' + $_.Exception.Message }
    }
} finally {
    try { RestoreService } catch { $result.Error += '; service: ' + $_.Exception.Message; $result.Succeeded = $false }
    foreach ($temporary in $pending) { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
    $service = ServiceState
    $result.Service = [ordered]@{State=$service.State;StartMode=$service.StartMode;PathName=$service.PathName;ProcessId=$service.ProcessId}
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$audit\deployment-result.json" -Encoding utf8
}
if (-not $result.Succeeded) { throw $result.Error }
