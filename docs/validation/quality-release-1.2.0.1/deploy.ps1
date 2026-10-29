param([Parameter(Mandatory=$true)][string]$PlanPath)
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
$phase = 'preflight'; $errorText = $null; $recoveryError = $null
$serviceStopped = $false; $backupVerified = $false
$changed = [Collections.Generic.List[string]]::new()
$retired = [Collections.Generic.List[object]]::new()
$temps = [Collections.Generic.List[string]]::new()
function Hash($path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
function Save($name, $value) {
    [IO.File]::WriteAllText((Join-Path $plan.EvidenceDirectory $name), ($value | ConvertTo-Json -Depth 12), $utf8)
}
function Checked($path, $root) {
    $full = [IO.Path]::GetFullPath($path)
    if (!$full.StartsWith([IO.Path]::GetFullPath($root).TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside declared root' }
    $item = Get-Item -LiteralPath $path -Force
    while ($null -ne $item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse path refused' }
        $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
    $full
}
function Inventory($root) {
    $queue = [Collections.Generic.Queue[string]]::new(); $queue.Enqueue($root)
    while ($queue.Count) {
        foreach ($file in Get-ChildItem -LiteralPath $queue.Dequeue() -Force) {
            $null = Checked $file.FullName $root
            if ($file.PSIsContainer) { $queue.Enqueue($file.FullName) }
            else { @{Relative=$file.FullName.Substring($root.Length+1);Bytes=$file.Length;SHA256=(Hash $file.FullName)} }
        }
    }
}
function Processes($name) {
    @(Get-CimInstance Win32_Process -Filter "Name='$name'" | Select-Object ProcessId,CreationDate,ExecutablePath,CommandLine)
}
function Restore-Service {
    if (!$script:serviceStopped) { return }
    $mode = switch ($originalService.StartMode) { 'Auto' {'Automatic'} 'Manual' {'Manual'} 'Disabled' {'Disabled'} default {throw 'Unknown original service mode'} }
    Set-Service PulseIndex -StartupType $mode
    if ($originalService.State -eq 'Running') {
        Start-Service PulseIndex
        (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,[TimeSpan]::FromSeconds(40))
    }
    $script:serviceStopped = $false
}
function Replace($target, $candidate, $expectedOld) {
    $null = Checked $target $plan.InstallDirectory
    $null = Checked $candidate $plan.InstallDirectory
    $old = $target + '.quality-old-' + [Guid]::NewGuid().ToString('N')
    if (![QualityRelease.Native]::ReplaceFile($target,$candidate,$old,0,[IntPtr]::Zero,[IntPtr]::Zero)) {
        throw "Atomic program replacement failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $retired.Add(@{Path=$old;SHA256=$expectedOld})
}
try {
    $plan = Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrative Windows token required' }
    if (!$plan.ValidationComplete) { throw 'Completed targeted validation required' }
    foreach ($file in $plan.Files) {
        $null = Checked (Join-Path $plan.BuildDirectory $file.Relative) $plan.BuildDirectory
        if ((Hash (Join-Path $plan.BuildDirectory $file.Relative)) -ne $file.SHA256) { throw 'Build changed after validation' }
    }
    if ((Hash $plan.GracefulExitTool) -ne $plan.GracefulExitHash) { throw 'Normal exit tool hash changed' }
    if (Test-Path -LiteralPath $plan.BackupDirectory) { throw 'Backup destination already exists' }
    $originalService = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" | Select-Object State,StartMode,PathName,ProcessId
    if ($originalService.PathName -ne ('"'+$plan.InstallDirectory+'\Pulse.Index.exe" --service')) { throw 'Unexpected service registration' }
    Save 'service-before.json' $originalService
    Save 'ui-before.json' @(Processes 'Pulse.exe')
    $networkBefore = @(Processes 'Pulse.Index.exe')
    Save 'network-before.json' @($networkBefore)
    $indexAcl = (Get-Acl -LiteralPath $plan.IndexDirectory).Sddl
    $phase = 'full-payload-backup'
    $inventory = @(Inventory $plan.InstallDirectory)
    if ($inventory.Count -lt 40) { throw 'Incomplete installed inventory' }
    New-Item -ItemType Directory -Path $plan.BackupDirectory | Out-Null
    foreach ($entry in $inventory) {
        $copy = Join-Path $plan.BackupDirectory $entry.Relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $copy) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $plan.InstallDirectory $entry.Relative) -Destination $copy
        if ((Hash $copy) -ne $entry.SHA256) { throw 'Full rollback hash mismatch' }
    }
    Save 'payload-before.json' @($inventory)
    $backupVerified = $true
    $phase = 'normal-ui-exit'
    foreach ($process in @(Processes 'Pulse.exe' | Where-Object {$_.ExecutablePath -eq (Join-Path $plan.InstallDirectory 'Pulse.exe')})) {
        & $plan.GracefulExitTool ([string]$process.ProcessId) > (Join-Path $plan.EvidenceDirectory ('normal-exit-'+$process.ProcessId+'.log')) 2>&1
        if ($LASTEXITCODE -ne 0) { throw 'Normal tray exit failed; no force termination' }
    }
    if (@(Processes 'Pulse.exe' | Where-Object {$_.ExecutablePath -eq (Join-Path $plan.InstallDirectory 'Pulse.exe')}).Count) { throw 'Installed UI remains resident' }
    # Give this installation's user agent time to finish its idle shutdown.
    # A still-resident image may be retired atomically; it is never force-killed.
    foreach ($agent in @($networkBefore | Where-Object {
        $_.ExecutablePath -eq (Join-Path $plan.InstallDirectory 'Pulse.Index.exe') -and $_.CommandLine -match '\s--network-agent(?:\s|$)'
    })) {
        $live = Get-Process -Id $agent.ProcessId -ErrorAction SilentlyContinue
        if ($live) { $null = $live.WaitForExit(20000); $live.Dispose() }
    }
    $phase = 'stop-index-service'
    Set-Service PulseIndex -StartupType Disabled
    $serviceStopped = $true
    Stop-Service PulseIndex
    (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(45))
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace QualityRelease { public static class Native {
    [DllImport("kernel32.dll", EntryPoint="ReplaceFileW", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ReplaceFile(string target,string candidate,string backup,uint flags,IntPtr exclude,IntPtr reserved);
} }
'@
    $phase = 'replace-changed-payload'
    foreach ($file in $plan.Files) {
        $target = Join-Path $plan.InstallDirectory $file.Relative
        $oldHash = Hash $target
        if ($oldHash -eq $file.SHA256) { continue }
        $candidate = $target + '.quality-new-' + [Guid]::NewGuid().ToString('N')
        $temps.Add($candidate)
        Copy-Item -LiteralPath (Join-Path $plan.BuildDirectory $file.Relative) -Destination $candidate
        if ((Hash $candidate) -ne $file.SHA256) { throw 'Staged payload hash mismatch' }
        Replace $target $candidate $oldHash
        $changed.Add($file.Relative)
    }
    $phase = 'restore-and-verify'
    Restore-Service
    foreach ($entry in $inventory) {
        $new = @($plan.Files | Where-Object {$_.Relative -eq $entry.Relative})
        $expected = if ($new.Count) {$new[0].SHA256} else {$entry.SHA256}
        if ((Hash (Join-Path $plan.InstallDirectory $entry.Relative)) -ne $expected) { throw 'Final full installation hash mismatch' }
    }
    if ((Get-Acl -LiteralPath $plan.IndexDirectory).Sddl -ne $indexAcl) { throw 'Index directory ACL changed' }
    $after = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" | Select-Object State,StartMode,PathName,ProcessId
    Save 'service-after.json' $after
    if ($after.State -ne $originalService.State -or $after.StartMode -ne $originalService.StartMode -or $after.PathName -ne $originalService.PathName) { throw 'Original service state not restored' }
    Save 'network-after-maintenance.json' @(Processes 'Pulse.Index.exe')
    $phase = 'complete'
} catch {
    $errorText = $_.Exception.Message
    Save 'deployment-error.json' @{Phase=$phase;Error=$errorText;Detail=($_|Out-String);Stack=$_.ScriptStackTrace}
    try {
        if ($backupVerified -and $changed.Count) {
            Set-Service PulseIndex -StartupType Disabled; $serviceStopped = $true
            Stop-Service PulseIndex
            (Get-Service PulseIndex).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(45))
            foreach ($relative in $changed) {
                $target = Join-Path $plan.InstallDirectory $relative
                $candidate = $target + '.quality-restore-' + [Guid]::NewGuid().ToString('N')
                $temps.Add($candidate)
                Copy-Item -LiteralPath (Join-Path $plan.BackupDirectory $relative) -Destination $candidate
                Replace $target $candidate (Hash $target)
                if ((Hash $target) -ne (Hash (Join-Path $plan.BackupDirectory $relative))) { throw 'Rollback hash mismatch' }
            }
        }
    } catch { $recoveryError = $_.Exception.Message }
} finally {
    try { Restore-Service } catch { $recoveryError = $_.Exception.Message }
    $deferred = @(
        foreach ($entry in $retired) {
            try {
                $null = Checked $entry.Path $plan.InstallDirectory
                if ((Hash $entry.Path) -ne $entry.SHA256) { throw 'Retired image hash mismatch' }
                Remove-Item -LiteralPath $entry.Path
            } catch { @{Path=$entry.Path;SHA256=$entry.SHA256;Reason=$_.Exception.Message} }
        }
    )
    foreach ($path in $temps) {
        if (Test-Path -LiteralPath $path) { $null = Checked $path $plan.InstallDirectory; Remove-Item -LiteralPath $path }
    }
    Save 'deployment-result.json' @{Success=($phase -eq 'complete' -and !$errorText -and !$recoveryError);Phase=$phase;Error=$errorText;RecoveryError=$recoveryError;BackupDirectory=$plan.BackupDirectory;BackupVerified=$backupVerified;BackupFiles=$inventory.Count;ChangedFiles=@($changed.ToArray());Deferred=$deferred;Utc=[DateTime]::UtcNow.ToString('o')}
}
if ($errorText -or $recoveryError) { exit 1 }
exit 0
