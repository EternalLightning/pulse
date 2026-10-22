param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$repo = 'E:\Projects\pulse'
$audit = Get-Content -LiteralPath "$PSScriptRoot\installer-audit.json" -Raw | ConvertFrom-Json
$portable = Get-Content -LiteralPath "$PSScriptRoot\..\portable-1.2.0.0-2026-10-23\package-audit.json" -Raw | ConvertFrom-Json
$currentStage = Split-Path -Parent $portable.Payload
$keep = @($audit.Installer, ($audit.Installer + '.sha256'), $audit.PortableArchive, ($audit.PortableArchive + '.sha256'), $currentStage, $audit.RetainedRollback)
$protected = @(git -C $repo ls-files --cached --others --exclude-standard | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $repo $_)) })
if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory source files' }
$candidates = [Collections.Generic.List[object]]::new()
function AddCandidate([string]$path, [string]$reason, [string]$archive = '') {
    $full = [IO.Path]::GetFullPath($path)
    if (-not $full.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Outside workspace: $full" }
    $relative = [IO.Path]::GetRelativePath($repo, $full)
    if ($relative -notmatch '^(build-raidrive-fix($|\\)|bench_data\\|dist\\|build\\)' -and $relative -notmatch '^[^\\]+\.log$') { throw "Unapproved cleanup root: $relative" }
    if (-not (Test-Path -LiteralPath $full)) { return }
    $item = Get-Item -LiteralPath $full -Force
    foreach ($retained in $keep) {
        if ($full -eq $retained -or $retained.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) -or $full.StartsWith($retained + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Protected release item: $relative" }
    }
    if (@($protected | Where-Object { $_ -eq $full -or $_.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) }).Count) { throw "Source or uncommitted file in cleanup: $relative" }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($item.PSIsContainer -and @(Get-ChildItem -LiteralPath $full -Recurse -Force -Attributes ReparsePoint).Count)) { throw "Reparse point in cleanup: $relative" }
    $files = if ($item.PSIsContainer) { @(Get-ChildItem -LiteralPath $full -File -Recurse -Force) } else { @($item) }
    $manifest = @($files | ForEach-Object { [pscustomobject]@{Path=[IO.Path]::GetRelativePath($repo,$_.FullName);Length=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash} })
    $candidates.Add([pscustomobject]@{Path=$full;RelativePath=$relative;Directory=$item.PSIsContainer;Reason=$reason;Archive=$archive;Files=$manifest;Count=$files.Count;Bytes=($files | Measure-Object Length -Sum).Sum})
}
if (-not $Apply) {
    AddCandidate "$repo\build-raidrive-fix" 'Superseded secondary build'
    Get-ChildItem -LiteralPath "$repo\dist" -Directory | ForEach-Object {
        if ($_.FullName -eq $currentStage -or $_.FullName -eq $audit.RetainedRollback) { return }
        if ($_.Name -like 'portable-stage-*') { AddCandidate $_.FullName 'Obsolete portable staging tree' }
        elseif ($_.Name -like 'rollback-before-*') { AddCandidate $_.FullName 'Superseded rollback; newest verified rollback retained' }
        elseif ($_.Name -eq 'operation-center-replacement-20261022-213051') { AddCandidate $_.FullName 'Obsolete deployment staging and older rollback' 'deployment-records' }
    }
    Get-ChildItem -LiteralPath "$repo\dist" -File | Where-Object { $_.Name -match '^Pulse(?:Setup-|-).*\.(exe|zip)$' -and $_.FullName -notin $keep } | ForEach-Object { AddCandidate $_.FullName 'Obsolete release package' }
    $generated = @('architecture-deployment-*','architecture-ops-*','batch-conflict-dialog','context-menu-loading','context-menu-order-installed-profile','context-menu-order-profile','context-menu-timeout-baseline','context-menu-timeout-current','fluent_nested_20261022','operation-center-application','operation-center-motion-before','operation-center-motion-installed-profile','operation-center-refinement-before','operation-center-smoke','operation-center-ui','operation-records-check','portable-1.2.0.0-isolated-profile','portable-1.2.0.0-smoke-*','shell-menu-compact','ui-motion-build')
    Get-ChildItem -LiteralPath "$repo\bench_data" -Directory | ForEach-Object {
        $directory = $_
        if (@($generated | Where-Object { $directory.Name -like $_ }).Count) { AddCandidate $directory.FullName 'Reproducible test output, temporary profile, build or source snapshot' }
    }
    Get-ChildItem -LiteralPath "$repo\bench_data" -File | Where-Object Extension -In @('.exe','.obj') | ForEach-Object { AddCandidate $_.FullName 'Standalone diagnostic binary or object' }
    Get-ChildItem -LiteralPath "$repo\bench_data" -File -Filter '*.log' | ForEach-Object { AddCandidate $_.FullName 'Consolidated previous diagnostic log' 'bench-logs' }
    Get-ChildItem -LiteralPath $repo -File -Filter '*.log' | ForEach-Object { AddCandidate $_.FullName 'Consolidated previous build or test log' 'root-logs' }
    Get-ChildItem -LiteralPath "$repo\build" -File | Where-Object { $_.Name -match '^pulse_.*(test|bench|perf|stress).*\.exe$' } | ForEach-Object { AddCandidate $_.FullName 'Rebuildable test executable' }
    Get-ChildItem -LiteralPath "$repo\build\CMakeFiles" -Directory | Where-Object { $_.Name -match '^pulse_.*(test|bench|perf|stress).*\.dir$' } | ForEach-Object {
        Get-ChildItem -LiteralPath $_.FullName -Recurse -File | Where-Object Extension -In @('.obj','.o','.pdb','.ilk','.idb','.iobj','.ipdb') | ForEach-Object { AddCandidate $_.FullName 'Rebuildable test object; target metadata retained' }
    }
    $plan = [pscustomobject]@{Time=(Get-Date).ToString('o');Retained=$keep;Targets=@($candidates);TargetCount=$candidates.Count;FileCount=($candidates | Measure-Object Count -Sum).Sum;Bytes=($candidates | Measure-Object Bytes -Sum).Sum;ProtectedSourceFileCount=$protected.Count}
    $plan | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath "$PSScriptRoot\cleanup-plan.json" -Encoding utf8
} else {
    $plan = Get-Content -LiteralPath "$PSScriptRoot\cleanup-plan.json" -Raw | ConvertFrom-Json
    $candidates = @($plan.Targets)
}
[pscustomobject]@{Targets=$plan.TargetCount;Files=$plan.FileCount;MiB=[math]::Round($plan.Bytes/1MB,2);Apply=$Apply.IsPresent} | ConvertTo-Json
if (-not $Apply) { exit }
if ((Get-FileHash -LiteralPath $audit.Installer -Algorithm SHA256).Hash -ne $audit.SHA256 -or (Get-FileHash -LiteralPath $audit.PortableArchive -Algorithm SHA256).Hash -ne $audit.PortableSHA256) { throw 'Release package changed before cleanup' }
$completed = [Collections.Generic.List[string]]::new()
try {
    foreach ($target in $candidates) {
        $full = [IO.Path]::GetFullPath($target.Path)
        $relative = [IO.Path]::GetRelativePath($repo, $full)
        if (-not $full.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase) -or
            ($relative -notmatch '^(build-raidrive-fix($|\\)|bench_data\\|dist\\|build\\)' -and $relative -notmatch '^[^\\]+\.log$')) { throw 'Unsafe cleanup target' }
        foreach ($retained in $keep) {
            if ($full -eq $retained -or $retained.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) -or $full.StartsWith($retained + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Protected release target' }
        }
        if (@($protected | Where-Object { $_ -eq $full -or $_.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) }).Count) { throw 'Source file in cleanup target' }
        $item = Get-Item -LiteralPath $full -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($target.Directory -and @(Get-ChildItem -LiteralPath $full -Recurse -Force -Attributes ReparsePoint).Count)) { throw 'Cleanup target became a reparse point' }
        if ($target.Directory -and @(Get-ChildItem -LiteralPath $full -File -Recurse -Force).Count -ne $target.Files.Count) { throw 'Cleanup directory contents changed' }
        foreach ($file in $target.Files) {
            if ((Get-FileHash -LiteralPath (Join-Path $repo $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Cleanup target changed: $($file.Path)" }
        }
        if ($target.Archive) {
            $destination = Join-Path $PSScriptRoot ('archived-records\' + $target.Archive)
            if (-not ([IO.Path]::GetFullPath($destination)).StartsWith($PSScriptRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe archive destination' }
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            $records = if ($target.Directory) { @(Get-ChildItem -LiteralPath $target.Path -File) } else { @(Get-Item -LiteralPath $target.Path) }
            foreach ($record in $records) {
                $copy = Join-Path $destination $record.Name
                Copy-Item -LiteralPath $record.FullName -Destination $copy
                if ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $record.FullName -Algorithm SHA256).Hash) { throw 'Diagnostic archive hash mismatch' }
            }
        }
        # All absolute targets and their contents were bounded and inventoried above.
        Remove-Item -LiteralPath $target.Path -Recurse -Force
        if (Test-Path -LiteralPath $target.Path) { throw "Cleanup incomplete: $($target.RelativePath)" }
        $completed.Add($target.RelativePath)
    }
} finally {
    [pscustomobject]@{Time=(Get-Date).ToString('o');Completed=@($completed);CompletedTargets=$completed.Count;PlannedTargets=$plan.TargetCount;Files=$plan.FileCount;Bytes=$plan.Bytes;AllCompleted=($completed.Count -eq $plan.TargetCount)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$PSScriptRoot\cleanup-result.json" -Encoding utf8
}
'Cleanup completed; manifests and archived diagnostic records retained.'
