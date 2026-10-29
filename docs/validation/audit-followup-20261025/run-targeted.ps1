param([string[]]$Only = @(), [string]$Manifest = 'runtime-results.json')
$ErrorActionPreference = 'Continue'
$audit_root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$audit_build = Join-Path $audit_root 'build_audit_msbuild_20261025\Release'
$audit_cases = @(
    @{ Name = 'index-ingest'; Exe = 'pulse_index_engine_test'; Args = @('--ingest-boundary-only') },
    @{ Name = 'index-usn'; Exe = 'pulse_index_engine_test'; Args = @('--usn-only') },
    @{ Name = 'index-notify'; Exe = 'pulse_index_engine_test'; Args = @('--notify-modified-only') },
    @{ Name = 'metafile-native'; Exe = 'pulse_metafile_input_test'; Args = @() },
    @{ Name = 'shell-commands'; Exe = 'pulse_shell_launch_regression_test'; Args = @('--commands-only') },
    @{ Name = 'shell-launch'; Exe = 'pulse_shell_launch_test'; Args = @((Join-Path $audit_build 'pulse_shell_launch_worker_fixture.exe'), '--commands-only') },
    @{ Name = 'shell-namespace'; Exe = 'pulse_shell_query_test'; Args = @((Join-Path $audit_build 'pulse_shell_query_fixture.exe'), '--namespace') },
    @{ Name = 'shell-preview-item'; Exe = 'pulse_shell_query_test'; Args = @((Join-Path $audit_build 'pulse_shell_query_fixture.exe'), '--preview-item') },
    @{ Name = 'shell-malformed-result'; Exe = 'pulse_shell_launch_client_test'; Args = @('--malformed-results') },
    @{ Name = 'context-reuse'; Exe = 'pulse_context_query_timeout_test'; Args = @('--reuse') },
    @{ Name = 'context-timeout'; Exe = 'pulse_context_query_timeout_test'; Args = @() },
    @{ Name = 'ops-followup'; Exe = 'pulse_audit_ops_followup_test'; Args = @() },
    @{ Name = 'session-this-pc'; Exe = 'pulse_json_persistence_test'; Args = @('--session-this-pc') },
    @{ Name = 'session-preferences'; Exe = 'pulse_json_persistence_test'; Args = @('--session-preferences') },
    @{ Name = 'session-windows'; Exe = 'pulse_session_windows_test'; Args = @() },
    @{ Name = 'app-lifecycle'; Exe = 'pulse_multiwindow_application_test'; Args = @('--app-lifecycle-only') },
    @{ Name = 'app-session'; Exe = 'pulse_multiwindow_application_test'; Args = @('--session-only') },
    @{ Name = 'binary-input'; Exe = 'pulse_binary_input_boundary_test'; Args = @() },
    @{ Name = 'permanent-fileop'; Exe = 'pulse_permanent_fileop_console_test'; Args = @(); WorkingDirectory = (Join-Path $audit_root 'build_audit_msbuild_20261025\console') },
    @{ Name = 'session-layout'; Exe = 'pulse_session_layout_test'; Args = @() },
    @{ Name = 'app-controller-layout'; Exe = 'pulse_app_controllers_test'; Args = @('--session-layout') },
    @{ Name = 'context-settings-save'; Exe = 'pulse_app_controllers_test'; Args = @('--context-settings-save') },
    @{ Name = 'system-context-registry'; Exe = 'pulse_system_ctx_test'; Args = @() },
    @{ Name = 'system-context-model'; Exe = 'pulse_system_ctx_model_test'; Args = @() },
    @{ Name = 'system-context-exchange-protocol'; Exe = 'pulse_system_ctx_exchange_test'; Args = @('--protocol-only'); WorkingDirectory = (Join-Path $audit_root 'build_audit_msbuild_20261025\console') },
    @{ Name = 'system-context-transaction-console'; Exe = 'pulse_system_ctx_transaction_console_test'; Args = @(); WorkingDirectory = (Join-Path $audit_root 'build_audit_msbuild_20261025\console') },
    @{ Name = 'user-storage'; Exe = 'pulse_user_storage_test'; Args = @() }
)
if ($Only.Count) { $audit_cases = @($audit_cases | Where-Object { $_.Name -in $Only }) }
$audit_results = foreach ($audit_case in $audit_cases) {
    $audit_exe = Join-Path $audit_build ($audit_case.Exe + '.exe')
    $audit_args = $audit_case.Args
    $audit_log = Join-Path $PSScriptRoot ($audit_case.Name + '.txt')
    $audit_work = if ($audit_case.WorkingDirectory) { $audit_case.WorkingDirectory } else { $audit_root }
    Push-Location -LiteralPath $audit_work
    try {
        $audit_output = & $audit_exe @audit_args 2>&1
        $audit_exit = $LASTEXITCODE
    } finally { Pop-Location }
    $audit_output | Set-Content -LiteralPath $audit_log -Encoding utf8
    if (-not (Test-Path -LiteralPath $audit_log)) { '' | Set-Content -LiteralPath $audit_log }
    $audit_record = [pscustomobject]@{
        Name = $audit_case.Name
        Executable = $audit_exe
        Arguments = $audit_args
        WorkingDirectory = $audit_work
        ExitCode = $audit_exit
        OutputLines = @($audit_output).Count
        Log = $audit_log
    }
    Write-Host ($audit_case.Name + ': exit ' + $audit_exit + ', output lines ' + @($audit_output).Count)
    $audit_record
}
$audit_results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot $Manifest) -Encoding utf8
