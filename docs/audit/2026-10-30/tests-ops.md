# 操作测试只读审计（2026-10-30）

完整阅读指定的 6 个测试文件，共 5,155 行；路径清单见 `tests-ops-coverage.json`。先检查当前工作树差异，再审读完整文件；其中 `operation_elevation_adapter_test.cpp`、`operation_records_test.cpp`、`ops_test_main.cpp` 有未提交修改，另外 3 个文件相对 HEAD 无差异。所有已有修改均保留。

只写本报告和覆盖清单；未修改、删除源码或测试，未提交，未构建，未运行测试。下文均为当前工作树静态证据，不表示已复现、测试已通过或真实设备/网络/UAC 已验证。定向阅读生产调用链及被包含的夹具只用于核对契约，不计入这 6 个文件的完整覆盖。

发现 3 项 P2 和 3 项 P3；P3 是测试可移植性或故障诊断问题，不与产品数据安全缺陷等量看待。

| ID | 优先级 | 问题 | 证据界限 |
| --- | --- | --- | --- |
| TO01 | P2 | 默认 ops 测试覆盖用户交互式剪贴板，已有隔离入口未复用 | 默认入口和 EmptyClipboard 调用已证实，未运行 |
| TO02 | P2 | 旧 UNC 警告驱动可能点击其他进程的永久删除确认 | 只按文件名/文字筛选且自动点击 Yes 已证实，误操作需同名窗口条件 |
| TO03 | P2 | 旧 UNC 入口仍等待 Windows 警告，与 Pulse 预确认契约冲突 | 当前生产调用链与测试等待条件已证实，未连接 localhost share |
| TO04 | P3 | 网络删除测试无条件假定 Z: 是映射网络盘 | 读取真实环境、无前置条件或 SKIP 已证实 |
| TO05 | P3 | 等待 helper 超时不计失败，后续断言可能消费未完成/前一个任务状态 | helper 和只检查错误文本的调用点已证实，不声称整个测试一定误报成功 |
| TO06 | P3 | 批量冲突模型回归为空时，测试在报告断言前访问空容器 | 未定义行为路径已证实，未注入空模型复现 |

## TO01 — P2：默认 ops 测试没有隔离剪贴板

位置：`src/bench/ops_test_main.cpp:1954-1974`；生产写入：`src/ops/clipboard.cpp:57-67,84-91`；已有隔离路径：`src/bench/ops_test_main.cpp:1394`、`src/bench/clipboard_isolation_cases.h:15-46,68-73`。

- **事实**：默认入口直接执行 20 轮 `WriteClipboard`，每轮先 Copy 再 Cut。`WriteClipboard` 在当前 window station 上调用 `EmptyClipboard` 并写入 CF_HDROP/移动标记；默认入口没有切换私有 station，也没有保存、恢复用户原内容。最后一次成功写入留下的是测试文件的 Cut 列表。
- **触发/影响**：在用户登录桌面直接运行无参数 `pulse_ops_test`，原剪贴板内容会被替换；随后用户在资源管理器粘贴还可能移动夹具文件。这里是剪贴板副作用，不等同于夹具目录外已经发生文件删除。
- **建议**：让默认套件也通过已有私有 window station 路径执行剪贴板检查，或仅保留独立隔离入口。隔离创建失败时按现有实现明确 SKIP，不回退到交互式剪贴板。仅在结束时恢复备份仍可能覆盖测试期间用户的新复制操作，不宜作为隔离替代。
- **最小验证建议（未执行）**：记录交互式 station 剪贴板序号/内容，运行隔离检查后确认未改变；覆盖隔离创建失败，确认执行 0 个写入断言且返回明确 SKIP。

## TO02 — P2：UNC 警告驱动没有确认窗口归属

位置：`src/bench/recycle_backend_test.cpp:699-735`，尤其 `:706-727`。同文件 `:525-542` 的网络观察器也会向非本进程的匹配窗口发送 WM_CLOSE，但自动接受的危险路径在前者。

- **事实**：驱动枚举所有可见顶层窗口，只排除测试自身 PID；其余任意进程只要子控件文字包含夹具叶名及“永久”或 `permanent` 就匹配。`--unc-warning-accept` 随后枚举含“是”或 `Yes` 的控件并发送 `BM_CLICK`，没有核实窗口属于本测试启动的 Shell helper，也没有校验完整路径。
- **触发/影响推断**：在观察期间，用户或其他测试恰有同叶名的永久删除确认窗口，驱动可能自动接受无关删除。取消路径也可能关闭无关窗口。TO03 的契约冲突使当前入口通常不会出现自己的原生警告，但不会阻止这一观察线程继续扫描其他窗口。
- **建议**：取消这类跨进程自动确认，改为解析 `PendingDeleteConfirmation` 的精确 token/路径并调用管理器确认 API。若必须观察原生 UI，只观察明确属于测试启动的 helper PID 的窗口；`src/bench/ops_feedback_test.cpp:529-550` 已有按本测试子进程收窄范围的做法。
- **最小验证建议（未执行）**：用无破坏性的独立进程创建含相同叶名和 Yes 按钮的模拟窗口，确认驱动不发送点击/关闭消息；不要用真实用户删除窗口进行此验证。

## TO03 — P2：旧 UNC 警告入口与当前删除契约不一致

位置：`src/bench/recycle_backend_test.cpp:667-668,738-753`；生产链：`src/ops/delete_operation.cpp:46-55,116-119,437-469`、`src/ops/delete_service.cpp:65-71`。

- **事实**：`--unc-warning-accept` / `--unc-warning-cancel` 提交 UNC `RecycleDelete` 后只等待任务完成，并期待外部 Windows 永久删除警告。当前 `BuildDeletePlan` 先把 UNC 目标分类为 Permanent；DeleteService 因而要求 Pulse 确认，`RunDelete` 先进入 `WaitingForDeleteConfirmation`，确认前不会调用删除后端。测试设置了有效 owner 和非空回调，但回调为空操作，从不调用 `ResolveDeleteConfirmation`。
- **触发**：预置的专用夹具存在且 localhost 管理共享可访问。测试自己的 30 秒等待结束后调用 CancelCurrent；`:744` 仍要求看见 Windows 警告，接受分支还要求删除成功。
- **影响推断**：该入口按当前契约无法验证声明的接受/拒绝行为，通常以超时/错误警告断言失败结束。它与同文件 `RunNetworkDeletion` 在 `:548-578` 使用 Pulse 确认并要求“不出现原生警告”的测试互相冲突。这是静态契约矛盾，不是对现场后端失败的判断。
- **建议**：用当前网络确认用例取代旧驱动，保留精确确认 token、取消后文件保留、接受后准确 completion/无 Undo 的断言。不要为满足旧测试恢复原生永久删除回退或自动点击原生警告。
- **最小验证建议（未执行）**：在独立网络夹具上只运行更新后的接受/拒绝入口，确认先观察 Pulse pending token，再作显式决定，且后端无第二次 Windows 确认。

## TO04 — P3：网络计划断言绑定开发机的 Z: 状态

位置：`src/bench/recycle_backend_test.cpp:490-497`；分类实现：`src/ops/delete_operation.cpp:46-55`；真正创建临时映射的代码在测试 `:499-512`。

- **事实**：`RunNetworkDeletion` 在创建自己的临时映射之前，对固定 `Z:\\Pulse-read-only-plan-check.txt` 无条件断言 Permanent。生产分类正确地查询实际 `GetDriveTypeW` / `WNetGetConnectionW`，并没有把 Z: 硬编码成网络盘。localhost share 不可用时的 SKIP 也晚于这条已失败断言。
- **触发/影响**：正常机器没有 Z:，或 Z: 是本地卷时，这条检查失败，甚至之后打印 share SKIP 仍返回失败；它不能区分产品回归与机器未满足未声明的映射条件。此路径仅构建计划，没有读取/删除 Z: 用户文件。
- **建议**：删除对开发机既有盘符的依赖，使用随后由测试独占建立的临时映射；映射不可用时明确 SKIP 对应集成检查。纯分类逻辑若需要所有分支，可注入查询结果。
- **最小验证建议（未执行）**：在没有 Z: 的环境运行网络入口，确认不因这个无关盘符失败；临时映射验证仍以实际创建结果为前提。

## TO05 — P3：超时和提交失败没有进入测试失败状态

位置：`src/bench/ops_test_main.cpp:213-248`；可直接受影响的断言例如 `:1685-1689`、`:2053-2070`。

- **事实**：`RunOp` 忽略 Submit 返回值，等待超时只写 stderr，既不增加 `g_fail`、不返回完成标记，也不停止继续提交；`RunConflictOp` 到期后直接返回 Status，连超时日志也没有。等待只看全局 completed_ops 增长，不核对所提交 task_id。部分调用仅以 `last_error.empty()`、冲突次数或仍存在的旧文件判断通过，没有要求当前任务终态。
- **触发/影响推断**：任务在处理冲突/写盘后迟迟不结束，或 Submit 被拒绝时，helper 仍返回普通 Status。单条 summary/skip 断言可把活动状态当通过；继续排队后，旧任务完成也可能结束新请求的等待，增加连锁误判和错误归因。后续其他断言或最终 Stop 仍可能失败/阻塞，不能据此声称整个测试一定返回 0。
- **建议**：helper 保留 Submit 的 task_id，明确返回“已完成且属于此请求”的结果；提交失败/超时直接记失败并终止该夹具的依赖步骤，必要时取消该任务。让调用点在检查内容前确认相应终态，不仅判断错误文本为空。
- **最小验证建议（未执行）**：用同步门固定任务超时，确认产生一个明确失败且不把旧任务结果用于下一操作；另覆盖 Submit 返回 0 的分支。

## TO06 — P3：空冲突行使回归测试先发生未定义行为

位置：`src/bench/operation_records_test.cpp:198-217`。

- **事实**：回调先把 `pending->rows.size() == 3` 写入 `metadata_ok`，随后无条件对由这些 rows 生成的向量调用 `back()`、`front()` 和 `pop_back()`。存在 PendingConflict 但 rows 为空时，在最终 `Check(metadata_ok && ...)` 之前已经访问空容器；外围的 `catch(std::exception)` 不能保证捕获这一未定义行为。
- **触发/影响**：批量模型回归为无行的旧式冲突对象时，本来要输出“immutable rows”失败的定向测试可能崩溃，且没有正常结束/清理报告。非空但行数错误时已有 metadata 断言足以发现，问题仅是空模型保护不足。
- **建议**：在构造重复/缺失答案前检查行数；模型不符合前提时先记录失败，再取消该 pending conflict 并结束此夹具，不继续做空容器变异。保留原三种无效响应断言，不弱化模型要求。
- **最小验证建议（未执行）**：注入 pending token 有效但 rows 为空的模型，确认可得到明确 FAIL 并结束，且原正常三行用例仍验证 stale/duplicate/missing 答案。

## 差异核对与未计为缺陷的边界

- 这 6 个文件的当前 diff 没有删除测试或以放宽条件代替原断言。提权部分复制续传从共享大夹具移到 `RunPartialContinuation`，并增加独立 `--partial-continuation` 入口；`src/ops/operation_elevation_adapter.cpp:1533-1561` 仍检查真实首段失败、未完成计划、首段产物身份不变、累计 2/2 进度及后段恰好一次 completion，不能仅因提示文字变化认定削弱验证。
- `operation_records_test --elevation-batch` 是直接构造状态并调用 `ApplyOperationContinuation` 的模型检查（`:52-83,467-515`）；`--task-status` 只跳过真实文件传输夹具，入口明确（`:598`）。这些结果不能证明真实提权传输/UAC，但筛选本身不是删除测试。
- `operation_elevation_adapter_test` 默认入口明示同完整性真实引擎/传输（`:145-147`）；只有显式 `--uac-fixture` 才走 production runas（`:386-477,489-496`）。该 UAC 入口允许用户取消并明确输出 `RESULT=UAC_CANCELLED` 及“仅验证取消”的说明（`:435-438`），不能把其退出 0 当作成功提权写入。
- `rename_ops_test --retained-identity` 明示本地模拟 provider 索引复用；网络检查另需 `--network-root`（`:87-118,295-300`）。网络 Copy Undo 的显式永久确认与生产的 `network_copy_undo` 特例（`delete_operation.cpp:388-405`）相符，不把它误报成所有 Undo 都允许永久删除回退。网络夹具使用独占唯一目录，清理明确列出的自有路径。
- `ops_feedback_test` 的默认重试/状态检查有私有 probe；权限入口确实改动隔离夹具 DACL 并恢复，原生 UI 集成入口另行显式选择。静态读代码不能证明真实 ACL、Restart Manager、Shell provider 或低完整性行为已经运行通过。
- `FileHash` 在打开失败时返回 0、读取失败时返回前缀哈希（`ops_test_main.cpp:196-210`）。这是内容断言的证据限制；本次没有找到足以证明整个指定入口因此无失败返回成功的独立路径，未将其另记为已确认的误通过缺陷。报告中的“哈希相等”不自动等于两个文件均完整读取。
- 默认 ops 文件夹使用固定 `bench_data/opstest`（`:85-92,1633-1646`），并非适合并发运行的独占目录。本次未执行并发测试，也未把路径字面上位于 bench_data 当作动态隔离验证；优先使用已有唯一 PID/tick 定向入口。

## 完整阅读清单

| 文件 | 行数 | 完整阅读 | 主要证据边界 |
| --- | ---: | --- | --- |
| `src/bench/ops_test_main.cpp` | 2,086 | 是 | 默认完整链路、多个 probe/filter 与真实网络入口并存；未运行 |
| `src/bench/rename_ops_test.cpp` | 423 | 是 | 本地身份模拟、真实 Shell/网络入口分开；未运行 |
| `src/bench/recycle_backend_test.cpp` | 816 | 是 | 包含当前 Pulse 确认与过时原生警告驱动；未运行 |
| `src/bench/operation_records_test.cpp` | 602 | 是 | 状态模型及真实引擎故障注入夹具；未运行 |
| `src/bench/operation_elevation_adapter_test.cpp` | 503 | 是 | 同完整性传输与显式真实 runas 分开；未运行 |
| `src/bench/ops_feedback_test.cpp` | 725 | 是 | 模型重试、隔离 DACL 与显式 Shell 集成入口；未运行 |

未声称被包含的其他 bench header 或外部分区已在本清单中逐文件完整审读。这里只完成指定六文件的静态审计。
