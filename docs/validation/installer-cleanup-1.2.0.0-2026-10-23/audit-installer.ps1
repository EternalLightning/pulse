$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$portable = Get-Content -LiteralPath "$PSScriptRoot\..\portable-1.2.0.0-2026-10-23\package-audit.json" -Raw | ConvertFrom-Json
$installer = Join-Path $repo 'dist\PulseSetup-1.2.0.0.exe'
$version = (Get-Item -LiteralPath $installer).VersionInfo.ProductVersion.Trim()
if ($version -ne $portable.Version) { throw 'Installer product version mismatch' }
& "$repo\scripts\check_release_payload.ps1" -BuildDir "$repo\build"
if (@(Get-Content -LiteralPath "$repo\build\installer-runtime-manifest.json" -Raw | ConvertFrom-Json).Count) { throw 'Unexpected dynamic runtime' }
$inputs = @(Get-Content -LiteralPath "$PSScriptRoot\installer-build.log" | ForEach-Object {
    if ($_ -match '^\s+Compressing: (.+)$') {
        $source = $Matches[1].Trim()
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Unknown installer source: $source" }
        $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $match = @($portable.Files | Where-Object SHA256 -EQ $hash)
        if (-not $match.Count) { throw "Installer input differs from verified portable payload: $source" }
        [pscustomobject]@{Source=$source;SHA256=$hash;PortableMatches=@($match.Path)}
    }
})
$expected = @($portable.Files | Where-Object Path -NE 'README.txt')
if ($inputs.Count -ne $expected.Count) { throw "Installer file count mismatch: $($inputs.Count) expected $($expected.Count)" }
$expectedHashes = @($expected.SHA256 | Sort-Object)
$actualHashes = @($inputs.SHA256 | Sort-Object)
if (@(Compare-Object $expectedHashes $actualHashes).Count) { throw 'Installer payload inventory mismatch' }
$rollback = Get-Content -LiteralPath "$PSScriptRoot\..\operation-center-motion-2026-10-23\rollback.json" -Raw | ConvertFrom-Json
foreach ($file in $rollback.Files) {
    if ((Get-FileHash -LiteralPath (Join-Path $rollback.Backup $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Retained rollback mismatch: $($file.Path)" }
}
$result = [pscustomobject]@{Version=$version;Installer=$installer;Length=(Get-Item -LiteralPath $installer).Length;SHA256=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash;Compiler='Inno Setup 6.7.3';Inputs=$inputs;VerifiedInputCount=$inputs.Count;PortableArchive=$portable.Archive;PortableSHA256=$portable.ArchiveSHA256;RetainedRollback=$rollback.Backup;VerifiedRollbackFiles=$rollback.Files.Count;ExecutedInstaller=$false;AuthenticodeStatus=(Get-AuthenticodeSignature -LiteralPath $installer).Status.ToString();KnownCompilerWarning='Admin installer uses per-user HKCU areas'}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$PSScriptRoot\installer-audit.json" -Encoding utf8
($result.SHA256 + '  ' + (Split-Path -Leaf $installer)) | Set-Content -LiteralPath ($installer + '.sha256') -Encoding ascii
[pscustomobject]@{Version=$version;VerifiedInstallerInputs=$inputs.Count;VerifiedRollbackFiles=$rollback.Files.Count;MiB=[math]::Round($result.Length/1MB,2);SHA256=$result.SHA256;Signature=$result.AuthenticodeStatus} | ConvertTo-Json
