$ErrorActionPreference = 'Stop'
$captureManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'desktop-Startup-manifest.json') -Raw | ConvertFrom-Json
$captureProduct = $captureManifest | Where-Object { $_.name -eq 'Pulse.exe' }
if ((Get-FileHash -LiteralPath $captureProduct.path -Algorithm SHA256).Hash -ne $captureProduct.sha256) {
    throw 'Product hash changed since the desktop startup sample.'
}
$captureRoot = Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../bench_data'))) ('cloud-root-shot-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($captureRoot) | Out-Null
$captureOutput = Join-Path $PSScriptRoot 'cloud-roots-system.png'
$captureStart = [Diagnostics.ProcessStartInfo]::new($captureProduct.path)
$captureStart.Arguments = '--test-instance --shot "' + $captureOutput + '" --shot-language zh-CN --size 1200x900 pulse:home'
$captureStart.UseShellExecute = $false
$captureStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$captureStart.WorkingDirectory = [IO.Path]::GetDirectoryName($captureProduct.path)
$captureStart.Environment['PULSE_TEST_DATA_DIR'] = $captureRoot
$captureStart.Environment['PULSE_TEST_THUMB_CACHE'] = 'off'
$captureStart.Environment['PULSE_TEST_HIDDEN_SHOT'] = '1'
$captureProcess = [Diagnostics.Process]::Start($captureStart)
try {
    if (!$captureProcess.WaitForExit(30000)) {
        $captureProcess.Kill()
        $captureProcess.WaitForExit()
        throw 'Owned screenshot process timed out.'
    }
    if ($captureProcess.ExitCode -ne 0 -or !(Test-Path -LiteralPath $captureOutput)) {
        throw ('Screenshot failed, exit=' + $captureProcess.ExitCode)
    }
    [ordered]@{ executable=$captureProduct.path; sha256=$captureProduct.sha256; fixture=$captureRoot;
        screenshot=$captureOutput; exit_code=$captureProcess.ExitCode } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cloud-roots-system.json') -Encoding UTF8
} finally { $captureProcess.Dispose() }
