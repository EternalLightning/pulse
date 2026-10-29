# App 与会话审计复核

日期：2026-10-25。对象：本工作树中的生产调用、模型与定向测试源。保留原有未提交修改。未改 `docs/report.md`，未触碰右键菜单设置相关文件，未启动任何共享构建。

本文件的“已补代码”仅表示实现与回归源已完成；最终编译、运行结果由主代理在独立构建目录补充，不能据此宣称功能已通过。

## 需要先纠正的范围判断

- 启动明确不恢复导航：`session.h` 的偏好接口注释、`app_main.cpp` 启动路径和 `multiwindow_application_test.cpp --session-only` 的断言一致。完整恢复 API 没有生产调用是事实，但不能据此认定现有启动行为是 P1 缺陷，更不能自动启用恢复。
- 生产退出原来只保存偏好。两套 API 同写 `session.json` 的冲突确实存在，但目前不是两条生产写入路径同时竞争；本轮拆开文件，保留未来恢复接口，不改变启动行为。
- JSON 已有全局预算：8 Mi 个宽字符、262144 个值、深度 128；UTF-8 文件读取上限 16 MiB。因此“大量对象”的风险成立，“完全无界”不准确。
- `DefaultNavigationRoot()` 是 Windows 所在驱动器的根目录，失败时才为空路径 This PC。S-05 原报告写成默认 Home 不准确。
- S-10 的自动凭据框不能从当前调用链直接推出：`RestoreSession` 不登记前台网络请求，失败结果只有前台请求才调用认证 UI；而完整启动恢复根本未接入。

## 会话逐项结论

| ID | 核实与必要性 | 本轮处理或后续条件 |
|---|---|---|
| S-01 | 完整恢复没有生产接入，且现有注释和集成测试明确禁止自动导航恢复。属于产品范围，不是当前启动回归。 | 保持原行为。只有明确要求恢复窗口/标签后，才设计入口与恢复策略。 |
| S-02 | `SaveSession` 与 `SaveSessionPreferences` 原来同写一个文件，潜在冲突成立。 | 已补代码：偏好写 `preferences.json`；缺少新文件时兼容读取旧 `session.json`；新文件损坏时返回失败，不复活旧设置；完整会话文件保持原字节。配置目录迁移白名单同步加入新文件。 |
| S-03 | 空路径被跳过，This PC 位置丢失，模型缺陷成立；目前不是生产启动丢窗口。 | 已补代码：只恢复预设所需的前 N 个槽位，槽位中的空路径保留 This PC，忽略额外尾部槽位；缺少的槽位克隆时也保留 This PC。 |
| S-04 | 捕获/序列化比例但不应用到运行时树，成立。 | 已补代码：复用 `RebuildLayoutRoot` 后按捕获顺序应用比例；数量不匹配保持默认，非法比例对应分支回到 0.5。 |
| S-05 | `RestoreLayoutTab` 返回时 root 为空，空 `RestoreWindowTabs` 未调用 loader，成立。 | 已补代码：恢复函数完整建树；空窗口也经过相同恢复函数与 loader；无 loader 时仍写入路径。默认位置保持既有 `DefaultNavigationRoot` 语义。 |
| S-06 | 缺少会话自身 tab/pane/group cap，且写文件未遵守读取预算，成立。 | 已补代码：每窗口最多 128 标签、128 分组，单标签最多 4 窗格；解析、序列化与直接恢复入口均有界；写入前按 JSON 和 UTF-8 预算校验，失败保留旧文件。写入超额对象按上限截取，原有 32 窗口策略保持。 |
| S-07 | undo 使用枚举数字、缺少独立 schema 版本，成立；当前枚举重排导致错误操作尚无证据。 | 交 ops 负责。建议稳定操作名或固定版本映射，兼容已知旧值，未知未来值失败关闭，并保留执行期身份校验。 |
| S-08 | 无系统结束会话保存消息，成立。tray 已在 `AppCore` 共享，不能说保存最后窗口必然丢多窗口 tray；undo 仍每窗口独立。 | 已补代码：全局 owner 处理 `WM_QUERYENDSESSION`，保存最近窗口偏好；确认 `WM_ENDSESSION` 再刷新；取消结束会话不重写。没有合并各窗口 undo，也没有恢复窗口导航。需产品决定是否以及如何保留多窗口 undo。 |
| S-09 | 完整 schema 顶层投影取 `windows.front()`，有明确往返测试支持。当前生产不用完整 schema。 | 保留。将来启用完整恢复前，决定顶层兼容投影来自第一窗口、最近窗口还是全局偏好。 |
| S-10 | 任意路径可进入完整快照并交 loader，成立；当前不存在完整启动恢复，自动凭据框的原述过强。 | 不改变已授权网络导航行为。未来启用恢复时再决定 UNC/搜索/设置等路径的延迟加载规则。 |
| S-11 | 只比较 `showCmd` 会遗漏最小化后的最大化恢复意图。 | 已补代码：捕获同时检查 `IsZoomed` 和最小化的 `WPF_RESTORETOMAXIMIZED`；普通窗口不因残留 flag 被最大化。 |
| S-12 | 偏好读取未经校验的有符号 mask，成立，且属于生产读取路径。 | 已补代码：负值回到 0，超范围位按实际枚举有效位截断。原 selftest 的失效 bit4 改为明确的有效内置链接枚举。 |
| S-13 | 快照确实没有 history/scroll/selection 等；但排序/分组/列宽另有生产 `folder_views` 偏好保存和加载。 | 原述应区分“完整会话字段缺失”和“已存在的按文件夹偏好”。不扩展会话内容；是否恢复临时状态需明确产品要求。 |
| S-14 | 两组测试对空路径语义覆盖不同，问题应修；但某旧 build 没有 exe，不能证明默认 CMake/CI 排除该目标。 | 已统一槽位语义并补定向断言。`add_executable(pulse_json_persistence_test ...)` 当前不是 `EXCLUDE_FROM_ALL`；CI 是否运行需由构建负责人核对实际配置，不能从 exe 缺失推断。 |

关键源码：`src/app/session.cpp`、`session_tab_model.cpp`、`session.h`、`app_main.cpp`、`src/common/user_storage.cpp`。完整恢复 API 的生产调用情况已用 `rg` 交叉核对。

## 其他 App 项

| ID | 核实与必要性 | 本轮处理或方案 |
|---|---|---|
| P-04 | 普通关闭、最后标签重置、TabController 批量关闭都可能直接擦除模型；订阅保留在 `pendingIndexSearches`，成立。只补 `CloseLayoutTab` 会漏掉控制器路径。 | 已补代码：`WindowTabs::before_close` 在实际擦除前执行；生产回调遍历关闭布局的窗格并统一 `TeardownTabSearch`，取消本地/网络/内容搜索、认证、内容选择和对应 pending 项。导航与转移复用该函数。其他标签会话保留，目录列举的共享取消规则不变。 |
| P-07 | 先重置最后标签、后尝试隐藏，隐藏失败仍破坏导航和固定状态，成立。 | 已补代码：`HideAppWindow` 返回成功与否，仅成功后执行原有 Home 重置。 |
| P-09 | `Acquire` 调用两次、转发结果被忽略，成立。 | 已补代码：缓存首次 Acquire 结果；正常自动启动仍允许无路径不转发；转发失败显示提示并返回非零，不静默报成功。没有改为启动第二个主实例。 |
| P-10（App 配套） | 已有 1 秒未 Armed 的 UNC 轮询，本地监视失败缺同等兜底，成立。 | 已补代码：现有兜底覆盖 `VisibleFolderPaths` 返回的全部可见物理路径；仍跳过已 Armed 或已有刷新请求的路径。监视句柄重开/失败退避由 fs 负责。 |
| P-15 | 接收 `WM_COPYDATA` 无来源校验，发送端 wParam 也是 0；外部请求可触发前台 UNC 导航与认证，成立。同完整性同用户边界须保留。 | 未扩大交互改造。来源 PID/映像验证需要相应发送端身份通道，凭据改显式动作也会改变交互；应单独设计并验证合法 CLI/系统集成请求，不宜临时禁止 UNC。 |
| P-17 | 每窗口创建时重复同步加载机器 verb 缓存并后台枚举，成立。上限条件实际可处理 401 个扩展。 | 暂缓：需要共享缓存、取消和按 HWND 投递的明确生命周期。原报告“每窗口都响应注册表失效”不准确：现为单一全局 watcher，绑定 owner 的 mailbox；失效路径不向全部窗口广播。 |
| P-18 | `SetSort/SetGroup/RememberFolderColumns` 在 UI 线程调用 `AppPrefs::Save`，最终同步 flush，成立。具体几十到几百 ms 尚未测量。 | 暂缓：异步 writer 必须统一 `AppCore` 快照/顺序、失败反馈与退出 flush，不能只在线程中引用可变 AppPrefs。 |
| P-24 | 卷标确实不走文件名组件验证，但卷标与文件名不是同一规则；单凭绕过不能证明缺陷或安全边界被突破。 | 不加错误的通用文件名校验。后续根据目标文件系统卷标规则与 Shell 失败反馈补专门验证；ops 侧另行核对。 |
| P-25 | 默认关联按组应用，失败不回滚所有组，事实成立；返回结构会保留 error/changed/external_change。 | 属于设置/关联事务范围，依用户最新指示留到右键菜单设置相关工作后，未改。可用现有 registry store 注入实现跨组快照与定向失败测试。 |
| P-29 | 启动迁移建线程后立即 join，事实成立；发生在窗口创建前，不能描述为交互 UI 卡住。 | 低收益简化建议。把它改成窗口显示后异步迁移会影响配置根目录切换时序，当前不扩大；若要删除线程直接同步调用也应单独处理。 |
| P-30 | HitTestVm 缓存依赖宽高与显式失效；每次绘制会替换当前模型。缓存策略存在，但未给出实际误点复现。 | 保留为风险；先找具体状态变更到输入之间的过期窗口，再补有针对性的失效或完整状态版本，不能假定单个 Tab generation 覆盖全部 UI 状态。 |

P-08/P-19 拖放另由拖放代理负责；P-20 实际属于 `index/content_index.cpp`，已请主代理交 index 代理。设置和右键菜单模块未修改。

## 验证与交接

本代理实际执行：

- 修改前旧 `build_release_clean/pulse_session_windows_test.exe`：6 PASS / 0 FAIL，exit 0，只证明旧序列化测试。
- 修改前旧 `pulse_json_persistence_test.exe --session-this-pc`：未能进入断言，无标准输出，原生退出码 -1073741502（0xC0000142）。保留启动失败信息，不将其记作本轮两个断言失败，也不推断根因。
- 修改后受影响文件 `git diff --check`：通过；存在既有 LF/CRLF 提示，不是差异格式错误。
- 未构建，未运行修改后的 exe；未执行完整 selftest，未操作用户实际窗口、偏好或文件。

主代理后续反馈：修改后的 `pulse_session_windows_test` 已实际运行，13 PASS / 0 FAIL，包含非法分割比例槽位的回归断言。其他带 user32 导入的入口存在 main 前 0xC0000142；最小纯控制台程序可运行，最小 `GetDesktopWindow` 探针也出现同一初始化错误。此证据只能界定环境限制，不能代替尚未运行的 WndProc、监视刷新和注销功能验证。

为单独验证布局模型，14 条现有 `--session-layout` 用例已移至 `src/bench/session_layout_cases.h`，原入口与新增 `src/bench/session_layout_test.cpp` 共用同一份断言。独立控制台入口只需生产 `session_tab_model.cpp`、`app_model.cpp`、`tab_group_model.cpp`、`fs_enum.cpp`，不复制模型实现；静态保留调用链只见 kernel32 路径 API。需由主代理使用 Release `/Gy`、`/OPT:REF` 链接并检查最终导入后确认能否避开环境限制。本轮没有修改 CMake，新增控制台入口仍待独立构建和运行。

建议主代理独立构建并只运行以下入口：

| 目标 | 筛选 | 目的 |
|---|---|---|
| `pulse_json_persistence_test` | `--session-this-pc` | 空 This PC 单/混合/缺失窗格补齐 |
| `pulse_json_persistence_test` | `--session-preferences` | 旧文件兼容、新文件优先、完整文件字节保留、损坏与超预算失败 |
| `pulse_session_windows_test` | 该小目标全部 | 现有完整 schema 往返、对象 cap、mask、最大化恢复标志 |
| `pulse_app_controllers_test` | `--session-layout` | 预设槽位、运行时树、嵌套比例/非法比例、空窗口 loader、关闭 hook、直接恢复 cap |
| `session_layout_test.cpp` 独立控制台入口 | 无筛选，共用 14 条布局模型断言 | 在剔除无用 UI 导入后验证同一生产模型；实际 WndProc 功能仍需原集成入口 |
| `pulse_multiwindow_application_test` | `--app-lifecycle-only` | 实际 WndProc 的关闭清理、托盘失败保留、真实本地无监视刷新、注销保存 |
| `pulse_multiwindow_application_test` | `--session-only` | 生产仍只启动一个 Home、新偏好文件无导航字段 |
| `pulse_user_storage_test` | 迁移用例 | 新偏好文件迁移配套；若不能筛选，应说明整个小目标的验证范围 |

停止条件：相关目标编译且上述定向检查通过后，不扩大到所有历史回归；若独立构建或运行受环境限制，记录具体错误，不能将旧 exe 或源码断言当新实现通过。

剩余产品决策：是否启用完整会话恢复；多窗口 undo 保留方式；恢复 UNC/虚拟路径策略；完整 schema 顶层投影；是否恢复历史、选择和滚动等临时状态。当前代码没有替用户决定这些行为。
