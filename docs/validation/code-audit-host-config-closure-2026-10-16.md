# Host 重建命令与配置失败语义（2026-10-16）

范围：未完成清单 U12，以及 R08 中机器配置和旧 SQLite 内容配置 fallback。保留开始时全部 dirty 修改。本文件不覆盖 R08 的查询数值/日期部分。

## U12：重建请求投递结果

`--rebuild-index` 不产生持久化操作，必须以本次 SCM control 是否被接受作为命令结果。原实现忽略 `ReloadService` 的 bool，无条件退出 0；改为直接返回共享 `SubmitServiceReload` 的 Win32 状态。该 helper 在 manager/service/control 失败后先保存错误再关闭句柄，避免清理覆盖错误；成功仅表示请求已投递，不表示重建完成。

独立夹具向生产 helper 注入 SCM 操作，覆盖成功、服务不存在、服务停止、control 拒绝以及 manager 拒绝，并核对关闭数量和没有多余 control 调用。生产命令分支直接使用此 helper。未调用真实 SCM、启动/停止服务或弹出 UAC。

静态复核上层：`IndexClient::RebuildElevated` 经 `RunElevatedIndexCommand` 将进程退出码 0 转为 true、非零转为 false；SettingsController 的 RebuildIndex 分支直接返回该 bool。因此该修复可进入既有失败反馈路径。未做真实设置 UI 或 UAC/跨进程提示验证。

## R08：机器配置契约

使用已有 `json::Document` 完整解析 JSON，并在临时 `IndexConfig` 中验证后发布。缺成员沿用历史默认值，空对象合法，空 index_path 沿用默认目录；未知成员可忽略。拒绝非对象、截断/尾随垃圾、重复键、已知成员错误类型、数组非字符串、非法 Unicode/转义及路径中的 NUL。version 仅接受当前格式 1；generation 接受正 uint64 整数，不允许负数、小数或溢出。

保留既有卷 ID 大写/去空白、排除路径分隔符规范化、尾分隔符裁剪、大小写去重语义。三个配置修改入口在 generation 已达 uint64 上限时明确失败，不允许回绕为 0。LoadMachineConfig 解析失败时返回 false；原有 ConfigureVolume/Exclude/Path 的后续 Save 路径不会执行。

JSON 夹具通过共享生产解析函数验证边界，不访问 ProgramData。未进行真实管理员机器目录 I/O、权限故障或并发配置写入验收。

继续核查所有 `LoadMachineConfig` 调用，确认旧消费者还会忽略失败：InstallService 会立即把默认对象保存覆盖原文件，DataDir 会回退机器默认缓存目录，Engine 也存在默认全卷枚举路径。本分工修复 InstallService 为读失败不保存且 SCM 前退出、保存失败同样退出；VolumesPayload 读取失败不发送默认配置的卷列表；DataDir 无已激活目录时只返回验证成功的配置路径，读取失败返回空。共享纯 helper 的五个注入断言覆盖这些 install/path 契约，已纳入最终 machine 组通过结果。主代理补齐 Engine 消费入口：先检查配置，再使用同一快照枚举；周期读取失败保留上次有效范围，RequestRebuild 失败保留原索引，Start 拦截空目录后再打开变更存储。这里只对相关局部补丁复核，不把这些检查说成 Engine 全文或完整运行验收。

## R08：sidecar 与旧 SQLite 契约

- sidecar 存在时必须可读且完整解码；损坏/访问失败不回退旧 DB，不重新启用过时 roots。
- sidecar 与旧 DB 都不存在才返回默认配置成功；旧 DB 存在但无法打开/读取、缺表、busy、损坏或已知值无效时返回失败。
- 旧 DB 的 roots、settings、已配置的 exclusions 在同一只读事务读取，每个 statement 必须到 SQLITE_DONE；任一失败不发布已读取的部分配置。保留历史规则：maximum_file_bytes 尚未配置时维持默认排除目录。
- 旧值使用带范围的整数检查，拒绝负尺寸、超范围 encoding、NUL/无效路径及超额 roots/exclusions；busy/locked 返回 ERROR_BUSY，其余存储/格式失败为 ERROR_INVALID_DATA。
- 新 checked `LoadContentIndexConfig(ContentIndexConfig&)` 及显式 database_path 重载失败保持调用者输出不变。Instant client 的状态读取传播错误，不把失败标成新的 ready 配置；显式保存仍可替换损坏 sidecar，作为用户主动恢复操作。保留无参 value API 供既有调用兼容，该 API 本身仍只能返回默认值并保留 GetLastError。
- `ReadConfigFile` 解码到临时对象，避免失败遗留部分 roots。`LoadOrCreateConfig` 仅在文件缺失时自动创建，损坏/不可读 sidecar 留给恢复处理。

夹具在第一次存储访问前重定向 LOCALAPPDATA 和 OverrideDefaultRootForTesting 到独立 `bench_data/host-config-<pid>-<tick>`，不写真实用户配置。包含晚期缺表、晚期坏 row、负值、SQLite exclusive lock、sidecar 优先级、截断 payload、损坏 sidecar/DB、字节保留与隔离清理。

## 实际验证

主代理添加独立 CMake 目标并分配构建槽后，执行：

`cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_host_config_test"`

构建退出 0，重新编译 host_config_test.cpp、content_configuration.cpp、index_config.cpp 并链接，无编译警告。按主代理本轮已实证的默认沙箱 VerifyGlobs 阻塞情况，采用已批准的 require_escalated 执行环境；本分工没有再做重复的默认构建尝试。

| 定向运行 | 实际结果 |
| --- | --- |
| `build\pulse_host_config_test.exe --service` | 5 PASS / 0 FAIL，退出 0 |
| `build\pulse_host_config_test.exe --machine` | 25 PASS / 0 FAIL，退出 0 |
| `build\pulse_host_config_test.exe --content` | 24 PASS / 0 FAIL，退出 0；隔离 fixture 清理通过 |

最终源码由主代理重新构建并运行上述三个 selector，合计 **54 PASS / 0 FAIL**，全部退出 0；包含准备/清理断言，不表示 54 个独立产品场景。本分工首次运行是 49 个断言，后续 install/path 五个新增断言及 checked path loader 均已包含在最终结果中。源码差异检查 `git diff --check` 通过。host/config 组没有撤回生产修复运行 red 对照；旧行为依据审查报告及原代码路径，新结果来自共享生产 helper/解析器与真实隔离 SQLite/sidecar。

首次检查直接调用点发现 ContentIndex 的 Instant 构造还有独立旧 DB fallback，Writer 的 LoadOrCreateConfig 失败也会继续使用默认 durable。内容分工已改为 checked path loader，以及 Writer 失败时关闭数据库并退出。其 `pulse_content_index_test --content-config-errors` 实际 **6 PASS / 0 FAIL，退出 0**；本分工读取 `bench_data/content-audit-closure-2026-10-16/green-content-config-errors.log` 确认输出覆盖自定义 DB 的 Instant/Writer 拒绝坏 sidecar，以及数据库和 sidecar 原字节保持不变。这六个断言单列，不混入 host/config 的 54 个计数。

## 协作复核的 Engine 边界

独立只读复核向主代理报告了三处与 U01/U02 相关的问题，随后对最终局部补丁再次阅读确认：

- 离线保留原来按 ID 升序遍历，合法 later-parent 移动会跳过 child；现由 ValidParentForest 生成父先子后的 parent_order，PreserveOfflineVolumesLocked 按该顺序复制并重映射 FRN。
- canonical 日志缺失而旧 letter 日志存在时，原先会忽略旧文件并在 OpenDeltas 删除；现 Replay 明确要求恢复，成功发布新快照前不打开/重置这些旧日志。
- replay 的 belongs 原先信任未验证 Node.unused 的 DFS 快捷区间；现以 ParentChainGuard 严格沿父链检查所属卷，防止坏范围放行跨卷 Patch/Tomb。

主代理报告最终定向结果如下；本分工只复核局部代码，没有另行构建或重复运行：

| Engine selector | 主代理最终结果 |
| --- | --- |
| `--delta-replay-only` | 25 PASS / 0 FAIL，退出 0 |
| `--mapped-validation-only` | 24 PASS / 0 FAIL，退出 0 |
| `--parent-cycle-only` | 14 PASS / 0 FAIL，退出 0 |
| `--outstanding-structure-only` | 71 PASS / 0 FAIL，退出 0；包含离线 later-parent/FRN 保留 |
| `--exclusions-concurrency-only` | 5 PASS / 0 FAIL，退出 0 |

上述计数是主代理交接结果，具体 Engine 证据以对应闭环记录为准，不将它们混入 host/config 计数，也不把编译成功等同于 UI 验收。

未做全量 selftest、发布打包、部署、真实服务重建、真实磁盘故障或视觉验收。
