# 面板动画帧调度验证（2026-10-13）

> 本文记录第一阶段验证。后续已将焦点交接移到绘制末尾、移除主 UI 常驻 16 ms 轮询；当前实现及扩大后的定向检查见 [UI 分层调度验证](ui-scheduling-2026-10-13.md)。

## 改动

筛选框展开/收起和面板标题按钮淡入淡出，改为在 `Render` 构建视图前推进。时间来自 `steady_clock`，按毫秒取样。动画尚未结束时复用现有 `WM_PAINT` / `Present` 续帧路径；定时器只负责发现状态变化和保留原有的输入框焦点交接。

保持原有 140 ms 筛选框时长、180 ms 按钮淡变时长和缓动曲线。新启动的动画即便首帧进度为零，也会请求下一帧；到达终点后停止续帧。既有隐藏、最小化及设备恢复保护仍应用于续帧判断。没有提高 UI 定时器频率、创建线程或更改系统计时器分辨率。

生产修改位于 `src/app/pane_animations.h` 和 `src/app/app_main.cpp` 的动画采样/续帧相关段落，保留 `app_main.cpp` 中已有的其他修改。

## 实际验证

通过 `scripts\vcvars.bat` 初始化工具链后：

```text
cmake --build build --target pulse_pane_animation_test pulse
build\pulse_pane_animation_test.exe
cmake --build build --target pulse_pane_animation_application_test
build\pulse_pane_animation_application_test.exe
```

应用级目标使用仓库已有的 `PULSE_BUILD_ARCHITECTURE_FIXTURES=ON` 配置；没有启用或运行完整 selftest。

- 状态测试：16 项通过。覆盖新面板初始化、首帧进度为零时续帧、4 ms 连续采样、快速反向、长时间中断、带文字的筛选框保持展开，以及 4/7/16/33 ms 四种采样间隔下的时长、单调性、范围和终止。
- 应用级测试：13 项通过。调用真实 `Render`、`WndProcImpl(WM_PAINT)` 和 `WndProcImpl(WM_TIMER)`；验证无需定时器也能推进展开、隐藏窗口不自发续帧、终点停帧、输入框待交接状态清除，以及收起终止。
- 4 张中间帧截图已生成并检查：`bench_data/pane_animation_ui/light-100.png`、`dark-100.png`、`light-150.png`、`dark-150.png`。覆盖浅色/深色、100%/150% DPI；浅色为 440 DIP 窄窗口，深色为 760 DIP。动画相关筛选区域未见重叠；窄窗口底部状态文本仍采用现有截断表现。
- 主程序及两个定向测试目标构建成功；本轮差异检查通过。

应用级夹具首次因遗漏首次绘制前的 `RecreateTextFormats` 初始化而发生访问冲突。补齐与应用启动流程一致的字体初始化后复跑通过；未为此修改生产渲染代码或弱化检查。

## 验证限制

这是隐藏窗口渲染与时间采样检查，未实测 144/240 Hz 物理显示器上的帧时间，也未测量整机空闲 CPU/GPU 或真实最小化窗口的功耗。隐藏窗口检查验证不自动续帧；最小化分支保留原判断，未单独进行桌面实测。隐藏窗口的输入框检查验证交接状态，不代表实际前台键盘焦点体验。静态截图不能证明动态流畅度。
