param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'))

$ErrorActionPreference = 'Stop'
$queryBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$queryLogs = Join-Path $queryBuild ('shell-query-verification-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$queryCases = @(
    @{ Name = 'query'; Exe = 'pulse_shell_query_test.exe'; Args = @((Join-Path $queryBuild 'pulse_shell_query_fixture.exe')) },
    @{ Name = 'icons'; Exe = 'pulse_shell_icons_test.exe'; Args = @() },
    @{ Name = 'archive-icons'; Exe = 'pulse_shell_icons_test.exe'; Args = @('--archive-icons') }
)
foreach ($queryCase in $queryCases) {
    if (-not (Test-Path -LiteralPath (Join-Path $queryBuild $queryCase.Exe) -PathType Leaf)) {
        throw "Missing test executable: $($queryCase.Exe)"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $queryBuild 'pulse_shell_query_fixture.exe') -PathType Leaf)) {
    throw 'Missing query fixture executable.'
}
New-Item -ItemType Directory -Path $queryLogs | Out-Null
$queryFailures = 0
Push-Location (Join-Path $PSScriptRoot '..')
try {
    foreach ($queryCase in $queryCases) {
        $queryExe = Join-Path $queryBuild $queryCase.Exe
        $queryArguments = $queryCase.Args
        $queryLog = Join-Path $queryLogs ($queryCase.Name + '.log')
        & $queryExe @queryArguments *> $queryLog
        $queryExit = $LASTEXITCODE
        Get-Content -LiteralPath $queryLog
        Write-Output "$($queryCase.Name): exit $queryExit"
        if ($queryExit -ne 0) { ++$queryFailures }
    }
} finally {
    Pop-Location
}
Write-Output "Logs: $queryLogs"
if ($queryFailures) { exit 1 }
exit 0
