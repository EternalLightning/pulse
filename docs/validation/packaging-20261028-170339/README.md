# 1.2.1.1 打包与安装版替换（2026-10-28）

按用户“替换安装版，打包安装包”的请求完成。版本保持 `1.2.1.1`，包含本次上游功能、性能适配及 OneDrive 重复入口修复。保留工作区已有修改，没有提交或发布。

## 交付产物

| 产物 | 大小 / 字节 | 验证 |
| --- | ---: | --- |
| [安装包](../../../dist/PulseSetup-1.2.1.1.exe) | 10,445,939 | Inno Setup 6.7.3 编译成功，记录 SHA256；没有执行安装向导或做干净安装测试。 |
| [便携包](../../../dist/Pulse-1.2.1.1-portable-win-x64.zip) | 12,938,805 | 41 个载荷逐项校验通过，版本、原始文件名、路径和内容检查无错误；作为本次安装版替换的干净载荷来源。 |

安装包 SHA256：`f97e68af7428c7bba102526dc415f2c1f1fb2be2e05a2572858f99fe1f35d4f7`。两个产物旁均有 `.sha256` 文件。旧的同名产物及校验文件已先复制到本目录的 `previous-artifacts/` 并核对哈希，没有直接丢弃。

只构建 `pulse`、`pulse_index`、`pulse_document`、`pulse_preview`、`pulse_shell`、`pulse_integration` 六个生产目标，增量构建无需再次编译。没有全量构建或重跑历史测试。[发布载荷检查](artifacts.json) 的便携载荷状态为通过，安装包状态仅为身份已记录，不能将其解释为已完成安装包实装验证。

静态运行时检查通过，六个程序和相关 DLL 没有需要额外打包的 MSVC 动态运行库；主程序不含 selftest dispatcher。见 [生产构建](build-production.log)、[运行库检查](runtime-check.log)、[便携打包](portable-build.log)、[安装包编译](installer-build.log)。Inno 保留一条 `PrivilegesRequired=admin` 配合 HKCU 区域的警告，没有为本次打包修改安装器策略。

## 安装版替换

通过当前运行进程和服务核实安装路径为 `D:\Applications\Pulse`。用户确认退出后再次检查，界面及其原 Shell/预览进程已退出。维护流程使用 [pulse-installed-update 技能](</E:/AppData/.codex/skills/pulse-installed-update/SKILL.md>) 和仓库 `scripts/replace_installed.ps1`。

[替换前计划](installed-plan.json) 为 41 个载荷、6 个变化文件：`Pulse.exe`、`Pulse.Index.exe`、`Pulse.Document.exe`、`Pulse.Preview.exe`、`Pulse.Shell.exe`、`Pulse.Integration.exe`。

索引程序本次也变化，先通过 UAC 提权并正常停止 `PulseIndex` 服务，再按安装路径及完整 `--network-agent` 命令行精确停止 PID 33396 的旧网络代理。没有终止用户界面。调用维护脚本进行备份、备份哈希核对、变化文件替换及全部载荷哈希核对，然后恢复索引服务。

[替换结果](installed-apply.json)：**Changed=6，Verified=41，Success=true**。服务在流程前后均为 **Running / Auto**，路径相同；生命周期详见 [结果](installed-lifecycle-result.json) 和 [记录](installed-update-transcript.log)。维护脚本内部的 RuntimePreserved 指其执行时已停止的服务状态未变化；原运行状态由外围流程单独恢复和核对。

回滚备份：

`E:\Projects\pulse\dist\installed-backups\20261028-170821-bac0840464994db990d5b8be1b1739ac`

其中 `manifest.json`、`payload/`、`result.json` 保留用于回滚。替换只写入计划中的六个程序，不操作个人配置、索引数据、登录状态或卸载文件。

## 安装后的定向检查

- 从实际安装目录运行，使用随机独立配置和关闭的测试缩略图缓存；离屏绘制主页，退出码为 0。[截图](installed-home.png) 已人工检查，侧栏可见两个云账户入口，主页布局没有新增重叠或缺失。
- 检查与恢复均使用普通用户令牌，`elevated=false`。用户已重新打开安装版，因此复用现有 PID 20820 及有效主窗口，不再启动多余窗口。后台网络代理 PID 39752 来自新的安装版主程序。见 [安装后检查](installed-smoke.json)。
- 最终再次执行只读替换计划，**41 个文件、Changed=0**，确认安装目录与本次载荷全部一致；索引服务仍为 Running / Auto。见 [最终核对](installed-final-plan.json)。

初次检查脚本要求新启动的 PID 必须驻留，遇到启动进程以 0 退出而报错。随后只读检查发现安装版已有有效窗口及 Shell/网络代理；修正检查脚本以复用既有实例，并重跑上述定向检查。保留 [初次诊断](installed-smoke-initial.log)，不将其解释为产品崩溃。

本次没有再次运行此前的功能断言。前序 [功能验证](../upstream-features-20261028/README.md) 与 [性能及去重验证](../performance-adaptation-20261028/README.md) 分别记录已完成的 251 项与 243 项断言，以及真实 SMB、手机、云同步服务和性能基准的未验收边界。本次安装目录的启动检查不替代这些实机功能验收。
