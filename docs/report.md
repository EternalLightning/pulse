# Pulse 审计报告

- 版本：1.2.1.0，当前工作树
- 审计日期：2026-10-25
- 审计方式：只读静态审阅 + 定向测试；未修改源码
- 特别说明：索引服务文件名搜索绕过调用者 ACL 已按产品决策撤下，不再列为缺陷

---

## 0. 审计范围与方法

### 0.1 范围

本报告审阅 `E:\Projects\pulse` 当前工作树，而不是已提交的 `a6ad4a2`。审计开始时工作树包含 53 个未提交修改（+1,922/−304）。所有结论以当前工作树为准。

规模：

| 项 | 数值 |
|---|---|
| `src` 文件数 / 行数 | 818 / 约 198,110 |
| 生产代码（含头文件） | 约 133,949 行 |
| 生产 `.cpp` | 约 104,639 行 |
| `src/bench` | 247 文件 / 约 67,423 行 |
| 测试目标 / `add_executable` | 119 / 134 |
| `CMakeLists.txt` | 1,279 行 |
| `bench_data` | 约 1.1 GB |

### 0.2 方法

- 按模块读取实现与调用链，使用 `rg` 做交叉引用核对。
- 4 个并行子代理分别覆盖 ops/fs、index/preview、app、ipc/shell/common；所有高危结论由主代理回到源码复核。
- UI 子代理达到轮次上限未产出，UI 结论仅来自定向静态检查。
- 会话持久化部分额外做了定向测试：
  - 在 `build_release_clean`（Ninja）构建 `pulse_json_persistence_test` 与 `pulse_session_windows_test`。
  - `pulse_json_persistence_test` 实际运行 **2 项失败，exit 1**；`pulse_session_windows_test` **6/6 通过**。
  - `build/` 是旧 NMake 目录，CMake 因头依赖不安全拒绝复用；本次未使用。

### 0.3 判定标记

- `【事实】`：已由源码、调用链或实际测试确认。
- `【推断】`：依赖运行条件、外部环境或未实测路径。
- `【建议】`：修复方向，不是补丁。

### 0.4 验证限制

- 未做完整 UI 交互、IME、触摸、辅助功能、窄布局实测。
- 未测试真实第三方 Shell 扩展、网络共享、恶意 NTFS 卷、多用户令牌、DPI/IME 场景。
- 性能结论来自静态复杂度与调用频率分析，不是测量值。
- 会话持久化的失败测试来自定向构建；其余测试未做全量回归。
- 审计对象包含未提交修改；若这些修改被回退或重写，行号与结论需要重新核对。

---

## 1. 执行摘要

### 1.1 总体评价

整体质量高于典型自研文件管理器。明显强项：

- 进程隔离：Index/Shell/Preview/Document 分进程，第三方 Shell 扩展和文档解析器不直接跑在 UI 进程。
- 路径与重解析点防护：destination guard、reparse/hardlink 捕获、目录 lease、句柄身份、归档 `SafeName`。
- IPC 边界：帧头、payload 上限、字符串/数组分配预算、预览响应总预算、服务端 PID 校验。
- 持久化：原子写、WRITE_THROUGH、崩溃日志写 LocalAppData。
- 缓存：缩略图缓存有界、请求去重、失败重试上限、generation 校验。

问题集中在四处：

1. 删除确认后的路径重用（TOCTOU）。
2. 索引服务的 USN/MFT 解析边界。
3. App/UI 生命周期与 UI 线程 IO。
4. 会话持久化两套实现的接入、schema 与运行时恢复语义。

撤下 P-02 后，确认问题/改进项 49 项：**P0 0、P1 8、P2 26、P3 15**。未发现可远程利用的漏洞或稳定复现的常规崩溃；P1 中的删除竞态可导致误删，P-03 在恶意/损坏卷条件下可导致 SYSTEM 服务越界。

### 1.2 会话持久化状态（重要）

当前工作树存在两套会话持久化：

| 套 | 类型 | 状态 |
|---|---|---|
| 完整会话 | `SessionSnapshot` / `SaveSession` / `LoadSession` / `CaptureLayoutTab` / `RestoreWindowTabs` | **没有生产调用者**；只有测试与 selftest 使用 |
| 偏好快照 | `WindowSessionSnapshot` / `SaveSessionPreferences` / `LoadSessionPreferences` / `CaptureWindowPreferences` | 生产路径实际使用；保存外观、tray、undo、窗口位置 |

生产调用点只有：

- 退出保存：[src/app/app_main.cpp:1873](file:///e:/Projects/pulse/src/app/app_main.cpp#L1873)
- 启动读取：[src/app/app_main.cpp:2187](file:///e:/Projects/pulse/src/app/app_main.cpp#L2187)

`SaveSession` / `LoadSession` 无生产调用；`CaptureLayoutTab` / `RestoreWindowTabs` 只出现在测试与 selftest。多窗口集成测试目前甚至明确断言启动只创建一个 Home 窗口、退出文件不包含 `windows/layoutTabs/path/activeTab/tabGroups`：[src/bench/multiwindow_application_test.cpp:493](file:///e:/Projects/pulse/src/bench/multiwindow_application_test.cpp#L493)、[src/bench/multiwindow_application_test.cpp:519](file:///e:/Projects/pulse/src/bench/multiwindow_application_test.cpp#L519)。

定向测试发现：

- `pulse_json_persistence_test` 有 2 项失败：
  - `session restore preserves This PC as an empty navigation path`
  - `session restore keeps This PC and folder panes in their original positions`
- `pulse_session_windows_test` 6/6 通过，但它只验证 JSON 序列化往返，不验证运行时 `RestoreLayoutTab`。
- 现有 `pulse_app_controllers_test` 通过，但它断言“空路径是占位符，应跳过”，与 `json_persistence_test` 对空路径的期望相反。

**结论**：如果完整会话恢复是发布目标，S-01/S-02/S-03 是发布门槛；如果不是，应删除这套死代码，避免继续维护两套口径。

### 1.3 P0/P1 Top 10

| 排名 | ID | 级别 | 问题 | 影响 |
|---|---|---|---|---|
| 1 | P-01 | P1 | 删除身份校验与 Shell 按路径删除之间的 TOCTOU | 确认后被替换的同名文件可能被误删 |
| 2 | P-03 | P1（条件 P0） | USN/MFT 记录边界校验缺失 | 恶意/损坏 NTFS 元数据可使 SYSTEM 服务越界读 |
| 3 | S-03 | P1 | 空路径 This PC 被当作占位符丢弃 | 会话恢复丢失 This PC 窗格；现有测试失败 |
| 4 | S-02 | P1 | 两套会话写同一个 `session.json`，schema 互斥 | 偏好写入会抹掉完整会话导航 |
| 5 | S-01 | P1 | 完整会话没有生产入口 | 启动不会恢复窗口/标签/分栏 |
| 6 | P-19 | P1 | 外部拖放同步 staging 阻塞 UI | Outlook/RDP 虚拟文件或大目录拖放冻结 |
| 7 | P-04 | P1 | 关闭搜索标签不取消索引/内容搜索 | 订阅、后台扫描与内存无界残留 |
| 8 | P-12 | P1 | context worker 无锁共享状态 + 无限等待 | 右键菜单卡死、4 个槽位耗尽 |
| 9 | S-10 | P2（条件 P1） | 恢复 UNC/虚拟路径会在启动时自动加载 | 自动连网、弹凭据框或重建搜索 |
| 10 | P-11 | P2 | 隐藏/排除目录继承缺失 | 排除目录下新文件仍可被搜索到 |

### 1.4 Top 3 优先事项

1. **删除 TOCTOU（P-01）**：不要再加“删除前再校验一次”的补丁；把永久删除改为按已冻结句柄删除，或把句柄传给 Shell host 并失败关闭。
2. **索引服务解析边界（P-03）**：补齐 USN/MFT 记录的文件名偏移/长度和记录边界校验；这是 SYSTEM 服务的内存安全问题。
3. **UI 线程与生命周期（P-19 + P-18 + P-04 + P-12）**：外部 staging 异步化、偏好写盘防抖、搜索 teardown 统一入口、context worker 状态加锁/有界等待。

如果即将启用完整会话恢复，S-01/S-02/S-03 应视为与上述同级的发布门槛。

### 1.5 简化规模估计

不存在“几千行压到几百行”的单一子系统。现实可删/可合并约 **1,200–2,000 行生产代码**（占生产代码 0.9%–1.5%），另有 **400–600 行 CMake/测试基础设施**可收敛。

如果完整会话恢复不启用，额外可删除约 **200 行生产代码 + 相关多窗口测试**。如果启用，则应删除两套 schema 中的一套，并复用现有 `RebuildLayoutRoot` / `MakePresetTree`，而不是继续手写 pane 构造。

### 1.6 三条底线结论

- **安全底线：基本守住，但不完全。** 路径归一化、重解析点/符号链接拒绝、归档路径安全、原子写、IPC 边界、索引服务认证与文件夹大小/变更流可见性都做得扎实；缺口是 P-01、P-03，以及预览沙箱、命令拼装、日志目录等 P2。
- **稳定底线：基本守住。** generation 取消、fail-closed 删除、进程隔离、有界缓存、原子日志都在；但关标签泄漏、context worker 竞争/挂起、worker 异常边界、会话注销不保存需要补。
- **性能底线：常见场景守住，边界场景会突破。** 后台枚举、缩略图缓存、搜索分页合格；外部拖放 staging、同步 fsync、每窗口 verb 扫描、每文档事务会直接打到 UI 或磁盘。

---

## 2. Bug 清单

### 2.1 P1（含条件 P0）

| ID | 级别 | 模块 | 症状/复现 | 根因 | 证据 | 影响 | 修复方向 | 补丁式 | 判定 |
|---|---|---|---|---|---|---|---|---|---|
| P-01 | P1 | shell_host/ops | 确认删除 A 后，另一进程把 A 替换为 B，最终删除 B | 身份复核后仍由 `IFileOperation` 按路径解析；保留句柄以 `FILE_SHARE_DELETE` 打开，不阻止改名/替换 | [shell_host/main.cpp:471](file:///e:/Projects/pulse/src/shell_host/main.cpp#L471)、[deletion_identity.cpp:24](file:///e:/Projects/pulse/src/shell_host/deletion_identity.cpp#L24)、[delete_operation.cpp:485](file:///e:/Projects/pulse/src/ops/delete_operation.cpp#L485) | 永久删除误删；回收站删除同样受影响 | 永久删除改用 `FileObjectReference::Delete`；回收站删除把句柄/文件 ID 传给 Shell host，不匹配即失败关闭 | 否 | 事实 |
| P-03 | P1（条件 P0） | index | 构造/损坏 NTFS 卷上的 USN 记录可使服务读越界 | `ApplyUsnLocked` 直接信任 `FileNameOffset/FileNameLength`；枚举回退只校验 60 字节头 | [index_engine.cpp:2727](file:///e:/Projects/pulse/src/index/index_engine.cpp#L2727)、[index_engine.cpp:3026](file:///e:/Projects/pulse/src/index/index_engine.cpp#L3026)、[index_engine.cpp:3128](file:///e:/Projects/pulse/src/index/index_engine.cpp#L3128) | SYSTEM 服务崩溃或越界数据进入索引；若索引可移动介质则物理接触即可触发 | 校验 offset >= 头部、length <= RecordLength-offset、对齐与版本；枚举路径对每条记录校验 `rec + RecordLength <= end` | 否 | 缺失校验为事实；利用条件为推断 |
| S-01 | P1 | app | 启动不恢复窗口/标签/分栏；多窗口测试断言只创建一个 Home 窗口 | 完整会话 API 没有生产调用者；生产只接入偏好快照 | [app_main.cpp:1873](file:///e:/Projects/pulse/src/app/app_main.cpp#L1873)、[app_main.cpp:2187](file:///e:/Projects/pulse/src/app/app_main.cpp#L2187)、[multiwindow_application_test.cpp:493](file:///e:/Projects/pulse/src/bench/multiwindow_application_test.cpp#L493) | “两套”实际只有一套生效；完整会话代码是死能力 | 若启用：增加全窗口 CaptureSession、启动恢复、退出保存并更新测试；若不启用：删除完整会话 API 与测试 | 否 | 事实 |
| S-02 | P1 | app | 完整会话与偏好快照互相覆盖 | 两套都写 `session.json`，偏好版用 `navigation=false` 省略导航字段 | [session.cpp:462](file:///e:/Projects/pulse/src/app/session.cpp#L462)、[session.cpp:473](file:///e:/Projects/pulse/src/app/session.cpp#L473) | 谁最后写谁赢；偏好写入后完整恢复变成空会话 | 拆成两个文件（如 `session.json` / `preferences.json`），或只保留一个完整 schema 并读改写；禁止偏好写入删除导航字段 | 否 | 事实 |
| S-03 | P1 | app | This PC 窗格在会话恢复后消失；2 个定向测试失败 | `RestoreLayoutTab` 把空路径当占位符跳过；但空路径在现有模型中表示 This PC | [session_tab_model.cpp:87](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L87)、[json_persistence_test.cpp:32](file:///e:/Projects/pulse/src/bench/json_persistence_test.cpp#L32)、[json_persistence_test.cpp:42](file:///e:/Projects/pulse/src/bench/json_persistence_test.cpp#L42) | 混合分栏少一个窗格，随后用第一个文件夹克隆补齐，两个窗格显示同一目录 | 保留前 `LayoutPresetCount(layout)` 个条目（空路径按 This PC），只丢弃超出 preset 的尾部空占位；仅当 `panes` 为空时走默认回退 | 否 | 事实（测试确认） |
| P-04 | P1 | app | 打开搜索标签后关闭，索引仍为它推送/重查 | `CloseLayoutTab` 只取消网络认证；`WindowTabs::CloseTab` 直接 erase；订阅项 `query.subscribe` 不会在回调中删除 | [app_navigation.cpp:2044](file:///e:/Projects/pulse/src/app/app_navigation.cpp#L2044)、[app_model.cpp:658](file:///e:/Projects/pulse/src/app/app_model.cpp#L658)、[app_navigation.cpp:722](file:///e:/Projects/pulse/src/app/app_navigation.cpp#L722) | `pendingIndexSearches` 无界增长；内容扫描继续；索引每次变更触发已关闭标签的合并与丢弃 | 统一 `TeardownTabSearch`：取消 index/network/content session、清 pending map；所有关闭路径调用 | 部分（先补入口），最终统一 | 事实 |
| P-12 | P1 | shell_host | 右键菜单偶发空结果/卡住；handler 挂起后 context 进程不回收 | `query_completed/invoked` 在 Run 线程无锁读写，Query 线程加锁重置；`CtxWorkerHandles::Stop` 无限等待线程 | [ctx_session_process.cpp:168](file:///e:/Projects/pulse/src/shell_host/ctx_session_process.cpp#L168)、[ctx_handlers.cpp:509](file:///e:/Projects/pulse/src/shell_host/ctx_handlers.cpp#L509) | 数据竞争 UB；4 个进程槽位耗尽后菜单持续空结果；fallback 线程永久泄漏 | 字段改 atomic 或统一 session mutex；Stop 设有界超时并终止 worker | 否 | 事实 |
| P-19 | P1 | app/ops | 拖入大型外部源或虚拟文件时窗口禁用、UI 冻结 | `StageDropSourcesOnWorker` 在 UI 线程 `join`；只泵 `WM_PAINT`；`EnableWindow(FALSE)` | [drop_staging.cpp:215](file:///e:/Projects/pulse/src/ops/drop_staging.cpp#L215)、[app_input.cpp:223](file:///e:/Projects/pulse/src/app/app_input.cpp#L223) | 拖放期间无输入、无取消、无进度 | 异步 staging + 进度/取消；完成后投递 drop；或改用 `IFileOperation`/流式读取 | 否 | 事实 |

### 2.2 P2

| ID | 模块 | 症状/复现 | 根因 | 证据 | 影响 | 修复方向 | 判定 |
|---|---|---|---|---|---|---|---|
| P-05 | fs/ops | 清空回收站提示成功，但仍有残留 | `$I` 尺寸 <24 或 >64KB、读取失败、孤儿 `$R` 被静默跳过 | [fs_recycle.cpp:109](file:///e:/Projects/pulse/src/fs/fs_recycle.cpp#L109)、[fs_recycle.cpp:148](file:///e:/Projects/pulse/src/fs/fs_recycle.cpp#L148)、[delete_operation.cpp:88](file:///e:/Projects/pulse/src/ops/delete_operation.cpp#L88) | 用户误以为数据已清除 | 统计跳过项并返回部分失败；提供损坏 `$I` 清理路径 | 事实 |
| P-06 | ops | 撤销失败/取消后该条历史消失，无法重试 | 非 Copy/Create 类型在 `Submit` 前 `pop_back`；RestoreRecycle 的 Submit 失败无补偿 | [ops_manager.cpp:1866](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L1866)、[ops_manager.cpp:1935](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L1935) | 还原回收站失败后无法再次撤销 | 完成后再消费历史；Submit 失败恢复条目 | 事实 |
| P-07 | app | 托盘不可用导致隐藏失败时，窗口仍在但标签已被重置回 Home | 先 `ResetLastTabToHome` 再 `HideAppWindow`；隐藏失败直接 return | [app_exit.cpp:75](file:///e:/Projects/pulse/src/app/app_exit.cpp#L75) | 用户关闭窗口后状态被破坏；固定标签语义丢失 | 隐藏成功后再重置；隐藏失败保持原状 | 事实 |
| P-08 | app | 从 Outlook/RDP 拖虚拟文件到托盘，Drop 返回后文件消失 | 托盘分支直接 `Collect` 原始 sources；仅标题栏分支调用 staging | [app_input.cpp:444](file:///e:/Projects/pulse/src/app/app_input.cpp#L444)、[app_input.cpp:218](file:///e:/Projects/pulse/src/app/app_input.cpp#L218) | 托盘批次失效 | 托盘分支复用 `StageDropSourcesOnWorker` | 事实 |
| P-09 | app | 双击文件但主实例挂起/转发失败时，新进程静默退出 | `ForwardOpenPath` 返回值被忽略；`Acquire` 调用两次 | [app_main.cpp:2287](file:///e:/Projects/pulse/src/app/app_main.cpp#L2287)、[single_instance_coordinator.cpp:42](file:///e:/Projects/pulse/src/app/single_instance_coordinator.cpp#L42) | 用户无窗口、无提示 | 检查返回值并报错/自启窗口；只 Acquire 一次 | 事实 |
| P-10 | fs | 目录监视启动失败静默跳过；每次 500ms 超时重开目录句柄 | `DirWatchSet::Sync` 对 Start 失败 `continue`；超时重开用于检测替换 | [fs_watch.cpp:414](file:///e:/Projects/pulse/src/fs/fs_watch.cpp#L414)、[fs_watch.cpp:140](file:///e:/Projects/pulse/src/fs/fs_watch.cpp#L140) | 文件变化不刷新且无提示；额外 IO | 暴露降级状态并回退轮询；失败退避 | 事实 |
| P-11 | index | 排除/隐藏目录下新建或重命名的文件仍可被搜索到 | walk watcher 与 rename 路径未应用 `ShouldSkipName`/`IsExcludedPath` | [index_engine.cpp:3984](file:///e:/Projects/pulse/src/index/index_engine.cpp#L3984)、[index_engine.cpp:4048](file:///e:/Projects/pulse/src/index/index_engine.cpp#L4048)、[index_engine.cpp:749](file:///e:/Projects/pulse/src/index/index_engine.cpp#L749) | 隐私/一致性缺口 | 所有 ingest 路径统一走排除/隐藏继承判定 | 事实 |
| P-13 | ops/shell_host | 文件名以 `%1` 结尾或目录以 `\` 结尾时命令参数错位；未加引号的注册表命令按前缀搜索可执行文件 | 多轮 `ReplaceAll` 替换插入路径中的占位符；拼接不转义末尾反斜杠；`CreateProcessW(nullptr, 未引号命令)` | [ops_manager.cpp:130](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L130)、[shell_launch_worker.cpp:202](file:///e:/Projects/pulse/src/shell_host/shell_launch_worker.cpp#L202) | 打开方式失败/执行错误程序；第三方 verb 命令含空格时可能执行 `C:\Program.exe` | 单次扫描替换；复用 `QuoteWindowsArgument`；优先 `ShellExecuteEx` | 事实 |
| P-14 | shell_host | 长路径/UNC/`\\?\` 下元数据、固定到快速访问、预览初始化失败 | `QueryShellName` 去前缀，`QueryShellNamespace`/`InitializePreviewItem` 不去 | [shell_query_metadata.cpp:24](file:///e:/Projects/pulse/src/shell_host/shell_query_metadata.cpp#L24)、[shell_query_metadata.cpp:50](file:///e:/Projects/pulse/src/shell_host/shell_query_metadata.cpp#L50)、[shell_query_metadata.cpp:99](file:///e:/Projects/pulse/src/shell_host/shell_query_metadata.cpp#L99) | 长路径场景功能缺失 | 统一 `ToShellParsingPath` 并补长路径测试 | 事实 |
| P-15 | app | 同完整性级别进程可发 `WM_COPYDATA` 让 Pulse 打开任意 UNC 并弹凭据框 | 未校验发送者 PID/映像；失败后自动 `WNetAddConnection3(CONNECT_INTERACTIVE)` | [app_main.cpp:942](file:///e:/Projects/pulse/src/app/app_main.cpp#L942)、[app_network_access.cpp:49](file:///e:/Projects/pulse/src/app/app_network_access.cpp#L49) | 枚举线程被占用；诱导用户输入凭据；非提权 | 校验发送者；路径限制；凭据改为显式用户操作 | 事实 |
| P-17 | app | 多窗口启动或注册表变化时重复读 32MB verb 缓存并枚举最多 400 个扩展 | 每窗口 `SeedShellVerbCache`；每个窗口都响应失效消息 | [app_commands.cpp:965](file:///e:/Projects/pulse/src/app/app_commands.cpp#L965)、[app_main.cpp:1550](file:///e:/Projects/pulse/src/app/app_main.cpp#L1550) | 磁盘/版本资源扫描叠加；线程与内存放大 | 全局单飞 + debounce；共享结果；取消上一轮 | 事实 |
| P-18 | app | 点击排序/分组/列宽时 UI 线程同步写配置并 fsync | `SetSort/SetGroup/RememberFolderColumns` 立即 `Save`；`WriteUtf8FileAtomic` 含 `FlushFileBuffers` 与 `MOVEFILE_WRITE_THROUGH` | [app_navigation.cpp:1808](file:///e:/Projects/pulse/src/app/app_navigation.cpp#L1808)、[app_navigation.cpp:1842](file:///e:/Projects/pulse/src/app/app_navigation.cpp#L1842)、[utf8_file.h:76](file:///e:/Projects/pulse/src/common/utf8_file.h#L76) | 慢盘/杀软下交互卡顿 | 防抖 + 后台 writer；退出时 flush | 事实 |
| P-20 | index | 首次内容索引慢、WAL 膨胀、磁盘写放大 | 每个文档一个 `BEGIN IMMEDIATE`/`COMMIT`，并多次 stat/打开/SELECT | [content_index.cpp:600](file:///e:/Projects/pulse/src/index/content_index.cpp#L600)、[content_index.cpp:591](file:///e:/Projects/pulse/src/index/content_index.cpp#L591) | 大批量索引吞吐低；WAL 持续增长 | 批量事务（N 文档）+ 预编译语句 + 单次打开 | 事实 |
| P-21 | index | 每个索引状态广播都重排全部订阅，filter epoch 使匹配缓存全失效 | 广播路径对每个订阅重新入队；`InvalidateFilterLocked` 全局递增；单 SearchThread | [index_host.cpp:280](file:///e:/Projects/pulse/src/index/index_host.cpp#L280)、[index_engine.h:398](file:///e:/Projects/pulse/src/index/index_engine.h#L398) | 多实时搜索页时 CPU 尖峰、结果抖动 | 按订阅合并/防抖；按路径增量失效 | 事实 |
| P-22 | preview/index | 打开超大 WMF/EMF 时预览进程内存/CPU 飙升；文档/预览解析器无降权 | GDI+ 构造 `Bitmap` 解析整文件后才校验尺寸；WMF 无大小上限；仅 job 内存/进程限制，无低完整性令牌 | [metafile_raster.cpp:61](file:///e:/Projects/pulse/src/preview_host/metafile_raster.cpp#L61)、[document_reader.cpp:308](file:///e:/Projects/pulse/src/index/document_reader.cpp#L308) | 预览 DoS；解析器漏洞获得用户级代码执行 | 先限文件大小/记录数；有界渲染；可行时 AppContainer/低 IL | 事实（无上限/无降权）；利用为推断 |
| P-35 | index | 内容索引打开文档时只允许 `FILE_SHARE_READ`，Office 保存/改名可能失败 | 宿主以 `FILE_SHARE_READ` 打开并保持最长 30s，OPC/IFilter 还要按名重开 | [document_host_main.cpp:31](file:///e:/Projects/pulse/src/index/document_host_main.cpp#L31) | 文档保存被共享冲突阻塞 | 放宽共享（`FILE_SHARE_READ|WRITE|DELETE`）或用快照副本；缩短持有时间 | 事实 |
| P-36 | ops | 大量回收站删除后，每秒重开所有 `$I/$R` 校验；undo 历史无上限 | worker 空闲每秒调用 `RefreshRecycleUndoValidity`；每个操作后也调用；`undo_` 无 cap | [ops_manager.cpp:1660](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L1660)、[ops_manager.cpp:2105](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L2105)、[ops_manager.cpp:1797](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L1797) | 持续磁盘 IO、内存增长、撤销菜单变慢 | 改为点击撤销时惰性校验 + 目录变更显式失效；历史 cap | 事实 |
| P-37 | ops/fs | 工作线程未捕获异常，`bad_alloc` 等可终止进程 | `WorkerThread` 直接调用 `ExecuteFileOperation`；`fs_watch::WorkerMain` 无 catch | [ops_manager.cpp:2092](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L2092)、[fs_watch.cpp:374](file:///e:/Projects/pulse/src/fs/fs_watch.cpp#L374) | 进程终止，操作状态未知 | 线程入口统一 catch，转为失败结果并保留 journal | 事实 |
| S-04 | app | 自定义分栏比例重启后回到 50/50 | `split_ratios` 被捕获/序列化，但 `RestoreLayoutTab` 不读取；`RebuildLayoutRoot` 硬编码 0.5 | [session_tab_model.cpp:53](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L53)、[session_tab_model.cpp:73](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L73)、[app_model.cpp:732](file:///e:/Projects/pulse/src/app/app_model.cpp#L732) | 分栏布局恢复不完整 | 按预设树先序应用比例；数量/范围不匹配时回退 0.5；补 live tree 测试 | 事实 |
| S-05 | app | 恢复后的 tab 在绑定前 root 为空；空会话 fallback 路径不一致 | `RestoreLayoutTab` 清空 root 不重建；依赖 `BindCurrentLayout` 懒重建；空 `layout_tabs` 的 fallback 无路径/loader | [session_tab_model.cpp:82](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L82)、[app_runtime.cpp:279](file:///e:/Projects/pulse/src/app/app_runtime.cpp#L279)、[session_tab_model.cpp:151](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L151) | root=null 期间布局/命中不一致；空会话得到 This PC/空白页而非 Home | restore 内完成 root 重建；统一空会话默认路径 | 事实 |
| S-06 | app | 损坏/手改 session 可生成大量 tab/pane/group 与启动加载请求 | 仅限制 32 个窗口；tab/pane/group 无上限；写出的文件还可能超过 JSON 8MB 读取上限 | [session.cpp:24](file:///e:/Projects/pulse/src/app/session.cpp#L24)、[session.cpp:226](file:///e:/Projects/pulse/src/app/session.cpp#L226)、[session_tab_model.cpp:143](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L143) | 内存/IO 放大；自写文件自读失败 | 读写两侧加 tab/pane/group cap；写入前检查总大小 | 事实 |
| S-07 | app/ops | undo 跨版本导入可能把 type 映射到另一种操作 | `pending_undo_json` 直接导入；`UndoFromJson` 只做 int 范围检查；`OpType` 以枚举序号持久化；无 undo 版本 | [app_main.cpp:644](file:///e:/Projects/pulse/src/app/app_main.cpp#L644)、[ops_manager.cpp:4351](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L4351)、[ops_manager.h:46](file:///e:/Projects/pulse/src/ops/ops_manager.h#L46) | 旧/手改 session 可能导入错误操作；执行期身份检查只能兜底 | type 改为稳定字符串或增加 undo 版本号；限制条目数；保留执行期身份检查 | 事实（解析）；影响为推断 |
| S-08 | app | 退出只保存最后窗口；注销/关机可能不保存 | `WM_DESTROY` 仅 `last_window` 保存；无 `WM_QUERYENDSESSION` | [app_main.cpp:1871](file:///e:/Projects/pulse/src/app/app_main.cpp#L1871) | 多窗口状态丢失；注销/重启丢 undo/tray | 退出前枚举所有窗口统一捕获；处理 `WM_QUERYENDSESSION` 并 flush | 事实 |
| S-09 | app | 完整会话顶层偏好取第一个窗口 | `SessionToJson` 使用 `windows.front()` 作为顶层投影 | [session.cpp:421](file:///e:/Projects/pulse/src/app/session.cpp#L421) | 下次启动主题/侧栏/详情面板来自第一个窗口，而非最后活动窗口 | 顶层使用最后活动窗口或全局偏好；明确 tray 归属 | 事实 |
| S-10 | app | 恢复 UNC/虚拟路径会在启动时自动加载 | 保存任意 `current_path`；加载失败路径触发网络认证 | [session_tab_model.cpp:66](file:///e:/Projects/pulse/src/app/session_tab_model.cpp#L66)、[app_navigation.cpp:1221](file:///e:/Projects/pulse/src/app/app_navigation.cpp#L1221) | 启动自动连网、弹凭据框或重建搜索 | 本地 DOS 路径立即加载；UNC/`pulse:` 延迟到用户激活或跳过 | 事实（路径保存）；触发为推断 |
| S-14 | app/CI | 会话失败测试未被默认构建覆盖 | `build/` 中没有 `pulse_json_persistence_test.exe`；本次构建后才暴露 2 个失败 | [json_persistence_test.cpp:32](file:///e:/Projects/pulse/src/bench/json_persistence_test.cpp#L32)、[app_controllers_test.cpp:431](file:///e:/Projects/pulse/src/bench/app_controllers_test.cpp#L431) | 回归长期不可见；两套测试对空路径断言相反 | 纳入 CI；统一空路径语义后再决定哪条断言保留 | 事实（测试运行） |

### 2.3 P3

| ID | 模块 | 问题 | 证据 | 修复方向 | 判定 |
|---|---|---|---|---|---|
| P-23 | shell_host/ipc | 可预测管道名 + `FIRST_PIPE_INSTANCE`，同机用户可先占坑使 Shell 集成不可用 | [shell_client.cpp:205](file:///e:/Projects/pulse/src/ipc/shell_client.cpp#L205)、[shell_host/main.cpp:1546](file:///e:/Projects/pulse/src/shell_host/main.cpp#L1546) | 随机后缀随子进程传递；失败换名；校验服务端映像 | 事实 |
| P-24 | app/ops | 驱动器卷标重命名绕过 `IsRenameComponent`，任意字符送入 `SetNameOf` | [app_hosted_edit.cpp:395](file:///e:/Projects/pulse/src/app/app_hosted_edit.cpp#L395)、[ops_manager.cpp:3933](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L3933) | 卷标单独校验或明确允许任意标签 | 事实 |
| P-25 | app | 默认文件管理器按组应用，失败只回滚该组 | [default_file_manager.cpp:267](file:///e:/Projects/pulse/src/app/default_file_manager.cpp#L267) | 整组事务或失败恢复快照 | 事实 |
| P-26 | ipc | 通用 `RSP_DONE` 解析失败时 pending 不清理、不回调 | [shell_client.cpp:560](file:///e:/Projects/pulse/src/ipc/shell_client.cpp#L560) | 畸形响应按失败完成并断开/Abort | 事实 |
| P-27 | shell_host | 日志用 `OPEN_ALWAYS` 写用户可写 LocalAppData，未校验 reparse/ACL | [shell_host/main.cpp:104](file:///e:/Projects/pulse/src/shell_host/main.cpp#L104) | 创建时校验 reparse/owner，受限 ACL | 事实 |
| P-29 | app | 启动迁移创建线程后立即 `join` | [app_main.cpp:2175](file:///e:/Projects/pulse/src/app/app_main.cpp#L2175) | 直接同步调用，或窗口显示后异步执行 | 事实 |
| P-30 | app | `HitTestVm` 只按宽高缓存；状态变化依赖显式失效或下一次绘制 | [app_runtime.cpp:1984](file:///e:/Projects/pulse/src/app/app_runtime.cpp#L1984) | 以 view generation 为 key，或状态变更统一失效 | 事实（缓存策略）；误点击为推断 |
| P-31 | ops/fs | 路径比较大小写敏感/不敏感混用 | [delete_operation.cpp:17](file:///e:/Projects/pulse/src/ops/delete_operation.cpp#L17)、[ops_manager.cpp:527](file:///e:/Projects/pulse/src/ops/ops_manager.cpp#L527)、[recycle_undo_validation.cpp:25](file:///e:/Projects/pulse/src/ops/recycle_undo_validation.cpp#L25) | 单一路径相等策略，按卷能力区分 | 事实 |
| P-32 | ops | 7z 密码通过命令行传递，同用户进程可读 | [archive.cpp:542](file:///e:/Projects/pulse/src/ops/archive.cpp#L542) | 改用 stdin/密码文件或受保护临时文件 | 事实 |
| P-33 | index | 网络索引代理单搜索互斥 + 10ms 轮询，第二客户端 500ms 后 `ERROR_BUSY` | [network_agent_main.cpp:97](file:///e:/Projects/pulse/src/index/network_agent_main.cpp#L97) | 允许并发或明确排队语义；事件驱动等待 | 事实 |
| P-34 | docs | README 指向不存在的 `docs/automatic-updates.md`；未发现自动更新实现 | [README.md:100](file:///e:/Projects/pulse/README.md#L100) | 删除链接或补文档/实现 | 事实 |
| P-38 | ops | 剪贴板打开重试 20×5ms，UI 线程最多阻塞 100ms | [clipboard.cpp:22](file:///e:/Projects/pulse/src/ops/clipboard.cpp#L22) | 缩短重试/后台重试；失败提示 | 事实 |
| S-11 | app | 最小化/托盘隐藏时 `maximized` 丢失 | [app_main.cpp:293](file:///e:/Projects/pulse/src/app/app_main.cpp#L293) | 使用 `WPF_RESTORETOMAXIMIZED` 或独立跟踪 `IsZoomed` | 事实 |
| S-12 | app | 侧栏/快速访问 mask 未校验，负值或超范围位会异常折叠 | [session.cpp:339](file:///e:/Projects/pulse/src/app/session.cpp#L339) | 按有效位 clamp | 事实 |
| S-13 | app | 会话不恢复 sort/group/filter/history/scroll/selection | [session.h:27](file:///e:/Projects/pulse/src/app/session.h#L27) | 明确产品预期；若要恢复则扩展快照 | 事实（范围） |

---

## 3. 稳定性风险

### 3.1 已守住的部分

- generation 取消与 stale 结果拒绝。
- 删除前冻结文件身份，校验失败即停止。
- 归档临时选择文件按身份与内容复核。
- 原子 journal；配置/会话原子写。
- 缩略图缓存 16MB/128 项上限、请求去重、失败重试上限。
- preview 响应总分配预算 4MB+128KB；JSON 8MB/128 深度/262k 值限制。
- IPC `PayloadReader` 对字符串、数组和总分配做预校验。
- Shell/Document/Preview 独立进程与 job 限制。

这些应保留，不要在简化中削弱。

### 3.2 主要风险

1. **搜索生命周期不闭合（P-04）**：关闭标签不走取消路径；`pendingIndexSearches` 的 subscribe 项不会自动删除，索引服务端订阅仍存活；内容搜索 `Cancel` 不在关闭路径。
2. **context worker 竞争与无限等待（P-12）**：`query_completed/invoked` 锁内写、锁外读；`Stop` 用 `INFINITE` 等待，第三方 handler 挂起会永久占用槽位。
3. **工作线程异常边界（P-37）**：`OpsManager::WorkerThread` 与 `fs_watch::WorkerMain` 没有顶层 catch，异常会终止进程。
4. **会话持久化未闭合（S-01/S-02/S-03/S-05）**：完整会话未接入；两套写同一文件；空路径语义错误导致 2 个测试失败；root 依赖懒重建。
5. **注销/关机不保存（S-08）**：没有 `WM_QUERYENDSESSION`，系统注销可能不走 `WM_DESTROY`。
6. **撤销历史无上限且每秒全量重验（P-36）**：长时间运行 + 大量删除会形成稳定后台 IO。
7. **目录监视降级无声（P-10）**：Start 失败直接跳过，用户看到陈旧列表。
8. **回收站清空部分成功被当作完整成功（P-05）**：损坏 `$I`、孤儿 `$R`、读取失败被静默跳过。
9. **文件操作恢复策略保守**：`ops_manager.cpp:759-767` 拒绝所有带非空 `dest_dir` 的 Copy/Move 恢复。这是 fail-closed，不是 bug，但应记录为恢复能力边界。

---

## 4. 安全审计

### 4.1 已守住的基线

- **路径与重解析点**：destination guard 对 reparse、hardlink、目录 lease、句柄身份做捕获；归档 `SafeName` 拒绝绝对路径、`..`、冒号、保留名、尾点/空格，并拒绝符号链接/硬链接/anti-items；临时选择文件按内容与身份复核。
- **删除**：确认前冻结 `FileObjectReference`，失败关闭。
- **IPC**：帧头/payload 上限、`PayloadReader` 全量预校验、shell client 校验服务端 PID、index transport 调用者令牌捕获/模拟/恢复失败即失败、服务端 SCM 认证、`PIPE_REJECT_REMOTE_CLIENTS`。
- **索引授权**：文件夹大小与变更流使用 `CallerVisibility`、身份快照、本地 DOS 路径限制。
- **持久化**：`WriteUtf8FileAtomic` 唯一临时文件 + `WRITE_THROUGH` + `FlushFileBuffers` + `MoveFileEx` 替换。
- **进程隔离**：Shell context、preview、document 独立进程；document host 有 job 内存/进程上限、句柄白名单、原子 job 关联。
- **预览响应**：`ValidatePreviewResponse` 对 magic/request/generation/尺寸/属性数/总分配联合校验，先验后分配。

### 4.2 缺口

| 编号 | 风险 | 级别 | 说明 |
|---|---|---|---|
| P-03 | SYSTEM 服务解析恶意 NTFS 元数据越界 | P1（可移动介质场景可升 P0） | 需确认服务索引范围是否包含可移动盘，以及普通用户能否挂载 |
| P-01 | 删除 TOCTOU | P1 | 需目录可被另一进程/用户写入；误删是数据丢失 |
| S-10 | 会话恢复 UNC/虚拟路径自动加载 | P2（条件 P1） | 启动自动连网/弹凭据或重建搜索；session.json 用户可写 |
| P-15 | `WM_COPYDATA` 无来源校验 + 自动凭据 UI | P2 | 同完整性级别同用户；不是提权，但可诱导凭据输入、占用枚举线程 |
| P-13 | 注册表 verb 未引号命令 + 命令展开缺陷 | P2 | 第三方 verb 配置错误时可能执行前缀程序；Pulse 未提权时影响有限 |
| P-22 | 预览/文档解析器无降权、WMF 无上限 | P2 | 解析器漏洞 → 用户级执行；内存/CPU DoS 已确认 |
| P-27 | 日志/dump 位于用户可写目录且未校验链接 | P3 | 同用户可预置重解析点重定向；管理员运行时影响待确认 |
| P-32 | 7z 密码在命令行 | P3 | 同用户进程可读 |
| P-23 | 管道名可预测 DoS | P3 | 同机用户可使 Shell 集成不可用，不能冒充服务端（PID 校验） |
| P-11 | 排除目录继承缺失 | P2（隐私） | 用户显式排除的路径仍可能被索引/搜索 |

### 4.3 关于索引服务文件名搜索 ACL

`index_host.cpp` 的文件名搜索传入空 authorize，而同文件的文件夹大小、变更流使用 `CallerVisibility`。该行为已由产品方明确为效率取舍，本报告**不再将其列为缺陷**。

需要保留的记录：

- 这是“已知取舍”，不是“已修复”。
- 在多用户/域机器上，它意味着任一认证用户可以枚举其他用户无法打开的文件名。
- 如果未来产品定位转向多用户或企业部署，应重新评估；当前不作为发布阻塞项。

### 4.4 安全结论

安全基线**基本守住，但不完全**。最需要立即处理的是 P-01 和 P-03；P-22/P-15/P-13 属于防御纵深与输入边界，应在 P2 批次处理。索引 ACL 项按产品决策撤下。

---

## 5. 性能审计

| ID | 场景 | 现状与证据 | 影响量级 | 修复方向 | 优先级 |
|---|---|---|---|---|---|
| P-19 | 外部/虚拟文件拖放 | UI 线程 join staging，禁用窗口，只泵 paint | 大目录/Outlook 虚拟文件可达秒级到分钟级冻结 | 异步 staging + 进度/取消 | P1 |
| P-18 | 排序/分组/列宽 | 每次变更同步 fsync 配置 | 慢盘/AV 下每次点击数十到数百 ms | 防抖 + 后台 writer | P2 |
| P-17 | 多窗口/注册表变化 | 每窗口读最大 32MB、枚举 400 扩展、起线程 | 多窗口启动与注册表抖动时磁盘/CPU 叠加 | 全局单飞 + debounce | P2 |
| P-36 | 回收站撤销 | 每秒重开所有 `$I/$R`；undo 无 cap | 大量删除后持续 IO，撤销菜单变慢 | 惰性校验 + 变更失效 + cap | P2 |
| P-21 | 实时搜索订阅 | 每次状态广播重排全部订阅；filter epoch 全失效；单 SearchThread | 多个实时搜索页时 CPU 尖峰、结果刷新过多 | 合并/防抖、按路径增量 | P2 |
| P-20 | 内容索引 | 每文档一个 IMMEDIATE 事务、多次 stat/打开/SELECT | 首次索引吞吐低、WAL 膨胀 | 批量事务、预编译、单次打开 | P2 |
| P-30 | 鼠标/拖拽输入 | 输入路径多次 `BuildVm`；`HitTestVm` 仅按尺寸缓存 | 大目录/多侧栏项时输入延迟；重复构建 | 每次事件构建一次并传引用；轻量 hit-test | P3 |
| P-10 | 目录监视 | 500ms 超时重开目录句柄 | 句柄紧张时 IO churn | 失败退避、共享监视 | P3 |
| P-33 | 网络索引 | 单搜索互斥 + 10ms 轮询 | 第二客户端 500ms 后 busy | 并发/事件驱动 | P3 |
| P-29 | 启动迁移 | 线程创建后立即 join | 启动阻塞（一次性） | 直接同步或窗口后异步 | P3 |
| P-38 | 剪贴板 | 20×5ms 重试 | UI 最多 100ms | 缩短/后台 | P3 |
| S-01/S-14 | 完整会话启动恢复 | 当前未接入；接入后需解析并加载所有窗口/标签 | 若 session 过大或含 UNC，会放大启动延迟 | 加 cap；本地路径立即加载，UNC/虚拟延迟 | P2（条件） |

**性能结论**：常见浏览、缩略图、后台枚举路径合格；真正会突破底线的是 P-19（直接冻结 UI）和 P-18（每次交互 fsync）。P-17/P-21/P-36 属于“多窗口 + 多搜索 + 大量删除”叠加后的系统性放大，应在 P2 批次一起处理。

---

## 6. 补丁收敛清单

| 收敛项 | 现有分散实现 | 统一根因方案 | 可删除/合并 |
|---|---|---|---|
| 路径相等 | `delete_operation.cpp:17` 大小写敏感；`ops_manager.cpp:527/2459` 不敏感；`recycle_undo_validation.cpp` 混用；`duplicate_scan.cpp` 另一套 | 一个 `PathEqual(path, path, policy)`，Windows 卷默认不敏感，case-sensitive 目录精确比较 | 4-6 处比较 |
| 路径拼接/父/叶/Within | `JoinPath` 4 份、`ParentOf` 2 份、`Leaf` 4-5 份、`Within` 4 份、`StartsWithPath` 2 份 | 收敛到 `pulse::path`；保留现有 `EqualInsensitive`/`StripExtendedPathPrefix` | 约 150-250 行 |
| 回收站 `$I` 解析 | `fs_recycle.cpp` 与 `shell_host/recycle_fileop.cpp` 两套 | 一个解析器，统一尺寸/字段/错误语义 | 约 60-100 行 |
| 文件名校验 | `IsRenameComponent` 严格；`batch_rename::IsValidFileName` 无保留名；`SafeName` 归档专用 | 一个策略对象，模式：rename/archive/label | 约 40-60 行 |
| 命令引号 | `ExpandShellCommand`、`TerminalCommandLine`、`SplitLaunchCommand` 各自处理 | 复用 `common/command_line.h::QuoteWindowsArgument`，单次占位符扫描 | 约 40-60 行 |
| 搜索取消 | `StartLoadingPath`、`CancelActiveContentSearch`、`tab_transfer`、`CloseLayoutTab` 各写一部分 | 单一 `TeardownTabSearch(tab)`，所有销毁/导航路径调用 | 约 100 行 + 消除 P-04 |
| 撤销有效性 | 每秒全量重验 + 每个操作后重验 + 事件失效 | 点击撤销时惰性校验 + 目录变更显式失效 | 约 150 行，消除 P-36 |
| 内容结果写入 | `ApplyChanges` 非流式全量重建 vs `StreamUpsert` 增量 | 只保留增量路径，非流式走同一实现 | 约 100-150 行 |
| 托盘/标题栏拖放 | 标题栏 staging，托盘不 staging | 两条路径共用 prepare 步骤 | 约 30 行，消除 P-08 |
| 会话持久化 | `SessionSnapshot` 与 `WindowSessionSnapshot` 两套写同一文件；完整会话未接入 | 拆文件或统一 schema；启用时复用 `RebuildLayoutRoot`/`MakePresetTree` | 约 200 行（不启用）或消除双写 |
| 单实例 Acquire | 启动迁移前后各调用一次 | 只 Acquire 一次，保留结果 | 约 10 行 |
| `RSP_DONE` 语义 | 通用分支与 launch 分支处理不一致 | 畸形响应统一按失败完成并断开/Abort | 约 20 行 |

**原则**：先收敛安全相关口径（路径、删除、回收站解析），再收敛性能相关（撤销校验、搜索取消），最后处理会话 schema。不要在同一提交里同时改行为与大规模重命名。

---

## 7. 代码简化清单

| 候选 | 现状 | 可减/可拆 | 风险 | 收益 |
|---|---|---|---|---|
| 未启用的完整会话 | 生产无调用者；仅测试/selftest | 删除约 200 行 + 多窗口测试；或完成接入 | 低（若确定不启用） | 去除“看似支持多窗口恢复”的虚假能力 |
| 两套会话 schema | 同一 `session.json`，字段集互斥 | 合并为一套完整 schema，或拆两个文件 | 中（需迁移旧文件） | 消除互相覆盖 |
| 路径/回收站/文件名/命令 helper | 多份语义分叉 | 合并约 300-500 行 | 中（必须逐项对齐大小写、长路径、错误码） | 降低安全校验口径漂移 |
| 撤销校验 | 周期全量 + 事件失效 + 无 cap | 重构约 150-250 行 | 中（回收站状态可能变化） | 去掉稳定后台 IO，简化状态机 |
| 内容结果双写路径 | 增量与全量两套 | 合并约 100-150 行 | 中（排序/分页语义需对齐） | 减少重复 SQL 与缓存失效路径 |
| `ops_manager.cpp` 4,413 行 | 队列、撤销、恢复、传输、打开、卷标混合 | 拆分为 4-6 个文件，不直接减行 | 低-中（移动代码） | 审查/测试边界清晰 |
| `index_engine.cpp` 4,095 行 | MFT/USN、过滤、聚合、遍历混合 | 按 USN/MFT/过滤/聚合拆分 | 中（共享锁与状态多） | 安全边界更易审计 |
| `app_input.cpp` 3,822 行 / `fluent_components.cpp` 2,780 行 | 输入分发/控件绘制集中 | 按区域拆分 | 低 | 可维护性 |
| CMake/测试 | 134 targets、119 测试、1,279 行 | 按域合并为约 40-50 个测试目标，CMake 约 700 行 | 低-中（CI 脚本需同步） | 构建/链接时间与维护成本下降 |

**明确结论**：不存在单个“几千行可压到几百行”的子系统。Pulse 的大文件主要是功能密度，不是层层转发。简化应聚焦“重复语义”和“死能力”，不要为了行数拆散有状态的核心（尤其 `index_engine` 的锁与 `ops_manager` 的 journal）。

---

## 8. 复用/重复造轮子

| 现有可复用 | 被重复实现的地方 | 建议 |
|---|---|---|
| `pulse::path::EqualInsensitive`、`StripExtendedPathPrefix`、`IsDriveRootPath` | 多处本地 `SamePath/JoinPath/ParentOf/Leaf/Within` | 统一入口，删除本地副本 |
| `common/file_identity.h` 的 `ReadFileIdentity` | `shell_host/deletion_identity.cpp` 自己实现且 `\\?\` 处理不同 | 合并；删除安全校验的第二套口径 |
| `FileObjectReference::Delete` | 永久删除仍走 Shell 路径 | 直接复用，解决 P-01 根因 |
| `common/command_line.h::QuoteWindowsArgument` | `ExpandShellCommand`、`TerminalCommandLine` | 复用，删除手写引号 |
| `WriteUtf8FileAtomic` | 各处配置写入 | 保留；将 UI 路径改为防抖后台调用 |
| `HitTestVm` | 输入路径反复 `BuildVm` | 扩展为按 generation 失效的单一入口 |
| 归档 `SafeName` | `batch_rename::IsValidFileName`、`IsRenameComponent` | 提取公共策略，按模式启用保留名/尾点检查 |
| `RebuildLayoutRoot` / `MakePresetTree` | `RestoreLayoutTab` 手写 pane 构造并留下 root=null | restore 内复用现有树构建，再应用比例 |
| `DefaultNavigationRoot` | 会话空路径处理与回退不一致 | 统一“空快照回退”和“空路径 = This PC”的语义 |
| 缩略图缓存 | 预览/图标各自解码 | 已较好；不要为“统一”引入跨进程缓存 |
| `ComPtr`/WIL 风格 RAII | 少量裸 COM 指针 | 保持现状，避免引入 WIL 全量依赖 |

---

## 9. 过度设计/冗余回退

| 项 | 现象 | 判断 | 建议 |
|---|---|---|---|
| 两套会话持久化 | 完整会话与偏好快照并存，写同一文件 | 功能重复 + schema 冲突 | 选一套：启用完整会话，或删除死代码 |
| 多窗口会话解析未接入 | `LayoutTabsToJson/ParseLayoutTabs` 等仅测试使用 | 死能力 | 删除或接入，二选一 |
| 单实例 `Acquire` 两次 | 启动迁移前后各一次 | 冗余 | 合并为一次 |
| 回收站 `$I` 两套解析 | 校验口径可能不同 | 重复设计 | 合并 |
| 删除前“再校验一次” | 已有冻结引用 + Shell 前复核，但仍是路径删除 | 补丁式防御，未解决根因 | 改为句柄删除 |
| 搜索取消分散在 4 处 | 关闭标签反而没有 | 补丁式覆盖不全 | 单一 teardown |
| 托盘与标题栏拖放分叉 | staging 只在一侧 | 行为不一致 | 共用 prepare |
| `RSP_DONE` 与 launch 分支处理不一致 | 畸形响应一边完成、一边挂起 | 冗余/不一致 | 统一失败完成语义 |
| `SplitLaunchCommand` 只在提权回退使用 | 正常启动直接 `CreateProcessW` | 两套启动语义 | 统一 `ShellExecuteEx` 或显式解析 |
| 每窗口 verb 缓存 | 全局数据按窗口重复加载 | 过度实例化 | 全局 owner 单飞 |
| 134 个测试可执行文件 | 每个小用例一个 exe | 构建碎片化 | 按域合并，保留失败定位能力 |
| `explicit_entry_refreshes` 裸 `Tab*` map | 有周期 prune，但生命周期靠扫描 | 复杂但可工作 | 若改 teardown，可改为按 generation 键 |

---

## 10. 建议方案与优先级

### P0（立即，发布前）

- 无确认 P0。
- 条件 P0：
  - 若 `PulseIndex` 服务索引可移动介质，且普通用户可挂载 crafted NTFS 卷，P-03 升为 P0。
  - 若产品转向多用户/域部署，已接受的索引文件名 ACL 取舍需要重新评估（当前不作为缺陷）。

### P1（第一批）

1. **P-01**：永久删除改为句柄删除；回收站删除把句柄/文件 ID 传入 Shell host，删除前用句柄复核，不匹配即失败关闭。验证：确认后替换同名文件，原文件不得被删。
2. **P-03**：统一 USN/MFT 记录边界校验，所有入口复用。验证：构造超长 `FileNameLength`/越界 `FileNameOffset` 的合成记录，服务不得越界且应标记索引不完整。
3. **P-19**：外部拖放 staging 异步化；UI 显示进度/取消；完成后投递 drop。
4. **P-04**：统一 `TeardownTabSearch`，所有关闭/转移/销毁路径调用。
5. **P-12**：context worker 字段 atomic 化或统一锁；`Stop` 有界等待并终止 worker。
6. **S-01/S-02/S-03**（若启用完整会话）：拆持久化文件；增加全窗口 CaptureSession；修复空路径 This PC 语义；让 2 个失败测试通过。

### P2（第二批，按模块分批）

按“ops/fs → index → app → shell/ipc → session”分批，每批只改一个语义域：

- ops/fs：P-05、P-06、P-36、P-37、P-31。
- index：P-11、P-20、P-21、P-35。
- app：P-07、P-08、P-09、P-17、P-18、P-30、S-04..S-09、S-14。
- shell/ipc：P-13、P-14、P-15、P-22。
- 会话：S-04 分栏比例、S-05 root/fallback、S-06 cap、S-07 undo 版本、S-08 全窗口保存、S-09 顶层投影、S-10 UNC/虚拟路径策略。

### P3（清理）

P-23、P-24、P-25、P-26、P-27、P-29、P-32、P-33、P-34、P-38、S-11、S-12、S-13。

### 不建议现在做

- 合并 preview/document/index 进程。
- 替换 Direct2D/DirectComposition。
- 引入跨平台抽象或重型事件总线。
- 把所有测试合并成单个巨型 exe。
- 在未决定是否启用完整会话前，同时重构两套 schema。

---

## 11. 回归测试建议

### 11.1 会话持久化（新增/修正）

- **空路径语义**：`RestoreLayoutTab` 保存一个空路径窗格，恢复后仍为 This PC；混合“空路径 + 文件夹”时顺序和数量不变。
- **失败用例**：`pulse_json_persistence_test` 当前 2 项失败，应作为回归基线；不要把测试删掉来“通过”。
- **分栏比例**：保存 0.7/0.3，恢复后 root 的 ratio 仍为 0.7/0.3；数量/范围非法时回退 0.5。
- **root**：恢复后立即检查 `tab.root` 非空，且 `CollectPanes` 顺序与快照一致。
- **空/损坏 session**：空 `layout_tabs` 应得到明确默认页（Home 或 This PC，需产品决定）；超大 tab/pane/group 应被截断。
- **双写冲突**：先 `SaveSession` 再 `SaveSessionPreferences`，再 `LoadSession`，导航不得丢失（或明确拆文件后不再存在该场景）。
- **全窗口保存**：两个窗口不同侧栏/标签，退出后两个窗口都能恢复；注销/关机路径也要覆盖。
- **隐藏最大化**：最大化后隐藏到托盘再退出，重启仍最大化。
- **UNC/虚拟路径**：含 UNC/`pulse:search:` 的 session 不应在启动时自动弹凭据或启动扫描。
- **测试覆盖**：把 `pulse_json_persistence_test` 纳入默认 CI；`pulse_app_controllers_test` 与 `json_persistence_test` 的空路径断言必须统一。

### 11.2 删除/回收站/撤销

- 确认后替换同名文件（句柄删除必须拒绝）。
- 回收站 `$I` 损坏/超限/孤儿 `$R`：清空应报告部分失败。
- RestoreRecycle 失败/取消后 undo 仍可重试。
- 大量回收站删除后撤销菜单响应时间；undo cap。
- 驱动器根删除拒绝；卷标重命名校验。

### 11.3 索引/搜索

- 合成越界 USN/MFT 记录：无崩溃、无索引污染、标记 gap。
- 排除/隐藏目录下新建/重命名文件不可搜索。
- 关闭搜索标签后索引更新不再触发该标签的合并/扫描。
- 内容索引批量事务：崩溃后 WAL 可恢复，文档不重复。
- 实时搜索订阅在多次索引变更下的刷新次数上限。

### 11.4 Shell/IPC

- 长路径/UNC/`\\?\`：元数据、固定到快速访问、预览初始化。
- 注册表 verb：带空格未引号路径、文件名含 `%1`、目录以 `\` 结尾。
- ctx handler 挂起：Stop 有界，进程槽位回收。
- 畸形 `RSP_DONE`：pending 清理，不挂起。
- 管道被占：Shell 集成失败可见，不静默。
- 单实例转发失败：有错误提示或自启窗口。

### 11.5 App/UI/性能

- 托盘不可用时关闭窗口：标签不重置。
- Outlook/RDP 虚拟文件拖到标题栏与托盘：两者都可用。
- 注销/关机：undo/托盘/窗口状态落盘。
- 多窗口 + 注册表变化：verb 缓存只扫描一次。
- 10k 条目下滑动/点击/排序：输入延迟与 hit-test 正确性。
- WMF/EMF 大文件：预览进程内存上限、UI 不冻结。
- 定向性能：1k 回收站撤销项的每秒 IO；10 个实时搜索标签的索引变更 CPU；1GB 内容索引吞吐与 WAL；多窗口启动 verb 读取次数。

---

## 12. 风险与待确认

1. **索引 ACL 取舍**：已按产品决策撤下，不作为缺陷。若未来转向多用户/域部署，需要重新评估；当前报告只把它记录为已知风险。
2. **P-03 可利用性**：需要确认 `PulseIndex` 是否索引可移动介质、普通用户能否挂载 crafted NTFS 卷。如果两者都成立，应从 P1 升为 P0。
3. **`index_shard.cpp:393-424` 待确认**：非活动 slot 在 manifest 发布前被替换；若 `SaveShardManifest` 失败，旧 manifest 的 `previous_slot` 可能指向已被覆盖的 slot。需要完整恢复路径与断电/写失败测试，未确认前不计入缺陷。
4. **P-22 沙箱状态**：代码中只看到 job 限制和 `BELOW_NORMAL_PRIORITY_CLASS`，未看到低完整性/AppContainer 令牌；需确认安装器或启动器是否在外部设置了受限令牌。
5. **P-15 的 UIPI 边界**：`WM_COPYDATA` 受 UIPI 限制，低完整性进程默认不能发送；报告中的“同用户进程”指同一完整性级别。
6. **P-13 的命令劫持条件**：需要第三方注册表 verb 本身使用未引号的可执行路径；Pulse 未提权时影响限于用户权限，不应描述成提权漏洞。
7. **S-01/S-02/S-03 的状态**：按当前工作树，完整会话没有生产入口；如果你在另一个分支/工作树完成了接入，应以那个版本为准。当前树的 2 个失败测试是实际运行结果。
8. **失败测试的代表性**：`pulse_json_persistence_test` 是定向构建后运行的；默认 `build/` 没有该目标，说明 CI 可能未覆盖。
9. **UI 覆盖不足**：UI 子代理达到轮次上限，未完成全量 UI 审计；IME、触摸、辅助功能、窄布局未验证。
10. **未做运行验证**：除会话定向测试外，没有构建/运行完整应用、`pulse.exe --selftest`、第三方 Shell 扩展、网络共享、恶意卷、多用户实测。
11. **工作树状态**：审计对象包含 53 个未提交修改；若这些修改被回退/重写，行号与结论需要重新核对。
12. **既有文档**：`docs/` 下已有 2026-10-22/23 的架构与 bug 审计记录；修复时应先对照 `docs/issue-pending-fix-checklist-2026-10-23.md`，避免重复或冲突。

---

## 13. 附录：证据索引

### 13.1 关键证据文件

- 索引与预览：[src/index/index_host.cpp](file:///e:/Projects/pulse/src/index/index_host.cpp)、[src/index/index_engine.cpp](file:///e:/Projects/pulse/src/index/index_engine.cpp)、[src/index/content_index.cpp](file:///e:/Projects/pulse/src/index/content_index.cpp)、[src/index/document_host_main.cpp](file:///e:/Projects/pulse/src/index/document_host_main.cpp)、[src/preview_host/metafile_raster.cpp](file:///e:/Projects/pulse/src/preview_host/metafile_raster.cpp)
- 删除与撤销：[src/shell_host/main.cpp](file:///e:/Projects/pulse/src/shell_host/main.cpp)、[src/shell_host/deletion_identity.cpp](file:///e:/Projects/pulse/src/shell_host/deletion_identity.cpp)、[src/ops/delete_operation.cpp](file:///e:/Projects/pulse/src/ops/delete_operation.cpp)、[src/ops/ops_manager.cpp](file:///e:/Projects/pulse/src/ops/ops_manager.cpp)、[src/ops/recycle_undo_validation.cpp](file:///e:/Projects/pulse/src/ops/recycle_undo_validation.cpp)、[src/fs/fs_recycle.cpp](file:///e:/Projects/pulse/src/fs/fs_recycle.cpp)
- App/UI：[src/app/app_navigation.cpp](file:///e:/Projects/pulse/src/app/app_navigation.cpp)、[src/app/app_input.cpp](file:///e:/Projects/pulse/src/app/app_input.cpp)、[src/app/app_exit.cpp](file:///e:/Projects/pulse/src/app/app_exit.cpp)、[src/app/app_main.cpp](file:///e:/Projects/pulse/src/app/app_main.cpp)、[src/app/app_commands.cpp](file:///e:/Projects/pulse/src/app/app_commands.cpp)、[src/app/app_runtime.cpp](file:///e:/Projects/pulse/src/app/app_runtime.cpp)、[src/ops/drop_staging.cpp](file:///e:/Projects/pulse/src/ops/drop_staging.cpp)、[src/ops/clipboard.cpp](file:///e:/Projects/pulse/src/ops/clipboard.cpp)
- 会话持久化：[src/app/session.cpp](file:///e:/Projects/pulse/src/app/session.cpp)、[src/app/session.h](file:///e:/Projects/pulse/src/app/session.h)、[src/app/session_tab_model.cpp](file:///e:/Projects/pulse/src/app/session_tab_model.cpp)、[src/app/app_model.cpp](file:///e:/Projects/pulse/src/app/app_model.cpp)、[src/app/app_runtime.cpp](file:///e:/Projects/pulse/src/app/app_runtime.cpp)
- 会话测试：[src/bench/json_persistence_test.cpp](file:///e:/Projects/pulse/src/bench/json_persistence_test.cpp)、[src/bench/session_windows_test.cpp](file:///e:/Projects/pulse/src/bench/session_windows_test.cpp)、[src/bench/app_controllers_test.cpp](file:///e:/Projects/pulse/src/bench/app_controllers_test.cpp)、[src/bench/multiwindow_application_test.cpp](file:///e:/Projects/pulse/src/bench/multiwindow_application_test.cpp)
- IPC/Shell：[src/ipc/protocol.h](file:///e:/Projects/pulse/src/ipc/protocol.h)、[src/ipc/shell_client.cpp](file:///e:/Projects/pulse/src/ipc/shell_client.cpp)、[src/shell_host/ctx_session_process.cpp](file:///e:/Projects/pulse/src/shell_host/ctx_session_process.cpp)、[src/shell_host/ctx_handlers.cpp](file:///e:/Projects/pulse/src/shell_host/ctx_handlers.cpp)、[src/shell_host/shell_launch_worker.cpp](file:///e:/Projects/pulse/src/shell_host/shell_launch_worker.cpp)、[src/app/single_instance_coordinator.cpp](file:///e:/Projects/pulse/src/app/single_instance_coordinator.cpp)
- 持久化/公共：[src/common/utf8_file.h](file:///e:/Projects/pulse/src/common/utf8_file.h)、[src/common/json_utils.h](file:///e:/Projects/pulse/src/common/json_utils.h)、[src/app/session.cpp](file:///e:/Projects/pulse/src/app/session.cpp)、[src/ui/thumbnail_cache.cpp](file:///e:/Projects/pulse/src/ui/thumbnail_cache.cpp)
- 文档：[README.md](file:///e:/Projects/pulse/README.md)、[docs/issue-pending-fix-checklist-2026-10-23.md](file:///e:/Projects/pulse/docs/issue-pending-fix-checklist-2026-10-23.md)

### 13.2 本次实际运行的测试

```text
cmd //c "call scripts\vcvars.bat && cmake --build build_release_clean --target pulse_json_persistence_test pulse_session_windows_test"
./build_release_clean/pulse_json_persistence_test.exe
./build_release_clean/pulse_session_windows_test.exe
./build/pulse_app_controllers_test.exe
```

结果：

- `pulse_json_persistence_test`：exit 1，2 项失败（空路径 This PC）。
- `pulse_session_windows_test`：6/6 通过（仅 JSON 序列化往返）。
- `pulse_app_controllers_test`：通过，但其“空路径应跳过”的断言与 `json_persistence_test` 相反。
- `build/` 是旧 NMake 目录，CMake 因头依赖不安全拒绝复用；本次使用 `build_release_clean`（Ninja）。

### 13.3 审计边界声明

本报告以当前工作树为准，只做静态审阅与定向测试，不构成完整运行验证。修复时应以最小受影响目标构建为起点，按 P0 → P1 → P2 批次推进；不要用“编译通过”替代功能验证，也不要在安全修复中夹带无关重构。

---

**报告结束。**
