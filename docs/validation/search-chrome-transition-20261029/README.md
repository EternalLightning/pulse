# 搜索框完整外观切换

此次验收对象是搜索框的两种完整外观：收起态的搜索图标、提示文字和 Ctrl+K，与展开态的文件/内容切换、输入提示及选项/关闭按钮。底线不再单独承担过渡反馈。

完整切换约 200ms：原有内容先淡出，再渐显展开内容并轻微位移，收起时反向播放。边框保持稳定，避免两套文字同时重叠。原生 EDIT 及其 composition 表面同步控制透明度，点击后立即接受输入；关闭保留当前输入文字供退出帧绘制。窄窗口的原地址栏也使用相同分段淡出。保留快速反转、高对比度及关闭系统动画的即时端点行为。

## 实际验证

- `pulse_child_edit_test.exe --opacity`：23 PASS、退出码 0，验证原生/composition 编辑框重绘、过渡透明度、临时状态清理及显示失败回退。
- `pulse_pane_animation_application_test.exe --search-interaction-only`：206 PASS、退出码 0；覆盖立即输入、重复点击防重弹、往返和反转、编辑框中间透明度及高对比度端点。
- 同一检查采样 56 张 PNG，覆盖浅/深主题、100%/150% 缩放及窄/正常窗口。像素检查排除底部焦点线，确认图标、提示和按钮本身具有不同的中间帧；固定进度下正向/反向画面相同。
- 已目视检查代表性的完整序列及窄窗口画面，修正初版交叉淡化造成的文字叠影。当前最终序列没有发现文字重叠、控件裁切。
- `cmake --build build-release --target pulse_child_edit_test pulse_pane_animation_application_test pulse --parallel 4` 成功，退出码 0；`build-release/Pulse.exe` 文件版本为 `1.2.2.0`。

最初编辑框用例缺少字体初始化导致显示断言失败，已补齐生产字体初始化并按上述结果复验；未弱化断言。先前自动审批额度错误导致一次复验未执行，用户要求继续后通过正常审批运行并取得上述结果。

生产配置恢复 `PULSE_BUILD_ARCHITECTURE_FIXTURES=OFF`、`PULSE_WITH_SELFTEST=OFF`。没有替换安装版、生成安装包或发布。PNG 通过测试 stdout 通道由启动器保存。
