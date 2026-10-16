# 2026-10-13 当前工作区重新打包

- 版本：1.1.2.0，不另行升版。
- 五个生产目标：Release / Ninja 增量构建检查成功，ninja: no work to do。
- 初次沙箱构建停留在 glob 检查，终止已确认的构建进程后在授权的正常 Windows 环境重试成功；没有据此推断产品故障。
- 载荷：40 文件，ZIP 与暂存文件逐项 SHA-256 一致；五个 EXE 版本及 OriginalFilename 一致，无嵌入 selftest。
- 包：E:\Projects\pulse\dist\Pulse-1.1.2.0-portable-win-x64.zip
- 大小：12068361 字节。
- SHA-256：EBFF6F9D84DAB18311F29DEC3814BC2ED68D8BAF2A9BF656848C392CAC023060
- 依赖：五个 EXE、lumatext/pdfium 和 7zip 工具的 dumpbin 检查未发现未打包的 VCRUNTIME/MSVCP/CONCRT 依赖。
- 暂存启动：隔离配置和 fixture，隐藏启动截图后退出，退出码 0；截图已人工查看，目录文件可见。
- 已清理：4 个旧包/重复暂存路径，82 文件，64065340 字节。
- 保留：本次完整回滚备份、权限恢复记录、功能测试证据和当前构建缓存。
- 部署状态：已更新 D:\Applications\Pulse，40 个目标文件哈希逐项核对一致；目标目录使用隔离配置启动截图后退出，退出码 0，截图已人工查看。
- 服务恢复：PulseIndex 运行中，Auto 启动模式与服务路径保持；网络索引代理运行中。
- 后续清理：另删除 4 个被本次完整备份替代的旧备份目录、45 文件；总计清理 8 个路径、127 文件、105042514 字节。
- 本次完整回滚备份：E:\Projects\pulse\build\deployment-backups\Pulse-before-1.1.2.0-20261013-181503-0068ce6ccd614deeb874646de92042c6，40 文件旧哈希核对通过。
- 未运行：完整功能/UI 回归、本次包的杀毒扫描、安装器构建；旧杀毒证据不用于证明本次包。

服务部署脚本已在授权后保存完整载荷备份，停止/恢复目标服务并逐文件核对哈希。目标目录启动检查与旧备份清理均已完成，完整记录见 completion.json。
