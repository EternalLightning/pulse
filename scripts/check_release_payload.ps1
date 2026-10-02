param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$expectedVersion = [version](Get-Content -LiteralPath (Join-Path $repo 'version.txt') -Raw).Trim()
foreach ($binary in @('Pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe', 'Pulse.Preview.exe', 'Pulse.Shell.exe')) {
    $binaryPath = Join-Path $BuildDir $binary
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $binaryPath).Path)
    if ([version]$info.ProductVersion -ne $expectedVersion) {
        throw "Release version mismatch: $binary is $($info.ProductVersion), expected $expectedVersion"
    }
    if ($info.OriginalFilename -cne $binary) {
        throw "Release filename mismatch: $binary identifies itself as $($info.OriginalFilename). Rebuild before packaging."
    }
}
$path = Join-Path $BuildDir 'Pulse.exe'
# Inspect the binary as well as build settings: /skipbuild can reuse an old EXE.
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
$text = [Text.Encoding]::ASCII.GetString($bytes)
$marker = [Text.Encoding]::ASCII.GetString(
    [Text.Encoding]::Unicode.GetBytes('PULSE_SELFTEST_CASE'))
if ($text.Contains($marker)) {
    throw 'Release payload contains the embedded selftest suite. Rebuild with PULSE_WITH_SELFTEST=OFF before packaging.'
}
Write-Output 'Release payload check passed: no embedded selftest dispatcher.'
