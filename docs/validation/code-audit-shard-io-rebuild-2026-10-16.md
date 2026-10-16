# 分片 I/O 状态与安全重建（2026-10-16）

续接 code-audit-shard-recovery-2026-10-16.md 的未解决边界，保留全部既有 dirty 修改。开始时重新阅读 AGENTS.md。本记录覆盖 index_shard.cpp/.h 和 index_engine_test.cpp 的定向分支；index_engine.cpp 的缓存读取调用修复及 index_cache_resolution_cases.h 由主任务负责。

## 实际复现

新增 `--shard-io-rebuild-only`，在更改生产实现前运行：17 PASS / 12 FAIL，退出 1。

- active 分享冲突、头部字节锁、payload 字节锁被当作内容无效，错误选择 previous 并重写 manifest：每种情形两个断言失败。
- manifest 分享冲突、字节锁仍进入无清单扫描，并允许发布路径消耗新候选：每种情形两个断言失败。
- 已有 manifest 且两个槽明确损坏或缺失时，保守发布保护拒绝健康重建：两个断言失败。

上层 `--cache-resolution-only` 在原 Engine 读取实现上运行：7 PASS / 2 FAIL、退出 1。失败为 manifest CRC 已拒绝的 A 槽仍被直接映射，以及 manifest 不可读时仍绕过清单读取 A。该结果已交主任务处理。

## 最终行为

- 新增 `ShardReadStatus`：Missing、Invalid、IoError、Valid，以及单独的 Unsupported。LoadShardManifest / FileCrc64 / ValidateShardBase / ResolveActiveShard 可选输出状态，既有 bool 调用保持源码兼容。Load 缺失清单保留原有 true 返回，并明确输出 Missing。
- 文件/父路径不存在归 Missing；打开、大小查询、ReadFile 失败归 IoError；实际短数据、头/大小/hash 不匹配及必要清单字段不成立归 Invalid。未来清单或快照版本归 Unsupported，不能作为损坏重建。
- manifest 或 active 遇到 IoError/Unsupported 时，不回退、不做孤立槽扫描；扫描孤立槽时任何候选发生这种错误，同样停止。回退完整读取及清单写回失败归 IoError，发布方不会覆盖旧槽或消耗候选。
- Resolve 返回 Missing/Invalid，且两个独立槽已确认不可用时，Publish 可以发布健康重建；保留有效原清单的 source_id 与 generation 递增，不删除整个索引目录。
- 兼容 unsigned 64-bit 清单数值和 active_slot == previous_slot 的已有清单读写；后者不信任另一物理槽作为 fallback，且恢复失败后保守拒绝自动重建。该异常布局未纳入双槽自动重建范围。
- 清单恢复仍按 manifest.previous_slot 选择，不把未记录的槽当作可信历史版本。

## 最终定向验证

使用 scripts/vcvars.bat 配置 x64 环境，只构建 pulse_index_engine_test。最终编译成功、退出 0；过程无编译错误。

| 筛选 | 最终结果 | 覆盖 |
| --- | --- | --- |
| --shard-io-rebuild-only | 44 PASS | 五种锁场景、状态分类、双坏/双缺失重建、身份/代际保留、未来版本拒绝、孤立槽 payload 锁、64位清单及同槽兼容 |
| --shard-recovery-only | 15 PASS | 已知 CRC 拒绝不重试、回退 payload 读取失败、manifest 写回失败与候选保留、正常恢复/下一代发布 |
| --shard-read-failure-only | 13 PASS | 首块/第二块完整读取失败、失败候选不得发布无校验清单、正常发布 |
| --cache-resolution-only | 12 PASS | 上层拒绝绕过 CRC/manifest 错误，正常读取、legacy import、真实 Engine CommitMappedFile 双坏重建及重载 |

四筛选均无 FAIL。相关 git diff --check 通过。前三组夹具按 PID/时间戳使用 bench_data 专用目录，已非递归清理。上层 cache 夹具按其实现保留隔离目录，最后一次路径为 `bench_data/cache-resolution-14196-5564828`；旧失败证据夹具为 `bench_data/cache-resolution-39836-5346578`。

## 限制

主任务整合补记：随后 `cmake --build build --target pulse` 退出 0，成功链接 Pulse.Index.exe 与 Pulse.exe，输出无新增编译警告；见 `code-audit-cache-loading-2026-10-16.md`。

- 状态分类不是原子文件事务；多个打开/检查/哈希/移动之间仍有替换竞争，未证明并发发布或父路径替换安全。固定 manifest.json.tmp 未在本轮改造。
- previous 没有保存的历史 hash 基准，重新完整计算只能确认本次读取完成，不能证明旧槽内容未静默变化。结构语义继续由 Engine 映射验证处理。
- 无 manifest 的孤立槽扫描新增完整哈希读取，以识别 payload 的 I/O 错误；这增加了该恢复分支的顺序读取成本，未做性能量化。正常有清单且有 hash 的 active 路径仍执行已有完整 hash 校验。
- manifest 必要字段及数值检查不等同于完整通用 JSON 解析器。当前检查保留仓库已有字符串提取方式。
- 同槽标记异常的自动重建、未知未来版本迁移、真实断电恢复、SMB/提供程序行为以及文件身份竞态未验收。未跑完整测试程序、完整 selftest 或全量构建；主程序构建由主任务另记。

## 阅读范围

本轮完整阅读 AGENTS.md、index_shard.cpp/.h；定向阅读/修改 index_engine_test.cpp 的接口 helper 与四个分片/缓存入口。主任务新增 cache 头的测试通过本轮可执行文件执行；本分工没有据此宣称已完整审查 index_engine.cpp。
