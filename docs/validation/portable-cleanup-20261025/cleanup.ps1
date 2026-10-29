param([switch]$Apply)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..')).TrimEnd('\')
$currentStage = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'current-stage.txt') -Raw).Trim()
$candidates = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
function Add-Candidate([string]$Path, [string]$Reason) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Outside repository: $full" }
    if (Test-Path -LiteralPath $full) { $candidates.TryAdd($full, $Reason) | Out-Null }
}
foreach ($name in @('build', 'build_release_clean', 'build_audit_followup_20261025')) {
    Add-Candidate (Join-Path $repo $name) 'Superseded build tree; active VS build retained'
}
foreach ($item in Get-ChildItem -LiteralPath (Join-Path $repo 'dist') -Directory -Filter 'portable-stage-*') {
    if ($item.FullName -ne $currentStage) { Add-Candidate $item.FullName 'Superseded portable staging copy' }
}
foreach ($item in Get-ChildItem -LiteralPath (Join-Path $repo 'dist') -File) {
    if ($item.Name -match '^Pulse(?:Setup-|-)(1\.2\.0\.[0-3])(?:-portable-win-x64\.zip|\.exe)(?:\.sha256)?$') {
        Add-Candidate $item.FullName 'Superseded release artifact; rollback directories retained'
    }
}
foreach ($item in Get-ChildItem -LiteralPath (Join-Path $repo 'bench_data') -Directory) {
    if ($item.Name -match '^index-storage-pair-\d+-\d+$|^folder-size-ui-cache-\d+-\d+$') {
        Add-Candidate $item.FullName 'Exclusive generated test fixture; regression source retained'
    }
}
foreach ($relative in @(
    'bench_data/upstream-edit-review-20261024/pulse-c4fe01ed82fe091f495f531972a854aa32caac2b',
    'bench_data/edit-upstream-ab-20261024/upstream',
    'bench_data/recycle-toolbar-runner.cpp',
    'docs/Pulse_标签页一体化优化方案.html',
    'docs/Pulse_激活分栏与多选标识方案.html'
)) { Add-Candidate (Join-Path $repo $relative) 'Downloaded comparison tree, superseded runner or historical UI mockup' }
foreach ($root in @('bench_data', 'docs/validation')) {
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $repo $root) -File -Recurse -Force) {
        if ($item.FullName.StartsWith($PSScriptRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $binary = $item.Extension.ToLowerInvariant() -in @('.exe','.dll','.obj','.pdb','.lib','.exp','.ilk','.res')
        $fixture = $root -eq 'bench_data' -and $item.Extension.ToLowerInvariant() -in @('.bin','.wal','.tmp','.zip','.tgz')
        if ($binary -or $fixture) { Add-Candidate $item.FullName 'Generated binary, index fixture or downloaded archive; logs retained' }
    }
}
$selected = [Collections.Generic.List[object]]::new()
foreach ($path in ($candidates.Keys | Sort-Object Length)) {
    if (@($selected | Where-Object { $path.StartsWith($_.path + '\', [StringComparison]::OrdinalIgnoreCase) }).Count) { continue }
    $item = Get-Item -LiteralPath $path -Force
    $files = @(if ($item.PSIsContainer) { Get-ChildItem -LiteralPath $path -Recurse -Force -File } else { $item })
    $children = @(if ($item.PSIsContainer) { Get-ChildItem -LiteralPath $path -Recurse -Force })
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or @($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
        throw "Reparse point in target; inspect manually: $path"
    }
    $selected.Add([pscustomobject]@{path=$path;reason=$candidates[$path];files=$files.Count;bytes=($files | Measure-Object Length -Sum).Sum})
}
$selected | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cleanup-plan.json') -Encoding utf8
if (-not $Apply) { Write-Output "Planned targets: $($selected.Count)"; exit 0 }
$verification = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'artifacts.json') -Raw | ConvertFrom-Json
if ($verification.status -ne 'passed' -or -not $verification.portable.payload_verified) { throw 'Package has not passed verification' }
if ((Get-FileHash -LiteralPath $verification.portable.path -Algorithm SHA256).Hash -ne $verification.portable.sha256) { throw 'ZIP changed since verification' }
$results = [Collections.Generic.List[object]]::new()
foreach ($target in $selected) {
    try {
        Remove-Item -LiteralPath $target.path -Recurse -Force -ErrorAction Stop
        $results.Add([pscustomobject]@{path=$target.path;status='removed';bytes=$target.bytes;error=$null})
    } catch {
        $results.Add([pscustomobject]@{path=$target.path;status='failed';bytes=0;error=$_.Exception.Message})
    }
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cleanup-result.json') -Encoding utf8
$removed = @($results | Where-Object status -eq 'removed')
$failed = @($results | Where-Object status -eq 'failed')
Write-Output "Removed=$($removed.Count); failed=$($failed.Count); MiB=$([math]::Round(($removed | Measure-Object bytes -Sum).Sum / 1MB, 1))"
if ($failed.Count) { exit 1 }
