# SPDX-License-Identifier: Apache-2.0
param(
    [string]$BuildDir = 'build-release',
    [string]$BeforeExe = '',
    [string]$OutputDir = 'bench_data/animation-frames',
    [int]$Width = 1100,
    [int]$Height = 820,
    [string]$Label = 'normal',
    [ValidateSet('present','waitable')][string]$Pacing = 'present',
    [switch]$GpuTimestamps,
    [switch]$PresentationStats,
    [ValidateSet(1,2)][int]$FrameLatency = 1,
    [switch]$Topmost,
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
$env:PULSE_FRAME_PACING = $Pacing.ToLowerInvariant()
$env:PULSE_PROFILE_GPU_TIMESTAMPS = if ($GpuTimestamps) { '1' } else { '0' }
$env:PULSE_PROFILE_PRESENT_STATISTICS = if ($PresentationStats) { '1' } else { '0' }
$env:PULSE_FRAME_LATENCY = [string]$FrameLatency
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
$variants = @()
if ($BeforeExe) { $variants += @{name='before';exe=$BeforeExe} }
$variants += @{name='after';exe='pulse_item_reflow_ui_test.exe'}
if ($AfterFirst) { [array]::Reverse($variants) }
$logs = @()
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
                $afterWindow = if ($Topmost) { [IntPtr]::new(-1) } else { [IntPtr]::Zero }
                $positionFlags = if ($Topmost) { 0x10 } else { 0x14 }
                $resized = [PulseFrameProfileWindow]::SetWindowPos($handle, $afterWindow, 30, 30, $Width, $Height, $positionFlags)
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
    if ($Pacing -eq 'waitable' -and $raw -notcontains '[FRAME_PACING] requested_waitable=1 enabled=1') {
        throw "$($variant.name) lacks enabled waitable-pacing evidence; see $log"
    }
    if ($GpuTimestamps -and !($raw -match '^GPU_CSV,[^,]+,[^,]+,[^,]+,[^,]+,1,valid,')) {
        throw "$($variant.name) has no valid measured GPU timestamp intervals; see $log"
    }
    if ($PresentationStats -and !($raw -match '^PRESENT_CSV,[0-9]+,')) {
        throw "$($variant.name) lacks raw presentation-statistics evidence; see $log"
    }
    if ($raw -notcontains '[ICON_READY] native_folder_sizes_16_256=1') {
        throw "$($variant.name) lacks native-icon readiness evidence; rebuild the baseline with the current measurement fixture. See $log"
    }
    [IO.File]::WriteAllLines([IO.Path]::ChangeExtension($log, '.csv'),
        [string[]]@($raw | Where-Object { $_.StartsWith('FRAME_CSV,') } | ForEach-Object { $_.Substring(10) }))
    Write-Output "[MEASURED] $($variant.name) ${Width}x${Height} pacing=$Pacing latency=$FrameLatency topmost=$Topmost GPU=$GpuTimestamps presentation=$PresentationStats : $log"
    $raw | Where-Object { $_.StartsWith('[FRAME_PROFILE]') -or $_.StartsWith('[GPU_PROFILE]') -or $_.StartsWith('[PRESENT_') -or $_.StartsWith('[DISPLAY]') -or $_.StartsWith('[ADAPTER]') }
    $logs += $log
}
$report = Join-Path $output ($Label + '-report.json')
& python (Join-Path $PSScriptRoot 'analyze_animation_frames.py') @logs --output $report
if ($LASTEXITCODE -ne 0) { throw 'Animation measurement analysis failed.' }
Write-Output "[REPORT] $report"
