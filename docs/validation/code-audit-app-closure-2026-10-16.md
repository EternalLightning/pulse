# App 补读与 R06/R07 定向修复（2026-10-16）

接续 `code-audit-outstanding-2026-10-16.md` 的第 5 节清单。保留本轮开始时所有既有未提交修改。本报告不把全文阅读、编译或独立夹具通过等同于真实 UI 验收。

## 阅读台账

本代理实际全文读取以下 18 个原清单文件。长输出有空隙时重新读取对应段落后才升级为全文；行数按本轮读取的生产版本记录，不计后续新增行。

| 文件（均在 src/app） | 原行数 | 本轮状态及检查重点 |
| --- | ---: | --- |
| app_hosted_edit.cpp | 709 | 全文；编辑器复用、焦点/IME、选择/提交 |
| app_change_tracking.cpp | 387 | 全文；请求身份、分页、局部失败/禁用状态 |
| app_ops_ui.cpp | 369 | 全文；提交与冲突、批量重命名、异步操作状态 |
| archive_dialog.cpp | 341 | 全文；密码/目录输入、模态所有者、GDI 释放 |
| archive_navigation.cpp | 326 | 全文；worker、取消、代际结果、临时提取 |
| new_item_dialog.cpp | 318 | 全文；验证、局部失效、模态循环及资源 |
| address_search.cpp | 432 | 全文；IME/debounce、范围、取消、内容查询刷新 |
| content_results_ui.cpp | 221 | 全文；选择异步结果、弱引用、选择版本验证 |
| folder_sizes.cpp | 489 | 全文；枚举部分失败、缓存、取消与退出 |
| context_menu_controller.cpp | 592 | 全文；菜单会话、缓存展示与可执行身份 |
| context_menu_prefs.cpp | 447 | 全文；完整 JSON 载入/保存/迁移、COM 计数 |
| context_menu.cpp | 512 | 全文；内置命令、排序/显示复用、路径候选 |
| default_file_manager.cpp | 282 | 全文；可逆注册、第三方修改、回滚及部分失败 |
| shell_verbs.cpp | 513 | 全文；注册表枚举、级联、OpenWith、二进制缓存 |
| app_prefs.cpp | 470 | 全文；解析范围、壁纸及系统集成接口 |
| settings_ui.cpp | 190 | 全文；共享布局、设置跳转及菜单 |
| settings_value_dialog.cpp | 272 | 全文；DPI、高对比原生例外、输入与资源 |
| sidebar_sections.cpp | 167 | 全文；共享菜单、隐藏/折叠和边界 |

另外 10 个大文件由主审查任务单独分配只读复核：`app_main.cpp`、`app_input.cpp`、`app_state.h`、`app_runtime.cpp`、`app_model.cpp`、`app_model.h`、`app_navigation.cpp`、`app_commands.cpp`、`global_search_window.cpp`、`places.cpp`。本代理对其中 main/state/commands 只读取 R07 所需段落，不能独立将这 10 项标为全文；以接手代理交付的台账为准。

新增/补充全文：`src/model/batch_rename.cpp/.h`、`src/model/batch_rename_preview.h`、`src/ui/batch_rename_dialog.cpp`、`src/app/shell_registry_watch_context.h`、`src/app/window_result_mailbox.h`、两个新独立测试文件。`app_commands.h` 读取接口相关段落，不冒充再次全文。

原清单的 28 项缺口由本包 18 项与 [大文件补读](code-audit-app-large-closure-2026-10-16.md) 的 10 项共同补齐，现已全部交付。新增 `window_result_mailbox.h`、`shell_registry_watch_context.h` 的阅读另有记录；model helper 与测试文件不混入 App 原清单计数。本轮没有重新普查当前整个 App 目录，因此不以历史加法推断全目录文件总数。

## R06：批量重命名预览同步文件探测

原路径是 EDIT 的 EN_CHANGE / chip / include-extension 事件和 Complete(true) 直接调用 `PreviewBatchRename`，其第二次遍历逐项调用 `GetFileAttributesW`。因此一个慢提供程序调用就占住消息派发线程，大选择集把该延迟叠加。该契约是预览时检查现存目标，并允许仅提交状态为 Ok 的条目；本轮保留同名目标冲突和内部重复目标判定。

新增 model 层 `BatchRenamePreview`：一个 worker 只保留最新待处理请求；新请求通过 generation 使旧任务在项目之间停止，发布时再次验证 generation。worker 只持共享状态与不可变输入，不捕获窗口、dialog 或 AppState。Close 清空队列/结果、推进 generation 并唤醒等待，不等待已进入的提供程序调用；该调用返回后 worker 退出。没有按按键创建一条新 worker。

对话框在编辑更新后禁止 Apply、显示已有本地化 loading 文本，50ms 定时消费本代结果；完成后才允许提交已探测的当前计划。Enter/Apply 不再重新同步探测。异常产生不可提交的失败状态，不接受部分计划。原同步 API 保留给既有离线调用，新增可注入的 exists 和 cancellation 参数用于实际生产 worker 与定向夹具。

独立测试 `pulse_batch_rename_preview_test` 使用 10000 项选择、门控慢 probe、代际覆盖、关闭不等待、关闭后完成、目标冲突及内部重复目标。它直接执行生产 helper 和生产模型，不修改真实文件。测试输出请求/销毁微秒耗时；主任务统一构建并运行结果：9 PASS、0 FAIL、退出 0。10000 项请求返回 11 微秒，门控 provider 未释放时销毁返回 0 微秒（计时精度下限），这是受控夹具耗时，不代表真实网络延迟。

限制：进行中的同步 GetFileAttributesW 本身无法在此改动中强制中止；下一代等待同一 worker 的当前调用返回，但 UI 继续处理输入/取消。未做真实 SMB/RaiDrive/云提供程序延迟实验，未做完整对话框的交互、截图、DPI 或主题验收；门控 probe 通过也不能替代这些环境。

## R07：注册表 watcher 通知与 stop handle

旧通知在 IsWindow 之后裸 PostMessage(hwnd, WM_SHELL_CACHE_INVALIDATE, 0, 0)，不能区分同值 HWND 的新窗口会话。复用已有 `WindowResultMailbox<bool>`，WM_CREATE 创建会话邮箱；worker 上下文持邮箱；主窗口 WM_DESTROY/WM_NCDESTROY 关闭邮箱；消费者只有 Take(wParam) 获得本会话通知才清缓存。Publish/Close 共用互斥约束，关闭后延后 worker 不能继续投递，已排队旧身份不能消费新会话通知。

把已修复的 DuplicateStop 所有权与有上限停止操作抽取到 `shell_registry_watch_context.h`，生产仍使用默认 2000ms 上限。测试用同一生产函数的 0ms 上限确定性走超时分支，不更改产品等待时间。真实 worker 继续保留它自己的重复 handle，UI 关闭自己的句柄后再释放门控，检查 worker 仍读到 WAIT_OBJECT_0，而不是 WAIT_FAILED。

独立测试 `pulse_shell_registry_watch_test` 同时覆盖 stop handle 超时、排队未派发通知、相同真实 HWND 上重建邮箱、旧身份拒收、新通知保留、关闭后延后 producer。窗口为 message-only；同值 HWND 的新会话是确定性模型，不宣称 Windows 实际窗口分配器复用了 HWND。没有写真实注册表或修改用户偏好。主任务统一构建并运行结果：8 PASS、0 FAIL、退出 0。

## 补读发现：最后标签删除后复活及损坏配置覆盖

另外 10 项的只读代理在 `places.cpp` 发现有效 `tags.json` 的空 tags 数组被当成未加载。原 Load 先导入旧 places.json 标签，仅当独立文件解析结果非空才替换；之后 EnsureDefaults 和 QueueTagSave 使删除最后标签后重启复活，损坏文件也可能被旧列表覆盖。

局部修复接受有效空数组为权威结果；仅独立文件确实不存在时允许 legacy 迁移。已有文件读失败、空文档、损坏 JSON、缺少 tags schema、非数组、无效成员类型及重复 ID 均让 Load 返回 false，并阻止该 catalog 后续 QueueTagSave 覆盖独立文件。错误分支可以继续呈现 legacy/default 内存内容，但其标签更改不会持久化；本轮没有添加新的错误提示 UI，不能据此声称加载失败已有充分可见提示。缺文件迁移及旧 0xRGB 格式继续支持。

新增 `pulse_json_persistence_test --places-tags-load`，仅跑隔离 tags 夹具，绕过完整 persistence suite。测试有效空列表、删除最后标签后 writer flush/重启、10 类损坏输入（含颜色错类型/负数/溢出）、错误后的 CreateTag 仍保留原文件、真实独占文件句柄造成的读失败、缺文件迁移。它使用 OverrideDefaultRootForTesting 和唯一 bench_data 目录，不触碰用户数据。生产改动还涉及 `src/model/places.h` 的写入抑制状态；读取型补充台账由发现代理提供。主任务统一构建并运行该 selector：27 PASS、0 FAIL、退出 0。

## 设计与复用复核

- 主窗口地址/过滤/重命名编辑已复用 `CreateHostedEdit`、`PlaceHostedEdit` 与 UI `edit_host` 呈现接口；原生 EDIT 的 IME、输入法、光标与可访问输入用途应保留，不能机械删除。
- archive/new-item/settings-value 均已复用 dialog_lifecycle 的 dim/fade/销毁接口，但 archive/new-item 仍有独立 GDI 调色及字体、按钮、布局代码。settings-value 已使用 Theme tokens 并明确保留高对比原生标题/按钮。这些窗口并非直接适用主窗口 AppState 版本的 hosted edit；不为减少几行代码把它们耦合进 AppState。
- archive/new-item 缺少 settings-value 对高对比和运行中 DPI 变化的明确处理，是设计一致性及可访问性候选；未完成可视实验，不能把既有固定色当成高对比已验收。本轮不在 R06/R07 修复中重写这些窗口。
- 菜单 controller 复用 FluentMenu 和统一命令模型；缓存菜单条目只供展示，可执行项验证会话 token、实时条目及快照，不应合并成只按数值 id 执行的捷径。
- 托盘/诊断等全局原生样式候选不在上述 18 项覆盖之内；由主任务汇总其负责文件的结论。未因此宣称全项目样式统一完成。
- folder_sizes 的局部 HANDLE 包装、default_manager 的 HKEY RAII 与 shell cache 的局部 HANDLE 仍各自贴合不同所有权；没有证据要求本轮将其抽成一个通用新框架。R07 仅抽取了实际需要生产/测试共用的所有权边界。

## 实际验证记录与边界

本代理遵守统一构建协调，没有启动构建或执行测试。已运行本轮生产文件的 `git diff --check`，未发现差异空白错误（已有 LF/CRLF 提示不当成编译错误）。主任务随后完成两个独立 target、`pulse_json_persistence_test` 及受影响 `pulse` 的最新构建，全部退出 0。实际运行 `pulse_batch_rename_preview_test` 为 9 PASS / 0 FAIL，`pulse_shell_registry_watch_test` 为 8 PASS / 0 FAIL，`pulse_json_persistence_test --places-tags-load` 为 27 PASS / 0 FAIL，全部退出 0。日志分别位于 `audit-closure-2026-10-16/pulse_batch_rename_preview_test-default.log`、`audit-closure-2026-10-16/pulse_shell_registry_watch_test-default.log`、`audit-closure-2026-10-16/pulse_json_persistence_test-places-tags-load.log`。这些数字包含准备/清理断言，不是独立产品场景数。未执行全量 selftest、安装、发布、真实服务操作或真实偏好修改。
