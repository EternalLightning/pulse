# 路径栏编辑态过渡

路径栏在面包屑与原生路径编辑框之间采用搜索框现有的约 200ms 分段淡出、渐显和 6 DIP 位移。边框保持稳定，焦点外观与编辑框透明度同步。编辑框创建后立即接受输入；关闭及切换到搜索时保留退出文字，快速反转从当前进度继续。高对比度及关闭系统动画时直接使用端点。

## 实际验证

- `pulse_pane_animation_application_test.exe --address-path-only`：92 PASS、0 FAIL、退出码 0。覆盖立即输入、原生编辑框中间透明度、关闭文字保留、快速反转、高对比度、路径/搜索双向切换，以及 40 张渲染截图。
- `pulse_pane_animation_application_test.exe --search-interaction-only`：206 PASS、0 FAIL、退出码 0，验证直接相关的既有搜索框交互与过渡；另存 56 张搜索框截图。
- 路径截图覆盖浅/深主题、100%/150% 缩放、680/1200 DIP 宽度及五个进度。目视检查 `contact-dark.png` 与 `contact-layout.png` 中的代表序列，路径栏未见文字叠影、裁切或与相邻控件重叠。截图使用生产渲染器绘制退出文字；原生编辑框的输入和透明度由前述独立断言验证，未作完整桌面人工交互验收。
- `cmake --build build-release --target pulse pulse_pane_animation_application_test --parallel 4`：成功，最后增量构建包含最终互切文字处理，退出码 0。生产产物为 `build-release/Pulse.exe`。
- 本次涉及文件的 `git diff --check` 无差异格式错误。保留共享工作区既有修改。

检查通过后恢复 `PULSE_BUILD_ARCHITECTURE_FIXTURES=OFF`、`PULSE_WITH_SELFTEST=OFF`。未替换安装版、打包或发布。

`run_validation.py` 调用上述隔离测试，通过 stdout 接收 PNG 并保存日志；不修改用户文件或偏好。构建测试前须临时启用架构测试目标。
