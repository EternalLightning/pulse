# 应用层续审（2026-10-15）

本轮以先前应用层报告、逐文件覆盖表和当前工作区代码为依据，保留既有未提交修改；未修改 CMake、ops、ui 或 index。全文读取代表覆盖范围，不代表无缺陷证明。

## 修复与实际验证

1. `session_tab_model.cpp:139`、`tab_controller.cpp:100`：持久化分组 ID 为 INT_MAX 时，原 `max_group + 1` 和后续 `next_tab_group_id++` 存在有符号溢出。新增 `tab_group_id.h` 统一安全递增、回绕，并在创建时跳过已存在的 ID；保留已有分组编号和引用。所有正整数均已占用时返回 0，调用方不创建分组。
2. `app_commands.cpp:821–891`：注册表线程原先从全局取得 stop handle，停止等待两秒后 UI 关闭该句柄，即使工作线程仍在初始化或等待。现在创建线程前复制 stop handle 到线程独占上下文，线程退出时 RAII 关闭自身句柄；创建失败也释放相关资源。`WaitForMultipleObjects` 的 WAIT_FAILED 现在退出循环，避免错误句柄导致反复投递/自旋。停止仍保持两秒等待上限。

实际执行 `cmake --build build --target pulse_json_persistence_test pulse`，退出 0，输出未出现新增编译警告。既有依赖修改触发 106 个计划步骤，包含 Pulse.Index；没有执行全量构建脚本。

`build/pulse_json_persistence_test.exe --session-group-id`：5 PASS、0 FAIL、退出 0；包含既有 This PC 恢复两项，以及 INT_MAX 恢复后避冲突、INT_MAX 分配回绕、非法计数器修正三项。不创建配置夹，不修改用户偏好，没有运行完整 persistence/selftest。

注册表修复已通过生产目标编译与所有权静态检查，**尚无动态超时复现通过证据**。尝试在 `bench_data/app_continuation/registry_stop_test.cpp` 从当前生产实现抽取停止代码，以延后 worker 启动模拟超时：MSVC 编译完成，但运行在初始句柄/上下文前置检查退出 1，未输出 PASS。没有凭该退出推断产品或沙箱根因，没有将探针算作通过。该探针也不等同于真实注册表卡顿/退出竞争验收。

## HWND 异步载荷：仍未修复

> 2026-10-16 后续：下述两个结果通道已改为窗口会话邮箱，并通过定向生命周期测试；见 `code-audit-app-async-2026-10-16.md`。以下保留修复前证据，不代表当前仍使用原来的堆指针投递。

- `app_commands.cpp` 的 PrefetchStaticVerbs、SeedShellVerbCache 与 `duplicate_scan_ui.cpp` 的 RequestDuplicateVolumeCache 仍使用 detached worker，通过 PostMessage 投递堆对象，仅投递失败时自行释放。
- `app_main.cpp` 的 WM_SHELL_VERBS 和 WM_DUP_VOLUMES 分支在正常派发时释放载荷；WM_DESTROY 只显式排空 WM_ARCHIVE_RESULT，没有等待上述 detached producer 或排空其结果消息。
- 因而存在“投递成功、窗口随后销毁、消息未交给载荷释放分支”的泄漏路径；HWND 复用也缺少会话身份校验。这里是源码生命周期证据，没有动态内存泄漏计数或 HWND 复用复现，不宣称已验证 UAF/崩溃。
- 仅加入 IsWindow 或在销毁时一次性 drain 不足以关闭 producer 并发窗口。后续需要窗口拥有的结果邮箱/关闭令牌与同步投递停止机制，并验证关闭前后完成、排队未派发、快速重建等时序。注册表 stop handle 修复不解决这些载荷风险，也未声称完全消除旧 HWND 的失效通知竞争。

## 本轮实际阅读覆盖

- 重新全文读取：`session_tab_model.cpp`、`tab_controller.h`、`tab_group_model.cpp`、`duplicate_scan_ui.cpp`。
- 新增全文覆盖：`search_options.cpp`、`app_column_view.cpp`；新增文件 `tab_group_id.h` 全文读取。
- 局部读取并保留部分状态：`app_commands.cpp`（文件头、静态菜单缓存/注册表 worker）、`app_main.cpp`（结果派发、WM_DESTROY）、`tab_controller.cpp`（创建分组入口）、`app_model.h`（分组模型声明）、`json_persistence_test.cpp`（本轮定向测试段）。
- 补读但输出存在截断，**不升级为全文**：`app_change_tracking.cpp`、`context_menu_prefs.cpp`、`folder_sizes.cpp`。其中 context_menu_prefs 补读 JSON 解析、迁移与持久化尾段；未补齐输出空隙。
- `code-audit-app-coverage-2026-10-15.md` 原表列出 app 生产文件 163 项，其中 133 项全文、30 项部分/仅扫描。按本轮确认新增的两个旧文件全文覆盖，原表仍有 28 项未全文读取；计入新 helper 后是 164 项、136 项全文、28 项未全文。原表未就地覆盖，本节是增量记录；测试文件不计数。

剩余范围包含 app_main/app_input/app_commands/app_runtime/app_model 等大文件，以及 app_navigation/app_ops_ui/content_results_ui/context_menu 等模块。不能据本轮声明 app 全部生产代码已逐行审完。未作真实 SMB/RaiDrive、长时间注册表阻塞或 UI 视觉验收。
