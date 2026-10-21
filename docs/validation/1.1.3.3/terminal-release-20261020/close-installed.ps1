$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class PulseUpdateWindows {
    public delegate bool Callback(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder value, int count);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
}
'@
$running = @(Get-CimInstance Win32_Process -Filter "Name='Pulse.exe'" | Where-Object ExecutablePath -eq 'D:\Applications\Pulse\Pulse.exe')
$ids = @($running.ProcessId)
$windows = [Collections.Generic.List[object]]::new()
$callback = [PulseUpdateWindows+Callback]{ param($hwnd,$data)
    [uint32]$ownerId = 0
    [PulseUpdateWindows]::GetWindowThreadProcessId($hwnd,[ref]$ownerId) | Out-Null
    if ($ids -contains $ownerId) {
        $name = [Text.StringBuilder]::new(256)
        [PulseUpdateWindows]::GetClassName($hwnd,$name,256) | Out-Null
        if ($name.ToString() -eq 'PulseMainWindow') { $windows.Add([pscustomobject]@{hwnd=$hwnd;pid=$ownerId}) }
    }
    return $true
}
[PulseUpdateWindows]::EnumWindows($callback,[IntPtr]::Zero) | Out-Null
foreach ($window in $windows) {
    if (![PulseUpdateWindows]::PostMessage($window.hwnd,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)) { throw 'Cannot request normal Pulse window close' }
}
foreach ($item in $running) {
    $process = Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue
    if ($process -and !$process.WaitForExit(30000)) {
        throw 'Pulse has not completed graceful exit; no process was forcibly terminated'
    }
}
[pscustomobject]@{normal_close_requested=$true;windows=$windows.Count;processes=$ids;completed=$true} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'close-installed.json') -Encoding utf8
"CLOSE_OK: $($windows.Count) windows; all target UI processes exited normally"
