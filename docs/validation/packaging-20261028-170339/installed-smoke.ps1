#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$smokeIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$smokePrincipal = [Security.Principal.WindowsPrincipal]::new($smokeIdentity)
if ($smokePrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run installed UI validation and session restore from the ordinary user token.'
}
$smokeInstall = 'D:\Applications\Pulse'
$smokeProduct = Join-Path $smokeInstall 'Pulse.exe'
$smokeProfile = Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../bench_data'))) ('installed-smoke-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($smokeProfile) | Out-Null
$smokeOutput = Join-Path $PSScriptRoot 'installed-home.png'
$smokeStart = [Diagnostics.ProcessStartInfo]::new($smokeProduct)
$smokeStart.Arguments = '--test-instance --shot "' + $smokeOutput + '" --shot-language zh-CN --size 1200x900 pulse:home'
$smokeStart.UseShellExecute = $false
$smokeStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$smokeStart.WorkingDirectory = $smokeInstall
$smokeStart.Environment['PULSE_TEST_DATA_DIR'] = $smokeProfile
$smokeStart.Environment['PULSE_TEST_THUMB_CACHE'] = 'off'
$smokeStart.Environment['PULSE_TEST_HIDDEN_SHOT'] = '1'
$smokeProcess = [Diagnostics.Process]::Start($smokeStart)
try {
    if (!$smokeProcess.WaitForExit(30000)) {
        $smokeProcess.Kill()
        $smokeProcess.WaitForExit()
        throw 'Owned isolated screenshot process timed out.'
    }
    if ($smokeProcess.ExitCode -ne 0 -or !(Test-Path -LiteralPath $smokeOutput)) {
        throw ('Installed screenshot failed, exit=' + $smokeProcess.ExitCode)
    }
    $smokeResult = [ordered]@{ executable=$smokeProduct; sha256=(Get-FileHash -LiteralPath $smokeProduct).Hash;
        elevated=$false; fixture=$smokeProfile; screenshot=$smokeOutput; isolated_exit_code=$smokeProcess.ExitCode }
} finally { $smokeProcess.Dispose() }
# Reuse a session the user has already reopened during validation.
$smokeRestored = Get-Process Pulse -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $smokeProduct } | Select-Object -First 1
$smokeResult.session_restore = 'existing_process'
if (!$smokeRestored) {
    $smokeRestored = Start-Process -FilePath $smokeProduct -WorkingDirectory $smokeInstall -WindowStyle Normal -PassThru
    $smokeResult.session_restore = 'launched'
}
$smokeDeadline = [DateTime]::UtcNow.AddSeconds(15)
do {
    Start-Sleep -Milliseconds 250
    $smokeRestored.Refresh()
    if ($smokeRestored.HasExited) {
        if ($smokeRestored.ExitCode -ne 0) { throw ('Restored installed UI exited: ' + $smokeRestored.ExitCode) }
        $smokeResult.launch_exit_code = 0
        $smokeRestored = Get-Process Pulse -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $smokeProduct } | Select-Object -First 1
        if (!$smokeRestored) { throw 'No installed UI after a zero-exit launch.' }
        $smokeResult.session_restore = 'forwarded_to_existing_process'
    }
} while (!$smokeRestored.MainWindowHandle -and [DateTime]::UtcNow -lt $smokeDeadline)
if (!$smokeRestored.MainWindowHandle) { throw 'Restored installed UI did not expose a main window.' }
$smokeResult.restored_process_id = $smokeRestored.Id
$smokeResult.restored_executable = $smokeRestored.Path
$smokeResult.restored_main_window = $smokeRestored.MainWindowHandle.ToInt64()
$smokeResult.service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" | Select-Object State, StartMode, PathName
$smokeResult | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'installed-smoke.json') -Encoding utf8
$smokeResult | ConvertTo-Json -Depth 5
