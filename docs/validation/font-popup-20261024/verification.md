# 字体接入与下拉动画验证（2026-10-24）

本次保留原工作区未提交修改，在现有字体迁移上补齐普通 UI 文案，并完善共用 FluentMenu 的关闭动画。构建产物为 `E:\Projects\pulse\build_release_clean\Pulse.exe`，没有替换安装目录中的程序。

## 实际结果

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| Release 主程序与两个相关测试目标构建 | 通过；最终增量构建无新增编译警告 | `pulse`、`pulse_menu_text_test`、`pulse_text_render_settings_test` |
| 下拉入场、中途选择、透明度连续、重复点击、退出、关闭系统动画 | 241 PASS / 0 FAIL | `menu-dropdown-animation.log` |
| 实际模态循环的入场、静止不重复提交、悬停子菜单、Escape 退场 | 11 PASS / 0 FAIL | `menu-animation-idle.log` |
| 失活取消、主/子菜单选择、关闭生命周期 | 21 PASS / 0 FAIL | `menu-lifecycle.log` |
| 原按钮重复点击、双击、外部点击转发、子菜单选择 | 15 PASS / 0 FAIL | `menu-toggle.log` |
| 借用搜索输入框、历史删除/清空、刷新保留输入和动画 | 29 PASS / 0 FAIL | `menu-history-refresh.log` |
| Auto/Sharp/Smooth 后端计数、普通文案、长文字省略、组合字符/补充平面簇、emoji 回退、面包屑几何和命中 | 694 PASS / 0 FAIL | `plain-text.log` |
| PNG 编码、签名及数量 | 16 张菜单 + 24 张字体/面包屑，均非空有效 PNG | 当前目录及 `plain-text/` |
| 定向 `git diff --check` | 通过；只有既有换行符提示 | 本次相关源文件 |

合计 1011 项定向断言通过。未运行完整自测、全量 Release 构建或其它历史回归。

## 可见行为

- 新建、排序、分组和搜索下拉使用共用菜单组件：200ms OutCubic 渐入、140ms InQuad 渐出，与操作中心相同；锚定搜索输入框的结果窗只淡入，不移动输入框。
- 菜单项选择、Escape、点击外部都能退场；入场中途关闭保留当前透明度和位置，避免跳到完全不透明。关闭期间不重复触发命令。
- 外部点击在原模态循环结束后转交，避免直接打开另一个菜单而重用仍在退场的状态。排序/分组支持原按钮再次点击关闭。
- 遵守 Windows 客户区动画和高对比度设置；外部失活/取消保留立即关闭语义。
- 字体补齐位置、真实 SDK 省略缺口及保留的旧路径见 `font-render-audit.md`。当前 Auto 优先 LumaText；Sharp/Smooth、复杂排版、编辑命中、字体不支持等路径仍可能使用 DirectWrite。

## 视觉检查范围与限制

抽查了两档缩放、明暗主题下的普通文字及菜单，以及真实 MainRenderer 面包屑；检查中英文末字、尾省略、扩展名、近/中/远段落位置和菜单标签，没有发现裁切或重叠。菜单 PNG 使用生产绘制组件与隔离测试条目，面包屑 PNG 使用真实主窗口绘制；不是每一种业务菜单内容的完整截图矩阵。没有本次修改前的截图对照，也没有逐一实际打开所有操作/播放/比较窗口。

动画由实际模态循环中间帧及确定性时间步检查验证；静态 PNG 本身不能证明动画节奏。其它业务窗口主要验证共用文字入口和构建，没有据此声称每个窗口已完成交互验收。

原生测试进程在图形初始化前就无法创建证据文件（`CreateFileW` 错误 5，见 `menu-access-probe.log`）。本次使用显式测试环境变量将真实 GPU 像素经 WIC 在内存编码，再由父 PowerShell 保存 PNG；未修改 ACL 或产品截图 API。最初直接写文件的失败属于本次环境限制，不记作截图通过；最终记录对应内存传输方式。

## 复现

在 x64 VS 开发环境中构建相关目标：

```powershell
cmake --build build_release_clean --target pulse pulse_menu_text_test pulse_text_render_settings_test -j 4
.\docs\validation\font-popup-20261024\run-validation.ps1
# 只重跑字体：
.\docs\validation\font-popup-20261024\run-validation.ps1 -CaseFilter plain-text
```

脚本只处理当前证据目录内的 PNG，保存前检查 PNG 签名，恢复其设置的进程环境变量；测试不修改用户文件和持久偏好。
