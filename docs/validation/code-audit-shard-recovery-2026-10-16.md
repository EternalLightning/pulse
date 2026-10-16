# 分片恢复续审（2026-10-16）

> 同日后续：已进一步区分 I/O、损坏、缺失与不支持版本，并为确证两槽不可恢复的情况恢复安全重建。见 `code-audit-shard-io-rebuild-2026-10-16.md` 和 `code-audit-cache-loading-2026-10-16.md`。下文保留先前阶段的验证与当时限制，不代表这些状态区分仍全部未修复。

保留工作区既有未提交修改，包括上一轮 FileCrc64 / PublishShardBase 完整读取检查。本次仅修改 index_shard.cpp 的恢复路径及 index_engine_test.cpp 的定向夹具。

## 已复现与修复

1. manifest 指定 active 槽的 CRC 不匹配且 previous 缺失时，ResolveActiveShard 会继续进入无清单扫描，以仅头部校验重新接受同一坏槽。现在已知 manifest 的两个槽均不可用时直接失败，不再降级忽略已知 checksum。
2. previous 槽头部可读、payload 被 LockFileEx 独占锁住时，FileCrc64 返回 0；恢复原先仍写入 active_crc64=0 并返回成功，后续因此不再校验 payload。现在完整哈希或大小读取失败就停止，不发布清单。本次夹具复现的是读取 I/O 失败，不将它描述为磁盘内容损坏。
3. manifest 被允许读取、但不允许删除共享的句柄占用时，SaveShardManifest 替换失败，原先被忽略且恢复返回成功。现在将写回失败传给调用方，只有 manifest 成功持久化才报告已恢复。
4. 直接调用方 PublishShardBase 原先忽略 ResolveActiveShard 失败。更严格的恢复失败返回会让它默认选择 A，覆盖仍由旧 manifest 指定的 active，且提前消耗 candidate。整合检查单独复现了此问题；现在已有 manifest 无法恢复、或无法确认 manifest 缺失时，在移动 candidate 前拒绝发布。只有确认文件/父路径缺失才允许未解析状态下首次发布。

## 实际验证

- 使用 scripts/vcvars.bat 配置 x64 编译环境，定向构建 pulse_index_engine_test。
- 修复前新增 `--shard-recovery-only`：7 PASS / 4 FAIL、退出 1。四个失败覆盖上述三处及失败后原 manifest 被改写。
- 初步修复后 `--shard-recovery-only`：12 PASS、退出 0。包括有效 active、CRC 拒绝、fallback payload 读取失败与原 manifest 保留、manifest 替换失败、解锁后恢复和持久化、无 manifest 孤立槽恢复。
- 整合新增发布者直接依赖检查：修复前 13 PASS / 1 FAIL、退出 1，失败断言为 candidate 与旧 active 未被保留。修复后最终 `--shard-recovery-only`：15 PASS、退出 0，另含正常下一代发布及 predecessor 保留对照。
- 直接依赖 `--shard-read-failure-only`：13 PASS、退出 0，保持上一轮完整哈希和发布失败语义。
- 夹具使用 bench_data 下按 PID/时间戳区分的专用目录，已非递归清理；未运行完整测试程序或完整 selftest。

## 未解决边界

整合补记：主任务完成 `cmake --build build --target pulse`，退出 0，当前 index_shard 同时编入 Pulse.Index.exe 与 Pulse.exe，输出没有新增编译警告。见 `code-audit-round-2026-10-16.md`。

- LoadShardManifest / ValidateShardBase 的 bool 接口仍混合不可读和内容校验失败。现有 active 暂时不可读时仍可能选择 previous；manifest 不可读时仍可进入无清单扫描。完整解决需要错误状态传播以及相应调用方策略，本次没有将 I/O 失败笼统称为损坏，也没有声称解决该策略。
- PublishShardBase 已处理 ResolveActiveShard 失败的直接依赖。当前保守策略也拒绝已有 manifest 且两个槽均不可恢复的发布；自动丢弃清单并重建的恢复流程未在本轮设计。manifest 状态检查与后续发布之间仍非原子事务。
- manifest 只保存 active 的 hash/size，没有 previous 的既有校验基准。回退重新计算 hash 仅证明本次完整读取，不能证明 previous 未被静默修改；完整结构仍由 Engine 的映射验证处理。未改持久化格式。
- 无清单扫描仍仅按头部与构建时间选择候选；固定 manifest.json.tmp、并发发布串行化、检查和使用之间的文件替换竞态均未动态验证。
- 本次不宣称主程序运行时恢复、真实磁盘故障、服务进程以及网络文件系统已经通过验收。

## 阅读范围

- 完整阅读：AGENTS.md、docs/validation/code-audit-core-continuation-2026-10-15.md、src/index/index_shard.cpp、src/index/index_shard.h。
- 定向阅读：src/bench/index_engine_test.cpp 的既有 diff、相关 fixture/入口及 legacy fallback 测试；src/index/index_engine.cpp 的 ResolveActiveShard / PublishShardBase 调用位置及 RefreshQueryShardsLocked 的映射加载条件。
- 本记录不是全项目审查完成声明。
