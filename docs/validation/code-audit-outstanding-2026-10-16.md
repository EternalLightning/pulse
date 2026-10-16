# Pulse 未完成审查事项报告

> 后续收口：本清单的 U01–U12、R01–R08 已完成局部修复/契约核实及定向验证，原 App/UI 阅读缺口和 Ops 台账已补齐。当前结果、证据及仍未执行的真实环境/视觉验收见 [2026-10-16 收口报告](code-audit-closure-2026-10-16.md)。以下保留为开始本轮工作时的历史状态。

日期：2026-10-16

项目：`E:\Projects\pulse`

范围：汇总既有审查记录及本次增量核对，作为后续审查、复现和修复的工作清单。不是全仓验收结论，也不是发布批准。

## 1. 当前结论

**全项目代码审查尚未完成。Core 阅读完成，但仍存在未修复的代码问题；App、UI 和部分操作模块仍有阅读缺口。**

| 范围 | 当前可确认状态 | 尚未完成 |
| --- | --- | --- |
| Core 清单 | 157/157 项已有全文阅读记录 | 下文列出的逻辑、持久化、错误处理及并发问题；全文阅读不等于无缺陷 |
| App 清单 | 原表 163 项：133 全文、30 部分；续篇补齐两个旧文件并新增一个 helper，记录为 164 项、136 全文、28 未全文 | 28 个原清单文件仍须补读；后续新增文件应同步更新总清单 |
| UI 清单 | 续篇先剩 19 项，随后补读 drag_drop.cpp，剩 18 项 | 大型绘制、菜单、预览、设置与对话框文件 |
| Model | 既有报告记录生产 cpp/h 已全文阅读 | 批量重命名的同步文件探测等跨层问题仍未关闭 |
| Ops / FS / Shell | 已有多轮定向阅读、修复及部分全文记录 | 不能据此声称全部逐行覆盖；管理器、提权适配/传输及部分依赖分支需要补齐台账 |

App 计数以 [App 覆盖表](code-audit-app-coverage-2026-10-15.md) 加 [App 续篇](code-audit-app-continuation-2026-10-15.md) 的增量记录为依据；不能直接把旧表的 30 项当成当前剩余数。UI 依据 [UI 续篇](code-audit-ui-continuation-2026-10-15.md) 和 [拖放审查](code-audit-drag-drop-2026-10-16.md)。这些是记录内的覆盖统计，不是对当前全部文件重新普查后的总数。

上一轮 Core 八组定向检查最终记录 **121 PASS / 0 FAIL**，并成功构建 `pulse`、`pulse_index`；该数字包含准备和清理断言，并非 121 个独立产品场景。本报告没有重跑这些检查，也没有把历史结果当成本次新结果。

## 2. 优先处理：索引身份与结构安全

P1 表示应优先复现和修复的严重正确性或安全问题；P2 表示正常功能结果、状态或可靠性问题。优先级不代表动态复现已完成。

### U01 — P1：跨卷增量日志重放错配全局节点 ID

- **位置**：`src/index/index_engine.cpp` 的 `ReplayDeltasLocked`、`ApplyUsnLocked`；`src/index/index_delta.cpp/.h` 的 Add 记录格式。
- **代码证据**：各卷新增节点在运行时共用全局递增 ID；Add 日志不保存该 ID，而 Patch、Tomb 和 parent 引用保存它。启动逐卷重放时重新分配 ID，无法保持原来跨卷交错的分配顺序。
- **影响**：索引中的修改或删除可能作用于另一卷条目，新目录的子节点可能挂错父节点。此处指索引数据错配，不是日志重放直接删除磁盘文件。已经推进的 USN checkpoint 使后续追赶不一定自动修正旧事件。
- **当前状态**：本次再次核对生产路径；已新增两卷交错新增、改名、属性修改、删除的测试草稿，但**尚未取得有效动态复现结果，也未修改生产实现**。具体执行记录见第 7 节。
- **下一步**：先排查夹具异常退出，取得原实现失败对照；再明确节点身份持久化与旧日志迁移策略。不能仅调整卷遍历顺序，也不能猜测旧日志中缺失的 ID。
- **关闭条件**：真实 DeltaLog 与真实 mmap Engine 夹具验证跨卷和同卷父子关系、FRN 映射、存活集合、属性、checkpoint；再次启动和继续追加日志后仍一致；旧版本日志有明确且验证过的恢复路径。

### U02 — P1，损坏输入路径：snapshot 父节点未校验，后续重建可越界

- **位置**：`index_engine.cpp` 的 `MapIndexFile`、`FlattenLocked`、`PreserveOfflineVolumesLocked`。
- **代码证据**：映射校验检查名称池等范围，却不拒绝越界 parent；后两者直接用 parent 索引 remap。Flatten 的尾部补扫仍能处理未进入正常子树的坏节点，使该访问可达。
- **影响**：损坏缓存可能在合并或保留离线卷时造成越界读取；父链环还可能被静默改成根节点。CRC 不能替代结构验证，普通用户缓存与旧缓存导入也需要保护。
- **当前状态**：主审查与独立只读复核均确认代码路径；未注入损坏 snapshot，未修复。
- **下一步**：验证 parent 上界和父链环；保留合法负 parent 根语义。卷 root/span 校验须考虑加载支持的旧格式，不能无依据套用新版本布局。内存重建路径也应防止未经映射校验的坏引用。
- **关闭条件**：复用 `pulse_index_engine_test --mapped-validation-only`，覆盖 parent 等于节点数、极大值、自环、多节点环、合法根和版本兼容；另验证内存坏 parent 无法进入越界 remap 访问。

来源：[Engine 审查 E1/E6](code-audit-engine-full-2026-10-16.md)。

## 3. 已有静态缺陷证据，但尚未完成修复与验证

| ID / 优先级 | 问题与位置 | 影响及触发 | 下一步及关闭条件 |
| --- | --- | --- | --- |
| U03 / P2 | 移动后继续信任旧 DFS 子树区间；`InSubtreeLocked`、`CollectMatchesLocked` | base 目录跨父节点移动后，旧范围仍包含它，新范围可能漏掉它 | 真实映射夹具移动 A/child 到 B，核对名称、通配符和后代查询与当前父链 oracle；同父改名和 live 新增作对照 |
| U04 / P2 | 稀疏 MatchSet 重复收集 patched base ID | 小于 65536 节点时，posting 和 patches 两条候选来源可重复加入同一条目，影响 total、分页 | 用属性/名称 patch 覆盖拼音开关、唯一 ID、总数和分页；验证稀疏与密集两分支 |
| U05 / P2 | 深路径截断或 MFT 长父链挂错根；`BuildPathLocked`、`BuildQueryPathLocked`、`BuildMftTree` | 64/256 层路径上限及 48-hop 建树回退可能返回缺祖先路径 | 合成父先/子先 FRN 链，跨越各边界；与完整路径、排除规则和父子关系比较，另保留防环能力 |
| U06 / P2 | `FullRebuild` 在 mutex 外写 excluded_paths_，读者持 shared mutex 访问 | 配置重建与 feed/coverage 读取可能发生数据竞争 | 统一快照发布锁，审查所有读写入口；定向并发验证，不把耗时扫描搬入 UI 线程或长临界区 |
| U07 / P2 | 有值的 `!file:foo` / `!folder:foo` 否定语义错误；`index_query.cpp` | 前者忽略否定；后者提前强制目录类型，未按整个谓词取反 | 以 foo/非 foo 文件与目录四类夹具验证带值、裸类型、双重否定及不同搜索后端；先统一谓词语义 |
| U08 / P2 | 含分隔符的路径谓词按单个名称组件匹配 | `foo\\bar`、相对 path 条件不能命中，`!path:C:\\skip` 等排除可能失效 | 比较完整规范化路径；覆盖 skip/skip2 边界、第二个绝对路径条件、local/network 一致性 |
| U09 / P2 | 网络根配置解析宽松；`LoadNetworkRootsFile` / `ExtractStringArray` | 损坏配置被当成成功的空列表，后续保存可覆盖原内容；合法 Unicode 转义也解析错误 | 区分空、缺键、损坏、I/O 失败；坏输入保留原文件；验证截断、错误类型、转义及失败后的添加/保存 |
| U10 / P2 | transient 内容/重复文件扫描丢失部分枚举错误；`content_search.cpp` | 一个根成功打开即可掩盖另一个根或子目录失败，部分结果被报告为完整 | 隔离正常根与缺失根，注入非 EOF 枚举失败；保留已确认命中并明确不完整状态 |
| U11 / P2 | persistent content agent 达到 16 个长期管道连接后退出 | 下一次创建管道实例失败触发退出并取消已有查询；正常 UI 可达性未验证 | 建立受控并发连接验证容量边界；新增请求饱和不得取消既有查询，恢复后可继续接入 |
| U12 / P2 | 重建命令忽略 `ReloadService` 失败；`index_host.cpp --rebuild-index` | 服务不存在、停止或拒绝控制时仍返回 0，上层误判已成功提交重建 | 注入服务控制结果并验证退出码和上层提示；成功与失败都验证，不操作用户实际服务 |

U03–U06 来源：[Engine 审查](code-audit-engine-full-2026-10-16.md)。U07–U08 来源：[查询审查](code-audit-query-full-2026-10-16.md)。U09 来源：[网络审查 NET-4](code-audit-network-full-2026-10-16.md)。U10–U11 来源：[内容搜索 CF-03/CF-04](code-audit-content-full-2026-10-16.md)。U12 来源：[索引 Host 审查 HOST-1](code-audit-index-host-full-2026-10-16.md)。以上均不标记为已动态复现或已修复。

## 4. 需要先核对契约或复现的候选风险

| ID | 候选风险 | 目前证据与限制 | 下一步 |
| --- | --- | --- | --- |
| R01 | 内容查询 root/roots 契约冲突 | wire 设置 root=首个根、roots=全部根；缓存/task 同时施加两者，额外不相交根可能被排除。已有 DFS 修复刻意保留该契约 | 对真实序列化、缓存和扫描入口做双根用例，统一有效范围含义后修复 |
| R02 | `ReconcileShared` 临时属性失败后 prune 旧缓存 | 属性失败跳过 epoch 更新，而最终 prune 条件未覆盖逐文件失败；未注入验证 | 注入暂时访问失败，检查旧条目保留及恢复后的更新/真实删除 |
| R03 | `Reconcile` 不完整扫描仍写 Ready | cursor.complete=false 会阻止 prune，但 Ready 与 root.error 在 UI 如何组合展示尚未核实 | 核对状态消费与界面，再决定是状态机缺陷还是已有错误提示足够 |
| R04 | 存储迁移失败回滚按路径删除 | `user_storage.cpp` 复制时锁住对象，但回滚未持续绑定目标身份；NoReparse 也没有覆盖全程的目录租约 | 隔离并发替换夹具验证回滚对象身份，确保不误删替换后的对象；列为高优先级安全候选 |
| R05 | 诊断导出/清理丢失枚举错误 | `diagnostics_exporter.cpp` 的循环结束和部分属性失败没有完整区分正常空与失败 | 只用隔离日志夹具注入枚举失败，检查返回状态及输出完整性，不触碰真实日志 |
| R06 | 批量重命名预览同步探测路径 | `batch_rename_dialog.cpp` 编辑更新同步调用 `PreviewBatchRename`，后者逐项 `GetFileAttributesW`；本次源码检索仍存在，未测延迟 | 测量大选择集/受控慢路径；必要时拆分纯预览与可取消、代际隔离的异步冲突探测 |
| R07 | 注册表 watcher 的旧 HWND 失效通知竞争 | 句柄所有权已修复，但 `WM_SHELL_CACHE_INVALIDATE` 仍独立于两个已迁移结果邮箱 | 针对退出、延后完成、窗口会话更替验证；不把已修复的堆载荷通道重复列为未修复 |
| R08 | 极端/异常配置输入 | 查询数值累乘/单位乘法未检查溢出，无效日期可能退化；机器配置宽松解析及旧 SQLite fallback 失败语义未验收 | 独立边界输入用例，明确拒绝或回退契约；目前不据此声称内存越界 |

来源：[内容搜索](code-audit-content-full-2026-10-16.md)、[内容索引](code-audit-content-index-full-2026-10-16.md)、[存储与诊断补读](code-audit-drag-drop-2026-10-16.md)、[App 审查](code-audit-app-2026-10-15.md)、[异步生命周期](code-audit-app-async-2026-10-16.md)、[查询审查](code-audit-query-full-2026-10-16.md)、[配置审查](code-audit-small-configuration-2026-10-16.md)。

## 5. 尚未补齐的阅读和设计检查

### App：原清单剩余 28 项

以下相对路径均位于 `src/app/`；从旧覆盖表剔除续篇已补齐的 `app_column_view.cpp`、`search_options.cpp`。局部修复不自动升级为全文阅读。

| 分组 | 文件 |
| --- | --- |
| 主窗口、输入与状态 | app_main.cpp、app_input.cpp、app_hosted_edit.cpp、app_state.h、app_runtime.cpp |
| 模型、导航与命令 | app_model.cpp、app_model.h、app_navigation.cpp、app_commands.cpp、app_change_tracking.cpp |
| 操作与归档 | app_ops_ui.cpp、archive_dialog.cpp、archive_navigation.cpp、new_item_dialog.cpp |
| 搜索与目录 | address_search.cpp、content_results_ui.cpp、global_search_window.cpp、folder_sizes.cpp |
| 菜单与 Shell | context_menu_controller.cpp、context_menu_prefs.cpp、context_menu.cpp、default_file_manager.cpp、shell_verbs.cpp |
| 设置与侧栏 | app_prefs.cpp、settings_ui.cpp、settings_value_dialog.cpp、places.cpp、sidebar_sections.cpp |

### UI：原清单剩余 18 项

以下相对路径均位于 `src/ui/`，已剔除后来补齐的 `drag_drop.cpp`。

| 分组 | 文件 |
| --- | --- |
| 对话框与组件 | advanced_search_dialog.cpp、color_picker.cpp、file_operation_dialog.cpp、fluent_components.cpp、fluent_components.h、command_icons.h、fluent_menu.cpp |
| 预览与文本 | lumatext_renderer.cpp、preview_handler_host.cpp、quick_preview_window.cpp |
| 绘制与布局 | ui_details_panel.cpp、ui_pane_list.cpp、ui_renderer_internal.h、ui_renderer.cpp、ui_renderer.h、ui_settings_view.cpp、ui_sidebar.cpp、window_material.cpp |

### 操作模块及原始设计目标

- `ops_manager.cpp`、`operation_elevation_adapter.cpp`、`operation_elevation_transport.cpp` 等大实现仍有只读 diff/关键路径、未完整覆盖的记录；应先补齐精确台账，不报一个未经核对的剩余文件总数。参见 [Ops 续篇](code-audit-ops-continuation-2026-10-15.md)。
- **原生窗口样式检查未全局收尾**：App 中诊断/隐私/操作恢复确认、输入错误提示、托盘 TrackPopupMenu，以及 UI 原生 Tooltip 仍有统一样式候选。图形初始化失败、系统高对比、输入/IME 宿主与第三方预览窗口具有合理的原生或降级用途，不能机械替换。
- **复用检查未全局收尾**：继续对照压缩/新建/设置输入窗口的 hosted-edit 与生命周期实现；回收站索引解码和局部 HANDLE/进程封装有共享候选。现有 toolbar/settings/column 几何已复用，不应重复建设；没有证据的抽象合并不作为必须修复项。

## 6. 验证缺口与验收边界

未执行不等于失败，也不表示每个后续修复都必须跑完整回归。按实际影响选择以下检查：

| 边界 | 尚无完整证据 | 适用时的验证方式 |
| --- | --- | --- |
| 文件系统和持久化 | 真实磁盘损坏、断电、跨卷重放、完整迁移竞争 | 优先隔离 snapshot/WAL、I/O 注入和对象替换夹具；必要时再扩大集成范围 |
| 网络和提供程序 | 真实 SMB 断连、映射盘变更、RaiDrive/云盘身份及阻塞行为 | 受控连接/断连与取消场景；不得拿枚举注入通过代替真实网络验收 |
| 服务与权限 | SCM/UAC、真实服务重建命令、运行中配置切换 | 先测试可注入适配层，再按需要安排真实服务验证，不默认修改用户服务 |
| 文档与 Shell | Office/PDF provider、默认查看器、外部 Shell 扩展长时间阻塞 | 对受影响 provider 的加载、取消、退出和错误恢复做集成验证 |
| 界面 | 主题、高对比、DPI、多屏、窄布局、快速切换和关闭 | 针对可见改动做截图/交互检查；编译通过或屏幕外 HWND 夹具不替代视觉验收 |
| 已修复注册表 stop handle | 当前仅有编译和所有权静态依据；此前探针前置检查退出，未获动态通过 | 修复隔离探针，再覆盖超时后 worker 完成与句柄生命周期 |

未执行完整 selftest、全量历史测试或发布验收；这不是要求下一步无差别执行。与当前修改相关的定向检查通过后停止，不为填补报告而重复旧测试。

## 7. 本次正在进行的跨卷夹具：如实交接

用户要求出具本报告时，U01 的工作停在测试准备阶段：

- 新增 `src/bench/index_delta_replay_cases.h`，并在 `src/bench/index_engine_test.cpp` 增加独立参数 `--delta-replay-only`。夹具使用 bench_data 隔离目录及两卷内存元数据，不运行真实 USN 监听或修改 C/D 盘用户文件。
- 本次未更改 `index_engine.cpp`、`index_delta.cpp/.h` 的生产逻辑；这些文件本来已有其它批次的未提交修改，均予保留。
- 初次沙箱构建停在 VerifyGlobs；定位并终止本次拥有的构建进程树后，相同 `pulse_index_engine_test` 目标在批准的执行环境下构建成功，退出 0。
- 新 selector 运行退出 1，无可诊断 stdout；改用命令局部、仓库内 TEMP/TMP 后仍退出 1、无输出。**原因未查明，不能认定为环境问题，也不能计为 U01 已复现。**
- 测试草稿保留待排查；未完成运行留下的 `bench_data/delta-replay-*` 及局部临时目录应在确认归属后清理，不能广泛删除 bench_data。
- 独立子代理只读复核了 U02，未修改文件、未构建；结论已纳入第 2 节。

因此此刻没有可报告的新增产品修复或新功能测试通过结果。本次报告本身只做内容、引用和差异检查。

## 8. 不应再列为“尚未修复”的历史事项

以下已有后续修复记录；保留各自验证限制，不因旧报告写过“未修复”而重复登记：

- **解压中断后重试保留损坏文件**：已由 [解压恢复报告](archive-retry-recovery-2026-10-15.md) 接替旧风险记录。重试仍在原目录，仅处理身份确认的上次未完成残留，保留其它文件；不要求用户换目录。
- **静态 Shell verb 与重复扫描卷列表的 HWND 堆载荷**：已迁移结果邮箱并有定向验证；剩余独立通知风险见 R07。[异步生命周期报告](code-audit-app-async-2026-10-16.md)。
- **分组 ID 溢出、注册表 stop HANDLE 所有权**：已有实现修复；后者动态验证缺口另列于第 6 节。[App 续篇](code-audit-app-continuation-2026-10-15.md)。
- **`!type:folder` / `!kind:folder`**：已有定向修复；不是 U07 的带名称 `!file:foo` / `!folder:foo` 问题。[类型否定报告](code-audit-folder-kind-negation-2026-10-16.md)。
- **Core 最近八组修复**：变更分类、网络扫描保留旧分片、内容查询线程退出、查询缓存缩小条件、Shell 启动失败恢复、task 范围/枚举完整性、MFT runlist 失败回退、MODIFIED 缓存失效均已有记录。[Core 汇总](code-audit-core-full-round-2026-10-16.md)。

## 9. 后续顺序与关闭标准

建议依次处理：**U01/U02 与 R04 的数据身份安全 → U03–U05 查询结构正确性 → U07–U12 错误语义和完整性 → 并发候选与 App/UI 补读 → 受影响范围的集成/视觉验证**。这是工作顺序，不是承诺固定批次数或工时；候选复现与覆盖补齐可能发现新问题。

每项关闭应具备：明确触发和依据、局部修复、能够区分旧新行为的定向验证、实际构建/测试结果、仍未覆盖的边界。未复现候选如经证据排除，应记录排除原因；不得仅因读过代码、编译成功或旧测试通过而关闭。

本报告保留既有未提交工作；没有提交、发布、部署、修改真实偏好或操作用户服务。
