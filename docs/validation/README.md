# 验证记录

当前源码版本为 **1.5.1.0**。构建和打包命令见[构建与打包指南](../build-and-package.md)，CI、签名和发布限制见[发布流程](../../scripts/RELEASE.md)。这里只记录实际执行的检查，不代表当前版本完成了完整回归或安装验收。

## 2026-11-13：界面与弹窗一致性

统一控件配色、键盘焦点、模态标题和主次按钮布局，修复窗口尺寸变化、DPI 更新、已打开窗口外观更新与长错误详情展示。弹窗定向检查共 1,208 项通过，控件图库检查通过；详见[弹窗修复验证](../popup-ui-consistency-fix-validation-2026-11-13.txt)和[前一轮界面修复验证](../ui-consistency-fix-validation-2026-11-13.txt)。真实系统高对比度切换、跨屏 DPI、第三方预览提供程序和已安装应用未实机验证；没有运行完整历史回归。

版本提升后定向构建 `pulse` 及其生产依赖成功；六个 EXE 的 FileVersion 和 ProductVersion 均核对为 `1.5.1.0`，生成的版本头文件与 `version.txt` 一致。版本变更未重跑上述功能用例；未生成安装包、便携包或 GitHub Release。

## 2026-11-12：预览主机报毒定位与修复

固定 MSVC 19.44.35229.0、x64、Release、`/O2 /Ob2 /MT /guard:cf`，使用本机卡巴斯基 2026-11-12 07:12 病毒库进行对照扫描，关闭本次扫描的 iChecker/iSwift 缓存；未提交人工复核。

| 对照样本 | 本机扫描结果 |
|---|---|
| 重建 1.4.1 源码和版本 | 未检出 |
| 同一源码，仅版本改为 1.5.0 | 未检出 |
| 仅加入扩展名判断去重 | 未检出 |
| 仅 JPEG 改用 WRL `FileHandle` | 未检出 |
| 仅 SVG 改用 WRL `FileHandle` | 未检出 |
| JPEG、SVG 同时改用 WRL `FileHandle` | `VHO:Trojan.Win64.SideWinder.gen` |
| 上述组合加 System32 DbgHelp 加载修正 | `VHO:Trojan-Ransom.Win32.Agent.gen` |
| 两处改用项目已有 `UniqueResource` | 未检出 |
| 修正版使用原 1.5.0 包构建 ID | 未检出 |
| 实际 `build-release/Pulse.Preview.exe` 修正版 | 1 个文件，0 检出、0 错误 |

这组对照定位了可复现的二进制触发组合，不表示 WRL 本身存在恶意行为，也不揭示杀软内部规则。修复保留自动关闭文件句柄的能力；JPEG 44 项、SVG/EMF 17 项检查通过，其中包括 32 次分配失败后的句柄计数检查。未扩大到完整回归，未重新打包或替换安装版。

定位修复时的扫描样本 SHA256：`F87A4F36A317BA75170F301956C5E382861AA4AFE32705E039034075D95FBAB6`。原 1.5.0 发布包预览文件 SHA256：`6B1AEB9343AF52125FB9B00528C658564306D6D72FE74FD78B0C46D17022A342`；已有发布包仍包含原文件。

清理时保留的现有产物最后写入时间为 16:52:44，SHA256 为 `41E78FA92E9193ACCA9A6DB7B6622F3A5B988F336E859ABB0EB44335D4DC2C7F`；17:16 的本机复扫同样为 0 检出、0 错误。

## 历史附件

2026-11-12 清理了 362 个临时目标，共 23,149 个文件、约 7.06 GiB 文件总大小，包括旧构建目录、测试数据、截图、对照源码和重复验证文档；保留当前生产程序、发布包、构建工具和增量构建所需的生产对象。已提交的历史附件仍可从[清理前的验证目录](https://github.com/EternalLightning/pulse/tree/aad8a3bc9a7f0fcfeff34a98bf3b154f9af8d18d/docs/validation)查阅；历史检查、安装路径和服务状态只描述当时操作。功能回归源码继续保留在 `src/bench`。
