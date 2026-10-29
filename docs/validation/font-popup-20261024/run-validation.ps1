param([string]$BuildDir = 'build_release_clean', [string]$CaseFilter = '*')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$savedEnvironment = $env:PULSE_TEST_STREAM_SNAPSHOTS
$savedEncoding = [Console]::OutputEncoding
$failedCases = 0
Push-Location $repo
try {
    $env:PULSE_TEST_STREAM_SNAPSHOTS = '1'
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $cases = @(
        @('pulse_menu_text_test.exe', '--dropdown-animation', 'menu-dropdown-animation'),
        @('pulse_menu_text_test.exe', '--animation-idle', 'menu-animation-idle'),
        @('pulse_menu_text_test.exe', '--lifecycle', 'menu-lifecycle'),
        @('pulse_menu_text_test.exe', '--toggle', 'menu-toggle'),
        @('pulse_menu_text_test.exe', '--history-refresh', 'menu-history-refresh'),
        @('pulse_text_render_settings_test.exe', '--plain-text-only', 'plain-text')
    )
    foreach ($case in $cases) {
        if ($case[2] -notlike $CaseFilter) { continue }
        $lines = [Collections.Generic.List[string]]::new()
        $saved = 0
        & (Join-Path $repo "$BuildDir/$($case[0])") $case[1] 2>&1 | ForEach-Object {
            $line = $_.ToString()
            if ($line -match '^PULSE_PNG\|([^|]+)\|([A-Za-z0-9+/=]+)$') {
                $path = [IO.Path]::GetFullPath($Matches[1])
                if (!$path.StartsWith($outputRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
                    [IO.Path]::GetExtension($path) -ne '.png') { throw "Unexpected snapshot destination: $path" }
                $bytes = [Convert]::FromBase64String($Matches[2])
                if ($bytes.Length -lt 64 -or [BitConverter]::ToString($bytes, 0, 8) -ne '89-50-4E-47-0D-0A-1A-0A') {
                    throw "Invalid PNG from $($case[2])"
                }
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
                [IO.File]::WriteAllBytes($path, $bytes)
                $lines.Add("[ARTIFACT] $path ($($bytes.Length) bytes)")
                ++$saved
            } else { $lines.Add($line) }
        }
        $nativeExit = $LASTEXITCODE
        $lines | Set-Content -LiteralPath (Join-Path $outputRoot "$($case[2]).log") -Encoding utf8
        $passed = @($lines | Where-Object { $_ -match '^\[PASS\]' }).Count
        $failed = @($lines | Where-Object { $_ -match '\[FAIL\]' }).Count
        Write-Output "$($case[2]): exit=$nativeExit pass=$passed fail=$failed png=$saved"
        $lines | Where-Object { $_ -match '\[FAIL\]' } | Write-Output
        if ($nativeExit -ne 0 -or $failed -gt 0 -or
            ($case[2] -eq 'menu-dropdown-animation' -and $saved -ne 16) -or
            ($case[2] -eq 'plain-text' -and $saved -ne 24)) { ++$failedCases }
    }
} finally {
    $env:PULSE_TEST_STREAM_SNAPSHOTS = $savedEnvironment
    [Console]::OutputEncoding = $savedEncoding
    Pop-Location
}
if ($failedCases) { exit 1 }
