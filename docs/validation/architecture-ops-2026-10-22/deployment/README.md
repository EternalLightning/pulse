# 安装目录替换结果

2026-10-22 已将本轮验证的 `1.1.4.0` 构建替换到 `D:\Applications\Pulse`。

- 完整备份安装目录 66 个文件，并逐一校验原文件与备份 SHA-256。
- 只替换五个发生变化的 EXE：`Pulse.exe`、`Pulse.Index.exe`、`Pulse.Document.exe`、`Pulse.Preview.exe`、`Pulse.Shell.exe`。
- 七个运行载荷文件（含两个未变化的 DLL）均与暂存构建哈希一致；其余 61 个安装文件与替换前一致。
- 五个 EXE 的版本与 OriginalFilename 检查通过；主程序为 Release、`PULSE_WITH_SELFTEST=OFF`，发布载荷检查通过。`dumpbin /dependents` 未发现需要额外部署的 VCRUNTIME／MSVCP／CONCRT 依赖。
- 替换时没有主窗口或文件操作辅助进程驻留；索引服务正常停止后替换，再恢复到原有 Running／Auto 状态。服务路径保持 `"D:\Applications\Pulse\Pulse.Index.exe" --service`。
- 当前会话网络代理恢复为安装路径的 `Pulse.Index.exe --network-agent`。
- 已启动安装目录的主程序，确认为 `1.1.4.0`、窗口标题“主页 - Pulse”、窗口句柄非零且有响应，保留窗口供用户验收；未自动点击用户界面或声称手工操作通过。

回滚备份：`E:\Projects\pulse\dist\rollback-before-architecture-20261022-205703-465`。本次未重新生成安装包或便携 ZIP，未修改版本号或服务启动配置；替换脚本未主动改写用户设置。

证据：`plan.json` 包含替换前状态、完整备份及暂存载荷哈希；`ServiceStop.json`、`ServiceStart.json` 记录服务控制结果；`applied.json` 记录替换及恢复结果；`smoke-recheck.json` 记录实际安装版启动结果。

首次启动探测因 PowerShell 对 `IntPtr` 的布尔转换而提前结束，记录为零句柄并判失败，原始 `smoke.json` 保留。改为直接比较句柄整数后，复查同一进程确认窗口已创建；这不是一次已复现的产品启动失败。

待执行的用户验收详见 `manual-tests.md`，优先检查真实剪贴板、Z 盘复制／剪切／重命名／删除／撤销及冲突取消。
