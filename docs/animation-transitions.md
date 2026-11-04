# 视图过渡与性能验证

记录日期：2026-10-31。源码版本：1.3.0.0。

## 行为与实现

列表、详细信息与图标视图之间按稳定文件身份连接可见项目的位置和图标尺寸，保留当前排序。菜单切换使用约 220 ms 过渡，滚轮切换使用约 120 ms 过渡；快速连续输入从当前画面重新定向。每项绘制一个不透明图像，避免前后两层交叉淡化产生拖影。

Shift / Ctrl 加鼠标滚轮支持多刻度及高精度滚轮输入，并保持选中文件的滚动锚点。文件夹标题使用固定左边距，避免快速切换详细信息视图时随名称列左右移动。搜索与本地筛选保留已发布项目，通过防抖和项目增删、位移过渡更新结果。

默认复用 Direct2D 绘图命令，减少动画期间重复准备文字和图标。几何缓存和稀疏身份查询限制每帧工作，视图图标过渡最多保留 256 个可见图像。Direct2D 已使用硬件渲染；布局、输入及命令准备仍在 CPU 上执行。

额外的像素图集缓存为实验功能，默认关闭。可通过进程环境变量 `PULSE_ANIMATION_PIXEL_CACHE=1` 开启；当前测量未证明稳定的 P95 / P99 收益，不作为默认优化结论。

## 已执行的定向检查

以下为本轮已有构建和测试记录，整理本文时未重复完整历史回归。

| 检查 | 结果与边界 |
| --- | --- |
| 受影响的生产与测试目标 | 构建成功，未发现新增编译警告；生产配置关闭内嵌自测。 |
| 视图和布局回归 | 3265 PASS / 0 FAIL；包含标题像素位置、浅深主题、100% / 150% DPI、八种模式的快速正反切换。 |
| 默认命令缓存动画反馈 | 872 PASS / 0 FAIL。 |
| 实验像素缓存动画反馈 | 872 PASS / 0 FAIL。 |
| 像素图集独立回归 | 25 PASS / 0 FAIL。 |
| Shell 查询连接错误回归 | 10 PASS / 0 FAIL；拒绝访问及无效名称保留原错误，缺失或忙碌管道保持有界重试，取消与辅助进程回收通过。 |
| 画面检查 | 已查看可控颜色图像在 240 Hz 采样下的中间态，为单个不透明图像，无前帧残影；该用例不依赖原生 Shell 图标。 |

连接回归修复前，拒绝访问及无效名称被重试至截止时间并误报为超时。修复后分别返回 `0x80070005`、`0x8007007B`，样本耗时为 0、16 ms，均只打开一次。生产管道权限与对端 PID 校验保持原有契约。

## 性能测量限制

测量环境检测到 NVIDIA RTX 3070 Ti 硬件适配器和 3440 × 1440、165 Hz 显示器。但测试进程令牌为 AppContainer，使用生产用户 SID 安全属性的管道打开返回错误 5，导致原生图标准备不完整。此证据说明本轮测量受限，不能解释普通桌面会话中的卡顿根因。

`scripts/measure_animation_frames.ps1` 要求基线与当前程序均提供 `[ICON_READY] native_folder_sizes_16_256=1`，缺少该标记即拒绝对照。旧的临时基线程序未具备此检查，不能直接用于最终验收。原始截图、日志与 CSV 留在本地忽略目录中。

尚未完成完整原生图标、实际目录下的高刷帧率验收，不能据此宣称抽帧问题已消除。应在能通过生产管道访问检查的普通桌面进程中，按同一图标工作量与窗口尺寸采样后再比较。本文不代表安装包、安装升级或已安装副本的验证。

## 定向复现入口

```powershell
& .\scripts\build.ps1 -BuildDir build-release -Targets @(
    'pulse', 'pulse_item_reflow_ui_test',
    'pulse_shell_query_connection_test', 'pulse_item_reflow_bitmap_test')
.\build-release\pulse_shell_query_connection_test.exe

$env:PULSE_TEST_SNAPSHOT_STDOUT = '1'
$env:PULSE_ANIMATION_PIXEL_CACHE = '0'
.\build-release\pulse_item_reflow_ui_test.exe unused --view-mode-feedback-only
$env:PULSE_ANIMATION_PIXEL_CACHE = '1'
.\build-release\pulse_item_reflow_ui_test.exe unused --view-mode-feedback-only
.\build-release\pulse_item_reflow_bitmap_test.exe
Remove-Item Env:PULSE_ANIMATION_PIXEL_CACHE
Remove-Item Env:PULSE_TEST_SNAPSHOT_STDOUT
```

构建工具和隔离配置要求见[构建指南](build-and-package.md)。构建、功能验证、打包、安装和发布分别执行并记录。


## 2026-11-04：按实际时间测量与首个 CPU 优化

提交 `4e7b39ba` 已保存并推送此前的全部改动。以下为该快照之后的独立性能工作。

`BuildVm` 现在可在已有帧观察器开启时累计基础模型、命令状态、操作任务、侧栏动画、标签、状态栏、托盘、详情、窗格及其他准备时间，并记录同一帧调用次数。关闭观察时不读取这些分段的时钟，不创建计时日志或容器。

真实渲染基准覆盖视图切换、筛选、列表滚动、标签增删和托盘拖甩。输入按经过时间触发，呈现由当前交换链/系统节奏驱动；基准没有目标 FPS、固定逐帧延时或按帧号触发的输入。预热和测量窗口以时间定义。显示模式里的名义刷新率只记录环境，不参与调度或丢帧判定。

新增独立 VM CPU 基准，以固定样本数量比较 10000 个目录条目、侧栏/标签/托盘增多时的模型构建成本。它不创建 compositor，布局尺寸为 0x0，不调用 Present，不访问原生 Shell 图标，不能替代真实窗口的展示测量。模型维度和结果 checksum 同时输出，防止把减少工作量误当优化。

### 本次对照

前后使用相同测量代码、模型、预热和三次重复，每个场景各 3072 个样本；正式对照采样期间没有并发构建。首次与构建并发的试跑单独保留，未用于下表。唯一生产优化是在没有自定义快速访问顺序时跳过稳定排序：原来所有条目的排序权重都相同，却仍在比较函数里反复规范化路径。自定义顺序的路径保留原实现。

| VM CPU 模型 | 优化前中位数 / P95（ms） | 优化后中位数 / P95（ms） |
| --- | --- | --- |
| 24 项侧栏、1 标签 | 0.1454 / 0.1977 | 0.0405 / 0.0494 |
| 256 项侧栏、1 标签 | 5.1051 / 7.8341 | 0.3769 / 0.5165 |
| 256 项侧栏、48 标签 | 5.4492 / 8.6108 | 0.3666 / 0.4832 |
| 256 项侧栏、48 标签、2048 暂存条目 | 5.5696 / 8.0176 | 0.7739 / 1.0761 |

原始记录和 JSON 报告位于本地忽略目录 `bench_data/performance-baseline-20261104/`：`vm-before.log`、`vm-after.log`、`vm-comparison.json`。以上是该机器上隔离 CPU 模型的结果，不能换算为实际显示 FPS，也不代表其他机器同等收益。

优化后另跑了真实 Present 工作负载，原生文件夹图标门禁通过，五个场景共 2657 个有效帧样本，无绘制错误。环境记录为 RTX 3070 Ti、3440x1440 显示模式，基准窗口默认 1100x820；名义刷新率为 165Hz。这是一次环境记录，不是目标刷新率或已覆盖的全部刷新率。原始记录为 `render-after.log`，报告为 `render-after.json`。

该次渲染中 VM 的 P95 为约 0.28–0.46ms，Paint 的 P95 为约 5.34–11.40ms。下一步应结合 GPU/合成跟踪细分绘制阶段，再决定缓存或调度改动。仅凭当前数据不能确定绘制等待与 GPU 执行各占多少。

### 指标语义与复现

- `FRAME_CSV` 是渲染循环的实际间隔和 CPU 侧阶段墙钟时间。`total_ms - present_ms` 仍可能包含驱动等待；`input_ms` 单列。Present 返回成功不是实际展示时刻，也不是已展示/丢弃帧的独立证据。
- `VM_CSV` 是独立 CPU 模型样本，不具有展示帧或 GPU 耗时的语义。
- `MEMORY_CSV` 按实际经过约一秒采样工作集、私有字节，以及可用时的 GPU local/nonlocal usage/budget；不可用值为 `-1`。进程内存包含基准模型和样本存储，预算不是已分配量。
- 统计采用 nearest-rank 分位数，区分场景和活动/非活动帧，不根据某个固定刷新率推断丢帧。真实展示、GPU 执行、堆分配次数和栈仍需要 ETW/WPA 跟踪；当前机器的 WPR 提供 CPU、GPU、DesktopComposition 和 Heap 配置，本次未采集这些跟踪。

```powershell
& .\scripts\build.ps1 -BuildDir build-navigation-motion -Targets @(
    'pulse_item_reflow_ui_test', 'pulse_frame_profile_test', 'pulse_quick_access_badges_test')
.\build-navigation-motion\pulse_frame_profile_test.exe
.\build-navigation-motion\pulse_quick_access_badges_test.exe --order-layout-only

# 先创建输出目录；该入口仅测 CPU 模型。
New-Item -ItemType Directory -Force bench_data/animation-frames | Out-Null
.\build-navigation-motion\pulse_item_reflow_ui_test.exe --frame-profile-vm-only > bench_data/animation-frames/vm.log
python scripts/analyze_animation_frames.py bench_data/animation-frames/vm.log --output bench_data/animation-frames/vm.json

# 当前版本的受控窗口渲染测量；原生图标未就绪时会拒绝结果。
.\scripts\measure_animation_frames.ps1 -BuildDir build-navigation-motion -Label current
# 需要对照时，另传同一测量协议构建的 -BeforeExe；支持 -AfterFirst 调换顺序。
```

本次验证包含计时探针 8 项、默认/自定义快速访问顺序 4 项及统计脚本 7 项检查；VM 样本自检和真实渲染基准均通过。生产 pulse 目标构建成功，测量包装脚本的启动、窗口尺寸调整、原生图标门禁与 JSON 报告生成也已端到端通过。此工作不涉及安装或发布。

## 2026-11-04：绘制分项与过渡命令包装

`FRAME_CSV` 增加 `paint_*_ms`：准备、背景、标题栏、工具栏、侧栏、窗格、详情、设置、状态栏、覆盖层和其余组合工作。列表准备、列表行绘制是窗格内部的嵌套计时，不能再与窗格耗时相加；多窗格按阶段累计。加载中的列表可能直接返回，此时仍计入窗格耗时。观察器关闭时不读取这些分段时钟。

分项测量是 CPU 侧墙钟时间，含 Direct2D/驱动调用中可能发生的等待。首轮 2531 个真实 Present 样本中，窗格和侧栏占主要部分；列表准备和行绘制均有明显长尾。该结果不等于 GPU 执行时间，也不足以直接确定应缓存哪一层。

本轮试验过移除 `SurfaceTransition` 在没有旧画面时的组合命令列表，直接绘制并保留当前画面。两轮对照交换新旧执行顺序后，筛选和滚动的整体 Paint P95 没有改善，不能用减少一次包装证明实际更快，因此已撤回该试验。当前保留原来的过渡绘制过程和首轮快速访问排序优化。

### 交换顺序的测量结果

每个程序每轮包含五个场景、每场景两次重复；两轮合计 11066 个有效帧样本，原生图标准备门禁通过，绘制错误为零。表内为每轮独立统计的 Paint P95，未平均或合并不同轮次的分位数。

| 场景 | 首轮：原实现 → 试验（ms） | 交换顺序后：原实现 → 试验（ms） |
| --- | --- | --- |
| 视图切换 | 6.7310 → 7.5627 | 4.7840 → 6.3089 |
| 筛选 | 8.7215 → 10.4880 | 5.3480 → 10.2896 |
| 滚动 | 10.9702 → 12.4937 | 5.0968 → 14.7072 |
| 标签 | 17.0061 → 10.2721 | 5.6529 → 10.8238 |
| 托盘 | 12.6484 → 9.7298 | 4.9444 → 11.9096 |

原始记录为本地忽略目录 `bench_data/paint-profile-20261104/pair-ab-*.log`、`pair-ba-*.log`，配套报告为 `pair-ab-report.json`、`pair-ba-report.json`。采样期间没有并发构建；仅在各轮测量结束、开始输出 CSV 后进行构建和截图检查。第二轮试验程序增加了批量日志缓冲，采样代码、场景和 CSV 格式不变，逐帧记录仍在测量结束后输出。

试验版本通过导航 412 项、设置/主页 197 项、路径栏 92 项、视图/滚轮切换 872 项检查，并查看了浅深色、100%/150% DPI 的部分中间态和最终截图。这些功能结果没有抵消性能结果，试验没有进入最终实现。设置夹具原先使用外观页编号测试已经移到启动与集成页的开关；现已修正编号并增加控件存在断言，原有动画断言保持不变。首次包装脚本的参数传递错误和旧页面失败日志均已保留。

保留的绘制探针通过 10 项定向检查；统计脚本通过 7 项检查。基准和截图批量输出启用缓冲，减少验证工具自身的输出等待，普通交互式诊断仍即时输出。当前没有设置目标刷新率或固定帧延时。

最终生产 `pulse` 目标构建成功，配置为 `PULSE_WITH_SELFTEST=OFF`；没有安装、打包或推送本轮性能改动。

恢复原过渡绘制路径后，两个验证程序已重新构建，以上四组动画专项再次通过，共 1573 项，截图共 142 张。最终版本另取得 3285 个有效渲染样本、24 个内存样本，原生图标准备通过，绘制错误为零；五个场景的 Paint P95 为 4.05–5.48ms。这是当前版本的独立观测，不作为撤回试验后的性能提升比例。记录和报告分别为 `final-current-after.log`、`final-current-report.json`，位于同一本地忽略目录。

ETW 尝试使用独立命名会话采集 WPR 的 CPU、GPU、DesktopComposition 配置；普通执行与沙箱外执行均在开始记录之前返回 `0xc5585011`，提示 `Failed to enable the policy to profile system performance.`。这是 Windows 性能分析权限不足，自动审批未拒绝工具执行。没有生成 ETL，最终确认 WPR 未在记录；本轮没有 GPU 执行、DWM 展示或丢帧统计。诊断保存在本地忽略目录 `bench_data/paint-profile-20261104/etw-capture-status.json`。

## 2026-11-04：列表状态复用、按需模型与缓存预算

滚动会令项目重排上下文变化，原来的 `ItemReflowAnimation::Snap` 每次都清空两张身份表并重新分配节点。现在保留仍出现的文件节点，覆盖为当前几何/来源/条纹并清除旧速度和动画，再移除离开的身份。首次布局、禁用动效、重复身份和 512 项容量规则保持原语义。`Reset` 仍立即清空。

列表准备进一步拆为布局、项目身份/目标槽、动画状态、列布局四个嵌套计时，位于 `paint_list_setup_ms` 内，不能与其重复相加。

### VM 复用边界

现有窗格已经按 snapshot 身份、Places/TagRevision、过滤映射和日期键复用行缓存、标签、筛选及日期分组，选择统计也按 selection revision 失效。隔离模型中 Pane 分段中位数仅约 1.5–4.8μs；本轮没有再引入整份 VM 缓存。Sidebar/Places 仍有公开可变的工作区、近期及网络状态，TagRevision 无法覆盖其全部变动。

本轮直接跳过空快速访问徽章目录的路径规范化查询，并让 `TrayDeckEntries` 按批次定位所需的循环窗口，避免先复制全部暂存项目。模型构建中的标题同步、偏好、异步详情请求及动画状态推进仍按原流程执行。

### 图像缓存预算

预算按缓存持有的逻辑负载字节记录，不把条目数当作内存量；下表各预算为进程共享的独立池。

| 缓存 | 预算与回收 | 范围 |
| --- | --- | --- |
| Shell 图标 GPU 位图 | 16MiB 硬预算，同时保留 128 条上限，按 LRU 驱逐 | 生产窗口通过 AppCore 共用同一 RenderCaches；CPU Shell 像素沿用原有 worker 预算。 |
| 缩略图 | 每缓存 16MiB 软预算，加进程 64MiB 软预算 | CPU 像素容量及主视图/快速预览各自的 GPU 副本分别计入；可见工作集允许超限，切离视图后回收闲置条目。 |
| 动画像素图集和背景 | 64MiB 硬预算 | 使用共享租约，一张 atlas 的全部切片只记一次，最后一个持有者释放才退还预算；不足时沿用命令列表回退。默认像素缓存仍关闭。 |

GPU 位图按宽×高×4记录，不代表图形驱动实际提交或驻留量；命令列表持有的外部引用、交换链及菜单工作表面、解码中的瞬时缓冲不包含在上述可驱逐缓存预算中。没有降低图像或文字分辨率。

### 等待型交换链对照入口

实验进程环境变量 `PULSE_FRAME_PACING=waitable` 在创建交换链时启用 DXGI 等待对象，窗口绘制只做非阻塞就绪检查。主消息循环同时等待各窗口句柄及 UI 消息；隐藏、最小化、原生 sizing/menu loop、设备丢失和等待失败都有对应处理。驱动失活检测的 1 秒是故障回退时限，不是帧周期或刷新率判断。

基准脚本支持 `-Pacing present` 和 `-Pacing waitable`。等待模式必须输出启用证据，不能把创建失败后的旧路径作为等待模式数据；每帧的 `wait_ms` 单列在 input/Render 之前，实际帧间隔包含等待，`total_ms` 不包含这段等待。基准使用消息联合等待，所有输入和动画继续按实际经过时间推进。

实现依据为微软的[等待型交换链说明](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)、[等待句柄寿命](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)和[消息联合等待](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex)。

另可设置 `PULSE_FRAME_LATENCY=2`（脚本 `-FrameLatency 2`）对照两个帧的队列深度；仅等待型路径解释该变量，默认深度为 1。设置后读取 DXGI 实际值核对，失败回退。队列深度是可排队帧数，不是 FPS。实验未默认开启。

### CPU 对照

列表重置基准每个样本执行 16 次同样的几何输入更新及重置，预热后测 1000 个样本，重复三次。下表是各次独立 P50 的范围（μs），不是合并分位数；前后 checksum 一致。

| 保留项目数 | 原实现 | 复用身份节点 |
| --- | --- | --- |
| 48 | 9.36–9.56 | 2.69–2.75 |
| 120 | 25.87–26.57 | 7.80–8.01 |
| 480 | 220.16–257.55 | 39.28–52.44 |

VM 对照每场景各 3072 个正式样本，模型维度、输入及 checksum 相同，采样时无并发构建。下表为构建总耗时中位数 / P95（ms），不能换算为显示 FPS。

| VM 模型 | 本轮修改前 | 本轮修改后 |
| --- | --- | --- |
| 普通 | 0.0469 / 0.0786 | 0.0152 / 0.0184 |
| 256 项侧栏 | 0.4396 / 0.6362 | 0.0958 / 0.2581 |
| 再加 48 标签 | 0.4504 / 1.0759 | 0.0903 / 0.1765 |
| 再加 2048 暂存条目 | 0.9769 / 1.7400 | 0.0909 / 0.1524 |

本轮原始数据位于本地忽略目录 `bench_data/remaining-performance-20261104/`：`reflow-before.log`、`reflow-after.log`、`vm-before.log`、`vm-after.log`、`vm-comparison.json`。

### 可选 GPU 时间戳

系统 ETW 权限仍不足。内置 WPR GPU/light 配置本身也包含系统采样，单纯移除 CPU 配置不能消除权限要求；本轮增加无需系统跟踪权限的可选 `PULSE_PROFILE_GPU_TIMESTAMPS=1`，基准脚本对应 `-GpuTimestamps`。

GPU 时间戳通过同一 immediate context 在 `BeginDraw` 前、`EndDraw` 后插标，终点位于 `Present` 之前。固定八槽查询池，使用 disjoint 查询校验频率，并以 `GetData(DONOTFLUSH)` 延后读取；没有同步等查询、flush 或覆盖未完成槽。无效、未完成和满槽丢弃分别记录，不能当作零毫秒。结束时只做一次有限轮询。

`GPU_CSV` 保留原始 64 位 ticks、频率、disjoint、HRESULT、场景和正式/预热标记；报告只统计正式窗有效记录，预热排除。GPU timestamp elapsed 可能包含命令供给空隙及调度/抢占，不能称为纯 GPU 执行、busy 百分比、DWM 成本或实际展示/丢帧统计。未启用时生产路径不创建查询。

依据为微软的[D3D11 查询类型](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)、[非阻塞读取](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)和[时间戳可靠性条件](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_timestamp_disjoint)。

### 渲染对照与采用结论

默认路径的两组前后对照为 `bundle-*`、`bundle-reversed-*`，合计 12811 个有效渲染样本。第二组交换执行顺序并使用 `-Topmost` 设置基准窗口置顶；置顶不保证未被其他全屏画面遮挡，实际可见性限制见下文。CPU 模型及重置成本明显下降，但这些小工作集中的整体 Paint / 帧间隔未获得稳定的同等改善，不能把隔离 CPU 收益换算为显示 FPS。

GPU 查询另作为诊断采样：`waitable-final-1`、`present-final-1`、`present-final-2`、`waitable-final-2` 共取得 9858 个正式窗有效区间，无 invalid、pending 或 dropped 正式样本。第一组五个场景的 GPU 区间 P95 为 1.61–3.56ms。查询会改变命令流，因此后续主要帧间隔对照关闭 GPU 查询，GPU 数据不作为没有查询时的展示性能。

主要调度对照依次运行等待型深度1、原调度、等待型深度2，再反序运行，共 13041 个有效样本。使用同一最终源码、相同输入/窗口/图标，设置 `-Topmost`，没有并发构建；未逐轮取得物理屏幕可见性证据。以下是各轮独立统计的帧起点间隔 P95（ms），不是显示时间戳或丢帧计数。

| 调度 | 滚动：第一轮 / 反序轮 | 托盘：第一轮 / 反序轮 |
| --- | --- | --- |
| 原调度 | 36.7322 / 6.2309 | 47.7568 / 6.2840 |
| 等待型，深度1 | 54.5823 / 6.1862 | 48.5196 / 12.1364 |
| 等待型，深度2 | 31.6994 / 6.1708 | 58.4666 / 6.1589 |

跨轮的绘制墙钟时间也存在明显波动，原因尚未定位。两种等待深度均未证明整体更优，因此默认保留原调度；等待型只保留实验入口，不宣称卡顿或高刷问题已经解决。数据文件为 `visible-wait1-{a,b}`、`visible-present-{a,b}`、`visible-wait2-{a,b}` 的日志及 JSON 报告。

首次等待型基准曾在场景终点取走就绪许可却不绘制，导致下一场景超时；现跨场景保留许可至实际绘制后释放，该失败记录保留在 `waitable-1-after.log`，未纳入有效对照。窗口调度专项最初的失败则来自夹具漏泵普通消息及尚未等待真实目录动画终点；修正夹具后原断言保留，并额外检查动画确已停止。

### 本轮定向验证

| 范围 | 实际结果 |
| --- | --- |
| 生产 `pulse` | 构建成功，`PULSE_WITH_SELFTEST=OFF`，默认沿用原调度。 |
| 列表重排状态 | 157 PASS；含连续滚动重置和随后动画不继承旧速度。 |
| 托盘循环窗口 | 252 组旧算法顺序/指针等价检查通过。 |
| 快速访问顺序/徽章 | 7 PASS；默认值、即时 override 和清空恢复。 |
| 图像预算 / WARP 图标预算 / 像素图集 | 分别 21 / 18 / 25 PASS。 |
| 缩略图已有共享与绘制专项 | 45 PASS；含取消、过期结果、双消费者、双设备、恢复和可见工作集。 |
| 生产窗口调度 | 深度1 16 PASS，深度2 5 PASS；消息响应、隐藏/最小化、resize、恢复与失活回退。 |
| 帧观察器 / Python 统计 | 最终分别 20 / 13 PASS；含 GPU hook 门禁、嵌套恢复、64位 ticks、无效结果过滤及展示统计状态分类。 |
| 导航 / 主页设置 / 路径栏 / 视图反馈 | 分别 412 / 197 / 92 / 872 PASS；像素缓存开启后视图反馈另 872 PASS。已查看部分浅深色、100%/150% DPI 中间态和最终截图。 |

缩略图专项中还修正了旧夹具人工键遗漏 `:thumbnail`、以及失败短路跳过 `EndDraw` 的问题，保留全部原断言，生产解码流程未因此改变。失败及修复后的日志均保留。

### 2026-11-05：展示统计与交付边界

基准增加可选 `PULSE_PROFILE_PRESENT_STATISTICS=1`（脚本 `-PresentationStats`），读取 `GetLastPresentCount`、`GetFrameStatistics` 及 `IDXGISwapChainMedia::GetFrameStatisticsMedia`，保留各自 HRESULT、原始计数、QPC 和状态。生产普通帧不调用这些诊断 API；采样不执行 `DwmFlush` 或同步等待。零值、不完整、unsupported、disjoint、reset 与有效但未推进分别记录；只有 PresentCount 增加才标为 advanced，不能把 SyncRefreshCount 单独推进当作新展示。

本机 `presentation-current-*` 诊断取得 3289 个正式渲染样本及 3953 个含边界/预热的原始展示统计快照。正式窗中基础和 media API 各有 3289 个有效快照、675 次计数推进观测；两者成功返回，末次基础 PresentCount 为 3948。675 是观测到计数变化的次数，不是实际展示帧数；异步计数会跳跃，也不能据此推导逐帧显示时间、丢帧数或“零丢帧”。API 依据见微软的[基础统计](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)和[统计字段说明](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics)。

物理屏幕抓图核对发现基准区域被前台全屏画面遮挡，Windows 也拒绝本次测试进程的前台激活请求。因此逻辑窗口可见、置顶及 DXGI 统计推进不能作为人工可见性验收，先前调度样本也不能代表已确认无遮挡的显示效果。用户选择先推送已验证代码，人工可见性稍后验收；目标 120Hz 电脑当前不可访问。本机生产构建及上述定向自动检查已完成，但尚未确认用户感知的高刷抖动已消除。

本次交付范围为完成本机可执行的验证并推送代码，暂不打包或更新本机安装。默认保留原调度，等待型路径仅供显式对照；没有固定显示帧率或针对某一刷新率设置帧周期。

### 2026-11-05：补做本机屏幕可见性检查

后续重新运行时，全屏遮挡已解除。对本轮测试进程核对可执行路径、启动时间及窗口归属后，直接从物理屏幕截取该窗口区域，取得无遮挡的 Pulse 画面；未使用离屏窗口快照代替。前台激活仍返回失败，但不影响这次实际可见的置顶窗口。未关闭或最小化其他应用，也未安装新程序。

本机显示模式为 3440×1440、165Hz，测试窗口为 1100×820，默认 present 调度。五场景仍为视图切换、过滤、滚动、标签页和暂存托盘，输入及动画按经过时间推进。捕获 18 张屏幕帧，抽查其中 12 张，覆盖全部五场景及部分连续帧；所查列表帧未见内容越过表头绘制，拖动/甩动托盘帧未见文字和底框分离，标签操作可见布局收缩过程。它们是离散屏幕帧的检查，不是逐显示帧录像或人眼连续流畅度验收。

本轮进程及分析脚本正常退出，3271 个正式渲染样本的基础/media 展示统计各有 3271 个有效快照、3175 次计数推进观测，无正式窗 error、unsupported、disjoint、zero、incomplete 或 reset。计数推进仍不等于实际展示帧数或零丢帧。截屏会干扰运行，因此本轮仅补充可见性和诊断证据，不与之前受遮挡的采样作性能优劣对照。

原始屏幕帧保存在本地忽略目录 `bench_data/remaining-performance-20261104/onscreen-sequence-*.png`，日志及报告为 `onscreen-recheck-*`。本机的无遮挡屏幕帧检查已补齐；另一台 120Hz 电脑仍不可访问，不能据此确认其体感抖动已消失。未变更生产代码、默认调度或本机安装。
