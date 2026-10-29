# 弹窗、下拉与搜索动画验证

本次范围：普通弹窗关闭淡出；下拉二次点击收起与退出动画；搜索历史、范围、选项和焦点反馈；权限续作保留原批次记录。已有未提交工作保留。

## 实际运行结果

| 定向检查 | 结果 |
| --- | --- |
| `pulse_menu_text_test --toggle` | 通过：普通下拉自动登记、子控件点击、重复点击、外部点击转发、子菜单 |
| `pulse_menu_text_test --lifecycle` | 通过：失焦、取消、应用切换、选中项、退出中间帧及最终隐藏 |
| `pulse_menu_text_test --dropdown-animation` | 289 PASS，16 张 PNG |
| `pulse_menu_text_test --animation-idle` | 11 PASS |
| `pulse_menu_text_test --history-refresh` | 29 PASS |
| `pulse_dialog_close_test` | 20 PASS |
| `pulse_dialog_fade_test --lifecycle` | 96 PASS，4 张 PNG |
| `pulse_pane_animation_application_test --search-interaction-only` | 83 PASS，32 张 PNG |
| `pulse_operation_records_test --task-status` | 38 PASS |

菜单与搜索截图覆盖浅/深主题、100%/150% 缩放和搜索区域窄/正常窗口。已检查代表性采样图像的文字、控件及边界，没有发现裁切或重叠。搜索中间帧由真实搜索绘制路径在固定进度下采样；菜单测试还验证真实模态消息循环内的动画与空闲行为。原生弹窗截图验证客户区内容；透明度递减和 composition/dim 生命周期由定向断言验证。

测试依次运行，不改动用户偏好或系统动画设置。弹窗测试报告系统动画关闭分支未运行；菜单测试通过自己的 motion override 验证立即显示/收起。搜索测试另外覆盖高对比度端点。

## 验证限制

`pulse_operation_elevation_adapter_test --partial-continuation` 两次在创建隔离目录时遭遇 `Access is denied`（系统临时目录及仓库隔离目录均如此），尚未执行到文件复制。不能据此声称真实文件续传或 UAC 已验证。批次记录的成功、取消、失败、排队取消、原始名称/路径和累计数量已由 operation-records 定向测试验证。

本环境原生测试子进程无法直接保存文件，PNG 通过测试已有的 stdout 协议由 PowerShell 启动器保存；未取消或放宽图像断言。

相关检查完成后，`version.txt` 更新为 `1.2.2.0`，生产配置保持 `PULSE_WITH_SELFTEST=OFF`，并恢复 `PULSE_BUILD_ARCHITECTURE_FIXTURES=OFF`。生产构建使用 `cmake --build build-release --target pulse --parallel 4`，包含主程序声明的助手依赖。未生成安装包、更新安装目录或发布。

生产构建退出码为 0；`Pulse.exe`、`Pulse.Index.exe`、`Pulse.Shell.exe`、`Pulse.Preview.exe`、`Pulse.Document.exe`、`Pulse.Integration.exe` 的文件版本均核对为 `1.2.2.0`。
