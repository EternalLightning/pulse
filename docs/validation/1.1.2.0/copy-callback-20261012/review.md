# CopyFile2 非字节回调导致速率循环异常

2026-10-12。用户反馈 F→E 两块机械硬盘复制 ISO，界面长时间估算、约 956 MB/s 高峰后衰减到零并循环，而任务管理器观察约 140 MB/s。本轮针对回调上报链路，不再次修改速率估算器或复制策略；保留此前未提交修改。

## 已确认的根因

CopyProgress 对 ChunkFinished、StreamFinished 和 Error 从对应结构读取累计字节。其余事件的局部变量仍为 0/0，但原条件 `transferred == total || now - last_report >= 100` 使这些事件绕过节流，被当作进度上报并更新 last_report。

ChunkStarted/StreamStarted/PollContinue 并不提供已复制累计字节。开始通知往往紧邻完成通知，错误更新 last_report 后，真正的 ChunkFinished 反而因不足 100 毫秒被忽略。上层虽然用最大值避免字节数倒退，但不断观察到重复累计值；之后有效回调的集中跳变导致速率冲高、衰减、归零。之前只修正 ETA 未覆盖这个上游输入错误。

字段依据见 [COPYFILE2_MESSAGE 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-copyfile2_message)。本次将既有字节提取和节流逻辑集中到 `CopyProgressReporter`，无字节进度事件直接返回，不更新上报时间。身份核验、目标句柄保留、错误阶段、暂停/取消检查仍由原 CopyProgress 执行，不因忽略统计事件而跳过。

## 真实 F→E 对照

只读确认 F/E 为 NTFS，分别位于 Disk 0/1；用户确认两者为机械硬盘。在专用目录中分别生成两份独立 4 GiB 文件，以未缓冲、write-through 写入并关闭源文件，减少源缓存预热。复制阶段使用与生产相同的 `COPY_FILE_COPY_SYMLINK | COPY_FILE_FAIL_IF_EXISTS`，并使用生产进度上报器和同一速率估算器采集数据。原规则和修正规则各复制一次；不读取或修改用户 ISO。

| 指标 | 原规则 | 修正规则 |
|---|---:|---:|
| 不含字节进度的错误 0/0 上报 | 4097 | 0 |
| 有效累计字节上报 | 10 | 189 |
| 首个已采样的非零速率时间 | 3.313 秒 | 1.047 秒 |
| 首次非零之后的零速率采样 | 15 | 0 |
| 估算峰值 | 384.769 MiB/s | 222.411 MiB/s |
| 复制调用平均速率 | 171.109 MiB/s | 132.595 MiB/s |

修正规则平均 132.595 MiB/s 约为十进制 139.035 MB/s。原复制耗时 23.938 秒，修正复制耗时 30.891 秒，返回 S_OK，目标大小与最终报告均为 4,294,967,296 字节。两次均收到 4096 个 ChunkStarted、4096 个 ChunkFinished、1 个 StreamStarted、1 个 StreamFinished；区别是旧规则把 4097 个开始通知全部上报成 0/0，挤掉了几乎所有真正的块完成进度。

这些是两次独立运行，磁盘负载、缓存与目录位置未严格控制；不能把平均耗时差异解释为本次复制策略改变或性能回退。探针复用生产统计器和 CopyFile2 参数，不包含完整 OpsManager 的所有身份核验或 UI。大文件验证检查精确大小与最终字节数，未逐字节哈希；另有生产 32 MiB 复制控制测试核验源/目标哈希。

原始记录：probe-before.csv、probe-after.csv；输入与二进制哈希：probe-before.json、probe-after.json；统计：comparison.json。四个专用 4 GiB 文件已逐项确认路径、名称与大小后清理，仅删除已知文件及空目录，无递归删除，见 fixture-cleanup.json。临时测试程序副本保留。

## 功能验证

- 修复前先提取原统计逻辑保持行为，独立回归复现 10 FAIL / 2 PASS；修正后同一检查 12 PASS / 0 FAIL。覆盖非字节通知不得上报/不得重置节流、开始与轮询交错不挤掉真进度、稳定 140 MiB/s 的输入、空流完成与完成绕过节流。最初一版检查有计数重合导致假通过，已加强实际参数判定；初版日志保留，未弱化断言。见 before-regression-initial.log、before-regression.log、regression.log。
- 受影响主程序、Shell、复制控制目标与独立回调目标构建成功，最终日志无编译警告/错误。
- 哈希一致的普通权限临时副本执行 `pulse_ops_test.exe --filter transfer-control`：11 PASS / 0 FAIL，退出 0。验证暂停确认、暂停时字节稳定、恢复完成、真实字节/项目总数、源目标哈希、取消清理和取消覆盖保留原目标。
- 两次真实跨盘探针均完成，退出 0。没有运行完整自测、压缩包回归或无关历史测试。没有改动任何 UI 布局，曲线继续读取既有速率字段。
- 卡巴斯基 21.26 于 21:23:22–21:23:25 对新主程序与 Shell 复扫；病毒库时间 2026-10-12 09:25:00，Report only，3 对象 OK（含嵌入对象）、检测/可疑/跳过/错误均 0，退出 0。见 kaspersky.log。没有改变防护配置或进行外部厂商提交。

## 速率含义与边界

修正后的数值仍是文件累计进度的速率，不能承诺与任务管理器的物理磁盘速率逐秒一致。[Windows 文件缓存文档](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)说明默认读写会经过系统文件缓存，缓存数据由系统随后落盘。此次修正规则实测仍有收尾阶段的真实进度减速，但不再把开始/轮询通知伪装成零进度；没有为了平滑曲线限幅、填造吞吐或强制改变缓冲模式。

本次未复测用户原约 29 GiB ISO，也未采集同一时段任务管理器曲线，不声称新数值永远稳定为 140 MB/s。当前便携 ZIP 保留先前打包快照，程序目录更新结果另记 deployment.json。

## 部署结果

用户确认已结束操作并关闭 Pulse 后，2026-10-12 21:28 更新 `D:\Applications\Pulse` 中的 Pulse.exe 与 Pulse.Shell.exe，版本均为 1.1.2.0。两份原程序已备份到 `E:\Projects\pulse\build\deployment-backups\Pulse-copy-callback-20261012-212810-83510acd05504367b4f8d228b409d5e8`。

复制后两份程序与已扫描的构建产物 SHA-256 一致，安装目录全部 40 个载荷文件校验通过。PulseIndex 服务仍为 Running / Auto，本次未修改服务，也未启动主界面。主程序 SHA-256 为 `C02118624476EE34A644CEB52AB30FC8BA904929D435FB6A1BC0E2A3BA1D7053`，Shell 为 `C95A6E462D856C7D9F59D4E80254F7B8D7173E06007F65835D149B214F00BCF4`；完整记录见 [deployment.json](deployment.json)。
