$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$version = '1.2.0.0'
$payloadName = "Pulse-$version-portable"
$stage = Get-ChildItem -LiteralPath "$repo\dist" -Directory -Filter 'portable-stage-*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName $payloadName) } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $stage) { throw 'Packaged payload not found' }
$payload = Join-Path $stage.FullName $payloadName
$archive = "$repo\dist\Pulse-$version-portable-win-x64.zip"
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $files = @(Get-ChildItem -LiteralPath $payload -File -Recurse | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($payload, $_.FullName)
        if ($relative -match '(^|[\\/])(bench_data|CMakeFiles|src)([\\/]|$)|\.(obj|pdb|lib|log|ilk)$|(^|[\\/])[^\\/]*test[^\\/]*\.exe$') {
            throw "Development file in payload: $relative"
        }
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        $entry = $zip.GetEntry($payloadName + '/' + $relative.Replace('\', '/'))
        if (-not $entry) { throw "ZIP entry missing: $relative" }
        $stream = $entry.Open()
        try { $zipHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
        finally { $stream.Dispose() }
        if ($hash -ne $zipHash) { throw "ZIP hash mismatch: $relative" }
        [pscustomobject]@{Path=$relative;SHA256=$hash;Length=$_.Length}
    })
    if (@($zip.Entries | Where-Object Name).Count -ne $files.Count) { throw 'ZIP contains extra files' }
} finally { $zip.Dispose() }
$versions = @(foreach ($name in @('Pulse.exe','Pulse.Index.exe','Pulse.Document.exe','Pulse.Preview.exe','Pulse.Shell.exe')) {
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $payload $name))
    if ($info.FileVersion -ne $version -or $info.ProductVersion -ne $version -or $info.OriginalFilename -cne $name) {
        throw "Wrong executable metadata: $name"
    }
    [pscustomobject]@{Name=$name;FileVersion=$info.FileVersion;ProductVersion=$info.ProductVersion;OriginalFilename=$info.OriginalFilename}
})
$result = [pscustomobject]@{Version=$version;Payload=$payload;Archive=$archive;ArchiveSHA256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash;Versions=$versions;Files=$files}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$PSScriptRoot\package-audit.json" -Encoding utf8
Write-Output ("Verified {0} staged and ZIP file hashes; {1} executable versions match {2}." -f $files.Count,$versions.Count,$version)
