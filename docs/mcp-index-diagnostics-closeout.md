# 索引性能排查收口：关闭临时诊断并清理旧日志

日期：2026-09-18（UTC+08:00）。按用户选择执行“同时关闭临时性能诊断”，保留修复、必要错误处理、结论报告、安装包和用户数据。

## 结论

已完成代码收口、定向验证、就地升级和旧日志清理。当前安装的是默认关闭临时性能诊断的 quiet 包。没有继续排查内存或优化短查询，也没有重复 30 分钟观察。

- Release 构建成功，143 项定向检查通过，0 项失败。
- 安装后及最终核验时，7 个部署文件 SHA-256 全部匹配新包清单。
- Windows 主机上清理 65 个明确列出的旧日志／采样文件；复核无遗漏、无再生。
- 清理后检查持续约 79.2 秒，覆盖原先一分钟日志周期；服务保持 Running、PID 30236 未变，机器和用户目录均未重新生成性能日志。
- 索引可查询，返回 2,910,342 项。索引目录、安装选项和机器索引配置保持不变。

## 诊断关闭方式与保留行为

新增 src/index/index_diagnostics.h：仅当进程启动前精确设置 PULSE_INDEX_DIAGNOSTICS=1 时启用临时性能诊断；默认关闭，进程内缓存开关状态。

默认关闭的内容：

- FilenameTiming 的计时采集、累计、到期检查和 JSONL 写入；强制 Flush 也不写入或截断旧文件。
- IndexMemoryProbe 的进程内存采样。
- Engine 的容器／USN 队列诊断快照，以及仅用于诊断的合并、名字池统计。
- ChangeTracker::Flush 中的可选内存／容量统计，不改变历史剪枝、快照或落盘流程。

保留名字池阈值、失败冷却、实际整理，以及真实结构变化触发的合并。USN 队列预算、错误状态、历史保留和持久化行为没有关闭。必要错误处理及其他运行日志没有被全局禁用。

既有 PULSE_SEARCH_TRACE 仍是独立的显式开关。本轮核对 Process / User / Machine 三个环境范围，两个诊断开关均未设置；未修改用户配置来隐藏问题。

改动涉及 src/index/filename_timing.h、filename_timing.cpp、index_memory_probe.h、index_engine.cpp、change_tracking.cpp，以及相应测试入口和新增的 src/bench/index_quiet_diagnostics_fixture.h。没有重置或覆盖工作区内其他已有修改。

## 定向验证

| 检查 | 通过数 |
|---|---:|
| 默认关闭诊断：计时、日志、heap/mapped 整理、真实合并、查询、历史落盘并重新打开 | 24 |
| 名字池整理回归 | 60 |
| 合并／维护回归 | 19 |
| USN 回归 | 10 |
| Feed 回归 | 16 |
| Feed 存储回归 | 13 |
| 显式启用后的 5,000 次内存采样检查 | 1 |
| **合计** | **143** |

测试进程显式启用需要断言诊断字段的用例；默认关闭用例先清除该进程的开关。没有为生产服务开启诊断。

通过项目 scripts/vcvars.bat 加载编译环境后，构建 pulse、pulse_index、pulse_index_engine_test、pulse_change_feed_memory_test 四个目标。Release / PULSE_WITH_SELFTEST=OFF，发布载荷检查及相关已跟踪源码的 git diff --check 均通过。首次未加载 SDK 环境的构建失败，已纠正；没有把该次失败算作成功构建。

## 安装产物与部署

- 安装包：dist/PulseSetup-1.0.32-quiet.exe，7,882,040 B。
- 清单：dist/PulseSetup-1.0.32-quiet.manifest.json。
- 安装包 SHA-256：C957E65992B24480B3AF01D8C14F1C7B23FB18361CD2E5BF99FBD8689A4569E4。
- pulse.exe：98B5BD944920B3674454522C5E60CA9083B70054E405AA2663E1D6F3F779967C。
- Pulse.Index.exe：84EA6A404620EBFEB1F72B1B925FFDEDC171E0B57656E1946E09BE4AFFE58138。

版本仍为 1.0.32，quiet 是包变体名；构建标识仍是 20260916T002441Z-62B651EE，身份以 SHA-256 为准。没有覆盖此前 5 个安装包，旧包哈希保持一致。

通过既有 installer/PulseSetup.iss 就地升级，使用 Windows RunAs / UAC，没有绕过提权。安装器退出 0，21:32:00 安装后核验完成；21:35:28 再次核验 7 个文件全部匹配清单。

安装目录仍为 C:/Program Files/Pulse，索引目录仍为 C:/ProgramData/Pulse/Index，安装选项仍为 indexservice,startup,desktopicon。机器索引配置 SHA-256 保持 E9645743B7A5C4D331BD6F7556F03D6A0C4C919BEE25CE726D198C7FC4C8E36B。

最终进程：服务 30236（Running / Auto）、主程序 40276、网络索引代理 40936、Shell 43148。服务在短时核验期间未重启；本轮没有做完整 UI 自动化或用户历史逐条完整性检查。

## 清理范围与留存

严格按预先盘点的文件清单逐个删除，没有递归删除 bench_data 或用户数据目录。

| 范围 | 文件数 |
|---|---:|
| bench_data 下已确认的旧排查日志、采样、分析输出 | 50 |
| 当前用户 TEMP 中 20260918 的 pulse-cpu / pulse-memory 系列 JSON | 13 |
| 实际索引目录的 pulse-index-timing.jsonl 及 .1 轮转文件 | 2 |
| **Windows 主机合计** | **65** |

仓库清理分别涉及 filename-idle-40760-180843000（1）、memory3-followup-20260918（3）、memory3-postinstall-20260918（3）、merge-capacity-20260918（1）、name-pool-deploy-20260918（23）、name-pool-validation-20260918（6）、retention-observation-20260918-1060（8）、usn-queue-fix-20260918（5）。

保留源码、测试、脚本、结论报告、安装包及清单、索引、历史、配置及配置备份。没有删除 TEMP 中的 Pulse-results SQLite 文件，也没有清理无关的 content-index、realtime-search 等测试目录。Agent 工作区下载的 4 份旧原始采样／分析副本也已删除；已交付报告保留。

执行说明：首批清理后的回执汇总遇到 PowerShell ordered dictionary 与 Measure-Object 的兼容性错误，导致当时的回执没有保存。随后只读复核原清单，确认 63 项已删除、2 个受保护服务日志仍在；经 Windows 提权流程精确删除这两个日志，再复核全部 65 项不存在。汇总代码已修正，没有重跑批量删除。首批删除时的逐文件哈希未留存，不把盘点字节数称为精确释放空间；两个服务日志的删除前哈希有独立回执。

收口审计留存在 bench_data/diagnostics-closeout-20260918/：cleanup-allowlist.json、cleanup-receipt.json、runtime-cleanup-receipt.json、validation.json、verification.json、final-state.json，以及本轮构建／安装记录。结论、安装物和机器配置共 32 个受保护文件仍在；旧安装包与机器配置哈希复核一致。

**此前报告引用的旧原始日志／采样文件已按用户要求删除，相应原始数据链接不再可用；报告本身和结论没有删除或改写。**

## 短时核验与已知限制

21:34:09–21:35:28 核验约 79.2 秒。机器索引目录及当前用户 Pulse 目录的 timing.jsonl / .1 均不存在，65 个已清理文件均未再生。现有查询探针退出 0、查询可用，但这不代表所有性能预算通过：

| 查询 | 探针 p95 ms | 预算结果 |
|---|---:|---|
| a | 289.30 | 超过 150 ms |
| ab | 52.34 | 超过 50 ms |
| abc | 35.32 | 满足 50 ms |
| report | 53.49 | 满足 100 ms |
| pdf | 2.34 | 满足 100 ms |

本轮只是功能和“日志不再生成”的收口检查，不是受控性能对照实验，也不据此声称解决了短查询超预算、长期内存增长或历史快照峰值。此前名字池修复说明见 docs/mcp-index-name-pool-compaction.md；旧部署观察快照已在 1.0.5.6 清理。
