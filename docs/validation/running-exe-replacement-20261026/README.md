# 运行中 EXE 替换与关闭提醒排查（2026-10-26）

## 1. 错误提示和备份残留：已复现并修复

原生 Windows 隔离测试使用复制到测试目录的真实 EXE，通过 `CreateProcessW(CREATE_SUSPENDED)` 保持其映像占用。复制、移动均复现原问题：旧 EXE 可以被改名为 `.pulse-backup-*`，新文件随后提交，但删除旧映像失败，最终只显示“替换备份未能恢复或清理”，错误码为 `E_FAIL`。移动还会先移走源文件。

修复在选择替换后、复制数据或改名之前，用不截断文件的 `FILE_WRITE_DATA` 加身份校验句柄检查普通目标文件。运行中的映像在此返回 `ERROR_SHARING_VIOLATION`（`0x80070020`），直接进入已有占用重试/取消提示；原 EXE 和源文件均保留，不产生临时文件或备份。

允许替换时，句柄一直保留到提交、备份删除或回滚完成，防止检查后又加载映像。备份清理和回滚复用该已验证句柄。正常替换仍保留原有回滚保护。

实现：`src/ops/ops_manager.cpp`、`src/ops/destination_guard.h`。
回归：`src/bench/replace_running_exe_cases.h`，由 `pulse_ops_test --filter running-exe-replacement` 独立运行。

## 2. 单窗口点 × 未提醒：本次具体路径尚未复现

用户明确：当时只有一个主窗口，未启用后台运行，通过主窗口右上角 × 关闭。读取当前配置时，`keep_running_on_close` 和 `global_search_enabled` 均为 false。

这条源码路径是 `RequestAppWindowClose` → `RequestApplicationExit` → `ConfirmWindowShutdown`。正在执行或排队的文件操作、未结束的归档操作会触发确认。新增真实单窗口传输测试，关闭两个后台设置后发送 `WM_CLOSE`，确认对话框出现；取消后窗口与任务均保留。该项通过。

截图中的任务已经进入 Failed；失败任务本身不等于仍在执行的任务。原实现的隔离复现中，工作线程正常收尾后也会移除活动恢复记录。修复后的复制、移动分别验证了：失败后、重试前恢复日志已清理，以及停止管理器后重新读取日志没有恢复任务。因此，不能据此认定用户那次通过 × 关闭后再次出现恢复提示的具体原因已经修复。当前未找到可供核对的那次遗留恢复日志。

同一检查中发现另外两个漏提醒分支，已补齐：关闭到托盘未启用时，全局搜索引发的后台隐藏，以及多窗口中保留任务的后台隐藏。确认说明任务会继续在后台运行，取消保留窗口。**显式开启关闭到托盘时仍不提示**，遵循用户原设计。真正退出时继续使用原来的停止任务确认。

实现：`src/app/app_exit.cpp`、`src/app/exit_confirmation.h` 及对应中英文字符串。
回归：`pulse_multiwindow_application_test --background-only`，增加单窗口 ×、全局搜索和多窗口确认断言。

## 实际验证

| 检查 | 结果 |
| --- | --- |
| MSBuild Release：`pulse_ops_test`、`pulse_multiwindow_application_test`、`pulse` | 构建完成，无新增编译警告 |
| `pulse_ops_test --filter running-exe-replacement` | 19 通过，0 失败；覆盖复制/移动占用、原文件保留、无残留、原操作重试、日志清理和进程退出后替换 |
| `pulse_ops_test --filter transfer-updates` | 12 通过，2 失败；正常覆盖、临时文件策略、取消、复制重试及失败回滚相关断言通过 |
| 撤去本次替换修复的独立对照程序，同一 `transfer-updates` 筛选 | 同样 12 通过，2 失败；失败均为锁定第二个目录移动及其重试断言，未修改或弱化测试 |
| `pulse_multiwindow_application_test --background-only` | 19 通过，2 失败；单窗口确认、托盘开启时无新增确认、全局搜索确认、多窗口确认和异步退出相关断言通过；托盘隐藏及隐藏后进度窗口断言失败，该测试未整体通过 |
| 本次相关文件 `git diff --check` | 通过 |

UI 测试最初在仓库目录启动时，因夹具 `create_directories` 权限错误退出；诊断确认后，改在已可写的构建目录运行。后续托盘测试中窗口仍可见，符合 `HideAppWindow` 在托盘图标注册不成功时拒绝隐藏的保护分支；托盘注册未成功的具体原因未继续定位，不能当作托盘隐藏验证通过。交互验证使用真实窗口和定时键盘自动化，没有做逐像素截图验收。

UI 测试主进程已经退出；其残留的 `Pulse.Index.exe --network-agent` 导致 `Start-Process -Wait` 继续等待。核对测试父进程 39148、构建目录和隔离夹具目录后，仅清理该测试子进程，取得测试退出码 1。未结束用户安装的 Pulse。

证据：`running-exe-before.log`、`running-exe-after.log`、`transfer-after.log`、`transfer-baseline.log`、`close-window.log`、`build.log`。

没有运行完整历史回归，没有替换安装目录中的程序。现有其他会话修改保留。第三项的用户原始单窗口漏提示与重启恢复提示仍需进一步复现，不能标为已解决。
