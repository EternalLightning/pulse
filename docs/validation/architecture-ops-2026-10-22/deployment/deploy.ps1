param([ValidateSet('Prepare', 'Apply', 'ServiceStop', 'ServiceStart')][string]$Mode = 'Prepare')
$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$installed = 'D:\Applications\Pulse'
$build = Join-Path $repo 'build'
$planPath = Join-Path $PSScriptRoot 'plan.json'
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function ServiceState {
    Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" |
        Select-Object Name, State, StartMode, PathName, ProcessId
}
function Processes {
    @(Get-CimInstance Win32_Process -Filter "Name='Pulse.exe' OR Name='Pulse.Index.exe' OR Name='Pulse.Shell.exe' OR Name='Pulse.Preview.exe' OR Name='Pulse.Document.exe'" |
        Select-Object ProcessId, Name, ExecutablePath, CommandLine)
}
function AssertNoInteractiveProcesses {
    $busy = @(Processes | Where-Object { $_.Name -ne 'Pulse.Index.exe' })
    if ($busy.Count) { throw 'Pulse UI or operation helpers are resident. Exit the application before replacement.' }
}
function ControlService([string]$Action) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        & $PSCommandPath -Mode $Action
    } else {
        $arguments = @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'), '-Mode', $Action)
        $helper = Start-Process -FilePath 'D:\Applications\PowerShell\7\pwsh.exe' -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        if ($helper.ExitCode -ne 0) { throw "Service helper failed: $Action, exit $($helper.ExitCode)" }
    }
}
if ($Mode -in @('ServiceStop', 'ServiceStart')) {
    try {
        $state = ServiceState
        if (!$state -or $state.PathName -ne '"D:\Applications\Pulse\Pulse.Index.exe" --service') { throw 'Unexpected service configuration' }
        if ($Mode -eq 'ServiceStop') {
            Stop-Service -Name PulseIndex
            (Get-Service PulseIndex).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(25))
        } else {
            Start-Service -Name PulseIndex
            (Get-Service PulseIndex).WaitForStatus('Running', [TimeSpan]::FromSeconds(25))
        }
        ServiceState | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot "$Mode.json") -Encoding utf8
        exit 0
    } catch {
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $PSScriptRoot "$Mode.error.txt") -Encoding utf8
        exit 1
    }
}
if ($Mode -eq 'Prepare') {
    if (Test-Path -LiteralPath $planPath) { throw 'A deployment plan already exists' }
    AssertNoInteractiveProcesses
    & (Join-Path $repo 'scripts/check_release_payload.ps1') -BuildDir $build
    $validated = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../final-build.json') -Raw | ConvertFrom-Json
    foreach ($entry in $validated.Payload | Where-Object Name -In @('Pulse.exe', 'Pulse.Index.exe', 'Pulse.Shell.exe')) {
        if ((Hash (Join-Path $build $entry.Name)) -ne $entry.SHA256) { throw "Validated build has changed: $($entry.Name)" }
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $rollback = Join-Path $repo "dist/rollback-before-architecture-$stamp"
    $stage = Join-Path $repo "bench_data/architecture-deployment-$stamp"
    if ((Test-Path -LiteralPath $rollback) -or (Test-Path -LiteralPath $stage)) { throw 'Deployment destination already exists' }
    [IO.Directory]::CreateDirectory($rollback) | Out-Null
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $reparse = @(Get-ChildItem -LiteralPath $installed -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    if ($reparse.Count) { throw 'Installation contains a reparse point; inspect before copying' }
    $backup = @(foreach ($file in Get-ChildItem -LiteralPath $installed -Recurse -File -Force) {
        $relative = [IO.Path]::GetRelativePath($installed, $file.FullName)
        $destination = Join-Path $rollback $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        $expected = Hash $file.FullName
        [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($file.FullName))
        if ((Hash $destination) -ne $expected -or (Hash $file.FullName) -ne $expected) { throw "Backup verification failed: $relative" }
        [pscustomobject]@{ Name = $relative; SHA256 = $expected }
    })
    $payload = @(foreach ($name in @('Pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe', 'Pulse.Preview.exe', 'Pulse.Shell.exe', 'lumatext.dll', 'pdfium.dll')) {
        $source = Join-Path $build $name
        $destination = Join-Path $stage $name
        [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
        $expected = Hash $source
        if ((Hash $destination) -ne $expected) { throw "Stage verification failed: $name" }
        [pscustomobject]@{ Name = $name; SHA256 = $expected; PreviousSHA256 = Hash (Join-Path $installed $name); Changed = $expected -ne (Hash (Join-Path $installed $name)) }
    })
    [pscustomobject]@{ Installed = $installed; Rollback = $rollback; Stage = $stage; Service = ServiceState; Processes = @(Processes); Backup = $backup; Payload = $payload } |
        ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $planPath -Encoding utf8
    [pscustomobject]@{ Rollback = $rollback; BackupFiles = $backup.Count; Changed = @($payload | Where-Object Changed | ForEach-Object Name) } | ConvertTo-Json
    exit 0
}
$plan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
if ($plan.Installed -ne $installed) { throw 'Unexpected installation target' }
AssertNoInteractiveProcesses
foreach ($entry in $plan.Backup) {
    if ((Hash (Join-Path $installed $entry.Name)) -ne $entry.SHA256 -or
        (Hash (Join-Path $plan.Rollback $entry.Name)) -ne $entry.SHA256) { throw "Installation or rollback changed: $($entry.Name)" }
}
foreach ($entry in $plan.Payload) {
    if ((Hash (Join-Path $plan.Stage $entry.Name)) -ne $entry.SHA256) { throw "Staged payload changed: $($entry.Name)" }
}
$currentService = ServiceState
if ($currentService.PathName -ne $plan.Service.PathName -or $currentService.StartMode -ne $plan.Service.StartMode -or $currentService.State -ne $plan.Service.State) {
    throw 'Service state changed after preparation'
}
$changed = @($plan.Payload | Where-Object Changed)
$indexChanged = @($changed | Where-Object Name -eq 'Pulse.Index.exe').Count -gt 0
$agents = @(Processes | Where-Object { $_.ExecutablePath -eq (Join-Path $installed 'Pulse.Index.exe') -and $_.CommandLine -match '(?:^|\s)--network-agent(?:\s|$)' })
$mutated = @()
$failure = $null
$rolledBack = $false
try {
    if ($indexChanged -and $plan.Service.State -eq 'Running') { ControlService 'ServiceStop' }
    if ($indexChanged) {
        foreach ($agent in $agents) {
            $live = Get-CimInstance Win32_Process -Filter "ProcessId=$($agent.ProcessId)"
            if ($live -and ($live.ExecutablePath -ne $agent.ExecutablePath -or $live.CommandLine -ne $agent.CommandLine)) { throw 'Network agent identity changed' }
            if ($live) { Stop-Process -Id $agent.ProcessId; Wait-Process -Id $agent.ProcessId -Timeout 10 -ErrorAction SilentlyContinue }
        }
        $remaining = @(Processes | Where-Object Name -eq 'Pulse.Index.exe')
        if ($remaining.Count) { throw 'An index process is still resident; replacement refused' }
    }
    AssertNoInteractiveProcesses
    foreach ($entry in $changed) {
        $mutated += $entry
        [IO.File]::WriteAllBytes((Join-Path $installed $entry.Name), [IO.File]::ReadAllBytes((Join-Path $plan.Stage $entry.Name)))
    }
    foreach ($entry in $plan.Payload) {
        if ((Hash (Join-Path $installed $entry.Name)) -ne $entry.SHA256) { throw "Installed payload verification failed: $($entry.Name)" }
    }
    foreach ($entry in $plan.Backup | Where-Object { $_.Name -notin $changed.Name }) {
        if ((Hash (Join-Path $installed $entry.Name)) -ne $entry.SHA256) { throw "Unchanged installed file differs: $($entry.Name)" }
    }
} catch {
    $failure = $_ | Out-String
    foreach ($entry in $mutated) {
        [IO.File]::WriteAllBytes((Join-Path $installed $entry.Name), [IO.File]::ReadAllBytes((Join-Path $plan.Rollback $entry.Name)))
        if ((Hash (Join-Path $installed $entry.Name)) -ne $entry.PreviousSHA256) { throw "Rollback verification failed: $($entry.Name)" }
    }
    $rolledBack = $mutated.Count -gt 0
} finally {
    if ($indexChanged -and $plan.Service.State -eq 'Running' -and (ServiceState).State -ne 'Running') { ControlService 'ServiceStart' }
    if ($indexChanged -and $agents.Count -and !@(Processes | Where-Object { $_.ExecutablePath -eq (Join-Path $installed 'Pulse.Index.exe') -and $_.CommandLine -match '(?:^|\s)--network-agent(?:\s|$)' }).Count) {
        Start-Process -FilePath (Join-Path $installed 'Pulse.Index.exe') -ArgumentList '--network-agent' -WorkingDirectory $installed -WindowStyle Hidden | Out-Null
    }
}
$after = ServiceState
if ($after.PathName -ne $plan.Service.PathName -or $after.StartMode -ne $plan.Service.StartMode -or $after.State -ne $plan.Service.State) { throw 'Service was not restored to its initial state' }
[pscustomobject]@{ Success = !$failure; Failure = $failure; RolledBack = $rolledBack; Changed = @($changed.Name); BackupFiles = $plan.Backup.Count; VerifiedPayloadFiles = $plan.Payload.Count; VerifiedUnchangedFiles = @($plan.Backup | Where-Object { $_.Name -notin $changed.Name }).Count; Rollback = $plan.Rollback; Service = $after; Processes = @(Processes) } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'applied.json') -Encoding utf8
if ($failure) { throw $failure }
Get-Content -LiteralPath (Join-Path $PSScriptRoot 'applied.json') -Raw
