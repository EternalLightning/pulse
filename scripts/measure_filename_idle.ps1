# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$ProcessId,
    [ValidateRange(1, 3600)]
    [int]$Seconds = 300,
    [ValidateRange(0.01, 100)]
    [double]$MaximumSingleCorePercent = 1.0
)

$ErrorActionPreference = 'Stop'
# Read-only observation of an existing process. This script never starts,
# stops, installs, or configures an index service and writes no preferences.
$observedProcess = [System.Diagnostics.Process]::GetProcessById($ProcessId)
$observedName = $observedProcess.ProcessName
$observedStarted = $observedProcess.StartTime.ToUniversalTime()
$cpuBefore = $observedProcess.TotalProcessorTime.TotalMilliseconds
$clock = [System.Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    $remainingMs = [Math]::Max(1, ($Seconds * 1000) - $clock.Elapsed.TotalMilliseconds)
    Start-Sleep -Milliseconds ([int][Math]::Min(15000, $remainingMs))
    $observedProcess.Refresh()
    if ($observedProcess.HasExited) { throw "Observed process $ProcessId exited before the sample completed." }
}
$cpuMs = $observedProcess.TotalProcessorTime.TotalMilliseconds - $cpuBefore
$elapsedMs = $clock.Elapsed.TotalMilliseconds
$singleCorePercent = 100.0 * $cpuMs / $elapsedMs
[pscustomobject]@{
    pid = $ProcessId
    process = $observedName
    process_started_utc = $observedStarted.ToString('o')
    observed_seconds = [Math]::Round($elapsedMs / 1000.0, 3)
    process_cpu_ms = [Math]::Round($cpuMs, 3)
    single_core_cpu_percent = [Math]::Round($singleCorePercent, 6)
    maximum_single_core_percent = $MaximumSingleCorePercent
    passed = $singleCorePercent -lt $MaximumSingleCorePercent
} | ConvertTo-Json
$observedProcess.Dispose()
if ($singleCorePercent -ge $MaximumSingleCorePercent) { exit 1 }
