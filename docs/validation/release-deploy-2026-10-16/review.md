# Pulse 1.1.2.0 打包、部署与清理（2026-10-16）

用户要求将当前程序复制到 `D:\Applications\Pulse`、打包、清理过往构建并提交全部代码改动。保留版本号 1.1.2.0，构建标识更新为 `20261016T113115Z-7CB90C40`。

## 构建及包验证

配置 Release、`PULSE_WITH_SELFTEST=OFF`，只构建五个生产目标 `pulse`、`pulse_index`、`pulse_document`、`pulse_preview`、`pulse_shell`，退出 0，无新增编译警告。构建前五目标已是最新；新构建标识触发元数据及直接依赖的增量重编，未修改产品逻辑。

执行现有 `scripts/package_portable.ps1 -BuildDir build` 和 payload checker。逐项比较暂存文件和 ZIP 条目：40/40 文件大小及 SHA-256 相同；五个 EXE 的 FileVersion/ProductVersion 为 1.1.2.0，OriginalFilename 匹配；不含内嵌 selftest 分发器或测试/开发中间产物。五个 PE 依赖检查无未随包提供的 VCRUNTIME/MSVCP/CONCRT。

- 包：`E:\Projects\pulse\dist\Pulse-1.1.2.0-portable-win-x64.zip`
- 大小：12,133,434 字节。
- SHA-256：`C739C08EFF9E641E0F900E0886850B5E47D1C198FCB23628B5BBD65C8497D7FC`。
- 清单：[package-verification.json](package-verification.json)、[payload-manifest.json](payload-manifest.json)。

暂存目录与安装目录分别使用 `--test-instance --shot`、独立 `PULSE_TEST_DATA_DIR` 和隔离文件夹启动，正常生成截图并退出 0。已查看两张截图，基本目录列表、中英文文件名、工具栏及状态栏正常显示；这是启动冒烟检查，不是完整 UI 验收。见 [暂存截图](staged-smoke.png)、[安装目录截图](deployed-smoke.png) 及两份 `*-smoke.json`。

本次沿用前一轮 [审查定向验证](../code-audit-closure-2026-10-16.md) 的 690 PASS / 0 FAIL，不为打包重复运行整个测试套件。该结果不包含本次新构建标识下重新执行全部断言的承诺。

## 安装目录更新

部署前 40 个文件逐项比较哈希：仅 `Pulse.exe`、`Pulse.Index.exe`、`Pulse.Preview.exe`、`Pulse.Shell.exe` 四个文件不同。`Pulse.Document.exe` 和其它 35 个文件内容相同，未重复复制。

通过 Windows UAC 运行预先准备的受限部署脚本。核对服务路径后临时停止原来 Running 的 PulseIndex 及目标目录的网络代理，备份四个旧文件并验证备份哈希，再替换四个文件。最终 40 个安装文件 SHA-256 全部与验证载荷一致。

回滚备份：`D:\Applications\Pulse\backups\before-audit-20261016-193444`，内含四个旧 EXE 及 backup-manifest.json。旧备份保留。部署脚本在复制失败时按反序恢复已尝试替换的文件；本次未触发回滚。

PulseIndex 已恢复 Running，启动方式 Auto 和服务命令行保持原值；网络代理以原普通用户上下文恢复运行。部署前没有运行主界面，因此未额外启动会加载真实会话的普通主窗口；安装目录的启动检查使用隔离实例并退出。

证据：[deployment-plan.json](deployment-plan.json)、[deployment.json](deployment.json)、[deployment-service.json](deployment-service.json)、[network-agent-restoration.json](network-agent-restoration.json)、[final-runtime-state.json](final-runtime-state.json)。没有手动改写用户偏好或调整服务配置。

## 旧构建清理

新包、部署、哈希与启动检查完成后，按精确路径清理旧 portable-stage 目录和已被替代的旧 ZIP 临时副本，共 41 个文件、36,706,090 字节（约 35.0 MiB）。删除前记录每个文件路径、大小和 SHA-256，并拒绝越界或重解析路径。见 [cleanup.json](cleanup.json) 和 [cleanup-inventory.json](cleanup-inventory.json)。

当前 ZIP 和暂存载荷、唯一当前 build 增量缓存、许可证/运行时、历史验证资料、既有回滚备份保留。`dist\verification-1.1.0.1` 实为历史截图/日志证据，不是旧程序构建，未将其混作二进制产物删除。没有清理源文件或回归夹具。

## Git 及验证边界

按用户要求，将当前全部未提交源码、脚本、资源、文档及其删除一起提交；不只提交本轮审查文件。生成的 build/dist 二进制和被忽略的日志不纳入源码提交。源码与说明文档的暂存差异检查通过；历史工具输出的 `.txt`/`.yaml` 存在原始尾随空格，保留原始证据字节，差异检查时明确排除这两类验证输出，不将它们报告成无空白问题。提交完成后核对 HEAD 与工作区状态。用户未要求推送，不操作远程仓库。

未生成安装器、重新签名、重新杀毒扫描、发布到远程或重跑完整功能回归。已有真实网络/provider、SCM/UAC 功能场景及跨屏/高对比视觉验收边界仍按审查报告保留；本轮实际 UAC 停止/恢复现有服务和文件复制成功，只证明部署链路成功。
