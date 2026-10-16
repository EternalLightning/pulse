$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$dist = (Resolve-Path -LiteralPath (Join-Path $repo 'dist')).Path
$current = Get-Content (Join-Path $PSScriptRoot 'package-verification.json') -Raw | ConvertFrom-Json
$plan = @(
    (Join-Path $dist 'portable-stage-f69812d621474fb5b5541d810eddb228'),
    (Join-Path $dist 'Pulse-before-20261016-portable.zip')
)
if ((Get-FileHash -LiteralPath $current.archive).Hash -ne $current.sha256) { throw 'Current package hash mismatch' }
if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'deployment.json'))) { throw 'No successful deployment evidence' }
$inventory = @()
foreach ($candidate in $plan) {
    $path = (Resolve-Path -LiteralPath $candidate).Path
    if (!$path.StartsWith($dist + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $path -eq $current.archive -or $current.payload_path.StartsWith($path + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cleanup target outside obsolete output: $path"
    }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse cleanup target' }
    $children = if ($item.PSIsContainer) { @(Get-ChildItem -LiteralPath $path -Force -Recurse) } else { @($item) }
    if (@($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Reparse cleanup child' }
    foreach ($file in @($children | Where-Object { !$_.PSIsContainer })) {
        $inventory += [pscustomobject]@{path=$file.FullName;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
    }
}
$inventory | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PSScriptRoot 'cleanup-inventory.json') -Encoding utf8
foreach ($candidate in $plan) {
    Remove-Item -LiteralPath $candidate -Recurse -Force
    if (Test-Path -LiteralPath $candidate) { throw 'Cleanup incomplete' }
}
[pscustomobject]@{
    timestamp=(Get-Date).ToString('o');removed_paths=$plan;removed_files=$inventory.Count
    removed_bytes=($inventory | Measure-Object bytes -Sum).Sum
    retained=@($current.archive,$current.payload_path,'E:\Projects\pulse\build',
        'D:\Applications\Pulse\backups','E:\Projects\pulse\build\deployment-backups',
        'E:\Projects\pulse\dist\verification-1.1.0.1')
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PSScriptRoot 'cleanup.json') -Encoding utf8
Get-Content (Join-Path $PSScriptRoot 'cleanup.json')
