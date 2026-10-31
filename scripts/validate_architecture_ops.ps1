# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

param(
    [string]$BuildDir = 'build-release',
    [string]$NetworkRoot = 'Z:\',
    [string]$OutputDir = ('docs/validation/architecture-ops-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')),
    [string[]]$CaseName = @()
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path (Join-Path $repo $BuildDir)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
[IO.Directory]::CreateDirectory($output) | Out-Null
$run = Join-Path $repo ('bench_data/architecture-ops-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$bin = Join-Path $run 'bin'
[IO.Directory]::CreateDirectory($bin) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $run 'bench_data')) | Out-Null
$cases = @(
    @{ Name = 'shell-completion'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'shell-completion') },
    @{ Name = 'open-workers'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'open-workers') },
    @{ Name = 'clipboard-isolated'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'clipboard-isolated') },
    @{ Name = 'mutation-undo'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'mutation-undo') },
    @{ Name = 'undo-identity'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'undo-identity') },
    @{ Name = 'source-contract'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'source-contract') },
    @{ Name = 'local-rename'; Exe = 'pulse_rename_ops_test.exe'; Args = @() },
    @{ Name = 'delete-confirmation'; Exe = 'pulse_recycle_backend_test.exe'; Args = @('--confirmation-identity') },
    @{ Name = 'delete-locked'; Exe = 'pulse_recycle_backend_test.exe'; Args = @('--locked-error') },
    @{ Name = 'network-transfer'; Exe = 'pulse_ops_test.exe'; Args = @('--network-fixture', $NetworkRoot) },
    @{ Name = 'network-undo'; Exe = 'pulse_rename_ops_test.exe'; Args = @('--network-root', $NetworkRoot) },
    @{ Name = 'network-delete'; Exe = 'pulse_recycle_backend_test.exe'; Args = @('--network-root', $NetworkRoot) }
)
$results = @()
if ($CaseName.Count) {
    $CaseName = @($CaseName | ForEach-Object { $_ -split ',' })
    $cases += @(
        @{ Name = 'search-merge'; Exe = 'pulse_search_merge_test.exe'; Args = @() },
        @{ Name = 'search-routing'; Exe = 'pulse_search_client_routing_test.exe'; Args = @() },
        @{ Name = 'view-preparation'; Exe = 'pulse_directory_contract_application_test.exe'; Args = @('--view-preparation-only') },
        @{ Name = 'shared-requests'; Exe = 'pulse_directory_contract_application_test.exe'; Args = @('--shared-requests-only') },
        @{ Name = 'details-worker'; Exe = 'pulse_directory_contract_application_test.exe'; Args = @('--details-worker-only') },
        @{ Name = 'transfer-control'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'transfer-control') },
        @{ Name = 'directory-skip'; Exe = 'pulse_ops_test.exe'; Args = @('--filter', 'directory-skip') },
        @{ Name = 'virtual-clipboard-local'; Exe = 'pulse_virtual_clipboard_test.exe'; Args = @(); Root = $run },
        @{ Name = 'virtual-clipboard-network'; Exe = 'pulse_virtual_clipboard_test.exe'; Args = @(); Root = $NetworkRoot }
    )
    $cases = @($cases | Where-Object { $_.Name -in $CaseName })
    if ($cases.Count -ne $CaseName.Count) { throw 'Unknown or duplicate validation case name' }
}
$payload = @(@('Pulse.Shell.exe') + @($cases.Exe) | Sort-Object -Unique)
foreach ($dll in @('lumatext.dll', 'pdfium.dll')) {
    if (Test-Path -LiteralPath (Join-Path $build $dll)) { $payload += $dll }
}
$hashes = foreach ($name in $payload) {
    $source = Join-Path $build $name
    $destination = Join-Path $bin $name
    [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
    $expected = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($expected -ne $actual) { throw "Staged payload mismatch: $name" }
    [pscustomobject]@{ Name = $name; SHA256 = $actual }
}
foreach ($case in $cases) {
    $stdout = Join-Path $output ($case.Name + '.stdout.log')
    $stderr = Join-Path $output ($case.Name + '.stderr.log')
    $params = @{
        FilePath = (Join-Path $bin $case.Exe)
        WorkingDirectory = $run
        RedirectStandardOutput = $stdout
        RedirectStandardError = $stderr
        PassThru = $true
        WindowStyle = 'Hidden'
    }
    if ($case.Args.Count) { $params.ArgumentList = $case.Args }
    $previousRoot = $env:PULSE_TEST_FIXTURE_ROOT
    try {
        if ($case.Root) { $env:PULSE_TEST_FIXTURE_ROOT = $case.Root }
        $process = Start-Process @params
    } finally { $env:PULSE_TEST_FIXTURE_ROOT = $previousRoot }
    $deadline = [DateTime]::UtcNow.AddMinutes(3)
    while (!$process.WaitForExit(1000) -and [DateTime]::UtcNow -lt $deadline) {}
    $timedOut = !$process.HasExited
    if ($timedOut) { Stop-Process -Id $process.Id -Force }
    $process.WaitForExit()
    $text = [IO.File]::ReadAllText($stdout)
    $result = [pscustomobject]@{
        Name = $case.Name
        ExitCode = $process.ExitCode
        TimedOut = $timedOut
        Passed = ([regex]::Matches($text, '\[PASS\]')).Count
        Failed = ([regex]::Matches($text, '\[FAIL\]')).Count
        Skipped = ([regex]::Matches($text, '\[SKIP\]')).Count
        Unavailable = $process.ExitCode -eq 77
    }
    $results += $result
    Write-Output ($result | ConvertTo-Json -Compress)
    [pscustomobject]@{ Stage = $bin; NetworkRoot = $NetworkRoot; Payload = $hashes; Results = $results } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
}
if ($results | Where-Object { ($_.ExitCode -ne 0 -and $_.ExitCode -ne 77) -or $_.TimedOut -or $_.Failed -ne 0 }) { exit 1 }
if ($results | Where-Object Unavailable) { exit 77 }
