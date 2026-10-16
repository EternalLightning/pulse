# ContentSearchClient 查询线程生命周期审查（2026-10-16）

## 范围与发现

本次只审查并修改 `src/index/content_search_client.cpp/.h` 的查询线程所有权回收，以及 `src/bench/content_index_test.cpp` 的定向回归入口。保留工作区其他既有修改。

`ActiveQuery` 持有 `std::thread`，析构时会 join；查询 lambda 同时捕获该 `ActiveQuery` 的 shared_ptr。`Worker` 原先移除 map 项后，只让 `replaced` / `retired` 局部 vector 自行析构。查询线程先置 `done = true`，再释放 lambda 捕获；如果协调线程在这两步之间释放最后一个外部 owner，最后的析构会发生在查询线程本身，调用 join 自身并触发 terminate。替换未退出查询时也存在同一所有权问题。

## 修复

- `Worker` 在释放 `replaced` 和 `retired` owner 前显式 join，且位于 `state_mu_` 锁外。
- 对被替换的查询先发出本地取消事件，防止重复排队的 generation 在此前 Cancel 尚未看见该查询时留下活跃订阅。
- `Cancel` / `SearchAsync` 不执行 join；UI 调用仍然只发信号与排队。`Stop` 保留原先的协调线程退出等待。

## 回归设计与限制

使用 `PULSE_TEST_CLIENT_LIFECYCLE=1` 仅运行本次用例。父测试启动同一测试程序的独立子进程，子进程以不启用持久代理的 Instant 配置运行真实 `Worker`，通过 friend 注入一个已经 done、尚在事件门处等待退出的 `ActiveQuery`。用例检查 Cancel 返回、真实 Worker 从 map 回收后仍持有所有权、释放门后查询线程实际退出和对象最终销毁。独立子进程将 terminate 转成退出码 91，并有超时终止保护。

该用例无需真实 agent、配置文件或服务，不修改用户索引或设置。它直接覆盖 `retired` 路径；`replaced` 路径通过相同所有权约束和调用点检查覆盖，尚未做真实代理订阅的端到端验证。所有权观察窗为 500 ms，有操作系统调度的边界；修复前对照结果需与实际退出码一起记录。

## 实际验证

- 仅构建 `pulse_content_index_test`（包含其已有依赖），使用 `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_content_index_test"`。最终构建退出 0，无新编译警告。未运行其他测试入口。
- 测试设置命令进程内的 `TEMP` / `TMP` 为仓库 `bench_data/client-lifecycle-temp`，设置 `PULSE_TEST_CLIENT_LIFECYCLE=1` 后运行 `build/pulse_content_index_test.exe`。
- 修复前对照：保留全部测试代码，只临时移除本次新增的两个 join 块。首次子进程退出 `3221226505`（`0xC0000409`），父测试 1 PASS / 1 FAIL、退出 1。将 terminate handler 同样设置于模拟查询线程后，同一原始生产逻辑子进程退出 **91**，父测试仍为 **1 PASS / 1 FAIL，退出 1**，确认触发线程内 terminate。随后只恢复本次两个 join 块。
- 最终修复后：定向构建退出 **0**；子进程退出 **0**；父测试 **2 PASS / 0 FAIL，退出 0**。
- `git diff --check` 通过。未运行全量构建、自测套件、真实代理订阅或 UI 验证。

环境限制：普通沙箱构建停在 VerifyGlobs，停止本次创建的 cmake/ninja 进程后，同一构建命令经提权工具执行成功。普通沙箱下测试进程初始化退出 `0xC0000142`、没有测试输出；同一测试命令在提权工具环境完成上述红绿对照。这些沙箱故障与内容相同的可运行对照分开记录，未修改产品逻辑来规避环境问题。
