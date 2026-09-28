#requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$version = (Get-Content version.txt -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version' }
$tag = "v$version"
$repository = 'jimmgreen/pulse'
$base = "https://github.com/$repository/releases/download/$tag"
$normal = "PulseSetup-$version.exe"
$win81 = "PulseSetup-$version-win81.exe"
$portable = "Pulse-$version-portable-win-x64.zip"
foreach ($file in @($normal, $win81)) {
    if (-not (Test-Path -LiteralPath "dist/$file")) { throw "Missing installer: $file" }
}
# A portable archive may be staged locally or uploaded to the draft before tagging.
if (-not (Test-Path -LiteralPath "dist/$portable")) {
    & gh release download $tag --repo $repository --pattern $portable --dir dist
    if ($LASTEXITCODE -ne 0) { throw 'Upload the portable ZIP to the release draft before publishing' }
}
$changesPath = "docs/releases/$version.md"
$changes = if (Test-Path $changesPath) { Get-Content $changesPath -Raw } else { '修复问题并改进使用体验。' }
$notesPath = 'dist/release-notes.md'
@"
# Pulse $version

## 下载安装

请按你的 Windows 版本选择安装包，两者均为 **64 位（x64）**。

| 系统 | 安装包 |
| --- | --- |
| **Windows 10 / Windows 11** | [$normal]($base/$normal) |
| **Windows 8.1 兼容版** | [$win81]($base/$win81) |
| **Windows 10 / 11 免安装版** | [$portable]($base/$portable) |

两个版本功能一致。Windows 8.1 上不可用的系统视觉效果会使用兼容显示方式。

免安装版解压后运行 pulse.exe；配置和缓存仍保存在当前用户的应用数据目录。ZIP 不会安装索引服务；需要全盘后台索引服务时请使用安装包。免安装版升级请下载新版 ZIP。

## 本次更新

$changes

请从上方链接手动下载适合当前 Windows 版本的安装包。
"@ | Set-Content -LiteralPath $notesPath -Encoding utf8
$existing = & gh release view $tag --repo $repository --json isDraft 2>$null
if ($LASTEXITCODE -eq 0) {
    if (-not ($existing | ConvertFrom-Json).isDraft) { throw 'This version is already published; increment version.txt' }
    & gh release edit $tag --repo $repository --title "Pulse $version" --notes-file $notesPath
} else {
    & gh release create $tag --repo $repository --verify-tag --draft --title "Pulse $version" --notes-file $notesPath
}
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the release draft' }
& gh release upload $tag --repo $repository "dist/$normal" "dist/$win81" "dist/$portable" --clobber
if ($LASTEXITCODE -ne 0) { throw 'Could not upload release assets; release remains a draft' }
& gh release edit $tag --repo $repository --draft=false --latest
if ($LASTEXITCODE -ne 0) { throw 'Could not publish the release' }
