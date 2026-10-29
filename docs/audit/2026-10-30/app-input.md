# app_input.cpp 全文静态审查

审查日期：2026-10-30。范围为 `src/app/app_input.cpp`，共 3872 行；全文与当前 diff 均已读取。为核定消息路由、手势归属和取消语义，定向读取了 `app_main.cpp`、`app_model.cpp`、`app_navigation.cpp`、`app_runtime.cpp`、`tab_transfer.cpp`、`tab_shortcuts.h` 及详情面板命中代码；这些定向读取不增加本工作包的全文覆盖数。覆盖明细见 `app-input-coverage.json`。

本工作包只写报告与覆盖清单，没有编辑或删除产品/测试源码，没有构建、运行测试、启动 helper/service 或更改用户配置。四项发现均为源码调用链结论；没有进行 GUI、真实网络延迟或键盘运行复现。

## IP01 / P2：详情属性开关在鼠标消息处理内同步访问目标文件

- **位置**：`src/app/app_input.cpp:2604-2617`，核心为 `2611,2614`。
- **触发**：选中 SMB/UNC、离线挂载盘或响应较慢的文件提供程序中的项目，在详情面板点击只读或隐藏属性。
- **证据（静态已确认）**：`app_main.cpp:1356-1359` 直接把 `WM_LBUTTONDOWN` 交给 `HandleLButtonDown`。该处理函数对 `vm.details.path` 顺序调用 `GetFileAttributesW`、`SetFileAttributesW`，成功后刷新视图；这条路径只有非空路径和开关索引判断，没有把文件访问移交后台任务。详情命中代码 `src/ui/ui_hit_test.cpp:761-763` 将只读/隐藏控件直接映射到此分支。
- **影响**：目标文件系统调用的等待时间进入 UI 消息处理，慢网络或提供程序停顿会使窗口在该调用期间无法响应；本次确认的是同步阻塞风险，没有测量实际延迟，也不声称所有远程点击都会卡顿。
- **建议**：通过现有操作/后台任务路径复制并处理目标路径；完成后验证目标身份与视图代次，再刷新仍匹配的视图，并提供失败结果。避免在 UI 线程等待工作完成。
- **定向验收建议（未运行）**：用隔离的延迟提供程序或受控远程 fixture 检查点击后窗口仍可处理消息；切换 Tab、目标消失及写属性失败时不刷新错误视图。

## IP02 / P2：Esc 没有取消侧栏排序手势，松键仍会提交排序

- **位置**：`src/app/app_input.cpp:3683-3684,3791-3800`；仍可提交的松键分支在 `2952-3059`。
- **触发**：开始侧栏分组、快速访问、收藏文件夹或标签排序，移动到新位置后保持左键按下，按 Esc，再松开左键。
- **证据（可达交互链静态推导）**：键盘入口只先取消菜单预览拖动和 `CancelWindowTransferDrag`；后者在 `src/app/tab_transfer.cpp:160-182` 仅识别 Tab/窗格转移状态。通用 Esc 分支随后取消内容搜索或清除文件选择，没有清除 `groupDrag*`、`pinDrag*`、`starDrag*`、`tagDrag*`，也没有为侧栏手势释放 capture。之后 `HandleLButtonUp` 仍根据这些保留状态执行 `CommitSidebarGroupDrag`（2956）、`ReorderQuickAccess`（2982）、`ReorderStarredFolder`（2994）或 `TagsReordered`（3046）。捕获丢失分支 `3335-3363` 已有保持原顺序的取消逻辑，但 Esc 没有复用它。
- **影响**：用户按 Esc 请求取消后仍会保存新顺序；若当前内容搜索正活跃，还可能先取消无关搜索。这个结论来自状态保留和松键提交链，没有声称已在 GUI 实测。
- **建议**：在通用文件选择/搜索 Esc 之前识别并取消所有侧栏 pending/active 手势，复用相应 reset，清理临时排序/动画状态并释放 capture，消费本次 Esc。
- **定向验收建议（未运行）**：对四类侧栏排序分别执行“拖动、Esc、松键”，确认顺序与持久化状态保持原值；普通松键提交仍有效。

## IP03 / P2：Alt 导航分支没有接通主窗口的系统按键消息

- **位置**：`src/app/app_input.cpp:3770-3775`；调用入口在 `src/app/app_main.cpp:1399-1409,1415-1423`。
- **触发**：主窗口拥有键盘焦点时使用 Alt+Up、Alt+Left 或 Alt+Right。
- **证据（静态消息路由）**：`HandleKeyDown` 明确为三种 Alt 组合调用 `GoUp`、`GoBack`、`GoForward`，但主窗口的 `WM_SYSKEYDOWN` 只单独处理 Alt+D 并把 Alt+Enter 转发给该函数，其他系统按键直接落到默认处理。常规 `WM_KEYDOWN` 才无条件转发。相邻注释 `app_main.cpp:1405-1406` 也说明 Alt+Enter 曾因相同路由遗漏而无法到达。对 `src` 的 accelerator 入口检索只发现两个返回 `S_FALSE` 的 COM 接口实现，没有应用级加速键分发表；主消息循环 `2675-2678` 为普通 TranslateMessage/DispatchMessage。
- **影响**：代码声明的 Alt 向上/后退/前进快捷键，在正常主窗口系统按键消息路径上无法到达导航实现；Backspace 分支仍有普通按键路径。本次未进行真实按键运行验证。
- **建议**：在 `WM_SYSKEYDOWN` 中将这些已声明的 Alt 导航组合转发到统一处理函数，并仅消费已处理组合，保留其他系统快捷键的默认处理。
- **定向验收建议（未运行）**：在有前进/后退记录且存在父目录的隔离窗口中验证三种快捷键分别导航；同时检查 Alt+D、Alt+Enter 与系统快捷键仍按预期工作。

## IP04 / P2：切换 Tab 后松键会把旧点击行号用于新 Tab 的选择

- **位置**：`src/app/app_input.cpp:653-686`，主要提交点 `681-683`；Tab 切换入口 `3695-3703`，松键入口 `3267-3268`。
- **触发**：Tab A 有多个选中项目；按住其中一个选中行且不拖动，保持左键按下并用 Ctrl+Tab 切换到有足够行数的 Tab B，随后松开左键。也可从 Ctrl 按下选中行的延迟取消选择路径触发。
- **证据（可达交互链静态推导）**：按下选中行时，`HandleListRowClick` 只把整数行号和 toggle 标志保存在窗口级 `clickCollapseIndex/clickToggleOnRelease` 中（667-674），行点击随后设置 capture（2199-2211）。键盘处理允许 Ctrl+Tab 切换，但只清除 `blankClickTab` 和重命名候选，没有取消这次延迟选择。`SwitchTab`（`src/app/app_navigation.cpp:2153-2161`）与 `BindCurrentLayout`（`src/app/app_runtime.cpp:272-300`）重绑当前窗格而不重置这些字段。松键后 `FinishListRowClick` 重新读取当前 `ActiveTab` 并将旧整数交给它的 `SelectOnly/ToggleSelect`，没有校验原 Tab、窗格或 view generation。
- **影响**：Tab B 的同号行被选中/切换选中，或其原有多选被替换，尽管点击发生在 Tab A。`Tab::SelectOnly` 的范围检查不能防止对有效同号行的误操作；旧行号超出 B 范围时也会先清空 B 的选择（`src/app/app_model.cpp:242-250`）。没有进行 GUI 复现。
- **建议**：在切换布局/导航或焦点上下文时取消未完成的行点击；或像相邻空白点击、重命名点击一样记录原 Tab/窗格及 generation，并仅在仍匹配时提交。将 pending drag 与延迟点击一起清理，避免手势继续作用于新视图。
- **定向验收建议（未运行）**：在两个内容不同的 Tab 中执行上述序列，分别覆盖 B 的有效/无效同号行和 Ctrl 延迟取消选择；B 的选择应保持原值。

## 去重与边界

- 普通 Fluent 菜单消息循环中的 Ctrl+W 与调用方原始 Tab 指针生命周期归父审查 AC03，本报告不重复编号；内容异步选择旧整数行号问题归 AC01，和 IP04 的未完成鼠标手势跨 Tab 提交不同。
- OLE 拖出后使用原始 Tab 指针的代码已检查，但没有核定独立、正常可达的释放触发链，未仅凭存在模态调用登记新的悬空指针缺陷。
- 本轮没有发现足以扩大范围的新证据；较低优先级的 spring-hover 行号归属候选未继续扩查，也不计入确认发现。

## 实际验证与限制

实际完成：1/1 文件全文读取（3872 行）、当前 diff 审阅、上述调用链静态核定、覆盖 JSON 精确集合/唯一性/行数检查，以及输出文件格式检查。没有构建、执行测试、启动生产程序/helper/service，没有进行 GUI、主题、DPI、真实远程 I/O 或运行性能验收；建议验证均未运行。
