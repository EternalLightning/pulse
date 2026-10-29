param(
    [Parameter(Mandatory)][string]$ExecutablePath,
    [Parameter(Mandatory)][string]$Label
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$output = Join-Path $PSScriptRoot ($Label + '.png')
$fixture = Join-Path $repo 'bench_data/quality-release-1.2.0.1/installed-smoke-fixture'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $ExecutablePath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$start.WorkingDirectory = $repo
$start.Arguments = '--test-instance --shot "' + $output + '" --size 1100x720 --light --view details "' + $fixture + '"'
$start.EnvironmentVariables['PULSE_TEST_HIDDEN_SHOT'] = '1'
$process = [Diagnostics.Process]::Start($start)
$finished = $process.WaitForExit(30000)
$result = [ordered]@{
    ExecutablePath = $ExecutablePath
    SHA256 = (Get-FileHash -LiteralPath $ExecutablePath -Algorithm SHA256).Hash
    Version = [Diagnostics.FileVersionInfo]::GetVersionInfo($ExecutablePath).FileVersion
    Finished = $finished
    ExitCode = if ($finished) { $process.ExitCode } else { $null }
    ScreenshotExists = (Test-Path -LiteralPath $output)
    Screenshot = $output
    Utc = [DateTime]::UtcNow.ToString('o')
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot ($Label + '-result.json')) -Encoding utf8
$process.Dispose()
if (!$finished -or $result.ExitCode -ne 0 -or !$result.ScreenshotExists) { throw 'Production startup verification failed' }
$result | ConvertTo-Json -Compress
