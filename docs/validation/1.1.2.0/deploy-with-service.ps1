$ErrorActionPreference = 'Stop'
$resultPath = Join-Path $PSScriptRoot 'deployment-service.json'
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
if (-not $service -or $service.PathName -ne '"D:\Applications\Pulse\Pulse.Index.exe" --service') {
    throw 'PulseIndex service does not match the requested target directory'
}
$wasRunning = $service.State -eq 'Running'
$failure = $null
$restorationFailure = $null
try {
    if ($wasRunning) {
        Stop-Service -Name PulseIndex
        (Get-Service -Name PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
    }
    & (Join-Path $PSScriptRoot 'deploy.ps1')
} catch {
    $failure = $_.ToString()
} finally {
    if ($wasRunning) {
        try {
            Start-Service -Name PulseIndex
            (Get-Service -Name PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(30))
        } catch { $restorationFailure = $_.ToString() }
    }
    $current = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
    [pscustomobject]@{
        timestamp = (Get-Date).ToString('o')
        was_running = $wasRunning
        state_after = $current.State
        start_mode_before = $service.StartMode
        start_mode_after = $current.StartMode
        path_before = $service.PathName
        path_after = $current.PathName
        process_id_after = $current.ProcessId
        deployment_error = $failure
        restoration_error = $restorationFailure
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultPath -Encoding utf8
    if (-not $failure -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'deployment.json'))) {
        $deployment = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Raw | ConvertFrom-Json
        $deployment.service_changes = $wasRunning
        $deployment | Add-Member -NotePropertyName service_configuration_changed -NotePropertyValue ($service.StartMode -ne $current.StartMode -or $service.PathName -ne $current.PathName)
        $deployment | Add-Member -NotePropertyName service_state_after -NotePropertyValue $current.State
        $deployment | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json') -Encoding utf8
    }
}
if ($failure -or $restorationFailure) { exit 1 }
exit 0
