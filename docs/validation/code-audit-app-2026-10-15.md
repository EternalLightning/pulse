# 应用层 / 模型层审查记录（2026-10-15）

本记录基于当前含大量既有未提交修改的工作区。扫描范围为 src/app 与 src/model；没有把目录扫描等同于逐文件全文审查。未修改 CMakeLists.txt，保留原有改动。

## 已修复并定向验证

- `src/model/batch_rename.cpp:104`：模板 `{n:999999999999999999999999999999999999}` 原先在最后 clamp 前进行 int 累乘，存在有符号溢出。现在每步限制为已支持的最大补零宽度 9。
- `src/model/batch_rename.cpp:66`：旧式 `digits` 未限制，过大值可超过固定输出缓冲区；统一将宽度限制在 0..9。
- `src/model/batch_rename.cpp:180`：`start + static_cast<int>(index)` 可在 INT_MAX 后溢出；使用 int64_t 编号和匹配的格式化，极端 size_t 输入按可表示范围保护转换。
- `src/app/selftest_1b2.cpp:4400` 附近新增超长模板、legacy digits=100、INT_MAX+1 回归断言。

实际验证：`bench_data/app_review/batch_padding_test.cpp` 链接真实 `src/model/batch_rename.cpp` 和 `src/fs/fs_enum.cpp`，MSVC C++20 `/W4 /permissive- /utf-8` 编译，无新增警告，进程退出 0。七个断言覆盖普通三位宽度、超长宽度、前导零宽度、legacy 超大/负宽度、INT_MAX+1、INT_MIN+2147483648。日志只输出统一 PASS；没有运行完整 selftest 或声称 GUI 验收完成。

## 原生窗口入口及复用判断

| 入口 | 当前行为 | 判断 |
|---|---|---|
| `app_main.cpp:323` | 图形初始化失败 MessageBox | 发生在 D3D/D2D 初始化失败后，保留原生错误入口有合理降级用途 |
| `app_main.cpp:426,437,442` | 清理诊断、隐私确认、是否包含服务日志 MessageBox | 可纳入统一确认组件，包含 Yes/No/Cancel 的入口须保留三个结果语义 |
| `app_main.cpp:518` | 恢复未完成操作 MessageBox | 可复用确认组件，但需要核对 WM_CREATE 阶段依赖是否已初始化 |
| `app_input.cpp:2159,2176,2205` | 设置错误、颜色输入错误、壁纸比例输入错误 | 可改为既有错误呈现/输入内校验；本轮未改变视觉行为 |
| `app_input.cpp:2568` | 清空最近记录确认 | 可复用既有确认组件 |
| `archive_dialog.cpp` | 原生 EDIT/BUTTON 承载的压缩输入窗口 | 单独实现，需与通用 hosted-edit/对话框生命周期继续对照，不因 CreateWindow 本身判定缺陷 |
| `new_item_dialog.cpp` | 自绘外框及原生编辑控件 | 已有主题、DPI、工作区边界限制和退出重投 |
| `settings_value_dialog.cpp` | 主题输入框，高对比时原生外框 | 高对比分支属于有意降级；不要机械删除 |

`archive_dialog.cpp`、`new_item_dialog.cpp`、`settings_value_dialog.cpp` 的 GetMessage 循环均已有 WM_QUIT 重投；`archive_operation_ui.cpp` 也有 quitting/quit_code 保存重投，未发现同类吞退出证据。没有为了只复用新 helper 扩大改动。

`src/app/batch_rename.h`、`search_query.h`、`places.h` 是 model 类型/函数的 using 兼容层，不是重复业务实现。`app/builtin_menu_items.cpp` 是命令映射，不能因为同名直接删除。

## 仍需处理或复现的风险

1. **UI 线程路径探测（源代码证据）**：`src/ui/batch_rename_dialog.cpp:280` 在编辑更新时同步调用 `model::PreviewBatchRename`；后者逐项 GetFileAttributesW 探测目标冲突。大量选择或离线 UNC 可能阻塞界面。建议分离纯规则预览与可取消、带代际的冲突探测；需要 UI/model 联动，已报告主代理。本轮未作真实 UNC 延迟测量。
2. **注册表观察线程停止（静态风险，未动态复现）**：`app_commands.cpp:871` 对线程等待两秒后不检查结果，就关闭 stop HANDLE；若线程未结束，后续等待会接触已关闭句柄。应让线程上下文持有 stop 句柄并区分 WAIT_TIMEOUT/WAIT_FAILED；未将其描述为已复现崩溃。
3. **HWND 异步投递生命周期（审查线索）**：`duplicate_scan_ui.cpp:24` 和 `app_commands.cpp:814,889` detached 线程向 HWND 投递堆载荷，只有 PostMessage 失败时释放。仍需专门检验窗口销毁/队列清理/句柄重用；未凭模式直接断定 UAF。

## 实际读取覆盖与限制

- 完整读取：AGENTS.md；model/batch_rename.h、batch_rename.cpp；app/home_catalog.cpp、app/app_exit.cpp；app/batch_rename.h、search_query.h、places.h。
- 局部正文或当前 diff：app/address_search.cpp、app_model.cpp、app_runtime.cpp、app_worker.cpp、archive_navigation.cpp、app_commands.cpp、app_main.cpp、app_input.cpp、archive_dialog.cpp、new_item_dialog.cpp、settings_value_dialog.cpp、duplicate_scan.cpp、duplicate_scan_ui.cpp、global_search_window.cpp、selftest_1b2.cpp；model/search_query.cpp、builtin_menu_items.cpp；app/builtin_menu_items.cpp。较大批量输出出现截断，因此这些文件不声称全文覆盖。
- 全目录路径清单和风险模式扫描：src/app、src/model 所有 cpp/h；线程/detach/CreateThread、原生窗口/MessageBox、消息循环、TODO/FIXME；相关测试及 CMake 目标定位。
- 仅符号索引：session.cpp、session_tab_model.cpp、places.cpp、settings_controller.cpp、folder_view_prefs.cpp、saved_search.cpp；其他文件仅目录/模式扫描，没有逐文件全文审阅。

因此这是一轮范围内重点审查与明确修复，不能作为“src/app、src/model 全部生产代码已逐行审完”的证明。真实 SMB/RaiDrive、退出竞争和 UI 外观尚未动态验收。

## 后续补读与修复

- 补读发现并修复 `session_tab_model.cpp` 将 This PC 空路径窗格跳过的问题。删除这一错误过滤，保留原窗格顺序和焦点。
- `pulse_json_persistence_test --session-this-pc` 定向运行：单个 This PC 窗格、This PC 加普通文件夹双窗格，共 2 PASS，退出 0。该筛选不创建/删除配置测试夹，没有混入此前完整测试的沙箱清理权限问题。
- `src/model` 的全部生产 cpp/h 已全文读取；应用层已补读多项模块，精确状态见 `code-audit-app-coverage-2026-10-15.md`，该清单替代上方较早的覆盖概述。
- 进一步核对 WM_DESTROY：当前只显式清理 WM_ARCHIVE_RESULT；静态菜单与卷列表 detached 投递仍没有对应 join/drain。风险未动态复现。
- 托盘菜单还有 `tray_controller.cpp` 的 TrackPopupMenu 原生入口，属于后续统一菜单样式范围。
- `RestoreWindowTabs` 的 `max_group + 1` 与 TabController 的后增 group ID 对持久化 INT_MAX ID 存在溢出边界，尚未修改或验证；正常 UI 产生该值需要极长使用时间，手改/损坏配置可达。
