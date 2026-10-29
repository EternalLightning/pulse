# ops/fs 只读审计（2026-10-30）

审计对象为当前工作树 `src/ops` 的 40 个文件和 `src/fs` 的 24 个文件，共 64 个文件、16,930 行；已逐文件完整阅读，清单见 `ops-fs-coverage.json`。先检查了本分区工作树差异，再审读完整实现；当前分区有 15 个已修改的受控文件和 19 个未跟踪文件，均作为现状审查并保留。下文行号对应本次读取的工作树。

只写本报告和覆盖清单；未修改、删除产品源码，未提交，未构建，未运行测试或破坏性复现。跨目录仅定向阅读调用链，不计入本分区完整覆盖。

发现 7 项 P2 和 1 项 P3。这里的优先级表示建议处理顺序；静态可达条件不等于现场已经发生。没有据此声称 MTP、SMB、Cloud Files、WinFsp/Dokan 或并发替换已经通过或失败于运行验证。

| ID | 优先级 | 问题 | 证据界限 |
| --- | --- | --- | --- |
| OF01 | P2 | 设备任务排队或换 lane 后失去超时预算 | 定时器生命周期与迁移分支已证实，未运行调度复现 |
| OF02 | P2 | MTP 枚举把失败 HRESULT 当作正常结束 | 返回值吞失已证实，未接真实设备拔出复现 |
| OF03 | P2 | 操作路径保护拒绝 Cloud Files reparse tag | 拒绝分支已证实，实际 provider 暴露 tag 的范围待验证 |
| OF04 | P2 | UNC 共享根无尾斜杠时不能捕获，直接子项传输被拒 | 路径分解与调用链已证实，未连接 SMB |
| OF05 | P2 | 递归固定/释放云文件忽略目录枚举失败 | 失败计数遗漏及 UI 消费方式已证实 |
| OF06 | P2 | “此电脑”/回收站未应用远程型本地挂载策略 | 策略分叉已证实，具体驱动挂起影响待验证 |
| OF07 | P3 | 提权协议丢弃批量冲突模型，操作体验分叉 | 编解码和 UI 分支已证实 |
| OF08 | P2 | 拖放暂存清理存在路径递归删除的替换窗口 | 无目录固定的删除间隙已证实，越界删除需并发条件，未动态复现 |

## OF01 — P2：排队和换 lane 使设备任务失去超时预算

位置：`src/fs/device_worker.cpp:99-109,124-149`；运行点：`:58-65`。调用证据：`src/app/device_registry.cpp:23,73,251-279`，设备列表和目录枚举使用同一个 worker，预算为 15 秒。

- **事实**：`Post` 在入队时创建一次性 timer，timer 保存当时的 `lane` 和 `seq`；`BudgetExpired` 只有在该 lane 正运行同一个 seq 时才处理。任务本身不保存预算/回调，开始执行时没有重新计时。放弃旧 lane 时，等待任务被移动到 fresh lane，其 timer 仍引用旧 lane。
- **触发**：A 任务运行较久，B 在队列中等到自己的 timer 到期；回调因 `running != B.seq` 返回，B 此后开始执行却没有 timer。另一条路径是 A 超时，B 被迁到新 lane；B 的 timer 在旧 lane 上判断，亦不会处理 B 的超时。
- **影响推断**：B 若卡在驱动/COM 内，新 lane 及其后续队列会再次永久等待，界面收不到这一请求的 `on_stuck`，与 worker 头文件承诺的后续恢复行为不一致。放弃 lane 不终止旧任务，这是实现事实；它不是此预算问题的修复。
- **建议**：把预算和超时回调放入 Task，在真正开始执行时针对当前 lane/seq 设 timer；完成后撤销 timer，迁移只移动尚未开始的 Task。若产品要限制排队总时长，应单独处理排队超时，不能让一次失效回调耗掉执行保护。
- **最小验证建议（未执行）**：同步点固定 A/B 顺序，分别复现“B timer 在排队时到期”和“B 迁移后阻塞”，断言 B 的超时回调只发生一次，C 能在 fresh lane 运行。app 分区的旧结果发布/连接 epoch 问题根因独立，不合并为同一缺陷。

## OF02 — P2：设备枚举把 Next 失败视为完整成功

位置：`src/fs/device_shell.cpp:76-89,160-195`；设备发现也复用该 helper：`:112-144`。

- **事实**：`ForEachChild` 在 `children->Next` 返回任意非 `S_OK` 时退出，然后统一返回 `true`。正常 `S_FALSE` 和错误 HRESULT 不可区分。上层 `Enumerate` 只在 `listed == false` 时返回 Failed/Disconnected，否则发送尾批并返回 None；它在 `:192` 有 cancellation 终检，不能声称所有取消后枚举都会回写成功结果。
- **触发**：绑定枚举器成功，枚举中途设备拔出、失联或 provider 返回失败；此前可能已收集部分行。
- **影响推断**：不完整列表被当作完整成功发布，空结果也可能掩盖断开/读取失败；设备发现中的存储枚举失败可能被解释为 locked。实际 HRESULT 的 provider 表现尚未用真实 MTP/PTP 设备验证。
- **建议**：用能保留 HRESULT/Cancelled/Stopped 的结果区分正常 EOF、调用方停止和失败。只有正常 EOF 才表示完整成功；部分行可保留，但必须携带不完整/错误状态。`src/ops/image_open.cpp:479-485` 已有区分 `S_FALSE` 与 `FAILED(hr)` 的本地例子。
- **最小验证建议（未执行）**：注入枚举器，先成功返回一批，再返回 `E_FAIL`/断开错误，验证不会返回 None；另外覆盖 S_FALSE 和主动取消，不要求真实手机才能做此定向回归。

## OF03 — P2：路径安全策略将 Cloud Files reparse 一律拒绝

位置：`src/ops/destination_guard.h:117-139,488-519`；传输调用链：`src/ops/ops_manager.cpp:2506-2514,2731-2734,2773-2778`；文件夹操作：`src/ops/folder_operations.cpp:119-125`。

- **事实**：Capture 读取 reparse 数据后，只接受 `IO_REPARSE_TAG_SYMLINK` 和 `IO_REPARSE_TAG_MOUNT_POINT`，其他 tag 无条件失败。它也用于 source parent，且 `PinPlainDirectory` 在 `destination_guard.h:193-200` 拒绝任何 reparse。仓库已在 `src/fs/cloud_pin.h:28-29` 识别 CLOUD tag 族，路径保护并未采用这一区分。
- **触发**：同步目录或某个祖先以 Cloud Files reparse point 暴露给这些 API，用户从中复制/移动，或将目标放在此目录。部分系统兼容性设置会隐藏 Cloud reparse；此审计没有把所有 OneDrive/WPS 路径都归为必现。
- **影响推断**：合法云目录会在实际传输前被报“目标路径身份无法确认或已变化”。即使 source 只是普通文件，其 parent 被捕获时也可触发。另 `ops_manager.cpp:3296-3299` 把所有 reparse 源交给原样复制 reparse 的分支，说明云占位符与名称重定向链接尚未在操作引擎中统一分类；未把这一分支另算独立问题。
- **建议**：在现有身份保护内明确区分名称重定向链接和 Cloud Files 占位符，采用 provider 支持的数据传输/水合行为。继续保留祖先身份、提交句柄和变更检查，不通过放行任意未知 tag 来绕过保护。
- **最小验证建议（未执行）**：用隔离同步目录覆盖普通子文件、带 CLOUD tag 的已落地目录和在线占位符，分别验证源/目标路径准入及复制后的内容；单纯去掉白名单不构成验证。

## OF04 — P2：无尾斜杠 UNC 共享根导致直接子项操作被拒

位置：`src/ops/destination_guard.h:521-549`；确定调用链：`src/ops/ops_manager.cpp:100-122,2506-2509`，以及 `src/ops/folder_operations.cpp:16-21,119-125`。

- **事实**：Components 必须找到 share 名后的下一根反斜杠才能得到 UNC root，找不到便在 `:539` 返回空集合。因此规范结果为 `\\server\share` 或 `\\?\UNC\server\share` 时，Capture 在 `:119` 拒绝；带末尾反斜杠的形式才有 root。`ParentOf` 会把源文件 `\\server\share\a.txt` 的父目录构造成无尾斜杠形式，`admit_source` 随即对该父目录调用 Capture。`fs::NormalizePath` 对 UNC 的分支（`src/fs/fs_enum.cpp:199-210`）也没有统一补共享根尾斜杠。
- **触发**：复制/移动直接位于共享根下的文件或文件夹；分组文件夹创建/恢复的 parent 恰好是共享根时亦可触发。
- **影响**：源内容完整、权限正常时也被静态路径分解拒绝，映射盘路径与 UNC 路径行为不一致。这里没有断言所有“复制到共享根”都失败：其 destination Capture 收到的是带文件名的路径，不应套用同一结论。
- **建议**：显式解析并规范化 server/share 根，接受有无尾斜杠的等价形式；为 Capture、Check、PinDirectories 等统一处理空 Components 的失败语义。当前 Check/PinDirectories 的空循环会返回 true（`:203-219,267-274`），与 Capture 的失败语义不同，应一并检查其调用前提。
- **最小验证建议（未执行）**：先针对纯路径拆分覆盖普通/扩展 UNC、共享根有无尾斜杠和根直接子项；再在隔离 SMB share 中定向验证 source admission。未连接实际 SMB，也未声称完成运行复现。

## OF05 — P2：云固定/释放忽略递归枚举失败

位置：`src/fs/cloud_pin.cpp:92-113`；UI 消费：`src/app/cloud_commands.cpp:136-143`。

- **事实**：顶层 Probe 和 SetFileAttributes 失败会增加 failed；进入子目录后，FindFirstFileEx 失败在 `:99` 直接 continue，FindNextFile 失败在 `:112-113` 也未读取错误码，二者均不增加 failed。UI 只在 failed 非零时显示失败，随后刷新路径；没有额外处理“不完整枚举”。
- **触发**：父目录属性修改成功，但列子目录被 ACL 拒绝、provider 离线或中途读取失败。
- **影响推断**：部分后代未被访问，用户却收不到失败提示。不能把“标记父目录后 provider 可能自行继承”当成已完成整树访问；本 API 头文件明确承诺对后代执行，当前返回值无法表达未完成的遍历。
- **建议**：区分空目录的正常结束与枚举错误，记录失败目录和错误码，向 UI 返回部分完成；保持现有 cancel 检查，继续避开外部 reparse。先收集证据，再决定 provider 继承策略是否可以替代主动遍历。
- **最小验证建议（未执行）**：使用隔离 fixture 或可注入枚举 API，制造父可写而子不可列、以及中途 Next 失败，断言结果和 UI 可见错误，而非只检查 changed 数量。

## OF06 — P2：远程型本地挂载策略在此电脑和回收站分叉

位置：`src/fs/fs_enum.cpp:469-490`、`src/fs/fs_recycle.cpp:178-190`、`src/ops/delete_operation.cpp:94-106`；现有策略：`src/fs/drive_kind.h:2-7,27-30,74-89`。

- **事实**：新 drive_kind 明确将 WinFsp/Dokan/CBFS 类本地呈现的云挂载视为 remote-like，并说明不扫描回收站。EnumerateThisPc 只特判 DRIVE_REMOTE，对其余盘直接 GetVolumeInformation；回收站列表和 EmptyRecycle 计划只按 DRIVE_FIXED/REMOVABLE 过滤，然后读取 `$Recycle.Bin`。这些点未复用 IsRemoteLikeDrive。
- **触发**：挂载驱动报告本地盘，但其服务、网络或介质正在长时间阻塞。
- **影响推断**：枚举工作线程可能卡在盘符查询或回收站访问，导致其他盘的“此电脑”/回收站列表迟迟不返回；清空回收站可能等待或因单个挂载无法完整列举而拒绝本地项目。没有称为 UI 主线程 I/O，也未实测任何具体驱动的挂起时长。
- **建议**：在介质访问前应用已有 remote-like 策略；对必须获取的远端信息复用已有有界异步查询/缓存策略。回收站支持范围需要列表与 EmptyRecycle 一致，不能一处跳过、一处仍以全盘完整性失败。
- **最小验证建议（未执行）**：先检查 remote-like 测试盘符标记的准入分支；再用隔离挂载验证超时、取消及其他盘仍能返回。此项是统一策略遗漏，不把本地、UNC、MTP 的合理后端差异算作重复。

## OF07 — P3：提权协议停留在单冲突模型

位置：`src/ops/operation_elevation_adapter.cpp:564-607,714-747,901-908,1375-1381`；当前引擎：`src/ops/ops_manager.cpp:3120-3131,3161-3178`；UI：`src/app/app_ops_ui.cpp:355-380`。

- **事实**：普通传输把全部冲突写进 ConflictItemInfo.rows，UI 用 rows.size()>1 打开可逐行决定的批量对话框。WriteConflict/ReadConflict 未传 rows；Control 只有 choice/apply_all，worker 只调用 ResolveConflict，不能调用 ResolveConflicts。经提权通道后 rows 恒为空。
- **触发**：受保护目标要求继续提权，剩余复制/移动含多个冲突。
- **影响**：相同任务在提权后退回逐项对话框/统一选择，失去逐行批量操作体验。仍有目标身份检查，不能据此声称可静默覆盖或已经丢数据。
- **建议**：扩展现有协议版本，序列化冲突 rows 及逐行 resolutions，保留 row_id、路径和元数据校验；让同一业务模型经过 IPC 保持语义，避免维护两个逐渐漂移的冲突 API。无需为此合并所有 Shell/本地操作后端。
- **最小验证建议（未执行）**：编解码 round-trip 覆盖两行不同选择；隔离提权 fixture 验证界面/结果与普通路径一致，拒绝重复或未提供的 row_id。

## OF08 — P2：拖放暂存清理未固定目录身份

位置：`src/ops/drop_staging.cpp:109-126,170-181,257-273`；可复用的现有防护例子：`src/ops/archive.cpp:195-217`。

- **事实**：RemoveTree 用路径读取 attributes，判断为非 reparse 目录后再按同一路径枚举/递归 DeleteFile。期间没有持有阻止替换的目录句柄，也没有固定每一级祖先身份。stage_root 的 reparse 检查仅在之前执行一次；失败清理和退出/启动 sweep 都调用该递归删除。archive 清理已在目录枚举期间持有 DestinationGuard，并用检查过的句柄删除。
- **触发条件**：同用户的外部进程或并发文件操作，在 attributes 检查后把 stage_root、stage 或子目录重命名并替换成指向其他可写目录的 junction。若替换发生在递归枚举前，枚举可跟随替换后的路径；子项 DeleteFile 亦经该路径访问。
- **影响推断**：存在删除 PulseDrop 以外可写子项的 TOCTOU 窗口。它要求并发替换，正常静止目录不构成复现；本次未运行竞态攻击，也没有碰用户临时文件。
- **建议**：将暂存清理接到已有保留对象身份/固定父目录的清理原语；所有祖先固定期间才枚举，reparse 只作为叶处理，替换则停止并留下残留。不能以名字位于 `%TEMP%` 或具有 PID 前缀代替所有权与路径身份。
- **最小验证建议（未执行）**：只在隔离 sandbox 中加入 attributes/枚举之间同步点，将 stage 子目录换成指向外部 sentinel 的 junction；验证 sentinel 完整、清理失败可见。不要在真实用户临时目录进行该实验。

## 设计与重复实现判断

- 本地目录 Native/Win32 枚举、UNC 路径、MTP Shell 命名空间以及提权进程有不同平台约束；未把这些后端本身列为重复。句柄身份、临时提交、历史身份 Undo 和无替换 rename 是数据安全约束，不能为了缩短代码删除。
- 优先收敛已出现可见漂移的策略边界：reparse 类型分类（OF03）、remote-like 盘策略（OF06）、ConflictItemInfo/逐行决策协议（OF07）、暂存清理身份模型（OF08）。这些都有具体结果或风险，收益比机械统一所有 Join/Quote helper 更明确。
- `src/fs/fs_snapshot.cpp:42-50` 的体积估算未计 shell_id、recycle_path 和 change 字符串。但当前 `app_navigation.cpp:1264-1267` 跳过 `pulse:` 虚拟目录缓存，而这些字段主要属于设备/回收站/变化记录。未证明当前路径下造成显著缓存超预算，故仅记扩展时维护点，不列独立缺陷。
- DeviceWorker 可持续遗留卡住的线程（`:75-80,99-109`），相比 watcher/probe 的固定配额存在资源上限差异；本次不以“detach 等于泄漏”直接定罪。修 OF01 时应同时明确退休线程上限与饱和行为，真实驱动永久阻塞的资源恢复仍需验证。

## 验证记录与边界

已执行：读取 AGENTS；检查工作树 diff/status；完整阅读清单中的 64 文件；定向交叉阅读 app 的设备、云命令、导航、冲突 UI；逐项核对错误传播、取消、身份与调用顺序；生成本报告及 JSON 清单。对 API 语义定向查阅了 Microsoft Learn 的 IEnumShellItems::Next 和 Cloud Files 兼容性材料，provider 可见性差异已保留为条件。

未执行：编译、既有单测、自测、全量回归、实际文件复制/删除、手机插拔、SMB 连接、云盘水合/释放、提权操作及并发目录替换。报告中的最小验证步骤是后续建议，不是通过记录。所列问题在修复前均未解决；本次只读审计不以既有测试文件存在或历史构建成功替代运行证据。
