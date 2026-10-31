# 构建与打包指南

本指南依据仓库当前脚本维护，适用于 Windows 10 / 11 x64。以下命令均从仓库根目录运行，打包采用当前工作区源码，包括尚未提交的修改。版本统一读取 `version.txt`，构建与打包不会自动递增版本。

## 环境与目录

- Visual Studio 2022 v143 C++ 工具及 Windows SDK；本地与 CI 均通过 `scripts/vcvars.bat` 选择 VS2022 并初始化 x64 环境。
- 项目使用 `scripts/build-tools.lock.json` 固定 CMake 3.31.10 和 Ninja 1.13.2。首次构建自动下载至 `.build-tools` 并验证 SHA256，不依赖 PATH 中其他软件附带的版本；后续可复用已验证的本地工具。
- PowerShell 7.2+（`pwsh`）用于便携包、产物校验及 CI；安装批处理内部还调用系统 Windows PowerShell。
- Inno Setup 6 用于编译安装包。`build_installer.bat` 先查找 PATH 中的 `ISCC.exe`，再查找 Program Files 和当前用户的标准安装位置。
- 保留仓库中的第三方二进制及许可证：`third_party/lumatext`、`third_party/pdfium`、`third_party/7zip` 等由构建或打包脚本使用。

`build-release` 是本地生产构建目录，`build-test` 用于独立测试配置，`build-ci` 是 CI 默认目录。`scripts/build.ps1` 为每个目录持有独占构建锁；同目录构建串行执行，不同目录可以并行。工具和临时文件放在 `.build-tools`，避免构建目录清理后工具失效，也不继承全局临时目录和隐藏的 CL/LINK 参数。`dist` 存放交付物和便携包暂存目录，`docs/validation` 存放按日期区分的验证记录。

## 生产构建

```powershell
.\build_release.bat
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
```

脚本配置 Ninja / Release、`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`、`PULSE_WITH_SELFTEST=OFF`，以并行度 4 构建 `pulse` 及其五个生产依赖，得到六个程序：

| CMake 目标 | 交付文件 | 用途 |
| --- | --- | --- |
| `pulse` | `Pulse.exe` | 主程序 |
| `pulse_index` | `Pulse.Index.exe` | 索引主机及服务 |
| `pulse_document` | `Pulse.Document.exe` | 文档正文提取 |
| `pulse_preview` | `Pulse.Preview.exe` | 预览主机 |
| `pulse_shell` | `Pulse.Shell.exe` | Shell 主机 |
| `pulse_integration` | `Pulse.Integration.exe` | 系统右键菜单集成辅助程序 |

构建步骤会准备相关 DLL、`7zip` 和许可证。脚本只构建交付目标及其依赖，不运行功能测试，也不生成安装包或 ZIP。

两个批处理均用第二个参数指定目录：

```powershell
.\build_release.bat "" build-custom
.\build_release.bat /clean build-custom
.\build_installer.bat /skipbuild build-custom
```

`/clean` 会在取得该目录构建锁后删除并重建指定构建目录，包含其中的旧二进制，请先保存需要的产物。清理目标必须在仓库内的 `build*` 或 `out` 目录下，且不能经过重解析点。普通配置不会将其他生成器的缓存静默转换为 Ninja。

## 日常增量构建与定向验证

从 PowerShell 使用统一入口，仅构建本次受影响目标；脚本负责初始化 VS2022 x64、固定工具和构建锁：

```powershell
& .\scripts\build.ps1 -BuildDir build-release -Targets pulse
# 若本次只涉及文档提取模块，可单独构建
& .\scripts\build.ps1 -BuildDir build-release -Targets pulse_document
```

`pulse` 会构建索引、Shell、预览、文档提取和集成依赖；其中 `pulse_document` 的依赖关系由 CMake 的 `foreach(document_target ...)` 声明。生产交付前使用 `build_release.bat` 更新完整交付目标。

需要测试时显式构建相关测试目标，按该目标提供的筛选选项运行；不要把无目标的 `cmake --build` 或 CI 全流程当作默认检查。内嵌自测仅在专用目录配置 `PULSE_WITH_SELFTEST=ON` 后可用，运行时应选择与本次改动相关的用例并等待进程退出。此类程序不能交付，生产包需要重新以 `OFF` 构建。

`build_d2d.bat` 使用同一工具入口和 Ninja，在独立 `build-d2d` 目录构建 `pulse_d2d_list`。

测试配置示例：`& .\scripts\build.ps1 -BuildDir build-test -Configure -ArchitectureFixtures -Targets pulse_pane_animation_application_test`。内嵌自测另外显式指定 `-EnableSelfTests`。`-CMakeArgs` 支持显式指定 LumaText 源码树或 SDK；脚本不再自动借用相邻项目的 `_deps` 缓存。

直接运行 CMake/Ninja 会绕过构建锁，避免同时对同一目录使用直接命令和统一入口。环境专项检查：`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_build_environment.ps1`。

## 一次构建生成安装包和便携包

若要保留旧的同版本文件，应先备份 `dist` 中的同名安装包、ZIP 和校验文件。脚本会覆盖同名产物，不自动备份；保持同一个版本号不代表文件内容相同。

在 PowerShell 中按顺序运行，并检查每一步的退出码：

```powershell
.\build_installer.bat
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }

pwsh -NoProfile -File .\scripts\package_portable.ps1 -BuildDir build-release
if ($LASTEXITCODE -ne 0) { throw 'Portable packaging failed' }
```

安装脚本会调用一次生产构建，再检查六个 EXE 的 FileVersion、ProductVersion、OriginalFilename 和主程序内嵌自测标记；随后扫描运行库依赖，最后调用 Inno Setup。

若刚完成同一份源码的构建，可改用：

```powershell
.\build_installer.bat /skipbuild build-release
```

`/skipbuild` 仍执行载荷和运行库检查，但不会判断源码是否比 EXE 更新。便携包脚本同样只使用现有文件，执行版本、自测标记和必需文件检查后创建 ZIP，不编译代码或运行功能测试。

输出为：

| 产物 | 路径 |
| --- | --- |
| 安装包 | `dist/PulseSetup-<version>.exe` |
| 便携包 | `dist/Pulse-<version>-portable-win-x64.zip` |
| 便携包暂存载荷 | `dist/portable-stage-<guid>/Pulse-<version>-portable/` |

暂存目录会保留。便携包及安装包包括六个程序、`lumatext.dll`、`pdfium.dll`、`7zip`、LICENSE、NOTICE、THIRD_PARTY_NOTICES.md 和第三方许可证；ZIP 另生成 README.txt。包内文件数以实际许可证树为准，不固定为某个历史验证记录的数量。

许可打包修改（EternalLightning，2026-10-30）：两种打包路径均从仓库直接补齐 7-Zip 的完整许可文本及 `7zip/7z2603-src.7z` 对应源码，避免复用旧构建目录时漏带新增许可材料。工具栏图标、d2d-mica 适配代码、SQLite 和拼音数据的许可或来源说明也随包分发，清单见[第三方声明](../THIRD_PARTY_NOTICES.md)。更新 7-Zip 二进制时，必须同时更新源码归档、校验值和脚本中的固定版本文件名。

## 产物校验与 SHA-256

```powershell
$version = (Get-Content .\version.txt -Raw).Trim()
$record = "docs/validation/packaging-$(Get-Date -Format yyyyMMdd-HHmmss)"
pwsh -NoProfile -File .\scripts\verify_release_artifacts.ps1 `
    -BuildDir build-release `
    -ZipPath "dist/Pulse-$version-portable-win-x64.zip" `
    -InstallerPath "dist/PulseSetup-$version.exe" `
    -OutputJson "$record/artifacts.json"
if ($LASTEXITCODE -ne 0) { throw 'Artifact verification failed' }
```

此脚本检查 ZIP 路径、重复或冲突条目、缺失或多余文件、测试/构建文件混入情况，将每个载荷的大小和 SHA-256 与构建目录或仓库资源比对，并检查 EXE 元数据、自测标记和生成的 README。安装包只记录文件身份，不解包比对，也不测试安装流程。

`-BuildDir` 必须指向实际打包目录。此校验脚本默认值仍为 `build`，因此示例显式传入 `build-release`。如校验历史产物，可指定 `-ExpectedVersion`，同时提供该版本对应的构建文件和资源；仅修改预期版本不能验证旧包内容。

本地打包和校验脚本不自动生成 `.sha256` 旁文件。需要时在最终签名和打包完成后运行：

```powershell
$version = (Get-Content .\version.txt -Raw).Trim()
foreach ($name in @("PulseSetup-$version.exe", "Pulse-$version-portable-win-x64.zip")) {
    $path = Join-Path dist $name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name" | Set-Content -LiteralPath "$path.sha256" -Encoding ascii
}
```

功能验证另行记录：可从解压后的便携包做隔离启动和相关操作检查；启动退出码、截图视觉检查、安装向导、UAC、服务注册及升级分别说明实际覆盖范围。黑屏截图或生成截图文件本身不能证明界面正常。

## LumaText 与运行库

本地脚本优先使用 `third_party/lumatext` 中的预编译包；显式设置环境变量 `LUMATEXT_SOURCE_DIR` 时改为构建对应源码。两者均不存在时，脚本还会尝试相邻的 `../lumatext` 源码目录。纯开发配置可回退到 DirectWrite，但交付包仍要求 LumaText DLL 和许可证齐全。

CI 用 `scripts/verify_lumatext_sdk.ps1` 校验 `cmake/lumatext-sdk.json` 固定的完整 SDK。本地 Release 脚本不会自动执行该固定校验。安装批处理通过 `stage_installer_runtime.ps1 -RequireStaticRuntime` 扫描六个程序及两个 DLL 的普通和延迟导入，拒绝 MSVC 动态运行库依赖。便携包脚本不会自动做这项运行库扫描，单独打 ZIP 时可先运行：

```powershell
pwsh -NoProfile -File .\scripts\stage_installer_runtime.ps1 `
    -BuildDir build-release -RequireStaticRuntime
if ($LASTEXITCODE -ne 0) { throw 'Runtime dependency check failed' }
```

扫描生成构建目录内的 `installer-runtime-manifest.json`。`/MT` 表示 MSVC 运行库静态链接，LumaText、PDFium 和 7-Zip 的独立 DLL 仍须随包交付。SDK 更新及旧 `/MD` 构建处理见[静态运行库与 SDK](static-runtime-packaging.md)。

## 安装、替换与发布

安装包运行时需要管理员权限；可选择安装并启动 PulseIndex 后台索引服务。便携包解压后运行 `Pulse.exe`，不会注册服务；用户配置、缓存和日志仍位于用户应用数据目录。

打包不会执行安装或替换已安装文件。维护现有安装的载荷替换由 `scripts/replace_installed.ps1` 单独完成，默认只规划，`-Apply` 才执行；它不负责停启进程和服务，也不替代安装向导验证。具体边界见[安装更新说明](../scripts/skills/pulse-installed-update/SKILL.md)。

CI、签名、符号归档与 GitHub Release 上传见[发布流程](../scripts/RELEASE.md)。最新实际验证结果见[验证索引](validation/README.md)，历史记录中的构建目录、文件数和已通过检查不作为当前默认值或验收结论。
