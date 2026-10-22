$ErrorActionPreference = 'Stop'
$package = Get-Content -LiteralPath "$PSScriptRoot\package-audit.json" -Raw | ConvertFrom-Json
$backup = Get-Content -LiteralPath "$PSScriptRoot\rollback.json" -Raw | ConvertFrom-Json
$before = Get-Content -LiteralPath "$PSScriptRoot\before-replacement.json" -Raw | ConvertFrom-Json
$root = 'D:\Applications\Pulse'
function Destination([string]$relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe installed path' }
    $path
}
function VerifyService {
    $service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'"
    if ($service.State -ne $before.Service.State -or $service.StartMode -ne $before.Service.StartMode -or
        $service.PathName -ne $before.Service.PathName) { throw 'Service differs from baseline' }
}
if (@(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Pulse*' -and $_.Name -ne 'Pulse.Index.exe' }).Count) {
    throw 'A Pulse window or helper is still running'
}
VerifyService
foreach ($file in $backup.Files) {
    if ((Get-FileHash -LiteralPath (Destination $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256 -or
        (Get-FileHash -LiteralPath (Join-Path $backup.Backup $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) {
        throw "Installed baseline or backup changed: $($file.Path)"
    }
}
$changed = @($package.Files | Where-Object {
    (Get-FileHash -LiteralPath (Destination $_.Path) -Algorithm SHA256).Hash -ne $_.SHA256
})
if (@($changed | Where-Object { $_.Path -notin @('Pulse.exe') }).Count) { throw 'Unexpected changed payload' }
$written = [Collections.Generic.List[string]]::new()
$temporary = $null
try {
    foreach ($file in $changed) {
        $source = Join-Path $package.Payload $file.Path
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.SHA256) { throw 'Staged hash mismatch' }
        $target = Destination $file.Path
        $temporary = $target + '.update-' + [guid]::NewGuid().ToString('N')
        Copy-Item -LiteralPath $source -Destination $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $file.SHA256) { throw 'Copy hash mismatch' }
        $written.Add($file.Path)
        Move-Item -LiteralPath $temporary -Destination $target -Force
        $temporary = $null
    }
    foreach ($file in $package.Files) {
        if ((Get-FileHash -LiteralPath (Destination $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) { throw 'Installed hash mismatch' }
    }
    VerifyService
    [pscustomobject]@{Version='1.2.0.0';Changed=@($changed.Path);VerifiedPayloadFiles=$package.Files.Count;Backup=$backup.Backup;ServiceUntouched=$true} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$PSScriptRoot\deployment-result.json" -Encoding utf8
} catch {
    foreach ($relative in $written) {
        Copy-Item -LiteralPath (Join-Path $backup.Backup $relative) -Destination (Destination $relative) -Force
    }
    throw
} finally {
    if ($temporary -and (Test-Path -LiteralPath $temporary)) { Remove-Item -LiteralPath $temporary -Force }
}
