# 复制速率与 ETA 定向测试结果

日期：2026-10-12。此文件整理此前已经执行的工具 stdout，不是新的测试执行日志；未为了归档重复运行测试。原始 stdout 位于本聊天工具输出，未单独保存原始日志文件。

## 复现与改动

审查对象为 `src/ops/transfer_rate.h` 的 `TransferRateEstimator`，独立测试为 `src/bench/transfer_rate_test.cpp`，CMake 目标为 `pulse_transfer_rate_test`。

旧估算器在速率 EMA 后再次独立平滑 ETA，使 ETA 与同屏速率及剩余字节数脱节。确定性恢复轨迹的旧输出为：

```text
[DIAG] recovery speed MiB/s=239.813 remaining MiB=20769 ETA seconds=239
```

按相同速率计算，剩余时间应为约 87 秒。修复保留 4 秒窗口、速率 EMA、100 毫秒采样节流和 750 毫秒预热；ETA 使用同一速率计算，整段观测无字节进展时清除旧估计，总量变化在采样节流内更新 ETA。复制、暂停、取消和撤销策略未改动。

## 旧估算器：已执行结果

命令：`build\pulse_transfer_rate_test.exe`。退出码：1。此时测试文件包含 16 项检查，结果为 11 PASS / 5 FAIL。以下摘录此前 stdout：

```text
[FAIL] steady ETA agrees with remaining bytes and displayed speed
[FAIL] slowdown and rapid recovery keep ETA consistent with speed
[DIAG] recovery speed MiB/s=239.813 remaining MiB=20769 ETA seconds=239
[FAIL] observed full window without byte progress clears stale estimates
[FAIL] progress after a stall restores a usable consistent estimate
[FAIL] changed totals update ETA even inside sampling throttle
5 failure(s)
```

相同旧估算器还通过独立 MSVC 构建执行得到同样的 5 个失败。两次旧算法执行均为此前工作中已完成的执行，归档时未重跑。

## 修复过程中的测试校正

首次修复后构建运行仍有 1 个失败，涉及总量变化检查：测试错误地要求“总量收缩并触发完成采样之前”和“完成采样之后”的速率完全相等。完成采样本身允许更新速率，此要求超出该检查目标。随后将比较基准移到完成采样之后，保留对下一次节流内总量增长的速率保持及 ETA 一致性断言。该校正没有删除或放宽 ETA 断言。

校正后 16 项检查全部通过；再新增两项离散回调边界检查：短间隔重复字节计数不能立即当作零吞吐，稀疏回调使用实际经过时间计算吞吐。

## 最终估算器：已执行结果

构建和运行命令：

```text
cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_transfer_rate_test && build\pulse_transfer_rate_test.exe"
```

退出码：0。MSVC x64 构建成功，结果为 18 PASS / 0 FAIL。此前口头汇报误将 Check 函数定义也计入检查数量，写成 19 PASS；归档按实际 stdout 更正为 18 PASS。以下为此前最终 stdout 中的全部测试及诊断行：

```text
[PASS] warmup does not invent speed or ETA
[PASS] steady byte progress yields steady throughput
[PASS] steady ETA agrees with remaining bytes and displayed speed
[PASS] slowdown and rapid recovery keep ETA consistent with speed
[PASS] four-second window and EMA recover after sustained faster progress
[DIAG] recovery speed MiB/s=239.813 remaining MiB=20769 ETA seconds=87
[PASS] a repeated byte count between chunks does not imply zero throughput
[PASS] observed full window without byte progress clears stale estimates
[PASS] progress after a stall restores a usable consistent estimate
[PASS] pause reset discards pre-pause speed and ETA
[PASS] resume warms up without counting pause time
[PASS] resume estimates only new transferred bytes
[PASS] total shrinking to processed bytes clears ETA immediately
[PASS] changed totals update ETA even inside sampling throttle
[PASS] unknown total has no remaining-time estimate
[PASS] same-tick completion clears ETA
[PASS] byte rollback starts a fresh estimate
[PASS] completion during warmup has no remaining time
[PASS] sparse callbacks measure transferred bytes across their elapsed interval
0 failure(s)
```

旧算法的基础完成及总量收缩检查已经通过；它们是覆盖边界，不是本次已复现缺陷。

## 验证范围与限制

上述检查为确定性累计字节/时钟轨迹，不包含真实跨盘约 22 GB ISO 复制、磁盘或缓存吞吐测量、UI 视觉验收。缺乏回调时估算器不会自行获知实时吞吐，不能把没有回调解释为磁盘实时速度为零。

现有图表每 250 毫秒采样已平滑的速率，保留约 30 秒；峰值标签记录任务历史最大速率。平台、高峰或下降不能单凭截图证明磁盘瓶颈。

当前 CopyFile2 参数未指定 `COPY_FILE_NO_BUFFERING`。微软文档说明该标志绕过系统缓存，且不建议暂停使用该标志的复制；本次未改变复制参数。[COPYFILE2_EXTENDED_PARAMETERS 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-copyfile2_extended_parameters)
