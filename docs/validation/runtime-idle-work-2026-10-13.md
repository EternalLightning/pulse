# 暂存区空转与后台旧快照持有优化（2026-10-13）

## 已确认的开销

1. PumpClipboardAvailability 不仅由维护定时器调用，也通过 CanPasteIntoCurrent 在 BuildVm 中执行。原先无论暂存区是否变化，每次都会遍历最新批次并复制全部 exists 路径，然后才比较输入是否相同。
2. PumpTrayMetadata 每个维护周期调用 ApplyMetadataResults 和 BeginMetadataQueries。即使没有完成结果、也没有待查询条目，仍会扫描全部暂存批次。
3. WorkerPool 的旧清理只删除排队 group 的失效订阅。已经开始运行的 group 仍可能持有已取消/被替换订阅的 cached_snapshot，直到底层生产任务返回。

## 改动

- StagingTray 提供可用路径修订号；收集、删除、清空、恢复、赋值、移动和存在性确认变化后失效。只有尺寸等元数据变化时不使路径缓存失效。
- ClipboardAvailability 新增版本化更新入口，输入修订和剪贴板状态不变时不调用路径收集器。仍按原有周期后台检查源文件，仍通过 generation 拒绝过期结果。直接传路径的原入口保留并使修订快捷路径失效，支持混合调用。
- 元数据查询扫描确认没有待办后记住该状态，新增条目/恢复/入队失败重试会重新唤醒；没有有效完成结果时立即返回。仍处于 Resolving 的请求不重复提交。
- 统一锁内 PruneDirectories 清理排队和运行中 group 的失效订阅。Cancel、Refresh、Project、LoadPaths 共用；保留有效订阅以及 worker 已持有且正在使用的局部 shared_ptr。

修改范围为 app_model.*、clipboard_availability.h、app_runtime.cpp、app_worker.cpp 及对应定向测试。没有降低动画帧率或拉长现有有效性检查周期，没有把文件检查移到 UI 线程。

## 验证

构建 pulse、pulse_staging_tray_test、pulse_clipboard_availability_test、pulse_fs_worker_cancel_test、pulse_pane_animation_application_test 成功。

| 命令 | PASS | FAIL |
| --- | ---: | ---: |
| pulse_clipboard_availability_test | 25 | 0 |
| pulse_staging_tray_test | 36 | 0 |
| pulse_fs_worker_cancel_test --projection | 68 | 0 |
| pulse_pane_animation_application_test | 33 | 0 |

合计 162 PASS、0 FAIL。日志位于 bench_data/runtime-waste-clipboard.log、runtime-waste-tray.log、runtime-waste-worker.log、runtime-waste-ui.log。受影响源码 git diff --check 通过。

新增检查包括：

- 连续 10,000 次输入不变的更新只调用一次路径采集器；无关修订但路径相同不取消已验证状态；路径变更拒绝旧结果；混合入口和剪贴板序列变化保持失效规则。
- 有限批次查询、耗尽后休眠、入队重试、新增后唤醒、存在性与尺寸变化区别、删除/清空、复制与移动的失效边界。既有会话恢复和陈旧元数据回归保持通过。
- producer 暂停期间，用 weak_ptr 检查取消最后订阅、取消一个同时保留另一个订阅，以及 Refresh 替换旧 generation 三种场景。旧快照在线程继续前释放；有效订阅正常完成，旧 generation 不发布。
- 原有应用级动画推进/终止、焦点交接状态、截止时间与隐藏停帧检查保持通过。

## 限制

这里的 10,000 次更新是确定性的采集次数验证，不是帧率基准。后台生命周期使用隔离 checkpoint，不能替代真实 SMB/第三方文件系统长时间阻塞测量。没有测量整进程 CPU、常驻内存或实际高刷新率帧时间变化；也没有把正在被任务实际使用的数据强制释放。
