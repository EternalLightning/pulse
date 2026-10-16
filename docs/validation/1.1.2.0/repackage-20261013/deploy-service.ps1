$ErrorActionPreference = 'Stop'
$expectedPath = '"D:\Applications\Pulse\Pulse.Index.exe" --service'
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
if (!$service -or $service.PathName -ne $expectedPath) { throw 'Service does not match deployment target' }
$wasRunning = $service.State -eq 'Running'
$failure = $null
$restoreFailure = $null
try {
    if ($wasRunning) {
        Stop-Service -Name PulseIndex
        (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(30))
    }
    $processes = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'Pulse%'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith('D:\Applications\Pulse\',[StringComparison]::OrdinalIgnoreCase) })
    foreach ($process in $processes) {
        if ($process.Name -ne 'Pulse.Index.exe' -or $process.CommandLine -ne '"D:\Applications\Pulse\Pulse.Index.exe" --network-agent') { throw "Interactive or unknown target process is running: $($process.Name):$($process.ProcessId)" }
        Stop-Process -Id $process.ProcessId -ErrorAction Stop
    }
    & (Join-Path $PSScriptRoot 'deploy.ps1')
} catch { $failure = $_.ToString() }
finally {
    if ($wasRunning) {
        try {
            if ((Get-Service PulseIndex).Status -ne 'Running') { Start-Service PulseIndex }
            (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,[TimeSpan]::FromSeconds(30))
        } catch { $restoreFailure = $_.ToString() }
    }
    $current = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
    [pscustomobject]@{timestamp=(Get-Date).ToString('o');was_running=$wasRunning;state_after=$current.State;start_mode_before=$service.StartMode;start_mode_after=$current.StartMode;path_before=$service.PathName;path_after=$current.PathName;process_id_after=$current.ProcessId;deployment_error=$failure;restoration_error=$restoreFailure} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment-service.json')
}
if ($failure -or $restoreFailure) { Write-Error "Deployment: $failure; Restoration: $restoreFailure"; exit 1 }
exit 0
