# 视图过渡与性能验证

记录日期：2026-10-31。源码版本：1.3.0.0。

## 行为与实现

列表、详细信息与图标视图之间按稳定文件身份连接可见项目的位置和图标尺寸，保留当前排序。菜单切换使用约 220 ms 过渡，滚轮切换使用约 120 ms 过渡；快速连续输入从当前画面重新定向。每项绘制一个不透明图像，避免前后两层交叉淡化产生拖影。

Shift / Ctrl 加鼠标滚轮支持多刻度及高精度滚轮输入，并保持选中文件的滚动锚点。文件夹标题使用固定左边距，避免快速切换详细信息视图时随名称列左右移动。搜索与本地筛选保留已发布项目，通过防抖和项目增删、位移过渡更新结果。

默认复用 Direct2D 绘图命令，减少动画期间重复准备文字和图标。几何缓存和稀疏身份查询限制每帧工作，视图图标过渡最多保留 256 个可见图像。Direct2D 已使用硬件渲染；布局、输入及命令准备仍在 CPU 上执行。

额外的像素图集缓存为实验功能，默认关闭。可通过进程环境变量 `PULSE_ANIMATION_PIXEL_CACHE=1` 开启；当前测量未证明稳定的 P95 / P99 收益，不作为默认优化结论。

## 已执行的定向检查

以下为本轮已有构建和测试记录，整理本文时未重复完整历史回归。

| 检查 | 结果与边界 |
| --- | --- |
| 受影响的生产与测试目标 | 构建成功，未发现新增编译警告；生产配置关闭内嵌自测。 |
| 视图和布局回归 | 3265 PASS / 0 FAIL；包含标题像素位置、浅深主题、100% / 150% DPI、八种模式的快速正反切换。 |
| 默认命令缓存动画反馈 | 872 PASS / 0 FAIL。 |
| 实验像素缓存动画反馈 | 872 PASS / 0 FAIL。 |
| 像素图集独立回归 | 25 PASS / 0 FAIL。 |
| Shell 查询连接错误回归 | 10 PASS / 0 FAIL；拒绝访问及无效名称保留原错误，缺失或忙碌管道保持有界重试，取消与辅助进程回收通过。 |
| 画面检查 | 已查看可控颜色图像在 240 Hz 采样下的中间态，为单个不透明图像，无前帧残影；该用例不依赖原生 Shell 图标。 |

连接回归修复前，拒绝访问及无效名称被重试至截止时间并误报为超时。修复后分别返回 `0x80070005`、`0x8007007B`，样本耗时为 0、16 ms，均只打开一次。生产管道权限与对端 PID 校验保持原有契约。

## 性能测量限制

测量环境检测到 NVIDIA RTX 3070 Ti 硬件适配器和 3440 × 1440、165 Hz 显示器。但测试进程令牌为 AppContainer，使用生产用户 SID 安全属性的管道打开返回错误 5，导致原生图标准备不完整。此证据说明本轮测量受限，不能解释普通桌面会话中的卡顿根因。

`scripts/measure_animation_frames.ps1` 要求基线与当前程序均提供 `[ICON_READY] native_folder_sizes_16_256=1`，缺少该标记即拒绝对照。旧的临时基线程序未具备此检查，不能直接用于最终验收。原始截图、日志与 CSV 留在本地忽略目录中。

尚未完成完整原生图标、实际目录下的高刷帧率验收，不能据此宣称抽帧问题已消除。应在能通过生产管道访问检查的普通桌面进程中，按同一图标工作量与窗口尺寸采样后再比较。本文不代表安装包、安装升级或已安装副本的验证。

## 定向复现入口

```powershell
& .\scripts\build.ps1 -BuildDir build-release -Targets @(
    'pulse', 'pulse_item_reflow_ui_test',
    'pulse_shell_query_connection_test', 'pulse_item_reflow_bitmap_test')
.\build-release\pulse_shell_query_connection_test.exe

$env:PULSE_TEST_SNAPSHOT_STDOUT = '1'
$env:PULSE_ANIMATION_PIXEL_CACHE = '0'
.\build-release\pulse_item_reflow_ui_test.exe unused --view-mode-feedback-only
$env:PULSE_ANIMATION_PIXEL_CACHE = '1'
.\build-release\pulse_item_reflow_ui_test.exe unused --view-mode-feedback-only
.\build-release\pulse_item_reflow_bitmap_test.exe
Remove-Item Env:PULSE_ANIMATION_PIXEL_CACHE
Remove-Item Env:PULSE_TEST_SNAPSHOT_STDOUT
```

构建工具和隔离配置要求见[构建指南](build-and-package.md)。构建、功能验证、打包、安装和发布分别执行并记录。
