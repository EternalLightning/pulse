param([string]$Workspace = 'E:\Projects\pulse',
      [ValidateSet('Preview', 'Pipeline', 'Transfer', 'Delete', 'Records', 'Startup')][string]$Test = 'Preview',
      [int]$WarmupSeconds = 7,
      [int]$SampleSeconds = 8)
$ErrorActionPreference = 'Stop'
$sourceRoot = Join-Path $Workspace 'build-release'
$evidenceRoot = Join-Path $Workspace 'docs\validation\performance-adaptation-20261028'
$runRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('pulse-preview-validation-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$names = switch ($Test) {
    'Transfer' { @('pulse_transfer_pipeline_test.exe') }
    'Delete' { @('pulse_permanent_fileop_console_test.exe') }
    'Records' { @('pulse_operation_records_test.exe') }
    'Startup' { @('Pulse.exe', 'Pulse.Shell.exe', 'Pulse.Preview.exe', 'Pulse.Index.exe', 'Pulse.Integration.exe', 'Pulse.Document.exe', 'pdfium.dll', 'lumatext.dll') }
    default { @('Pulse.Preview.exe', 'pulse_preview_test.exe', 'pulse_thumbnail_pipeline_test.exe', 'pdfium.dll', 'lumatext.dll') }
}
$manifest = foreach ($name in $names) {
    $source = Join-Path $sourceRoot $name
    $destination = Join-Path $runRoot $name
    Copy-Item -LiteralPath $source -Destination $destination
    $before = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $after = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($before -ne $after) { throw "Hash mismatch: $name" }
    [pscustomobject]@{ name=$name; sha256=$before; path=$destination }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot ('desktop-' + $Test + '-manifest.json')) -Encoding UTF8
$results = @()
Push-Location $Workspace
try {
    foreach ($filter in @('image-thumbnail-only', 'thumbnail-transport-only', 'thumbnail-failures-only').Where({ $Test -eq 'Preview' })) {
        $log = Join-Path $evidenceRoot ($filter + '-desktop.log')
        & (Join-Path $runRoot 'pulse_preview_test.exe') ('--' + $filter) *> $log
        $results += [pscustomobject]@{ test=$filter; exit_code=$LASTEXITCODE; log=$log }
    }
    if ($Test -in @('Preview', 'Pipeline')) {
        $log = Join-Path $evidenceRoot 'thumbnail-pipeline-desktop.log'
        & (Join-Path $runRoot 'pulse_thumbnail_pipeline_test.exe') *> $log
        $results += [pscustomobject]@{ test='thumbnail-pipeline'; exit_code=$LASTEXITCODE; log=$log }
    }
    if ($Test -eq 'Transfer') {
        $log = Join-Path $evidenceRoot 'transfer-pipeline-desktop.log'
        & (Join-Path $runRoot 'pulse_transfer_pipeline_test.exe') *> $log
        $results += [pscustomobject]@{ test='transfer-pipeline'; exit_code=$LASTEXITCODE; log=$log }
    }
    if ($Test -eq 'Records') {
        $log = Join-Path $evidenceRoot 'operation-records-desktop.log'
        & (Join-Path $runRoot 'pulse_operation_records_test.exe') *> $log
        $results += [pscustomobject]@{ test='operation-records'; exit_code=$LASTEXITCODE; log=$log }
    }
    if ($Test -eq 'Startup') {
        $log = Join-Path $evidenceRoot 'startup-desktop.log'
        & (Join-Path $PSScriptRoot 'sample-idle.ps1') -Executable (Join-Path $runRoot 'Pulse.exe') -Output (Join-Path $evidenceRoot 'idle-desktop.json') -WarmupSeconds $WarmupSeconds -SampleSeconds $SampleSeconds *> $log
        $results += [pscustomobject]@{ test='startup'; exit_code=0; log=$log }
    }
    if ($Test -eq 'Delete') {
        foreach ($filter in @('parallel', 'default')) {
            $log = Join-Path $evidenceRoot ('permanent-' + $filter + '-desktop.log')
            if ($filter -eq 'parallel') {
                & (Join-Path $runRoot 'pulse_permanent_fileop_console_test.exe') --root-parent (Join-Path $Workspace 'bench_data') --filter parallel *> $log
            } else {
                & (Join-Path $runRoot 'pulse_permanent_fileop_console_test.exe') --root-parent (Join-Path $Workspace 'bench_data') *> $log
            }
            $results += [pscustomobject]@{ test=('delete-' + $filter); exit_code=$LASTEXITCODE; log=$log }
        }
    }
} finally { Pop-Location }
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot ('desktop-' + $Test + '-results.json')) -Encoding UTF8
$results | Format-Table -AutoSize
if ($results.Where({ $_.exit_code -ne 0 }).Count) { exit 1 }
