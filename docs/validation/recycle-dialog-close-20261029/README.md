# 清空回收站弹窗空白修复

关闭扫描进度窗口时，淡出循环持续清空 WM_PAINT 队列，而进度窗口每次绘制又请求下一帧，导致关闭流程无法推进。透明度动画结束后窗口仍未隐藏，可能留下透明矩形并阻止显示删除确认窗口。

修复为每轮最多处理一次 WM_PAINT，随后重新检查动画时限并隐藏窗口。没有改变回收站删除逻辑。

## 验证结果

- `pulse_dialog_close_test.exe`：23 PASS，退出码 0。新增持续自我重绘的窗口用例，要求在重绘流停止前完成关闭，同时验证临时状态清理。
- 对照程序仅将修复后的 `if (PeekMessageW(... WM_PAINT ...))` 恢复为旧 `while`，编译独立副本而不回退共享源码。同一用例失败，退出码 1；其余 22 项通过。见 `baseline-close.log`、`fixed-close.log`。临时对照源码和二进制已清理。
- `pulse_link_confirmation_ui_test.exe --recycle` 和 `--recycle --light --narrow`：均退出码 0。使用虚构路径显示 EmptyRecycle 扫描进度，隐藏后进入永久删除确认并按默认取消；检查进度及时隐藏、标题实际可见、布局和 owner 恢复。浅色用例将确认窗口改为 144 DPI、420×570 像素。
- 上述 UI 检查通过 `PULSE_TEST_STREAM_SNAPSHOTS=1` 输出内存 PNG，由启动器保存；`*_visible.png` 为测试自身窗口屏幕区域，截图前不调用检查或重绘钩子。`recycle-ui.log` 记录结果。
- 已目视检查深色和浅色 150% 窄窗口截图，标题、路径和按钮没有发现裁切或重叠。
- `cmake --build build-release --target pulse_dialog_close_test pulse_link_confirmation_ui_test pulse --parallel 4` 成功；生产 `Pulse.exe` 文件版本为 1.2.2.0，selftests 保持关闭。
- 本次涉及的源码通过 `git diff --check`。现有其他会话修改保留。

没有调用实际删除服务，没有清空用户回收站，没有替换安装版或打包发布。
