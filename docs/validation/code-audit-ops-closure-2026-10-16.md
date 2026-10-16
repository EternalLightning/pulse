# Ops / FS / Shell 补读台账（2026-10-16）

本轮从 `code-audit-outstanding-2026-10-16.md` 第 5 节及两份 2026-10-15 Ops 报告接续，保留既有 dirty 工作。新增局部修复限于目录冲突 Skip 的收尾处理、Shell 诊断日志的长度边界，以及 v1 回收站索引的路径终止符校验；没有提交、部署、启动真实 UAC 或操作用户文件。

## 本轮确认及修复

### OPS-C1 / P2：跳过目录冲突后仍对目标执行目录元数据收尾

`OpsManager::RunTransfer` 冲突选择 `Skip` 后将目录放入 `skipped_prefixes`，主传输循环会跳过其所有后代，Move 的源目录清理也已有相应排除。然而 Copy 和 Move 的末尾 `SetDirectoryMetadata` 循环仍遍历这些目录。目录源与同名普通文件冲突时，目标没有目录租约，`DestinationGuard::SetDirectoryMetadata` 返回 false，用户明确选择跳过后整项操作仍被标成 Failed。嵌套冲突还会使已经传输的无冲突兄弟项与错误终态并存。

本轮仅给这两个元数据循环加上已跳过源子树判断，复用既有路径组件边界匹配；没有改动删除授权、冲突选择或回滚策略。新增 `pulse_ops_test --filter directory-skip`，使用独立 `bench_data/opstest-directory-skip-<pid>-<tick>`，覆盖 Copy/Move × 根/嵌套目录冲突，核对完成状态、源子树与原目标内容、兄弟项继续处理、跳过目录不产生完成记录、全跳过不产生 Undo，并核对夹具清理。

### SHELL-C1 / P2：HostLog 将 snprintf 的所需长度用作实际写入长度

`src/shell_host/main.cpp::HostLog` 原有 `char buf[512]`，`snprintf` 返回所需字符数 `n` 后直接 `WriteFile(..., n, ...)`。`CtxCrashFilter` 的模块路径等长消息可使 `n > 511`；格式化虽截断，文件写入仍越过实际数组，读取相邻栈内存。该结论来自可达调用及长度契约，不声称已经触发真实进程崩溃或敏感数据泄漏。

新增 `host_log_line.h::FormatHostLogLine`，由实际格式化过程返回固定缓冲区和受限长度，HostLog 只写入数组中实际保留的字节。`shell_host_log_test.cpp` 覆盖短消息精确输出、2048 字符消息的截断内容、恰好 511 字节时的换行及终止符边界。主代理负责新增独立 CMake 目标 `pulse_shell_host_log_test`。

### FS-C1 / P2：v1 回收站索引接受未终止的截断原路径

`fs_recycle.cpp::ParseIndexBytes` 的 v2 分支已拒绝超出实际字节的声明长度，但 v1 分支直接接受 `wcsnlen(p, maxn)` 的结果；记录在原路径中途截断且没有 NUL 时，仍会将剩余字节作为完整原路径。该值会进入列表及 `ValidateRecycleUndoPair` 的还原目的地核对。本轮在扫描达到可用上界且没有 NUL 时拒绝记录，保留有效终止路径的行为。已有 `pulse_recycle_backend_test --index-validation` 增加有效 v1、完整字符但无终止符、路径中途截断三项定向断言；未操作真实回收站。

## 三个重点文件的全文补读

以下范围覆盖正文、条件编译分支和失败/退出路径；不是仅查看 diff 或符号列表。行号以补读时快照为准，后续局部修改可能移动行号。

| 文件 / 原行段 | 本轮逐段核对的分支 |
| --- | --- |
| `ops_manager.cpp` 1–589 | 路径/命令展开、驱动根约束、无覆盖 rename、CopyFile2 回调/暂停、源元数据、临时名、reparse 复制与失败删除、哈希和 recovery 解析 |
| 同文件 590–1180 | recovery 读写/拒绝不确定重放、Start/BeginShutdown/Stop、Submit/Open 邮箱、取消和暂停、锁失败采集及重试准备 |
| 同文件 1181–1694 | Undo reservation 和历史身份校验、执行分派、Undo 完成消费、回收身份刷新、历史 JSON 使用、异步 Shell 启动及回调 |
| 同文件 1695–2250 | worker/MenuThread 的生命周期、请求映射、Transfer 重试计划、源租约、枚举/取消、根移动优化、目标快照及 link 决策 |
| 同文件 2251–2690 | 进度、跳过/KeepBoth/Replace、备份、对象句柄 rename、reparse、目录创建、复制内容锁及暂停续传 |
| 同文件 2691–3111 | 哈希失败、临时产物提交/清理、Move 删除源、元数据、备份恢复/保留、部分 Undo 重组、完成和失败重试状态 |
| 同文件 3112–3738 | 驱动标签、精确回收恢复、rename/batch 分支、Shell 创建与等待超时、Undo 严格反序列化 |
| `operation_elevation_adapter.cpp` 1–428 | 编解码大小/字符串/枚举/路径/身份限制、操作请求契约、guard 导出及计划字段 |
| 同文件 429–931 | 请求解码、status/conflict/delete/snapshot/control、句柄复制、父进程进度累计导入和用户决策桥接 |
| 同文件 932–1397 | 父进程提交、helper 缺失、未知终态恢复记录、worker 导入历史对象、父 PID/HWND、执行线程/取消/最终 snapshot |
| 同文件 1398–1741 | 生产入口，以及 TESTING 的 fixture/部分复制/Undo/持久化/路径/畸形 payload 分支 |
| `operation_elevation_transport.cpp` 1–647 | 同用户/创建时间/映像路径/高权限准入、随机本地管道与 ACL、帧边界、overlapped 完成取消、连接/父存活、Cancel/Control/TerminalAck、未知结果、线程退出和响应上界 |

尤其确认：普通 produced-object Undo 的旧 JSON 缺少 `produced` 身份时，`UndoFromJson` 将 `supported` 置 false；本轮没有通过抓取当前同名对象来补足历史删除授权。身份拒绝仍是失败；传输丢失终态仍保留 unknown recovery，不能自动重放。

## 16 个旧 Detailed cpp 的全文补齐

下列文件均已读至文件尾，包含条件编译测试分支及失败/退出路径。此表记录本轮阅读区间和核对内容；不是依据符号搜索升级历史证据。

| 文件 / 本轮完整区间 | 核对内容 |
| --- | --- |
| `ops/clipboard.cpp` 1–186 | 打开/清空/锁定剪贴板、HDROP 字节与终止边界、Preferred DropEffect、Unicode 文本及所有权转移 |
| `ops/delete_operation.cpp` 1–551 | BuildDeletePlan、网络降级、回收配对、确认/身份快照、helper 结果完整性、部分/未知完成、Undo 消费与重试 |
| `ops/delete_service.cpp` 1–129 | 计划准入、确认生成/单次 token 决策、取消、Consumed 状态和执行 token 防回绕 |
| `ops/drop_staging.cpp` 1–276 | 临时路径判断、链接/复制/重解析拒绝、目录枚举错误、失败回滚、线程 UI 等待和过期清理 |
| `fs/fs_enum.cpp` 1–575 | 路径/UNC、游标取消恢复、NT 异步 IO 生命周期、有限能力回退、虚拟驱动与共享枚举 |
| `fs/fs_net_cache.cpp` 1–397 | PNCH 长度限制、临时提交排序、UNC 探测槽位、超时后实际 IO 所有权及结果消息 |
| `fs/fs_recycle.cpp` 1–186 | v1/v2 索引边界、当前用户 SID、卷枚举与不完整容量；本轮修复 v1 终止符 |
| `fs/fs_watch.cpp` 1–439 | 通知解析、rename 配对、目录身份替换、IO 取消完成、有限退休与 callback 停止门 |
| `shell_host/ctx_session_process.cpp` 1–242 | 进程配额/job、pipe peer、分段菜单/invoke、可见窗口超时、终止后回收与停止 |
| `shell_host/deletion_identity.cpp` 1–35 | 大小写差异必须同卷同 FILE_ID，重解析点本身身份读取 |
| `shell_host/recycle_fileop.cpp` 1–176 | ITransferSource sink、历史身份再验、取消、同卷当前用户配对和返回路径后验 |
| `shell_host/shell_interaction_worker.cpp` 1–70 | picker COM 所有权/参数、提权进程等待/heartbeat/取消结果 |
| `shell_host/shell_launch_process.cpp` 1–335 | 排队准入、owner 创建时间、pipe peer、未知执行结果、可见 UI 延时、取消和 runner join |
| `shell_host/shell_query_icons.cpp` 1–135 | WIC 格式/尺寸/alpha、jumbo 裁剪、图标及 PIDL/COM 所有权 |
| `shell_host/shell_query_metadata.cpp` 1–149 | namespace/PIDL 校验、marshal 初始化、快捷方式无 Resolve 解析与长路径 fallback |
| `shell_host/shell_query_worker.cpp` 1–138 | parent 创建时间、私有 pipe peer/帧限制、COM、查询异常和 TESTING 故障分支 |

除 FS-C1 外，本次补齐没有确认新的可修复产品缺陷；对驱动/Provider 可返回性和 Shell 按路径执行的原有边界不作原子性承诺。

## 当前生产文件普查和证据级别

实际以 `Get-ChildItem -File` 普查 `src/ops`、`src/fs`、`src/shell_host` 三个目录：本轮开始为 **32 + 11 + 22 = 65** 个生产文件；新增 `host_log_line.h` 后为 **66**。这不是全仓总数，也不含 `src/bench`。本台账逐项列出，避免以重点文件数量代替目录普查。

状态说明：**本轮全文**是本轮读完整个文件；旧 **历史详细** cpp 已在本轮补读全文（见分段台账），原报告的历史通过记录仍不代表本轮执行；**历史全文**是续篇明确 Complete/Full 的记录；**归档包**由并行归档补读报告承接，不在本轮修改范围内。历史测试数字均不当成本轮新结果。

| src/ops 文件 | 阅读证据 |
| --- | --- |
| archive.cpp | 归档包；两份旧报告及 archive-retry-recovery-2026-10-15.md |
| archive.h | 归档包；旧报告详细契约/提取路径阅读 |
| archive_progress.h | 归档包；旧报告历史详细 |
| archive_retry.h | 归档包；后续新增文件，旧 Ops 两份覆盖范围不能自动覆盖它 |
| clipboard.cpp | 本轮全文；补齐原 Detailed 记录 |
| clipboard.h | 本轮全文 |
| copy_progress_report.h | 本轮全文；此前历史详细 |
| delete_operation.cpp | 本轮全文；补齐原 Detailed 记录 |
| delete_operation.h | 本轮全文；此前历史详细 |
| delete_service.cpp | 本轮全文；补齐原 Detailed 记录 |
| delete_service.h | 本轮全文；此前历史详细 |
| destination_guard.h | 本轮全文，补齐原 targeted 阅读 |
| drop_staging.cpp | 本轮全文；补齐原 Detailed 记录 |
| drop_staging.h | 本轮全文 |
| file_identity.h | 本轮全文；实际实现位于 Core 已覆盖 common/file_identity.h |
| file_lock_owner.cpp | 本轮全文，补齐原 lock probing/termination targeted 阅读 |
| file_lock_owner.h | 本轮全文 |
| file_object_reference.h | 本轮全文；此前历史详细 |
| image_open.cpp | 历史全文；Ops 续篇明确 contents/lifetime/COM/activation 全文记录 |
| image_open.h | 本轮全文 |
| operation_elevation_adapter.cpp | 本轮全文 |
| operation_elevation_adapter.h | 本轮全文 |
| operation_elevation_transport.cpp | 本轮全文 |
| operation_elevation_transport.h | 本轮全文 |
| operation_permission.cpp | 本轮全文，补齐原 targeted 阅读 |
| operation_presentation.h | 本轮全文，补齐原 targeted 阅读 |
| ops_manager.cpp | 本轮全文 |
| ops_manager.h | 本轮全文 |
| produced_undo.h | 本轮全文；此前历史详细 |
| recycle_undo_validation.cpp | 本轮全文；此前历史详细 |
| recycle_undo_validation.h | 本轮全文；此前历史详细 |
| transfer_rate.h | 本轮全文；此前历史详细 |

| src/fs 文件 | 阅读证据 |
| --- | --- |
| fs_enum.cpp | 本轮全文；补齐原 Detailed 记录 |
| fs_enum.h | 本轮全文 |
| fs_net_cache.cpp | 本轮全文；补齐原 Detailed 记录 |
| fs_net_cache.h | 本轮全文 |
| fs_recycle.cpp | 本轮全文；本轮补充 v1 截断索引修复 |
| fs_recycle.h | 本轮全文 |
| fs_snapshot.cpp | 本轮全文，补齐原 targeted 阅读 |
| fs_snapshot.h | 本轮全文 |
| fs_watch.cpp | 本轮全文；补齐原 Detailed 记录 |
| fs_watch.h | 本轮全文 |
| path_error.h | 本轮全文；此前历史详细 |

| src/shell_host 文件 | 阅读证据 |
| --- | --- |
| ctx_handlers.cpp | 历史全文；Ops 续篇 Complete 记录 |
| ctx_handlers.h | 本轮全文 |
| ctx_session_process.cpp | 本轮全文；补齐原 Detailed 记录 |
| ctx_session_process.h | 本轮全文 |
| delete_fileop.h | 本轮全文 |
| deletion_identity.cpp | 本轮全文；补齐原 Detailed 记录 |
| deletion_identity.h | 本轮全文 |
| host_log_line.h | 本轮新增并全文核对 |
| main.cpp | 本轮全文，补齐原 progress/worker/delete-dispatch targeted 阅读，含菜单线程和 watchdog |
| operation_worker.rc | 本轮全文（共 2 行，包含已由 Core 记录的 strings.rcinc） |
| recycle_fileop.cpp | 本轮全文；补齐原 Detailed 记录 |
| recycle_fileop.h | 本轮全文 |
| shell_interaction_worker.cpp | 本轮全文；补齐原 Detailed 记录 |
| shell_interaction_worker.h | 本轮全文 |
| shell_launch_process.cpp | 本轮全文；补齐原 Detailed 记录 |
| shell_launch_process.h | 本轮全文 |
| shell_launch_worker.cpp | 本轮全文，补齐原 owner/entry targeted 阅读，含 COM、窗口循环和 TESTING 分支 |
| shell_query_icons.cpp | 本轮全文；补齐原 Detailed 记录 |
| shell_query_icons.h | 本轮全文 |
| shell_query_metadata.cpp | 本轮全文；补齐原 Detailed 记录 |
| shell_query_metadata.h | 本轮全文 |
| shell_query_worker.cpp | 本轮全文；补齐原 Detailed 记录 |
| shell_query_worker.h | 本轮全文 |

## 验证交接及边界

主代理串行协调共享 build，本包没有另起构建。主代理回传的本轮实际结果如下；日志保存在 `docs/validation/audit-closure-2026-10-16/` 中对应 target/selector 的日志文件：

- `pulse_ops_test --filter directory-skip`：17 PASS、0 FAIL，exit 0；日志 `pulse_ops_test-filter-directory-skip.log`。
- `pulse_shell_host_log_test`：3 PASS、0 FAIL，exit 0；验证纯格式化长度及截断内容。
- `pulse_recycle_backend_test --index-validation`：6 PASS、0 FAIL，exit 0；验证 v1/v2 索引字节边界。
- `pulse_shell`：生产目标集成构建 exit 0；此结果证明编译集成，不等同于真实 Shell 扩展运行验收。

本轮针对本包修改文件的 `git diff --check` 通过（只有既有 LF/CRLF 提示）。提权 adapter/transport 测试已全文阅读，现有同完整性传输模式使用 CreateProcess，明确不验证真实 UAC；它们的历史通过记录没有被改写为本轮执行。

未覆盖：真实 UAC 同意/拒绝/凭据切换及高权限目录变更、真实 SMB/RaiDrive/云 Provider 稳定身份/重解析/断连、第三方 Shell 扩展和 Photos 原生窗口交互、完整 GUI/DPI/主题视觉验收。Shell/provider 的按路径绑定与执行并不因此升级为原子身份删除保证。目录读取完成、编译完成、纯 helper 测试都不能代替这些边界验证。
