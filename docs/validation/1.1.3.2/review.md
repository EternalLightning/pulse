# Pulse 1.1.3.2 验证记录

日期：2026-10-18。

## 范围

基线为本轮开始前提交的 `0e81e18`，优化提交为 `9f150fd` 与 `93af249`。版本号由 `1.1.3.0` 提升到 `1.1.3.2`。

重点验证窗口显示过渡、菜单亚克力采样、输入/绘制路径、会话恢复和设置迁移失败反馈。未重新审查操作、索引和 Shell 的安全契约。

## 实际改动

- 恢复 `WS_CAPTION` 原生窗口过渡样式；隐藏窗口从任务栏固定图标、托盘或冷启动显示时，由 DirectComposition 在首个完整帧上启动 180ms 平滑淡入。系统关闭动画或高对比度时跳过。
- 菜单亚克力采样只覆盖卡片、模糊边距和 120 DIP 增长余量；主菜单与子菜单各自持有采样，子菜单在显示前采样，避免把自身旧像素当背景。
- 只读命中路径复用上一帧 ViewModel；`BuildVm(false)` 跳过操作/索引状态、选择大小遍历和剪贴板探测。窗口标题按变化更新，鼠标离开只注册一次，相同尺寸不重建 swap chain。
- 会话恢复跳过空路径 pane；设置迁移失败刷新卷视图。
- 菜单卡片的采样键改用专用结构；子菜单渲染失败时立即隐藏，不再让后续 Present 显示旧表面。
- 关闭系统动画或启用高对比度时，显式把已排队的窗口淡入恢复为不透明，避免遗留状态让窗口保持透明。
- 合并非必要状态栏构建代码。

## 构建与打包

`build_installer.bat` 完整 Release 构建和 Inno Setup 编译成功，退出码 0。

| 产物 | 字节 | SHA256 |
|---|---:|---|
| `dist/PulseSetup-1.1.3.2.exe` | 9,945,439 | `1373DDD0C10FCBE4AB2E3BEAA45C9C341B9D3DDE5F77A093C897E63EEA600D08` |
| `dist/Pulse-1.1.3.2-portable-win-x64.zip` | 12,173,958 | `C8E5F9DEAFF8F7B57BCB56D059FDA33632434FD2FA39A911360A591A08054CC1` |

Release 二进制版本均为 `1.1.3.2`：

| 文件 | 字节 | SHA256 |
|---|---:|---|
| `Pulse.exe` | 7,395,328 | `8B8C1E6710A75C951AEB26026389A11B9DF5EBA99B311EA00477669605AE0408` |
| `Pulse.Index.exe` | 3,406,848 | `528312469736842C1950803C3BB07BCB458F140018E7C58A7967688F26BC65F6` |
| `Pulse.Document.exe` | 237,056 | `59BD7EC2C42600E466A6D75FFBA7AF2D9D88A0DF0AF71B27ED45491FD1B53110` |
| `Pulse.Preview.exe` | 264,704 | `4614D9AE6CEA4494B6975E56280FCE24AB522B13BCC8B94EAC791F9EA3D03DC0` |
| `Pulse.Shell.exe` | 1,418,240 | `6B1887ABA28470B7EC9D0A1C8EF05F703F7E992D05943FCFF20241D769731A69` |

## 定向回归

Release 构建产物上重新执行：

- `pulse_app_controllers_test`：PASS（会话空 pane 恢复、滚动状态、迁移失败刷新）
- `pulse_pane_animation_application_test`：PASS
- `pulse_multiwindow_application_test`：0 failures
- `pulse_clipboard_availability_test`：28 checks，0 failures
- `pulse_session_windows_test`：0 failures
- `pulse_titlebar_layout_test`：0 failures
- `pulse_menu_text_test` 默认用例：PASS
- `pulse_menu_text_test --acrylic-transparent`：PASS，包含“采样裁剪到弹窗和模糊边距”和“实时子菜单增长复用干净背景”
- `pulse_sidebar_scrollbar_fade_test`：PASS（Debug/Ninja 构建阶段）

`pulse.exe --shot 1200x800 --test-instance` 使用隔离数据目录渲染成功，退出码 0。

## 本地部署

用户结束 Pulse 相关进程并停止 `PulseIndex` 服务后，完整程序载荷已重新复制到 `D:\Applications\Pulse`。

- 备份：`dist/rollback-1.1.3.2-full-20261018-115459/`
- 已替换：`Pulse.exe`、`Pulse.Index.exe`、`Pulse.Document.exe`、`Pulse.Preview.exe`、`Pulse.Shell.exe`、`lumatext.dll`、`pdfium.dll`
- 五个可执行文件版本均为 `1.1.3.2`
- `Pulse.exe` SHA256：`8B8C1E6710A75C951AEB26026389A11B9DF5EBA99B311EA00477669605AE0408`
- 从部署目录执行 `--shot 1200x800 --test-instance` 成功，退出码 0；记录见 `dist/deploy-1.1.3.2.json`
- `PulseIndex` 当前为 Stopped；当前会话没有服务控制权限，无法自动启动，需要用户以管理员身份启动，或重新运行安装包时启用索引服务。

## 边界

- 完整程序载荷已复制，但 `PulseIndex` 服务未能由当前会话启动；启动服务需要管理员权限。
- 未运行完整 `pulse.exe --selftest`、全部历史测试、真实任务栏点击录像或真实安装/卸载升级。
- 动画通过等价的隐藏/恢复路径探针和慢速过渡验证，没有物理鼠标任务栏点击录屏。
- `pulse_menu_text_test --acrylic`（非透明桌面曝光项）依赖当前桌面顶层窗口环境；本次未将其作为通过项。透明亚克力夹具和裁剪断言已通过。
- 未测量 GPU 帧时间、整机 CPU 或内存的净变化。
