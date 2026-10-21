param([Parameter(Mandatory=$true)][string]$BinaryDirectory,[string]$Label='staged')
$ErrorActionPreference = 'Stop'
$fixture = Join-Path $PSScriptRoot 'smoke-fixture'
New-Item -ItemType Directory -Path (Join-Path $fixture 'files') -Force | Out-Null
'Pulse release smoke fixture' | Set-Content -LiteralPath (Join-Path $fixture 'files\example.txt')
'中文文件名' | Set-Content -LiteralPath (Join-Path $fixture 'files\中文文件名.txt') -Encoding utf8
$previousData = $env:PULSE_TEST_DATA_DIR
$previousHidden = $env:PULSE_TEST_HIDDEN_SHOT
try {
    $env:PULSE_TEST_DATA_DIR = Join-Path $fixture "$Label-profile"
    $env:PULSE_TEST_HIDDEN_SHOT = '1'
    $image = Join-Path $PSScriptRoot "$Label-smoke.png"
    $arguments = @('--test-instance','--shot',('"'+$image+'"'),'--size','1200x800','--light',('"'+(Join-Path $fixture 'files')+'"'))
    $process = Start-Process -FilePath (Join-Path $BinaryDirectory 'Pulse.exe') -ArgumentList $arguments -WorkingDirectory $BinaryDirectory -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(30000)) { $process.Kill(); $process.WaitForExit(); throw 'Isolated smoke timed out' }
    if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $image)) { throw "Smoke failed: $($process.ExitCode)" }
    [pscustomobject]@{binary_directory=$BinaryDirectory;exit_code=$process.ExitCode;image=$image;isolated_profile=$env:PULSE_TEST_DATA_DIR} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot "$Label-smoke.json") -Encoding utf8
    "SMOKE_OK: $Label; exit=0"
} finally { $env:PULSE_TEST_DATA_DIR=$previousData; $env:PULSE_TEST_HIDDEN_SHOT=$previousHidden }
