param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Output,
    [int]$SampleSeconds = 8,
    [int]$WarmupSeconds = 7
)
$ErrorActionPreference = 'Stop'
$sampleExecutable = (Resolve-Path -LiteralPath $Executable).Path
$sampleParent = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../bench_data'))
$sampleRoot = Join-Path $sampleParent ('idle-sample-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($sampleRoot) | Out-Null
$sampleStart = [Diagnostics.ProcessStartInfo]::new($sampleExecutable, '--test-instance')
$sampleStart.UseShellExecute = $false
$sampleStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$sampleStart.WorkingDirectory = [IO.Path]::GetDirectoryName($sampleExecutable)
$sampleStart.Environment['PULSE_TEST_DATA_DIR'] = $sampleRoot
$sampleStart.Environment['PULSE_TEST_THUMB_CACHE'] = 'off'
$sampleWatch = [Diagnostics.Stopwatch]::StartNew()
$sampleProcess = [Diagnostics.Process]::Start($sampleStart)
try {
    $sampleIdle = $sampleProcess.WaitForInputIdle(15000)
    $sampleInputIdleMs = $sampleWatch.ElapsedMilliseconds
    if ($sampleProcess.HasExited) { throw ('Process exited: ' + $sampleProcess.ExitCode) }
    Start-Sleep -Seconds $WarmupSeconds
    $sampleProcess.Refresh()
    $sampleCpuBefore = $sampleProcess.TotalProcessorTime.TotalMilliseconds
    $samplePrivateBefore = $sampleProcess.PrivateMemorySize64
    $sampleThreadsBefore = @{}
    foreach ($sampleThread in $sampleProcess.Threads) {
        try { $sampleThreadsBefore[$sampleThread.Id] = $sampleThread.TotalProcessorTime.TotalMilliseconds } catch {}
    }
    $sampleCpuClock = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $SampleSeconds
    $sampleProcess.Refresh()
    $sampleElapsed = $sampleCpuClock.Elapsed.TotalMilliseconds
    $sampleCpuMs = $sampleProcess.TotalProcessorTime.TotalMilliseconds - $sampleCpuBefore
    $sampleThreads = @(foreach ($sampleThread in $sampleProcess.Threads) {
        try {
            if ($sampleThreadsBefore.ContainsKey($sampleThread.Id)) {
                [ordered]@{ id=$sampleThread.Id; cpu_ms=($sampleThread.TotalProcessorTime.TotalMilliseconds - $sampleThreadsBefore[$sampleThread.Id]) }
            }
        } catch {}
    })
    $sampleResult = [ordered]@{
        executable = $sampleExecutable
        fixture = $sampleRoot
        process_id = $sampleProcess.Id
        input_idle = $sampleIdle
        input_idle_ms = $sampleInputIdleMs
        sample_wall_ms = $sampleElapsed
        cpu_ms = $sampleCpuMs
        cpu_one_core_percent = 100.0 * $sampleCpuMs / $sampleElapsed
        private_bytes = $samplePrivateBefore
        warmup_seconds = $WarmupSeconds
        main_window_handle = $sampleProcess.MainWindowHandle.ToInt64()
        threads = $sampleThreads
        window_style_requested = 'Hidden'
        scope = 'Isolated UI process only; actual visibility is not independently verified; input-idle is not first-present; child services excluded.'
    }
} finally {
    $sampleCloseSent = $false
    $sampleForcedExit = $false
    if (!$sampleProcess.HasExited) {
        $sampleCloseSent = $sampleProcess.CloseMainWindow()
        if (!$sampleProcess.WaitForExit(5000)) {
            $sampleForcedExit = $true
            $sampleProcess.Kill()
            $sampleProcess.WaitForExit()
        }
    }
    if ($sampleResult) {
        $sampleResult.close_main_window_sent = $sampleCloseSent
        $sampleResult.forced_exit = $sampleForcedExit
        $sampleResult.exit_code = $sampleProcess.ExitCode
        $sampleResult | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
    }
    $sampleProcess.Dispose()
}
