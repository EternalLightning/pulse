# 变更跟踪及 feed 全文审查（2026-10-16）

本轮只读审查生产代码；本组 5 个文件开始和结束检查的 diff 均为空。没有构建、运行测试、连接真实服务或写用户日志、配置；仅新增此报告。既有 dirty 保留。

## 覆盖

| 文件 | 阅读状态 |
| --- | --- |
| src/index/change_tracking.cpp | 全文 432 行；首次输出中间截断部分已通过 316–432 行补齐 |
| src/index/change_tracking.h | 全文 |
| src/index/change_tracking_client.cpp | 全文 294 行 |
| src/index/change_tracking_client.h | 全文 |
| src/index/index_feed.h | 全文 |

已读 AGENTS.md、code-audit-small-index-boundaries-2026-10-16.md；检索既有 core/app coverage，确认此前这组仅清单或局部阅读。memory registry 关键词检索无相关命中，没有依赖历史记忆作结论。

必要依赖：change_summary_accumulator.h 全文；index_host.cpp 480–590 行（owner、授权、协议解析）；index_engine.cpp Start/Stop；network_index.cpp SeedChanges/ObserveChanges、Stop 与 Flush 定位；content_instant_session.cpp 118–178 行（feed 失败与取消）；app_change_tracking.cpp 仅启动/请求/取结果符号；change_tracking_test.cpp 仅相关符号及测试名称；change_tracking_client_test.cpp 前 90 行。其余依赖不声明全文覆盖。

## 发现与修复

P2：同优先级的移入/移出连续事件，摘要和明细选择相反的分类。

- 位置：change_tracking.cpp Merge，98–103 行；对照 change_summary_accumulator.h Add。
- 触发：同一非零 file_id 先从 C:\outside\a 移到 C:\watched\a，再移到 C:\elsewhere\a，两个事件均为 Renamed。对 watched 查询时 Relative 依次生成 MovedIn 和 MovedOut。
- 现有防护：身份折叠会归为同一文件；Relative 正确区分两次移动；问题发生在折叠阶段。Merge 使用 >= 保留较早的 MovedIn，但随后更新为较新事件的路径与 ID。摘要 accumulator 使用 >= 接受较新的 MovedOut，因此摘要显示移出、明细显示移入。
- 最小复现：独立 ChangeTracker Lease(owner,true)，Record 上述两个不同路径的事件，然后比较 Details(watched) 的 kind 和 Summaries(watched).counts。反向序列（先移出再移入）也应覆盖。
- 修复边界：仅调整 Merge 的相等优先级策略以与 accumulator 一致，保留 Deleted→Created 特例；在已有 tracker 测试加入双向移动断言。初审时尚未修改，已报告主代理；后续在该边界内完成修复。
- 初审验证状态：静态路径确认，当时尚未执行复现；后续修复与实测见下。

### 后续修复及定向验证（2026-10-16）

- 已复现并修复上述 P2。`Merge` 仅将保留旧分类的 `>=` 改为 `>`，同优先级时采用较新事件分类，与 accumulator 一致；Deleted→Created 特例保留。
- `change_tracking_test.cpp` 新增 `--merge-only`，默认运行也包含这些用例。三个独立 tracker 分别记录同一非零 file_id 的移入→移出、移出→移入、Deleted→Created；每组核对明细分类、摘要唯一计数与分类、最新事件 ID/文件 ID/路径/旧路径/时间，共 9 条断言。
- 红测：生产修复前运行该 selector，两条移动分类断言失败，另外 7 条通过，进程退出 1。绿测：应用单字符修复并重建同一目标后，9 条全部通过，进程退出 0。
- 实际构建命令：`cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_change_tracking_test"`。普通 sandbox 构建停在 VerifyGlobs，已中止后再用同一命令提升执行；红测和绿测所需构建均成功，无新增编译警告输出。
- 默认 `TEMP=D:\AppData\Temp` 下测试在输出断言前退出 `0xC0000409`，普通和提升运行均如此，未推断根因。仅在测试命令进程中将 `TEMP`/`TMP` 指向仓库隔离空目录 `bench_data/change_tracking_temp` 后，执行 `.\build\pulse_change_tracking_test.exe --merge-only` 得到上述红/绿结果；测试按 PID 建立和清理子目录，没有修改持久环境或用户配置。
- 本轮只构建此目标、运行此 selector，未运行完整 tracker 套件、真实服务、真实移动操作或 UI；不将此结果扩展为这些范围的功能通过。

## 已核对及未确认为缺陷的边界

- Load 检查版本、大小、记录数及尾部；损坏输入形成 gap。Save 完成写入和 close 检查后才替换，不重复此前 writer 审查。
- Flush 在锁内快照并清 dirty，写失败重新标记 dirty/gap；并发 Record 保留新的 dirty。生产 Engine/NetworkIndex 的最终 Flush 在 worker join 后执行，未找到两个 Save 并行的调用路径，因此没有把理论上的并发 Flush 当作缺陷。
- Lease 保留 tracking_since；重新启用和重启通过 gap 元数据说明中断；Seed 明确禁用 mtime 基线。记录裁剪同时使缓存 revision 失效，摘要缓存还检查时间截断与到期。
- Details 授权发生在分页前；service summary 通过授权后的 Details 派生并标记不完整，不直接暴露共享摘要缓存。授权上下文/完整服务 ACL 不在本组全文范围。
- 客户端交换有 request_id 关联及总 deadline，取消后排空 OVERLAPPED 才销毁事件。摘要发布再次校验 ID；详情队列和结果有数量上限。Stop 期间释放 lease 仍可消耗各自超时，未声称立即返回。
- Feed 限制响应字节数/记录数并检查尾部，取消会排空 pending I/O。Request 对每次 Transfer 设超时，但跳过 status 的循环没有总 deadline；持续 status 而没有请求响应可延迟结束。尚未证实合法 host 的具体长期触发场景，本轮只记录限制，不作为已复现故障或擅自更改。
- 分配失败、线程构造异常、真实磁盘断电、权限撤销、跨进程 pipe 伪造以及真实 UI 快速切换未故障注入。未运行现有 transport 测试（其带参路径会连接生产管道），不把源码检查算成测试通过。
