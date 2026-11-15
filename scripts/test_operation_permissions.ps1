# SPDX-License-Identifier: Apache-2.0
param(
    [string]$BuildDir = 'build-release',
    [string]$EvidenceDirectory = 'bench_data/operation-permissions',
    [string[]]$Case = @(),
    [switch]$IncludeUi,
    [switch]$WithUac
)
$ErrorActionPreference = 'Stop'
$permissionPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if ($permissionPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this suite from an ordinary terminal so protected operations genuinely require elevation.'
}
$permissionBuild = (Resolve-Path -LiteralPath $BuildDir).Path
$permissionEvidenceParent = (New-Item -ItemType Directory -Path $EvidenceDirectory -Force).FullName
$permissionEvidence = (New-Item -ItemType Directory -Path (Join-Path $permissionEvidenceParent (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))).FullName
$permissionRuntime = Join-Path ([IO.Path]::GetTempPath()) ('PulsePermissionTests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $permissionRuntime | Out-Null
$permissionCases = @(
    @{ name='shell-contract'; category='no-uac-contract'; exe='pulse_shell_transfer_result_test.exe'; args=@() },
    @{ name='permission-policy'; category='no-uac-policy'; exe='pulse_operation_permission_policy_test.exe'; args=@() },
    @{ name='recycle-contract'; category='no-permanent-fallback'; exe='pulse_shell_recycle_contract_test.exe'; args=@() },
    @{ name='recycle-worker'; category='same-privilege-transport'; exe='pulse_operation_elevation_adapter_test.exe'; args=@() },
    @{ name='ordinary-overwrite'; category='ordinary-access'; exe='pulse_shell_operation_integration_test.exe'; args=@('--case','overwrite') },
    @{ name='protected-operations'; category='administrator-required'; exe='pulse_shell_operation_integration_test.exe'; args=@('--case','permissions') },
    @{ name='create-delete-restore'; category='ordinary-access'; exe='pulse_shell_operation_integration_test.exe'; args=@('--case','create-delete') }
)
if ($IncludeUi) {
    $permissionCases += @{ name='pulse-confirmation-ui'; category='private-desktop-ui'; exe='pulse_permission_operation_prompt_ui_test.exe';
        args=@('--file-operation-only', (Join-Path $permissionEvidence 'ui')) }
}
if ($WithUac) {
    Write-Output 'Explicit WithUac selected: Windows will request real administrator consent for isolated fixtures.'
    $permissionCases += @{ name='real-uac'; category='real-uac'; exe='pulse_shell_operation_integration_test.exe'; args=@('--case','permissions-admin') }
}
if ($Case.Count) {
    foreach ($permissionSelectedCase in $Case) {
        if ($permissionSelectedCase -notin $permissionCases.name) { throw "Unknown or disabled case: $permissionSelectedCase" }
    }
    $permissionCases = @($permissionCases | Where-Object { $_.name -in $Case })
}
$permissionArtifacts = @()
$permissionNames = @('Pulse.Shell.exe') + @($permissionCases.exe | Select-Object -Unique)
if ($IncludeUi) { $permissionNames += 'lumatext.dll' }
foreach ($permissionName in $permissionNames) {
    $permissionSource = Join-Path $permissionBuild $permissionName
    $permissionCopy = Join-Path $permissionRuntime $permissionName
    # Fresh bytes inherit the normal temporary directory's label, rather than a Low label on build outputs.
    $permissionInput = [IO.File]::OpenRead($permissionSource)
    try {
        $permissionOutput = [IO.File]::Open($permissionCopy, 'CreateNew', 'Write', 'None')
        try { $permissionInput.CopyTo($permissionOutput) } finally { $permissionOutput.Dispose() }
    } finally { $permissionInput.Dispose() }
    $permissionHash = (Get-FileHash -LiteralPath $permissionSource -Algorithm SHA256).Hash
    if ($permissionHash -ne (Get-FileHash -LiteralPath $permissionCopy -Algorithm SHA256).Hash) {
        throw "Hash mismatch: $permissionName"
    }
    $permissionArtifacts += @{ name=$permissionName; sha256=$permissionHash }
}
$permissionResults = @()
foreach ($permissionCase in $permissionCases) {
    $permissionLog = Join-Path $permissionEvidence ($permissionCase.name + '.log')
    $permissionArguments = @($permissionCase.args)
    & (Join-Path $permissionRuntime $permissionCase.exe) @permissionArguments *> $permissionLog
    $permissionExit = $LASTEXITCODE
    $permissionLines = @(Get-Content -LiteralPath $permissionLog)
    $permissionResults += [ordered]@{
        name=$permissionCase.name; category=$permissionCase.category; exit_code=$permissionExit
        passed=@($permissionLines | Where-Object { $_ -match '^\[PASS\]' }).Count
        failed=@($permissionLines | Where-Object { $_ -match '^\[FAIL\]' }).Count
        blocked=@($permissionLines | Where-Object { $_ -match '^\[BLOCKED\]' }).Count
        integrity=@($permissionLines | Where-Object { $_ -match 'integrity RID=' })
        log=$permissionLog
    }
    Write-Output "[$($permissionCase.category)] $($permissionCase.name): exit=$permissionExit"
    $permissionLines | Where-Object { $_ -match '^\[(FAIL|BLOCKED|DIAG|RESULT)\]' } | Write-Output
}
[ordered]@{
    timestamp=Get-Date -Format o; runtime=$permissionRuntime; artifacts=$permissionArtifacts
    real_uac_requested=[bool]$WithUac; results=$permissionResults
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $permissionEvidence 'results.json') -Encoding utf8
Write-Output "Evidence: $permissionEvidence"
Write-Output "Hash-verified runtime retained: $permissionRuntime"
if (@($permissionResults | Where-Object { $_.exit_code -ne 0 -or $_.failed -or $_.blocked }).Count) { exit 1 }
exit 0
