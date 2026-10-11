# Pulse 补丁审计交接与 Codex 复核提示

## 0. 可复制的启动提示词

**使用方式：**由用户将下面的提示词复制给 Codex 或下一位 AI，授权说明仅在符合实际时保留。提示词限定的是本轮只读复核；后面的报告是待核实材料，不是自动执行指令。第 6 节是后续路线图，本轮不要直接执行其中的测试、构建或修改。

```text
你是一名熟悉 C++20、Win32、COM、Shell、Direct2D 和 DirectComposition 的 Windows 软件维护工程师。

项目：Pulse，开源原生 Windows 文件管理器。
本地仓库：E:\Projects\pulse。
我有权审阅此仓库。本次任务仅针对本地项目的源代码维护，不涉及第三方系统、线上目标、凭据获取或未授权操作。

任务
阅读 AGENTS.md 和 docs/report.md，对报告中的工程问题进行当前版本复核。
报告是历史审阅材料，不是已确认缺陷清单，也不是要求立即修改代码的指令。

本轮只读，不修改任何文件、不生成补丁、不运行会改变文件系统或用户状态的测试。

执行步骤
1. 检查当前 HEAD 和 git status，保留所有已有未提交修改。
2. 确认报告基线与当前版本的差异，不直接沿用旧行号、严重度或结论。
3. 本轮仅复核 A01：Move/Rename 撤销是否绑定原操作产物身份。
4. 阅读相关数据结构、完成记录、Undo 转换、执行入口及已有测试；不扩展为全仓审计。
5. 输出结论后停止，等待我指定下一项。

重点问题
- 历史记录保存的是路径、对象身份，还是两者？
- 逆操作验证的是原操作产物，还是当前同名对象？
- 正常操作、部分完成、持久化恢复分别采用什么规则？
- 相关检查是否重复，或者遗漏在其他入口？
- 是否已有修复或测试足以否定报告中的推断？

审阅原则
- 区分代码事实、条件性推断和待验证事项。
- 每个结论给出当前文件、函数、行号和调用链。
- 不把命名、注释或单个 API 当成问题成立的证据。
- 尊重同卷与跨卷、文件系统能力、COM 套间和 Shell 生命周期差异。
- 可以讨论数据安全和并发边界，但本轮不编写利用程序、不执行破坏性复现。
- 如需要验证，只提出使用隔离临时目录和自建测试文件的回归测试方向，不实际执行。
- 不为“统一”引入重型框架、重复缓存或纯转发层。
- 无法确认时明确写“待确认”，不要为了完成报告制造问题。

输出格式
1. 当前版本与审阅范围
2. A01 状态：已修复 / 仍存在 / 部分修复 / 证据不足
3. 代码事实与证据
4. 条件性影响及验证限制
5. 最小维护建议与理由，不提供完整补丁
6. 必要的定向回归测试建议
7. 应保留的已有设计
```

### 逐项使用与阶段边界

- 后续复核其他项时，将提示词中的 **A01、问题描述、重点问题及输出状态项**一并替换；例如 A05 聚焦停止闸门、锁序、I/O 所有权与终态，A07 聚焦枚举、排序与 UI 发布。不要只换编号而保留不相关问题。
- 审阅、验证、修改分别由用户明确发起；提出测试建议不等于获准执行，提出维护方向不等于获准生成补丁。
- 这份提示词用于准确表达合法的本地维护任务，不隐藏安全问题，也不保证解除服务侧内容限制。仍被限制时使用产品提示中的官方申请或支持入口，不通过换词隐藏真实目的。
- 下文保留原有事实、条件性推断、优先级和证据；所有问题都需要按当前版本重新核验。

## 1. 先读：范围、可信度与执行边界

- **审计版本：**`main`，HEAD `185cb83290b57384feef73a4ba21568cc2c3cef8`。
- **范围：**最近 25 次提交 `490e114^..185cb83`；基线 `8602e4269fcddfb7a7e9a4391a8e31baebe02301`。
- **规模：**净涉及 584 个文件、51,537 行新增、11,786 行删除。提交均已建立地图，但**没有逐行穷尽所有变更块**。
- **技术栈：**C++20 / MSVC / Win32 / COM / Shell / Direct2D / DirectComposition / DirectWrite；可选 LumaText。见[构建配置](<../CMakeLists.txt#L8-L30>)。
- **本轮仅静态审计：**没有构建、运行测试、性能测量或安全利用验证。下面的“代码事实”与“故障推断”必须区分；**没有已动态证实的 P0**，不代表没有 P0 风险。
- **未提交补丁不在审计范围内。**审计时工作区有删除错误展示及测试等原有修改；相关位置用 `git show HEAD:<path>` 核对过，不能把这些修改归因到历史提交。
- **本文件替换旧 `fec6df9` 工作树审计。**旧 F 编号不延用，旧缺陷不能直接当作当前缺陷：Copy/Create 已有历史产物 ID、Move 清理已有来源身份、索引连接已有认证、UNC 探测已有实际资源上限。
- 后续实施前先读[仓库规则](<../AGENTS.md>)，检查新 HEAD、当前 diff 和相关调用链；保留用户未提交修改。**此文不是立即全库重构的授权。**

**核心判断：**主要问题不是防御代码数量，而是局部检查未闭合整个操作的对象证明、生命周期或结果契约。先补保证链，再删冗余；不要只搬文件、加包装层或改接口名字。

## 2. 第一优先级：破坏性操作的四种证明不能混用

必须分别证明：①历史产物身份；②当前叶子身份；③祖先路径归属；④内容稳定性。路径只是定位器，单个 FileID 不能代替全部保证。

### A01 / P1：Move、Rename Undo 没有绑定历史产物身份

- **事实：**`PushUndo` 仅对 Copy/Create 保存 `produced_identities`；Move/Rename 逆操作只重建路径，逆 Move 再 fresh 捕获当前对象。
- **推断：**产物在 Undo 前被移走、旧目标出现同名新对象，Undo 可移动/改名这个新对象。是否进一步覆盖原位置对象取决于冲突选择，不能声称无条件覆盖。
- **方向：**Move/Rename/BatchRename 统一携带执行产物的历史证明，验证后通过对应 handle 变更；缺 ID 的旧持久化记录不能自动信任当前同名对象。
- **证据：**[PushUndo / Undo](<../src/ops/ops_manager.cpp#L1392-L1493>)、[逆 Move fresh 捕获](<../src/ops/ops_manager.cpp#L2026-L2035>)。
- **最小回归：**操作完成 → 产物改名 held → 创建同名 replacement → Undo；replacement 不动、历史不被错误消费。覆盖同卷、跨卷、KeepBoth、JSON 恢复。

### A02 / P1：来源子项 fresh ID 不能证明仍属于授权来源树

- **事实：**root 捕获后按路径枚举；来源 reference 允许 WRITE/DELETE sharing；对子项 fresh 捕获 ID。root 的最终身份拒绝可能发生在子项已经移动之后。
- **推断：**root 捕获后改名 held，在旧名建立 junction 指向 outside，枚举可能将 outside 子项作为有效来源。场景采用目录 merge 或跨卷路径，避开同卷 root rename-only 快路。
- **方向：**枚举与子项变更期间保护来源祖先身份/结构，或 handle-relative admission；不要机械保留全树所有文件 handle。
- **证据：**[root 与子项扫描](<../src/ops/ops_manager.cpp#L2031-L2108>)、[来源共享模式](<../src/ops/file_object_reference.h#L20-L36>)、[最终清理](<../src/ops/ops_manager.cpp#L2702-L2738>)。
- **最小回归：**在 root 元数据完成、首次枚举前确定性换 junction，outside 内容和属性必须完全不变。

### A03 / P1：copy-delete 只比对象 ID，未保护复制后的内容版本

- **事实：**CopyFile2 返回、可选 verify、目的地提交后按 ID 删除来源；应用保留的来源 handle 允许写，`Matches` 只比较身份。
- **推断：**复制结束后同一对象 append/truncate，ID 不变，新内容没有副本却可能被删除。**不依赖假设 CopyFile2 复制期间的共享语义。**
- **方向：**明确不可变内容策略；可取得禁止写 lease 时保持到清理，不能取得时拒绝 destructive cleanup 或保留来源并报告部分完成。再比 mtime/size/hash 仍有 check/use 窗口。
- **证据：**[提交后清理](<../src/ops/ops_manager.cpp#L2644-L2708>)、[身份匹配与 Delete](<../src/ops/file_object_reference.h#L20-L61>)。
- **最小回归：**复用 `before_source_cleanup_`，在 copy/verify 后改写同一个对象，不替换路径；新内容来源必须保留。现有[来源身份测试](<../src/bench/ops_test_main.cpp#L1220-L1273>)主要测名称替换。

### A04 / P1：解压发布仍有祖先 TOCTOU

- **事实：**成员过滤、归档本体锁、独立 stage 有效；但 `NoReparse` 是瞬时属性检查，后续 `create_directories` / `MoveFileExW` 仍按路径使用。
- **推断：**检查后、发布前换目标祖先 junction 可越界写。需要目标或临时树存在并发替换者，**未证明仅一个离线恶意包即可利用**。
- **方向：**最终发布采用稳定目录 lease / owned handle 提交，收口到现有目标保护原则；保留 no-overwrite、跨卷发布和 partial listing 拒绝。
- **证据：**[NoReparse](<../src/ops/archive.cpp#L159-L167>)、[归档锁与完整列表](<../src/ops/archive.cpp#L315-L325>)、[检查后发布](<../src/ops/archive.cpp#L364-L375>)。
- **最小回归：**在逐项检查后、提交前换目标/临时来源祖先；outside 不可写，取消与清理不得扩大影响。

## 3. 第二优先级：IPC 与异步状态必须闭环

### A05 / P1：停止、取消与 I/O 发起之间有缺口

| 链路 | 代码事实与故障推断 | 证据 |
|---|---|---|
| Index | 单次 CancelIoEx 后 join；header/body 间无持久停止闸门。取消时没有 pending I/O，随后才发起 body read，对端不发正文可无界等待。Disconnect 还可能先等阻塞 writer 的锁。 | [Stop](<../src/index/index_client.cpp#L41-L52>)、[帧读写](<../src/index/index_client.cpp#L157-L186>) |
| Shell | Submit 持 send_mutex，且持 pending_mutex 执行无限写；Stop/Abort/Cancel 先等 send_mutex。背压可使取消等在被取消者后面。 | [锁内发送](<../src/ipc/shell_client.cpp#L180-L226>)、[Cancel/Abort](<../src/ipc/shell_client.cpp#L305-L324>) |
| Preview | 约 1 秒预算只等响应头；正文同步 ReadAll，Reset 单次 CancelSynchronousIo 后 join。头部预算不是整帧预算。 | [Reset](<../src/ui/thumbnail_cache.cpp#L62-L70>)、[正文与映射](<../src/ui/thumbnail_cache.cpp#L482-L524>) |

- **共同方向：**持久停止信号；停止后禁止新 I/O；取消不依赖阻塞写占有的锁；整帧/整请求 deadline；完成回收后才释放 buffer / OVERLAPPED。
- **不要误修：**不能 join 超时后释放仍在使用的数据；不能为共用传输抹平 read-only、交互、modeless、有副作用请求的差异。
- **最小回归：**只发 header，在 body 发起前 Stop；peer 活着但不读，超过管道缓冲后 Cancel/Stop；Preview 半帧。检查有界终态及真实完成回收。
- **归因：**部分结构在基线已存在；空闲 CPU 修复和后台转发确实有效，不能把所有同步 Submit 都说成直接在 UI 调用。

### A06 / P1：Preview 响应没有完整 schema 验证

- **事实：**文本/属性有长度上限，但 mapping_chars 直接 resize；像素消费未统一验证 width/height/stride、组合合法性、generation 和总分配预算。
- **推断：**异常/被破坏的 helper 响应可造成巨额分配或未捕获异常；未证实普通媒体输入能直接利用、内存越界或提权。
- **方向：**分配/正文读取/映射前一个入口完成整个响应验证；失败退役连接并给终态。
- **证据：**[Preview 消费](<../src/ui/thumbnail_cache.cpp#L450-L542>)；参考[ShellQuery 严格验证](<../src/ipc/shell_query_protocol.h#L148-L190>)，复用原则而非强行共用 wire format。

### A09 / P1：网络调度所有权与状态 freshness 混用

- **事实：**Finish(probe_id) 只保护当前调度槽；不匹配的结果仍更新网络状态与 net_readonly。枚举错误被压成 bool，UNC 的访问拒绝也可显示 Offline。
- **方向：**按路径 latest ticket/epoch 判结果有效性；未被更新探测取代的晚成功可以接受。保留错误类别，分开权限拒绝、断网、取消、饱和、provider 失败。
- **证据：**[结果无条件应用](<../src/app/app_main.cpp#L1597-L1623>)、[超时后晚结果](<../src/fs/fs_net_cache.cpp#L275-L289>)、[错误展示](<../src/app/app_navigation.cpp#L1009-L1025>)。
- **回归：**A 超时 → B 更新 → A 晚返回，不得覆盖 B；没有 B 时仍能接受有效晚成功。不要简单丢掉所有 Finish=false 结果。

### A10 / P1：物理资源有上限，前台终态/恢复却不完整

- **事实：**Worker 全局上限 8、单池通常 2–4；UNC probe 上限 4，超时不释放真正阻塞的所有者，这是正确保护。Refresh 可返回 0；probe admission 失败保留队首，资源释放未闭合重试唤醒。
- **推断：**同池全部阻塞会拖住本地请求；启动失败可留下 loading；第五个 probe 等资源释放后未必自动恢复。
- **方向：**保留实际 live+retired 上限；拒绝/饱和必有终态；物理资源释放唤醒逻辑队列；必要时隔离本地与网络/provider 预算。
- **证据：**[Worker 启动](<../src/app/app_worker.cpp#L63-L87>)、[Refresh 拒绝](<../src/app/app_worker.cpp#L116-L122>)、[前台 probe pump](<../src/app/app_navigation.cpp#L80-L98>)、[probe 上限](<../src/fs/fs_net_cache.cpp#L298-L330>)。
- **回归：**4 个实际探测占满，第 5 个排队，释放一个资源后自动恢复或明确终止；禁止以逻辑超时重新计数绕过上限。

## 4. 第三优先级：目录/UI 根因与可删冗余

| ID / 优先级 | 事实、方向及验证 |
|---|---|
| **A07 / P1：UI 全量快照处理** | 通知属性虽在后台，发布仍复制全快照并 sort；新建项又 stable_sort；path+sort 请求可对同目录重复枚举。方向：一次 immutable FS facts、后台排序投影、UI 只验证 generation 并交换结果。证据：[通知发布](<../src/app/app_notify_patch.cpp#L30-L49>)、[新项展示](<../src/app/app_ops_ui.cpp#L65-L74>)、[请求 key](<../src/app/app_worker.cpp#L116-L137>)。回归：大目录小通知、多 pane 不同排序，计数枚举/排序/复制量和 UI 长帧。 |
| **A08 / P1：名称身份固定 case-fold** | 通知 positions 使用 towlower hash/equality；大小写敏感目录同时 A.txt/a.txt 时，可更新/删除错误记录。方向：名称身份与显示排序分开，精确名称及目录能力收口；Shell/归档不能套同一普通 FS 规则。证据：[索引与应用](<../src/app/snapshot_patch.cpp#L99-L196>)。回归：真实 case-sensitive 目录的增删改名和选择。 |
| **A11 / P1：同步动画** | 连续时长/背景补丁最终仍 UI WaitForCommitCompletion、DwmFlush、Sleep(8) 循环；HEAD 循环时长 110ms。方向：DComp 动画或非阻塞生命周期，保留高对比度/禁动画/非合成差异。证据：[等待与循环](<../src/ui/dialog_lifecycle.cpp#L160-L209>)。回归：快速关闭、嵌套模态、owner/focus、禁用动画。 |
| **A12 / P2：退役算法靠测试保活** | KeepEntryOrder 及旧 ApplyDirNotify/Batch 的外部调用集中在测试；生产改用 resolved 流程，旧保序状态反复清 false。删除确认无生产用途的算法/参数/状态，把有价值测试迁移到实际入口；不要删仍用的 FollowEntryRenames/PruneEntryRenames。证据：[旧保序](<../src/app/entry_order_hold.cpp#L50-L78>)、[旧通知 API](<../src/app/snapshot_patch.cpp#L56-L94>)、[resolved 流程](<../src/app/snapshot_patch.cpp#L122-L196>)。 |
| **A13 / P2：drag-over 属性 I/O** | HeaderDropHint → FirstDroppableFolder → GetFileAttributesW 在 hover 重复执行。“无 COM”不等于无阻塞。方向：本次 drag 元数据/cheap hint，后台 generation 查询，drop 最终验证另排队；不能裸跨套间用 IDataObject。证据：[属性查询](<../src/ui/drag_drop.cpp#L156-L163>)、[UI 调用](<../src/app/app_input.cpp#L332-L347>)。 |
| **A14 / P2：缓存同步解析** | 导航/WarmupUnc 直接 LoadNetSnapshot，包含读盘、reserve、字符串解析。方向：已有后台 I/O + generation 发布；新鲜枚举优先，不增加第三份缓存。证据：[导航调用](<../src/app/app_navigation.cpp#L967-L984>)、[加载器](<../src/fs/fs_net_cache.cpp#L138-L186>)。 |
| **A20 / P3：文本度量重复路由** | 文件名统一 DWrite，但 Luma 全局 enabled 时仍可走组合测量 key，再被 format 路由拒绝。方向：绘制/度量使用同一轻量路由、复用 advance 缓存；不造 factory/strategy，不据此删整个 Luma。证据：[format 路由](<../src/ui/ui_compositor.cpp#L138-L158>)、[度量入口](<../src/ui/typography.cpp#L357-L368>)。收益未实测。 |
| **A22 / P2 候选：枚举 fallback 过宽** | NT runtime_error 后再跑 Win32；诊断已保留，但哪些错误代表真实能力差异尚缺证据。先按错误码/provider 测量再收窄，不贸然删除兼容后端，不推断绕过 ACL。证据：[fallback](<../src/fs/fs_enum.cpp#L492-L505>)。 |

## 5. 明确不能误删的正确设计

| 保留项 | 理由与证据 |
|---|---|
| **A15：目标 snapshot / lease / handle 提交** | 确认授权快照、稳定祖先、owned leaf 提交分别保护不同阶段；不是调用次数多就冗余。归档应向这里收口。见[目标保护](<../src/ops/destination_guard.h#L30-L109>)、[提交](<../src/ops/ops_manager.cpp#L2644-L2698>)。 |
| **A16：本地/网络删除差异与回收凭证** | 网络无原生回收；Undo 不能永久降级；confirmed/uncertain 不是 HRESULT bool。保留单删除授权入口、混合批次分组、cancel 不代表 rollback。见[删除服务](<../src/ops/delete_operation.cpp>)、[回收证明](<../src/shell_host/recycle_fileop.cpp#L101-L171>)。删除文件有未提交展示修改，先核对 HEAD。 |
| **A17：Shell session/query/launch/interaction 分工** | 不可控扩展要可终止进程；COM/modeless 需要 owning STA；query 是只读预算，交互和 UAC 寿命不同；有副作用 dispatched 后不能自动重放。可共用低层认证/RAII，不可改成万能线程池。见[session 寿命](<../src/shell_host/ctx_session_process.cpp#L117-L178>)、[query STA](<../src/shell_host/shell_query_worker.cpp#L90-L104>)、[no-replay](<../src/ipc/shell_client.cpp#L8-L23>)。 |
| **A18：helper 与 SYSTEM service 两种认证** | helper exact image/token；service 用 SCM running PID、own-process、LocalSystem 配置。普通用户不能同样 OpenToken SYSTEM；portable UI 与 installed service 路径不同可合法。不能简化成同 SID。见[鉴别](<../src/index/index_server_auth.h#L19-L54>)、[认证后发布](<../src/index/index_client.cpp#L107-L119>)。 |
| **A19：共享 CPU 像素 + per-consumer GPU / epoch / interest** | 不同 D2D 设备不能随意共用位图；CPU 数据支持重建；可见 pinning 避免重复解码。软工作集预算不等于硬上限 bug，应先定义/量测压力；有限 transient retry 有依据。见[资源模型](<../src/ui/thumbnail_cache.h#L70-L125>)、[可见预算](<../src/ui/thumbnail_cache.cpp#L213-L229>)。 |
| **A21：简单 UI 与 hook 修正** | hook_requested 真源、消息只唤醒是实质并发修复；局部布局、快速访问重排、隐藏属性变淡不值得造重型抽象。见[hook](<../src/ui/preview_handler_pan.h#L99-L124>)、[重排](<../src/app/places.cpp#L1162-L1190>)。视觉状态仍未实测。 |

## 6. 下一 AI 的最小行动路线

1. **重新核验，不照抄本报告修代码。**读当前 HEAD、目标 diff、调用链与测试；记录问题已修复/仍存在/待确认。本文行号锚定审计版本，后续可能漂移。
2. **先做 A01–A04 的确定性故障测试。**若复现误操作、树外变更或新内容丢失，按实际影响升级 P0。修复只覆盖被证实的契约缺口。
3. **再闭合 A05–A06。**测试半帧、背压、取消先于 I/O 发起、畸形响应；不能只测试空闲 reader 或正常小帧。
4. **处理 A08–A10 的身份/终态，再处理 A07/A11 的响应性能。**保持 generation、排序版本、窗口/连接 epoch 各自职责，不叠更多全局开关。
5. **最后删 A12，做 A13/A14 与低风险整理。**清理测试专用旧实现时迁移真实回归，不为通过删除/弱化断言。
6. **定向验证后停止扩大范围。**仅构建相关目标、运行对应筛选；不默认 full selftest / build_release / 全历史回归。不要把编译成功等同功能通过。

### 最重要的测量/验收指标

- 破坏性操作：replacement/outside/未复制新内容完全保留；拒绝时历史记录和 recovery 状态准确。
- IPC：Stop/Cancel 有界，实际 I/O 完成被回收；OutcomeUnknown 不重放；分配与资源数量有上限。
- 目录：每路径枚举次数、每投影排序次数、复制字节量、UI p95 长帧；不承诺未经测量的加速倍数。
- 平台：按相关能力选择 NTFS/ReFS/FAT/SMB、映射网络盘、云占位符、长路径；不是每次都跑全部组合。

## 7. 未完成与容易误报的边界

- 运行故障、真实 FS/provider sharing/取消行为、回收最后身份读取到执行的对象绑定仍待确认。
- 本次没有批准 GUI 主题/DPI/窄布局表现，也没有认证全部 Windows 8.1 动态能力。
- 存储迁移、安装/便携分发、索引引擎内部、全部预览 provider 未逐行穷尽；不能将局部报告扩展成全仓认证。
- 隔离进程首先是故障/资源边界；没有额外 token/AppContainer 证据，不得夸大为低权限安全沙箱。
- “同功能多路径”只有在语义、权限域、原子性、套间和寿命相同且证据充分时才可合并。

**交接结论：先修历史身份、祖先归属、内容版本与 I/O 生命周期；保留 Windows/COM/Shell 真实约束，再减少重复枚举、UI 排序和退役算法。**

## 8. 历史复核命令

```powershell
git status --short
git rev-parse HEAD
git log '490e114^..185cb83' --reverse --format="%h %s" --shortstat
git diff '490e114^' 185cb83 -- <path>
git show <commit> -- <path>
git show 185cb83:<path>
```

主要归因：`7dc21de` 目标保护/生命周期整合；`cac5f32` 来源 ID、Undo Copy/Create、目录契约；`b8a7604` idle CPU、context session；`185cb83` Shell query/launch、SCM 认证及目录后续修复。原有风险与这些提交新引入的风险必须分开。

## 9. 第三批落实记录：A05–A06

本批已落实并完成下列定向检查。第 3 节保留审阅时的事实与推断；本节记录当前工作区的处理结果，不代表全仓或全部 provider 验收。

| 入口 | 旧做法的缺口 | 当前通用规则 |
|---|---|---|
| 公共管道传输 | 只有单次 pending 等待预算，多次分块可不断续期 | PipeIoOptions 共享绝对 deadline；每次发起前检查持久取消；取消/超时后回收实际完成再释放 OVERLAPPED、事件和缓冲区 |
| Index | Stop 单次 CancelIoEx；Disconnect 先等待阻塞 writer；头与正文间取消可能落空 | 连接同时拥有管道和取消事件；读写保留共享所有权；先发取消信号再等待锁和线程；首字节启动 5 秒整帧预算 |
| Shell | 发送期间占有 pending 锁；Stop/Cancel/Abort 等在阻塞发送后面 | pending 锁只管理注册与终态；取消先打断连接；完整帧仍由发送锁串行化；统一 RetireConnection 退役失效连接；保留各请求原有重放政策 |
| Preview | 只给响应头约 1 秒预算，正文同步读取；Reset 依赖单次 CancelSynchronousIo | 请求、响应正文、属性与映射 ACK 共用 1 秒预算；Reset 置持久停止事件并等待 worker 完成回收；删除旧的同步取消和清理时单次取消调用 |
| Preview 响应消费 | 分散限制若干字符串；映射名、像素组合和总分配未统一准入 | ValidatePreviewResponse 在可变分配前验证固定 schema、请求身份、合法类型/状态、尺寸/stride 和总预算；属性逐项先验证长度再分配；只映射已验证的像素字节数；缓存成本按实际拥有的数据计算 |

Preview producer 仅在确实发布映射名时等待 ACK，避免映射创建失败后等待客户端无法发送的 ACK。正常响应的类型、失败语义、代次和缓存重试政策保留。

实际验证结果：

- 公共 IPC：传输筛选 19 PASS / 0 FAIL；包含传输和原有负载解析的目标 42 PASS / 0 FAIL（包含前述传输检查，不相加）。
- Index：传输生命周期 15 / 0；writer 空闲 19 / 0；帧边界 15 / 0；原有结果路由 21 / 0。
- Shell：背压、半帧、停止缺口及连接退役 31 / 0；原有请求路由 20 / 0；OpsManager Shell 终态筛选 10 / 0。
- Preview：传输/异常 schema 32 / 0；真实本地 helper 响应兼容 11 / 0；文本边界 34 / 0；矢量响应 17 / 0；缓存 45 / 0；失败语义 11 / 0。
- 主程序、受影响 helper 和上述测试目标编译成功；日志保留在 build/batch3-*.log。

验证说明：沙箱启动返回 0xC0000142，改为获准的完整 Windows 环境执行；系统临时目录拒绝创建，实际响应夹具限定在项目 bench_data。一次 Ops 测试筛选参数写错而误启动默认测试，已停止，记录有 36 PASS / 9 FAIL；该中断运行不计作本批验证，也未据此推断失败原因或扩展修复。随后正确的 --filter shell-completion 定向检查为 10 PASS / 0 FAIL，误启动进程没有残留子进程。

范围仍限于本地自建管道和上述真实 helper 内容类型。真实 SMB/RaiDrive、其他第三方预览 provider、GUI 快速切换及完整历史回归未在本批验收。

## 10. 第四批落实记录：A08–A10（2026-10-11）

本批完成名称身份、网络结果 freshness 与请求准入终态的实现及定向验证。第 3、4 节仍保留审阅时的事实；本节说明当前工作区变化，不代表真实 SMB、全部 provider 或完整 UI 验收。

| 问题 | 旧做法的缺口 | 当前通用规则 |
|---|---|---|
| A08 名称身份 | 通知位置表、批量修改合并、保序改名链和选择跟随使用大小写折叠，可能合并 A.txt / a.txt | 普通文件系统通知的物理名称按精确字符串匹配；单条、批量、resolved 通知、队列合并和返回子目录选择使用同一规则；显示排序仍使用原有排序器 |
| A09 结果 freshness | Finish 只验证调度槽，但晚结果仍无条件更新网络状态；枚举失败只保留 bool，UNC 失败一律显示 Offline | 调度槽与每路径 latest ticket 分开；入队即预留 ticket，重试保留原 ID；超时为 provisional，未被新请求替代的最终晚结果仍有效；最终结果清理 freshness 记录 |
| A09 错误语义 | 权限、取消、饱和和未知 provider 失败丢失原因 | 枚举和探测保留原始 Win32 error_code，共用 PathError 分类；仅连接/超时错误显示 Offline；权限/provider 显示具体系统错误，保留 UNC 快照只读；取消探测保留既有连接状态 |
| A10 前台拒绝 | Worker 启动/Refresh 拒绝可留下 loading；旧结果可能覆盖拒绝状态；probe 满额后的队首缺少唤醒 | 普通目录、显式刷新、标签/收藏/最近/回收站统一拒绝终态；列视图也结束为错误。拒绝仍推进 generation watermark，屏蔽之前排队的结果；探测实际所有者释放后发消息唤醒原队列，永久启动错误发布终态 |
| A10 本地执行保障 | 同池所有线程可被 UNC、回收站或后台 provider 调用占满 | 每池保留一个只执行本地枚举的线程，其余线程仍可处理本地和 provider 请求；启动需同时取得至少两个实际名额，保留全局 8 个 live+retired 上限和探测 4 个物理上限 |

删除了 AppState 中独立的 probeUnc/probeQueue，队列、请求 ID 和 freshness 由既有 UncProbeScheduler 统一拥有。没有增加轮询重试定时器，也没有以逻辑超时释放实际 I/O 名额。错误文字复用 FileOperationErrorText。旧共享请求夹具改为在枚举 checkpoint 处门控，继续验证共享代次与取消归属，不再假设后台 I/O 可以占满所有线程。

实际验证结果：

- 名称模型筛选：24 PASS / 0 FAIL / 1 SKIP；通知应用筛选：9 PASS / 0 FAIL。模型修复前为 11 PASS / 11 FAIL。
- 探测调度筛选：8 PASS / 0 FAIL；真实受控探测筛选：22 PASS / 0 FAIL，修复前为 16 PASS / 6 FAIL。
- 网络/准入应用筛选：17 PASS / 0 FAIL。最初 12 项有效修复前用例为 3 PASS / 9 FAIL；最终补充取消、拒绝后旧结果、列视图拒绝及真实资源唤醒。四个真实阻塞探测超时后仍占物理名额，第五个 UI 请求由资源释放消息自动恢复并完成，无需另一次用户请求。
- Worker 生命周期：36 PASS / 0 FAIL；枚举取消：12 PASS / 0 FAIL。覆盖退休所有者、剩一名额时启动拒绝、实际返回恢复、本地枚举保留线程、拒绝仍推进 watermark，以及具体枚举错误码。
- 共享目录请求筛选：10 PASS / 0 FAIL（含 6 项列视图检查）；独立共享列视图筛选：6 PASS / 0 FAIL，后者与前者重叠，不相加。
- 主程序与本批受影响测试目标构建成功；git diff --check 通过。主代理日志为 build/batch4-*.log；没有运行完整 selftest 或完整历史回归。

验证限制与未解决项：

- 隔离目录启用 FileCaseSensitiveInfo 返回 ERROR_ACCESS_DENIED (5)，真实 A.txt/a.txt 同目录端到端用例为 SKIP；精确名称模型、通知应用以及普通目录真实大小写改名已验证，不能据此声称真实敏感目录全链路通过。
- 原有保序模块另记录 35 PASS / 1 FAIL，失败为“reopening the same directory clears held order and rename hints”。复核 HEAD 中 Tab::NavigateTo 同路径提前返回已存在，未将其扩展为本批修复。
- 本地保留线程依赖显式 UNC/回收站/provider 请求分类；映射盘、云目录等使用本地路径的阻塞 provider 尚不能保证隔离。真实 SMB/RaiDrive、第三方 provider、完整视觉验收未执行；原有 presentation-state 筛选只更新了模拟探测 ID，未重跑。

本批后仍待处理 A07、A11、A12、A13、A14、A20、A22 共 7 项；A22 仍是需要先收集错误码/provider 证据的候选项。

## 11. 第五批落实记录：A07 / A11（2026-10-11）

本批将普通目录的事实枚举、排序投影和 UI 发布拆开，并将对话框淡入改为非阻塞生命周期。保留前四批的身份、generation、取消及实际 I/O 所有者约束。

| 问题 | 删除的旧路径 | 当前通用规则 |
|---|---|---|
| A07 通知发布 | UI 复制整个快照、直接 sort，以及纯删除事件立即在 UI 发布的特判 | 全部通知通过既有后台 I/O 队列解析并准备 immutable 投影；同一 source 只应用一次事实变化，同一排序只投影一次；UI 验证 snapshot/path/view/applied/notify generation 和排序设置后交换指针 |
| A07 新建项展示 | RevealPendingCreatedItem 再复制快照并 stable_sort | 展示使用已经发布的投影，只按精确名称选择和滚动；大小写不同的物理名称不会合并 |
| A07 枚举与排序 | path+sort 各自重复枚举，UI 同步重排目录和缓存快照 | 不同排序订阅共享 pending/inflight 的一次目录事实枚举，各自后台生成投影；已加载目录切换排序只重排；仍在刷新的目录切换排序订阅其新事实，不丢掉刷新终态；同排序订阅可借用已有发布代次 |
| A07 freshness / 取消 | 缓存显示、重排与真实枚举完成缺少明确区分 | 缓存投影为同 generation 的 provisional，最终枚举为 final；缓存不结束 pending，不覆盖最终错误，不写磁盘缓存；纯重排不更新身份、Git 和 store freshness；同 key 的新 Refresh 取得新事实；无订阅的 queued producer 和缓存引用及时回收 |
| A11 动画生命周期 | UI WaitForCommitCompletion、DwmFlush、Sleep(8)、GdiFlush、cloak 及同步帧循环 | DComp 淡入使用动画对象，native 淡入使用窗口计时器；调用后继续处理消息；关闭立即隐藏/销毁；窗口销毁、解绑及自然完成共用取消与清理，恢复 alpha/color key/backdrop，仅移除本次临时添加的 layered 样式 |

共享目录组只存在于待执行/执行中的请求，没有新增持久缓存、线程或退休任务链。列视图借用的发布代次必须匹配排序设置；显式刷新归属随排序订阅转移。通知准备失败或发布校验失败仍保留旧行并请求重新枚举。

实际验证结果：

- Worker 投影筛选：51 PASS / 0 FAIL。覆盖不同排序只枚举一次、同 key 新刷新取得新 facts、排序期间保留真实刷新、同 key Project 借代次、取消/替换的排队 producer 与缓存回收，以及 provisional 不写 UNC 磁盘缓存。
- 目录投影应用筛选：14 PASS / 0 FAIL；共享目录/列视图请求：10 PASS / 0 FAIL；网络/准入终态：17 PASS / 0 FAIL。覆盖双窗格同时纯排序均完成、独立选择、显式刷新归属转移、错误排序结果拒绝、缓存和最终错误终态。
- 通知大目录投影筛选：6 PASS / 0 FAIL。50,000 行、两个 pane、7 次后台准备/前台发布，中位数 16.643 ms，p95（本组最大值）24.325 ms；无发布前修改源快照，排序设置变化整批拒绝。通知应用模块：27 PASS / 0 FAIL；通知名称筛选：9 PASS / 0 FAIL。
- 目录模型契约：14 PASS / 0 FAIL；名称模型：24 PASS / 0 FAIL / 1 SKIP。
- 对话框关闭：13 PASS / 0 FAIL；淡入生命周期：89 PASS / 0 FAIL。清理不覆盖动画期间其他样式修改的两项用例，修复前为 2 FAIL，修复后通过。主程序、上述测试目标及四个直接依赖动画实现的展示/控件目标构建通过，无新增编译警告。
- git diff --check 通过。主代理日志为 build/batch5-*.log，动画日志为 bench_data/dialog_fade/lifecycle-test.log 和 close-test.log；未运行完整 selftest 或完整历史回归。

验证限制：5 万行的计时只覆盖当前机器的受控通知发布，没有修复前同条件计时，不能据此声称整体性能提升比例；UI 仍有列表计数、选择跟随和列视图索引处理，最慢一次仍超过 16 ms。没有执行真实 SMB/RaiDrive、第三方 provider、完整桌面动画录像或完整视觉验收。动画当前开启且非高对比度，禁动画/高对比度分支保留但没有通过切换系统设置动态验证。真实大小写敏感目录仍因 FileCaseSensitiveInfo 返回错误 5 而 SKIP。首次模型夹具在系统临时目录创建失败（错误 5），改用仓库内隔离夹具目录后相关筛选通过。

代码量说明：第五批开始时全部脏树相对 HEAD（含未跟踪源文件）的净变化为生产代码 +896 行、测试 +3036 行、文档/构建配置 -476 行，合计 +3456 行；这是当时整个工作区的统计，不能全部归因于前四批，也不是第五批完成后的现值。测试是主要增长来源，生产代码也确实增加。A11 本批生产 +129/-98，净增 31 行，新增部分承担跨消息状态和资源清理。通用规则减少了重复算法和运行路径，并不自动保证总代码行数下降；不能将全部增长都解释为必要。

A12 尚有约 150 行旧 KeepEntryOrder / ApplyDirNotify / ApplyDirNotifyBatch 实现及失效状态字段仅靠测试保活。下一批应先将有意义的回归迁移到生产入口，再删除这些实现、参数和写 false 的状态；FollowEntryRenames / PruneEntryRenames / FillDirEntry 仍有生产用途，保留。

本批后待处理 A12、A13、A14、A20、A22 共 5 项；A22 仍需要先收集错误码/provider 证据。A07 的全量 UI 处理并未完全消失，当前完成的是本节列出的事实/投影收口，剩余发布成本以实际计时记录。


## 12. 第六批落实记录：A12 退役实现清理（2026-10-11）

本批确认旧实现无生产调用后删除，保留名称身份、选择跟随和通知失败原子性的生产规则。统计以本批开始时的文件为基线，保留此前未提交修改。

- 删除 KeepEntryOrder、旧 ApplyDirNotify / ApplyDirNotifyBatch 及其 keep_order 参数；同时删除仅供旧入口使用的 FindName / InsertSorted 和排序依赖。通知只保留 ResolveDirNotifies / ApplyResolvedDirNotifies，排序仍由后台 ProjectDirectorySnapshot 准备。
- 删除 Tab::refresh_keeps_order / order_held 及写 false 的路径。ClearEntryOrderHold 改名 ClearEntryRenameHints，只负责仍有生产用途的改名提示；FollowEntryRenames / PruneEntryRenames / FillDirEntry 保留。
- 将模型与旧自测迁移到生产事实解析/投影入口，不增加兼容包装。旧算法互比改为具体物理名称、大小、排序和原快照不变的断言；Name/Size/Mtime/Type 的升降序均有独立预期结果。删除只为旧保序开关重复执行的组合。
- 旧默认模型“同路径重开清空提示”的历史失败迁移为当前导航契约：同路径是 no-op 并保留未完成提示，不同目录导航才清理。新增不同目录的清理断言；不通过删除该场景获得通过。

本批生产代码净减少 197 行；bench 测试净增加 25 行，旧 selftest 测试净增加 67 行，测试合计净增加 92 行。源代码合计净减少 105 行（不含报告）。统计依据为 build/batch6-line-counts.json，不是整个脏树相对 HEAD 的差异。

实际验证结果：

- 目录模型 35 PASS / 0 FAIL；目录契约筛选 10 PASS / 0 FAIL；名称筛选 22 PASS / 0 FAIL / 1 SKIP；面包屑导航 58 PASS / 0 FAIL。
- 真实通知应用模块 27 PASS / 0 FAIL；通知名称筛选 9 PASS / 0 FAIL；目录投影应用 14 PASS / 0 FAIL；共享目录/列视图 10 PASS / 0 FAIL。
- 应用夹具 --listing-races-only：16 PASS / 0 FAIL；--created-only：53 PASS / 0 FAIL。四张 100%/150% DPI、浅色/深色图片渲染和保存检查通过，未人工审阅。夹具手工注入 worker 结果与通知 mailbox，未启动真实 worker/watch，不能据此证明真实线程竞争通过。
- 本批没有运行应用夹具的 transfer/title 和 ArchitectureCases；其中旧的同步纯删除假设仍需另行迁移，未声称无筛选应用夹具通过。
- 主程序和上述相关目标构建通过，无新增编译警告；源码中无旧 API、旧字段或旧清理方法引用，git diff --check 通过。主代理日志为 build/batch6-*.log。

旧 selftest_1b2.cpp 已通过 MSVC /Zs 的完整语法检查；当前 PULSE_WITH_SELFTEST=OFF，未执行这份旧自测的功能断言。真实大小写敏感目录仍因权限错误 5 为 SKIP；没有扩大到完整历史回归、真实 SMB/RaiDrive 或完整 UI 验收。

本批后待处理 A13、A14、A20、A22 共 4 项；A22 仍需先收集错误码/provider 证据。

## 13. 第七批落实记录：A14 磁盘缓存后台发布（2026-10-11）

本批移除导航和 UNC 预热中的同步 LoadNetSnapshot。两处 UI 路径只提交已有 WorkerPool 请求；读盘、文件校验、字符串解析和排序投影均由后台 worker 完成。

- 复用现有 provisional / final 发布合同。共享目录请求优先使用当前订阅者已有的内存事实；没有内存事实时，后台读取一次磁盘事实，为各排序订阅者准备投影，不增加线程池、第三份缓存或独立请求模型。
- 可选缓存不存在、损坏或读取抛错均不阻止正常枚举。缓存结果保留同一 pending generation；最终枚举按原成功/失败入口终结，缓存不提前写为新鲜 SnapshotStore。
- 缓存时间随 WorkResult 发布，UI 只有通过 path、pending/applied generation、排序列、方向和文件夹排序模式校验后才显示已有 Cache / Refreshing 提示。最终成功清除缓存时间与提示，最终失败保留缓存列表和错误终态；迟到缓存不能覆盖终态。
- UNC 预热仍提交实际枚举，成功结果进入既有共享 store；实际导航可以独立后台载入磁盘缓存，预热不再在 UI 上把旧磁盘事实注入共享 store。
- 列视图祖先/子列复用 NoteColumnStripResult 接收同一临时结果，要求精确匹配待完成代次，保留该代次直到 final；最终完成后不再接受迟到缓存。列的 rows 仍按所属 tab 的规则构建，没有另加 UNC 分支。

以本批开始时为基线，生产代码净增加 11 行（navigation -8、worker +17、WorkResult +1、列发布 +1）；测试代码净增加 326 行，其中隔离缓存文件/竞态夹具 227 行，worker 测试入口 +9 行，网络与列状态断言净 +90 行。源码合计净增加 337 行，不含本报告；CMake 仅在原测试目标宏定义行增加 PULSE_NET_CACHE_TESTING=1，净行数不变。统计证据为 build/batch7-line-counts.json，保留此前脏树，不能当作整个工作区相对 HEAD 的增量。

实际验证：

- pulse、pulse_fs_worker_cancel_test、pulse_directory_contract_application_test 构建通过；补齐测试的 localization.h 依赖后最终构建无新增编译警告。列发布补充后只重建 pulse 和目录应用测试目标。
- pulse_fs_worker_cancel_test --net-cache：33 PASS / 0 FAIL；真实隔离本地 cache 文件、后台线程、共享内存优先、缺失/损坏/异常、读盘期间取消和同键替换均有定向断言。
- pulse_fs_worker_cancel_test --projection：51 PASS / 0 FAIL。
- pulse_directory_contract_application_test --network-state-only：最终 29 PASS / 0 FAIL，包含主列表与列共享缓存、时间提示、结果匹配和最终完成后的迟到缓存拒绝。
- pulse_directory_contract_application_test --directory-projection-only：14 PASS / 0 FAIL；--shared-requests-only：10 PASS / 0 FAIL。
- src/app 中 LoadNetSnapshot 只剩 worker 调用；git diff --check 通过。日志为 build/batch7-*.log。

缓存读取测试使用 bench_data 下的隔离文件，UNC 枚举在起点注入已知错误以避免访问真实 provider；这证明本地缓存读写及后台发布合同，不证明真实 SMB/RaiDrive 延迟。UI 状态检查手工投递 WorkResult，未作截图或完整 UI 验收，也未运行完整历史回归。磁盘解析本身仍是 worker 内同步操作，取消会阻止发布，不保证即时停止读盘。相关用例通过后停止验证。

本批后待处理 A13、A20、A22 共 3 项；A22 仍需先收集错误码/provider 证据，再决定是否收窄 fallback。

## 14. 收尾落实记录：A13 / A20 / A22（2026-10-11）

按用户“最后的全部做完”的授权，本轮连续完成剩余三个审阅项，不再分批等待确认。此前第 9–13 节的“待处理”数量是各批完成时的历史状态；当前分批计划中的修复项已全部落实，真实 provider 和完整 UI 验收仍有下述边界。

| 项目 | 原实现的问题 | 本轮统一规则 |
|---|---|---|
| A13 拖拽元数据 | HeaderDropHint 在每次 hover 同步查属性；最终目录判断也在 UI 查询，快捷方式另用 detached thread | 同一拖入 sources 只向已有 WorkerPool 提交一次元数据查询；UI 使用廉价导航候选，完成后按 hint generation 刷新当前拖拽上下文；最终 Drop 单独排队重新验证目录/快捷方式，仍检查 pane 寿命、slot、path 和 view generation |
| A20 文本度量路由 | 全局 Luma 开启时，文件名先建立组合缓存 key，随后又被 format 规则拒绝 Luma | 绘制与度量共用 UsesLumaText；进入缓存前确定路由，文件名直接复用 DWrite advance 缓存；删除 key 中重复查询开关的位，不增加 factory/strategy |
| A22 枚举重试 | NT 枚举 runtime_error 普遍触发 Win32 二次枚举，实际失败与能力差异混在一起 | 保留数值 NTSTATUS/Win32 错误和查询阶段，只对 NT API 缺失、查询明确 NOT_IMPLEMENTED / NOT_SUPPORTED 兼容重试；其他错误保留原诊断并终止，失败清空部分结果，双后端失败保留双诊断 |

A13 只跨线程传 owned 路径和 weak 状态，不传 IDataObject。DragOver 保存最新坐标、修饰键、允许效果与来源偏好；后台完成复用这些上下文，DragLeave 后不能重放。hover 与已接受 Drop 的 generation 分开，清理 hover 不取消最终操作；提交被拒绝时释放 hint sources，后续 hover 可以重新提交。尚未确认元数据时只提供 COPY/LINK 导航候选，不授权 MOVE；最终 Drop 重新判断，复制回退仍必须满足原接受效果与来源允许效果。实际目录查询使用后台同步 GetFileAttributesW，generation 取消阻止过期发布并在逐路径查询前检查，不能中断已经进入的 OS 调用。原最终快捷方式 detached thread 已改用既有 WorkerPool。

A22 从“待补证据候选”落实为窄兼容规则。Microsoft 的 MS-SMB2 QUERY_DIRECTORY 语义区分不支持的信息类与非法信息类、参数、权限失败；MS-ERREF 定义 NOT_IMPLEMENTED / NOT_SUPPORTED，RtlNtStatusToDosError 提供数值转换。打开阶段即使返回这两个状态也不重试；CreateEvent 失败、拒绝访问、路径缺失、参数/信息类错误、截断/缓冲区不足、无效设备请求和未知状态均不再自动换后端。官方定义与注入用例支持此策略，尚不能证明所有真实 provider 都仅用这些状态报告能力差异，因此保留 Win32 后端并保留原始诊断。

代码量按各任务接手时的脏树基线统计：

- 生产编译保留的代码净增加 101 行：A13 +63、A20 +2、A22 +36。A13 的 app_input.cpp 净减 20 行，新增独立 app_drag_hint.cpp 60 行；其余为提示状态与已有拖拽入口的复用。没有增加线程池、持久元数据缓存或后端策略框架。
- 测试专用 seam 净增加 40 行，仅在 PULSE_FS_ENUM_TESTING 下编译。
- 测试用例、入口和目标配置净增加 464 行：A13 +311、A20 +34、A22 +119。源码合计净增加 605 行，不含本报告。统计凭据为 build/final-cleanup-line-counts.json，A22 原始基线及宏区拆分为 build/a22-line-count-evidence/line-count-manifest.json。不能把整个工作区相对 HEAD 的 diff 当作本轮增量。

本轮并非以行数下降为验收条件：A20 减少重复路由，A22 删除盲目重试，A13 则补齐移走 UI I/O 后必须具备的异步归属、取消与最终验证合同。新增行数主要来自可复现回归（测试与 seam 共 504 行）；生产部分的增加不代表又增加了同功能补丁分支。

实际验证（全部进程退出码 0）：

- pulse 和受影响的六个测试目标构建通过；两份最终构建日志无 MSVC 警告或错误。日志：build/final-cleanup-build.log、build/final-drag-build.log。
- pulse_directory_contract_application_test --drag-hints-only：33 PASS / 0 FAIL。真实本地隔离目录、后台 worker、重复 hover、取消后排空、新 sources/reset、拒绝提交后重试、普通文件和 shortcut 候选均有断言；最终目录被删除时拒绝导航并显示失败 UI，LINK 不触发复制回退。
- pulse_drag_drop_test --target-session-only：8 PASS / 0 FAIL。owned FileDataObject 与隐藏窗口覆盖最新上下文刷新、DragLeave、最终重新提取 sources/preference 与 performed effect。
- pulse_typography_cache_test：17 PASS / 0 FAIL。真实 DWrite 与确定性 Luma 替身，覆盖文件名复用 advance 缓存、CJK/空白/空文本/长文本及既有缓存边界。
- pulse_fs_enumeration_test --fallback-only：21 PASS / 0 FAIL / 1 SKIP；--cancel-only：6 PASS / 0 FAIL。本地缺祖先路径记录 NtCreateFile status=0xc000003a / error=3，本地文件作为目录记录 status=0xc0000103 / error=267，两者都没有重试且输出为空。其余能力/失败状态用注入检查，部分结果清理、兼容重启和双后端诊断通过；SKIP 明确表示缺少真实 SMB/RaiDrive fixture。
- 直接依赖检查：pulse_fs_worker_cancel_test 默认 12 PASS / 0 FAIL、--projection 51 PASS / 0 FAIL；pulse_header_drop_effect_test 4 PASS / 0 FAIL。
- 合计 152 PASS / 0 FAIL / 1 SKIP。没有运行完整 selftest、默认全部枚举测试或完整历史回归。日志为 build/a13-*.log 和 build/final-*.log；隔离拖拽/枚举 fixture 已清理。
- src/app 的生产 FirstDroppableFolder 调用只剩后台提示和最终验证两处；旧自测调用仍保留。旧 QueueHeaderShortcutDrop 已无引用；git diff --check 通过。

验证限制：未执行真实 SMB/RaiDrive 延迟或 provider 兼容测量、有效 .lnk 解析与第三方外部拖入源验收；没有真实 Luma 渲染、性能测量或主题/DPI/窄布局截图。隐藏窗口与本地文件测试证明本轮状态/生命周期合同，不能替代这些真实环境验收，也不证明仓库其他产品问题全部归零。相关检查通过后停止扩大范围，保留已有未提交改动，未提交或发布。

## 15. 便携测试包交付与冗余复查（2026-10-11）

本次用户要求在清理冗余后提供便携包。分批修复计划已完成，交付前仅复查新增路径与退役 API，不扩大到无关重构。

- 删除 ShellClient 的非拥有 pipe_ 镜像和两处同步赋值；真实管道只由既有 Connection 持有。测试改查 CurrentConnection，不再依赖第二份状态，保留所有断言。
- 删除 BuildNotifyProjections 中重复的 request.projections.clear() 及多余块：结果在入口已清空，局部 projections 仅在整批成功后提交，失败直接返回即可。
- 以本次清理开始时的脏树为基线，生产代码净减 6 行，测试净减 1 行。退役 ApplyDirNotifies / RetainEntryOrder / ApplyHeldEntryOrder / ClearHeldOrderState 无引用；保留有效回归、Win32 兼容枚举后端和已有未提交修改。

打包保持 version.txt 的 1.1.0.1，Release / PULSE_WITH_SELFTEST=OFF / PULSE_WITH_LUMATEXT=ON。只构建包所需的五个生产目标及三个清理相关测试目标。实际验证：Shell routing 20 PASS、transport liveness 31 PASS、operation shell completion 10 PASS、notify projection 6 PASS，合计 67 PASS / 0 FAIL；各进程退出码 0。构建无新增 MSVC 警告或错误，git diff --check 通过。未重跑已完成批次或完整历史回归。

交付包：dist/Pulse-1.1.0.1-portable-win-x64-test-20261011-093312.zip，11,604,074 字节，SHA-256 D79014414D87905CB047C7122D39A6CE7E8D8FCC54CF58CC97436BCB6CF49828。原同版本便携 ZIP 已恢复并核对原哈希，既有 dist 目录/清单保留；测试包使用独立日期标识。

- 40 个 payload 文件全部与 ZIP 路径、长度和 SHA-256 匹配；五个 EXE 的 FileVersion / ProductVersion 为 1.1.0.1，OriginalFilename 匹配。程序、DLL、7zip、许可证齐全，无测试可执行文件、日志、obj/pdb/lib/CMake/bench_data；没有内嵌旧 selftest dispatcher。
- 对五个 EXE、两个主 DLL 和 7zip 的 EXE/DLL 运行 dumpbin /dependents；没有未随包携带的 VCRUNTIME/MSVCP/CONCRT 依赖。动态加载 provider 和服务运行不在此静态检查范围。
- 使用包内程序、隔离 profile 和测试目录进行隐藏窗口启动检查，进程正常退出 0。生成截图没有可用渲染画面，因此仅记录启动/退出结果，不判定视觉通过，也不推断产品或环境根因。
- 审计凭据：dist 下同名 .manifest.json、.cleanup.json；构建、测试、依赖和打包日志为 build/portable-*.log。独立测试说明为 docs/portable_test_20261011.md，dist 下另有同名测试包的 -testing.md 副本。

人工测试重点：专用目录中的复制/移动/改名及撤销（包括同名替代文件拒绝）、真实 RaiDrive/SMB 的刷新/断线/快速切换、目录与有效 .lnk/第三方压缩软件拖入、双窗格大目录的独立排序/选择和通知更新、预览/右键/退出，以及主题/DPI/窄窗格的文本和对话框表现。真实 provider 与完整 UI 验收尚待人工测试；便携版仍使用用户应用数据目录，不会因解压自动注册索引服务。未提交或发布。

## 16. 人工反馈修复（2026-10-11）

本轮按用户确认的范围修复权限反馈、精确回收还原、右键菜单、打开方式、撤销本地化和窄窗口标题栏。用户已撤回标题拖入禁止与“仅打开菜单就自行切回 Explorer”两项；失活处理针对切换到其他程序后菜单残留。没有为 C 盘、Explorer、中文菜单标题或临时 Shell 命令编号添加特殊分支。

| 反馈 | 之前的问题 | 当前规则 |
|---|---|---|
| 权限错误误报占用 | 访问拒绝既用于探测 RM，也被当作明确锁错误；无 owner 的复制/移动仍显示占用处理 | 分开可探测错误、明确共享冲突和真实 owner 证据；访问拒绝无 owner 时显示原权限诊断，明确共享冲突保留无 owner 重试 |
| 同原路径多个回收版本 | 手动还原只提交原路径，丢失选中 $R；第一个还原后其余遇到已存在目标 | 提交原路径与精确 $R 配对；验证并固定 $R/$I 身份，复用 UniqueCopyPath 和 DestinationGuard::RenameHandle，以原子不覆盖方式保留两者；逐项记录实际目标和失败，成功部分仍发布刷新结果 |
| 菜单抖动和快速展开消失 | 重绘强制完全不透明，后续动画又退回中间态；子菜单锚点未跟随 | 全部事件重绘共用当前透明度/偏移，子菜单跟随主菜单位置；异步刷新保留稳定内置行及已打开子菜单 |
| 新建/加载/顺序/图标 | Pulse 三项与原生 New 分散，未就绪命令看似失效，Rename 图标走不同资源 | 背景与工具栏共用一个 New 组合，内置三项在前，追加原生模板；按 NewMenu CLSID 和规范动词去除文件夹/.txt重复；加载行明确表示等待，未验证缓存命令仍禁用；Rename 共用工具栏 SVG |
| 切出后菜单残留 | 主窗口失活未取消菜单，发送式消息未走 TrackPopup 的取消息路径 | owner/menu/edit 内部焦点转移保持菜单；应用失活、外部激活和取消立即隐藏主/子菜单，清除尚未执行的结果；正常选中仍返回所选命令 |
| 打开方式只有仅一次 | 优先调用 SHOpenWithDialog，限制了现代系统选择器的关联选项 | 保留一个 openas 菜单入口，经公开 Shell 类选择指定系统 Unknown 类的 openas handler，交由 Windows 同窗提供始终/仅一次；删除拆分入口及默认设置 URI 的临时实现 |
| 英文撤销夹带中文 | UndoLabel/Describe 的动词及多项格式硬编码中文 | 使用同一 localization 资源翻译动作、完整 Undo 格式与多项说明，切换语言时完整一致 |
| 窄窗口标题栏重叠 | 标签最小宽度超过预算，命中又先于 caption | 固定 caption/新建预算，标签共用裁剪视口及左右箭头/滚轮滚动；活动标签自动显露，caption 优先命中；分组、固定和拖动使用同一边界 |

回收还原不静默覆盖既有文件，不去重相同原路径，也不移除 Undo 的原目标为空保护；只把实际成功项及目标发布为完成结果。失败包含原路径、回收 payload 与数值错误。取消保存已完成结果并为未尝试项目记录取消。$I 元数据不绕过删除授权作不可逆删除，失去 $R 的孤立索引由枚举忽略。

定向验证结果：

| 检查 | 结果 |
|---|---|
| ops_feedback --permission-feedback | 10 PASS / 0 FAIL；隔离目录DACL拒绝文件创建，真实Copy返回0x80070005且不缓存锁重试；独占来源对照返回0x80070020，关闭句柄后普通重试成功；原DACL已恢复 |
| ops_feedback --manual-restore | 15 PASS / 0 FAIL；隔离双版本、已有目标、原子不覆盖、部分成功、目录名含点、身份替换、取消及锁分类 |
| ops_feedback --operation-labels | 40 PASS / 0 FAIL；10种动作的中英/单多项文案 |
| file_lock_owner_test | 16 PASS / 0 FAIL；分类与真实 fixture holder/RM/安全重试 |
| locked_operation --decisions | 18 PASS / 0 FAIL；保留计划、完成过滤、取消/代际拒绝及真实目标共享冲突 |
| app_controllers --context-menu-refresh / --context-menu-com | 12 + 11 PASS / 0 FAIL；组合、排序、加载、单一openas及当前会话绑定 |
| menu_text --command-state / --live-refresh / --lifecycle | 3 + 3 + 17 PASS / 0 FAIL；稳定状态更新、动画/子菜单跟随和失活/取消拒绝旧结果 |
| titlebar_layout | 1169 PASS / 0 FAIL；530几何断言分别在无compositor/有compositor两种状态执行，36张快照及对应渲染检查；不是1169个独立场景 |
| menu_text 图标快照 | 50 PASS / 0 FAIL；生成6张100/125/150% DPI深浅主题快照 |

UI截图保存在 build/feedback-ui-evidence/bench_data/，已查看窄窗口100%浅色、150%深色及菜单Rename两种主题证据，无黑图、标签覆盖caption或图标异常。标题栏fixture之前复用旧对象链接时的访问异常，在统一源码重编后未复现；不把此前失败记为通过。

初始构建目录产物带Low完整性标签，RM返回29。复测使用正常临时目录中的SHA256一致副本，RM返回0并通过真实holder断言，没有修改系统权限。锁决策fixture原先缺少现有生产规则所需的source_identity，现已用生产FileObjectReference::Open捕获身份，并增强替换前身份断言；没有放宽生产校验。手动还原的第一次原子改名测试失败后，已删除新造的rename缓冲并复用既有DestinationGuard helper，保留并增强目标内容保护断言后通过。

打开方式自动探针通过系统Unknown类的openas注册入口发起选择器，没有选择应用或改默认关联；探针截图捕获到了中间占位窗口，不能作为最终对话框视觉通过证据。随后用户现场确认系统窗口正确显示“始终”和“仅一次”。默认关联实际持久化、不同Windows版本/文件类型及第三方Shell模板仍需便携包验收；真实回收站/provider未作破坏性自动测试。没有执行完整selftest或全部历史回归。

本轮交付：dist/Pulse-1.1.0.1-portable-win-x64-test-20261011-115633.zip（版本1.1.0.1），SHA256 5ED1F71E6CEE2FC1DA81EEC61F8BC65477EDF0E4535068596D5EDCD549E931F2。40个payload文件逐ZIP entry核对长度/哈希，并核对五个EXE版本、OriginalFilename、开发产物排除及九个运行文件依赖；无未携带的VCRUNTIME/MSVCP/CONCRT依赖。原正式ZIP已恢复并复核原哈希。最新主程序/Shell/定向目标构建通过，最终构建日志无warning/error；日志build/feedback-final-build.log、build/feedback-delivery-build.log。完整逐项验证清单与边界已加入同名manifest.json，人工步骤见docs/portable_test_20261011.md。git diff --check通过。
## 17. 文件操作按需提权（2026-10-11）

用户确认开始补齐权限操作后，本轮实现一次性管理员工作进程。资格来自真实权限 HRESULT（访问拒绝、缺少特权、需要提权），不按盘符或路径名称判断；明确占用及有占用者证据的失败仍走既有占用流程。普通尝试失败后，确认窗口说明失败项、来源、目标和 UAC，默认取消；同一失败任务与 generation 只能确认一次，取消或过期不派发。

- 复制/移动保留未完成计划、成功根、源文件历史引用和目标目录快照；管理员工作进程复用 RunTransfer，既有产物不重复复制。冲突、进度、暂停和取消回到原窗口。
- 单项/批量重命名保留源身份，实际修改句柄再次验证。重试与还原提交复用 DestinationGuard 的父目录身份核对和相对父句柄原子提交，祖先继续保护。
- 新建文件夹/文本文档、删除、回收还原与卷标重命名接入同一权限失败规则。删除续传只提交未完成物理根及其历史身份；清空回收站在第一次 admission 后冻结具体根，不重新枚举新增项目。相关撤销保留历史身份、原始删除来源和父进程历史预约。
- 手动回收还原与 Undo 还原共用精确 $R/$I 引擎；手动冲突保留两者，Undo 原目标占用则拒绝，不改变撤销目的地，不隐式删除 $I。
- 同目录 Pulse.exe 以 runas 启动 --file-operation-worker 模式，在正常应用初始化前进入一次性工作流程；没有新增发布 EXE，也不让整个 Pulse 长期以管理员身份运行。
- 本地随机命名管道核对双方 PID、创建时间、用户 SID 和规范映像路径，拒绝远程连接，限制消息大小。请求、进度、控制及终态经过 typed codec 验证；累计完成结果只导入一次，撤销产出身份来自真实引擎。
- 丢失终态时明确显示结果未知，保留已证明的完成结果和恢复证据。恢复日志增加 outcome_unknown 标记，未知的重命名/新建/还原等操作以及未取得终态的活动提权任务均禁止重放。

实际定向验证（各正常运行进程退出码 0）：

| 检查 | PASS / FAIL | 范围 |
|---|---:|---|
| 提权通信 | 16 / 0 | 终态、累计进度、控制回复、取消、死亡/断连、限长和认证拒绝 |
| 操作适配器 | 31 / 0 | 真实复制/移动/改名/批量改名/新建/永久删除，冲突确认、相关撤销、真实部分复制续传、$R/$I 还原与替换拒绝、未知恢复日志禁止重放 |
| 权限反馈 | 17 / 0 | 隔离目录真实 DACL 拒绝、独占文件对照、资格、一次性确认、过期和取消 |
| 精确还原 | 17 / 0 | 同原路径多版本、部分成功、目录冲突、历史 $R/$I 变化、Undo 不覆盖、取消 |
| 确认窗口 | 218 / 0 | 中文/英文、浅色 96 DPI、深色 144 DPI 窄窗口、默认取消/关闭/过期/退出/重复确认；24 张截图 |

合计 299 PASS / 0 FAIL。UI 夹具使用生产强调色初始化；复核了中文浅色、英文浅色、深色窄窗口截图。相关源码检查和构建没有新增 MSVC 警告；未执行完整历史回归。回收还原与未知终态保护修改后，仅重跑直接受影响的适配器、权限和还原检查，没有重跑无关批次。日志为 build/elevation-*-test.log、build/elevation-final-build-2.log，截图位于 build/elevation-ui-evidence/bench_data/permission_prompt_ui。

真实 UAC：在管理员可写、普通令牌不可写的独立临时夹具中已弹出 Windows UAC。用户明确表示拒绝；父进程记录 Cancelled（failure_hr=S_OK）。成功预期夹具因此有两项 FAIL，原 ACL 恢复及临时目录清理检查通过。这是拒绝授权路径的现场证据，不能当成成功高权限复制，也不据此判断产品回归；原日志 build/elevation-uac-test.log 保留。随后补充 fixture 对 UAC_CANCELLED/UAC_COMPLETED 的独立断言和结果标识，未再次触发 UAC，新增取消断言也未宣称已运行。

验证边界：同完整性 worker 测试证明续传和结果协议，不能替代成功 UAC 与实际 C 盘保护目录验收。本轮未实际操作用户回收站、修改卷标或运行跨卷/真实网络 provider 的提权场景。目前认证限制同一用户账户的 UAC 提权，输入另一管理员账户凭据会被 SID 校验拒绝；管理员权限也不能保证解决显式拒绝 ACL、占用或 provider 限制。

本轮交付测试包：dist/Pulse-1.1.0.1-portable-win-x64-test-20261011-130709.zip，11,683,879 字节，SHA-256 14140CA08DC80AEA3F21313F93A527A9D29982D8A70745F287F5DDEBEF4EB93E。版本保持 1.1.0.1；40 个 payload 文件逐项与 ZIP 长度、路径和 SHA-256 匹配，五个 EXE 版本及 OriginalFilename 匹配，未携带测试 EXE、开发产物或 selftest dispatcher。原正式便携 ZIP 已恢复并核对原哈希，旧测试包保留。独立 manifest 和 -testing.md 位于 dist；未提交、未安装服务或发布。

## 18. 权限与链接反馈修订（2026-10-11）

本轮只处理用户反馈的权限窗口、删除分类和目录链接确认，保留此前批次及其他未提交修改。

- 权限窗口去掉“失败路径”，只显示实际操作的源路径和目标路径。标签使用无背景行，路径保留主题输入框背景；删除只有源路径，新建只有目标路径。路径来自当前 generation 的管理员重试请求和实际计划项，不再使用短的进度标签推断端点；重试或取消退役请求时同步清除详情。
- 占用者报告在公共出口按 PID 排除当前 Pulse 进程，分类、重试和界面使用同一报告。其他 Pulse 实例仍保留，明确的共享/锁错误仍按占用处理，不因过滤自身变成权限错误。真实权限失败继续按既有 HRESULT 规则决定提权资格，没有按 C 盘特判。
- 链接确认删除“全部继续”复选框，标明“链接路径”和“实际路径”；继续接受本次操作中的所有链接目标，下一任务仍需重新确认。继续后仍重新固定和验证目标身份，链接被改指向不能借此绕过检查。硬链接没有解析路径时仍标为“目标路径”。
- 清理退役的失败路径及全部继续资源标识，更新中文和英文说明。普通冲突的批量应用规则不受影响。

实际定向验证（正常 Windows 环境，进程退出码均为 0）：

| 检查 | PASS / FAIL | 本轮范围 |
|---|---:|---|
| 占用者证据 | 20 / 0 | 仅当前进程的句柄不产生占用者证据；外部独占者和同名 Pulse 实例仍被报告；明确共享失败仍属于占用 |
| 权限反馈 | 24 / 0 | 完整缓存路径与进度标签分离；隔离 DACL 下真实删除权限拒绝；源保留、无占用者、可提权、取消不入队 |
| 链接批量继续 | 6 / 0 | 两个独立 junction 只确认一次；下一任务重新确认；取消不写入；确认期间改指向仍拒绝 |
| 权限窗口 | 353 / 0 | 中文/英文，浅色 96 DPI、深色 144 DPI 窄窗口；复制/删除/新建详情，取消/过期/重入/退出与按钮边界 |
| 链接窗口 | 6 / 0 | 中文深浅主题、英文、144 DPI 中英文窄窗口及默认 Enter 取消 |

合计 409 PASS / 0 FAIL。真实删除夹具记录 phase=8、hr=0x80270021、lock_retry=0、owners=0、elevation=1；夹具恢复文件和父目录 ACL，仅使用隔离测试文件。目视复核了权限窗口中英文浅色、中文深色窄窗口删除，以及链接浅色和深色窄窗口截图：标签无框、路径有框，未见裁切或按钮重叠。长路径仍按现有路径省略规则显示。

受影响目标 pulse、pulse_ops_feedback_test、pulse_ops_test、pulse_file_lock_owner_test、pulse_permission_operation_prompt_ui_test、pulse_link_confirmation_ui_test 构建成功，日志未发现 MSVC warning/error；未执行完整历史回归。日志为 build/feedback-refine-*-test.log，UI 截图位于 build/feedback-refine-ui-evidence/bench_data。初次删除夹具遗漏 Shell 生命周期和有效 owner 窗口、初次权限 UI 断言误要求两行内容一定滚动；修正夹具后对应检查通过，初次失败日志留档，未据此改动产品分类或放宽按钮边界检查。

验证边界：本轮未再次弹出 UAC，未操作用户实际 C 盘保护文件或回收站。成功授权后的真实高权限写入仍需现场验收；此前用户拒绝 UAC 的结果不能当作成功写入证据。

本轮新测试包：dist/Pulse-1.1.0.1-portable-win-x64-test-20261011-140507.zip，11,684,766 字节，SHA-256 99E4A14F38F2B6543444F44542EB33E0224F850933D0E9CACDB07C8BC1A459C6。版本保持 1.1.0.1；40 个 payload 文件逐项通过 ZIP 路径、长度及 SHA-256 核对，五个 EXE 的版本和 OriginalFilename 匹配，无测试程序或开发产物。打包恢复了原正式 ZIP 并核对哈希；旧测试包保留。独立 manifest 和 -testing.md 同存于 dist；未提交、未发布。

## 19. 新建菜单预加载与固定结构（2026-10-11）

用户反馈右键菜单和功能栏“新建”每次显示“加载中…”后删除该行，造成菜单持续变化。本轮统一现有 ContextMenuController 的规则，没有新建缓存服务或增加定时器。

- 两个入口均移除加载占位行；OpenMenu 固定首次 COM 展示快照，包括空快照。后台部分/完整结果只能重新绑定已有项目和启用状态，不插入新模板或移除旧行。冷加载时本次显示已准备好的项目，新增原生模板在下次打开时展示。
- 静态注册表项在菜单打开期间也保持原布局及命令索引；新结果写入已有缓存，下次 OpenMenu 应用。防止结构固定后，点击旧索引却执行新注册表项目。
- 复用既有主窗口 timer，在前台空白区域及功能栏上下文提前准备当前目录菜单。打开菜单前消费已排队的 Shell 回复。关闭或选择 Pulse 内置命令时保留后台目录会话；执行 Shell 命令仍消费并退役该会话。
- 复用核对路径、背景/项目类型、Shift 选项、目录 view generation，并受 UI/helper 共用的 120 秒寿命限制。配置/注册表缓存失效会关闭旧会话，过期回复不被接受；旧缓存始终只能展示，仍需绑定当前会话后执行。
- 移除重复的 ctxHoverPrefetched 路径记录及重置逻辑，查询去重统一由控制器负责。

实际定向检查（正常 Windows 环境，隔离夹具，退出码均为 0）：

| 检查 | PASS / FAIL | 范围 |
|---|---:|---|
| 菜单刷新与新建 | 27 / 0 | 冷加载两入口、无占位行、预加载及关闭后复用、代次/Shift/配置变化、部分及完整回复不改变结构、下次新增、静态绑定保持 |
| 命令安全 | 31 / 0 | 当前身份、重复编号/文字拒绝、缓存禁用、部分结果映射与精确执行 |
| COM 控制器 | 12 / 0 | 查询准入、缓存失效退役、已展示命令与当前编号核对、完成与关闭 |
| 命令状态模型 | 3 / 0 | 更新编号/启用状态不改变布局，结构不匹配拒绝原地更新 |
| 菜单局部刷新模型 | 3 / 0 | 保持内置行与已有部分 |
| 菜单窗口交互 | 17 / 0 | 淡入、子菜单锚点、刷新、内部焦点、切其他窗口及取消/关闭 |

合计 93 PASS / 0 FAIL。主程序、Shell helper、控制器测试与菜单 UI 测试构建成功。首次构建夹具出现路径转义警告，已修正；最终受影响重建日志无 MSVC warning/error。旧 COM 夹具在 InvalidateCaches 后继续使用旧 token，和新的退役规则冲突；补充拒绝旧回复、重新建立当前会话后原有身份检查全部通过。首次失败日志保留为 build/menu-stability-com-initial-test.log。未删除或弱化历史身份检查，未运行完整历史回归。

以本次接手时的局部脏树为基线，生产源码净变化 0 行，测试净增加 98 行，不含文档；凭据为 build/menu-stability-line-counts.json。各文件既有修改保留。

验证边界：控制器检查使用受控 Shell 回复，UI 检查使用实际菜单模型/窗口。未现场操作用户目录的第三方 ShellNew 扩展、真实网络 provider，另外使用实际控制器和 FluentMenu 完成中英文、深浅主题、100%/150% 缩放共 8 组渲染夹具检查（58 PASS / 0 FAIL），生成 16 张截图并抽查中英文浅色准备完成菜单、中文深色 150% 冷加载菜单，未见裁切或项目重叠；截图不替代真实第三方扩展现场验收；120 秒失效条件核对了公共常量与调用规则，没有等待真实会话自然超时。首次加载很慢时只显示 Pulse 三项是明确的本次展示规则，下一次打开才加入已准备好的原生模板。


## 20. 确认冗余清理、硬编码修订与 1.1.1.0 交付（2026-10-11）

本轮在第 19 节完成之后执行。不新增产品功能，保留本次接手之前的未提交改动。静态审查覆盖 src 各生产模块、测试调用、CMake 构建映射、脚本和安装配置；仅删除全库引用核对确认无调用的接口及控制流证明不可达的分支。

### 确认删除的代码

- ShellClient::Rename / RestoreRecycle 及对应 host 参数、接收和执行路径：没有生产或测试调用。当前重命名走身份约束本地实现，还原走 RunManualRecycleRestore / RestoreRecyclePair。协议编号 5 和 11 保留为退休编号，不重新分配。
- 旧回收站读取和恢复私有工具、旧 PathExists / RenameDestination / OperationPostconditionSatisfied：仅服务上述退休路径。后置成功补丁已无法对任何现役请求生效，因此删除。
- 公共 Shell 操作段已只执行授权永久删除，删除不可达的回收和旧删除分支、恒为 false 的 recycle_only 状态及恒真授权条件；保留两次产物身份检查、Shell 解析路径核对、HRESULT、取消、实际删除/回收结果数组及旧未授权删除拒绝。COM 接口 override 和 provider 实际回收证据不作为死代码删除。
- 全库无调用的 Painter::OmnibarHintReservePx / DrawOmnibarHints / ButtonGlyphRect 及两个专用常量；MainRenderer::AddressSearchButtonRect / RowFromYInPane / TitleBarRect；TabController::Palette；IndexClient::RequestConfigureVolume；Engine::SaveCache / CompactLocked。现役几何计算、调色板、配置提权和缓存合并实现保留。

### 硬编码修订

- 14 处空输入、初始导航、会话恢复、标签克隆等 C 盘回退统一调用 path::DefaultNavigationRoot，读取 Windows 所在卷；读取失败回到既有“此电脑”视图。没有扫描磁盘或在 UI 线程添加 provider 查询。当前机器为 C 盘系统，未在实际非 C 系统运行。
- version.txt 升至 1.1.1.0；主程序 manifest 从同一 CMake 版本生成，三个相关 RC 入口及依赖同步更新。五个 EXE 的 FileVersion/ProductVersion/OriginalFilename 和实际嵌入主程序 manifest 已核对。安装器不再使用过时 1.0.0 缺省值，要求现有构建入口传入 version.txt 的版本。
- 发布脚本目标由上游仓库改为当前发行版 EternalLightning/pulse；只进行 AST/静态核对，没有执行外部发布。
- 文件操作、产物身份验证、诊断导出、存储迁移错误和缓存年龄接入已有本地化体系，保留错误细节、路径与错误码。新增 60 个资源 ID，迁移/复用 16 个旧 ID；runtime_messages.inc 同时生成 RC 字符串和无资源初始化时的不可变回退，只有一份中英文文案定义。生成真实文件名的“副本”、用户数据及协议/系统常量保留；没有机械改写所有字面量。历史已保存错误文本不会随之后的语言切换重新生成。

### 行数和测试

相对本轮全面审查开始时的脏树，按 src 物理行统计（包括注释、空行及新增 .inc）：实现代码 -268 行，资源宏/枚举/RC/共享文案定义 +192 行，生产源码净 -76 行；必要测试净 +78 行。证明见 docs/validation/1.1.1.0/slimming-line-counts.json。此统计不是相对 Fork 或 HEAD，也不包含前置菜单测试的 +98 行。

| 定向检查 | PASS / FAIL |
|---|---:|
| 无资源回退、中英文资源、缓存单位边界、完整长路径和错误码 | 9 / 0 |
| 现役集中资源完整性与语言 | 6 / 0 |
| 操作状态标签 | 40 / 0 |
| 既有会话恢复用例的独立筛选 | 5 / 0 |
| Shell 完成路由 | 10 / 0 |
| 实际 Shell 新建和 Ping 后响应 | 3 / 0 |
| 新建/删除机器失败及路径传输 | 21 / 0 |
| 手动回收还原 | 18 / 0 |
| 实际授权回收、撤销和永久删除 | 6 / 0 |
| 产物身份、同名替换和撤销安全边界 | 61 / 0 |
| 隔离存储组件 | 30 / 0 |

合计 209 PASS / 0 FAIL，11 组进程退出码均为 0。五个运行目标和相关目标构建通过，最终日志无新增 MSVC warning/error。没有运行完整历史回归。

首轮 RC 依赖扫描将无 BOM 的 UTF-8 宏按 CP936 解读，出现 C4819/C2001；给共享文案和 RC 包含文件添加 UTF-8 BOM 后通过，未抑制诊断。集中资源旧夹具假设所有编号连续，退休编号 2215 已在本轮开始前不存在；改为列出 48 个现役 StringId，完整性断言未减少。首轮身份文案断言关联了另一条错误文案；将实际 ValidateProducedUndo 提示也接入共享规则后，身份、失败码、路径和保护替代文件的断言全部保留并通过。上述初次失败日志及最终日志保存在 docs/validation/1.1.1.0。

### 清理和交付

- 删除 1171 个确认的旧构建/测试产物和包路径，删除前逻辑长度合计 997,861,653 字节（951.64 MiB）。没有找到可证明无用的回归源码，故保留。清单见 docs/validation/1.1.1.0/cleanup.json；修正了清理脚本对字典汇总字段返回 0 的统计错误，逐路径原始长度记录保留。
- 保留当前 CMake/生产构建、第三方依赖、历史必要日志/截图、源码备份、permission-recovery 的 ACL 与恢复资料；旧 1.1.0.1 ZIP 和旧复制载荷已删除。
- 便携包 dist/Pulse-1.1.1.0-portable-win-x64.zip：11,713,991 字节，40 个文件逐项核对 ZIP 路径、长度和 SHA-256；无测试 EXE、对象、日志或嵌入 selftest dispatcher。SHA-256：B2B3C9566A5829323490676D550A7D6A513C38C92607364D997599C6F448FCD0。
- 首次载荷的 Pulse.Preview.exe 曾被 Kaspersky 检出并移除，MD5 BD23B0E1800B2E481738BEF849E90248；先前 40 文件核验和启动截图不覆盖该告警。保留原事件、原哈希和扫描对照记录，未提交复核，未调整防护策略。
- 依据源码/二进制差异，预览 producer 将 ACK 准入从映射名称字符串改为成功创建的像素视图；仅有效像素共享内存等待客户端确认，映射或视图失败不等待，保留全部失败状态与清理。没有撤销此前的失败保护，没有改名、混淆或删除崩溃诊断。定向真实响应和传输检查 43 PASS / 0 FAIL；初次夹具目录创建失败的日志保留，使用内容相同的普通权限副本后两组退出 0。
- 修正后预览 EXE 的 MD5 为 A4D871DA4C5CFB1C916388607E786048。本机 Kaspersky 对 build 及 D:\Applications\Pulse 的新文件扫描均为 0 detected、退出 0；复制到原安装位置后 40 个文件重新核验一致，保留配置与部署备份。本机结果不能证明厂商正式认定误报或保证后续云规则的结果。
- 新 ZIP 替换首次被告警载荷，11,714,038 字节，40 文件逐项核验。当前 SHA-256：062770533A853ED6D3E47EBC97770E05C037DF5CCB48CB4C6141E8F81F20F503；上面的旧 ZIP 哈希仅为告警历史记录，不能用于当前包校验。告警与修正详见 docs/validation/1.1.1.0/antivirus-review.md。

边界：本轮没有执行真实第三方 ShellNew 扩展、SMB/RaiDrive、UAC 成功授权写入、服务注册/安装器安装及实际非 C 系统验收。这些不由静态审查、定向 fixture 或编译结果替代。
