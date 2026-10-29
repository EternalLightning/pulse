# 分组顺序菜单验证

- 将单个“切换分组顺序”改为“升序”“降序”两个单选项；方向命令显式设置顺序，重复选择保持当前方向。
- `build_release.bat` 完成，构建目录为 `build-release`，Release、`PULSE_WITH_SELFTEST=OFF`。日志：`build-release/group-order-release-build.log`。
- `pulse_group_toolbar_ui_test.exe --group-order-only` 的 4 项命令检查通过：升序、重复升序、降序、不分组下忽略方向命令。
- 同次测试的 8 项菜单创建检查通过，8 项截图保存检查失败；未完成外观验证。完整结果见 `ui-test.log`，不能将本次测试整体记为通过。
- `pulse_command_catalog_test` 未能链接，缺失 `IsOneDriveCloudRoot`、`SortCloudRoots`、`IsUnderCloudFilesRoot`、`CloudRootColor`、`RequestCloudRootRefresh`、`CloudRootCatalog` 等现有云盘依赖符号，未执行。日志：`build-release/group-order-test-build-final.log`。
- 初次界面测试因窄字符 `main` 使用 `__wargv` 退出，已改为从 `argc/argv` 读取筛选参数并重新执行。

未构建安装包、替换安装目录或发布版本。
