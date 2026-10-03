param(
    [string]$BuildDirectory = 'build',
    [switch]$IncludeRestartManager
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path (Join-Path $root $BuildDirectory)).Path
$output = Join-Path $root 'bench_data\upstream-integration-checks'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$env:PULSE_TEST_FIXTURE_ROOT = $output
$checks = @(
    @{ Name = 'pulse_dialog_text_fit_test'; Args = @() },
    @{ Name = 'pulse_startup_command_test'; Args = @() },
    @{ Name = 'pulse_default_file_manager_test'; Args = @() },
    @{ Name = 'pulse_entry_order_hold_test'; Args = @() },
    @{ Name = 'pulse_quick_access_badges_test'; Args = @() },
    @{ Name = 'pulse_sidebar_scrollbar_fade_test'; Args = @() },
    @{ Name = 'pulse_child_edit_test'; Args = @() },
    @{ Name = 'pulse_menu_text_test'; Args = @((Join-Path $output 'menu')) },
    @{ Name = 'pulse_dialog_edit_integration_test'; Args = @() },
    @{ Name = 'pulse_locked_operation_test'; Args = @('--decisions') },
    @{ Name = 'pulse_locked_item_prompt_ui_test'; Args = @((Join-Path $output 'locked-prompts')) },
    @{ Name = 'shell_navigation_fixture_test'; Args = @() },
    @{ Name = 'entry_order_application_fixture_test'; Args = @() },
    @{ Name = 'tab_close_interaction_test'; Args = @() },
    @{ Name = 'batch1_display_test'; Args = @((Join-Path $output 'display')) },
    @{ Name = 'confirmation_paths_fixture_test'; Args = @() }
)
if ($IncludeRestartManager) {
    # These only use newly-created holder processes/files. No user's process is
    # ever terminated; the tests verify the exact controlled PID set first.
    $checks += @{ Name = 'pulse_file_lock_owner_test'; Args = @() }
    $checks += @{ Name = 'pulse_locked_operation_test'; Args = @() }
}
Push-Location $root
try {
    foreach ($check in $checks) {
        $name = $check.Name
        $arguments = $check.Args
        $executable = Join-Path $build "$name.exe"
        if (!(Test-Path -LiteralPath $executable)) { throw "Build the focused test first: $name" }
        $suffix = if ($arguments -contains '--decisions') { '-decisions' } else { '' }
        $log = Join-Path $output "$name$suffix.log"
        Write-Output "=== $name $arguments ==="
        & $executable @arguments 2>&1 | Tee-Object -FilePath $log
        $code = $LASTEXITCODE
        if ($code) { throw "$name failed with exit $code; diagnostics: $log" }
    }
} finally { Pop-Location }
