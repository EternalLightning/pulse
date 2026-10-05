# Pulse 功能接口与实现边界审阅

审阅日期：2026-10-05。基于当前工作区（含已有未提交修改）的生产 CMake 目标、公开头文件及关键调用链。本文仅记录审阅结论，不代表已实施整改，不覆盖测试夹具的全部内部符号。

> 下文为修改前的架构审阅快照。后续按“保持用户可观察行为不变”实施的 F2 第一阶段、F4、F5、F8，以及实际验证与仍未处理项，见 [行为保持优化报告](E:/Projects/pulse/docs/architecture-preserving-optimization.md)。不要将下文历史证据路径和“尚未实施”结论当作当前实施状态。

这里的“一个接口对应一个实现”指一个功能有明确的生产入口和执行责任人。协调器、传输适配器和后端不是同一接口的三套实现；只有输入、语义和责任相同的重复代码才应合并。不同权限、文件类型或执行能力所需的后端必须保留。下表按实际功能接口族汇总，而非将每个 getter、布局辅助函数和 Win32 回调当成独立功能。

## 1. 生产程序及实际执行位置

| 目标/产物 | 生产责任 | 已核实的入口与边界 |
| --- | --- | --- |
| `pulse` / `Pulse.exe` | 主窗口、状态、导航、异步目录加载、操作调度、UI、查询客户端 | [CMake UI 源文件筛选](E:/Projects/pulse/CMakeLists.txt)，[app_main.cpp](E:/Projects/pulse/src/app/app_main.cpp)。默认 `PULSE_WITH_SELFTEST=OFF` 时不编译本地 Engine；主程序仍包含网络索引实现文件，原因见发现 F5。 |
| `pulse_index` / `Pulse.Index.exe` | 本地名称索引的服务/会话 helper，以及按命令参数选择的网络、内容 agent | [index_host.cpp](E:/Projects/pulse/src/index/index_host.cpp) `wWinMain` 分派 `--network-agent`、`--content-instant-agent`、`--content-index-agent`、`--content-index-observer`、`--content-agent`；这些角色不是同时存在于一份 UI Engine。 |
| `pulse_shell` / `Pulse.Shell.exe` | 删除/回收/恢复、部分创建/重命名、第三方上下文菜单 COM | [shell_host/main.cpp](E:/Projects/pulse/src/shell_host/main.cpp)，[protocol.h](E:/Projects/pulse/src/ipc/protocol.h)。不可因“统一实现”将第三方 COM 和删除执行搬回窗口线程。 |
| `pulse_preview` / `Pulse.Preview.exe` | 缩略图、静态预览、文本/十六进制、属性、Shell 缩略图提供器 | [preview_host/main.cpp](E:/Projects/pulse/src/preview_host/main.cpp)，[preview_protocol.h](E:/Projects/pulse/src/ipc/preview_protocol.h)。位图使用共享映射，文字/属性走管道。 |
| `pulse_document` / `Pulse.Document.exe` | OOXML、Office IFilter、PDFium/PDF IFilter 文本提取 | [document_host_main.cpp](E:/Projects/pulse/src/index/document_host_main.cpp)，[document_protocol.h](E:/Projects/pulse/src/index/document_protocol.h)。CMake 明确将文档解析器排除在 UI 和持久索引写入体之外。 |

共享库已有明确用途：`pulse_text` 是文本解码，`pulse_user_storage` 是用户存储定位/迁移，`pulse_localization` 是资源本地化，`pulse_document_reader` 是文档进程读取适配，`pulse_content_task` 是交互扫描补充，`pulse_fs_watch` 是文件观察，`pulse_sqlite` 是 SQLite/FTS5，`pulse_crash_reporter` 是崩溃记录。静态库/源码同时链接多个目标不等于运行了多份功能。

## 2. 功能接口清单

表中“唯一实现/链路”是当前入口的生产实现归属；标注“需收敛”的行不是声称已完成合并。接口的内联小函数以声明头本身为实现。

### 2.1 应用、目录、模型与个性化数据

| 功能 | 当前入口/契约 | 唯一实现/链路与边界 |
| --- | --- | --- |
| 启动、消息调度、程序退出 | `wWinMain`；`RequestApplicationExit/CompleteApplicationExit` | [app_main.cpp](E:/Projects/pulse/src/app/app_main.cpp)、[app_exit.h](E:/Projects/pulse/src/app/app_exit.h)/[app_exit.cpp](E:/Projects/pulse/src/app/app_exit.cpp)；退出协调器停止现有 worker/agent。 |
| 单实例及外部打开转发 | `SingleInstanceCoordinator` | [single_instance_coordinator.h](E:/Projects/pulse/src/app/single_instance_coordinator.h)/[single_instance_coordinator.cpp](E:/Projects/pulse/src/app/single_instance_coordinator.cpp)；独立实例到主窗口的请求边界。 |
| 普通目录加载 | `StartLoadingPath` → `WorkerPool::Refresh` | [app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp) → [app_worker.h](E:/Projects/pulse/src/app/app_worker.h)/[app_worker.cpp](E:/Projects/pulse/src/app/app_worker.cpp) → [fs_enum.h](E:/Projects/pulse/src/fs/fs_enum.h)/[fs_enum.cpp](E:/Projects/pulse/src/fs/fs_enum.cpp) `EnumerateDirectory`；IO/排序在 worker，结果以 generation 接收。 |
| 指定路径集合加载 | `WorkerPool::LoadPaths` | 同一 [app_worker.cpp](E:/Projects/pulse/src/app/app_worker.cpp)；星标、标签等虚拟集合复用目录条目模型，不是第二个目录枚举器。 |
| 目录快照、身份、LRU | `SnapshotStore`、`SnapshotPtr`、`QueryDirectoryIdentity` | [fs_snapshot.h](E:/Projects/pulse/src/fs/fs_snapshot.h)/[fs_snapshot.cpp](E:/Projects/pulse/src/fs/fs_snapshot.cpp)；`shared_ptr<const vector<DirEntry>>`，按项数和字节预算淘汰。 |
| 目录变化观察 | `DirWatch/DirWatchSet::Start/Sync` | [fs_watch.h](E:/Projects/pulse/src/fs/fs_watch.h)/[fs_watch.cpp](E:/Projects/pulse/src/fs/fs_watch.cpp)；[app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp) 应用通知或安排刷新，不在观察层画 UI。 |
| UNC 探测、离线列表缓存 | `StartUncProbe`、`SaveNetSnapshot/LoadNetSnapshot` | [fs_net_cache.h](E:/Projects/pulse/src/fs/fs_net_cache.h)/[fs_net_cache.cpp](E:/Projects/pulse/src/fs/fs_net_cache.cpp)；应用层调度在内联 [unc_probe_scheduler.h](E:/Projects/pulse/src/app/unc_probe_scheduler.h) 及导航模块。与名称索引缓存的用途不同。 |
| 回收站列表与占用查询 | `EnumerateRecycleBin/QueryRecycleBinInfo/ReadRecycleIndex` | [fs_recycle.h](E:/Projects/pulse/src/fs/fs_recycle.h)/[fs_recycle.cpp](E:/Projects/pulse/src/fs/fs_recycle.cpp)；worker 加载列表。只读列表接口不应与回收站删除后端合并。 |
| 导航、后退前进、切换/拆分布局 | `NavigateTo/GoBack/GoForward/ApplyLayoutPreset`；`Tab/WindowTabs/SplitContainer` | [app_navigation.h](E:/Projects/pulse/src/app/app_navigation.h)/[app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp) 负责启动副作用；[app_model.h](E:/Projects/pulse/src/app/app_model.h)/[app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp) 负责状态变换。两层同名 `NavigateTo` 是协调与模型两个责任。 |
| 标签页分组、固定、关闭菜单 | `TabController` | [tab_controller.h](E:/Projects/pulse/src/app/tab_controller.h)/[tab_controller.cpp](E:/Projects/pulse/src/app/tab_controller.cpp)，变更回调连接应用和模型。 |
| 列式浏览 | 列视图模型相关契约、应用列视图入口 | [column_view_model.h](E:/Projects/pulse/src/app/column_view_model.h)/[column_view_model.cpp](E:/Projects/pulse/src/app/column_view_model.cpp)，[app_column_view.h](E:/Projects/pulse/src/app/app_column_view.h)/[app_column_view.cpp](E:/Projects/pulse/src/app/app_column_view.cpp)；呈现在 [ui_column_strip.cpp](E:/Projects/pulse/src/ui/ui_column_strip.cpp)。 |
| 排序、名称过滤、日期分组 | `EntryLess/SetFolderSortMode`；`CollectFilterMatches`；日期分组接口 | [entry_sort.h](E:/Projects/pulse/src/app/entry_sort.h)/[entry_sort.cpp](E:/Projects/pulse/src/app/entry_sort.cpp)，[app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp)，[date_groups.h](E:/Projects/pulse/src/app/date_groups.h)/[date_groups.cpp](E:/Projects/pulse/src/app/date_groups.cpp)。名称检索排序另有索引规则，不能仅因字段近似就替换。 |
| 目录视图偏好 | `FolderViewPrefs::Find/Set/ReadJson/AppendJson` | [folder_view_prefs.h](E:/Projects/pulse/src/app/folder_view_prefs.h)/[folder_view_prefs.cpp](E:/Projects/pulse/src/app/folder_view_prefs.cpp)。 |
| 首页、驱动器目录/卷标呈现 | `HomeCatalog::Refresh/Fill/UpdateDriveLabel` | [home_catalog.h](E:/Projects/pulse/src/app/home_catalog.h)/[home_catalog.cpp](E:/Projects/pulse/src/app/home_catalog.cpp)；卷标写入由 OpsManager 执行，展示/读取和写入是不同责任。 |
| 工作区、网络位置、标签、星标、最近访问、快速访问 | `PlacesCatalog` | [places.h](E:/Projects/pulse/src/app/places.h)/[places.cpp](E:/Projects/pulse/src/app/places.cpp)；目录路径和 JSON 索引的规范拥有者，UI 菜单由 app commands/quick_access 适配。 |
| 标签 ADS 与发现 | `ReadTagAdsV2/WriteTagAdsV2`、`QueueVisibleTagDiscovery/QueueTagAds` | [places.cpp](E:/Projects/pulse/src/app/places.cpp) 的格式实现，应用任务调度在 [app_runtime.cpp](E:/Projects/pulse/src/app/app_runtime.cpp)/[app_commands.cpp](E:/Projects/pulse/src/app/app_commands.cpp)；[tag_discovery_cache.h](E:/Projects/pulse/src/app/tag_discovery_cache.h) 为发现结果去重缓存。 |
| 暂存托盘 | `StagingTray` | [app_model.h](E:/Projects/pulse/src/app/app_model.h)/[app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp)；操作在 [app_ops_ui.cpp](E:/Projects/pulse/src/app/app_ops_ui.cpp)，呈现/动画由 runtime/UI 适配。与通知区域 `TrayController` 是两个不同功能。 |
| 会话保存恢复 | `SaveSession/LoadSession/CaptureLayoutTab/RestoreWindowTabs` | [session.h](E:/Projects/pulse/src/app/session.h)/[session.cpp](E:/Projects/pulse/src/app/session.cpp)；存布局、路径、托盘、撤销 JSON 等，不拥有操作执行。 |
| 全局偏好 | `AppPrefs::Load/Save/ToJson/FromJson` | [app_prefs.h](E:/Projects/pulse/src/app/app_prefs.h)/[app_prefs.cpp](E:/Projects/pulse/src/app/app_prefs.cpp)，应用入口应通过设置协调器写入。 |
| 文件详情元数据与链接解析 | `FetchDetailsMeta`；`ResolveLink/ResolveLinksInPlace` | [details_meta.h](E:/Projects/pulse/src/app/details_meta.h)/[details_meta.cpp](E:/Projects/pulse/src/app/details_meta.cpp)，[link_resolve.h](E:/Projects/pulse/src/app/link_resolve.h)/[link_resolve.cpp](E:/Projects/pulse/src/app/link_resolve.cpp)；非窗口线程任务，缓存/消息投递由 runtime/worker 协调。 |
| 文件夹大小及包含项计数 | `FolderSizes::Sync/Calculate/Get`；`StartDetailsSizeWalk` | [folder_sizes.h](E:/Projects/pulse/src/app/folder_sizes.h)/[folder_sizes.cpp](E:/Projects/pulse/src/app/folder_sizes.cpp) 是列表侧；[app_runtime.cpp](E:/Projects/pulse/src/app/app_runtime.cpp) 是详情侧。存在真正重复扫描，需收敛，见 F1；索引汇总后端仍需保留。 |

### 2.2 名称、内容、变化与重复文件搜索

| 功能 | 当前入口/契约 | 唯一实现/链路与边界 |
| --- | --- | --- |
| 高级搜索输入编译与展示 | `CompileSearchQuery/ParseSearchQuery/SplitSearchQueryText` | [search_query.h](E:/Projects/pulse/src/app/search_query.h)/[search_query.cpp](E:/Projects/pulse/src/app/search_query.cpp)；高级搜索表单适配，不是另一个文件匹配引擎。 |
| 名称查询语言、种类、拼音与排序评分 | `ParseQuery/RankName`、相关 query/pinyin 契约 | [index_query.h](E:/Projects/pulse/src/index/index_query.h)/[index_query.cpp](E:/Projects/pulse/src/index/index_query.cpp)，[search_kinds.cpp](E:/Projects/pulse/src/index/search_kinds.cpp)、[filename_pinyin.cpp](E:/Projects/pulse/src/index/filename_pinyin.cpp)；多个 provider 共用语义。 |
| 本地名称索引客户端 | `IndexClient::Start/SearchAsync/TakeResult/CancelSession` | [index_client.h](E:/Projects/pulse/src/index/index_client.h)/[index_client.cpp](E:/Projects/pulse/src/index/index_client.cpp) → [index_protocol.h](E:/Projects/pulse/src/index/index_protocol.h) → [index_host.cpp](E:/Projects/pulse/src/index/index_host.cpp) → `Engine::Search`。 |
| 本地名称索引构建、增量、持久化 | `Engine::Start/Search/RequestRebuild` | [index_engine.h](E:/Projects/pulse/src/index/index_engine.h)/[index_engine.cpp](E:/Projects/pulse/src/index/index_engine.cpp) 为拥有者，MFT、路径、delta、shard、visibility、pinyin、folder_sizes 是其分工实现文件，非相互替代的引擎。 |
| 网络名称索引客户端 | `NetworkAgentClient::SearchAsync/Roots/AddRoot/RemoveRoot/Rebuild` | [network_agent_client.h](E:/Projects/pulse/src/index/network_agent_client.h)/[network_agent_client.cpp](E:/Projects/pulse/src/index/network_agent_client.cpp) → [network_agent_protocol.h](E:/Projects/pulse/src/index/network_agent_protocol.h) → [network_agent_main.cpp](E:/Projects/pulse/src/index/network_agent_main.cpp) → `NetworkIndex`。 |
| 网络名称索引构建、观察、查询 | `NetworkIndex` | [network_index.h](E:/Projects/pulse/src/index/network_index.h)/[network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp)；UNC 用户权限/网络枚举后端，与本地原始卷 MFT/USN 后端不同。 |
| 名称搜索联合与分页 | `DispatchIndexSearch/AcceptIndexProviderResult/ApplySearchHits`；`MergeSearchResults` | [app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp) 为 UI 协调器，[network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp) 为联合结果实现。重复前缀、复制与重排见 F2；该联合功能应有独立轻量实现文件。 |
| 内容搜索请求、取消、配置、状态 | `ContentSearchClient` | [content_search_client.h](E:/Projects/pulse/src/index/content_search_client.h)/[content_search_client.cpp](E:/Projects/pulse/src/index/content_search_client.cpp) → [content_search_protocol.h](E:/Projects/pulse/src/index/content_search_protocol.h) → [content_agent.cpp](E:/Projects/pulse/src/index/content_agent.cpp)；主窗口启动默认 Instant 或 Observer，重复文件任务使用非持久 agent。 |
| 内容任务与缓存搜索 | `ContentIndex::Search/SearchTask/SearchCached`；`RunContentTaskSupplement` | [content_index.h](E:/Projects/pulse/src/index/content_index.h)/[content_index.cpp](E:/Projects/pulse/src/index/content_index.cpp)，[content_task_search.h](E:/Projects/pulse/src/index/content_task_search.h)/[content_task_search.cpp](E:/Projects/pulse/src/index/content_task_search.cpp)。Instant/LegacyWriter/Observer 有不同存储/写入责任，不应未经行为验证删掉。 |
| 直接内容/重复文件扫描 | `RunContentSearch`，`ContentSearchMode` | [content_search.h](E:/Projects/pulse/src/index/content_search.h)/[content_search.cpp](E:/Projects/pulse/src/index/content_search.cpp)；不依赖已有索引的任务执行路径，是任务扫描后端，非 UI 直接读取全文。 |
| 内容结果接收、暂存、分页、过滤、选择解析 | `ContentSearchSession::Accept`；`ContentResultStore`；`DeferContentSelection` | [content_search_session.cpp](E:/Projects/pulse/src/index/content_search_session.cpp) 接收 generation 并写结果库；[content_result_store.h](E:/Projects/pulse/src/index/content_result_store.h)/[content_result_store.cpp](E:/Projects/pulse/src/index/content_result_store.cpp) 为磁盘暂存/页面缓存拥有者；[content_results_ui.cpp](E:/Projects/pulse/src/app/content_results_ui.cpp) 为 UI 选择协调。 |
| 文档全文读取 | `ReadSearchableDocument`、`DocumentReadSession/CreateDocumentReadPool` | [document_reader.h](E:/Projects/pulse/src/index/document_reader.h)/[document_reader.cpp](E:/Projects/pulse/src/index/document_reader.cpp)；普通文本走 `pulse::text::ReadFile`，提取型文档走文档进程池。 |
| Office/PDF 文本提取后端 | `ExtractOfficeOpenXml/ExtractOfficeFilter/ExtractPdfiumText` | [document_host_main.cpp](E:/Projects/pulse/src/index/document_host_main.cpp) 选择 [office_ooxml.cpp](E:/Projects/pulse/src/index/office_ooxml.cpp)、[document_filter.cpp](E:/Projects/pulse/src/index/document_filter.cpp)、[pdfium_text.cpp](E:/Projects/pulse/src/index/pdfium_text.cpp)。新 Office 走 OOXML、旧 Office/RTF 走 IFilter，PDF 具有受限兼容回退；不是可无差别合并的重复。 |
| 正文解码、编码策略 | `pulse::text::Decode/ReadFile` | [text_decode.h](E:/Projects/pulse/src/common/text_decode.h)/[text_decode.cpp](E:/Projects/pulse/src/common/text_decode.cpp)；预览仍有重复小实现，见 F3。 |
| 重复文件 UI、保留项、删除选择 | `DuplicateScanSession`；`StartDuplicateScan/CancelDuplicateScan` | [duplicate_scan.h](E:/Projects/pulse/src/app/duplicate_scan.h)/[duplicate_scan.cpp](E:/Projects/pulse/src/app/duplicate_scan.cpp) → 内容客户端/扫描，删除再转统一 OpsManager。分组/选择与哈希执行是不同责任。 |
| 变化跟踪、摘要/明细与租约 | `ChangeTrackingClient`、应用 change tracking 入口 | [change_tracking_client.h](E:/Projects/pulse/src/index/change_tracking_client.h)/[change_tracking_client.cpp](E:/Projects/pulse/src/index/change_tracking_client.cpp) → 本地/网络 host；[change_tracking.h](E:/Projects/pulse/src/index/change_tracking.h)/[change_tracking.cpp](E:/Projects/pulse/src/index/change_tracking.cpp) 是记录与查询模型，[app_change_tracking.cpp](E:/Projects/pulse/src/app/app_change_tracking.cpp) 是 UI 协调。 |
| 文件 feed 与索引文件夹大小 | `IndexFeedConnection::Request/FolderSizes`；`Engine::FolderSizes` | [index_feed.h](E:/Projects/pulse/src/index/index_feed.h)、[folder_size_protocol.h](E:/Projects/pulse/src/index/folder_size_protocol.h)、[index_folder_sizes.cpp](E:/Projects/pulse/src/index/index_folder_sizes.cpp)、[folder_size_index.h](E:/Projects/pulse/src/index/folder_size_index.h)；单独工作线程连接，不能替换为窗口线程同步调用。 |
| 搜索历史与保存的查询 | `SearchHistory`；`SavedSearchStore` | [search_history.h](E:/Projects/pulse/src/app/search_history.h)/[search_history.cpp](E:/Projects/pulse/src/app/search_history.cpp)，[saved_search.h](E:/Projects/pulse/src/app/saved_search.h)/[saved_search.cpp](E:/Projects/pulse/src/app/saved_search.cpp)；保存输入/模式，不复制引擎。 |
| 全局搜索浮窗与热键 | `ToggleGlobalSearch/ApplyGlobalSearchSettings`；`GlobalSearchWindow` | [global_search_controller.cpp](E:/Projects/pulse/src/app/global_search_controller.cpp)、[global_search_window.h](E:/Projects/pulse/src/app/global_search_window.h)/[global_search_window.cpp](E:/Projects/pulse/src/app/global_search_window.cpp)、[global_search_hotkey.cpp](E:/Projects/pulse/src/app/global_search_hotkey.cpp)；复用索引客户端，窗口/热键生命周期独立。 |

### 2.3 文件操作、Shell、预览与呈现

| 功能 | 当前入口/契约 | 唯一实现/链路与边界 |
| --- | --- | --- |
| 文件操作队列、取消、暂停、冲突、重试 | `OpsManager::Submit/CancelCurrent/PauseCurrent/ResolveConflict/RetryLockedOperation` | [ops_manager.h](E:/Projects/pulse/src/ops/ops_manager.h)/[ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp)；[app_ops_ui.cpp](E:/Projects/pulse/src/app/app_ops_ui.cpp) 仅收集请求并呈现状态。 |
| 复制/移动与验证 | `OpRequest` → `OpsManager::RunTransfer` | [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp) 的生产实现包含 `MoveFileExW` 快速路径及 `CopyFile2`，冲突/暂停/验证在同一队列；不能把全部操作误记为经过 Shell。 |
| 删除确认与一次性准入 | `DeleteService::Prepare/Resolve/TakeAccepted/ReserveExecutionToken`；`BuildDeletePlan` | [delete_service.h](E:/Projects/pulse/src/ops/delete_service.h)/[delete_service.cpp](E:/Projects/pulse/src/ops/delete_service.cpp)，[delete_operation.h](E:/Projects/pulse/src/ops/delete_operation.h)/[delete_operation.cpp](E:/Projects/pulse/src/ops/delete_operation.cpp)；所有删除来源先经 OpsManager，授权 token 与计划快照不能用通用“路径列表接口”抹掉。 |
| 回收/永久删除、恢复回收站 | `OpsManager::RunDelete` → `ShellClient::DeleteAuthorized/RestoreRecycle` | [shell_client.cpp](E:/Projects/pulse/src/ipc/shell_client.cpp) → [shell_host/main.cpp](E:/Projects/pulse/src/shell_host/main.cpp)、[recycle_fileop.cpp](E:/Projects/pulse/src/shell_host/recycle_fileop.cpp)、内联 [delete_fileop.h](E:/Projects/pulse/src/shell_host/delete_fileop.h)；永久删除不可自动替代失败回收。公开旧 `DeleteRecycle/RealDelete` 被拒绝，见 F6。 |
| 创建、普通重命名、批量重命名、卷标改名 | `OpRequest`/`OpsManager::RunShellOp/RunDriveLabelRename`；`PreviewBatchRename` | [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp) 负责执行与状态；[batch_rename.h](E:/Projects/pulse/src/app/batch_rename.h)/[batch_rename.cpp](E:/Projects/pulse/src/app/batch_rename.cpp) 是规则/预览；[drive_label_rename.cpp](E:/Projects/pulse/src/app/drive_label_rename.cpp) 是应用入口。卷标不是文件名，真实后端是 `SetVolumeLabelW`。 |
| 撤销、恢复日志、回收撤销有效性 | `OpsManager::Undo/UndoToJson/UndoFromJson/RetryRecovery` | [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp)，[recycle_undo_validation.cpp](E:/Projects/pulse/src/ops/recycle_undo_validation.cpp)；恢复/撤销复用操作队列和删除准入，不应另设直接 IO 入口。 |
| 文件占用者查询/关闭协作 | `file_lock_owner.h` 的报告/查询契约 | [file_lock_owner.h](E:/Projects/pulse/src/ops/file_lock_owner.h)/[file_lock_owner.cpp](E:/Projects/pulse/src/ops/file_lock_owner.cpp)；OpsManager 捕获失败并按重试 generation 调度。 |
| 打开、打开方式、属性、命令、终端 | `OpsManager::OpenWith/ShowProperties/ExecuteVerb/OpenWithApp/ExecuteCommand/OpenTerminal` | [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp) 专用 STA OpenMailbox，与传输工作队列分离，避免被复制任务串行阻塞。 |
| 剪贴板文件/文本、完成剪切 | `WriteClipboard/WriteClipboardText/ReadClipboard/CompleteCutClipboard` | [clipboard.h](E:/Projects/pulse/src/ops/clipboard.h)/[clipboard.cpp](E:/Projects/pulse/src/ops/clipboard.cpp)；读取/写入系统数据对象是接口，不执行粘贴文件 IO。 |
| OLE 文件拖放 | `DoFileDragDrop/FileDataObject/WindowDropTarget` | [drag_drop.h](E:/Projects/pulse/src/ui/drag_drop.h)/[drag_drop.cpp](E:/Projects/pulse/src/ui/drag_drop.cpp)；拖入操作回调交给 app/OpsManager，不能与 clipboard 系统协议入口直接混为一谈。 |
| 压缩包列表与解压 | `ListArchive/ExtractArchive` | [archive.h](E:/Projects/pulse/src/ops/archive.h)/[archive.cpp](E:/Projects/pulse/src/ops/archive.cpp) 运行 7zip；[archive_navigation.cpp](E:/Projects/pulse/src/app/archive_navigation.cpp) 是虚拟目录/临时文件生命周期；[archive_dialog.cpp](E:/Projects/pulse/src/app/archive_dialog.cpp) 是输入交互。 |
| 系统上下文菜单查询/调用 | `ContextMenuController` → `OpsManager::QueryShellMenu/InvokeShellMenu` → `ShellClient::QueryContextMenu` | [context_menu_controller.cpp](E:/Projects/pulse/src/app/context_menu_controller.cpp)、[ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp)、[shell_client.cpp](E:/Projects/pulse/src/ipc/shell_client.cpp)、[shell_host/main.cpp](E:/Projects/pulse/src/shell_host/main.cpp)/[ctx_handlers.cpp](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp)；当前 DTO 重复见 F4。 |
| 静态 Shell 动词与菜单偏好 | `EnumerateStaticVerbs`；`ContextMenuPrefs` | [shell_verbs.h](E:/Projects/pulse/src/app/shell_verbs.h)/[shell_verbs.cpp](E:/Projects/pulse/src/app/shell_verbs.cpp)，[context_menu_prefs.cpp](E:/Projects/pulse/src/app/context_menu_prefs.cpp)；注册表静态动词与动态 IContextMenu 的能力不同，保留后端但统一呈现入口。 |
| 缩略图、静态预览、文字/hex、属性 | `ThumbnailCache::Draw/Properties/CachedProperties` | [thumbnail_cache.h](E:/Projects/pulse/src/ui/thumbnail_cache.h)/[thumbnail_cache.cpp](E:/Projects/pulse/src/ui/thumbnail_cache.cpp) → Preview IPC → [preview_host/main.cpp](E:/Projects/pulse/src/preview_host/main.cpp)；WIC、Shell、SVG、WMF/EMF/GIF 依文件能力分派。 |
| Office 等交互预览提供器 | `PreviewHandlerHost::Sync/Hide/Reset` | [preview_handler_host.h](E:/Projects/pulse/src/ui/preview_handler_host.h)/[preview_handler_host.cpp](E:/Projects/pulse/src/ui/preview_handler_host.cpp)；实际是 UI 进程专用 STA 和 overlay HWND，不经过 Pulse.Preview；停滞 apartment 可退役但不能强行释放 COM。 |
| 视频/音频交互播放 | `VideoPreview::Open/Play/Seek/Step/Snapshot` | [video_preview.h](E:/Projects/pulse/src/ui/video_preview.h)/[video_preview.cpp](E:/Projects/pulse/src/ui/video_preview.cpp)，MFPlay 按需加载、每次打开独立 mailbox；静态视频缩略图是不同功能。 |
| 快速预览窗口 | `QuickPreviewWindow::Show/Update/Hide` | [quick_preview_window.h](E:/Projects/pulse/src/ui/quick_preview_window.h)/[quick_preview_window.cpp](E:/Projects/pulse/src/ui/quick_preview_window.cpp)，配套 playback/input/render 文件；复用 ThumbnailCache/PreviewHandlerHost/VideoPreview，而非第四个解码引擎。 |
| Shell 图标 | `ShellIconCache::Draw/DrawSystemIcon` | [shell_icons.h](E:/Projects/pulse/src/ui/shell_icons.h)/[shell_icons.cpp](E:/Projects/pulse/src/ui/shell_icons.cpp)；异步精确图标与默认图标缓存，与像素预览语义不同。 |
| 绘制、命中、布局、主题/DPI | `MainRenderer::Render`；`Compositor`；`BuildVm/FillPaneViewModel` | [ui_renderer.h](E:/Projects/pulse/src/ui/ui_renderer.h)/[ui_renderer.cpp](E:/Projects/pulse/src/ui/ui_renderer.cpp)，[ui_compositor.h](E:/Projects/pulse/src/ui/ui_compositor.h)/[ui_compositor.cpp](E:/Projects/pulse/src/ui/ui_compositor.cpp)，[app_runtime.cpp](E:/Projects/pulse/src/app/app_runtime.cpp)/[app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp)；专门 ui_* 文件实现各视图，不应因文件拆分认定为重复 Renderer。 |
| 文字、编辑控件、Fluent 菜单/对话框、通知 | `Compositor` 文字契约；`edit_host`；`FluentMenu/NotificationToast` | [typography.cpp](E:/Projects/pulse/src/ui/typography.cpp)、[lumatext_renderer.cpp](E:/Projects/pulse/src/ui/lumatext_renderer.cpp)、[edit_host.cpp](E:/Projects/pulse/src/ui/edit_host.cpp)、[fluent_menu.cpp](E:/Projects/pulse/src/ui/fluent_menu.cpp)、[fluent_components.cpp](E:/Projects/pulse/src/ui/fluent_components.cpp)、[dialog_lifecycle.cpp](E:/Projects/pulse/src/ui/dialog_lifecycle.cpp)、[notification_toast.cpp](E:/Projects/pulse/src/ui/notification_toast.cpp)；LumaText/DirectWrite 有运行时能力回退，需保留。 |

### 2.4 设置、系统集成、存储与诊断

| 功能 | 当前入口/契约 | 唯一实现/链路与边界 |
| --- | --- | --- |
| 设置交互与异步任务 | `SettingsController::BindUi/ToggleUi/IndexAction/ConfigurationAction/NetworkAction/DiagnosticsAction` | [settings_controller.h](E:/Projects/pulse/src/app/settings_controller.h)/[settings_controller.cpp](E:/Projects/pulse/src/app/settings_controller.cpp)；设置改变产生 SettingsEffect，IO 任务不应从 ui_settings 绘制层启动。 |
| 默认文件管理器、Win+E、此电脑、开机启动 | `SystemIntegration::Start/Toggle/Tick`；`ApplyDefaultFileManager/RestoreDefaultFileManager`；`AppPrefs::ApplyLaunchOnStartup` | [system_integration.cpp](E:/Projects/pulse/src/app/system_integration.cpp)、[default_file_manager.h](E:/Projects/pulse/src/app/default_file_manager.h)/[default_file_manager.cpp](E:/Projects/pulse/src/app/default_file_manager.cpp)、[startup_launch.cpp](E:/Projects/pulse/src/app/startup_launch.cpp)、[app_prefs.cpp](E:/Projects/pulse/src/app/app_prefs.cpp)；注册表操作有备份/恢复和外部改动检测。 |
| Explorer 窗口接管与 Shell 选择交付 | `ExplorerWindowTakeover`；`ShellWindowRegistry` | [explorer_window_takeover.h](E:/Projects/pulse/src/app/explorer_window_takeover.h)/[explorer_window_takeover.cpp](E:/Projects/pulse/src/app/explorer_window_takeover.cpp)，[shell_window_registry.h](E:/Projects/pulse/src/app/shell_window_registry.h)/[shell_window_registry.cpp](E:/Projects/pulse/src/app/shell_window_registry.cpp)；前者接管请求，后者登记窗口/交付选择，非两个默认管理器实现。 |
| 通知区域图标、后台隐藏/恢复 | `TrayController` | [tray_controller.h](E:/Projects/pulse/src/app/tray_controller.h)/[tray_controller.cpp](E:/Projects/pulse/src/app/tray_controller.cpp)。 |
| 本地索引配置、服务安装与提权 | `IndexClient::RequestInstallService/RequestConfigureVolume/RequestRebuild`；配置接口 | [index_client.cpp](E:/Projects/pulse/src/index/index_client.cpp)、[index_config.h](E:/Projects/pulse/src/index/index_config.h)/[index_config.cpp](E:/Projects/pulse/src/index/index_config.cpp)、[index_path_service.cpp](E:/Projects/pulse/src/index/index_path_service.cpp)、[index_host.cpp](E:/Projects/pulse/src/index/index_host.cpp)；机器级设置和用户级配置需分别拥有权限边界。 |
| 内容配置及状态 sidecar | `LoadContentIndexConfig/SaveInstantContentConfig`；`ContentSearchClient::Configure/GetStatus` | [content_configuration.cpp](E:/Projects/pulse/src/index/content_configuration.cpp)、[content_config_storage.h](E:/Projects/pulse/src/index/content_config_storage.h)、[content_search_client.cpp](E:/Projects/pulse/src/index/content_search_client.cpp)；sidecar 存配置/状态，不等于另一份全文数据库。 |
| 配置/用户索引定位迁移 | `storage::Schedule/ApplyConfiguration/ApplyUserIndex`；`ApplyUserIndexCheckpoint` | [user_storage.h](E:/Projects/pulse/src/common/user_storage.h)/[user_storage.cpp](E:/Projects/pulse/src/common/user_storage.cpp)，[user_index_storage.h](E:/Projects/pulse/src/index/user_index_storage.h)/[user_index_storage.cpp](E:/Projects/pulse/src/index/user_index_storage.cpp)；用户数据迁移与 machine index migration 的所有权不同。 |
| 本地化 | `l10n::Initialize/SetLanguage/Get` | [localization.h](E:/Projects/pulse/src/common/localization.h)/[localization.cpp](E:/Projects/pulse/src/common/localization.cpp)，资源 [strings.rcinc](E:/Projects/pulse/src/common/strings.rcinc)；界面与 host 的原始错误协议需区分。 |
| 崩溃与诊断导出 | crash reporter 契约；`diagnostics::Export/ClearCrashReports` | [crash_reporter.h](E:/Projects/pulse/src/common/crash_reporter.h)/[crash_reporter.cpp](E:/Projects/pulse/src/common/crash_reporter.cpp)，[diagnostics_exporter.h](E:/Projects/pulse/src/common/diagnostics_exporter.h)/[diagnostics_exporter.cpp](E:/Projects/pulse/src/common/diagnostics_exporter.cpp)；index/document 具有额外统计/trace 辅助接口。 |

## 3. 有源码证据的重复及成本

### F1：列表大小和详情大小各自递归扫描同一目录（优先收敛）

证据：[folder_sizes.cpp](E:/Projects/pulse/src/app/folder_sizes.cpp) 的 `Scan::Step` 用 `FindFirstFileExW/FindNextFileW` 递归、分片、检查部分结果/溢出；[app_runtime.cpp](E:/Projects/pulse/src/app/app_runtime.cpp) 的 `StartDetailsSizeWalk` 创建另一线程和另一递归栈，统计 bytes/files/dirs；`BuildVm` 根据详情目标自动启动后者，[folder_sizes_ui.cpp](E:/Projects/pulse/src/app/folder_sizes_ui.cpp) 同时同步列表目录请求。

结论：同一目录出现在列表且被选中展示详情时，两套路径可独立读取相同子树；这是可发生的重复目录 IO 和线程/状态维护，不是基于文件数量的猜测。尚未测量触发频率、目录规模或耗时。它们目前也不是完全同义：列表实现有 Partial/Unavailable/Indexed/Cached 状态和 cloud/reparse 保护，详情实现还需文件/目录计数，失败目录被跳过。

最小建议：以 `FolderSizes` 为统一查询/缓存/失效拥有者，扩展结果中的可选 files/dirs 和完整性；详情以 path+generation 请求计数能力。索引只提供字节值时可先显示 indexed bytes，再按需扫描计数，避免为了计数丢弃索引快路径。不得仅删除旧 walker、用字节数冒充完整计数。

### F2：文件名联合搜索翻页重取前缀，并复制和重排已保存结果

证据：[app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp) `RequestSearchPage` 把下一次 limit 设为已载入数+页大小，实际 offset 为零；`DispatchIndexSearch` 两个 provider 从零查询前缀；`AcceptIndexProviderResult` 把 `pending.local/pending.network` 左值传给按值参数的 `MergeSearchResults`；[network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp) 再搬入新 vector 并 `std::sort`，然后应用 offset/limit；`ApplySearchHits` 在 offset=0 时新建条目 vector，用移动字符串生成 `DirEntry`。

结论：每次翻页可重新传输已展示前缀；按值调用明确复制两个保存的 `SearchResult` 及其中字符串，联合排序明确重复处理前缀。保留 pending 结果是为了订阅一方更新时仍可联合另一方，不能直接 `std::move` 掉 pending 而破坏后续 live 合并。若两方返回 m 条候选，当前联合完整排序的理论成本为 O(m log m)，没有测得实际卡顿时间。

最小建议：第一步让合并函数接收只读结果引用，只复制最终输出所需条目，保留订阅快照；抽出共用比较器，在满足 provider 顺序完全一致的前提下用有界双路合并。第二步才讨论稳定游标/增量分页；需要明确本地与网络 revision、live 更新和全局排名，不应简单把统一 offset 发给每个 provider（会漏全局结果）。

### F3：预览重复实现文本判别/解码，已经存在行为差异

证据：[preview_host/main.cpp](E:/Projects/pulse/src/preview_host/main.cpp) 自有 `LooksBinary/DecodeText/ReadPrefix`；[text_decode.cpp](E:/Projects/pulse/src/common/text_decode.cpp) 自有 `LooksBinary/Decode/ReadFile`，生产 `pulse_preview` 未链接 `pulse_text`。两者都有 BOM、UTF-8→ACP 和控制字节判断；common 拒绝奇数字节 UTF-16，预览忽略末尾不成对字节；common 对带 UTF-8 BOM 的非法内容不回退 ACP，预览可回退；预览替换控制字符且限制 12000 字符。

结论：这是同一解码基础策略的重复实现和一致性成本；不证明它导致 CPU 热点。预览只读前缀、搜索需稳定整文件读取，是必须保留的读取语义差别。

最小建议：统一 byte→text 解码和二进制判别，把预览的前缀容错、显示清洗和截断显式作为策略/适配保留；不以 `text::ReadFile` 直接替代 `ReadPrefix`。确认预览前缀可能截断 UTF-8/UTF-16 后再规定末尾容错，补与此次差异相关的定向用例。

### F4：Shell 菜单 IPC DTO 和操作层 DTO 字段完全重复

证据：[shell_client.h](E:/Projects/pulse/src/ipc/shell_client.h) 的 `CtxMenuItem` 与 [ops_manager.h](E:/Projects/pulse/src/ops/ops_manager.h) 的 `ShellMenuItem` 具有同样的 id、enabled、separator_after、has_children、child、verb、text、clsid、handler；[ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp) `cb.ctx_items` 分配新 vector，逐项搬运所有字段，再传 `OnCtxItems`。

结论：此层转换没有增加语义，存在第二个 vector 分配和逐行结构搬运。字符串已 move，并非复制全部字符串；菜单规模/性能影响未测。后续 `ShellMenuEntry` 增加显示命令、children 等 UI 语义，不能与 wire DTO 一起盲目删掉。

最小建议：把 wire 解码后的领域菜单项放在中立的 shared contract，客户端和 OpsManager 使用同一类型；保留 request-id↔menu-token 映射、会话生命周期及 UI 树形呈现转换。若兼容需要保留类型名，用 alias 过渡而非重复结构。

### F5：联合结果函数与完整网络索引执行体同文件，使 UI 保留后端编译依赖

证据：[CMakeLists.txt](E:/Projects/pulse/CMakeLists.txt) UI GLOB 包含 `src/index/*.cpp`，排除本地 Engine/content 执行体，但未排除 `network_index.cpp`；[app_state.h](E:/Projects/pulse/src/app/app_state.h) 生产状态用 `NetworkAgentClient`；[network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp) 同时定义 `NetworkIndex` 执行体与 `MergeSearchResults`，UI 需要后者。

结论：生产 UI 编译边界未把“结果联合”与“网络执行”隔离。这是依赖/编译成本和职责边界问题。未检查 Release 链接裁剪结果，不能宣称 NetworkIndex 全部代码驻留、启动了第二个索引或增加了多少内存。显式 `list(APPEND ... network_agent_client.cpp)` 也不能仅凭 GLOB 重复列项断言 MSVC 重复编译，需看生成规则。

最小建议：将联合比较/合并接口与实现拆成轻量 `search_result_merge.h/.cpp`，再把 network_index 执行源限定到 Pulse.Index 和对应测试。Query/Hit/SearchResult 等稳定契约可进一步从庞大 engine 头拆出，减少客户端对执行私有定义的编译耦合；按真实依赖做定向目标验证。

### F6：删除旧公开入口看似可用，实际恒拒绝；应收紧接口表面

证据：[shell_client.cpp](E:/Projects/pulse/src/ipc/shell_client.cpp) `DeleteRecycle/RealDelete` 直接返回拒绝结果；[shell_host/main.cpp](E:/Projects/pulse/src/shell_host/main.cpp) 对 legacy deletion frame 返回 `E_ACCESSDENIED`。生产授权入口 `DeleteAuthorized` 为私有，只有 OpsManager 友元调用，按授权计划 token 执行并禁止重放。

结论：不是两个删除执行实现，而是旧兼容/防御接口与真正授权路径并存；公共 API 会误导调用方。最小建议：检查生产/测试调用后，移除或显式废弃客户端旧入口；host 的旧帧拒绝可保留为兼容安全防线。不能用接口合并绕过 DeleteService。

### F7：管道分块 IO 有相似实现，但取消/截止时间/身份验证策略不同

证据：[content_search_client.cpp](E:/Projects/pulse/src/index/content_search_client.cpp)、[change_tracking_client.cpp](E:/Projects/pulse/src/index/change_tracking_client.cpp)、[index_feed.h](E:/Projects/pulse/src/index/index_feed.h) 均实现 overlapped transfer；[network_agent_client.cpp](E:/Projects/pulse/src/index/network_agent_client.cpp) 已使用 transport 工具；[index_transport_security.h](E:/Projects/pulse/src/index/index_transport_security.h) 同时定义身份、ACL、验证和传输；Shell/Preview 的 [protocol.h](E:/Projects/pulse/src/ipc/protocol.h)/[preview_protocol.h](E:/Projects/pulse/src/ipc/preview_protocol.h) 有自己的帧与读取方式。

结论：有重复基础 IO 机制，但现阶段不能把所有进程重试/取消/超时归为可互换的一套策略。最小建议：仅提取底层“完整传输+取消+deadline”的 RAII 工具，以参数保留差异；server identity、payload 上限、授权删除不可重放、预览 mapping ACK、文档内存准入仍由各协议拥有。不增加统一消息总线或多一层序列化。

### F8：worker 的排序契约依赖整个 Renderer 头（编译边界）

证据：[app_worker.h](E:/Projects/pulse/src/app/app_worker.h) 直接 include [ui_renderer.h](E:/Projects/pulse/src/ui/ui_renderer.h)，`WorkItem/Refresh/LoadPaths` 使用其中的 `ui::SortColumn/SortDirection`。排序的实际执行在 [app_worker.cpp](E:/Projects/pulse/src/app/app_worker.cpp)/[entry_sort.cpp](E:/Projects/pulse/src/app/entry_sort.cpp)。

结论：工作线程的请求契约被渲染层大头文件绑定，这是编译/模块职责耦合；include 不会在运行时自动产生额外 IO 或列表复制。后续可把排序枚举拆进中立、轻量契约头，renderer 和 worker 共用；本次仅记录，不做泛化搬迁。

### F9：两个 DriveDisplayName 是不同展示适配，但编辑端依赖展示字符串逆解析

证据：[common/drive_labels.h](E:/Projects/pulse/src/common/drive_labels.h) `format::DriveDisplayName(DirEntry)` 用条目形成显示；[app/drive_labels.h](E:/Projects/pulse/src/app/drive_labels.h) `app::DriveDisplayName(root,label)` 为 home/卷标更新生成名称。`DriveLabelFromDisplayName` 按固定后缀取回 label，实际用于 [app_hosted_edit.cpp](E:/Projects/pulse/src/app/app_hosted_edit.cpp) 的 rename 初始内容和旧值。

结论：相同函数名不代表可以直接删除一个实现，两个入口输入不同；没有证据证明它导致性能问题。可将 root/label 的显示格式基础规则收敛，并让 rename 读取明确的卷标字段而非展示名称，避免展示文案变更影响编辑语义；需要先定义条目/Home 的原始 label 所有权，不应本次顺手改模型。

## 4. 数据经过多层是否产生额外成本

| 真实链路 | 已核实的数据操作 | 判断与最小处理 |
| --- | --- | --- |
| 枚举 → WorkResult → SnapshotStore/Tab → PaneViewModel → Renderer | [fs_snapshot.h](E:/Projects/pulse/src/fs/fs_snapshot.h) 定义共享只读 SnapshotPtr；[app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp) `FillPaneViewModel` 直接赋共享快照、内容结果、filter/tag/date cache。 | 经过多层不等于多次全列表复制。保留共享快照和 generation，不为了减少函数层数把 IO 放进 Renderer。增量快照重建另按实际通知成本衡量。 |
| 本地/网络索引 → IPC bytes → SearchResult → pending → 联合 → DirEntry | 客户端解码；pending 持有候选；F2 左值到按值函数造成完整结果复制；联合排序；最终 Hit 字符串 move 到目录条目。 | 主要可收敛点是前缀重传、pending 到合并复制和重复重排；协议 DTO 到 UI row 的一次语义转换可保留。 |
| 内容 agent → batch bytes → ContentHit → ContentSearchSession → ContentResultStore → Tab/UI 可见页 | [content_search_session.cpp](E:/Projects/pulse/src/index/content_search_session.cpp) 把 hits 写入结果库后清空 update.hits，仅向 UI 传共享 results；[content_result_store.cpp](E:/Projects/pulse/src/index/content_result_store.cpp) SQLite 暂存 hits/visible，异步页面缓存。 | 有串行化与磁盘写入成本，但用于限制 UI 的常驻结果内存，不是各层反复传全文。需要测 batch 数、临时库 bytes、页面命中和 first-visible 延迟后决定优化；不能把内容索引数据库和 UI 结果库仅凭都用 SQLite 判为重复数据库。 |
| 文件 → Pulse.Document → UTF-16 body → 内容匹配/缓存 | [document_host_main.cpp](E:/Projects/pulse/src/index/document_host_main.cpp) 写 response 及 body；[document_reader.cpp](E:/Projects/pulse/src/index/document_reader.cpp) 读入 output，plain text 走 common；最大字节、池、取消和准入有明确协议。 | 文档隔离带来提取文本传输，这是进程边界的实际成本；不是全文先送到 UI 再送回索引。若测出大文档输送占比高，再评估 host 侧短路/分块匹配，并保留全文缓存和多 needle 语义。 |
| 解码 pixels → Preview 共享映射 → ThumbnailCache pixels → D2D bitmap | [preview_host/main.cpp](E:/Projects/pulse/src/preview_host/main.cpp) `memcpy` 解码 vector 到 mapping；[thumbnail_cache.cpp](E:/Projects/pulse/src/ui/thumbnail_cache.cpp) `result.pixels.assign` 从映射复制后 ACK；绘制阶段 CreateBitmap 后清空/shrink CPU vector。 | 至少有两次 CPU 像素搬运，再有 bitmap 上传；mapping 并不代表零拷贝。现有复制允许尽快 ACK 关闭 host 映射、避免 host 等 UI。若实测值得优化，可让 decode 直接写映射，或显式延长 mapping 生命周期至上传；不得直接删 assign 造成悬空指针。 |
| Shell wire DTO → Ops DTO → UI entry/menu | F4 第一层是同字段搬运；UI entry 增加命令/树/偏好。 | 收敛相同 DTO，保留领域到呈现的转换；暂无性能数值。 |

## 5. 必须保留的真实多后端与边界

1. 本地 MFT/USN 和 UNC 网络枚举/观察依赖不同权限、存储和可达性；用户级 network agent 与机器级本地 service 不应强行成为同一后台线程。
2. 内容缓存查询、Instant 任务扫描、Observer、非持久直接扫描有不同新鲜度/写入约束；可统一客户端门面和 query model，保留执行能力。
3. 删除准入和执行分离、一次性 token、准确已删除/已回收路径、不可重放是行为保证；计划层不是可删除的冗余“包装”。
4. `CopyFile2/MoveFileExW` 的传输、IFileOperation 回收/删除、恢复回收站、`SetVolumeLabelW` 卷标具有不同 API 能力；统一业务队列，不统一成假想万能 Shell 调用。
5. 静态预览解码、交互 IPreviewHandler、MFPlay 视频、Shell 图标不能用一个 bitmap 接口替代；共享选择版本/视口等公共模型即可。现有 IPreviewHandler 在 UI 进程专用 STA 中，不宜称“全部预览已经进程隔离”。
6. OOXML、旧 Office IFilter、PDFium/PDF IFilter 回退服务不同格式；文档 parser 进程与 Preview 解码进程不能为少一次 IPC 而装进 UI。
7. 目录列表、索引 feed、回收站、压缩包及 home 是不同数据源；统一条目和加载结果协议，不删除真实数据源。
8. LumaText/DirectWrite、Windows 材质兼容降级、主题/DPI 能力路径是运行时兼容后端；需保留验证过的 fallback 条件。

## 6. 最小整改顺序和验证界限

建议逐项实施，而非先增加全局 facade/接口继承层：先 F3 文本基础策略、F4 菜单 DTO、F5 轻量联合文件；再 F1 文件夹统计唯一拥有者；最后 F2 联合候选的引用/有界合并及稳定分页。F6 公共删除入口清理和 F7 IO 工具提取分别验证兼容语义，不与业务改造混成一次大重构。

本文架构发现仅做静态审阅和文档链接/符号核查，未针对 F1–F9 运行功能测试或性能 benchmark；另行实施的资源整改验证见下一节报告。源码能证明分配、复制、递归、排序及 IPC 操作存在；不能证明每个场景的耗时、卡顿或内存峰值。未来整改只运行直接受影响目标/用例：文件夹大小相关用例；名称搜索联合分页/订阅场景；预览文本编码场景；Shell 菜单会话场景；内容/文档取消与进程退出场景。按实际改变扩大验证，不能用完整测试通过替代以上行为的定向验证。

## 7. 本次资源解耦与接口收敛的关系

[本地化资源审查报告](E:/Projects/pulse/docs/localization-resource-audit.md) 记录资源集中管理及显示文案参与逻辑判断的问题；[资源解耦汇总](E:/Projects/pulse/docs/resource-decoupling-review.md) 记录资源整改边界、方案与验收。资源整改阶段的源码实施范围仅包括这些已确认的资源耦合问题，具体完成情况与实际验证结果以资源解耦汇总为准。后续少量架构整改以文首链接的行为保持优化报告为准，本文原始 F1–F9 发现不能整体作为已完成的接口收敛或性能优化。

资源解耦与“一个功能接口对应一个责任明确的实现”遵循同一原则：错误类型、取消终态、失败路径和展示类别应由已有执行结果或模型状态提供，呈现层消费这些契约并选择资源文案，避免再从显示文字反向恢复机器信息。此次应优先复用已有字段，只在确实缺少结果证据的边界补充必要契约；不新增重复执行后端，也不为一处文案判断增加通用服务层。

本次资源整改未同时实施 F1–F9 的扫描、查询分页、DTO、编译依赖或进程传输调整。本文的功能表提供责任归属参考，两个资源文档记录此次改动的具体范围；架构整改应按对应功能明确语义和定向验证，不将本次编译或资源检查结果用于证明这些建议已实现。
