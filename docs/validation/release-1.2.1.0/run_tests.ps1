param([string[]]$Only, [string]$WorkingDirectory, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$build = if ($BuildDirectory) { $BuildDirectory } else { Join-Path $repo 'build_release_clean' }
$cases = @(
    @{ Name='child-auto'; Exe='pulse_child_edit_test.exe'; Args='' },
    @{ Name='child-sharp'; Exe='pulse_child_edit_test.exe'; Args='--sharp' },
    @{ Name='child-smooth'; Exe='pulse_child_edit_test.exe'; Args='--smooth' },
    @{ Name='dialog-auto'; Exe='pulse_dialog_edit_integration_test.exe'; Args='' },
    @{ Name='dialog-sharp'; Exe='pulse_dialog_edit_integration_test.exe'; Args='--sharp' },
    @{ Name='dialog-smooth'; Exe='pulse_dialog_edit_integration_test.exe'; Args='--smooth' },
    @{ Name='settings'; Exe='pulse_text_render_settings_test.exe'; Args='' },
    @{ Name='hosted'; Exe='pulse_hosted_edit_stability_test.exe'; Args='--zh' }
)
$results = @()
if ($Only) { $cases = @($cases | Where-Object { $_.Name -in $Only }) }
foreach ($case in $cases) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $build $case.Exe
    $start.WorkingDirectory = if ($WorkingDirectory) { $WorkingDirectory } else { $PSScriptRoot }
    $start.Arguments = $case.Args
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $null = $start.Environment.Remove('PULSE_TEST_COMPOSED_EDIT')
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(60000)) { throw "Test timed out: $($case.Name), PID=$($process.Id)" }
    $log = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot ($case.Name+'.log')), $log)
    $result = [pscustomobject]@{ Name=$case.Name; ExitCode=$process.ExitCode; Pass=([regex]::Matches($log,'\[PASS\]').Count); Fail=([regex]::Matches($log,'\[FAIL\]').Count) }
    $process.Dispose()
    $results += $result
    $resultName = if ($Only) { 'tests-' + ($Only -join '-') + '.json' } else { 'tests.json' }
    $results | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot $resultName) -Encoding utf8
    $result | ConvertTo-Json -Compress
    if ($result.ExitCode -ne 0 -or $result.Fail -ne 0 -or $result.Pass -eq 0) { throw "Test failed: $($case.Name)" }
}
