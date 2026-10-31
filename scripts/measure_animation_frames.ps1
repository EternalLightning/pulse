# SPDX-License-Identifier: Apache-2.0
param(
    [string]$BuildDir = 'build-release',
    [string]$BeforeExe = 'pulse_animation_profile_before-20261031.exe',
    [string]$OutputDir = 'docs/validation/animation-cache-20261031',
    [int]$Width = 1100,
    [int]$Height = 820,
    [string]$Label = 'normal',
    [switch]$AfterFirst,
    [switch]$PixelCache
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = (Resolve-Path -LiteralPath (Join-Path $repo $BuildDir)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
if (!$output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Measurement output must stay inside the Pulse checkout.'
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$env:PULSE_ANIMATION_PIXEL_CACHE = if ($PixelCache) { '1' } else { '0' }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class PulseFrameProfileWindow {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
    private delegate bool WindowCallback(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder value, int size);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int command);
    [DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    public static IntPtr FindOwnedWindow(uint process) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr hwnd, IntPtr data) {
            uint owner; GetWindowThreadProcessId(hwnd, out owner);
            if (owner != process) return true;
            var name = new StringBuilder(256); GetClassName(hwnd, name, name.Capacity);
            if (!String.Equals(name.ToString(), "Static", StringComparison.OrdinalIgnoreCase)) return true;
            found = hwnd; return false;
        }, IntPtr.Zero);
        return found;
    }
}
'@
$variants = @(@{name='before';exe=$BeforeExe}, @{name='after';exe='pulse_item_reflow_ui_test.exe'})
if ($AfterFirst) { [array]::Reverse($variants) }
foreach ($variant in $variants) {
    $executable = (Resolve-Path -LiteralPath (Join-Path $build $variant.exe)).Path
    $log = Join-Path $output ($Label + '-' + $variant.name + '.log')
    $errorLog = Join-Path $output ($Label + '-' + $variant.name + '.stderr.log')
    $process = Start-Process -FilePath $executable -ArgumentList @('unused','--frame-profile-presented') `
        -WorkingDirectory $repo -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errorLog -PassThru
    $processHandle = $process.Handle
    $resized = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    while (!$process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        $handle = [PulseFrameProfileWindow]::FindOwnedWindow($process.Id)
        [uint32]$owner = 0
        if ($handle -ne [IntPtr]::Zero) {
            [void][PulseFrameProfileWindow]::GetWindowThreadProcessId($handle, [ref]$owner)
            if ($owner -eq $process.Id) {
                $resized = [PulseFrameProfileWindow]::SetWindowPos($handle, [IntPtr]::Zero, 30, 30, $Width, $Height, 0x14)
                [void][PulseFrameProfileWindow]::ShowWindowAsync($handle, 4)
                break
            }
        }
        Start-Sleep -Milliseconds 10
    }
    $process.WaitForExit()
    [uint32]$exitCode = 0
    if (![PulseFrameProfileWindow]::GetExitCodeProcess($processHandle, [ref]$exitCode)) { throw 'Cannot read fixture exit status.' }
    if (!$resized -or $exitCode -ne 0) {
        throw "$($variant.name) measurement failed: resized=$resized exit=$exitCode; see $log"
    }
    $raw = [IO.File]::ReadAllLines($log)
    if ($raw -notcontains '[ICON_READY] native_folder_sizes_16_256=1') {
        throw "$($variant.name) lacks native-icon readiness evidence; rebuild the baseline with the current measurement fixture. See $log"
    }
    [IO.File]::WriteAllLines([IO.Path]::ChangeExtension($log, '.csv'),
        [string[]]@($raw | Where-Object { $_.StartsWith('FRAME_CSV,') } | ForEach-Object { $_.Substring(10) }))
    Write-Output "[MEASURED] $($variant.name) ${Width}x${Height}: $log"
    $raw | Where-Object { $_.StartsWith('[FRAME_PROFILE]') -or $_.StartsWith('[DISPLAY]') -or $_.StartsWith('[ADAPTER]') }
}
