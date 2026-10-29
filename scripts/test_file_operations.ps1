param(
    [Parameter(Mandatory = $true)][string]$BinaryDirectory,
    [Parameter(Mandatory = $true)][string]$LocalParent,
    [string]$NetworkParent = 'Z:\',
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
$matrixBinaryPath = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$matrixLocalPath = (Resolve-Path -LiteralPath $LocalParent).Path
$matrixEvidencePath = (New-Item -ItemType Directory -Path $EvidenceDirectory -Force).FullName
$matrixRunName = 'file-operation-matrix-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
$matrixRuntime = Join-Path $env:TEMP ('PulseFileOperationValidation-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $matrixRuntime | Out-Null

# A normal temporary runtime avoids the observed build-directory startup error.
# The same process token runs byte-identical binaries and the production host.
$matrixArtifacts = foreach ($matrixName in @('pulse_ops_test.exe', 'Pulse.Shell.exe')) {
    $matrixSource = Join-Path $matrixBinaryPath $matrixName
    $matrixCopy = Join-Path $matrixRuntime $matrixName
    Copy-Item -LiteralPath $matrixSource -Destination $matrixCopy
    $matrixSourceHash = (Get-FileHash -LiteralPath $matrixSource -Algorithm SHA256).Hash
    $matrixCopyHash = (Get-FileHash -LiteralPath $matrixCopy -Algorithm SHA256).Hash
    if ($matrixSourceHash -ne $matrixCopyHash) { throw "SHA256 mismatch: $matrixName" }
    [ordered]@{ name = $matrixName; source = $matrixSource; copy = $matrixCopy; sha256 = $matrixSourceHash }
}

$matrixLog = Join-Path $matrixEvidencePath ($matrixRunName + '.log')
$matrixArguments = @('--file-operation-matrix', $matrixLocalPath)
if ($NetworkParent) { $matrixArguments += $NetworkParent }
& (Join-Path $matrixRuntime 'pulse_ops_test.exe') @matrixArguments *> $matrixLog
$matrixExit = $LASTEXITCODE
$matrixLines = @(Get-Content -LiteralPath $matrixLog)
$matrixSummary = [ordered]@{
    timestamp = Get-Date -Format o
    runtime_directory = $matrixRuntime
    artifacts = @($matrixArtifacts)
    local_parent = $matrixLocalPath
    network_parent = $NetworkParent
    exit_code = $matrixExit
    passed = @($matrixLines | Where-Object { $_ -match '^\[PASS\]' }).Count
    failed = @($matrixLines | Where-Object { $_ -match '^\[FAIL\]' }).Count
    blocked = @($matrixLines | Where-Object { $_ -match '^\[BLOCKED\]' }).Count
    groups = @($matrixLines | Where-Object { $_ -match '^== ' })
    fixture_roots = @($matrixLines | Where-Object { $_ -match 'exclusive fixture=' })
    log = $matrixLog
}
$matrixSummary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $matrixEvidencePath ($matrixRunName + '.json')) -Encoding utf8
$matrixLines | Select-Object -Last 12 | Write-Output
Write-Output "Evidence: $matrixLog"
exit $matrixExit
