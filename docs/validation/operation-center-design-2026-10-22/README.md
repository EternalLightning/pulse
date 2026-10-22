# 操作中心卡片排版

参考 Files 的状态图标、标题层级和弱化次要操作的方式，结合 Pulse 任务弹窗已有的状态标签、路径摘要、进度条和统计分区。

参考源码：https://github.com/files-community/Files/blob/main/src/Files.App/UserControls/StatusCenter/StatusCenter.xaml

本次修改：

- 标题采用任务摘要，失败摘要与错误相同时回退为操作名称；状态单独显示为标签。
- 来源与目标在常规宽度并排，窄宽度纵向排列，复用路径中间省略逻辑。
- 当前传输显示剩余时间、剩余项目或大小、当前速度；暂停和等待状态不显示过期速度或 ETA。
- 已结束任务省略当前文件和进度条；错误保留摘要及详细信息入口。
- 详细信息、取消、打开目标文件夹采用透明按钮，暂停和继续保留普通按钮。

实际验证：

- `cmake --build build --target pulse_operation_center_ui_test`：通过。
- `cmake --build build --target pulse`：通过。
- `pulse_operation_center_ui_test.exe`：170 PASS，0 FAIL，无 SKIP。包括任务动作回调、更新期间点击、滚动、收起动画、恢复入口、材质、深浅色、472/280 DIP 宽度和 100%/150%/200% DPI。
- 人工检查实际渲染器导出的正常、暂停、失败、已完成、中英文、窄宽度和恢复入口预览，未发现控件重叠。

验证使用隔离任务数据和生产卡片渲染器，没有操作用户文件或替换安装目录程序；不代表安装版端到端验收。

`after-light.png`、`after-dark.png` 为中文预览；`narrow-150-light.png` 为窄宽度预览。`before-light.png` 保留修改前已有的混合任务截图，使用的是不同语言与任务展示数据，供排版参考。构建与测试原始输出见本目录日志。
