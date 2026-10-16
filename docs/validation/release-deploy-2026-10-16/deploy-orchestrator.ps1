$ErrorActionPreference = 'Stop'
$target = 'D:\Applications\Pulse'
function FindAgent {
    @(Get-CimInstance Win32_Process -Filter "Name='Pulse.Index.exe'" | Where-Object {
        $_.ExecutablePath -eq (Join-Path $target 'Pulse.Index.exe') -and
        $_.CommandLine -eq '"D:\Applications\Pulse\Pulse.Index.exe" --network-agent'
    })
}
$agentWasRunning = (FindAgent).Count -gt 0
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$exitCode = 1
$restoreAllowed = $true
try {
    $arguments = @('-NoProfile', '-File', ('"' + (Join-Path $PSScriptRoot 'deploy-service.ps1') + '"'))
    if ($admin) {
        $child = Start-Process (Join-Path $PSHOME 'pwsh.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
    } else {
        $child = Start-Process (Join-Path $PSHOME 'pwsh.exe') -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
    }
    if (!$child.WaitForExit(60000)) {
        $restoreAllowed = $false
        throw 'Deployment still running; inspect the child process and evidence before retrying'
    }
    $exitCode = $child.ExitCode
} finally {
    if ($restoreAllowed -and $agentWasRunning -and !(FindAgent).Count) {
        $agent = Start-Process (Join-Path $target 'Pulse.Index.exe') -ArgumentList '--network-agent' -WorkingDirectory $target -WindowStyle Hidden -PassThru
        if ($agent.WaitForExit(1000)) { throw "Restored network agent exited: $($agent.ExitCode)" }
    }
    [pscustomobject]@{
        was_running = $agentWasRunning; process_ids = @((FindAgent) | ForEach-Object ProcessId)
        deployment_exit = $exitCode
    } | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'network-agent-restoration.json') -Encoding utf8
}
if ($exitCode -ne 0) { throw "Deployment failed: exit $exitCode; see deployment-service.json" }
Get-Content (Join-Path $PSScriptRoot 'deployment.json')
Get-Content (Join-Path $PSScriptRoot 'deployment-service.json')
