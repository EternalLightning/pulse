$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\..\..')).Path
$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) "pulse-worker-isolation-medium-$PID"
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null
$executables = @('pulse_operation_elevation_transport_test.exe', 'pulse_operation_elevation_adapter_test.exe', 'Pulse.Shell.exe')
$copies = foreach ($name in $executables) {
    $source = Join-Path $workspace "build\$name"
    $target = Join-Path $fixtureRoot $name
    Copy-Item -LiteralPath $source -Destination $target -Force
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($sourceHash -ne $targetHash) { throw "Copy mismatch: $name" }
    [pscustomobject]@{ name = $name; source = $source; target = $target; sha256 = $sourceHash }
}
& icacls.exe (Join-Path $fixtureRoot 'pulse_operation_elevation_transport_test.exe') | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'medium-copy-integrity.log')
& whoami.exe /groups | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'medium-runner-groups.log')
$results = foreach ($name in $executables | Where-Object { $_ -like '*_test.exe' }) {
    $target = Join-Path $fixtureRoot $name
    $log = Join-Path $PSScriptRoot ($name.Replace('.exe', '-medium.log'))
    $errorLog = Join-Path $PSScriptRoot ($name.Replace('.exe', '-medium-errors.log'))
    $process = Start-Process -FilePath $target -WorkingDirectory $workspace -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError $errorLog
    [pscustomobject]@{ name = $name; exit_code = $process.ExitCode; log = $log }
}
[pscustomobject]@{ copied_binaries = $copies; tests = $results; native_uac_fixture_run = $false } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'medium-results.json')
$results | Format-Table -AutoSize
if (@($results | Where-Object { $_.exit_code -ne 0 }).Count) { exit 1 }
