# SPDX-License-Identifier: Apache-2.0
param([string]$BuildDir = 'build-release',
    [string]$Fixture = 'customization_features_application_test')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
& (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir -Targets pulse
if ($LASTEXITCODE) { throw 'Production build failed' }
$environment = & cmd.exe /d /c "call `"$PSScriptRoot\vcvars.bat`" >nul && set"
if ($LASTEXITCODE) { throw 'Cannot initialize the compiler environment' }
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0,$separator), $line.Substring($separator+1), 'Process')
    }
}
& (Join-Path $PSScriptRoot 'test_integration_fixture.ps1') -BuildDirectory $BuildDir -Fixture $Fixture
