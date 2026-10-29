#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$updateInstall = 'D:\Applications\Pulse'
$updateWorkspace = 'E:\Projects\pulse'
$updatePayload = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'payload-dir.txt') -Raw).Trim()
$updateResult = [ordered]@{ success=$false; install_dir=$updateInstall; service_before=$null;
    service_after=$null; stopped_network_agents=@(); replacement=$null; error=$null }
$updateServiceStopped = $false
Start-Transcript -LiteralPath (Join-Path $PSScriptRoot 'installed-update-transcript.log') | Out-Null
try {
    $updateUi = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.exe'" |
        Where-Object { !$_.ExecutablePath -or $_.ExecutablePath -eq (Join-Path $updateInstall 'Pulse.exe') })
    if ($updateUi.Count) { throw 'Installed Pulse UI is still resident; no lifecycle changes made.' }
    $updateService = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
    if (!$updateService -or $updateService.PathName -ne ('"' + (Join-Path $updateInstall 'Pulse.Index.exe') + '" --service')) {
        throw 'Index service does not match the verified installation.'
    }
    $updateResult.service_before = $updateService | Select-Object State, StartMode, PathName
    if ($updateService.State -ne 'Running') { throw 'Service state changed since the reviewed plan.' }
    Stop-Service -Name PulseIndex -ErrorAction Stop
    $updateServiceStopped = $true
    (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
    $updateAgents = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.Index.exe'" |
        Where-Object { $_.ExecutablePath -eq (Join-Path $updateInstall 'Pulse.Index.exe') -and
            $_.CommandLine -eq ('"' + (Join-Path $updateInstall 'Pulse.Index.exe') + '" --network-agent') })
    foreach ($updateAgent in $updateAgents) {
        $updateProcess = Get-Process -Id $updateAgent.ProcessId -ErrorAction Stop
        if ($updateProcess.Path -ne (Join-Path $updateInstall 'Pulse.Index.exe')) { throw 'Agent identity changed.' }
        Stop-Process -InputObject $updateProcess -ErrorAction Stop
        if (!$updateProcess.WaitForExit(5000)) { throw 'Selected network agent did not stop.' }
        $updateResult.stopped_network_agents += $updateAgent.ProcessId
    }
    & (Join-Path $updateWorkspace 'scripts\replace_installed.ps1') -PayloadDir $updatePayload -InstallDir $updateInstall -Apply |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'installed-apply.json') -Encoding utf8
    $updateResult.replacement = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installed-apply.json') -Raw | ConvertFrom-Json
    if (!$updateResult.replacement.Success) { throw 'Maintained replacement script did not report success.' }
} catch {
    $updateResult.error = $_.ToString()
} finally {
    if ($updateServiceStopped) {
        try {
            Start-Service -Name PulseIndex -ErrorAction Stop
            (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(30))
        } catch { $updateResult.error = ($updateResult.error + '; service restore: ' + $_.ToString()) }
    }
    try {
        $updateResult.service_after = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" |
            Select-Object State, StartMode, PathName
        if ($updateResult.service_before -and
            ($updateResult.service_before | ConvertTo-Json -Compress) -ne ($updateResult.service_after | ConvertTo-Json -Compress)) {
            $updateResult.error = ($updateResult.error + '; service state was not restored exactly')
        }
    } catch { $updateResult.error = ($updateResult.error + '; service check: ' + $_.ToString()) }
    $updateResult.success = $updateResult.replacement -and $updateResult.replacement.Success -and !$updateResult.error
    $updateResult | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'installed-lifecycle-result.json') -Encoding utf8
    Stop-Transcript | Out-Null
}
if (!$updateResult.success) { exit 1 }
