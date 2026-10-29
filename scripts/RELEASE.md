# 发布流程

本地构建、安装包和便携包的完整命令见[构建与打包指南](../docs/build-and-package.md)。以下说明 CI、签名、符号归档和 GitHub Release 的实际行为；流程定义以仓库当前脚本为准。

## 本地交付入口

在仓库根目录依次运行，每一步成功后再继续：

```powershell
.\build_installer.bat
pwsh -NoProfile -File .\scripts\package_portable.ps1 -BuildDir build-release
```

安装脚本包含一次生产构建；已有同一源码的构建时可用 `.\build_installer.bat /skipbuild build-release`。版本取自 `version.txt`，输出为 `dist/PulseSetup-<version>.exe` 和 `dist/Pulse-<version>-portable-win-x64.zip`。这两个入口不签名、不执行安装，也不上传或发布。同版本产物会被覆盖，需要保留时先备份。

`scripts/check_release_payload.ps1` 检查六个 EXE 的版本、原始文件名和内嵌自测标记。`scripts/verify_release_artifacts.ps1` 比对便携包内容并记录安装包身份；显式传入实际 `-BuildDir`，其默认值仍为 `build`。校验报告不代表安装向导、服务注册或实际界面已经验收。

## CI 构建与回归

`scripts/build_release_ci.ps1` 使用 PowerShell 7.2+、Visual Studio 2022 v143、Ninja 和 Inno Setup 6，默认构建目录为 `build-ci`，且要求位于仓库内。它先校验固定的 LumaText SDK，再启用内嵌自测构建并运行脚本列出的回归用例，最后关闭内嵌自测、重建主程序并编译安装包。`pulse` 的依赖会带上五个辅助程序，文档提取依赖通过 CMake 的 `foreach(document_target ...)` 声明。测试清单以脚本为准，不在此文档复制。

需要运行这一回归流程时，在仓库根目录执行：

```powershell
pwsh -NoProfile -File .\scripts\build_release_ci.ps1 -BuildDir build-ci
```

当前限制：

- CI 不调用 `stage_installer_runtime.ps1 -RequireStaticRuntime`；本地安装批处理才包含该扫描。SDK 固定校验不能替代所有交付文件的运行库扫描。
- CI 只生成安装包，不生成便携 ZIP、`.sha256` 文件、签名或符号归档。

本次文档更新没有修改这些脚本，也没有重新运行 CI；流程说明不代表当前版本的测试已经通过。

## GitHub Actions 与发布

[release.yml](../.github/workflows/release.yml) 由 `v[0-9]*` 标签推送或手动触发，要求运行引用严格为 `refs/tags/v<version.txt>`；手动运行也应选择对应标签。构建作业运行 CI 脚本并上传 `installer-windows`，另保存可用的诊断日志。

发布作业下载安装包后调用 `scripts/publish_release.ps1`。该脚本固定操作 `EternalLightning/pulse`，需要已认证且有写权限的 GitHub CLI（工作流使用 `GH_TOKEN`）：

1. 检查安装包，查找本地便携 ZIP；缺失时从同版本 Release 草稿下载该 ZIP。
2. 读取 `docs/releases/<version>.md` 生成说明；不存在时使用脚本内的默认说明，写入 `dist/release-notes.md`。
3. 创建或更新草稿，将安装包和便携 ZIP 上传（同名附件使用 `--clobber`）。
4. 将草稿公开发布并标记为 latest。若该版本已公开，脚本拒绝继续，需使用新版本。

工作流不生成或传递便携 ZIP，所以在发布作业运行前，必须已经向匹配标签的草稿上传当前版本 ZIP；仅在本地生成 ZIP 不会将其传给发布作业。

本地直接调用 `publish_release.ps1` 会执行上传并公开发布，不是预览命令；它不会重新构建或验证 ZIP 与安装包是否来自同一批文件。发布前应完成相关功能检查、产物比对和说明整理。

## 可选签名

`scripts/package_release.ps1` 的原意是使用证书指纹和 HTTPS 时间戳服务完成 Authenticode 签名，但当前仍将签名、构建 ID 和符号来源固定为 `build`，调用的构建和安装脚本却默认使用 `build-release`。它可能读取缺失或过时文件，不能直接当作当前可用的签名发布入口。

有签名需求时，在配置好 `signtool.exe`、代码签名证书及时间戳服务后，对已确认的同一构建目录执行：签署六个生产 EXE → 用该目录 `/skipbuild` 编译安装器 → 签署安装器 → 从已签名的构建目录生成便携包 → 重新校验最终产物并生成 SHA-256。签名会改变文件哈希，旧校验文件必须更新。修复签名脚本的目录一致性后才能改回自动入口。

## 可选符号归档

`scripts/archive_symbols.ps1` 默认读取 `build`，应显式指定实际目录，例如：

```powershell
pwsh -NoProfile -File .\scripts\archive_symbols.ps1 -BuildDir build-release
```

它要求 `pulse_build_id.txt`、六个生产 EXE 及各自同名 PDB，缺少任一文件都会失败；输出为 `dist/symbols/<version>/<build-id>/`，附带大小和哈希清单。普通 Release 构建未显式配置生成 PDB，不能假定满足归档前提。符号需在同一源码和配置的构建中生成并与交付 EXE 配套，不能混用其他目录中的旧 PDB。
