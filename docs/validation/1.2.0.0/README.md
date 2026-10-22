# Pulse 1.2.0.0 安装版替换核验

- 版本来源：`version.txt` 从 1.1.4.0 升至 1.2.0.0。
- 使用现有 Release、静态 MSVC 运行库配置，关闭嵌入式 selftest；重新配置 CMake 后，仅构建五个正式程序。
- 五个程序的 FileVersion、ProductVersion、OriginalFilename 均已核对。发布检查通过，40 个目录文件与 ZIP 条目 SHA-256 全部一致；导入检查未发现未随包提供的 VCRUNTIME、MSVCP 或 CONCRT 依赖。
- 安装目录：`D:\Applications\Pulse`。替换前没有运行中的主界面或网络代理，仅 PulseIndex 服务运行。
- 完整回滚备份：`E:\Projects\pulse\dist\rollback-before-1.2.0.0-20261023-002440`，66 个文件逐一核对 SHA-256。
- 实际替换六个内容变化的文件：五个程序和 README.txt。没有删除旧文件或修改用户配置。
- 升级后再次核验 40 个安装文件哈希、五个安装程序版本、六个用户配置文件哈希。
- PulseIndex 恢复 Running / Auto，服务路径保持 `"D:\Applications\Pulse\Pulse.Index.exe" --service`。
- 从安装目录运行隐藏的 `--test-instance --shot` 隔离启动测试，正常退出，退出码 0；检查截图确认夹具目录与测试文件已显示。
- 清理该测试产生的网络代理后，系统恢复为仅 PulseIndex 服务运行，与替换前一致。

本次没有运行完整历史回归，也没有重新执行文件传输或操作中心交互。卡片改动在此前已通过 182 项定向 UI 检查，本次版本更新没有再次改动这些实现。

原始证据：`package-audit.json`、`rollback.json`、`deployment-result.json`、`installed-verification.json`、`dependencies.txt`、`installed-smoke.png` 及构建日志。
