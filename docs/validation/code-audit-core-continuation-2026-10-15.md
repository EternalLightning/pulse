# 索引、IPC、公共层续审（2026-10-15）

基线 HEAD 为 `da36fb4`，保留续接时已有 dirty 工作区。本文件仅记录本次续审；旧测试结果不算本次重跑结果。

## 本次修复

### 分片读取失败被误判为完整哈希

`index_shard.cpp::FileCrc64` 原先在 ReadFile 失败后以 got==0 或 GetLastError 判断 EOF；失败的 ReadFile 同样可能把 got 置零，导致返回空内容或部分内容的哈希。隔离文件分别锁住首块、第二块，旧实现两个断言均 FAIL，正常完整读取对照 PASS。

现在只有 ReadFile 成功且读到零字节才报告完成。PublishShardBase 在读取大小或完整哈希失败时不再发布 active_crc64=0 的清单（0 原本会禁用后续校验）。候选可能已移动到非活动槽；失败不会发布新清单，不宣称有完整事务回滚。

定向命令：`pulse_index_engine_test --shard-read-failure-only`，最终 13 PASS、退出 0。覆盖首块/第二块读取失败、解锁正常读取、失败禁止发布清单、正常发布。夹具位于唯一 bench_data 子目录并完成非递归清理。

### 结果缓存预取代际与 pending 登记竞态

`ContentResultStore::Prefetch` 原先在 mu 锁外读取 page_epoch。若 RefreshPages 在读取后、插入 pending 前切换代际并清空 pending，则旧代际请求随后入队；Load 拒绝旧代际却不会删除这条刚登记的 pending，同页后续请求被一直抑制，直到再次刷新。

现在在 mu 下同时读取代际并登记 pending，使刷新清理与请求登记有明确先后关系。这是源码同步顺序确认的修复，尚未通过确定性调度夹具复现该精确交错。

`pulse_content_paging_test --task-stream` 首次在默认 TEMP/TMP 下 13 FAIL、1 PASS；保留该失败记录。相同可执行文件将 TEMP/TMP 指向隔离的 `bench_data/audit-stream-5a74966691ee49f99d000bd5b4a129ab` 后 14 PASS、退出 0。改变临时路径后通过，不据此断言第一次失败的底层原因已经查明。覆盖流式合并、分页读取、过滤、排序、取消保留结果与身份升级，不能代替精确竞态复现。

## 交接验证收尾及整合

### 增量日志读取失败与损坏区分

补读 `DeltaLog::Replay` 后新增 `--delta-read-failure-only`：已有日志被独占打开时旧实现错误返回成功，首次运行该断言 FAIL，缺失文件与正常日志对照 PASS。现在只有文件/父路径不存在可作为缺失日志处理；读取/文件大小查询失败返回失败并提供错误码。

调用方原先在 Replay 返回 false 后删除日志。同步修正为仅对 ERROR_INVALID_DATA 进行原有损坏处理；分享冲突等 I/O 错误保留日志并记录诊断，避免将新错误报告变成错误删除。测试另外区分截断日志与分享冲突。无卷 walk 分支仍忽略 Replay 返回值，这个既有上层错误呈现边界未在本次改写；不声称完整解决恢复重试策略。

最终定向 `--delta-read-failure-only` 7 PASS、退出 0；`pulse_index_engine_test pulse` 目标构建成功，含修改后的 index_delta、index_engine 及依赖，未运行完整套件。

- `cmake --build build --target pulse_index_engine_test`：收尾时 no work to do；随后 `--mapped-validation-only` 10 PASS、退出 0，含 prefix table 损坏拒绝用例。不是仅引用旧版 8 PASS。
- 本次最后 `cmake --build build --target pulse`：成功，退出 0；包含本轮 index_shard/content_result_store/preview_handler_host 的编译与 Pulse.exe 链接。此前 app 子任务已完成 app_commands、session/tab 改动的主程序编译。
- 新修改相关 `git diff --check` 通过，仅既有换行符提示。
- 未跑全量构建、完整 selftest、真实 SMB/RaiDrive、桌面主题/DPI 与第三方预览提供程序验收。

## 后续风险与复用判断

- `DeltaLog::Replay` 的文件不存在/读取失败区分已修复；无卷 walk 分支的失败呈现及完整重试策略仍需进一步审查。
- 分片 manifest 仍使用固定 .tmp；需确认所有发布者的串行化契约后再决定是否复用唯一临时文件机制，不从固定文件名直接推断当前存在并发写。
- `ResolveActiveShard` 回退路径仍需单独审查读取失败、manifest 重写失败与旧槽可信度；本次发布修复不声称覆盖所有恢复语义。
- `index_migration.cpp` 已使用 hash/size 验证及非递归清理，但 hash 检查到 DeleteFile 之间的替换竞争、父级 reparse 的动态替换未动态验证。不要把静态路径检查视为原子对象身份保护。
- `ipc::PipeTransfer` 已有取消后回收 OVERLAPPED 的完整公共实现；index transport 也有独立超时传输。复用前需要保留错误码与截止时间语义，不机械合并。

## 阅读覆盖

逐文件状态见 `code-audit-core-coverage-2026-10-15.md`。本次全文阅读只代表文件正文已读完，不代表功能验证或证明无缺陷。index_engine.cpp 仅映射加载及直接使用段、现有 diff；大型索引宿主/查询实现与 IPC client 尚未全文覆盖。其余目录由各分工的记录负责，不能据此宣称全项目通读完成。
