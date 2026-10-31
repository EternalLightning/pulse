# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

param([string]$RepoPath = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath $RepoPath).Path
$pin = Get-Content -LiteralPath (Join-Path $repo 'cmake/lumatext-sdk.json') -Raw | ConvertFrom-Json
if ($pin.source -ne 'committed' -or $pin.path -ne 'third_party/lumatext' -or
    $pin.manifest -ne 'sdk-manifest.json' -or $pin.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
    throw 'Unsupported LumaText SDK pin'
}
$sdkRoot = Join-Path $repo $pin.path
$manifestPath = Join-Path $sdkRoot $pin.manifest
if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne $pin.sha256) {
    throw 'LumaText SDK manifest checksum mismatch'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.architecture -ne 'x64' -or
    $manifest.configuration -ne 'Release' -or $manifest.runtime -ne 'MT' -or
    $manifest.toolset -ne 'v143' -or -not $manifest.files) {
    throw 'LumaText release SDK must be x64 Release, v143, static CRT'
}
$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$prefix = [IO.Path]::GetFullPath($sdkRoot) + [IO.Path]::DirectorySeparatorChar
foreach ($file in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $sdkRoot $file.path))
    if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not $names.Add($file.path) -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw "Invalid SDK manifest entry: $($file.path)"
    }
    if ((Get-Item -LiteralPath $path).Length -ne $file.bytes -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "LumaText SDK file checksum mismatch: $($file.path)"
    }
}
foreach ($required in @('bin/lumatext.dll', 'lib/lumatext.lib', 'include/lumatext/lumatext.h',
    'include/lumatext/lumatext.hpp', 'lib/cmake/LumaText/LumaTextConfig.cmake',
    'lib/cmake/LumaText/LumaTextConfigVersion.cmake', 'lib/cmake/LumaText/LumaTextTargets.cmake',
    'lib/cmake/LumaText/LumaTextTargets-release.cmake', 'share/LumaText/licenses/LICENSE',
    'share/LumaText/licenses/FreeType.txt', 'share/LumaText/licenses/HarfBuzz.txt',
    'share/LumaText/licenses/Unicode.txt')) {
    if (-not $names.Contains($required)) { throw "SDK manifest omits $required" }
}
foreach ($file in Get-ChildItem -LiteralPath $sdkRoot -Recurse -File -Force) {
    $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
    if ($relative -ne $pin.manifest -and -not $names.Contains($relative)) {
        throw "Unpinned SDK file: $relative"
    }
}
Write-Host "Verified $($names.Count) pinned LumaText SDK files (v143 /MT)."
Write-Output $sdkRoot
