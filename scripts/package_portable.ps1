#requires -Version 7.2
param([string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$version = (Get-Content (Join-Path $repo 'version.txt') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw 'Invalid version' }
& "$PSScriptRoot/check_release_payload.ps1" -BuildDir $build
$files = @('pulse.exe', 'Pulse.Index.exe', 'Pulse.Document.exe', 'Pulse.Preview.exe',
    'pulse_shell.exe', 'lumatext.dll', 'pdfium.dll')
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $file))) { throw "Missing portable dependency: $file" }
}
$dist = Join-Path $repo 'dist'
$stage = Join-Path $dist ("portable-stage-" + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage "Pulse-$version-portable"
New-Item -ItemType Directory -Path $payload -Force | Out-Null
foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $build $file) -Destination $payload }
$archiveTools = Join-Path $build '7zip'
foreach ($file in @('7z.exe', '7z.dll', 'License.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $archiveTools $file))) { throw "Missing archive dependency: $file" }
}
Copy-Item -LiteralPath $archiveTools -Destination (Join-Path $payload '7zip') -Recurse
foreach ($license in @('PDFium', 'LumaText')) {
    $source = Join-Path $build "licenses/$license"
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing licenses: $source" }
    New-Item -ItemType Directory -Path (Join-Path $payload "licenses/$license") -Force | Out-Null
    Copy-Item -Path "$source/*" -Destination (Join-Path $payload "licenses/$license") -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $payload 'licenses/ib-pinyin') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party/ib-pinyin-cpp/LICENSE.txt') -Destination (Join-Path $payload 'licenses/ib-pinyin')
New-Item -ItemType Directory -Path (Join-Path $payload 'licenses/FilesIcon') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party/files-icon/LICENSE-MIT.txt') -Destination (Join-Path $payload 'licenses/FilesIcon')
Copy-Item -LiteralPath (Join-Path $repo 'third_party/files-icon/README.md') -Destination (Join-Path $payload 'licenses/FilesIcon')
@"
Pulse $version — Windows 10 / 11 x64 免安装版

解压整个文件夹后运行 pulse.exe。请保留同目录的辅助程序、DLL、7zip 和 licenses 文件夹。
此版本无需安装，但配置、缓存和日志仍保存在当前用户的应用数据目录，并非全部数据随程序目录携带。
解压不会注册 PulseIndex 系统服务。需要全盘后台索引服务时，请使用安装版。
如需升级免安装版，请下载新版 portable ZIP 并解压到新目录。

Extract the complete folder and run pulse.exe. Keep all helper programs, DLLs, 7zip and licenses together.
Settings, caches and logs remain in the current user's application-data directory.
Extraction does not register the PulseIndex service. Use the installer for the system indexing service.
To upgrade the portable edition, download a new portable ZIP and extract it to a new directory.
"@ | Set-Content -LiteralPath (Join-Path $payload 'README.txt') -Encoding utf8
$archive = Join-Path $dist "Pulse-$version-portable-win-x64.zip"
Compress-Archive -LiteralPath $payload -DestinationPath $archive -Force
Write-Output $archive
