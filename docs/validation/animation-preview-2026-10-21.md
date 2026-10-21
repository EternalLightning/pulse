# 现有预览审查与动画第一版

日期：2026-10-21。保留现有未提交工作，不增加预览格式、悬停预览或其他内容功能。

## 变更

- 修复名称以 `.svg/.emf/.wmf` 结尾的目录被当作图片解码的问题。目录属性现在优先使用已有 Shell 图标路径；审查与真实 helper 对照见 [预览记录](preview-audit-2026-10-21.md)。
- 筛选框与分栏标题控件快速反向切换时，先采样当时的动画值再改变目标。
- 侧栏与详情区折叠、标签/页签重排、暂存卡片及滚动条统一使用缓存的系统动画策略。关闭动画或高对比时直接到终态；拖动反馈和滚动条自动隐藏仍保留。
- 详情预览的位图、文本和十六进制内容就绪后短淡入 160ms，选择变化会重新开始；缩放、拖动和普通重绘不重启。位图使用现有绘制透明度，不增加额外离屏图层。第三方预览处理器 HWND 沿用原有显示方式。
- 文件操作窗口的完成文件行与完成标记淡入 160ms，其余进度、统计和按钮保持稳定。原有完成停留时间不变。
- 通知卡片使用短淡入与 6 DIP 位移；停留倒计时不再反复重绘没有变化的内容。操作窗口的逐帧判断改为缓存系统动画设置，并响应设置变更。

## 桌面证据

通过隔离测试实例、真实鼠标/键盘输入及 `CopyFromScreen` 录制。测试文件和应用数据位于 `bench_data/animation-desktop/`，没有替换安装目录。

- [标签蓝条伸缩与快速反向切换](animation-preview-2026-10-21/tabs-desktop.gif)
- [筛选展开/收起](animation-preview-2026-10-21/filter-desktop.gif)
- [侧栏折叠](animation-preview-2026-10-21/sidebar-desktop.gif)
- [两张图片间切换及预览淡入](animation-preview-2026-10-21/preview-desktop.gif)
- [实际桌面图标选中蓝线与详情面板](animation-preview-2026-10-21/icon-selection-desktop.png)

录制帧率受桌面截取与保存耗时限制，不作为显示器刷新率或用户主观流畅度的性能测量。

## 定向验证

- `pulse`：Release 构建及最终依赖重建完成。
- `pulse_preview_test --response-compatibility-only`：23 PASS / 0 FAIL，包括真实 helper 的目录图标、位图尺寸、文本、hex、属性及不可用响应兼容。
- `pulse_pane_animation_test`：29 PASS / 0 FAIL，包括快速反向采样、禁用动画终态和 30/60/144/240 Hz 类似采样时序。
- `pulse_file_operation_failure_test --completion-only`：20 PASS / 0 FAIL。4 对浅/深色、96/168 DPI 的起始与终点截图中，变化仅发生在完成行和标记，其余像素完全一致。
- `pulse_file_operation_failure_test --motion-only`：11 PASS / 0 FAIL，连续绘制期间计时器正常交付。
- `pulse_pane_animation_application_test`：79 PASS / 0 FAIL。覆盖内容就绪淡入、选择切换、禁用动画与高对比策略、浅/深色及 DPI 场景。原始夹具低于已有的 1000 DIP 详情面板显示阈值；调整夹具宽度并确认面板可见后重跑通过。

最终 `build/Pulse.exe` SHA-256：`1978062CCB244ECD7BB29343A09D21D0C26A74879CF5B9FCB65BF745F543E141`。

系统关闭动画通过显式策略参数/缓存注入验证；生产路径另用高对比覆盖验证，没有修改用户真实 Windows 动画或高对比设置。没有运行完整自测或全部历史回归。
