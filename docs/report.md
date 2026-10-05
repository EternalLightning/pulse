五个调研代理全部完成，证据已齐。现在交叉核对并输出最终审计报告。

# Pulse 架构审计报告

## 0. 审计范围与方法

- **范围**：`E:\Projects\pulse` 全部 `src/` 产品代码（约 11.3 万行，555 个 .h/.cpp），含 `app`（45k 行）、`ui`（35k）、`index`（22k）、`ops`（6k）、`fs`（1.7k）、`ipc`、`preview_host`、`shell_host`、`common`；`src/bench`（28k 行）仅在测试分布相关处涉及。构建系统（CMakeLists.txt、installer）仅用于识别进程拓扑。
- **方法**：静态阅读 + 5 路并行按模块调研（app / ui / fs+ops / index+ipc+宿主进程 / 横向抽象与数据流），所有结论回查 `文件:行号` 证据。未做运行时 profiling，性能结论基于调用链与线程/句柄模型的静态推断。
- **标注约定**：未特别标注的为**事实**（有直接代码证据）；推测性判断标【推断】；优化方向集中在第 10、11 节，属**建议**。第 12 节列出无法静态确认的项。

---

## 1. 执行摘要：P0/P1 问题 Top 列表

| # | 级别 | 问题 | 证据 |
|---|------|------|------|
| 1 | P0 | **ui ↔ app 头文件级双向依赖**：ui 大量包含 `../app/*`，而 `app/places.h:4` 回含 `../ui/view_layout.h`，分层在编译期已打穿 | `src/ui/ui_renderer_internal.h:10`、`src/ui/ui_sidebar.cpp:10`、`src/app/places.h:4` |
| 2 | P0 | **同一目录内容最多同时存 4 份、失效策略各异**（SnapshotStore / tab 拷贝 / ContentResultStore / MFT 索引），陈旧显示与内存放大风险 | `src/fs/fs_snapshot.h:36`、`src/index/content_result_store.h:20`、`src/app/app_model.h` |
| 3 | P1 | **AppState 上帝结构**：单结构约 470 个字段，36 个 app 文件直接共享可变访问，且头级 include ui/fs/index/ops 全部模块 | `src/app/app_state.h:132-696`、`src/app/app_state.h:4-47` |
| 4 | P1 | **变更监听三套并行系统**：fs_watch（每目录一线程，上限 64）+ folder_sizes 独立 subtree watch + index 的 USN/RDCW，同一目录树被重复监听 | `src/fs/fs_watch.cpp:209,316`、`src/app/folder_sizes.cpp:307-321`、`src/index/index_engine.cpp:3679` |
| 5 | P1 | **目录枚举 4 套实现**：NT API 主路径、FindFirstFileEx 静默回退、folder_sizes 自写递归、ops RunTransfer 用 `std::filesystem` | `src/fs/fs_enum.cpp:233-330`、`src/app/folder_sizes.cpp:88-89`、`src/ops/ops_manager.cpp:2137` |
| 6 | P1 | **全局搜索窗自带第二套全套搜索客户端**（Index/NetworkAgent/ContentSearch 各一份），与 AppState 内实例平行重复 | `src/app/global_search_window.cpp:81-85` vs `src/app/app_state.h:237-241` |
| 7 | P1 | **偏好状态多处双写/三写**：darkMode 存 3 处、change_tracking_enabled 每 tick 比对重同步、folder_sort_mode 双写 | `src/app/session.h:42`、`src/app/app_change_tracking.cpp:106-116`、`src/app/entry_sort.cpp:65` |
| 8 | P1 | **同名头文件/同名函数并存**：`common/drive_labels.h` 与 `app/drive_labels.h` 各定义不同签名的 `DriveDisplayName`；`SortDirection` 定义两次 | `src/common/drive_labels.h:19`、`src/app/drive_labels.h:8`、`src/ui/sort_types.h:6`、`src/ui/fluent_components.h:29` |

正面结论（先说清，避免误改）：本项目**几乎没有虚接口滥用**——全项目纯虚接口仅 1 个且是测试注入点（`src/app/default_file_manager.h:21-31`）；无事件总线、无服务定位器、无单例；进程隔离拓扑（Shell/Preview/Index/Document 四宿主）边界清晰、职责单一。问题集中在**横向重复实现**与**耦合**，而非"过度抽象"。

---

## 2. 技术栈、模块边界与入口

- **技术栈**：C++20 / MSVC / Win32 + Direct2D + Direct3D11 + DirectComposition；COM/Shell API（IFileOperation、IContextMenu、IThumbnailProvider、IPreviewHandler）；NT 原生 API（NtQueryDirectoryFile、USN/MFT）；可选 LumaText 文本库（延迟加载，`CMakeLists.txt:39-115`）。CMake ≥3.25 + Ninja。
- **进程拓扑（5 个可执行目标）**：
  - `Pulse.exe` 主程序（UI + 导航 + ops 客户端 + 进程内复制/移动/重命名），`CMakeLists.txt:297`
  - `Pulse.Shell.exe` Shell COM 隔离（IFileOperation 删除/回收/新建、第三方 IContextMenu），`CMakeLists.txt:423`
  - `Pulse.Preview.exe` 缩略图/自绘预览解码，`CMakeLists.txt:430`
  - `Pulse.Index.exe` 一 exe 多角色（`--service` SCM 全盘索引 / 用户态 helper / `--network-agent` / 内容索引代理 ×4），`src/index/index_host.cpp:1177-1214`
  - `Pulse.Document.exe` 文档文本提取（PDF/OOXML/IFilter），`CMakeLists.txt:750`
- **入口**：`wWinMain` 在 `src/app/app_main.cpp:1985`。启动顺序：崩溃处理 → DPI → `OleInitialize` → 构造 AppState（:2016）→ 单实例 → 会话恢复 → 命令行 → 建窗（:2187-2221）→ 消息循环（:2398）。
- **IPC**：全部命名管道，公共框架 `src/ipc/protocol.h:118-354`（MsgHeader + PayloadWriter/Reader + PipeRead/PipeWrite）；但预览（`preview_protocol.h:41-78`）与文档宿主（`document_protocol.h:22-52`）各用独立 struct 协议，magic 共 5 套（PLPS/PIDX/内容搜索/网络代理/folder_size 复用 id 9/109）。

---

## 3. 实际功能清单

**用户可见**：多标签/双窗格导航、地址栏（路径+搜索 omnibar）、面包屑、8 种视图模式、列视图（Miller columns）、排序/分组/过滤、多选、重命名（单个/批量）、文件操作进度与冲突解决对话框、回收站（删除/还原/列表）、文件夹大小显示、快速预览（Alt+P）、缩略图、图标、上下文菜单（内置+Shell 扩展+自定义排序/隐藏）、拖放、剪贴板、主页（驱动器/已知文件夹卡片）、重复文件扫描、设置中心、主题（Mica/Acrylic、深浅色）、快捷键帮助、命令面板、驱动器卷标重命名。

**系统集成**：全局搜索热键弹窗、默认文件管理器接管（Explorer 窗口接管 `explorer_window_takeover.cpp`）、注册为默认文件管理器（注册表）、单实例转发、崩溃日志（%LOCALAPPDATA%\Pulse）。

**后台能力**：全盘 MFT/USN 索引服务（可选 PulseIndex 服务，安装器注册）、网络位置索引代理、内容索引（PDF/OOXML/IFilter 文本提取）、拼音搜索、变更跟踪（"最近更改"视图）、目录快照缓存、网络路径探测缓存。

---

## 4. 功能-接口-实现汇总表

| 功能域 | 用户功能 | 入口/命令 | 接口 | 实现 | 调用方 | 实现数 | 状态 | 证据 |
|---|---|---|---|---|---|---|---|---|
| 枚举 | 目录列表 | NavigateTo | fs::Enumerate | NT API 主 + FindFirstFileEx 回退 | app_worker | 2 | 双路径，回退静默 | fs_enum.cpp:233-330, 400-407 |
| 枚举 | 文件夹大小 | 状态栏/列 | FolderSizes | 索引查询 + 自写递归 | folder_sizes_ui | 2 | 有意主备，见 §6 | folder_sizes.cpp:208 vs 88-89 |
| 枚举 | 传输前扫描 | RunTransfer | （无） | std::filesystem 递归 | ops_manager 内部 | 1（第 3 套枚举代码） | 冗余实现 | ops_manager.cpp:2137 |
| 变更 | 目录刷新 | DirWatch | fs::DirWatch | RDCW，每目录一线程 | app_runtime | 1 | 但与其他两套并行 | fs_watch.cpp:209,316 |
| 变更 | 最近更改 | 虚拟视图 | index::ChangeTrackingClient | USN+定时轮询 | app_change_tracking | 1 | 与 fs_watch 双系统 | app_change_tracking.cpp:83-166 |
| 搜索 | 文件名搜索 | 地址栏/标签 | IndexClient.SearchAsync | Pulse.Index（服务或 helper） | app_navigation | 1 | 无进程内兜底 | app_navigation.cpp:218-231 |
| 搜索 | 内容搜索 | 高级搜索 | ContentSearchClient | 独立代理进程 + 扫描兜底 | app_navigation | 1 | task_scan 兜底 | search_options.cpp:96-97 |
| 搜索 | 全局搜索 | 全局热键 | 第二套三客户端 | 同左 | global_search_window | **2 套实例** | P1 重复 | global_search_window.cpp:81-85 |
| 操作 | 复制/移动 | 命令/拖放 | OpsManager::Submit | 进程内 CopyFile2/MoveFileEx | app_ops_ui | 1 | 头注释与实现不符 | ops_manager.cpp:2608,2266；ops_manager.h:3-5 |
| 操作 | 删除/回收 | Del | ShellClient | Pulse.Shell IFileOperation | ops_manager | 1 | 边界清晰 | shell_host/main.cpp:629-638 |
| 回收站 | 列表/还原 | 虚拟视图 | fs_recycle | 直接解析 $I* 二进制 | app | 1 | 不走 Shell 命名空间 | fs_recycle.cpp:22-157 |
| 图标 | 文件图标 | 列表渲染 | ShellIconCache | 进程内 SHGetFileInfoW（worker 线程） | ui_renderer | 1 | 无 TTL 失效 | shell_icons.cpp:266-271 |
| 缩略图 | 缩略图 | 列表/预览 | ThumbnailCache | 管道→Pulse.Preview | ui_renderer、quick_preview | **2 实例** | 各起子进程 | ui_renderer.h:1177、quick_preview_window.h:142 |
| 预览 | 快速预览 | Alt+P | preview_handler_host | 进程内 IPreviewHandler | quick_preview_window | 1 | 与进程外缩略图链并存（部分有意） | preview_handler_host.cpp:336-613 |
| 菜单 | 上下文菜单 | 右键 | MenuCmd 枚举 | 内置构建+Shell 会话 | app_commands | 1 | 横跨 5 文件 | context_menu.h:18-109 |
| 拖放 | 拖入/拖出 | OLE | FileDataObject 等 | 进程内 OLE | app（回调注入） | 1 | 健康 | drag_drop.cpp:56-81 |
| 重命名 | 文件重命名 | F2 | HostedEdit 覆盖层 | app_hosted_edit | app_input | 1 | 4 条重命名路径之一，见 §6 | app_hosted_edit.cpp |
| 文本 | 文本渲染 | 全部绘制 | Painter::DrawText | LumaText 主 + DWrite 回退 + 原生 EDIT | ui 全局 | 2+1 | 主备有意，选择规则分散 3 处 | fluent_components.cpp:495-503、ui_compositor.cpp:138-152、edit_host.cpp:34-39 |

---

## 5. 接口与实现问题

1. **虚接口滥用：不存在**【事实】。全项目纯虚接口仅 `DefaultManagerRegistryStore`（`default_file_manager.h:21-31`），且生产代码无实现、唯一派生类在测试（`bench/default_file_manager_test.cpp:17`）——属合理测试注入，可保留，也可考虑直接删接口让测试用注册表重定向（P3 可选）。
2. **纯转发/适配小层**（P2）：
   - `global_search_controller.cpp`（56 行）4 个函数全是 AppState↔Hotkey/Window 转发；
   - `column_view_model.cpp:8-42` 整体是逐字段拷贝器；
   - `MergeSearchResults`（`index/search_result_merge.cpp:32`，53 行独立文件）唯一调用方 `app_navigation.cpp`；
   - `SubmitWithConflictResolution` 一行包装（`app_ops_ui.cpp:85-89`）；
   - `folder_sizes_ui.cpp` 是模型→VM 标签适配器。
   单个看都不大，合起来是"胶水文件群"，建议内联回唯一调用方。
3. **占位/死代码**（P2）：`ShellClient::DeleteRecycle/RealDelete` 公有方法恒返回 E_ACCESSDENIED，真正删除走 friend 的 `DeleteAuthorized`（`src/ipc/shell_client.cpp:200-212`）——公有接口是假的，应直接私有化或删除。
4. **过期文档**（P3）：`ops_manager.h:3-5` 注释声称"所有文件操作经 Pulse.Shell.exe 执行"，实际复制/移动/重命名在进程内（`ops_manager.cpp:237-239` 注释明确拒绝走 shell）——注释会误导后续架构判断。
5. **消息号管理**（P3）：WM_APP 编号集中在 `app_state.h:63-83` 但号段跳跃，且 ui/index 各自另起号段（`file_operation_dialog.h:15` +0x2B1、`video_preview.cpp:26` +87、`index_host.cpp:55`），插入新消息易撞号。

---

## 6. 功能多实现/多样式问题

| 功能 | 套数 | 明细与判断 | 证据 |
|---|---|---|---|
| 目录枚举 | 4 | NT API（主）、FindFirstFileEx（静默回退）、folder_sizes 自写递归、ops 用 std::filesystem。后两者应复用 fs_enum；NT↔FindFirst 回退属无证据双路径（见 §9） | fs_enum.cpp:233-330；folder_sizes.cpp:88-89；ops_manager.cpp:2137 |
| 搜索入口 | 5+ | 地址栏 omnibar、命令面板、高级搜索对话框、标签内 `pulse:search:`、独立全局搜索窗。后端大体共用索引，但**全局搜索窗持有第二套客户端实例**是真重复；搜索历史两份（`search_history.cpp` + `address_search_history.cpp`）是真冗余 | global_search_window.cpp:81-85；app_state.h:237-241 |
| 排序 | 2 栈 | 列表 `EntryLess`（entry_sort.cpp:77）一套；搜索侧 `ResultSort`/`BetterHit`/`ContentResultSort`/SQL 排序另一套。功能域不同（列表 vs 相关性），**部分属真实差异**；但 `SortDirection` 类型定义两次、`ColumnKind` 与 `SortColumn` 平行枚举应统一 | ui/sort_types.h:6 vs fluent_components.h:29 |
| 重命名 | 4 路径 | 文件（hosted_edit 覆盖层）、标签页（app_commands.cpp:1527）、批量（batch_rename+dialog）、驱动器卷标（drive_label_rename）。对象不同属真实差异；应确认标签/文件是否共用 HostedEdit 控件，若各自实现编辑覆盖层则为重复【待确认】 | app_hosted_edit.cpp；app_commands.cpp:1527；drive_label_rename.cpp:6 |
| 文本渲染 | 3 | LumaText（主）、DirectWrite（回退）、原生 EDIT（最终回退）。主备关系有意，但选择规则分散在 compositor/painter/edit_host 三处；`Compositor::DrawLumaText` 对 fileNameFormat 一律返回 false 的硬编码（ui_compositor.cpp:143-144）说明路线未收敛 | fluent_components.cpp:495-503；edit_host.cpp:15-19 |
| 文本缓存 | 3 | MeasureCache 16384 条 + g_text_width_cache 4096 条 + g_text_layout_cache 1024/256 条，失效都依赖 `typography::Generation()`——同一代际三套表，应合并 | typography.cpp:273-307；ui_renderer_internal.h:78-80；fluent_components.cpp:115-120 |
| 控件 | 2×3 | 按钮：`Painter::DrawButton` vs `MainRenderer::DrawButton`（ui_renderer.cpp:393）；滚动条：`Painter::DrawScrollbar` vs `MainRenderer::DrawScrollbar`（ui_pane_list.cpp:1857）；工具提示：自绘 vs comctl32 原生（fluent_menu.cpp:845） | fluent_components.cpp:714,1787,1482 |
| 变更监听 | 3 系统 | fs_watch（RDCW/目录线程）+ folder_sizes subtree watch + index USN/RDCW。index 侧为索引服务属真实分工；前两者在主进程内重复监听同一目录树 | fs_watch.cpp:316；folder_sizes.cpp:307-321 |
| 预览 | 2 | 进程外 Pulse.Preview（缩略图+自绘预览）vs 进程内 IPreviewHandler 宿主（Alt+P 预览窗格）。缩略图出进程防爆是有意的；预览窗格在进程内是否同样应出进程需权衡（第三方 preview handler 崩溃会带走主进程）【推断】 | preview_handler_host.cpp:336-613；preview_host/main.cpp:490-555 |

---

## 7. 数据流与性能问题

| 路径 | 经过层次 | 重复数据/转换/IO | 影响 | 简化方向 | 证据 |
|---|---|---|---|---|---|
| 目录导航 | UI 点击 → NavigateTo → Tab → WorkerPool → fs 枚举 → SnapshotStore → WM_WORKER_RESULT → ApplyWorkerResult → DirEntry→ListEntryView | 文件项 5 种结构体（DirEntry/Hit/ContentHit/Row/ListEntryView），搜索结果 3 跳转换；tab 另持 snapshot 拷贝 | 每目录数据 2-4 份驻留；转换代码散布 | 统一文件项结构（DirEntry 为唯一规范），Hit/ContentHit 只在边界转换一次 | fs_enum.h:11；index_engine.h:42；content_search.h:53；ui_renderer.h:62；app_navigation.cpp:317 |
| 变更刷新 | RDCW 线程 / USN / 定时器 → 各自管线 → InvalidateRect（335 处/40 文件） | 三套监听系统并行；RefreshPath 12 个调用点；drive_label_rename 手改 snapshot 后又全量 RefreshPath | 刷新放大、线程/句柄浪费（fs_watch 上限 64 线程 + folder_sizes 每 root 一线程） | 主进程内变更通知单一来源（DirWatch 一处订阅、多方分发）；标签改名走增量更新 | fs_watch.cpp:12,316；app_navigation.cpp:1126；drive_label_rename.cpp:29 |
| 图标获取 | UI → ShellIconCache → worker SHGetFileInfoW | 图标不经 shell_host（有意）；但无 TTL 失效，系统图标变更不刷新；三级缓存（generic/exact 索引 + 128 位图） | 图标陈旧【推断：低频】；缓存层级偏多 | 保留单缓存+代际失效；评估是否并入 shell_host 统一 Shell 边界 | shell_icons.cpp:63-64,283-303 |
| 缩略图 | UI → ThumbnailCache ×2 实例 → 各自派生 Pulse.Preview 子进程 | 主窗格与快速预览窗各持一个 ThumbnailCache、各起一个 Preview 进程 | 进程与管道资源翻倍；失效代际各自为政 | 共享一个 ThumbnailCache/一个 Preview 连接 | ui_renderer.h:1177；quick_preview_window.h:142；thumbnail_cache.cpp:9-13 |
| 文件夹大小 | UI → FolderSizes → 索引服务 或 自写递归（含独立 DirWatch） | 与 fs_enum 重复的递归代码；JSON 磁盘缓存 + 内存 + 索引三态 | 重复 IO（与 SnapshotStore 并行枚举同目录） | 递归扫描复用 fs_enum；watch 复用 DirWatchSet | folder_sizes.cpp:72-118,307-321 |
| 管道 IO | 4 份阻塞读写实现（ipc 公共、preview、content_search_client、document_host）+ 5 套超时/重连/重拉逻辑 | 同模式代码 4-5 份 | 缺陷修复需在多处同步 | 收敛到 ipc/protocol.h 一套带超时的读写+重连 helper | protocol.h:312-354；preview_protocol.h:73-78；content_search_client.cpp:27-53；document_host_main.cpp:16-34 |
| UI 线程纪律 | 总体良好；风险点：拖放路径 SHParseDisplayName/GetFileAttributesW（OLE 模态期间）、空态 SVG 首次绘制同步建 WARP 设备、图形错误时 UI 线程同步写日志 | 网络路径上 SHParseDisplayName 可阻塞秒级【推断】 | 拖放卡顿、首帧卡顿 | 拖放解析下沉 worker；SVG 设备复用主设备或预热；日志异步 | drag_drop.cpp:65,335；svg_bitmap.cpp:23-59；ui_compositor.cpp:25-33 |

---

## 8. 架构摆放与耦合问题

1. **ui ↔ app 双向头依赖（P0）**：ui 至少 10 处包含 `../app/*`（places.h、resource.h、search_query.h、batch_rename.h、builtin_menu_*），而 `app/places.h:4` 回含 `../ui/view_layout.h`。后果：ui 无法独立编译/测试，"app 拥有状态、ui 拥有渲染"的分层名存实亡。方向：把 `places`（数据目录）下移到 common 或独立 model 头，resource id 收敛到一处。
2. **AppState 上帝结构（P1）**：约 470 字段（`app_state.h:132-696`），经 `GetAppState(hwnd)` 挂 GWLP_USERDATA 全局可达，36 个文件共享可变访问；且头级 include ui/fs/index/ops（:4-47）使所有包含者传递依赖全部模块。它实质是服务定位器的结构体形态。方向：按生命周期/子系统拆聚合（渲染上下文、导航上下文、搜索客户端组、ops 组），字段私有化+访问函数。
3. **app 层跨层直调（P1）**：app 直接 `ShellExecuteW`（app_main.cpp:417/423/1526）、`CoCreateInstance` 文件对话框（app_commands.cpp:1676/1701）、`SHGetFileInfoW`（home_catalog.cpp:18/32）、`GetFileAttributesExW`（app_model.cpp:901/1026、app_runtime.cpp:87/249）、`FindFirstFileW`（app_main.cpp:2354）。与"Shell 工作隔离到 shell_host"的既定边界不一致——要么承认边界是"仅隔离第三方 COM 与删除"并改文档，要么收口。
4. **领域代码错放（P2）**：文件夹大小计算在 app 而非 fs/ops（folder_sizes.cpp）；`common/shell_menu_item.h`、`common/preview_extensions.h` 是领域类型混入工具层；`common/drive_labels.h` 与 `app/drive_labels.h` 同名不同签名（P1 易错包含）。
5. **测试代码散在产品目录（P2/P3）**：src/app 内约 9.9k 行测试（selftest_1b2.cpp 单文件 6226 行 + 12 个 *_test.cpp）；`src/ui/ui_hit_test.cpp` 名字像测试实为命中测试实现，命名误导。
6. **键盘输入双路径（P2）**：`HandleKeyDown`（app_input.cpp:3441）为主，但 WndProc 直接处理 Alt+D/Alt+Enter/Esc/WM_HOTKEY（app_main.cpp:1181-1209），快捷键语义分散两处。

---

## 9. 过度设计、冗余、兜底、臭抽象、无意义文件拆分

- **无证据 fallback**：`fs_enum.cpp:403` `catch(...)` 无日志吞掉 NT 枚举失败回退 FindFirstFileEx——回退原因不可诊断，且 NT API 为未文档化接口。建议：要么记录回退原因，要么直接用 FindFirstFileEx（LARGE_FETCH 已获大部分收益）删双路径【建议，需 benchmark 佐证】。
- **重复防御**：路径助手三处重复——`ParentOf`（ops_manager.cpp:134-157）≈ `fs::ParentPath`（fs_enum.cpp:170）；`DisplayPath`（ops_manager.cpp:193-197）≈ `path::StripExtendedPathPrefix`（common/path_utils.h:25，语义略异）；`FileName`（ops_manager.cpp:31）≈ `FileNameOf`（fs_recycle.cpp:13）。
- **无意义文件拆分**（单调用方/几十行）：`content_lifecycle.cpp`（23 行）、`drive_label_rename.cpp`（32 行）、`search_result_merge.cpp`（53 行）、`date_groups.cpp`（69 行）、`change_tracking_polling.h`（8 行单包含）、`breadcrumb_menu.cpp`（无对应头）；全局搜索拆 3 文件（hotkey 29 行 + controller 56 行 + window 696 行）；index 一族单使用 header（usn_read_wait.h、usn_packet_queue.h、change_journal_writer.h 等各 1 个 include 点）。
- **巨头文件反向问题**：app_input.cpp 3630 行、app_main.cpp 2405 行（入口+全消息路由+截图脚手架）、app_runtime.cpp 2344 行、app_commands.cpp 2022 行、fluent_components.cpp 2782 行、ui_renderer_internal.h 2179 行（头文件承载大量实现）。拆分方向合理但粒度两极分化。
- **每 UNC 探测起两线程**：`fs_net_cache.cpp:183-215` CreateThread + detached std::thread 并存。
- **偏好双写**（见 §1-7）：属"同一状态多处真相"，是冗余而非兜底。

---

## 10. 第一性原理简化清单（建议）

按"最小必要"原则，从用户任务反推：

1. **一个文件项结构**：DirEntry 为全项目唯一规范结构；Hit/ContentHit 仅在索引 IPC 边界转换为 DirEntry；ListEntryView 改为 DirEntry 的视图引用（去一次拷贝）。
2. **一个变更源**：主进程内 DirWatchSet 为唯一点，folder_sizes、snapshot 失效、UI 刷新全部订阅它；folder_sizes 的 subtree watch 与独立递归扫描删除（复用 fs_enum 的迭代器接口）。
3. **一个搜索客户端组**：全局搜索窗改用 AppState 已有三客户端（或把客户端组提取为可共享对象），删除第二套实例与 controller 转发层；合并两份搜索历史。
4. **一个枚举实现**：收敛到 FindFirstFileEx 或 NT API 其一（以 benchmark 为准），另一路径删除或带日志保留；ops/folder_sizes 复用同一迭代入口。
5. **一套管道 IO 与重连**：ipc/protocol.h 提供带超时的读写 + 统一"断管→杀子进程→重启→重放一次"helper，preview/content/document 三份私实现删除。
6. **一个 ThumbnailCache**：主窗格与预览窗共享实例与 Preview 子进程连接。
7. **一套文本度量缓存**：三个 cache 合一，失效沿用 typography::Generation。
8. **控件去重**：MainRenderer 的 DrawButton/DrawScrollbar 删除，统一走 Painter；菜单 tooltip 统一自绘或统一原生（建议自绘，删 comctl32 依赖）。
9. **状态单真相**：darkMode/change_tracking_enabled/folder_sort_mode 只存 AppPrefs，session 与全局变量改为读取时投影；删除每 tick 重同步循环。
10. **内联胶水文件**：§9 列出的单调用方小文件并入调用方；index 单使用 header 并入使用点。
11. **同名异物清理**：common/drive_labels.h 并入 app/drive_labels.h（或反之），SortDirection 保留 ui/sort_types.h 一处。
12. **删假接口**：ShellClient 恒失败公有桩私有化；ops_manager.h 头注释改为如实描述分工。

---

## 11. 去耦合优先级路线图（只给方向）

1. **第一阶段（编译边界）**：斩断 ui→app 头依赖（places/resource/菜单字符串下移）；拆分 app_state.h 的 include 全集。收益：分层可验证、编译依赖收敛。风险低，纯移动。
2. **第二阶段（状态收口）**：AppState 按子系统聚合分组+访问函数化；偏好状态单真相化（删双写/重同步）。收益：消除整类状态不一致 bug。
3. **第三阶段（数据流统一）**：文件项结构统一 + 变更源统一 + 枚举实现统一。这是性能与内存收益最大的一步，也是改动面最大的一步，建议在 1、2 之后做。
4. **第四阶段（功能去重）**：搜索客户端组共享、缩略图缓存共享、控件/文本缓存去重、IPC helper 收敛。彼此独立，可并行逐项落地。
5. **全程不做的事**：不引入新抽象层、不引入事件总线/接口工厂（本项目无此问题，谨防矫枉过正）、不做跨平台抽象。

---

## 12. 风险与不确定项

- 【待确认】主窗格与快速预览的两套 ThumbnailCache 是否有意隔离（失效代际独立）——合并前需确认。
- 【待确认】标签页重命名与文件重命名是否共用 HostedEdit 控件实现。
- 【待确认】NT 枚举 vs FindFirstFileEx 的性能差异无 benchmark 数据，删双路径前需实测（bench_data 可用）。
- 【待确认】ShellIconCache 无 TTL 失效是已知取舍还是疏漏。
- 【待确认】全局搜索窗在网络索引未配置时的合并行为（app_navigation.cpp:228）仅静态阅读，未运行验证。
- 【待确认】进程内 IPreviewHandler 宿主（preview_handler_host）的崩溃风险是否已被第三方 handler 实际触发过——决定是否迁出进程。
- 本报告未做运行时验证（未启动应用、未 profiling），性能影响量级为静态推断。
- 搜索"5+ 入口"中地址栏/命令面板/高级搜索可能共用后端，重复主要在 UI 与历史存储，实际冗余度低于入口数。

---

## 13. 附录：证据索引

**耦合**：app_state.h:4-47,132-696,724；app/places.h:4；ui_renderer_internal.h:10；ui_sidebar.cpp:10；app_main.cpp:417,1181-1209,1526,2354；app_commands.cpp:1676；home_catalog.cpp:18,32。

**变更/枚举**：fs_watch.cpp:12,209,316,370-397；folder_sizes.cpp:72-118,208,307-321；fs_enum.cpp:144-168,196-231,233-330,400-407；ops_manager.cpp:2137；app_change_tracking.cpp:83-166；app_navigation.cpp:1126,1467-1475；drive_label_rename.cpp:29；index_engine.cpp:184,2836,3679。

**搜索**：global_search_window.cpp:81-85,174-181；app_state.h:237-241；app_navigation.cpp:218-231,317；search_options.cpp:59,96-97；search_result_merge.cpp:14,32；index_client.cpp:65,113,337。

**ops/shell**：ops_manager.h:3-5,360-423；ops_manager.cpp:31,134-157,193-197,237-239,320,2226-2249,2266,2556,2608；shell_client.cpp:181-190,200-212,482-500；shell_host/main.cpp:152,629-638；fs_recycle.cpp:13,22-157。

**ui**：fluent_components.cpp:115-120,495-503,714,1482,1787；ui_renderer.cpp:393；ui_pane_list.cpp:1857；ui_compositor.cpp:25-33,138-152；shell_icons.cpp:63-64,182,238,266-303；thumbnail_cache.cpp:9-13,31-68,145-146,260-263；typography.cpp:273-307；drag_drop.cpp:56-81,65,335；svg_bitmap.cpp:23-59；preview_handler_host.cpp:336-613；edit_host.cpp:15-19,34-39。

**ipc/协议**：protocol.h:69,118-354；preview_protocol.h:41-78；document_protocol.h:22-52；content_search_client.cpp:27-53,256-270；document_host_main.cpp:16-34；folder_size_protocol.h:6。

**杂项**：common/drive_labels.h:19 vs app/drive_labels.h:8；ui/sort_types.h:6 vs fluent_components.h:29；default_file_manager.h:21-31；session.h:42；entry_sort.cpp:65；fs_net_cache.cpp:183-215；selftest_1b2.cpp（6226 行）；app_input.cpp（3630 行）；ui_renderer_internal.h（2179 行）。