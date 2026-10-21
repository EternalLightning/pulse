$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$version = (Get-Content -LiteralPath (Join-Path $repo 'version.txt') -Raw).Trim()
$archive = Join-Path $repo "dist\Pulse-$version-portable-win-x64.zip"
$stages = @(Get-ChildItem -LiteralPath (Join-Path $repo 'dist') -Directory -Filter 'portable-stage-*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "Pulse-$version-portable") })
if ($stages.Count -ne 1) { throw 'Expected exactly one current portable stage' }
$payload = Join-Path $stages[0].FullName "Pulse-$version-portable"
& (Join-Path $repo 'scripts\check_release_payload.ps1') -BuildDir $payload
$versions = @(foreach ($name in 'Pulse.exe','Pulse.Index.exe','Pulse.Document.exe','Pulse.Preview.exe','Pulse.Shell.exe') {
    $info = (Get-Item -LiteralPath (Join-Path $payload $name)).VersionInfo
    [pscustomobject]@{name=$name;file_version=$info.FileVersion;product_version=$info.ProductVersion;original_filename=$info.OriginalFilename}
})
$manifest = @(Get-ChildItem -LiteralPath $payload -Recurse -File | ForEach-Object {
    [pscustomobject]@{path=[IO.Path]::GetRelativePath($payload,$_.FullName).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'payload-manifest.json') -Encoding utf8
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $files = @($zip.Entries | Where-Object { !$_.FullName.EndsWith('/') })
    if ($files.Count -ne $manifest.Count) { throw 'ZIP and payload counts differ' }
    $expected = @{}
    foreach ($item in $manifest) { $expected["Pulse-$version-portable/$($item.path)"] = $item }
    $seen = @{}
    foreach ($entry in $files) {
        if ($entry.FullName -match '\.(obj|pdb|lib|log)$|(^|/)(CMakeFiles|bench_data|tests?)(/|$)' -or
            $entry.FullName -match '(^|/)\.\.(/|$)' -or !$expected.ContainsKey($entry.FullName) -or $seen.ContainsKey($entry.FullName)) {
            throw "Unexpected ZIP entry: $($entry.FullName)"
        }
        $seen[$entry.FullName] = $true
        $stream = $entry.Open()
        try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
        finally { $stream.Dispose() }
        if ($hash -ne $expected[$entry.FullName].sha256 -or $entry.Length -ne $expected[$entry.FullName].bytes) {
            throw "ZIP hash mismatch: $($entry.FullName)"
        }
    }
} finally { $zip.Dispose() }
[pscustomobject]@{version=$version;archive=$archive;sha256=(Get-FileHash -LiteralPath $archive).Hash;
    payload_path=$payload;stage=$stages[0].FullName;files=$manifest.Count;zip_hashes_match=$true;versions=$versions} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'package-verification.json') -Encoding utf8
"PACKAGE_OK: $version; $($manifest.Count) files; all ZIP hashes match"
