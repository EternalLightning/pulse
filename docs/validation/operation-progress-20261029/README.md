# 文件操作状态标签与解压速度采样

日期：2026-10-29

## 本次修改

- 复制、移动、解压、删除及清空回收站等进行中的胶囊标签采用固定蓝色底与蓝色文字，适配深浅主题；高对比度仍使用系统配色。
- 解压器报告的是整数百分比。首次有效报告仅建立采样基线，后续仅在进度增加时更新速度，避免把空轮询或重复百分比作为零字节样本。
- 超过 `max(4 秒, 上次百分比更新间隔 × 2)` 无进展时清除过期速度与 ETA；恢复后仍按跨越停滞的实际时间计算。
- 通用复制/移动速度估算器未修改。

## 实际验证

- `cmake --build build-release --target pulse_archive_test pulse_file_operation_failure_test pulse --parallel 4`：成功，无新增编译警告。生产构建保持 `PULSE_WITH_SELFTEST=OFF`。
- `pulse_archive_test.exe --progress-sampling-only`：退出码 0，14 项通过；包括启动基线、稀疏百分比、低速更新、真实减速、停滞与恢复，见 `archive-sampling.log`。
- `pulse_file_operation_failure_test.exe --progress-only`：使用仓库现有 `PULSE_TEST_STREAM_SNAPSHOTS=1` 输出截图，由 PowerShell 保存 PNG；退出码 0、0 失败、52 张截图。见 `ui-test.log`。
- 已目视检查复制蓝色标签，以及中文解压弹窗的深色 96 DPI、浅色 168 DPI 截图，无裁切或重叠。界面中的锯齿速度历史为该界面测试主动输入的模拟数据。
- `git diff --check`：通过。

## 验证限制

`pulse_archive_test.exe --progress-only` 的采样及三个进度管道断言通过，但进入真实 ZIP/7z 测试时，创建 `bench_data/archive-progress-<pid>/source/sub/empty` 返回 `Access is denied`，退出码 1；真实解压未验证通过。默认截图保存同样返回 `0x80070005`，通过仓库现有内存截图流方式完成了界面验证，未删除或弱化失败断言。

已更新 `build-release/Pulse.exe`；未打包、安装或发布。
