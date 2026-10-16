# Filename Engine 全文静态审查（2026-10-16）

初始阶段只读审查生产代码并形成以下发现。随后按主任务授权，仅修复 E4 的 MODIFIED 缓存失效并完成定向红/绿验证，保留所有已有 dirty 修改。E1/E2/E3/E5/E6 及同步风险仍是静态发现，**没有动态复现**；其夹具说明是后续建议。没有启动真实服务或修改用户文件、配置。

## 阅读范围

- 完整阅读 `src/index/index_engine.cpp` 1–3918 行：分为 1–450、451–900、901–1350、1351–1800、1801–2250、2251–2700、2701–3150、3151–3600、3601–3918 行的连续有界输出，各段没有截断。
- 完整阅读 `src/index/index_engine.h` 1–474 行：首次大批输出截断后，重新读取 1–136、137–249、250–290、291–文件尾；不把初次截断输出作为全文依据。
- 先阅读 AGENTS.md、当前 engine diff，以及 cache-loading、shard-recovery、shard-io-rebuild 报告，避免重报已经修复的机器缓存 CRC/manifest 绕过与 I/O 状态问题。
- 局部依赖：`index_delta.h` 全文；`index_delta.cpp` Open、QueueAdd、Replay Add/Patch/Tomb 分支；`index_query.cpp` QueryIsSimpleName、pinyin 设置；`index_engine_test.cpp` 测试符号与相关夹具入口；change-tracking-full、small-index-boundaries 报告。未声称这些依赖全部全文覆盖。
- memory registry 关键词检索没有用于本轮结论的历史条目；以下均依据当前工作区源码。

文件行号按本轮读取快照；并行任务继续修改时，以函数和语句定位为准。已知 `!path:C:\skip` / `foo\bar` 跨路径分量匹配问题与 QueryCanNarrow 修复由其他工作包处理，本报告不重复登记。

## 新发现

### E1 / P1：分卷 delta 重放重新分配全局 ID，可能把修改/删除应用到另一卷文件

位置：`ReplayDeltasLocked` 2281–2367 行，特别是 Add 分支 2308–2312 与逐卷循环 2348 行；`ApplyUsnLocked` QueueAdd/QueuePatch/QueueTomb；`index_delta.cpp` QueueAdd 与 Replay Add。

运行时所有卷的新节点共用 `BaseCount() + live_.nodes.size()` 全局 ID。Add 日志只写 parent/name/frn 等内容，没有保存分配得到的 ID；Patch/Tomb 及 Add 的 parent 却保存全局节点 ID。启动重放按 vols_ 顺序逐卷完整处理，Add 又顺序追加，因此不能保持原先跨卷交错的分配顺序。

具体序列：base 节点数为 N、卷顺序 C→D。第一轮仅 D 新建文件 d，得到 N；第二轮 C 新建 c，得到 N+1；之后 D 删除 d，日志为 Tomb(N)，并已记录 USN checkpoint。重启先回放 C：c 被分配 N；回放 D：d 被分配 N+1，而 Tomb(N) 删除 c，d 仍存在。若某个新目录随后拥有子节点，Add.parent 也会指向错误节点。checkpoint 已推进，后续正常 catch-up 不会必然补回这些旧事件。

最小验证：在隔离索引目录建立两卷 base，按 D-add/C-add/D-tomb 写真实 DeltaLog，然后由真实 Engine 重放，核对路径、FRN 映射、存活集合与父子关系；再以同卷新增目录/子文件以及 Patch 作对照。修复需要稳定记录/恢复节点身份或安全重建旧版日志，不能仅改变卷遍历顺序。

### E2 / P2：移动后仍信任旧 DFS 区间，导致目录作用域漏查或误查

位置：`InSubtreeLocked` 803–815 行；`CollectMatchesLocked` 1052–1060 行；`FlattenLocked` 写 Node.unused；USN 与 RDCW rename 更新 parent 的分支。

不可变 base 中 `Node.unused` 表示 DFS 子树终点。移动 base 节点只更新 overlay parent，没有使旧区间失效。`InSubtreeLocked` 对落入旧区间的 base ID 立即返回 true，不再检查实际父链。通用匹配收集还把 path_prefix 的扫描限制为该旧区间与 live_，没有补扫移入的 base 节点。

具体序列：经 Flatten/映射的 A\child 移动到 B\child。child 及其 base 后代仍会被判为 A 的成员；B 的通用扫描则可能根本不访问这些 ID。简单名称 posting 与通用查询会出现不同的作用域结果。清 filter cache 不能修复这个结构事实，合并后重新 Flatten 才会暂时恢复区间。

最小验证：真实映射 fixture，使用真实 rename 路径把 base 目录跨父节点移动，分别查询 A/B scope 的普通名称、通配符、后代文件；与当前 parent 链 oracle 比较。应覆盖同父改名和新增 live_ 节点作为对照。

### E3 / P2：稀疏结果集重复加入 patched base 节点

位置：`MatchSet::Begin/Add` 322–342 行；`CollectMatchesLocked` 994 行与 1036–1040 行。

总节点数小于 65536 时，MatchSet 使用 vector 且 Add 不去重。posting pass 只在 `term.pinyin` 为 true 时跳过 patches_；对 pinyin=false 的简单名称查询，它先按当前 overlay 匹配并加入 base ID，随后 patches_ 循环再次加入相同 ID。

触发例：小型已映射索引中某文件仅发生属性变化，因此进入 patches_，名称仍包含“报告”；查询“报告”或 `nopinyin: alpha`。total 与 hits 可重复，分页和排序还可能产生不同的重复表现。中文查询默认不要求拼音候选即可触发，不只影响用户显式关闭拼音。

最小验证：两节点/小规模映射 fixture，创建 attr-only patch，比较纯名称查询与全扫描 oracle 的唯一 ID 和 total；同时覆盖名称 patch、pinyin 开/关和跨越 65536 阈值。需要避免将 sparse Add 的隐含唯一性前提继续带入两个候选来源。

### E4 / P2：RDCW 修改属性后没有使筛选缓存失效（已复现并修复）

位置：`ApplyNotifyLocked` FILE_ACTION_MODIFIED，3845–3868 行；`Search` exact_cached_page / narrowing，1360–1375 行。

FILE_ACTION_MODIFIED 成功读取新 size/mtime 后更新 attrs 或 patches_，没有调用 InvalidateFilterLocked。PollWalkWatches 只递增 revision_，但 Search 复用条件检查 filter_epoch_，不检查 revision_。因此重复 size/date 查询仍复用旧成员集合，total 和 membership 保持旧值；FolderSizeIndex 的 Reset 也没有发生。其他新增/删除/改名分支有失效调用。

最小验证：隔离 watched-file fixture，先执行会命中缓存的 size/date 查询，改写文件长度和时间，用真实通知解析入口应用 MODIFIED，再重复相同查询与递进查询；另外验证 folder size。只在成功获得新属性后失效即可，不需要每个未知 MODIFIED 都扩大工作量。

实际修复仅在成功读取并写入属性的 MODIFIED 分支新增 `InvalidateFilterLocked()`，最终位于 index_engine.cpp:3865；新属性同步使查询缓存与 FolderSizeIndex 失效。没有改动其他通知分支、持久化格式或本报告其他发现。

新增 `src/bench/index_notify_modified_cases.h`，在 EngineTestAccess 内引入，并用 `pulse_index_engine_test --notify-modified-only` 独立调用。夹具使用 `bench_data/notify-modified-<pid>-<tick>` 下的真实文件；分别覆盖 live_ 和真实写入/映射的 base 属性 patch。每种模式检查 size:>20、size:<20、dm:2030、dm:2020 的缓存进入/离开，并在通知前后检查 FolderSizes。文件长度由 10 改为 30，mtime 固定从 2020 年变为 2030 年，不依赖当前日期。映射后先 join 拼音 worker，再 prime 查询，避免其异步 epoch 更新掩盖原问题。

- 修复前：直接使用前一队列构建任务已经编入该夹具的当前 exe，定向 selector 得到 **37 PASS / 16 FAIL，退出 1**。8 个场景的实际属性读取均通过；缓存成员和目录大小各失败一次。隔离目录清理通过。
- 应用上述一行修复后：执行 `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_index_engine_test"`，退出 0，只重新编译 index_engine.cpp 并链接，无新增编译警告输出。
- 同一 selector 最终 **53 PASS / 0 FAIL，退出 0**，包括缓存进入/离开、两种属性存储分支、目录大小重算与非递归隔离清理。相关源码 `git diff --check` 退出 0。
- 按主任务已确认的环境限制，以上构建和测试直接使用单命令作用域提权，没有重复此前沙箱 VerifyGlobs/进程初始化失败；没有改持久环境。未运行完整 index_engine 测试程序或完整 selftest。
- 本测试直接调用生产通知解析入口，验证了 MODIFIED 后属性/筛选/聚合行为，没有启动真实 ReadDirectoryChangesW 监听或服务，不把它扩展为 OS 通知交付、GUI、真实权限变更验收。

### E5 / P2：合法深层路径被截成后缀，未解析的长父链还会直接挂到卷根

位置：`BuildPathLocked` 775–789 行（64 节点）；`BuildQueryPathLocked` 2006–2021 行（256 节点）；`BuildMftTree` 内 build_path（64 节点）与 3083 行的 48-hop 循环。

两条路径构造函数达到固定层数后仍返回成功字符串，缺少盘符/祖先的相对后缀会进入授权、搜索 hit、属性读取、排除规则和 feed。MFT 读取没有统一的 64/256 层总深度限制：父节点已创建时可继续构建更深的树。这里不能把防环上限等同于完整路径。

此外，BuildMftTree 遇到尚未建成的长父链时最多收集 48 层；到上限后 parent_idx 仍是默认卷根，于是把收集到的最高层直接挂根。FRN 排序不保证父节点在子节点之前，目录合法性不保证该策略正确。此问题可能在超过 48 层时先破坏树，不能仅把路径数组改大就宣称修复全部深层路径。

最小验证：合成合法 MFT FRN 链，覆盖父节点在前和子节点在前两种 FRN 顺序，以及 48、64、256 边界；对比完整路径与排除规则。夹具不需要在真实文件系统建立数百层目录。已有 ParentChainGuard 可用于有界防环，但达到界限应明确失败或保持完整合法链，不能静默采用后缀/根回退。

### E6 / P1（损坏输入路径）：映射验证未拒绝越界 parent，后续 Flatten 会越界索引

位置：`MapIndexFile` 的 nodes 校验 1850–1853 行；`FlattenLocked` 2172 行；`PreserveOfflineVolumesLocked` 2099–2102 行。

MapIndexFile 检查 node 名称池范围及 order ID，却没有验证 node.parent 或 DiskVol.root_idx 等结构引用。一个其余字节范围正常、parent=N+100 的节点可通过该层验证。FlattenLocked 的 emit 对这个 parent 直接访问 `remap[static_cast<size_t>(node.parent)]`，没有上界检查；PreserveOfflineVolumesLocked 也有相同直接索引。这不是仅仅结果缺项，可能成为损坏缓存恢复期间的越界读/崩溃。

机器正常 manifest 的 hash 对偶发 payload 修改有保护，但用户模式缓存以及旧单文件导入直接依赖 MapIndexFile，不能把 hash 当作所有入口的结构验证；有效 hash 也不等于结构正确。现有 ParentChainGuard 能防部分查询遍历挂死，不能保护这些 remap 下标。

最小验证：隔离写入一份合法 snapshot，只修改某节点 parent 为 node_count，确认加载被拒绝；另覆盖 cycle、卷 root/first/count 越界以及重复/无序 child_order 的处理策略。负 parent 作为合法根需要保留，不能统一拒绝所有负数。完整排序/唯一性语义未动态验证。

## 另一个待处理的同步风险

`FullRebuild` 3302 行在取得 mutex_ 之前给 excluded_paths_ 赋值，而 ReadFeed、ChangeCoverage 等可在各自持有 shared mutex_ 时遍历该 vector。运行中的配置重建允许这些调用与 vector 赋值并行；读者持锁不能同步未持锁的写者。此为静态可达的数据竞争风险，尚未做并发故障注入。Worker 启动时的初始化赋值应与运行中的重建赋值分别考虑。最小边界是把配置快照的发布纳入同一锁，建树工作仍保留在 worker。

## 已检查但不扩大结论的边界

- 当前 diff 的机器缓存读取已直接检查 ResolveActiveShard 状态，写入 staging 路径与读取许可分离；本报告未重跑原有 84 条相关断言，也不声称它们在本轮通过。
- prefix starts 的零起点/单调性、pool/frn 的除法范围检查、volume_id NUL 检查及 posting local ID 验证已存在。上述 E6 是这些检查之外的结构引用问题。
- MergeBase 发布失败会恢复旧 built_unix_ 并退避；RebuildVolumeMft 的 RecoverFailedVolumes 调用方也恢复失败计数/时间。FullRebuild 保存失败的 live_ 回退、已有 delta 句柄与订阅 generation、分片/aggregate 的原子一致性仍值得定向验证，未在本轮宣称具备事务性。
- OpenDeltasLocked 对 Open 失败仍删除并重开；此前报告已指出 Replay I/O 的完整重试策略未解决。此次未把它登记成已复现的新 I/O 故障，也未修改先前报告。
- Walk watch 启动/rearm 的返回值、移入已有目录的后代扫描、移出/删除目录后的后代残留、非管理员缓存停机期间变化、容量限制及取消预算只做静态阅读，未注入测试，不描述为验收通过。
- 此报告只提升 engine.cpp/.h 的阅读覆盖，不是所有 Engine 方法实现文件或全仓审查完成声明。真实 MFT/USN/RDCW、权限变化、服务并发、网络 provider、断电以及 GUI 均未动态验收。
