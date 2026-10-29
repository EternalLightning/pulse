$ErrorActionPreference = 'Stop'
$plan = Get-Content (Join-Path $PSScriptRoot 'deployment-plan.json') -Raw | ConvertFrom-Json
$deployment = Get-Content (Join-Path $PSScriptRoot 'deployment-result.json') -Raw | ConvertFrom-Json
if (!$deployment.Success -or !$deployment.BackupVerified) { throw 'Verified deployment required' }
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ReleaseIntegrity {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int type, IntPtr buffer, int size, out int required);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    public static int Rid(int id) {
        IntPtr process = OpenProcess(0x1000, false, id), token = IntPtr.Zero, buffer = IntPtr.Zero;
        if (process == IntPtr.Zero) throw new Win32Exception();
        try {
            if (!OpenProcessToken(process, 8, out token)) throw new Win32Exception();
            int length;
            GetTokenInformation(token, 25, IntPtr.Zero, 0, out length);
            buffer = Marshal.AllocHGlobal(length);
            if (!GetTokenInformation(token, 25, buffer, length, out length)) throw new Win32Exception();
            IntPtr sid = Marshal.ReadIntPtr(buffer);
            uint count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            return Marshal.ReadInt32(GetSidSubAuthority(sid, count - 1));
        } finally {
            if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer);
            if (token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process);
        }
    }
}
'@
if ([ReleaseIntegrity]::Rid($PID) -ne 8192) { throw 'Ordinary Medium launcher required' }
$files = @(foreach ($file in $plan.Files) {
    $path = Join-Path $plan.InstallDirectory $file.Relative
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion
    $match = $hash -eq $file.SHA256
    if (!$match -or ($file.Relative.EndsWith('.exe') -and $version -ne $plan.Version)) { throw 'Installed payload identity mismatch' }
    @{Relative=$file.Relative;SHA256=$hash;Version=$version;Match=$match}
})
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" | Select-Object State,StartMode,ProcessId,PathName
$before = Get-Content (Join-Path $PSScriptRoot 'service-before.json') -Raw | ConvertFrom-Json
if ($service.State -ne $before.State -or $service.StartMode -ne $before.StartMode -or $service.PathName -ne $before.PathName) { throw 'Service state mismatch' }
$exe = Join-Path $plan.InstallDirectory 'Pulse.exe'
$ui = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.exe'" | Where-Object {$_.ExecutablePath -eq $exe})
if (!$ui.Count) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exe
    $start.WorkingDirectory = $plan.InstallDirectory
    $start.UseShellExecute = $false
    $process = [Diagnostics.Process]::Start($start)
    $null = $process.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1500
    $process.Refresh()
    if ($process.HasExited) { throw 'Restored UI exited unexpectedly' }
    $process.Dispose()
    $ui = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.exe'" | Where-Object {$_.ExecutablePath -eq $exe})
}
if (!$ui.Count) { throw 'Installed UI missing' }
$uiResult = @(foreach ($item in $ui) {
    $rid = [ReleaseIntegrity]::Rid($item.ProcessId)
    if ($rid -ne 8192) { throw 'Installed UI is not an ordinary Medium process' }
    @{ProcessId=$item.ProcessId;CreationDate=$item.CreationDate;ExecutablePath=$item.ExecutablePath;IntegrityRid=$rid}
})
$agents = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.Index.exe'" | Where-Object {$_.ExecutablePath -eq (Join-Path $plan.InstallDirectory 'Pulse.Index.exe')} | Select-Object ProcessId,CreationDate,ExecutablePath,CommandLine)
if (!@($agents | Where-Object {$_.CommandLine -match '\s--network-agent(?:\s|$)'}).Count) { throw 'Network agent missing' }
$result = @{Success=$true;Version=$plan.Version;Files=$files;Service=$service;UI=$uiResult;IndexHosts=$agents;Utc=[DateTime]::UtcNow.ToString('o')}
$result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $PSScriptRoot 'final-installed-verification.json') -Encoding utf8
@{Success=$true;Version=$plan.Version;OrdinaryMediumLaunch=$true;UI=$uiResult;Utc=$result.Utc} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PSScriptRoot 'user-ui-restored.json') -Encoding utf8
@{Success=$true;Version=$plan.Version;FileCount=$files.Count;UI=$uiResult;Service=$service} | ConvertTo-Json -Depth 4
