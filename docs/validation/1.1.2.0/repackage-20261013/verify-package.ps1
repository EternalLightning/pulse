$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$evidence = $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo 'version.txt') -Raw).Trim()
$archive = Join-Path $repo "dist\Pulse-$version-portable-win-x64.zip"
if (Test-Path -LiteralPath $archive) {
    [pscustomobject]@{ path=$archive; sha256=(Get-FileHash -LiteralPath $archive).Hash; bytes=(Get-Item -LiteralPath $archive).Length } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'previous-package.json')
}
& (Join-Path $repo 'scripts\package_portable.ps1') -BuildDir (Join-Path $repo 'build') | Tee-Object -FilePath (Join-Path $evidence 'package.log')
$stages = @(Get-ChildItem -LiteralPath (Join-Path $repo 'dist') -Directory -Filter 'portable-stage-*' | Sort-Object CreationTime -Descending)
$payload = Join-Path $stages[0].FullName "Pulse-$version-portable"
$manifest = @(Get-ChildItem -LiteralPath $payload -File -Recurse | ForEach-Object {
    [pscustomobject]@{path=[IO.Path]::GetRelativePath($payload,$_.FullName).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
} | Sort-Object path)
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence 'payload-manifest.json')
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entries = @($zip.Entries | Where-Object { $_.Name })
    if ($entries.Count -ne $manifest.Count) { throw 'ZIP file count mismatch' }
    foreach ($entry in $manifest) {
        if ($entry.path -match '(?i)(^|/)(bench_data|CMakeFiles)/|\.(obj|pdb|lib|log)$|(^|/)[^/]*test[^/]*\.exe$') { throw "Development artifact: $($entry.path)" }
        $zipEntry = $zip.GetEntry("Pulse-$version-portable/$($entry.path)")
        if (!$zipEntry -or $zipEntry.Length -ne $entry.bytes) { throw "ZIP path/size mismatch: $($entry.path)" }
        $stream = $zipEntry.Open()
        $hash = [Security.Cryptography.SHA256]::Create()
        try { $actual = [Convert]::ToHexString($hash.ComputeHash($stream)) } finally { $stream.Dispose(); $hash.Dispose() }
        if ($actual -ne $entry.sha256) { throw "ZIP hash mismatch: $($entry.path)" }
    }
} finally { $zip.Dispose() }
$versions = @(foreach ($name in @('Pulse.exe','Pulse.Index.exe','Pulse.Document.exe','Pulse.Preview.exe','Pulse.Shell.exe')) {
    $info = (Get-Item -LiteralPath (Join-Path $payload $name)).VersionInfo
    if ($info.FileVersion -ne $version -or $info.ProductVersion -ne $version -or $info.OriginalFilename -cne $name) { throw "Version/filename mismatch: $name" }
    [pscustomobject]@{name=$name; file_version=$info.FileVersion; product_version=$info.ProductVersion; original_filename=$info.OriginalFilename}
})
$record = [pscustomobject]@{timestamp=(Get-Date).ToString('o');version=$version;build_id=(Get-Content -LiteralPath (Join-Path $repo 'build\pulse_build_id.txt') -Raw).Trim();archive=$archive;sha256=(Get-FileHash -LiteralPath $archive).Hash;bytes=(Get-Item -LiteralPath $archive).Length;payload_path=$payload;files=$manifest.Count;zip_hashes_match=$true;versions=$versions}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'package-verification.json')
$record | Select-Object version,files,bytes,sha256,payload_path | ConvertTo-Json -Compress
