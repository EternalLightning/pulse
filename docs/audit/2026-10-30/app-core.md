# App core 只读审查（2026-10-30）

审查对象为 `src/app` 中文件名不匹配 `(context|menu|settings|system_ctx|cloud|network|device|lan_|server_|sidebar|quick_access|places|folder_view_prefs|default_file_manager|explorer_takeover|registry)` 的文件。`*_test.cpp` 和 `selftest_1b2.*` 由测试分区负责，`explorer_window_takeover.*` 由集成分区负责。保留全部已有修改；本分区没有修改产品源码，没有构建或运行测试。

本分区分配的 163 个文本文件已逐文件完整读取；完整阅读清单见 `app-core-coverage.json`。`app_input.cpp` 已转交索引分区完整补读，不计入本分区分母。二进制 `pulse.ico`、`pulse-icon.png` 仅确认为资源文件，不列入源码阅读覆盖。阅读从当前 diff、接口与完成回调开始，再追踪相应实现。以下所有结论是当前工作树的静态代码证据；没有将历史结果当成本次动态验证。

## AC01 / P2：内容搜索通配选择结果以旧行号应用到可变结果集

- 位置：`src/app/content_results_ui.cpp:174–184`、`:186–203`；支持证据 `src/index/content_result_store.cpp:493–510`、`:225–300`、`:435`。
- 触发：内容搜索结果仍接受实时插入、删除或重新排序；通配选择的 `Match` 已计算出行号，但 UI 尚未消费 `WM_CONTENT_SELECTION`，此时结果集发生变化。
- 事实：通配任务只保存 store 与 `selection_revision`；`Match` 返回整数 `matches`，在持有 `io` 锁时遍历当时的 `visible.pos`，释放锁后回调。完成端只校验同一 store、同一选择修订号与无错误，随后 `SelectIndices(result->matches)`。任务没有捕获结果集 epoch。索引分区交叉确认：实时更改和 `SetSort` 会刷新页、增加 `page_epoch`，而 Match 不返回它。
- 推论与影响：在计算完成到 UI 消费之间改变页顺序，即使用户没有改动选择，旧行号也可能指向其他文件；后续复制、删除或打开会针对错选对象。该窗口的代码条件成立；实际调度频率和可见后果尚未动态复现。
- 建议：Match 返回捕获的结果集 epoch 或稳定文件身份，UI 消费前验证同一版本；过期时丢弃或重算。沿用现有选择任务，不另建平行状态系统。
- 定向验收：在 Match 完成后拦住消息消费，向结果集插入/删除靠前条目或改变排序，再释放完成；必须保持原匹配文件身份或取消该次选择，不能选中新的同号条目。

## AC02 / P2：目录通知增量更新丢失快捷方式目标元数据

- 位置：`src/app/snapshot_patch.cpp:18–32`、`:90–102`；`src/app/app_notify_patch.cpp:145–147`；`src/app/app_worker.cpp:496–499`；`src/app/app_navigation.cpp:1962–1978`；`src/app/entry_sort.cpp:73–80`。
- 触发：正在浏览的文件夹中，目录快捷方式 `.lnk` 被其他程序新增、修改或重命名，目录通知成功进入增量路径而未退回完整枚举。
- 事实：`FillDirEntry` 从空 `DirEntry` 填充基本属性、尺寸和时间，不解析 `link_target`；`ApplyResolvedDirNotifies` 用它覆盖或插入行。通知工作只执行 ResolveDirNotifies 与 BuildNotifyProjections。完整枚举则执行 ResolveLinksInPlace。打开单行时，只有已有 `link_target` 才在 Pulse 内 NavigateTo；缺少目标就通过 OpenWith 打开 `.lnk`。排序也仅将已解析的目录链接归入文件夹组。
- 推论与影响：通知后的目录快捷方式可能被当作普通文件排序、无法形成列视图子列，双击改为交给 Shell，行为与刷新后的同一快捷方式不同。外部 Shell 最终如何打开受系统关联影响；本次未运行桌面复现。
- 建议：在现有通知 I/O 工作中对涉及的快捷方式解析目标，再构建投影；不能仅保留旧目标来处理真实目标已改动的 MODIFIED 事件。保持代数、路径与快照校验。
- 定向验收：外部新增、改目标及重命名一个目录 `.lnk` 后，比较增量更新与手动刷新产生的条目、文件夹排序及内部打开目标；另测失效链接与取消/导航离开。

## AC03 / P1：菜单循环允许关闭标签，返回后访问已释放标签页

- 位置：`src/app/app_change_tracking.cpp:364–382`；`src/app/app_input.cpp:3681–3695`、`:3741–3742`；`src/app/app_navigation.cpp:2123–2150`；`src/app/app_model.cpp:658–664`；`src/app/app_main.cpp:441–455`。菜单侧支持证据：`src/ui/fluent_menu.cpp:526–532`、`:1349–1385`、`:1541–1553`、`:1593–1596`、`:1683–1700`（UI 分区交叉核实）。
- 触发：至少两个未固定标签页；在当前变更记录页打开时间/类型普通菜单，按 Ctrl+W 关闭当前标签，然后在仍打开的菜单中选择一个项目。
- 事实：调用方在进入 TrackPopup 前保存原始 `Tab*`，返回正选择后直接写 `change_days/change_kind` 并调用 LoadChangeView。普通无过滤菜单使用 WS_EX_NOACTIVATE，焦点留在 owner；菜单循环只吞已识别的导航键，Ctrl+W 继续分派给主窗口。HandleKeyDown 没有 menu.IsOpen 防护，Ctrl+W 关闭标签；WindowTabs::CloseTab erase 释放其 Pane/Tab。before_close 只清理搜索，关闭流程没有关闭菜单或保持该 Tab 存活。
- 推论与影响：菜单选择返回后存在对已释放 Tab 的读写，可能崩溃或破坏内存；若相同地址复用，可能改动其他标签。该释放和解引用链经 app/UI 两侧静态交叉确认，尚未动态或内存诊断器复现。
- 建议：菜单只保存可验证的 Pane/Tab 身份及原路径/代数，返回后从活对象表重新查找；对象已销毁/转移/导航就丢弃结果。统一规定普通菜单期间标签快捷键的语义并避免仅靠裸指针存活假设。同类调用点如 `tab_controller.cpp:249–296`、`:298–355` 和 `search_options.cpp:148–214` 也应按同一契约核验。
- 定向验收：自动化执行上述 Ctrl+W 序列，覆盖关闭当前页、切页、单例关闭及标签迁移；验证原页释放后菜单结果不写入旧地址，主窗口能继续交互。使用地址消毒/页堆等可用内存诊断时记录实际结果。

## AC04 / P1：徽章编辑弹窗跨设备变化保留已释放的侧栏条目指针

- 位置：`src/app/app_commands.cpp:1346–1365`、`:1387–1399`、`:1299–1302`；`src/app/app_main.cpp:1033–1043`。菜单侧支持证据：`src/ui/fluent_menu.cpp:1512`、`:1578–1595`、`:1686–1690`（UI 分区交叉确认）。
- 触发：对未收藏的桌面/下载等快速访问条目打开徽章文字编辑弹窗，保持弹窗打开时插入或拔出 USB 卷，随后继续输入文字或选择颜色。
- 事实：ShowStarredBadgeEditor 在进入 TrackPopup 前保存 `SidebarEntry* quick_access`，过滤重建回调及 apply_color 随后直接写该指针。TrackPopup 的过滤模式仍使用全线程 RunModalLoop，未消费的设备消息分派给 owner；WM_DEVICECHANGE 对卷到达/移除立即调用 RefreshSidebarModel，后者整体替换 `s.sidebar`，释放旧 quick_access 向量元素。该消息路径没有 dismiss 弹窗或重查条目。
- 推论与影响：弹窗继续输入/选色时写入已销毁的 SidebarEntry，其中包含 std::wstring 赋值；可能崩溃或破坏内存。释放与后续解引用路径经 app/UI 两侧静态交叉确认，尚未用实际 USB 或内存诊断器复现。此项对象来源与 AC03 不同，不能只禁止 Ctrl+W 来修复。
- 建议：保存条目路径及条目类型，在每次过滤回调、选色和提交时重新查找当前条目；不存在时终止编辑。对整个侧栏重建维持稳定条目所有权也可，但应复用现有模型而非保留任意旧指针。
- 定向验收：在快速访问徽章弹窗打开时触发真实或受控卷到达/移除，再输入、选色和关闭；在内存诊断下确认没有旧条目访问，并确认新侧栏同一路径条目正确显示已保存徽章。

## AC05 / P2：侧栏重建在窗口消息线程同步查询所有逻辑盘

- 位置：`src/app/app_model.cpp:1318–1326`；`src/app/app_commands.cpp:1299–1302`；`src/app/app_main.cpp:493`、`:1033–1043`；`src/app/app_commands.cpp:2191–2193`。
- 触发：启动窗口、USB 卷到达/移除或切换语言时重建侧栏，系统存在响应缓慢的映射网络盘、可移动盘或其他卷。
- 事实：BuildSidebarModel 直接遍历 GetLogicalDrives 的所有盘符，逐个调用同步 GetVolumeInformationW 与 GetDiskFreeSpaceExW，没有按盘类型排除、缓存、后台调度或超时。调用方 RefreshSidebarModel 被 WM_CREATE、WM_DEVICECHANGE 和 UI 设置效果直接调用；这部分 I/O 实际处于主窗口消息线程。HomeCatalog 的后台 Refresh 在这些同步调用返回后才执行。
- 推论与影响：任一卷查询等待其文件系统/provider 时，窗口无法处理绘制、输入和其他完成消息；后台 HomeCatalog 不能消除此路径的前置等待。代码中的同步位置确定；慢盘造成的实际等待时长、系统弹窗及具体 provider 行为本次未动态验证。
- 建议：侧栏先使用已缓存卷事实/占位标签，在现有后台目录或 HomeCatalog 工作上采集卷名与容量，再通过已存在的完成机制更新；设备变化只让受影响盘符失效，避免每次在 UI 重查全部盘。
- 定向验收：用受控延迟的卷查询或实际慢映射盘触发侧栏重建，记录 UI 心跳/输入响应与完成更新；确认查询离开 UI 线程，盘移除和过期结果不会覆盖新模型。

## AC06 / P2：截图计时输出把 snprintf 所需长度当成实际缓冲区长度

- 位置：`src/app/app_main.cpp:2207–2214`。
- 触发：使用 `--shot` 浏览较长的 ASCII 文件夹路径，且完整 `path=...` 加计时字段超过 255 字节；截图保存成功后写入 `.timing.txt`。
- 事实：格式化使用 256 字节栈缓冲 `char buf[256]`。snprintf 的正返回值 `n` 是完整格式化所需字节数，截断时可以大于缓冲区实际内容；代码仅检查 `n > 0`，直接把 n 传给 WriteFile。
- 推论与影响：长路径时 WriteFile 从栈缓冲区之外读取数据写入计时文件，可能泄露相邻栈内容或读取失败；此路径只在截图诊断模式触发，不影响普通短路径格式化。本次为静态长度推导，未运行截图或诊断器复现。
- 建议：使用动态尺寸格式化，或将写入长度限制为 `sizeof(buf) - 1` 并明确处理截断；检查 WriteFile 返回值及实际写入长度。
- 定向验收：对完整输出恰好小于、等于和超过 256 字节的路径分别生成计时内容，确认输出长度受实际缓冲区约束，文件中没有末尾之外的栈字节。

## 结构观察与未报缺陷的候选

- WorkerPool 已区分本地目录事实生产与排序投影，使用请求代数、独立 mailbox、受限 I/O 队列和有界退出；不能因其未直接使用另一后端而认定重复造轮子。重复同 sort key 的 Refresh 会新建事实请求，不保证 N 次调用恰好执行 N 次完整枚举，取消/队列裁剪会影响实际 I/O。
- FolderSizes 已有有界工作线程、分片扫描、轮转、索引批量、reparse/cloud 避免触发和请求 epoch；本次未据此报告新的确定缺陷。
- `ApplyLaunchOnStartup` 使用固定 StartupCommandLine，不将 start_to_tray 直接编码进 Run 命令，故“切换托盘写入旧参数”候选排除。
- 归档提取对话框后的裸指针及 HomeCatalog 映射盘元数据阻塞仍缺少确定的自然触发/动态证据，不列入已确认编号。

## 覆盖边界

本分区 163/163 个文本文件均已完整读取，包括 `app_main.cpp` 与 `app_runtime.cpp`；无局部阅读文件计入覆盖。`app_input.cpp` 已转交索引分区补读，不列入本分区覆盖分母。支持引用的 index/UI 文件未计入本分区完整覆盖（其负责分区另有清单）。

本报告未进行编译、功能测试、截图或性能测量；因此没有对桌面流畅度、主题/DPI布局、第三方 provider 行为、真实 SMB/MTP/云端与已安装版本给出验收结论。
