# Engine 未尽项收口记录（2026-10-16）

范围：outstanding 的 U01–U06，以及查询/严格配置读取在 Engine 中的直接消费者。保留原有未提交修改，未操作真实 USN、用户索引或服务。

## 修复与证据

| 项目 | 最终实现与定向证据 |
| --- | --- |
| U01 跨卷 ID | Delta V2 的 Add 持久化全局 ID；完整读取各卷日志后，在 Add 边界按 ID 合并，各卷其它记录保持日志顺序。两卷交错新增、改名、属性、删除、FRN、checkpoint 和再次追加/重启均通过。原实现在修好 root-only 测试前置条件后为 10 PASS / 3 FAIL；首次修复为 13 PASS / 0 FAIL，最终扩展为 25 PASS / 0 FAIL。 |
| U01 旧日志/损坏恢复 | V1 无法恢复缺失身份，明确要求文件系统重建。重放失败回滚全部 overlay 和 checkpoint，保留原日志，阻止新日志写入、merge 和 journal 消费；canonical 缺失而 letter 日志存在也进入恢复。验证旧字节保留、真实隔离 FullRebuild 发布及再次启动；伪造 DFS 范围的跨卷 Tomb 被严格父链所属卷检查拒绝。 |
| U02 父链结构 | mmap 验证越界 parent、自环、多节点环、卷 root、V9 span；保留合法负根及 V7/V8 reserved 字段兼容。Flatten/离线保留也先验证内存父链，避免越界 remap；Flatten 使用迭代 DFS。离线保留按父先子后顺序，移动到较大 ID 父节点的合法子节点和 FRN 不再丢失。 |
| U03 移动后 scope | base parent 改动标记使旧 DFS 快捷范围失效，USN、重放和 notify rename 均发布该标记。实际 mmap 移动后比较父链 oracle、原/新 scope、名称、通配符、path、同父改名和新增后代。 |
| U04 稀疏重复 | posting 跳过全部 patched base ID，由 patch 分支统一收集。稀疏及 65536 节点密集夹具验证属性/名称 patch、拼音开关、唯一命中、total 和分页。 |
| U05 深路径 | 常规/query/MFT 路径构造移除固定 64/256/48 上限，使用完整合法父链；循环/未解析 FRN 链拒绝挂根。父先/子先 48、64、256、300 层分别验证完整路径、排除传播及真实 mmap。 |
| U06 排除快照 | FullRebuild 与 Worker 对 excluded_paths_ 的发布统一持 mutex；扫描留在工作线程。10 次隔离真实重建并发 feed、coverage、search 和排除快照读取通过。压力检查不能代替竞争检测器证明。 |

严格机器配置读取失败时，Start、RequestRebuild、FullRebuild、Worker、Volumes 不再用默认全卷配置继续执行；周期拓扑读取失败保留上次范围。后台优先级由 RAII 平衡。查询消费使用共享名称/类型和完整路径匹配，非法 size/date 在否定后仍拒绝。配置解析和查询边界的独立结果另见对应收口报告。

## 最终定向验证

| `pulse_index_engine_test` selector | PASS / FAIL | 退出码 |
| --- | --- | --- |
| `--delta-replay-only` | 25 / 0 | 0 |
| `--mapped-validation-only` | 24 / 0 | 0 |
| `--parent-cycle-only` | 14 / 0 | 0 |
| `--outstanding-structure-only` | 71 / 0 | 0 |
| `--outstanding-query-only` | 20 / 0 | 0 |
| `--exclusions-concurrency-only` | 5 / 0 | 0 |

合计 159 个通过断言，包括前置条件和清理，不代表 159 个独立产品场景。日志位于 [本轮证据目录](audit-closure-2026-10-16/)，文件名为 target 加 selector；查询最终日志为 `pulse_index_engine_test-outstanding-query-final.log`。相关目标重新编译成功，生产 `pulse`、`pulse_index` 和 `pulse_shell` 构建成功。未运行完整 Engine 测试程序或全量 selftest。

查询集成首次 16 PASS / 3 FAIL。精确诊断显示实际集合仅多出自动建立的 `C:\fixture` 范围目录自身；现有 scope 契约包含自身，测试遗漏了该对象。新增空查询断言全部五个对象，再修正三个精确预期；未修改生产实现迎合测试。原诊断日志保留。U01 最初 root-only 夹具误用不接受空输入的 BuildMftTree，随后访问空卷；已先修前置检查，再取得旧实现有效失败对照，没有把准备异常当作产品复现。

## 边界

没有真实多卷 USN/卷卸载、磁盘损坏、断电、SMB/provider 或 GUI 端到端验收。旧 V1 日志采用重建，重建发布前保留原字节，不声称可无损反推旧 ID。深链和大索引性能尚无生产数据基准；本轮不据夹具通过宣称全项目无缺陷。首次受限执行的构建/进程初始化失败与产品断言分开记录。
