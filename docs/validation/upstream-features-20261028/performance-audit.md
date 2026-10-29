# 上游适配审查：启动、空闲、动画、回收、SMB、缩略图与文件操作

> 本文保留首次检查时的代码证据，下面的“缺失/尚未适配”描述属于当时基线。2026-10-28 后续实现及最新验证结果见 [性能适配记录](../performance-adaptation-20261028/README.md)，七项最新汇总见 [功能适配总表](README.md#七项检查结果含补充两项)。按用户后续决定，上游动画效果、悬停重绘、暂停立即生效及即时反馈均排除，不再作为待适配项。

- 审查日期：2026-10-28。
- 固定上游：`be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26`。
- 本地审查基线：`HEAD a6ad4a2d` 加当前工作区已有修改；本文件的代码行号以审查时工作区为准。
- 范围：原清单的后 5 项，以及补充的缩略图、复制/移动/删除性能 2 项，共 7 项。仅检查实现，不移植优化，不修改产品代码，不涉及 AI 功能。
- 按用户最新要求：不追踪悬停重绘优化，不将“暂停立即生效”或暂停请求即时反馈列为待适配项。
- 方法：读取 `AGENTS.md`、本地相关实现及上游相关提交，比较行为；“已有”仅代表代码路径存在，不代表本次已经完成真实设备、性能或视觉验收。

## 7. 启动先显示、延迟初始化与体积优化：部分已有，核心启动适配缺失

上游参考：`315605c6`（首帧先于激活、分消息延后服务初始化）、`ec5f903f`（首帧不使用的系统 DLL 延迟加载）。

本地证据：

- `src/app/app_main.cpp:420` 在 `WM_CREATE` 内调用 `quickPreview.Initialize`；`:677` 附近仍在 `WM_CREATE` 创建操作窗口。上述工作发生于 `CreateWindowExW` 返回之前。
- `src/app/app_main.cpp:2391` 执行普通 `ShowWindow`，`:2392` 紧接 `UpdateWindow`；没有上游按步骤处理的 `WM_DEFERRED_STARTUP`、`AwaitFirstListing`、首帧之后才 `SetForegroundWindow` 的路径。
- 本地已有 `QueueShowTransition`（`app_main.cpp:2390`），这是显示过渡动画，不等同于缩短首帧前初始化。
- `CMakeLists.txt:97` 对 LumaText 使用 `/DELAYLOAD:lumatext.dll`；未发现上游 `ec5f903f` 中 version、netapi32、mpr、winhttp、crypt32、secur32、setupapi、cfgmgr32、rstrtmgr、winmm 的主程序延迟加载选项。

结论：保留了本地显示过渡及部分依赖延迟加载；不能声称已经适配上游“先首帧、后激活、分步初始化”及整套启动/体积优化。

可安全补齐的方向：把预览窗口、操作窗口等初始化封装成可重复调用的按需 helper，再将预热安排在首帧后、每条消息只做一步。保留早期用户操作的即时按需初始化路径、截图/托盘/最大化启动分支以及窗口销毁检查。系统 DLL 延迟加载可单独评估，但必须检查各 DLL 实际导入和首次调用行为。

验证边界：需要分别测冷启动和热启动、中文 IME、普通/最大化/托盘启动，记录创建、显示、首个 present、激活时点；仅编译或看到动画不能证明启动性能改善。当前审查未运行这些测量。

## 8. idle 按需唤醒：部分已有；悬停重绘不纳入适配

上游参考：`8e284ff3`（安静后降低轮询频率、目录和文件夹大小结果主动唤醒）、`77919d7f`（details hover 只画旧/新行及移动高亮覆盖区域，使用 dirty rect present）。

本地证据：

- `src/app/app_ui_schedule.cpp:15-20` 的维护 timer 在可见状态为 100 ms，隐藏状态为 200 ms，未按用户静止时间进一步放慢；因此不能套用上游安静 500 ms、隐藏 1000 ms 的结论。
- 同文件 `:23-46` 已把工具提示、重命名、地址搜索、预览标签到期等安排成单次 deadline timer；本地已存在自己的按需调度，不能判作“完全没有”。
- `src/app/app_main.cpp:273-274` 只在标签、预览等仍在动画时继续驱动帧；`src/app/app_visual_animation.cpp:156` 汇总尚未结束的视觉动画。
- `src/app/app_main.cpp:1151-1160` 附近仍在维护 tick 查询 `folderSizes.TakeChanged()`、处理目录通知；`src/app/folder_sizes.cpp:485` 是 `changed.exchange(false)`，未见其结果通过上游 `WM_UI_KICK` 唤醒 UI 的实现。
- `src/app/app_input.cpp:1902`、`:1914`、`:1920`、`:1935` 的 hover 变化调用 `InvalidateRect(hwnd, nullptr, FALSE)`，使整个客户区失效。
- `src/ui/ui_compositor.cpp:277` 主交换链使用 `DXGI_SWAP_EFFECT_FLIP_DISCARD`；`:288`、`:309` 的 `FLIP_SEQUENTIAL` 是创建降级路径。相关本地渲染器未见上游 `PartialRedraw`、`RenderPartial`、`Present1` dirty rect 链路。

结论：本地已有单次 deadline 和动画按需续帧；静止期进一步减频、共享结果主动唤醒尚未完整适配。上述 hover 源码记录指文件列表行高亮，不是悬浮卡片；按用户要求，本轮不追踪重绘优化，不计入待适配项。

可安全补齐的方向：先补文件夹大小和目录通知的合并唤醒信号，再降低维护轮询频率，避免结果变慢。

验证边界：应记录静止时 CPU、唤醒次数和 present 次数，并在静止后生成目录/文件夹大小结果测到屏延迟。本次未进行 CPU、GPU 或真实界面验收。

## 9. 菜单、侧栏、标签与预览动画：部分已有，各区域存在差异

上游参考：`f456c5e8`（菜单按显示节奏动画、根据锚点方向滑入）、`f29dde59`（菜单消息队列每次最多排空 8 ms）、`b15f955a`（侧栏拉伸选择标记、滑动 hover plate、转动箭头）、`24efcf10`（标签活动底板及增删动画）、`45207f05`（详情内容切换 reveal）。

| 区域 | 本地效果及代码证据 | 固定上游的效果差异 |
| --- | --- | --- |
| 菜单 | `src/ui/fluent_menu.cpp:1391-1480` 有打开约 200 ms 的淡入/位移、关闭约 140 ms 的淡出，打开位移方向固定；动画期间约 16 ms 等待消息，持续排空消息队列 | 上游普通菜单从锚点弹性展开、行内容错开出现，并根据向上/向下展开选择滑入方向；子菜单也有滑入和位置过渡。使用 DwmFlush 跟随显示刷新，并限制每轮消息处理时间为 8 ms，减少消息繁忙时挤占动画。过滤面板使用单独的淡入路径。 |
| 侧栏 | `src/app/app_visual_animation.cpp:113-136` 有滚动条渐隐与分组折叠；`src/ui/ui_sidebar.cpp:163-173` 及 `fluent_components.cpp:1576` 附近直接绘制当前 selected/hovered 底色；分组箭头按展开状态切换图形 | 上游选中底板在项目间移动时拉伸回弹，悬停底板连续滑动并淡入淡出，分组箭头旋转 90 度。本地分组折叠动画已有，差异集中在这些指示元素。 |
| 标签 | `src/ui/ui_renderer.cpp:665-725` 绘制并移动选中标签的底部指示线；`src/ui/tab_indicator_animation.h` 使用本地拉伸过渡；`tab_close_animation.h` 和 `app_visual_animation.cpp` 已处理关闭后的宽度、位置与重排 | 上游活动标签整块背景以弹簧移动；新标签缩放、上移并淡入，关闭标签保留短暂残影缩小淡出，其他标签弹性补位。本地已有关闭/重排过渡，差异并非“标签没有动画”。 |
| 详情面板 | `src/ui/ui_details_panel.cpp:219-239` 驱动预览内容淡入；`content_reveal_animation.h` 采用约 160 ms 渐显；`src/app/app_visual_animation.cpp:138-150` 有约 150 ms 预览区域展开/收起 | 上游切换文件时还对名称、按钮和属性做从上到下的柔边揭示及轻微上移；快速连续切换时直接替换，固定的预览区域保持原位。 |
| 独立快速预览切换文件 | `src/ui/quick_preview_window.cpp` 的 Update 重置内容并重绘，未见上游导航滑入的状态与计时路径 | 上游上一项/下一项按导航方向滑入约 36 px 并淡入，外框和标题保持原位；外部预览处理器窗口也保持原位。媒体播放帧、手动图片缩放不计入这项切换动画。 |
| 独立快速预览显示/隐藏 | 本地通过 ShowWindow 显示/隐藏窗口 | 上游当前也明确关闭了“从文件图标放大”的自定义窗口动画，保留系统显示/隐藏效果。纠正先前报告：没有自定义图标缩放动画不构成缺失。 |

本地 `src/ui/ui_renderer.cpp:74-78` 根据系统动画设置与高对比度决定是否启用 motion。上表记录的是效果与调度差异，不表示所有上游效果都必须照搬，也不表示已有本地动画存在功能错误。若后续选择适配，应沿用本地模型并保留现有标签/预览行为。

可运行的现有定向验证：构建并运行 `pulse_tab_indicator_animation_test`；详情/标签相关应用验证可选 `pulse_pane_animation_application_test`。后者 `--home-settings-only` 专门验证首页/设置，不应拿其通过结果代表侧栏、菜单或 Quick Look 已验收。当前工作区默认 `build/` 不存在，本审查没有构建或运行这些程序。

视觉边界：尚未实际观察明暗主题、动画关闭/高对比度、不同 DPI、窄窗、快速切换、菜单打开时主窗持续动画等场景；不确认帧节奏与视觉质量通过。

## 10. 回收失败绝不静默永久删除：已有本地替代实现，需要真实拒绝场景验证

上游参考：`87ee0cf8` 对关闭回收站、策略禁用及超容量等情形做提前拒绝，并阻止 Shell 的永久删除回退。

本地实现与上游结构不同，不能仅凭没有上游文件/符号就判作缺失：

- `src/shell_host/recycle_fileop.cpp:101` 起的 `RecycleAuthorized` 在核对源文件身份后，通过父文件夹的 `ITransferSource` 调用 `RecycleItem`（`:155`）；使用独立的回收 API，没有调用永久删除 API 的回退分支。
- 同文件 `TransferSink::Fail` 返回 provider 错误；`Advise` 失败即终止；回收之后还检查返回的当前用户回收站 payload、源/目标身份和 `$I` 原始路径信息。未验证成功的结果不会冒充可撤销回收成功。
- `src/shell_host/main.cpp:445` 的授权回收请求走上述独立回收后端。
- `src/ops/delete_operation.cpp:503-505` 分开处理网络永久删除与本地回收成员，避免整个混合批次切换成永久删除。
- `src/ops/delete_operation.cpp:597-610` 对明确的回收拒绝 HRESULT 重新生成剩余项的永久删除请求，再进入 `RunDelete` 的确认流程；不是直接调用永久删除后端。撤销请求不走这个回退。
- `src/shell_host/delete_fileop.h:21-23` 明确列出可触发重新确认的回收拒绝 HRESULT。该文件中的 `IFileOperation` flags 注释不能替代实际 `RecycleAuthorized` 路径的检查。

结论：已具备“回收请求使用独立回收 API；明确拒绝后另行确认永久删除”的代码保证。没有本次实测证据证明所有 Windows provider、回收策略与容量限制均按预期拒绝，不能把本结论写成所有机器上的绝对功能验收。

建议使用隔离文件与隔离环境验证：关闭回收站/NoRecycleFiles 策略、超容量文件与目录、回收站不可用、普通成功回收及撤销；对拒绝后的永久删除确认分别取消和接受，取消时应保留原始文件，接受时只删除重新确认的剩余原身份对象。不要修改用户真实回收策略来做自动测试。现有 `pulse_recycle_backend_test --confirmation-identity` 可验证确认期间身份替换；`--identity-only`、`--index-validation`、`--locked-error` 可按改动范围选择；这些不能替代策略禁用/容量拒绝场景。

## 11. SMB 批量复制、删除性能：上游并行优化缺失

上游参考：`025a7f27`（share 复制四个并行任务）、`0de915de`（share/cloud 批量永久删除最多八个并行任务，失败项仍串行重试/提示）、`12d63887`（目标文件夹一次列举后批量探测）。上游提交中的 loopback 数据属于其环境与实现，本报告不复用其耗时、加速比或性能承诺。

本地证据：

- 本地没有上游 `src/ops/transfer_engine.cpp/.h`；`src/ops/ops_manager.cpp:2823` 顺序遍历 transfer entries，`:3142` 在当前 entry 同步调用 `transfer_copy_file_`，没有针对 share 的四任务复制路径。
- `src/shell_host/permanent_fileop.h:155-162` 先顺序捕获授权根，再顺序调用每根的 `Delete`；`:99-112` 的目录枚举与递归删除也未引入上游网络批量并发。
- `src/ops/ops_manager.cpp:2801` 起对冲突候选逐项 `PathExists`/`ReadEntryMetadata`，不能据此声称已适配上游目标文件夹一次列举探测。
- 本地网络路径会在 `src/ops/delete_operation.cpp:117-119` 正确转为明确的永久删除 disposition；这属于行为与确认适配，不构成批处理性能优化。

结论：网络操作的本地安全/身份检查已有，但本次检查的上游 SMB 并行复制/删除优化未落入本地实现。

补齐前提：本地 transfer 和 delete 采用 `DestinationGuard`、确认身份快照、句柄固定对象与重试/撤销记录；不可直接复制上游循环或共用非线程安全的状态/guard。应另行规划受限并发：明确每个任务的授权对象、独立进度累加、取消传播、目标冲突仲裁及失败重试记录，再通过隔离 SMB 夹具验收。

验证边界：`pulse_recycle_backend_test --network-root <映射远程盘根>` 有现成远程身份/删除验证入口（`src/bench/recycle_backend_test.cpp:653`），但不是吞吐 benchmark。真实性能测量应使用用户授权的测试共享及唯一隔离目录，覆盖小文件批次、大文件、嵌套目录、失败/取消、远程映射盘与 UNC，并记录服务器/协议/缓存/防病毒状态。本次没有连接 SMB、操作共享或运行性能测量。

## 12. 缩略图并行解码、相机预览与下一屏预取：少量基础能力已有，主要提速链路缺失

上游参考：`24a0685c`（按 CPU 核心数启动至多四个解码 host、慢格式隔离与二级磁盘缓存）、`31c4756e`（下一屏预取、列举期间预热 host、文件夹合成图缓存）、`bed2a530`（图片网格先尝试资源管理器缓存、相机 JPEG 内嵌预览）。

本地证据：

- `src/ui/thumbnail_cache.h:145-159` 只有一个 pipe、一个 child 和一个 worker；`thumbnail_cache.cpp:151` 创建该线程，`:515-548` 的循环逐个取请求，再同步执行 `Connect() && FetchResponse(...)`。这是异步串行解码，不是上游网格缓存的多 host 并行。
- 本地已有内存 LRU、请求去重和消费者代次保护，但不存在上游 `thumbnail_disk_cache.cpp/.h`、`SetParallelism`、`Warm` 和缩略图 `Prefetch` 接口。
- `src/preview_host/main.cpp:597-654` 的 Shell 缩略图路径已先尝试 `IThumbnailCache` 的 `WTS_INCACHEONLY`，再调用 provider 或提取。这是已有的系统缓存能力。
- 但普通直接图片走 `main.cpp:713-738`：先 `DecodeImage`，失败才回退 Shell；`DecodeImage` 的 `:302-345` 从 WIC frame 解码/缩放，没有上游 `jpeg_embedded_preview` 的显式内嵌预览优先路径。因此不能把“存在系统缓存调用”写成“图片网格统一优先使用系统缓存”。Shell/provider 自己可能采用内嵌预览，也不代表 Pulse 已接入上游的新调度路径。
- `src/ui/ui_pane_list.cpp:1544-1548` 为正在绘制的项目调用缩略图 `Draw`，未发现下一屏缩略图入队逻辑。同文件 `:1445` 的 `content_results->Prefetch` 是内容搜索结果分页，不是缩略图预取。

结论：**部分基础能力已有；并行解码、相机内嵌预览优先、普通图片系统缓存优先、下一屏预取及上游磁盘二级缓存均未完整适配。** 不应宣称已获得上游这轮缩略图提速。

补齐时需保留本地预览进程隔离、列表与快速预览消费者代次、取消与云占位文件保护；不能只把现有共享 pipe 改为多个线程同时使用。验收应区分冷/热缓存、JPEG/RAW/视频/PDF、快速滚动与切目录，记录首屏及下一屏到图耗时。本轮仅审查源码，未构建、未运行解码或性能测试。

## 13. 复制、移动、删除提速及大文件进度：部分适配

上游参考：`2d0ef9eb`（按存储类型决定小文件并行数）、`c0453b2b`（大文件不阻塞小文件队列及按需绕过系统缓存）、`e35707c0`（大文件字节进度、暂停请求即时显示与 HDD 无缓冲策略）、`8b22cc75`（减少逐项元数据及路径扫描）、`0de915de`（批量远程删除并发）。

| 子能力 | 本地证据 | 结论 |
| --- | --- | --- |
| 小文件直接写入 | `src/ops/ops_manager.cpp:3097-3102`：小于 64 MiB 且不是替换冲突时，直接使用最终目标名；大文件/替换仍使用临时文件 | 已有，并保留本地目标身份保护。 |
| 同盘移动改名 | `ops_manager.cpp:3007-3037`：对可改名的移动尝试 `DestinationGuard::RenameLeaf`，只有允许的跨卷失败才退回复制 | 已有，不能把所有移动都算作先复制后删除。 |
| 大文件字节进度 | `src/ops/copy_progress_report.h:11-35` 只发布携带字节数的 chunk/stream 完成及错误消息，按 100 ms 节流；`ops_manager.cpp:2710-2735,3111-3118` 更新字节、速度、比例和 ETA | 已有对应的进度修正；回调被系统 I/O 阻塞时仍可能暂时没有更新。 |
| 暂停与继续（已有能力） | `ops_manager.cpp:351-353` 在 CopyFile2 回调中返回 PAUSE，`:3149-3175` 等待继续并使用 RESUME_FROM_PAUSE | 已有回调驱动的暂停/续传；按用户要求，不追踪“暂停立即生效”及暂停请求即时显示差异。 |
| 按磁盘并行复制、大/小文件分流、无缓冲策略 | 本地没有 `transfer_engine.cpp/.h`、`SmallCopyPool`、按存储类型的并行调度或 `COPY_FILE_NO_BUFFERING` 调用；`RunTransfer` 仍顺序处理 entries | 上游主要吞吐优化尚未适配，包含减少逐项开销的部分改动也未完整接入。 |
| 删除提速 | `src/shell_host/permanent_fileop.h:99-112,155-165` 按目录和根逐项处理；本地独立回收 API 负责安全回收 | 不能把回收安全等同于删除吞吐提速；前述网络批量并发删除仍缺失。 |

结论：**小文件直接写、同盘移动改名、大文件进度和暂停续传已有；并行/存储感知调度与无缓冲策略尚未完整适配。** 不能直接宣称已获得这轮吞吐提升；“暂停立即生效”不再列入待适配项。

本轮没有运行文件操作，也未测吞吐或真实大文件进度间隔。上一轮的 251 项功能定向断言不构成上述性能宣传的实测证据。

## 本次实际验证

完成相关本地源码、固定上游提交与构建目标的只读比对；未构建、未运行功能测试、未修改回收策略、未对用户文件执行复制/删除，未运行真实 SMB、CPU 或动画测量。本轮补充审查仅更新本报告及汇总 README，没有修改产品代码。
