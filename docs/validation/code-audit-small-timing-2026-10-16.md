# 计时与文档诊断模块审查（2026-10-16）

本轮主代理直接完成上一轮未完成的六文件组。先读取当前规则、目标 diff（开始时均为空）和覆盖台账；未重启此前报 502 的子代理。未确认新的功能缺陷；完成一处行为保持的重复实现收敛。

## 全文覆盖

| 文件 | 初始全文行数 |
| --- | ---: |
| src/index/content_timing.cpp | 538 |
| src/index/content_timing.h | 51 |
| src/index/filename_timing.cpp | 92 |
| src/index/filename_timing.h | 37 |
| src/index/document_memory_trace.h | 74 |
| src/index/document_metrics.h | 86 |

六文件共 878 行（改动前）。Core 台账共 157 文件，全文从 98 增至 104，未全文从 59 降至 53。其他模块的台账不由这个数字代表。

## 改动

content_timing.cpp 的 AddDocument 已调用 AddReaderMetrics，却又手写了一遍 AddExtractionMetrics 的提取指标累加。将这部分替换为 AddExtractionMetrics(sum, metrics)，保留读取指标累加与上层调用方式。

修改前用脚本提取两处赋值表达式，确认 24 个字段逐个同名相加、字段集合完全一致。没有改变计数语义、字段输出、日志路径、频率或容量；既有调用内联辅助函数，不引入新分配、锁或运行时配置。没有把去重记作 bug 修复。

## 审查要点

- ContentTiming 以 mutex 保护计数和快照，工作线程只更新内存，序列化及文件 I/O 在快照锁外。固定 16 worker、8 slow sample、16 failure/error 槽和路径截断限制增长；生产 content_task_search 当前最多 6 个 worker，未发现正常路径超出槽导致漏完成计数。
- 时间源采用商/余数换算，计时差用 Since 防止负差下溢。阶段/lane API 验证范围，token 查找失败不修改槽；重复 Finish 后不再写终态。
- 仅枚举完成且 completed == queued 时声明 lane 完成。文件 bytes/metrics 的 EndFile 与独立记录接口可以分别使用；当前生产调用只通过 EndFile 传递提取指标，没有发现重复计数调用。
- JSON 路径与控制字符转义、768 wchar 截断、慢样本筛选、轮转与独立 summary 均已阅读。内容计时默认写 LOCALAPPDATA 下进程专属日志，不能把它与显式启用的 filename/parser diagnostics 混为“全部默认关闭”。
- FilenameTiming 是 worker-owned，普通模式由 IndexDiagnosticsEnabled 提前返回；End 保存的 reason 在已查调用处是固定字面值，未发现临时字符串悬垂。每分钟写入和 1 MiB 轮转为既有行为。
- DocumentMemoryTrace 在构造时选显式诊断目录，采样使用栈缓冲，路径按 UTF-16 JSON escape 保留代理对。按 sequence 和 8 MiB 上限停止写入；phase 在 host 三处为固定字符串，输入路径由 host 限长。写入失败关闭句柄，不继续输出。
- DocumentReadMetrics 区分提取/读取指标、累计值、最后一次失败和峰值；不能直接相加重叠 wall-time 得到总耗时。DocumentMetricScope Finish 可重复调用但只累加一次。

必要局部依赖：content_task_search.cpp 的 worker 数与 EndFile 调用；document_host_main.cpp 的三处 Sample 及路径限制；document_reader.cpp 的 AddExtractionMetrics 调用；index_engine.cpp 的 FilenameTiming::End 调用点；CMake 对测试和生产库的挂接。上述依赖未计作新增全文。

## 实际验证

- 24 字段静态等价核对通过。
- `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_content_timing_test pulse_index"`：退出 0，受影响静态库、专项测试与 Pulse.Index 链接成功，无新增编译警告。
- `build\pulse_content_timing_test.exe`：23 PASS / 0 FAIL，退出 0。既有专项用例验证指标聚合、1200 次并发记录、转义、终态、轮转、不可写目标和句柄释放。使用 bench_data 隔离目录，退出清理；未改真实用户日志或配置。
- `git diff --check -- src/index/content_timing.cpp` 通过。没有新增测试、全量构建、全量 selftest 或历史回归；构建和运行采用此前可用的 require_escalated 执行环境。

## 限制

没有动态验证 filename/parser 日志的磁盘故障、真实解析器内存极限或所有代码页。日志短写可能留下不完整最后一条记录，仍属于既有 best-effort 行为；本轮没有把诊断写入升级为可靠存储。主动 force 快照可绕过一秒节流；正常调用约束与强制操作需区分。保留现有未提交修改，未提交或发布。
