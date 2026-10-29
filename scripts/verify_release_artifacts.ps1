#requires -Version 7.2
<#
.SYNOPSIS
Read-only portable payload verification; installer identity reporting only.
.DESCRIPTION
Pass the actual packaging directory explicitly; local Release uses build-release.
Checks ZIP payload identity, paths and EXE metadata, and records installer identity.
Does not build, install, extract the installer, or generate SHA-256 sidecar files.
See docs/build-and-package.md for the complete workflow.
.EXAMPLE
$version = (Get-Content ./version.txt -Raw).Trim()
./scripts/verify_release_artifacts.ps1 -BuildDir build-release `
    -ZipPath "dist/Pulse-$version-portable-win-x64.zip" `
    -InstallerPath "dist/PulseSetup-$version.exe" `
    -OutputJson "docs/validation/packaging-$(Get-Date -Format yyyyMMdd-HHmmss)/artifacts.json"
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [Parameter(Mandatory)][string]$ZipPath,
    [string]$InstallerPath,
    [Parameter(Mandatory)][string]$OutputJson,
    [string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
if (-not $ExpectedVersion) { $ExpectedVersion = (Get-Content -LiteralPath (Join-Path $repo 'version.txt') -Raw).Trim() }
$issues = [Collections.Generic.List[string]]::new()
$payloadResults = [Collections.Generic.List[object]]::new()
$exeVersions = [Collections.Generic.List[object]]::new()
$expected = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$executables = @('Pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe', 'Pulse.Preview.exe', 'Pulse.Shell.exe', 'Pulse.Integration.exe')
$report = [ordered]@{
    schema_version = 1
    verified_at_utc = [DateTime]::UtcNow.ToString('o')
    expected_version = $ExpectedVersion
    scope = 'Portable paths, payload identity, EXE metadata and selftest marker; installer file identity only'
    status = 'failed'
    build_dir = [IO.Path]::GetFullPath($BuildDir)
    portable = $null
    installer = [ordered]@{ status = 'not_provided'; installation_tested = $false; payload_verified = $false }
    executable_versions = $exeVersions
    payload = $payloadResults
    errors = $issues
}

function Get-ArtifactIdentity([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    if ($item.PSIsContainer) { throw "Expected file: $Path" }
    return [ordered]@{ path = $item.FullName; size_bytes = $item.Length; sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash }
}

function Add-ExpectedFile([string]$RelativePath, [string]$SourcePath) {
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) { throw "Missing payload source: $SourcePath" }
    $expected.Add($RelativePath, [IO.Path]::GetFullPath($SourcePath))
}

function Add-ExpectedTree([string]$RelativePath, [string]$SourcePath) {
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Container)) { throw "Missing payload directory: $SourcePath" }
    $files = @(Get-ChildItem -LiteralPath $SourcePath -File -Recurse -Force)
    if ($files.Count -eq 0) { throw "Empty payload directory: $SourcePath" }
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($SourcePath, $file.FullName).Replace('\', '/')
        Add-ExpectedFile "$RelativePath/$relative" $file.FullName
    }
}

try {
    if ($ExpectedVersion -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw 'Invalid expected version' }
    $build = (Resolve-Path -LiteralPath $BuildDir).Path
    foreach ($file in ($executables + @('lumatext.dll', 'pdfium.dll'))) { Add-ExpectedFile $file (Join-Path $build $file) }
    foreach ($file in @('LICENSE', 'NOTICE')) { Add-ExpectedFile $file (Join-Path $repo $file) }
    foreach ($file in @('7z.exe', '7z.dll', 'License.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $build "7zip/$file") -PathType Leaf)) { throw "Missing archive dependency: $file" }
    }
    Add-ExpectedTree '7zip' (Join-Path $build '7zip')
    foreach ($license in @('PDFium', 'LumaText')) { Add-ExpectedTree "licenses/$license" (Join-Path $build "licenses/$license") }
    foreach ($mapping in @(
        @('licenses/ib-pinyin/LICENSE.txt', 'third_party/ib-pinyin-cpp/LICENSE.txt'),
        @('licenses/FilesIcon/LICENSE-MIT.txt', 'third_party/files-icon/LICENSE-MIT.txt'),
        @('licenses/FilesIcon/README.md', 'third_party/files-icon/README.md'),
        @('licenses/MicrosoftBrand/LICENSE.pdf', 'assets/microsoft_brand/LICENSE.pdf'),
        @('licenses/MicrosoftBrand/README.md', 'assets/microsoft_brand/README.md'),
        @('licenses/FluentSystemIcons/LICENSE', 'assets/fluent/LICENSE'),
        @('licenses/FluentSystemIcons/SOURCES.md', 'assets/fluent/SOURCES.md')
    )) { Add-ExpectedFile $mapping[0] (Join-Path $repo $mapping[1]) }

    # Read the generated README template without executing the packaging script.
    $parseTokens = $null
    $parseErrors = $null
    $packageAst = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'package_portable.ps1'), [ref]$parseTokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw 'Cannot parse package_portable.ps1' }
    $templates = @($packageAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.ExpandableStringExpressionAst] -and
        $node.Extent.Text.StartsWith('@"') -and $node.Value.StartsWith('Pulse $version')
    }, $true))
    if ($templates.Count -ne 1) { throw 'Portable README template changed; update verifier' }
    $readmeBytes = [Text.UTF8Encoding]::new($false).GetBytes(
        $templates[0].Value.Replace('$version', $ExpectedVersion) + [Environment]::NewLine)
    $readmeHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($readmeBytes))

    foreach ($file in $executables) {
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $build $file))
        $actual = '{0}.{1}.{2}.{3}' -f $version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart, $version.FilePrivatePart
        $exeVersions.Add([ordered]@{ file = $file; file_version = $actual; product_version = $version.ProductVersion; original_filename = $version.OriginalFilename; source = 'build; ZIP identity checked by SHA256' })
        if ([Version]$actual -ne [Version]$ExpectedVersion -or [Version]$version.ProductVersion -ne [Version]$ExpectedVersion) {
            $issues.Add("EXE version mismatch: $file ($actual / $($version.ProductVersion) != $ExpectedVersion)")
        }
        if ($version.OriginalFilename -cne $file) { $issues.Add("EXE original filename mismatch: $file identifies as $($version.OriginalFilename)") }
    }
    $mainBytes = [IO.File]::ReadAllBytes((Join-Path $build 'Pulse.exe'))
    $selftestMarker = [Text.Encoding]::ASCII.GetString([Text.Encoding]::Unicode.GetBytes('PULSE_SELFTEST_CASE'))
    if ([Text.Encoding]::ASCII.GetString($mainBytes).Contains($selftestMarker)) { $issues.Add('Release payload contains the embedded selftest dispatcher') }

    $report.portable = Get-ArtifactIdentity $ZipPath
    $report.portable['payload_verified'] = $false
    $zip = [IO.Compression.ZipFile]::OpenRead($report.portable.path)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $root = "Pulse-$ExpectedVersion-portable"
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            $segments = $name.TrimEnd('/').Split('/')
            $unsafe = -not $name -or $name -match '[<>:"|?*\\\x00-\x1f]' -or $name.StartsWith('/')
            foreach ($segment in $segments) {
                if (-not $segment -or $segment -in @('.', '..') -or $segment -match '[. ]$|^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') { $unsafe = $true }
            }
            if ($unsafe) { $issues.Add("Unsafe ZIP path: $name"); continue }
            if (-not $seen.Add($name.TrimEnd('/'))) { $issues.Add("Duplicate/case-conflicting ZIP path: $name"); continue }
            if ((($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000) { $issues.Add("ZIP symbolic link: $name"); continue }
            if ($segments[0] -cne $root) { $issues.Add("Unexpected ZIP root: $name"); continue }
            $relative = ($segments | Select-Object -Skip 1) -join '/'
            if ($name.EndsWith('/')) {
                if ($relative -and -not @($expected.Keys | Where-Object { $_.StartsWith("$relative/", [StringComparison]::OrdinalIgnoreCase) }).Count) {
                    $issues.Add("Unexpected payload directory: $relative")
                }
                continue
            }
            $forbidden = $relative -match '(?i)\.(obj|log|pdb|lib|ilk|exp|tmp|dmp|cache|res|idb|ipch)$|(^|/)(tests?|bench_data|CMakeFiles|\.git)(/|$)'
            if ($relative -match '(?i)\.exe$' -and $relative -notin ($executables + @('7zip/7z.exe'))) { $forbidden = $true }
            if ($forbidden) { $issues.Add("Build/test artifact in ZIP: $relative") }
            $source = $null
            if ($relative -ceq 'README.txt') {
                $sourceHash = $readmeHash
                $sourceLength = $readmeBytes.Length
                $source = 'package_portable.ps1 generated README template'
            } elseif ($expected.ContainsKey($relative)) {
                $source = $expected[$relative]
                $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
                $sourceLength = (Get-Item -LiteralPath $source).Length
            } else {
                $issues.Add("Unexpected payload file: $relative")
                continue
            }
            $stream = $entry.Open()
            try { $zipHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) } finally { $stream.Dispose() }
            $matches = $zipHash -ceq $sourceHash -and $entry.Length -eq $sourceLength
            $payloadResults.Add([ordered]@{ path = $relative; size_bytes = $entry.Length; sha256 = $zipHash; source = $source; source_sha256 = $sourceHash; matches_source = $matches })
            if (-not $matches) { $issues.Add("Payload SHA256/size mismatch: $relative") }
        }
        foreach ($relative in (@($expected.Keys) + @('README.txt'))) {
            if (-not $seen.Contains("$root/$relative")) { $issues.Add("Missing ZIP payload: $relative") }
        }
    } finally { $zip.Dispose() }
    $report.portable['payload_verified'] = $issues.Count -eq 0
} catch { $issues.Add($_.Exception.Message) }

if ($InstallerPath) {
    try {
        $installer = Get-ArtifactIdentity $InstallerPath
        $installer['status'] = 'identity_recorded'
        $installer['installation_tested'] = $false
        $installer['payload_verified'] = $false
        $report.installer = $installer
    } catch {
        $report.installer = [ordered]@{ path = [IO.Path]::GetFullPath($InstallerPath); status = 'unavailable'; installation_tested = $false; payload_verified = $false }
        $issues.Add("Installer unavailable: $($_.Exception.Message)")
    }
}
if ($issues.Count -eq 0) { $report.status = 'passed' }
$output = [IO.Path]::GetFullPath($OutputJson)
# Keep even an accidental report-path collision from replacing input artifacts.
$inputs = @($expected.Values) + @($ZipPath, $InstallerPath, $PSCommandPath, (Join-Path $PSScriptRoot 'package_portable.ps1'))
foreach ($inputPath in $inputs) {
    if ($inputPath -and $output.Equals([IO.Path]::GetFullPath($inputPath), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Output JSON must not replace a verification input: $output"
    }
}
$parent = Split-Path -Parent $output
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $output -Encoding utf8
Write-Output "Release artifact verification: $($report.status); payload files=$($payloadResults.Count); errors=$($issues.Count); report=$output"
if ($issues.Count) { $issues | ForEach-Object { Write-Output "[FAIL] $_" }; exit 1 }
exit 0
