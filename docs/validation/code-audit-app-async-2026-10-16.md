# 应用层异步结果生命周期复核（2026-10-16）

范围：接续 `code-audit-app-continuation-2026-10-15.md` 的 HWND 异步载荷未修复项，仅处理静态 Shell verb 枚举与重复文件扫描卷列表两个结果通道。保留工作区已有未提交修改。

## 源码证据与改动

原 `PrefetchStaticVerbs`、`SeedShellVerbCache`、`RequestDuplicateVolumeCache` 在 detached 线程分配结果，将所有权交给 `PostMessageW` 的 lParam；只有投递失败或 UI 消息派发时释放。成功排队之后销毁窗口，会绕开释放分支；旧 producer 仅持 HWND，无法区分窗口会话。

新增 `src/app/window_result_mailbox.h`，沿用项目中共享 mailbox 的结果所有权模式，并为窗口消息增加同步关闭约束：

- mailbox 持有结果，消息仅携带递增的会话身份，lParam 为 0。消息被丢弃不再丢失堆对象所有权。
- `Publish` 与 `Close` 使用同一互斥锁；关闭清空结果、撤销 HWND。关闭返回之后旧 producer 无法再次投递。没有在锁内执行枚举或等待工作线程。
- `WM_CREATE` 建立新 mailbox，`WM_DESTROY` 首先关闭两个 mailbox，`WM_NCDESTROY` 幂等关闭兜底并清除 userdata（覆盖创建失败绕过正常销毁清理的路径）；旧线程只持旧 mailbox，不捕获 AppState。旧通知即使遇到同值 HWND，也无法消费新会话结果。
- 投递失败立即移除并释放新结果；Shell verb 预热循环在失败或关闭后退出。已在执行的注册表/卷枚举可完成，其结果会被关闭 mailbox 拒收。

生产接入文件：`src/app/app_state.h`、`src/app/app_commands.cpp`、`src/app/duplicate_scan_ui.cpp`、`src/app/app_main.cpp`。新增聚焦测试 `src/bench/window_result_mailbox_test.cpp`，CMake 仅新增 `pulse_window_result_mailbox_test` 目标。

## 实际验证

执行 `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_window_result_mailbox_test"`，退出 0，没有新增编译警告。

执行 `build/pulse_window_result_mailbox_test.exe`，11 PASS、0 FAIL、退出 0。使用真实 Win32 message-only 窗口及真实 PostMessage/PeekMessage，检查关闭前消费、重复通知、结果释放、已排队未派发时关闭、旧会话通知拒收、新会话结果保留、旧 producer 拒收、门控工作线程在关闭后完成、200 次并发发布/关闭，以及无效 HWND 投递失败释放。

同值 HWND 的重建风险通过在相同真实 HWND 上建立新 mailbox 来确定性模拟；没有宣称 Windows 实际分配器在测试中复用了 HWND。载荷释放以 weak_ptr 生命周期验证，没有运行进程级泄漏检测器。检索 src/app 与 src/bench 没有发现额外手工投递这两个消息的旧堆载荷测试。创建失败兜底通过源码检查，未注入主窗口渲染器初始化失败。

受影响生产目标 `pulse` 的统一构建由主审查任务执行，结果由主任务补记；上述测试独立验证实际 mailbox 实现，不等同于已完成真实 Shell 菜单/卷枚举 UI 验收。未运行全量构建脚本、selftest 或其他历史套件。源码定向 diff 检查通过，工作区已有换行提示不代表编译错误。

## 限制与剩余项

整合补记：主任务随后完成 `cmake --build build --target pulse`，退出 0，实际编译以上生产接入文件并成功链接 Pulse.exe，输出没有新增编译警告。详见 `code-audit-round-2026-10-16.md`；不将编译当作 UI 验收。

这两个通道之外的 HWND 异步消息没有在本次修改中统一迁移。此前注册表 watcher 的 `WM_SHELL_CACHE_INVALIDATE` 仍是独立通知路径，其旧 HWND 竞争不在本次结果载荷修复的结论内。没有修改枚举算法、阻塞 provider 的取消能力、消息队列耗尽时原有 pending 状态恢复行为，也没有宣称所有异步生命周期问题均已消除。
