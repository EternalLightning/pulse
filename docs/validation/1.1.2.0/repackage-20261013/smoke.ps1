param([string]$BinaryDirectory, [string]$Label = 'staged')
$ErrorActionPreference = 'Stop'
$fixture = Join-Path 'E:\Projects\pulse\bench_data' 'release-smoke-20261013'
New-Item -ItemType Directory -Path (Join-Path $fixture 'files') -Force | Out-Null
'Pulse packaging smoke fixture' | Set-Content -LiteralPath (Join-Path $fixture 'files\example.txt')
'中文文件名' | Set-Content -LiteralPath (Join-Path $fixture 'files\中文文件名.txt') -Encoding utf8
$previousData = $env:PULSE_TEST_DATA_DIR
$previousHidden = $env:PULSE_TEST_HIDDEN_SHOT
try {
    $env:PULSE_TEST_DATA_DIR = Join-Path $fixture "$Label-profile"
    $env:PULSE_TEST_HIDDEN_SHOT = '1'
    $image = Join-Path $PSScriptRoot "$Label-smoke.png"
    $args = @('--test-instance','--shot',('"'+$image+'"'),'--size','1000x700','--light',('"'+(Join-Path $fixture 'files')+'"'))
    $process = Start-Process -FilePath (Join-Path $BinaryDirectory 'Pulse.exe') -ArgumentList $args -WorkingDirectory $BinaryDirectory -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(30000)) { $process.Kill(); $process.WaitForExit(); throw 'Startup smoke exceeded 30 seconds' }
    if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $image)) { throw "Startup smoke failed: exit=$($process.ExitCode)" }
    [pscustomobject]@{timestamp=(Get-Date).ToString('o');binary_directory=$BinaryDirectory;exit_code=$process.ExitCode;image=$image;isolated_profile=$env:PULSE_TEST_DATA_DIR} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot "$Label-smoke.json")
    "SMOKE_OK: $Label; exit=$($process.ExitCode); $image"
} finally { $env:PULSE_TEST_DATA_DIR=$previousData; $env:PULSE_TEST_HIDDEN_SHOT=$previousHidden }
