# 缓存加载与分片恢复边界续审 — 2026-10-16

接续 `code-audit-shard-recovery-2026-10-16.md`，保留已有未提交修改。本轮检查分片状态传播及直接上层加载，不扩大到完整索引、USN、服务或 GUI 回归。

## 上层发现

`index_engine.cpp` 中原 CachePath 同时用于读缓存和确定写入临时路径。机器索引 ResolveActiveShard 失败时，它直接返回 base-a.bin。TryLoadCache 随后执行结构映射检查，未再遵守 manifest 的 CRC 拒绝，因此即使下层已拒绝该槽，上层仍可能加载它。manifest 暂时不可读时也可能沿此路径绕过清单。

本轮将读取入口与写入路径选择分开：TryLoadCache 在机器模式下直接检查分片解析状态，I/O 错误和不支持的版本停止加载；确证缺失/无效时保留旧单文件缓存验证与导入。写入路径仍服务于候选临时文件，最终采用哪个槽由 PublishShardBase 决定。

## 阅读范围与其他调用方

- 定向阅读 index_engine.cpp 的 CachePath、CommitMappedFile、WriteVolumeShards、RefreshQueryShardsLocked、MergeBase、TryLoadCache、卷重建、初次构建发布和 Worker 启动路径。未提升 index_engine.cpp 为全文覆盖。
- CommitMappedFile 已检查 PublishShardBase 返回值；分片解析失败时 RefreshQueryShardsLocked 放弃分卷查询，保留 aggregate 查询模式。
- WriteVolumeShards 当前未传播发布失败，可能留候选文件，但查询端不会把未发布新世代当作已提交分片；本轮不改其接口或候选清理策略。
- 非机器模式 TryLoadCache 在 MapIndexFile 失败后仍按原行为删除旧缓存，未细分其映射失败原因；这是另一个待单独复现的错误处理边界，不能用本轮机器分片测试宣称覆盖。

## 上层定向验证

新增 `src/bench/index_cache_resolution_cases.h`，由 `pulse_index_engine_test --cache-resolution-only` 调用真实 Engine 的加载与提交路径。未另设替代解析器。

- 旧 index_engine.cpp 配新下层状态实现：7 PASS、2 FAIL、退出 1。两个失败分别为 CRC 已拒绝 A 槽仍被直接加载、manifest 被独占时仍采用缓存。
- 修改读取入口后：12 PASS、退出 0。涵盖 CRC 拒绝、manifest 锁定与解锁、缺少分片时导入旧单文件缓存及其带校验值的发布、双槽真实损坏后提交映射候选并重新加载。
- 夹具使用 `bench_data/cache-resolution-<pid>-<tick>`，明确保留供复核；只把进程内索引目录指向夹具，结束时恢复该测试进程的路径/模式设置，没有写真实索引或用户配置。
- 这是生产 Engine 方法的定向调用，没有启动完整索引服务、MFT 扫描、USN 工作线程或真实磁盘故障。更换文件到加载之间的并发替换不由本测试证明安全。

分片状态与重建策略的实现、最终验证见 `code-audit-shard-io-rebuild-2026-10-16.md`。

## 整合收尾

最终同一实现下，`--shard-io-rebuild-only` 44 PASS、`--shard-recovery-only` 15 PASS、`--shard-read-failure-only` 13 PASS、`--cache-resolution-only` 12 PASS，共 84 PASS，各筛选退出 0。不把修复前失败或中间重复运行计入此数。

随后执行 `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse"`，退出 0，当前代码成功链接 Pulse.Index.exe 与 Pulse.exe，输出没有新增编译警告。相关源码 `git diff --check` 退出 0。仅运行这些直接相关筛选，没有全量 selftest 或历史套件。

构建通过不等同于索引服务、实际网络盘或 GUI 验收。全仓审查仍未完成；本轮补齐的是分片状态和机器缓存读取的直接链路。
