# 打开方式顶层菜单调整

- 将静态 `openas` 入口从“更多选项”移到顶层“打开”后，沿用原命令编号和执行路径。
- 保留原有分类和单项显隐设置，具体应用快捷项继续位于“更多选项”。
- 保留“打开”所在操作组的分隔线；静态命令在菜单打开期间保持绑定，不被后到的注册表结果替换。

## 实际验证

- 构建 `pulse`、`pulse_app_controllers_test`、`pulse_context_menu_loading_test`：成功。
- `pulse_app_controllers_test.exe --context-menu-refresh`：32 PASS，0 FAIL。
  覆盖新入口位置、Shell 查询完成前后、显隐设置、注册表异步刷新和原 `openas` 回调。
- `pulse_app_controllers_test.exe --context-menu-overflow`：16 PASS，0 FAIL。
- `pulse_context_menu_loading_test.exe --open-with docs/validation/open-with-top-level-2026-10-23`：14 PASS，0 FAIL。
  使用生产菜单合成和绘制代码，以精简内置菜单夹具生成浅色/深色、100%/150% 四张 PNG；文字宽度检查通过，并已查看四张图片。
- 本次三个源文件的 `git diff --check` 通过。

## 验证范围

未替换安装版，未通过真实系统选择器启动应用。执行路径验证使用回调记录；PNG 是隐藏窗口渲染的菜单夹具，不是安装版完整右键菜单截图。
