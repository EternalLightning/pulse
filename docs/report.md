# Pulse 原生 Windows 文件管理器只读审计报告

## 0. 审计范围与方法

### 0.1 范围与基线

- 审计对象：[Pulse 工作区](<E:/Projects/pulse/>) 的**当前工作树**；Git 基线为 `fec6df9`，工作区已有大量未提交变更。本报告不把这些变更视为本次审计产生的修改。
- 按要求排除 `build`、`build-release`、`third_party`、`dist` 的生成物或第三方实现；阅读构建声明以确认依赖及目标，不审计第三方库内部。
- 重点覆盖：
  - Win32 主循环、线程、COM apartment 与关闭路径；
  - 目录枚举、快照、排序、通知、UNC 探测与缓存；
  - 文件操作、删除授权、撤销、Shell 菜单及拖放；
  - 本地／网络索引、USN、MFT、内容搜索、文档读取；
  - 图标、缩略图、快速预览、视频与文字渲染；
  - 模型、持久化、模块耦合及测试边界。
- 文件发现约涉及 583 条源代码路径；采用**入口到实现、实现到消费者、正常路径到失败路径**的定向追踪，不宣称逐行覆盖所有代码。

### 0.2 执行方式与实际验证

本次未修改项目文件，未生成补丁或报告文件，未构建，未运行功能测试、基准测试或安全利用验证。

实际执行的只读检查：

| 检查 | 实际结果 | 能证明什么／不能证明什么 |
|---|---|---|
| [UI 依赖检查脚本](<tools/check_ui_dependencies.py>)，使用 Python `-B` 执行 | `[PASS] 92 UI files; no direct or transitive includes into src/app` | 证明受检查 UI 文件没有包含应用层头文件；**不证明语义解耦、线程安全或运行正确** |
| `git diff --check`，排除上述目录 | 退出码 0；有 CRLF 提示 | 未发现该检查识别的补丁空白错误；不代表编译或测试通过 |
| 工作树、相关 diff、测试源码检查 | 已检查 | 用于区分现有实现、迁移别名和测试变化；不推断历史测试执行情况 |

遵循了[仓库工作规则](<AGENTS.md>)的只读边界。四个独立工作包分别审计文件系统、操作／Shell、索引、预览，主审复核高风险调用链并统一结论。

### 0.3 结论标记

- **事实**：源码可直接确认的控制流、字段、调用、锁或协议行为。
- **推断**：在明确触发条件下，由事实推导的运行后果；不等于已动态复现。
- **建议**：设计或修复方向，不是本次已执行的修改。
- **待确认**：依赖 Windows 版本、第三方 provider、权限环境或运行测量的结论。

严重度按影响区分：**P0** 为正确性、崩溃、灾难性性能或关键契约不可靠；**P1** 为架构、分层、调度和数据流问题；**P2** 为冗余、过度设计及不必要成本；**P3** 为维护性问题。后文用 `F01` 等稳定编号关联证据和建议。

---

## 1. 执行摘要

### 总体判断

项目不是“只有界面、没有实现”的样板：已有实质性的原生文件操作、索引、授权、COM 隔离、缓存与测试体系。多进程边界、不可变快照、删除授权门、网络凭据隔离等方向总体合理。

但几个基础身份与生命周期契约尚未闭合：

1. **路径被当成原对象身份**，影响跨卷移动和撤销。
2. **异步请求发送先于回调路由注册**，并用一个全局槽保存不同请求的完成结果。
3. **缓存菜单布局被当成可执行命令身份**，且 host 重绑后仍转发旧 ID。
4. **“读到部分数据”被当成完整索引成功**，混合 USN reason 又可能被提前丢弃。
5. **逻辑超时／代际失效不等于物理工作结束**；部分 UNC 和视频任务没有真实在途上限。
6. **路径级共享快照混入视图排序**，导致多个消费者对同一缓存的解释不一致。

**建议优先处理这些契约，再做模块整理；仅移动文件、增加包装层或重命名接口不能消除风险。**

### 1.1 P0 优先项

| 编号 | 发现 | 条件与后果 | 核心证据 |
|---|---|---|---|
| **F01** | 请求先发后注册；DONE 单槽；解锁后读写完成字段 | 快回调可丢菜单结果或覆盖文件完成；存在真实 C++ 数据竞争，操作队列可能一直等待 | [请求发布](<src/ops/ops_manager.cpp#L1971-L1995>)、[完成回调](<src/ops/ops_manager.cpp#L1782-L1801>)、[等待消费](<src/ops/ops_manager.cpp#L3373-L3405>) |
| **F02** | 跨卷 Move 提交目的后按源路径删除 | 复制结束后源路径被替换，可永久删除未被复制的新对象 | [复制及提交后删除](<src/ops/ops_manager.cpp#L2619-L2771>)、[计划字段](<src/ops/ops_manager.h#L88-L99>) |
| **F03** | Copy／Create Undo 未绑定原产物身份 | 同名产物被替换后，撤销可回收无关对象；网络路径新确认后可永久删除 | [Undo 记录](<src/ops/ops_manager.h#L101-L110>)、[撤销转换](<src/ops/ops_manager.cpp#L1524-L1546>) |
| **F04** | Shell handler 线程启动失败分支 UAF | 事件创建失败、线程创建成功时，线程继续使用已释放 worker | [创建及检查顺序](<src/shell_host/main.cpp#L1254-L1269>)、[线程访问](<src/shell_host/main.cpp#L1115-L1153>) |
| **F05** | Shell 命令身份不可靠，重绑 `live` 后仍发送旧 ID | 动态菜单、缓存跨会话或 ID 复用时可能执行非用户所选命令 | [应用层重绑](<src/app/context_menu_controller.cpp#L238-L253>)、[host 分派](<src/shell_host/main.cpp#L1343-L1358>) |
| **F06** | 本地索引客户端不验证服务端，响应 count 可先触发巨大分配 | 真 host 未占端点时，伪服务端可伪造结果或通过恶意计数触发未捕获异常 | [连接](<src/index/index_client.cpp#L98-L138>)、[分配](<src/index/index_client.cpp#L195-L259>) |
| **F07** | `SECURITY_CHANGE` 提前返回，丢弃同一 USN 的其他 reason | 混合改名／删除／创建／数据变化被漏应用，cursor 仍推进 | [reason 分支](<src/index/index_engine.cpp#L2584-L2602>)、[cursor 提交](<src/index/index_engine.cpp#L2888-L2915>) |
| **F08** | MFT 中段读取失败仍返回成功 | 发布缺失的基线快照，未变化的既存文件不会靠后续 USN 自然补齐 | [MFT 返回条件](<src/index/index_mft.cpp#L303-L340>)、[fallback 条件](<src/index/index_engine.cpp#L2941-L2955>) |
| **F09** | 网络搜索失败不产生客户端终态 | 主窗等待两端 ready，可阻止已完成的本地结果交付 | [错误响应](<src/index/network_agent_main.cpp#L94-L113>)、[客户端早退](<src/index/network_agent_client.cpp#L215-L247>)、[主窗合并](<src/app/app_navigation.cpp#L629-L644>) |
| **F10** | 订阅去重同时排除 `error`、`incomplete` | rows／total 不变时，结果质量变化不再通知客户端 | [payload 尾部](<src/index/index_host.cpp#L295-L310>)、[去重范围](<src/index/index_host.cpp#L448-L460>) |

以上均有静态证据；具体崩溃、误执行、数据损失及利用过程**未动态复现**。

### 1.2 P1 处理重点

- **F11–F17：目录与调度。** 快照未包含排序身份；列式视图可接受更旧结果；过时 UNC 枚举仍占 worker；不支持通知的 provider 可热重试；UNC 探测及部分关闭路径没有真正有界的物理结束契约。
- **F18–F20：接口与持久化。** 剪贴板 HDROP 处理不一致；压缩包密码状态依赖英文诊断；简化 JSON parser 不满足结构与数值边界。
- **F21：内容搜索。** scope／size 未及时下推，局部查询仍承担广域候选与 N+1 访问。
- **F22–F27：预览。** 临时失败长期负缓存、属性协议限额不一致、文本截断状态不完整、LumaText alignment 未入缓存键、视频退休资源未限流、GIF 重复前缀合成。
- **F29–F31：共享边界。** UNC 缓存并发写入、路径规范化和“大 AppState＋含领域对象的 ViewModel”仍需收敛。

### 1.3 应保留的正确方向

- Shell IPC 已有当前用户 DACL、拒绝远程、first-instance、双向 PID 校验、负载上限和删除 epoch／一次性授权。
- 真实索引服务已有调用者令牌与可见性检查；不能因 **F06 客户端认证遗漏**断言整个服务无权限控制。
- 网络索引保持用户凭据边界，不能为了统一实现搬进 SYSTEM 服务。
- 文档解析器没有加载进 UI／持久索引 writer，已有进程、资源和超时隔离。
- 不可变目录快照、共享 CPU 预览结果、按消费者保留 GPU 资源均有合理依据。
- 内置菜单和 Banner 已有类型化身份；不是所有界面文字比较都属于机器契约错误。

---

## 2. 技术栈、模块与入口

### 2.1 技术栈

| 层面 | 已确认技术 | 证据及说明 |
|---|---|---|
| 编译与平台 | C++20、MSVC、CMake 3.25+、Windows SDK | [构建定义](<CMakeLists.txt#L1-L30>)；启用 `/W4`、`/permissive-`、CFG 等 |
| 界面 | Win32、Direct2D、DirectWrite、DirectComposition | [应用状态](<src/app/app_state.h#L132-L170>)、[渲染接口](<src/ui/ui_renderer.h#L1-L16>) |
| 文字 | DirectWrite；可选 LumaText | [选择与 fallback](<CMakeLists.txt#L32-L79>) |
| 文件系统 | Win32 文件 API、NT 目录查询、`ReadDirectoryChangesW`、目录快照 | [NT 枚举](<src/fs/fs_enum.cpp#L330-L350>)、[通知](<src/fs/fs_watch.cpp#L209-L260>) |
| 文件操作 | `CopyFile2`、handle／Win32 rename、Shell 删除与回收 | [操作分派](<src/ops/ops_manager.cpp#L1269-L1295>) |
| 搜索 | MFT／USN、用户态 UNC 索引、SQLite FTS5、SHA-256 | [索引目标](<CMakeLists.txt#L363-L390>)、[SQLite 配置](<CMakeLists.txt#L705-L729>) |
| 预览／文档 | WIC、SVG／metafile 分支、Shell thumbnail、MFPlay、`IPreviewHandler`、OPC、IFilter、PDFium | [预览分派](<src/preview_host/main.cpp#L560-L719>)、[文档分派](<src/index/document_reader.cpp#L519-L540>) |
| IPC | 命名管道、typed header、UTF-16／数值 payload、共享图像数据 | [Shell 协议](<src/ipc/protocol.h>)、[预览协议](<src/ipc/preview_protocol.h#L7-L74>) |

Windows 8.1 候选构建有工具链约束与 CI 声明，但本次不能认证实际平台兼容性，见[构建约束](<CMakeLists.txt#L21-L30>)及[发布工作流](<.github/workflows/release.yml#L1-L36>)。

### 2.2 进程与线程边界

```text
主 UI
 ├─ 目录 WorkerPool → fs 枚举 → immutable snapshot → 窗口消息
 ├─ Ops file worker / open worker
 │    └─ ShellClient → Shell helper
 │          ├─ 主 STA：文件 Shell 操作
 │          └─ 菜单会话 STA → 各 handler STA
 ├─ IndexClient → 本地索引 helper / service
 ├─ NetworkAgentClient → 当前用户网络索引 agent
 ├─ ContentSearchClient → 内容查询 / writer / observer 模式
 │    └─ 文档 helper：文本提取
 └─ Thumbnail / QuickPreview
      ├─ preview helper：图像、文本、属性等
      ├─ IPreviewHandler 隔离路径
      └─ 视频 worker：MFPlay
```

| 入口／模块 | 线程及 COM 事实 | 审计评价 |
|---|---|---|
| `wWinMain` | UI 调用 `OleInitialize`，通过 `GetMessage` 驱动 | [入口](<src/app/app_main.cpp#L1981-L1992>)、[消息循环](<src/app/app_main.cpp#L2394-L2401>) |
| 操作 worker／打开 worker | 均初始化 STA；打开与传输分开 | [操作 worker](<src/ops/ops_manager.cpp#L1752-L1758>)、[打开 worker](<src/ops/ops_manager.cpp#L1610-L1615>)；避免打开文件阻塞传输是合理分工 |
| Shell helper | 主 STA 使用窗口消息；菜单协调器和 handler 各自 STA 并泵消息 | [主线程](<src/shell_host/main.cpp#L1563-L1607>)、[菜单线程](<src/shell_host/main.cpp#L1114-L1197>) |
| 本地索引 | accept／search、client reader／writer，engine 与各 volume USN 工作线程 | [索引 host](<src/index/index_host.cpp>)；不是单线程读库服务 |
| 文档 helper | 主 STA；PDF IFilter 分支使用独立 MTA | [文档 host](<src/index/document_host_main.cpp#L67-L98>) |
| Details 元数据 worker | 独立 `std::thread`；已读路径没有 COM 初始化，调用 `SHGetFileInfoW` | [worker](<src/app/app_runtime.cpp#L62-L115>)；见 F17 |

已读的 Shell／图片打开路径未发现直接把未封送的 COM interface 裸指针跨 STA 传递。图片打开站点使用 FTM，保存 PIDL／快照并在调用 apartment 内重新绑定，见[图片打开实现](<src/ops/image_open.cpp#L55-L118>)。这不等于完成了所有 COM 调用的兼容性验证。

---

## 3. 实际功能识别

以下是**源码已接线的功能**，不是本次运行验收结果：

1. **文件浏览**：目录导航、返回／前进、窗口标签、分栏、列式浏览、排序、过滤、选择、缓存优先显示。
2. **文件操作**：复制、移动、冲突处理、暂停／取消、可选 SHA-256 校验、单项／批量重命名、新建、回收、永久删除、恢复、清空回收站及部分撤销。
3. **Shell 互操作**：打开／打开方式、属性、第三方右键菜单、系统剪贴板、拖入／拖出。
4. **搜索**：本地文件名索引、UNC 文件名索引、拼音查询、主窗与独立全局搜索窗口、内容搜索、索引缓存及实时补扫、保存搜索。
5. **重复文件**：范围选择、大小分桶、采样、完整哈希、结果交互。
6. **预览**：图标、缩略图、详情预览、快速预览、文本／十六进制、图像／GIF、Shell preview handler、视频。
7. **压缩包**：7z 驱动的列表浏览、密码交互、提取和打开提取产物。
8. **组织功能**：稳定 TagId、标签 ADS、收藏／最近项目、工作区和网络位置。
9. **辅助信息**：目录大小、元数据／权限摘要、目录变化记录。
10. **应用设置**：外观、语言、DPI 相关布局、快捷键、托盘及会话持久化。

代表性接线证据：[导航](<src/app/app_navigation.cpp#L1537-L1550>)、[操作分派](<src/ops/ops_manager.cpp#L1269-L1295>)、[重复扫描](<src/app/duplicate_scan_ui.cpp#L39-L83>)、[全局搜索](<src/app/global_search_controller.cpp#L35-L55>)、[快速预览](<src/app/app_commands.cpp#L1776-L1788>)、[标签写入](<src/app/app_commands.cpp#L185-L193>)。

---

## 4. 功能—接口—实现映射表

“实现数”限定为本次追踪到的生产实现或执行分支，不把类、头文件、兼容别名数量当成实现数量。

| 领域 | 用户功能 | 入口／命令 | 接口 | 实现 | 调用者 | 实现数 | 状态 | 证据 |
|---|---|---|---|---|---|---|---|---|
| 浏览 | 目录导航／刷新 | `NavigateTo`、`Refresh` | `WorkerPool`、`SnapshotStore` | NT 批量枚举＋Win32 兼容／流式分支 | 主窗各 pane | 1 管线，2 类枚举后端 | 已接线；取消与视图身份存在问题 | [导航](<src/app/app_navigation.cpp#L1537-L1550>)、[worker](<src/app/app_worker.cpp#L219-L281>) |
| 浏览 | 排序 | `SetSort` | `Refresh(path,col,dir,folders)` | worker 排序／`EntryLess` | 标签、列表、列式视图 | 共用比较逻辑；排序位置不止一处 | 排序触发重枚举；F11 | [排序入口](<src/app/app_navigation.cpp#L1658-L1685>)、[比较器](<src/app/entry_sort.cpp#L39-L108>) |
| 浏览 | 标签／分栏／列式浏览 | 窗口标签及 pane 命令 | `Tab`、布局、`ColumnStrip` | 应用模型与结果分发 | 主窗 | 1 模型体系 | 结果代际契约不一致 | [布局模型](<src/app/app_model.h#L342-L375>)、[列式结果](<src/app/app_column_view.cpp#L289-L306>) |
| 浏览 | 目录自动更新 | UI timer | `DirWatchSet`、notify batch | watch thread→批处理→快照补丁 | 主窗 | 1 watch 管线 | 生命周期有保护；失败重试与 UI I/O 有问题 | [timer](<src/app/app_main.cpp#L934-L943>)、[补丁分发](<src/app/app_navigation.cpp#L1269-L1301>) |
| 操作 | 复制／移动 | `Submit` | `OpRequest`、transfer plan | `RunTransfer` | 菜单、快捷键、拖放、操作 UI | 1 流程；普通文件／同卷／reparse 分支 | F02；目的对象保护较强 | [分派](<src/ops/ops_manager.cpp#L1292-L1295>)、[复制](<src/ops/ops_manager.cpp#L2606-L2619>) |
| 操作 | 重命名／批量重命名 | Rename／BatchRename | `RunShellOp`→`RenameInProcess` | 本地 no-overwrite／handle 路径 | rename UI／批量工具 | 1 已接线；另有 IPC 候选旧路径 | IPC 路径未找到生产调用 | [批量入口](<src/ops/ops_manager.cpp#L3138-L3158>)、[单项入口](<src/ops/ops_manager.cpp#L3205-L3212>)、[IPC 接口](<src/ipc/shell_client.h#L48-L52>) |
| 操作 | 删除／回收／清空回收站 | 删除命令 | `DeleteService`→私有 `DeleteAuthorized` | host admission→永久删除／显式回收 | 所有删除请求 | 1 授权门，2 语义后端 | 合理分支，不是重复删除实现 | [计划](<src/ops/delete_operation.cpp#L49-L100>)、[私有边界](<src/ipc/shell_client.h#L69-L72>) |
| 操作 | 新建／回收站恢复 | Create／Restore | `ShellClient` | host `ExecuteCreate/ExecuteRestore` | 操作 worker | 1 IPC 管线，多命令 | 已接线；共享 F01 风险 | [接口](<src/ipc/shell_client.h#L48-L52>)、[host 分派](<src/shell_host/main.cpp#L613-L625>) |
| 操作 | 撤销 | `Undo` | `UndoEntry` | 操作反向请求 | 操作 UI | 1 栈／多操作逆向 | Copy／Create 产物身份缺失，F03 | [记录](<src/ops/ops_manager.cpp#L1430-L1441>)、[撤销](<src/ops/ops_manager.cpp#L1524-L1546>) |
| 互操作 | 复制／剪切／粘贴 | Clipboard commands | `WriteClipboard/ReadClipboard` | Win32 clipboard | UI、暂存区 | 1 读写实现 | HDROP 契约待运行确认，F18 | [读取](<src/ops/clipboard.cpp#L115-L127>) |
| 互操作 | 拖入／拖出 | OLE drop／drag | drop callbacks、`IDataObject` | Shell data object 优先，最小 CF_HDROP fallback | 主窗 | 2 data object 策略 | 合理互操作降级 | [拖出](<src/ui/drag_drop.cpp#L492-L512>)、[拖入解析](<src/ui/drag_drop.cpp#L150-L155>) |
| Shell | 打开／属性 | `QueueOpen`／properties | 独立打开 worker／属性 STA | Shell 打开、图片打开站点 | 主窗 | 各功能 1 路径 | 有实际 COM 分工 | [打开 worker](<src/ops/ops_manager.cpp#L1610-L1615>)、[多文件属性](<src/ops/ops_manager.cpp#L68-L123>) |
| Shell | 第三方右键菜单 | Query／Invoke | controller→Ops→ShellClient | 会话 STA＋handler STA；aggregate fallback | 主窗菜单 | 1 服务，2 绑定策略 | F01、F04、F05 | [菜单请求](<src/ops/ops_manager.cpp#L1958-L1995>)、[会话实现](<src/shell_host/main.cpp#L1197-L1370>) |
| 搜索 | 文件名搜索 | SearchAsync | `IndexClient`／`NetworkAgentClient` | 本地 `Engine::Search`／UNC `SearchLoop` | 主窗、全局窗口 | **2 engine** | 权限域不同，不能直接合并 | [本地客户端](<src/index/index_client.cpp>)、[网络客户端](<src/index/network_agent_client.cpp#L215-L247>) |
| 搜索 | 全局搜索窗口 | `ToggleGlobalSearch` | 共享文件名 clients | 独立窗口 debounce／merge／timeout | 全局快捷键 | 2 UI 前端，复用 engine | 该窗口有 10 秒 timeout | [接线](<src/app/global_search_controller.cpp#L35-L46>)、[timeout](<src/app/global_search_window.cpp#L537-L541>) |
| 搜索 | 内容搜索 | Content SearchAsync | content request／batch | one-shot、SQLite cache、`SearchTask` 校验＋补扫 | 主窗、全局窗口、保存搜索 | **3 执行路径** | 不是 3 个可互删实现；F21 | [接口与模式](<src/index/content_index.h#L7-L68>)、[查询](<src/index/content_index.cpp#L1275-L1387>) |
| 搜索 | 重复文件 | `StartDuplicateScan` | `ContentSearchMode::Duplicates` | 大小→sample→SHA256→文件身份去重 | duplicate UI | **1 算法** | 已接线；未运行大数据／竞争测试 | [入口](<src/app/duplicate_scan_ui.cpp#L39-L75>)、[算法](<src/index/content_search.cpp#L432-L509>) |
| 文档 | 搜索文档内容 | document reader | 单一提取入口 | 文本、OPC、Office IFilter、PDF 分支 | content 查询／writer | 1 入口，4 类分支 | 进程隔离有依据 | [分派](<src/index/document_reader.cpp#L519-L540>)、[隔离](<src/index/document_reader.cpp#L297-L343>) |
| 压缩包 | 浏览／密码／提取 | archive navigation | `ArchiveResult`／7z adapter | 外部进程＋`-slt` 解析 | archive worker/UI | 1 backend | 错误／密码协议不可靠，F19 | [结果类型](<src/ops/archive.h#L17-L21>)、[消费](<src/app/archive_navigation.cpp#L225-L252>) |
| 预览 | 图标／缩略图／属性 | Draw／Properties | icon cache／thumbnail cache | shared CPU result、consumer GPU | 列表、列式、详情、侧栏、快速预览 | 各服务 1；多个消费者 | 有共享机制；F22、F23、F28 | [共享 owner](<src/ui/ui_renderer.h#L1179>)、[属性读取](<src/ui/thumbnail_cache.cpp#L442-L456>) |
| 预览 | 图像／GIF／文本／hex | preview request | `PreviewRequest/Response` | 一个 helper dispatcher，多媒体分支 | thumbnail／quick preview | 1 dispatcher，2 request kind，多内容分支 | F24、F27 | [协议](<src/ipc/preview_protocol.h#L8-L69>)、[分派](<src/preview_host/main.cpp#L560-L719>) |
| 预览 | 快速预览／handler／视频 | `ToggleQuickPreview` | `QuickPreviewWindow`／handler／video | 各自一个生产实现 | 主窗 | 3 职责不同的服务 | 不应合并为万能 decoder；F26 | [入口](<src/app/app_commands.cpp#L1776-L1788>)、[视频](<src/ui/video_preview.cpp#L142-L178>) |
| 文字 | UI 文字绘制 | Draw text | typography／Luma renderer | DirectWrite／LumaText | 各 renderer | 2 可选 backend | 合理 fallback；F25 | [选择](<CMakeLists.txt#L32-L79>)、[缓存](<src/ui/lumatext_renderer.cpp#L616-L662>) |
| 辅助 | 目录大小 | Calculate／详情选择 | `FolderSizes`；独立 details scan | 索引查询／递归扫描 | 列表与详情 | **2 递归扫描入口＋索引后端** | 存在真实收敛机会，F16 | [FolderSizes](<src/app/folder_sizes.cpp#L383-L417>)、[详情扫描](<src/app/app_runtime.cpp#L959-L1042>) |
| 组织 | 标签／工作区／收藏／最近 | tag／pin 命令 | `PlacesCatalog`、稳定 TagId | catalog 持久化＋ADS adapter | 菜单、侧栏、虚拟视图 | 1 catalog，非别名双实现 | 模型仍混合 I/O 职责 | [模型](<src/model/places.h#L21-L120>)、[标签接线](<src/app/app_commands.cpp#L185-L193>) |
| 辅助 | 变化记录 | timer／结果消息 | change client／response | service 查询→UI 投影 | 主窗 | 1 管线 | 受索引一致性及端点信任影响 | [接收](<src/app/app_change_tracking.cpp#L271-L281>) |
| 应用 | 会话／托盘 | startup／tray commands | session store／`TrayController` | JSON 持久化／Win32 notification icon | 主窗 | 各 1 | JSON 边界需加强 | [会话读取](<src/app/session.cpp#L356-L370>)、[托盘](<src/app/tray_controller.cpp#L13-L95>) |

---

## 5. 接口问题

### 5.1 异步完成接口：注册、隔离、终态和消费不是同一个原子契约

#### F01 — P0：请求发布竞态＋全局 DONE 单槽＋解锁后数据竞争

**事实**

- `MenuThread` 先调用 Query，再登记 request ID 与 token 的映射；Invoke 也先发送，再加入 `ctx_invoke_ids_`。见[发布顺序](<src/ops/ops_manager.cpp#L1971-L1995>)。
- `OnCtxItems` 找不到映射就丢弃结果，见[菜单回调](<src/ops/ops_manager.cpp#L1941-L1948>)。
- 未被识别为菜单完成的 DONE 写入全局 `done_id_/done_ready_/done_error_` 单槽，见[DONE 回调](<src/ops/ops_manager.cpp#L1782-L1801>)。
- `WaitShellDone` 等待区持锁，但退出该作用域后才读取字段、移动 `std::wstring`、清 `done_ready_`；其他线程在锁内写同字段。见[消费位置](<src/ops/ops_manager.cpp#L3373-L3405>)。
- transport 发送失败可以在 `Submit` 返回前同步 `FireDone`；正常 reader 在回调前已移除 pending。见[Submit](<src/ipc/shell_client.cpp#L165-L195>)、[reader](<src/ipc/shell_client.cpp#L415-L416>)。

**调用链**

`MenuThread → ShellClient::Submit → reader／FireDone → Ops callbacks`  
并发于  
`WorkerThread → RunDelete／RunShellOp → WaitShellDone`。

**触发与推断**

快回调可能早于映射注册；早到的菜单 DONE 可覆盖文件 DONE。被覆盖请求的 pending 已移除，超时 `Abort` 又只在 pending 仍存在时补发完成，见[Abort](<src/ipc/shell_client.cpp#L262-L280>)。因此，等待可能持续到停止；解锁后的字符串移动与并发赋值构成实际 C++ 数据竞争。具体崩溃未动态验证。

**建议**

ID 预留及回调路由注册先于发送；按 request ID 保存独立结果；在同一锁内一次移出完整完成对象，包括删除证据。不要继续用“一个文件 worker”解释跨类别共享单槽的安全性。

#### F09 — P0：网络搜索错误没有交付终态

**事实**

host 过载或超时返回 `RSP_RESULT` 文本，客户端遇到请求失败或非 `RSP_SEARCH` 直接返回，没有登记结果或通知。见[服务端](<src/index/network_agent_main.cpp#L94-L113>)、[客户端](<src/index/network_agent_client.cpp#L215-L247>)。

主窗登记两路查询，并在网络 roots 存在时等待两路 ready，见[查询登记](<src/app/app_navigation.cpp#L216-L232>)、[合并屏障](<src/app/app_navigation.cpp#L629-L644>)。

**推断**

网络端失败可阻止已经返回的本地结果发布。**独立全局搜索窗口不能混为同一行为**：它另有 10 秒 timer，见[启动 timer](<src/app/global_search_window.cpp#L181-L187>)和[超时处理](<src/app/global_search_window.cpp#L537-L541>)。

**建议**

每条请求必有 success／error／cancel／partial 终态；错误码、完整性和请求身份作为字段，不靠诊断文本。合并器应允许失败来源贡献“已完成但不完整”，而不是永远未 ready。

#### F10 — P0：订阅比较忽略了结果质量

**事实**

payload 尾部依次为 revision 8 字节、error 4 字节、incomplete 4 字节；去重排除了全部 16 字节，见[序列化](<src/index/index_host.cpp#L295-L310>)及[hash](<src/index/index_host.cpp#L448-L460>)。

**推断**

同样的 rows／total 从完整变为授权超时或不完整，或者恢复完整，均可能被吞掉。

**建议**

比较“结果语义”，仅排除纯通知 revision；error／incomplete 必须参与变化判断。

---

### 5.2 对象身份接口：路径只能定位，不能证明“还是原对象”

#### F02 — P0：跨卷移动删除新的同名源对象

**事实**

`TransferPlanEntry` 没有保留实际复制源的身份／租约，见[计划](<src/ops/ops_manager.h#L88-L99>)；复制回调保留的是目的句柄，见[CopyProgress](<src/ops/ops_manager.cpp#L421-L437>)。

同卷 `MoveFileExW` 遇到 `ERROR_NOT_SAME_DEVICE` 后进入复制；复制、可选哈希及目的提交完成后，对 `entry.source` 按路径读取属性、清只读、`DeleteFileW`，见[跨卷分支](<src/ops/ops_manager.cpp#L2563-L2577>)及[最终删除](<src/ops/ops_manager.cpp#L2763-L2771>)。

**调用链**

`Submit → ExecuteFileOperation → RunTransfer → CopyFile2 → destination commit → DeleteFileW(source path)`。

**触发与推断**

复制关闭源之后、删除之前，另一主体把原源移走并放入新文件。目的包含旧源，删除却命中新源，构成条件性永久数据损失。路径 SHA-256 校验不能消除校验后的替换窗口。

**建议**

从实际复制源捕获对象身份，并保持足以完成安全删除的 lease／handle；破坏性动作必须证明仍是该对象。可复用现有[文件身份适配](<src/ops/file_identity.h#L8-L41>)的 extended／legacy 区分，不能把“不支持 FileIdInfo”简化成“信任路径”。

**正向证据**

目的侧已经有 adopt、owned leaf、检查和固定句柄提交机制，见[目的提交保护](<src/ops/ops_manager.cpp#L2707-L2761>)。问题不是“复制完全没有 TOCTOU 防护”，而是防护未贯穿源删除。

#### F03 — P0：撤销没有证明删除的是原操作产物

**事实**

Copy／Create 的 `UndoEntry` 保存路径而非 produced 对象身份；`PushUndo` 不记录产物 ID；撤销把目的路径转成 `RecycleDelete`。见[结构](<src/ops/ops_manager.h#L101-L110>)、[入栈](<src/ops/ops_manager.cpp#L1430-L1441>)、[撤销](<src/ops/ops_manager.cpp#L1524-L1546>)。

删除准备按当前路径分类；网络路径变为 Permanent，见[删除计划](<src/ops/delete_operation.cpp#L86-L100>)。当前删除身份验证不能回溯证明它是原产物；同路径比较更不是历史对象证明，见[身份比较](<src/shell_host/deletion_identity.cpp#L23-L33>)。

**推断**

原产物被删除或移走后，新对象占用原路径，Undo 可回收新对象。网络路径经新的永久删除确认后，仍可能删除错误的新对象。不能把它表述为“绕过所有确认”。

**建议**

提交时记录每个产物的卷／FileID；撤销计划及后端执行前验证原产物身份。持久化后无法证明的记录应停止或标记不可撤销。`undo_revision` 仅证明栈版本，不证明盘面身份。

---

### 5.3 Shell 命令接口：会话、handler 和命令身份必须绑定

#### F04 — P0：事件失败后仍启动线程并释放 worker

**事实**

先创建两个事件，再 `CreateThread(worker.get())`，最后检查三个句柄；事件失败、线程成功时 `continue` 释放 `unique_ptr`。线程仍读写该裸指针。见[创建顺序](<src/shell_host/main.cpp#L1254-L1269>)、[线程体](<src/shell_host/main.cpp#L1115-L1153>)。

**推断**

低资源或失败注入下出现 UAF／堆破坏，同时存在该失败分支的句柄清理问题。正常路径的 STA 设计不能抵消此失败路径。

**建议**

事件全部成功后才启动线程；线程启动后，所有退出／失败清理都必须先确保线程停止或由独立所有者接管。

#### F05 — P0：缓存与重绑不能可靠表示用户所选命令

**事实**

1. 应用缓存按扩展名／background 等条件复用；同路径复用未完整考虑 extended 模式，见[缓存条件](<src/app/context_menu_controller.cpp#L152-L170>)。
2. cached click 用 verb／text 找 live ID；匹配不到但 ready 时仍可退回旧数字 ID，见[点击路径](<src/app/context_menu_controller.cpp#L238-L253>)。
3. 应用按首个 verb、再首个 display text 匹配，不含 handler 身份，见[匹配](<src/app/context_menu_controller.cpp#L466-L477>)。
4. host 优先接受同数字 ID，未验证它仍代表相同命令，见[FindItemId](<src/shell_host/ctx_handlers.cpp#L460-L474>)。
5. host 计算 `live` 并据其选择 handler，却没有把 `live` 写回 invocation；worker 调用的仍是原 `item_id`，见[分派](<src/shell_host/main.cpp#L1343-L1358>)、[实际调用](<src/shell_host/main.cpp#L1152-L1153>)。
6. 协议只有 session／item／verb／text，没有 handler 与命令代际身份，见[协议](<src/ipc/protocol.h#L282-L296>)。

**可构造分支**

旧 ID 258 失效、verb 重绑得到 live ID 2 时，dispatcher 可以选择第一 handler，却仍向它发送 258。实际 offset 与用户所选不一致；第三方 handler 是报错还是执行其他动作，待运行确认。

**推断**

动态菜单、跨 session cache、数字 ID 复用和同 verb／caption 的多个 handler 都可导致失败或误执行。破坏性命令误执行是风险后果，未宣称已发生。

**建议**

可执行身份至少绑定会话代际、handler 身份和 canonical command；展示缓存只负责布局。身份不匹配时停止，不能用“文字差不多”或旧数字 ID 授权。发送的必须是验证后的 live ID，并验证范围与 enabled 状态。

**相关 P1 问题**

- 按展示文字去重及生成 catalog key，会把相同 caption 的不同命令合并，也使偏好受语言变化影响，见[去重](<src/app/context_menu_controller.cpp#L29-L65>)、[catalog key](<src/ipc/ctx_menu_util.h#L219-L239>)。
- 静态 verb 执行静默限制为前 16 个目标，见[目标截断](<src/app/context_menu_controller.cpp#L270-L287>)。这是用户选择范围契约，不应只是内部 cap。

**排除误报**

[Fluent menu 的 DisplayEqual](<src/ui/fluent_menu.cpp#L81-L115>)用于决定显示重绘，随后更新 command；不能把纯显示相等直接判为命令身份漏洞。[内置菜单映射](<src/app/builtin_menu_items.cpp#L9-L23>)则正确使用枚举／数值身份。

---

### 5.4 数据边界与错误协议

#### F06 — P0：本地索引端点信任与数组分配边界

**事实**

本地客户端直接连接固定 pipe，未核验 server PID、image、token；响应提供的计数直接用于 `reserve`，工作线程未捕获这些解析／分配异常。见[连接](<src/index/index_client.cpp#L98-L138>)、[数组分配](<src/index/index_client.cpp#L195-L259>)、[worker](<src/index/index_client.cpp#L335-L373>)。

[协议的 payload 字节上限](<src/index/index_protocol.h#L8-L13>)和[ReadMsg 检查](<src/index/index_client.cpp#L158-L170>)不能约束“小帧里的巨大 count”。

**触发与推断**

真 host 未占用端点时，允许连接的伪服务端可返回伪造元数据；恶意 count 可要求巨大分配并触发 UI 进程未捕获异常。此处是本地端点占用／响应攻击，不是已证明的提权。

**建议**

复用并统一端点认证，明确区分 SYSTEM 服务与用户 helper；分配前检查 count、剩余字节、最小记录长度和业务页上限。[网络 transport 的验证函数](<src/index/index_transport_security.h#L126-L138>)已提供可参考的实际实现。

**边界澄清**

真实服务已有 per-message caller 捕获和可见性检查，见[host 授权](<src/index/index_host.cpp#L426-L439>)、[可见性检查](<src/index/index_caller_visibility.h#L34-L115>)。F06 不等于“索引服务任意泄露所有文件”。

#### F18 — P1，运行行为待确认：剪贴板 HDROP 地址／句柄不一致

[剪贴板读取](<src/ops/clipboard.cpp#L121-L127>)将 `GlobalLock` 返回地址强转为 HDROP；[拖放读取](<src/ui/drag_drop.cpp#L150-L155>)则传 `medium.hGlobal`。

公开的 [DragQueryFileW 契约](<https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragqueryfilew>)和 [Shell clipboard formats](<https://learn.microsoft.com/en-us/windows/win32/shell/clipboard>)不足以给该地址强转提供可移植保证。建议统一按 HDROP／HGLOBAL 的约定使用。

本次**不声称剪贴板普遍失效**：具体兼容行为依 Windows 实现而定；仓库已有[20 次 copy／cut roundtrip 测试](<src/bench/ops_test_main.cpp#L1320-L1339>)，本次未运行。

#### F19 — P1：密码状态依赖英文人类诊断

`PasswordError` 精确匹配包含路径的英文错误行；公开结果没有结构化 status／exit code，调用方依赖 `error.empty()` 与 `password_required`。见[诊断匹配](<src/ops/archive.cpp#L160-L169>)、[公开返回](<src/ops/archive.cpp#L194-L197>)、[结果类型](<src/ops/archive.h#L17-L21>)、[UI 消费](<src/app/archive_navigation.cpp#L225-L252>)。

版本、语言或路径显示形式变化可能使密码需求被当成普通失败。

建议返回结构化 outcome、exit code、密码状态（允许 Unknown）和独立 display message。**文档化 `7z -slt` 字段属于机器协议，不是本问题。**

同时，[Run](<src/ops/archive.cpp#L124-L130>)已对非零退出且空输出补错误文本，不能声称此类错误被普遍判成功。

#### F20 — P1：持久化 parser 是字符串搜索，不是 JSON 结构解析

[JSON helper](<src/common/json_utils.h#L27-L84>)具有以下事实：

- 按 key 子串寻找冒号，不确认对象层级；
- 不支持完整 JSON escape，`\u` 不按 Unicode 解释；
- 整数逐位累乘不检查溢出，存在有符号整数溢出 UB；
- bool 接受前缀而不确认 token 结束。

[会话读取](<src/app/session.cpp#L356-L370>)实际使用该 helper。

建议明确持久化 schema 和完整边界；采用真实 parser 或严格限定并验证其输入子集。这里没有证明远程输入可达，不能包装成远程漏洞。

#### F23 — P1：预览属性长度协议两端不一致

host 的[属性构造](<src/preview_host/main.cpp#L194-L205>)和[WriteString](<src/preview_host/main.cpp#L260-L263>)未限制 value 长度；UI [reader](<src/ui/thumbnail_cache.cpp#L442-L456>)只允许 1024 字符。

合法属性值达到 1025 字符即可被 reader 判为协议失败，随后 helper 停止且失败被缓存。测试 reader 的允许长度不同，见[测试解析器](<src/bench/preview_test_main.cpp#L203-L207>)。

建议将 label／value／count 上限集中为协议常量；超长显示属性应明确截断或返回受控错误，不能由两端不同的隐式规则决定。

#### F24 — P1：文本截断与编码边界未表达完整

[DecodeText](<src/preview_host/main.cpp#L123-L140>)最多保留 12000 字符；[MakeTextOrHex](<src/preview_host/main.cpp#L167-L188>)却仅按“文件字节是否读完”设置 truncated。

因此，20000 字节 ASCII 文件可以显示 12000 字符，却报告未截断。32 KiB prefix 恰好切在 UTF-8 多字节字符内部时，又可能使整个 prefix 回退 CP_ACP。

建议区分 byte／character／display 截断，安全处理 UTF-8 边界；不能让有限预览暗示内容完整。

#### F30 — P1：路径规范化未先解析所有相对语义

[NormalizePath](<src/fs/fs_enum.cpp#L145-L168>)只对没有 drive designator 的路径调用 `GetFullPathNameW`。`C:subdir` 会直接得到扩展前缀，`C:\a\..\b` 也未先解析 dot segment。

用户地址输入确实会进入该函数，见[地址提交](<src/app/app_hosted_edit.cpp#L189-L196>)和[导航](<src/app/app_navigation.cpp#L1537-L1545>)。

依据 [Windows 路径与命名空间规则](<https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file>)，drive-relative 与扩展路径具有不同语义。建议先解析有效 Windows 输入，再添加扩展前缀；不要把“以盘符开头”当成“已完整规范化”。

---

### 5.5 索引完整性接口

#### F07 — P0：混合 USN reason 丢失

**事实**：[ApplyUsnLocked](<src/index/index_engine.cpp#L2584-L2602>)遇到 `SECURITY_CHANGE` 提前返回；[CatchUpVolume](<src/index/index_engine.cpp#L2888-L2915>)仍推进并持久化 cursor。

**调用链**：`USN stream → packet queue → engine worker → CatchUpVolume → ApplyUsnLocked → cursor commit`。

**推断**：同记录内同时出现 SECURITY 与 RENAME／DELETE／CREATE／DATA 时，其他变化漏应用，索引及其派生结果可持续错误。

**建议**：权限刷新与其他 reason 的处理应叠加；只有明确消费了所有有效位，才可提交该记录进度。

#### F08 — P0：MFT 部分读取被宣称成功

**事实**：[EnumerateMft](<src/index/index_mft.cpp#L303-L340>)在后续 ReadAt 失败时跳出当前 run，最终只要读到记录数非零就返回 true；[IndexVolumeMft](<src/index/index_engine.cpp#L2941-L2955>)只有 false 才进入 fallback。

**调用链**：`FullRebuild → IndexVolumeMft → EnumerateMft → BuildMftTree → snapshot commit`。

**推断**：中段失败／短读后，缺失基线可被发布。未发生后续 USN 的既存文件不会自动恢复。

**建议**：区分 Complete／Failed／Cancelled／Capped；部分读取不能代表完整成功。失败时 fallback，或保留旧快照并显式标 gap。

---

## 6. 多实现与多风格分析

| 观察到的多路径 | 判断 | 应如何处理 |
|---|---|---|
| NT 目录查询与 Win32 枚举 | **合理平台分支** | 保留能力检测与兼容 fallback；统一错误／完整性／取消契约，而非强制一个底层 API |
| 普通复制、同卷移动、raw reparse | **不同文件语义** | 不应为了去重删除 reparse 处理；但源对象身份应贯穿各破坏性分支 |
| 永久删除与显式回收 | **不同语义的必要后端** | 保留同一个授权门；不可把“回收失败就永久删除”当透明 fallback |
| 本地索引与 UNC 索引 | **不同权限与凭据域** | 保留隔离；统一上层结果终态，不能搬到一个高权限万能服务 |
| one-shot／缓存内容查询／校验＋补扫 | **不同执行职责** | 不是三个重复搜索引擎；明确 freshness／coverage 契约后优化访问形状 |
| OPC／Office IFilter／PDFium／PDF IFilter | **格式和环境差异** | 保留格式适配及解析进程；不将复杂解析器挪入 UI |
| WIC／SVG／metafile／Shell thumbnail／handler／MFPlay | **媒体与 provider 能力不同** | 不建立无差别 fallback 链；每个降级应说明丢失的语义 |
| DirectWrite 与 LumaText | **可选 backend** | 保留选择；统一绘制输入的缓存身份，修正 F25 |
| Shell IDataObject 与最小 FileDataObject | **互操作增强＋兼容降级** | 有依据，不是重复剪贴板实现 |
| FolderSizes 递归与详情独立递归 | **真实职责重叠** | 收敛一个可取消扫描服务，按消费者投影结果 |
| 剪贴板与拖放中的 HDROP 读取 | **风格分歧导致契约风险** | 统一解析约定；F18 需定向运行验证 |
| 主窗和全局窗口的搜索合并 | **前端不同，但完成策略重复且分歧** | 两个 UI 可保留；共享结果终态／合并语义，避免一个吞错、一个依赖 timer |
| 本地 rename 与 Shell IPC rename | **当前扫描调用点为 0 的候选旧路径** | 先确认所有入口、协议兼容及外部调用，再决定删除；不能仅凭一次 grep 判死代码 |
| app／model 同名头文件 | **兼容别名，不是第二实现** | [places alias](<src/app/places.h#L1-L31>)、[batch rename alias](<src/app/batch_rename.h#L1-L13>)不能计为重复业务逻辑 |

补充事实：[标签实现](<src/app/places.cpp#L15>)使用 model namespace，但物理位置仍在应用目录。这反映迁移未完全完成，不等于构建引用损坏。

---

## 7. 数据流与性能审计表

下表证明的是访问／调度形状。除明确的算法复杂度外，不给出未经测量的耗时、CPU 或吞吐数字。

| 路径 | 经过的层 | 重复数据／转换／I/O | 影响 | 简化方向 | 证据 |
|---|---|---|---|---|---|
| **F11：目录→排序→共享缓存** | pane→worker→枚举→排序→path store→其他 pane | 不同排序分别枚举；sorted snapshot 存入 path-only cache | 一个 pane 可接收另一排序结果；排序成本与枚举成本耦合 | 共享未排序 FS 事实；排序为 per-pane 投影 | [WorkResult](<src/app/app_worker.h#L34-L45>)、[缓存更新与接收](<src/app/app_navigation.cpp#L984-L1040>) |
| **F11：列式结果** | worker result→column | 成功后把 generation 清零，未保留 applied watermark | 更旧同路径成功结果可覆盖新结果 | request generation 与 applied generation 分开 | [列式结果](<src/app/app_column_view.cpp#L289-L306>) |
| 排序命令 | UI→Refresh→枚举→sort | 仅改变显示顺序仍重读文件系统 | 本地重复 I/O；UNC 放大延迟 | 已有事实快照上做排序投影 | [SetSort](<src/app/app_navigation.cpp#L1658-L1685>) |
| **F12：过时 UNC 导航** | 队列→2–4 worker→NT／Win32 provider | generation 在完整枚举后才检查；旧不同 key 工作未被物理取消 | 全部 worker 被旧 UNC 阻塞时，新本地导航及 EnqueueIo 饥饿 | 消费者取消、按介质隔离预算；在途旧任务有实际治理 | [枚举后检查](<src/app/app_worker.cpp#L228-L247>)、[无限 pending wait](<src/fs/fs_enum.cpp#L330-L350>) |
| **F13：通知失败重试** | watch→ReadDirectoryChanges→reopen→watch | provider 不支持通知但目录可成功重开时，无 backoff 循环 | CPU 热重试、overflow 与重枚举风暴的条件性风险 | Unsupported／Offline 状态、退避、受控 polling | [失败分支](<src/fs/fs_watch.cpp#L209-L260>) |
| **F14：UNC 探测** | UI scheduler→inner thread＋waiter→timeout→下一探测 | 1500ms 仅释放逻辑占位；waiter 继续无限等 inner；每次两线程 | 多 pinned roots／重复导航可积累 blocked worker、waiter 与 handle | 统计 live＋retired 的物理上限；拒绝／合并超过预算的请求 | [探测](<src/fs/fs_net_cache.cpp#L171-L217>)、[调度](<src/app/app_navigation.cpp#L79-L98>) |
| **F15：通知补丁** | UI timer→Drain→各 distinct snapshot→stat→publish | 每种现有快照复制整列表，并为事件同步 stat | 大目录复制成本；网络 stat 可阻塞 UI | 后台收集一次 FS delta，UI 应用投影；保留现有批次合并 | [timer](<src/app/app_main.cpp#L934-L943>)、[逐快照补丁](<src/app/app_navigation.cpp#L1269-L1301>) |
| **F15：暂存区收集** | copy／cut／OLE drop→Collect→属性查询 | UI／drop 回调同步逐路径 stat | UNC 或不可响应 provider 可卡交互 | 收集意图与验证分离，异步填充状态 | [Collect](<src/app/app_model.cpp#L886-L917>)、[copy/cut](<src/app/app_ops_ui.cpp#L185-L200>)、[drop](<src/app/app_input.cpp#L321-L327>) |
| **F16：目录大小** | BuildVm→独立 details scan；FolderSizes→另一次 scan | 两条递归入口；详情 scan 遇枚举失败跳过，仍 done | 重复 I/O；不完整大小被当完成；关闭可能等待 provider | 一个扫描服务，typed partial／error；ViewModel 只读 | [详情 scan／join](<src/app/app_runtime.cpp#L959-L1042>)、[FolderSizes](<src/app/folder_sizes.cpp#L383-L417>) |
| **F17：详情元数据** | BuildVm→path-key worker→窗口消息 | 同一路径变化不重新请求；权限／owner／volume 多次查询 | 元数据可长期过时；权限摘要不能代表完整 token access | path＋对象版本＋view/window epoch；集中元数据读取 | [请求条件](<src/app/app_runtime.cpp#L1856-L1872>)、[接受条件](<src/app/app_main.cpp#L1453-L1471>)、[权限摘要](<src/app/details_meta.cpp#L41-L125>) |
| **F21：内容缓存查询** | SearchTask→SQL candidate→C++ scope／size→body／version | 广域 count／sort；逐候选 body、逐 positive version；全 documents negative scan | 小 root 常见词仍承担大 cache 成本 | 下推廉价谓词，有限批量读取，保持同 WAL snapshot | [候选 SQL](<src/index/content_index.cpp#L1275-L1301>)、[后过滤](<src/index/content_index.cpp#L1369-L1387>)、[后续查询](<src/index/content_index.cpp#L1523-L1593>) |
| 菜单等待与刷新 | UI wait→窄消息泵→完成→多次 refresh | 等待 QS_ALLINPUT，但仅泵 Shell 消息；再 immediate／400／1600ms 刷新 | 延迟时未消费输入可反复唤醒；重复枚举是补偿机制 | 非阻塞菜单状态机；基于可靠完成／通知刷新 | [等待](<src/app/context_menu_controller.cpp#L204-L216>)、[窄泵](<src/app/app_commands.cpp#L939-L949>)、[刷新时点](<src/app/context_menu_controller.cpp#L497-L510>) |
| **F22：预览失败缓存** | Draw→helper→1s deadline→StoreResult | temporary timeout 被作为稳定失败 key 缓存；generation 不在 key | 同身份直到 LRU／Reset 才重试 | 错误分类、短 TTL、重试预算；稳定 unsupported 可长缓存 | [key](<src/ui/thumbnail_cache.cpp#L53-L57>)、[timeout](<src/ui/thumbnail_cache.cpp#L408-L416>)、[失败存储](<src/ui/thumbnail_cache.cpp#L487-L491>) |
| **F26：视频退休资源** | Open→detached thread→MFPlay→Reset hide | 每次 Open 新线程；blocked create／open／Shutdown 未被循环 deadline 控制 | 快速切换可积累 hidden HWND 与退休线程 | 真实退休上限；隔离或串行 admission，不以 stop flag 代替结束 | [Open／Reset](<src/ui/video_preview.cpp#L142-L178>)、[同步 provider 调用](<src/ui/video_preview.cpp#L250-L256>)、[Shutdown](<src/ui/video_preview.cpp#L398-L403>) |
| **F27：GIF** | frame request→新 decoder→从 0 合成到目标帧→缩小 | 每请求重做前缀；UI 只留少量帧 | 播放一轮可呈 O(N²) 前缀工作；大原始 canvas 增加峰值内存 | 连续解码状态／checkpoint＋有界帧缓存；约束源 canvas | [合成](<src/preview_host/main.cpp#L362-L453>)、[帧缓存](<src/ui/thumbnail_cache.cpp#L347-L360>) |
| **F28：等价预览请求** | details／quick→raw pixel key→helper clamp | details 请求 2048，helper 最终 ≤1024；quick bucket 已 ≤1024；GIF cap 512 | 同样输出重复解码／缓存；文本 pixel 维度也无意义 | 在 key 前规范化有效解码身份 | [details](<src/ui/ui_details_panel.cpp#L210-L219>)、[quick](<src/ui/quick_preview_window.cpp#L463-L465>)、[clamp](<src/ipc/preview_protocol.h#L20-L37>) |
| 排序比较器 | `std::sort`→EntryLess→path／localized name | O(N log N) 比较过程中构造派生 key | 字符串／锁成本形状存在，实际收益待测量 | 每轮排序预计算稳定 key | [比较器](<src/app/entry_sort.cpp#L39-L108>) |
| **F29：UNC 磁盘缓存写入** | 多 worker→SaveNetSnapshot→同 `.tmp` | 同路径临时名，无 per-path writer／generation 协调；结果被忽略 | 写失败、旧结果覆盖风险；未证明实际混合损坏 | 独立临时名＋序列化提交＋版本检查 | [保存](<src/fs/fs_net_cache.cpp#L55-L83>)、[调用](<src/app/app_worker.cpp#L348-L357>) |

### 7.1 F11 的确定性排序身份冲突

`WorkResult` 不携带 sort specification；store 按 path 存储；tab 在 pending 为 0 时可接收较新的同路径结果，见[结果结构](<src/app/app_worker.h#L34-L45>)、[store 接口](<src/fs/fs_snapshot.h#L39-L56>)、[接收条件](<src/app/app_navigation.cpp#L984-L1040>)。

可构造顺序：

1. pane A 的 Name 结果 gen1 已应用，pending 清零；
2. pane B 的 Size 结果 gen2 返回；
3. A 也通过同路径、较新 generation 检查，接受 B 的 Size snapshot；
4. A 仍显示 Name 排序状态。

[PrepareEntrySnapshot](<src/app/app_model.cpp#L143-L161>)不会无条件按 A 的 sort 重新投影，且一次刷新后清 keep flag。因此，这不是仅“缓存效率问题”，而是事实快照与视图身份混合造成的列表正确性风险。

`WorkKey` 还未包含 folder-sort mode，见[key 与 Refresh](<src/app/app_worker.cpp#L108-L128>)；请求语义并未完整入 key。

### 7.2 F15 的失败语义：stat 失败不等于对象不存在

UI 实际调用的是 keep-order notify patch 路径。[ApplyHeldNotifies](<src/app/snapshot_patch.cpp#L157-L192>)对非预验证 rename 项的 `FillDirEntry` 失败直接删除旧行，没有区分暂时访问失败与不存在。

**推断**：modified／added 事件期间短暂 access denied 或 provider 故障，会把仍存在的对象从视图移除。rename 预验证失败另有 full-enum 行为，不能把所有 rename 都描述成静默删除。

**建议**：Absent、Unavailable、Changed 必须区分；暂时无法 stat 的行保留或标 stale，并安排受控验证。

### 7.3 F12／F13／F14 的必要区分

- [WorkerPool::Stop](<src/app/app_worker.cpp#L82-L105>)已有 `CancelSynchronousIo`、100ms bounded wait 和 shared state retirement，且有全局活跃上限。**本报告不声称 WorkerPool Stop 无界或存在通用 UAF。**
- watch 也有 owner／overlapped／回调门控和 live＋retired 约束。F13 是**失败重试策略**，不是否定其内存生命周期设计。
- 每秒[未 armed UNC 刷新](<src/app/app_navigation.cpp#L1504-L1509>)与 F14 的探测调度是两件事，不能声称探测自动每秒启动一次。
- Windows 文档明确存在网络 redirector／文件系统不支持通知的情形，见[ReadDirectoryChangesW](<https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw>)。成功 reopen 不代表通知能力恢复。

### 7.4 F16／F17 的结果真实性与关闭风险

- 详情递归遇 `FindFirstFile` 失败跳过该目录，最后仍标 done，见[详情扫描](<src/app/app_runtime.cpp#L959-L1031>)。大小结果需要 incomplete／error，不应只给完成 bool。
- [WM_DESTROY](<src/app/app_main.cpp#L1668-L1694>)仍在 UI 关闭路径停止并等待若干组件；只有部分 stop 被移到[退出 worker](<src/app/app_exit.cpp#L30-L39>)。不响应的 provider 可使目录大小等 join 卡住关闭，运行表现待确认。
- Details worker 未初始化 COM，却调用 Shell 元数据函数，见[worker](<src/app/app_runtime.cpp#L62-L115>)；相关 provider 的表现待确认。
- [权限摘要](<src/app/details_meta.cpp#L41-L107>)主要依据用户 SID／有效权限，不是完整访问 token 判定；读＋写标作 Modify 也不等同包含 delete 的完整修改权限。该信息应标明“摘要”，不能作为实际授权依据。

---

## 8. 架构与耦合

### F31 — P1：文件级分离已经推进，语义边界尚未完成

| 现象 | 事实 | 影响与建议 |
|---|---|---|
| `AppState` 是全应用能力入口 | 同时持有 renderer、quick preview、tabs、raw pane 指针、store、worker、watch、队列、侧栏等，见[聚合状态](<src/app/app_state.h#L132-L212>) | 聚合所有权本身合理，但大量命令接受整个对象，无法看出所需能力和允许修改范围；按具体用例缩窄接口 |
| model 依赖渲染／搜索实现类型 | [应用模型头](<src/app/app_model.h#L3-L11>)包含 renderer／content result store | 模型难以独立验证，搜索与浏览状态交织；提取稳定领域值，不再让模型依赖大型渲染头 |
| UI ViewModel 仍握领域服务对象 | 含 FS snapshot、可变 content store、mutable row cache、catalog 指针，见[PaneViewModel](<src/ui/ui_renderer.h#L174-L187>) | include 边界通过不等于 UI 只读边界；可通过这些对象读取／依赖领域状态 |
| `BuildVm` 不纯 | 会触发详情元数据与独立目录大小扫描，见[详情请求](<src/app/app_runtime.cpp#L1856-L1872>)、[扫描启动](<src/app/app_runtime.cpp#L1913-L1931>) | 绘制／布局调用频率影响业务调度；应由选择／导航事件启动任务，VM 读取已发布状态 |
| aggregate header 放大修改面 | [app_internal](<src/app/app_internal.h#L1-L11>)汇总状态、运行、导航、操作、命令、输入等 | 多 TU 文件拆分但能力依然相互可见；以显式依赖替代“任何处理器都能触达所有状态” |
| Places model 仍混合 I/O | [model 接口](<src/model/places.h#L21-L120>)与[实现](<src/app/places.cpp#L884-L1122>)覆盖目录／ADS／持久化职责 | stable TagId 是正向设计；但目录位置迁移不应被当成已完成纯模型化 |

**不建议**立即引入完整事件总线、DI 容器、通用 repository／service 框架。优先划清四种真实边界：

1. 导航／选择意图；
2. 文件系统事实；
3. 异步请求与结果；
4. UI 只读投影。

COM、进程隔离、授权、凭据这些原生边界应保留，不宜用一套抽象擦平。

---

## 9. 过度设计、冗余、fallback 与文件碎片化

### 9.1 真实可简化项

- **目录大小重复扫描**：F16 是相同业务事实被两套调度拥有，值得收敛；索引查询与递归扫描本身则是有意义的两种数据来源。
- **路径级缓存承载排序**：增加了请求 key、keep-order、代际和多 pane 修补的复杂度，却仍没表达完整身份，见[共享加载](<src/app/app_navigation.cpp#L909-L954>)和[多快照通知补丁](<src/app/app_navigation.cpp#L1269-L1301>)。
- **单槽完成包装**：F01 用少量字段隐藏 request kind／id 的复杂度，属于表面简单、真实契约更复杂。
- **补偿性刷新**：菜单操作后的 immediate／400／1600ms 刷新是经验补偿，不是可靠完成协议，见[刷新调度](<src/app/context_menu_controller.cpp#L497-L510>)。
- **候选旧 rename 接口**：[ShellClient::Rename](<src/ipc/shell_client.h#L50>)和 host 请求分支仍在，但本次扫描未找到生产调用。可以列为清理候选，不能未经全入口确认直接删除。

### 9.2 F25 — P1：LumaText surface cache 缺少 alignment

[SurfaceKey](<src/ui/lumatext_renderer.cpp#L95-L113>)及[构造](<src/ui/lumatext_renderer.cpp#L616-L625>)不包含 alignment；录制的 origin 却依 alignment 改变，见[绘制 origin](<src/ui/lumatext_renderer.cpp#L656-L662>)。

因此，同样 text／layout／size／颜色先 leading、后 center／trailing 时，可以直接重放错误位置的 command list。该缺陷由 key 与录制内容不一致直接推出，不依赖性能测量。

建议令缓存键代表实际录制语义，或者让 alignment 作为重放时变换且不被提前烘焙。不是再加一层缓存即可解决。

### 9.3 F22／F28／F29：缓存多并不等于身份完整

- F22 的 temporary failure 没有时间有效性；[Evict consumer](<src/ui/thumbnail_cache.cpp#L91-L98>)并不等同清除负缓存。
- F28 使用 raw request pixels，而 helper 使用有效 cap；多个 key 代表相同解码结果。
- F29 的同 `.tmp` 多 writer 缺少提交版本；应保守归为并发缓存风险，**没有证据证明已经发生灾难性文件损坏**。
- [UNC 缓存序列化](<src/fs/fs_net_cache.cpp#L55-L79>)与[加载](<src/fs/fs_net_cache.cpp#L120-L130>)未完整保存部分 cloud／link 派生字段。缓存优先数据与实时结果是否等价应显式声明，不能默认为相同完整度。

### 9.4 不应误删的 fallback

以下都有平台或安全依据：

- NT→Win32 目录枚举；
- Shell data object→CF_HDROP 降级；
- 每-handler 菜单绑定→aggregate 菜单 fallback；
- WIC／metafile／Shell thumbnail 能力差异；
- PDFium／IFilter 和 OPC／legacy Office 分支；
- 索引查询→递归扫描；
- 普通文件、reparse、同卷 rename／跨卷 copy 的不同实现；
- 解析器／Shell provider 的进程隔离。

应审计的是**fallback 是否改变语义、错误是否被保存、身份是否连续**，不是把多分支数量当成缺陷。

### 9.5 P3：构建与文件组织

[CMake 生产源码收集](<CMakeLists.txt#L295-L317>)采用递归 glob 加 regex 排除。随着测试／迁移文件增多，目标归属依赖命名和排除规则，维护风险高于显式模块目标；本次未证明有错误源文件被纳入生产目标。

兼容头是迁移手段，不是重复实现。反过来，把大处理器分成许多仍共享 `AppState` 的文件，也不能单凭文件变小宣称解耦。应以依赖和状态所有权衡量，而非“文件越多越好”或“全部合回一个文件”。

---

## 10. AI 代码反模式分类

本节分类的是**可观察代码模式**，不推断作者是否使用 AI，也不推断生成过程或动机。

| 类别 | 本仓库可观察证据 | 判断 |
|---|---|---|
| 展示文本充当机器身份 | F05 的 text rebind／catalog key；F19 的英文 PasswordError | 已确认，需要 typed identity／outcome |
| 路径字符串充当对象身份 | F02 源删除；F03 produced undo | 已确认，破坏性动作风险高 |
| 先发请求后补登记 | F01 Query／Invoke | 已确认；快速回调不是罕见“边角情况” |
| 全局单槽模拟请求协议 | F01 DONE 字段 | 已确认；隐藏并发类别而非真正串行化 |
| bool／空字符串压扁结果状态 | F08 partial MFT、F16 size done、F19 error.empty | 已确认；Complete／Partial／Failed 被混合 |
| 逻辑取消冒充物理结束 | F12 stale enum、F14 probe、F26 video | 已确认生命周期形状；实际资源积累需测量 |
| 缓存键未覆盖真实语义 | F11 sort、F25 alignment、F28 effective pixels | 已确认；缓存“命中”不代表结果可复用 |
| 临时失败永久化 | F22 timeout negative cache | 已确认；需要 TTL／分类而非无限重试 |
| 静默限制输出却不表达损失 | F05 前 16 目标、F24 字符截断 | 已确认；不能只以“有 cap”证明契约可靠 |
| 自造简化 parser 替代标准边界 | F20 JSON helper | 已确认，结构及溢出问题可直接阅读验证 |
| 以业务后过滤代替数据层范围控制 | F21 candidate SQL／N+1 | 已确认访问形状；实际耗时待测量 |
| 用额外刷新／轮询补偿缺失协议 | 菜单多时点刷新、F13 立即 reopen retry | 部分有兼容需求，但不应替代终态与能力状态 |
| 文件拆分／namespace 迁移冒充解耦 | F31 aggregate state／领域对象进入 VM | 架构尚未完成；不把迁移别名算成多实现 |
| 无依据的“统一后端” | 本地／UNC、文档、preview 原生分支 | **不成立**；这些多路径多数有实际依据 |
| 编造 API／跨 apartment 裸传 COM | 本次已读重点路径 | **未发现可证实案例**；API 使用错误不等于 API 不存在 |
| 删测试／弱化断言换通过 | 当前相关 diff | **无证据成立**；不能从删除一个 setup assignment 推断作弊或 AI 动机 |

正向例子：[类型化 Banner 测试](<src/bench/presentation_state_cases.h#L73-L104>)明确验证语言变化不影响机器状态。[当前变化记录用例](<src/bench/presentation_state_cases.h#L182-L209>)仍保留机器元数据断言。审计应保留这些证据，而不是为了“敌意审查”凑反模式。

---

## 11. 第一性原理简化方向

| 基本事实 | 最小应有模型 | 可以消除的复杂度 |
|---|---|---|
| 一个目录的文件事实与用户排序无关 | 共享 immutable FS facts；每 pane 自己的 order／filter projection | 按 sort 反复枚举、path cache 的排序冲突、逐排序快照重复 patch |
| 路径是 locator，不是 object identity | locator＋对象身份＋必要 lease | Copy／Move／Undo／retry 中反复路径再验证仍留下的 TOCTOU |
| 一次请求只有一个最终结果 | request ID＋typed terminal result＋发送前路由 | DONE 单槽、早回调丢失、超时后永远未 ready |
| 完整性是结果的一部分 | error／partial／coverage 与 rows 一起比较和发布 | F08／F10 等“数据没变所以状态也没变”错误 |
| 超时仅代表消费者不再等待 | live＋retired 资源预算；真实取消或隔离退场 | F14／F26 的后台遗留积累 |
| 缓存复用依赖有效输出语义 | 规范化的 effective key＋版本／TTL | 像素 cap 等价请求、alignment 错复用、temporary failure 永久化 |
| 大小／属性是异步业务事实 | 一个可取消事实服务，多消费者只读订阅 | BuildVm 启动业务、两次递归扫描、路径不变就永久 stale |
| 小 scope 查询不应支付全库成本 | 先过滤廉价 metadata，再批量取必要 body | 广域 count／sort／negative scan 与 N+1 |

具体原则：

1. **保留少量显式边界，不增加万能框架。**
2. **先闭合契约，再优化缓存。** 缓存错误数据只会更快、更持久地交付错误。
3. **机器字段与 display message 分离。** 文字可以本地化、截断、去助记符；命令／状态身份不能因此变化。
4. **对不可证明的破坏性动作 fail-closed。** 不支持某种身份查询时，不应退回纯路径授权。
5. **保留原生权限和 COM 边界。** 用户 UNC 凭据、service caller visibility、STA provider 及解析进程不应为“统一”被擦平。
6. **访问形状先于微优化。** 去掉重复枚举／广域候选／前缀 GIF 合成，优先于优化单次字符串复制。
7. **错误必须可观测。** unsupported、offline、temporary、partial、cancelled 不能都映射成 false／空列表／离线文本。

---

## 12. 解耦与治理路线图（仅方向）

本路线图不包含补丁、直接重构或本次执行承诺。

| 阶段 | 目标 | 方向 | 验收边界 |
|---|---|---|---|
| **1：操作安全与完成协议** | F01–F05 | 发送前登记；per-request completion；完整锁内消费；源／产物身份；handler 创建清理；命令身份绑定 | 同步快回调、DONE 交错、源／产物替换、创建失败注入、fake handler offset |
| **2：索引可信与完整性** | F06–F10 | 本地 server 验证；count-before-allocation；混合 reason；MFT 完整状态；统一搜索终态与质量比较 | fake server、恶意 count、混合 USN、MFT 第二 chunk 失败、相同 rows 质量变化 |
| **3：目录事实与视图分离** | F11、F15 | 未排序 snapshot；独立 pane projection；request／applied watermark；后台 delta | 同路径不同排序、多 pane、列式旧结果、stat temporary failure |
| **4：物理工作预算与关闭** | F12–F17、F26 | 统计 live＋retired；UNC admission／backoff；统一大小服务；元数据版本；VM 只读 | 不响应 provider、快速导航／切换、退出、资源峰值与部分结果 |
| **5：预览及查询成本** | F21–F28 | 下推 SQL scope；batch body；协议限额；TTL；有效 cache key；GIF 增量／checkpoint | 多 root query statement／候选数、长属性、截断、alignment、长 GIF、重复消费者 |
| **6：结构清理** | F19、F20、F29–F31及 P3 | 结构化 adapter；持久化 parser；单 writer 提交；缩窄用例依赖；确认后移除旧接口 | 定向单元／集成验证；不以文件搬迁或 include pass 代替功能验证 |

优先从已有类型、身份 helper、worker 与协议扩展，不应一次性改成新的通用任务框架。每阶段只扩大到直接影响模块；后续实施时按[仓库验证规则](<AGENTS.md>)选择定向构建／测试，不默认全量重构与全历史回归。

---

## 13. 风险、测试与不确定性

### 13.1 已有测试不等于覆盖本报告触发条件

| 领域 | 已有证据 | 本次发现所需的缺口 |
|---|---|---|
| 文件操作 | [pause／cancel／目标替换测试](<src/bench/ops_test_main.cpp#L824-L882>)、[undo 持久化测试](<src/bench/ops_test_main.cpp#L1306-L1317>) | CopyFile2 返回后的源替换；Copy／Create 产物替换；同步 DONE／菜单 DONE 交错 |
| Shell handler | 相关实现与测试入口已阅读 | 两事件／线程分别创建失败；旧 ID→live ID 的实际 offset；同 caption／verb 多 handler |
| 本地索引安全 | [security test](<src/bench/index_security_test.cpp#L63-L77>) | 本地 fake server、巨大 count、截断数组、未捕获分配异常 |
| USN／MFT | [单 reason／队列用例](<src/bench/index_engine_test.cpp#L59-L87>)、[overflow 用例](<src/bench/index_engine_test.cpp#L152-L200>)、[delta partial write](<src/bench/index_engine_test.cpp#L759-L785>) | 混合 reason＋cursor 提交；原始 MFT 读失败／多 run 短读；不是 delta 写失败 |
| 订阅／网络结果 | [订阅 churn 测试](<src/bench/index_host_stress.cpp#L203-L228>)、[网络安全测试](<src/bench/network_security_test.cpp#L30-L57>) | 相同 rows 的 error／incomplete 双向变化；RSP_RESULT 错误到主窗的终态 |
| worker 生命周期 | [Stop gate 测试](<src/bench/worker_lifecycle_test.cpp#L8-L55>) | 该测试明确不调用真实 provider／FS；不能证明 navigation cancellation |
| 目录大小 | [本地 Stop 用例及 UNC skip](<src/bench/folder_sizes_scan_test.cpp#L73-L82>) | 不响应 UNC、access denied 的 partial state、UI 关闭 join |
| 内容查询 | [content index 测试](<src/bench/content_index_test.cpp#L998-L1017>) | 多 root 中 candidate 行数、SQL statement 数、body 读取量 |
| 预览 | [cache 测试](<src/bench/thumbnail_cache_test.cpp>)、[文本 preview 用例](<src/bench/preview_test_main.cpp#L382-L387>) | failure TTL、生产属性限额、字符截断、UTF-8 边界、alignment、退休视频预算 |
| 压缩包 | [当前诊断测试](<src/bench/archive_test.cpp#L86-L124>) | 替代诊断／版本／路径表示、取消、输出限额与密码状态独立性 |
| 展示状态 | [typed Banner 用例](<src/bench/presentation_state_cases.h#L73-L104>) | 这是正向证据；不能替代所有菜单／archive 文字契约的验证 |

“缺口”表示本次定向检查没有看到匹配回归，不是断言整个仓库绝对没有其他覆盖。

### 13.2 CI 与验证边界

[发布 CI](<.github/workflows/release.yml#L1-L36>)调用[发布验证脚本](<scripts/build_release_ci.ps1#L37-L101>)，声明了定向测试目标、退出码检查和 selftest。不能写“没有 CI”或“所有测试从未运行”。

本次没有触发该 CI，也没有运行其中的测试；因此：

- 不报告编译成功；
- 不报告 COM／Shell／UNC 功能通过；
- 不报告 UI、主题、DPI、窄布局的视觉通过；
- 不报告任何性能数字或安全利用成功；
- 不把静态 include check 当成以上验证。

### 13.3 仍需运行确认的事项

1. **剪贴板 HDROP**：具体 Windows 兼容行为，F18。
2. **阻塞 provider**：UNC、MFPlay、IFilter、Shell extension 的真实取消与退出表现；静态已证明等待／资源形状，但积累速度未测量。
3. **菜单后果**：重绑旧 ID 可证明分派错误，实际 handler 的误执行行为依第三方实现。
4. **本地 fake pipe**：需要真 host 未占端点、攻击者可创建该端点等条件；不是已证明提权。
5. **高权限 helper 的可见性／MIC**：部分路径依 service 模式 gate；跨完整性连接可达性待确认，不作为本报告已证实攻击结论。
6. **UNC `.tmp` 并发写入**：未证明已发生混合内容损坏，保守报告为提交竞态与旧数据／失败风险。
7. **排序本地化**：缓存有稳定化及同步机制，本次没有依据宣称通用 localization 数据竞争。
8. **COM 消息泵**：部分 STA worker 的兼容性取决于所用对象；preview handler worker 已有消息泵，不能一概称所有后台 STA 无泵。

### 13.4 安全隔离的准确表述

文档 helper 使用继承 handle 白名单、kill-on-close job、进程／内存／超时及并发限制，见[文档隔离](<src/index/document_reader.cpp#L297-L343>)。它是**同用户权限下的故障与资源隔离**，不是 AppContainer／低权限操作系统安全沙箱。报告不能夸大其权限约束，也不能因其不是安全沙箱就否定进程隔离价值。

---

## 14. 证据索引

| 索引 | 对应结论 | 关键文件／函数位置 |
|---|---|---|
| **E01：构建与入口** | 范围、技术栈、进程结构 | [构建基础](<CMakeLists.txt#L1-L79>)；[主程序／Index 目标](<CMakeLists.txt#L295-L390>)；[文档目标](<CMakeLists.txt#L775-L807>)；[wWinMain／OleInitialize](<src/app/app_main.cpp#L1981-L1992>) |
| **E02：操作完成协议** | F01 | [Ops DONE callback](<src/ops/ops_manager.cpp#L1782-L1817>)；[MenuThread](<src/ops/ops_manager.cpp#L1958-L1995>)；[WaitShellDone](<src/ops/ops_manager.cpp#L3373-L3405>)；[ShellClient::Submit](<src/ipc/shell_client.cpp#L165-L195>) |
| **E03：源与产物身份** | F02、F03 | [TransferPlan／UndoEntry](<src/ops/ops_manager.h#L88-L110>)；[CopyProgress](<src/ops/ops_manager.cpp#L421-L437>)；[提交及源删除](<src/ops/ops_manager.cpp#L2707-L2771>)；[Undo](<src/ops/ops_manager.cpp#L1524-L1546>)；[删除分类](<src/ops/delete_operation.cpp#L86-L100>) |
| **E04：Shell 生命周期与命令** | F04、F05 | [handler 创建](<src/shell_host/main.cpp#L1254-L1269>)；[handler 调用](<src/shell_host/main.cpp#L1115-L1153>)；[dispatch_invoke](<src/shell_host/main.cpp#L1343-L1370>)；[FindItemId](<src/shell_host/ctx_handlers.cpp#L460-L474>)；[应用 cached click](<src/app/context_menu_controller.cpp#L238-L287>) |
| **E05：端点与响应边界** | F06、F18 | [本地索引连接／解析](<src/index/index_client.cpp#L98-L259>)；[worker](<src/index/index_client.cpp#L335-L373>)；[网络 VerifyServer](<src/index/index_transport_security.h#L126-L138>)；[clipboard](<src/ops/clipboard.cpp#L121-L127>)；[drag/drop](<src/ui/drag_drop.cpp#L150-L155>) |
| **E06：索引一致性** | F07、F08 | [ApplyUsnLocked](<src/index/index_engine.cpp#L2584-L2602>)；[CatchUpVolume](<src/index/index_engine.cpp#L2888-L2915>)；[EnumerateMft](<src/index/index_mft.cpp#L303-L340>)；[IndexVolumeMft](<src/index/index_engine.cpp#L2941-L2955>) |
| **E07：搜索终态与质量** | F09、F10 | [网络 SearchAndReply](<src/index/network_agent_main.cpp#L94-L113>)；[SearchRequest](<src/index/network_agent_client.cpp#L215-L247>)；[主窗合并](<src/app/app_navigation.cpp#L629-L644>)；[SearchPayload／去重](<src/index/index_host.cpp#L295-L310>)、[hash 范围](<src/index/index_host.cpp#L448-L460>) |
| **E08：快照与调度** | F11、F12 | [WorkResult](<src/app/app_worker.h#L34-L45>)；[Refresh／Stop](<src/app/app_worker.cpp#L82-L128>)；[enum 后代际检查](<src/app/app_worker.cpp#L228-L247>)；[tab 应用](<src/app/app_navigation.cpp#L984-L1040>)；[column 应用](<src/app/app_column_view.cpp#L289-L306>) |
| **E09：通知与 UNC** | F13–F15、F29、F30 | [watch retry](<src/fs/fs_watch.cpp#L209-L260>)；[UNC probe](<src/fs/fs_net_cache.cpp#L171-L217>)；[UI patch](<src/app/app_navigation.cpp#L1269-L1301>)；[失败 stat](<src/app/snapshot_patch.cpp#L157-L192>)；[cache save](<src/fs/fs_net_cache.cpp#L55-L83>)；[NormalizePath](<src/fs/fs_enum.cpp#L145-L168>) |
| **E10：大小与元数据** | F16、F17 | [详情大小扫描](<src/app/app_runtime.cpp#L959-L1042>)；[FolderSizes stop](<src/app/folder_sizes.cpp#L383-L417>)；[Details worker](<src/app/app_runtime.cpp#L62-L115>)；[path-only request](<src/app/app_runtime.cpp#L1856-L1872>)；[权限摘要](<src/app/details_meta.cpp#L41-L125>) |
| **E11：错误与持久化** | F19、F20 | [PasswordError／公开返回](<src/ops/archive.cpp#L160-L197>)；[archive result](<src/ops/archive.h#L17-L21>)；[archive UI](<src/app/archive_navigation.cpp#L225-L252>)；[JSON helper](<src/common/json_utils.h#L27-L84>)；[session 消费](<src/app/session.cpp#L356-L370>) |
| **E12：内容查询** | F21 | [candidate SQL](<src/index/content_index.cpp#L1275-L1301>)；[后过滤／body](<src/index/content_index.cpp#L1369-L1387>)；[version／negative scan](<src/index/content_index.cpp#L1523-L1593>)；[模式与接口](<src/index/content_index.h#L7-L68>) |
| **E13：预览** | F22–F28 | [thumbnail key](<src/ui/thumbnail_cache.cpp#L53-L57>)；[timeout／property reader](<src/ui/thumbnail_cache.cpp#L408-L456>)；[失败存储](<src/ui/thumbnail_cache.cpp#L487-L491>)；[文本解码](<src/preview_host/main.cpp#L123-L188>)；[Luma cache](<src/ui/lumatext_renderer.cpp#L616-L662>)；[video](<src/ui/video_preview.cpp#L142-L178>)；[GIF](<src/preview_host/main.cpp#L362-L453>) |
| **E14：架构及正向防护** | F31、合理边界 | [AppState](<src/app/app_state.h#L132-L212>)；[PaneViewModel](<src/ui/ui_renderer.h#L174-L187>)；[app aggregate header](<src/app/app_internal.h#L1-L11>)；[caller visibility](<src/index/index_caller_visibility.h#L34-L115>)；[文档隔离](<src/index/document_reader.cpp#L297-L343>)；[typed presentation test](<src/bench/presentation_state_cases.h#L73-L104>) |
| **E15：验证声明** | 本次实际检查、CI 与限制 | [依赖检查脚本](<tools/check_ui_dependencies.py>)；[发布 workflow](<.github/workflows/release.yml#L1-L36>)；[发布测试脚本](<scripts/build_release_ci.ps1#L37-L101>)；[仓库规则](<AGENTS.md>) |

**最终结论：**项目的原生平台选择和主要隔离边界值得保留。当前最需要收敛的不是技术栈，而是**请求身份、对象身份、结果完整性、视图投影和物理任务预算**。先修正这些契约，才能使后续缓存优化、模块拆分与界面改进建立在可信的数据和生命周期之上。
