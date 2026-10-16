$ErrorActionPreference = 'Stop'
$target = 'D:\Applications\Pulse'
$agentWasRunning = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.Index.exe'" | Where-Object { $_.ExecutablePath -eq (Join-Path $target 'Pulse.Index.exe') -and $_.CommandLine -eq '"D:\Applications\Pulse\Pulse.Index.exe" --network-agent' }).Count -gt 0
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$exitCode = 1
try {
    if ($admin) {
        $process = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -ArgumentList @('-NoProfile','-File',('"'+(Join-Path $PSScriptRoot 'deploy-service.ps1')+'"')) -WindowStyle Hidden -PassThru
    } else {
        $process = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -ArgumentList @('-NoProfile','-File',('"'+(Join-Path $PSScriptRoot 'deploy-service.ps1')+'"')) -Verb RunAs -WindowStyle Hidden -PassThru
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
} finally {
    $agent = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.Index.exe'" | Where-Object { $_.ExecutablePath -eq (Join-Path $target 'Pulse.Index.exe') -and $_.CommandLine -eq '"D:\Applications\Pulse\Pulse.Index.exe" --network-agent' })
    if ($agentWasRunning -and !$agent.Count) {
        $agent = @(Start-Process -FilePath (Join-Path $target 'Pulse.Index.exe') -ArgumentList '--network-agent' -WorkingDirectory $target -WindowStyle Hidden -PassThru)
        Start-Sleep -Seconds 1
        if ($agent[0].HasExited) { throw 'Restored network agent exited unexpectedly' }
    }
    [pscustomobject]@{was_running=$agentWasRunning;process_ids=@($agent | ForEach-Object { if ($_.Id) { $_.Id } else { $_.ProcessId } });deployment_exit=$exitCode} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'network-agent-restoration.json')
}
if ($exitCode) { throw "Elevated deployment exited with $exitCode; inspect deployment-service.json" }
Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment-service.json')
Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deployment.json')
