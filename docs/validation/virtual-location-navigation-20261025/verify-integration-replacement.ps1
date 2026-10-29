param([Parameter(Mandatory)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$fixture = Join-Path $repo ('bench_data/integration-replacement-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $fixture 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
$binaries = @('Pulse.exe','Pulse.Index.exe','Pulse.Document.exe','Pulse.Preview.exe','Pulse.Shell.exe','Pulse.Integration.exe')
foreach ($name in $binaries) { Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $payload $name) }
foreach ($name in @('lumatext.dll','pdfium.dll')) { Set-Content -LiteralPath (Join-Path $payload $name) -Value 'fixture dependency' }
$global:PulseReplacementFixture = @{ Calls = 0; FailAfterWrite = $false; Install = '' }
# These mocks are scoped to this standalone fixture process, with workspace-only installations.
function Get-CimInstance {
    param([string]$ClassName, [string]$Filter)
    if ($ClassName -eq 'Win32_Service') {
        $global:PulseReplacementFixture.Calls++
        return [pscustomobject]@{
            State = $(if ($global:PulseReplacementFixture.FailAfterWrite -and $global:PulseReplacementFixture.Calls -gt 1) { 'Running' } else { 'Stopped' })
            StartMode = 'Auto'
            PathName = (Join-Path $global:PulseReplacementFixture.Install 'Pulse.Index.exe') + ' --service'
        }
    }
    if ($ClassName -eq 'Win32_Process') { return }
    throw "Unexpected fixture query: $ClassName"
}
$checks = 0
function Check([bool]$ok, [string]$message) {
    if (!$ok) { throw "[FAIL] $message" }
    $script:checks++
    Write-Output "[PASS] $message"
}
function Prepare([string]$name, [bool]$failAfterWrite) {
    $install = Join-Path $fixture $name
    New-Item -ItemType Directory -Path $install | Out-Null
    Get-ChildItem -LiteralPath $payload -File | Copy-Item -Destination $install
    Set-Content -LiteralPath (Join-Path $install 'Pulse.exe') -Value 'old fixture main'
    Set-Content -LiteralPath (Join-Path $install 'pulse_integration.exe') -Value 'old fixture helper'
    Remove-Item -LiteralPath (Join-Path $install 'Pulse.Integration.exe')
    $global:PulseReplacementFixture.Calls = 0
    $global:PulseReplacementFixture.FailAfterWrite = $failAfterWrite
    $global:PulseReplacementFixture.Install = $install
    return $install
}
function Invoke-Replacement([string]$install, [string]$backup, [switch]$Apply) {
    if (![IO.Path]::GetFullPath($install).StartsWith($fixture + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture installation escaped workspace.' }
    & (Join-Path $repo 'scripts/replace_installed.ps1') -PayloadDir $payload -InstallDir $install -BackupRoot $backup -Apply:$Apply
}
try {
    $install = Prepare 'success' $false
    $oldMain = (Get-FileHash -LiteralPath (Join-Path $install 'Pulse.exe')).Hash
    $oldHelper = (Get-FileHash -LiteralPath (Join-Path $install 'pulse_integration.exe')).Hash
    $plan = Invoke-Replacement $install (Join-Path $fixture 'success-backups') | ConvertFrom-Json
    Check ($plan.Changed -eq 3 -and $plan.Changes -contains 'pulse_integration.exe' -and $plan.Changes -contains 'Pulse.Integration.exe') 'plan includes new helper and legacy retirement'
    $result = Invoke-Replacement $install (Join-Path $fixture 'success-backups') -Apply | ConvertFrom-Json
    Check ($result.Success -and $result.Changed -eq 3) 'replacement transaction succeeds'
    Check (!(Test-Path -LiteralPath (Join-Path $install 'pulse_integration.exe'))) 'legacy helper is removed'
    Check ((Get-FileHash -LiteralPath (Join-Path $install 'Pulse.Integration.exe')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload 'Pulse.Integration.exe')).Hash) 'new helper matches payload hash'
    Check ((Get-FileHash -LiteralPath (Join-Path $result.Backup 'payload/pulse_integration.exe')).Hash -eq $oldHelper) 'legacy helper backup matches original hash'
    Check ((Get-FileHash -LiteralPath (Join-Path $result.Backup 'payload/Pulse.exe')).Hash -eq $oldMain) 'previous main backup matches original hash'
    $noOp = Invoke-Replacement $install (Join-Path $fixture 'success-backups') | ConvertFrom-Json
    Check ($noOp.Changed -eq 0) 'completed migration is idempotent'
    $install = Prepare 'rollback' $true
    $backupRoot = Join-Path $fixture 'rollback-backups'
    $failed = $false
    try { Invoke-Replacement $install $backupRoot -Apply | Out-Null } catch {
        $failed = $_.Exception.Message.Contains('Service or index-agent state changed')
    }
    Check $failed 'injected post-write failure reaches rollback'
    Check ((Get-FileHash -LiteralPath (Join-Path $install 'pulse_integration.exe')).Hash -eq $oldHelper) 'rollback restores retired helper'
    Check (!(Test-Path -LiteralPath (Join-Path $install 'Pulse.Integration.exe'))) 'rollback removes newly introduced helper'
    Check ((Get-FileHash -LiteralPath (Join-Path $install 'Pulse.exe')).Hash -eq $oldMain) 'rollback restores previous main'
    $resultFile = Get-ChildItem -LiteralPath $backupRoot -File -Filter result.json -Recurse | Select-Object -First 1
    $rollback = Get-Content -LiteralPath $resultFile.FullName -Raw | ConvertFrom-Json
    Check (!$rollback.Success -and $rollback.RollbackErrors.Count -eq 0) 'rollback report contains no recovery errors'
    Write-Output "$checks checks, 0 failures"
} finally {
    Remove-Variable -Name PulseReplacementFixture -Scope Global
    # The fixture is a freshly created, verified workspace subdirectory.
    if ([IO.Path]::GetFullPath($fixture).StartsWith((Join-Path $repo 'bench_data') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $fixture -Recurse -Force
    }
}
