# 索引、预览与相关 IPC 审查（2026-10-25）

## 范围与证据边界

核对 `docs/report.md` 对 index、preview、IPC 及 common 的相关判断。先只读审查，收到主代理实施消息后在既定范围修改；保留已有未提交修改。以下“成立”指源码机制成立，不等于报告中的性能或利用结果已动态复现。

## 逐项结论

| 项目 | 当前结论 | 建议级别及必要性 | 最小处理方向与用户行为 |
|---|---|---|---|
| P-03 | 成立。`IsIndexNoiseLocked`、`ApplyUsnLocked` 在访问文件名之前没有验证 V2 固定头、offset、length。`CatchUpVolume` 只验证公共头及外层长度，异常尾部还可能被当作成功推进 cursor；`FSCTL_ENUM_USN_DATA` 回退使用 `rec->FileName` 而不按 offset，并在读字段后才检查 RecordLength。 | P1，必须修。SYSTEM 服务中的输入边界缺失属实；实际越界/利用未动态复现。条件 P0 在当前策略下没有成立依据：`index_config.cpp:401-406` 排除 removable bus/不支持持久索引的设备，`BuildVolumeMft`/`RebuildVolumeMft` 要求 Fixed 和 PersistentIndexPathAllowed。 | 提取有界 USN 记录校验供 journal packet、同步 catch-up、枚举回退复用。拒绝截断头、无进展长度、越界 offset/length、UTF-16 非对齐及不支持版本；坏数据应 gap 并拒绝推进 cursor。正常记录不改结果；损坏记录由不确定结果改为明确失败/重建。 |
| P-11 | 部分成立。USN 路径已应用 ShouldSkipName、IsExcludedPath、祖先隐藏与 RefreshSubtreeVisibilityLocked（`index_engine.cpp:2863-2916`），报告不能再指其缺失。walk watcher 的 EnsureChainLocked、ADDED/RENAMED_NEW_NAME 确实没有同等规则，rename 还保留 old.flags。 | P2，必须修一致性。这里“隐藏”是 kFlagHidden 策略（如 node_modules、回收站等），不是所有带 FILE_ATTRIBUTE_HIDDEN 的目录，也不包括任意点目录。 | 在 live chain 创建及 watcher rename 重算可见性，并在目录移动后刷新子树；feed/change tracker 同样过滤不可见项。增补 live/mapped 新建、跨排除边界移动、目录改名隐藏/恢复、祖先缺失用例。用户显式排除/策略隐藏路径不再漏出，普通路径行为保持。 |
| P-20 | 每文档事务与重复元数据/语句构建属实（`content_index.cpp:564-623`）；“首次慢”“WAL 持续增长”没有当前性能证据。数据库使用 WAL + synchronous=NORMAL（1088），没有关闭默认 autocheckpoint 的代码。 | P3 性能候选；在取得吞吐/WAL 峰值证据前，不按 P2 功能故障改写事务边界。 | 单独测索引吞吐、事务次数、WAL 峰值、退出及崩溃恢复，再决定有限批量与语句复用；不为审计结论做纯重构。 |
| P-21 | 状态广播会重排订阅、全局 filter epoch 失效成立；但 QueueSearch 已按 client/session 删除旧排队工作（`index_host.cpp:376-403`），SearchThread 已 hash 抑制相同页面重复发送（460-474）。报告“缺少合并”“结果抖动”过强。 | P3 性能候选。仅在多订阅+状态广播压测显示重复扫描有意义时调整。 | 可将无修订变化的纯状态广播与数据刷新分开；需覆盖初建、pinyin ready、离线/权限变化等不一定对应相同 revision 的状态，避免误跳过必须刷新的结果。 |
| P-22 | 前置大小/记录预算缺失成立，GDI+ 构造在 extent 检查之前（`metafile_raster.cpp:61-69`）。无受限令牌成立。需要修正报告表述：document 有 job 内存/进程限制；preview 的 `thumbnail_cache.cpp:154-182` 直接 CreateProcess，未见 job，仅请求约 1s 截止及失败后 StopChild。 | P2 输入边界有必要；解析器漏洞及用户级执行为推断，未复现。完整沙箱属于兼容性工程，不能等同于补文件限额。 | 本批可先限制输入字节并在 GDI+ 之前检查 WMF/EMF 头部/声明大小/记录预算，保留普通 WMF、EMF、EMF+ 成功。低 IL/AppContainer 独立评估字体、COM/provider、UNC 等兼容性。 |
| P-35 | 成立但报告建议不足。guard 以 FILE_SHARE_READ 打开（`document_host_main.cpp:31`）确实会阻塞并发写入/删除；注释明确用于冻结随后按名重开的源。`document_filter.cpp:32` 的 fallback 自身也用 STGM_SHARE_DENY_WRITE，单改 guard 不能消除全部冲突。 | P2 已知并发限制；应保留问题但不做仅增加共享位的补丁。 | 可靠修复需让解析器读取稳定快照/同一源对象，或增加完整前后版本校验并让所有解析器允许共享。现有 live document reader 仅验证 begin/fallback 版本，未在宿主提取成功后复核源版本。直接放宽可能让正文与源版本错配。 |
| P-23 | 可预测名称成立；ShellClient 已验证 server PID 必须等于自己启动的 child PID（`shell_client.cpp:217-225`），因此当前证据支持本地拒绝服务，不支持伪造响应。 | P3 合理；随机 token 可降低先占坑可能性，需要 client/server 协同。 | 交 Shell/IPC 所有者处理，避免与其修改冲突。 |
| P-26 | 成立。通用 RSP_DONE 的前三字段解析失败时不会 erase pending 或 Dispatch（`shell_client.cpp:560-578`），读取循环继续；删除结果分支已有 malformed 失败完成。 | P2 稳定性更合适，但触发依赖损坏/失常子进程；应补失败完成测试。 | 对畸形完成帧统一按失败完成，并决定断开/Abort；交 Shell/IPC 所有者处理。 |
| P-33 | 成立：network agent 为保护 NetworkIndex 单 latest-result slot 以 timed_mutex 串行搜索，等待 500ms 后 ERROR_BUSY，实际结果每 10ms 轮询最多约 6s（`network_agent_main.cpp:94-120`）。 | P3 并发/延迟限制，当前做法有明确所有权理由，不能只去掉互斥。 | 需要结果按 request ID 保存及事件唤醒后才能安全并发；无用户症状或测量前保留现状。 |
| P-18/common | WriteUtf8FileAtomic 的 FlushFileBuffers 与写穿标志属实，它们保障原子持久化；卡顿取决于 app 调用者是否同步、介质延迟。 | 由 app 所有者核实与处理防抖/后台保存；不删除 common 的持久化保障。 | 不改公共 helper 的 durability 语义。 |

## 报告的其余相关建议

- 第 7/8 节的 index 大文件拆分、codec/命令行/file identity 复用仅为维护建议。当前不按行数拆 index_engine，也不合并 preview/document/index 进程。
- 第 12 节 shard 发布疑问：非活动槽在 manifest 写入前替换为一个已验证 base；若 manifest 写失败，旧 active 仍在，旧 previous 会指向新的有效 base。不能仅凭这一顺序推导数据损坏；断电/注入 I/O 失败及恢复是否回退到意外 generation 仍需专门验证，不计为已确认缺陷。
- P-02 filename 索引 ACL 已由报告按产品取舍撤下；当前 SearchThread 的无 authorize 回调与源码注释一致，不重新包装为缺陷。

## 建议实施顺序和停止条件

1. P-03：一个有界 USN parser + journal/catch-up/enum 三入口；增加合成正常/损坏记录回归，只验证所改目标及筛选用例。
2. P-11：补 watcher ingest 与 rename 的可见性继承及 feed 过滤，使用现有 EngineTestAccess 做 live/mapped 有针对性的回归。
3. P-22：若主代理选择纳入本批，先做可独立验证的 metafile 输入预算；完整沙箱与 P-35 快照方案另行定界。

## 已实施及验证入口

- P-03：新增 `src/index/usn_record.h`，在 `UsnStream::Read` 每个独立包、同步 catch-up、应用前整批验证及枚举回退使用相同校验。枚举按实际文件名 offset 取值；损坏 journal 数据使 feed gap、journal 失效，旧 cursor 保留，避免应用部分批次或静默跳过尾部。
- P-11：live chain 新节点继承策略可见性，watcher rename 重算自身及后代隐藏状态；排除项不会进入 watcher feed。跨入排除路径只发旧可见路径删除，跨出只发新可见路径创建，避免 feed 暴露被排除路径。
- P-22：新增 `src/preview_host/metafile_input.h`，32 MiB 字节预算、100,000 实际记录预算，解析前验证 WMF/EMF 容器和记录边界。GDI+ 使用已验证的不可变内存 IStream，避免路径替换绕过预算；输出最长边仍不超过 1024。未加入受限令牌或 job，复杂单记录的解析资源仍由现有请求截止限制。
- P-20/P-21/P-33 不做没有测量依据的性能重构；P-35 保留冻结源策略，原因见上表。

定向回归入口：

```powershell
.\build_audit_msbuild_20261025\Release\pulse_index_engine_test.exe --ingest-boundary-only
.\build_audit_msbuild_20261025\Release\pulse_index_engine_test.exe --usn-only
.\build_audit_msbuild_20261025\Release\pulse_index_engine_test.exe --notify-modified-only
.\build_audit_msbuild_20261025\Release\pulse_metafile_input_test.exe
.\build_audit_msbuild_20261025\Release\pulse_preview_test.exe --vector-only
```

`--ingest-boundary-only` 覆盖正常/损坏 USN 边界、整批拒绝无 cursor 前移，及 live/mapped watcher 新建、目录/文件跨排除边界、skip-name 改名、feed 语义和未配对改名的目录类型。独立 metafile 测试实际生成并渲染 WMF/EMF/EMF+，并验证畸形长度、校验和、字节/记录超限及 extended path。构建与运行由主代理统一在独立目录执行；本子任务没有构建共享目录。

主代理确认上述依赖桌面的测试目标已构建，但执行时均在 `main` 之前返回 `0xC0000142`，没有测试输出；最小 `GetDesktopWindow` 程序同样失败，纯 console 程序可运行。因此当前不得把 Engine ingestion、watcher 或原生 metafile 渲染列为功能通过。新增 `src/bench/binary_input_boundary_test.cpp`，只用生产头文件和标准库的 console 回归，覆盖实际 USN/EMF/WMF 二进制布局的正常/损坏边界及精确预算，交主代理统一构建运行。这项补充只验证纯解析路径，不替代前述 Engine 或原生预览验证；结果待补。

已完成源代码语法检查：在 `bench_data/audit_index_syntax` 目录以 MSVC `cl /Zs /std:c++20 /EHsc /W4 /permissive- /utf-8` 检查 `index_engine_test.cpp`、`metafile_input_test.cpp`、`index_engine.cpp`、`metafile_raster.cpp`，exit 0，无 warning/error。此项仅证明编译语法与警告检查，不代替运行验证。
